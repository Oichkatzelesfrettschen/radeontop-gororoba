/*
    Copyright (C) 2012 Lauri Kasanen
    Copyright (C) 2018 Genesis Cloud Ltd.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, version 3 of the License.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "radeontop.h"

#include <math.h>

// The dumper owns no clock.  One published generation is one completed
// measurement window, so waiting for a generation newer than the last printed
// one is what makes a line correspond to exactly one window.  An independent
// sleep would print a generation twice or skip one whenever a window ran long,
// and nothing in the old output could distinguish either case.
#define DUMP_POLL_MS 200

// Renders a lane duty, or N/A when the window validated no read of the register
// the lane comes from.  A 0.00% value would read as a confirmed-idle block.
static const char *format_percent(char *buffer, size_t size, double fraction) {
	if (isnan(fraction))
		snprintf(buffer, size, "N/A");
	else
		snprintf(buffer, size, "%.2f%%", 100.0 * fraction);

	return buffer;
}

static void lane_field(FILE *f, const struct engine_masks *masks,
		const struct collector_snapshot *snapshot,
		enum collector_lane lane, const char *name, const char *separator) {
	char buffer[16];

	// A zero mask means the block is absent on the selected family, or its exposure
	// on the target remains unconfirmed and the lane stays unexposed until
	// an observation supports it.
	if (!masks->lane[lane])
		return;

	fprintf(f, "%s%s %s", separator, name,
		format_percent(buffer, sizeof(buffer),
			collector_lane_fraction(snapshot, lane)));
}

// Serializes one measurement class's health.  A lane percentage is divided by
// its own register's valid count, so a capture that carries only the status
// denominator cannot distinguish a figure resting on 12 valid reads from one
// resting on 120.
static void signal_health(FILE *f, const char *name, uint32_t capability,
		const struct collector_snapshot *snapshot,
		const struct collector_signal_stats *stats) {
	if (!(snapshot->capabilities & capability)) {
		fprintf(f, ", %s unsupported", name);
		return;
	}

	fprintf(f, ", %s %llu/%llu+%lluf", name,
		(unsigned long long) stats->valid,
		(unsigned long long) snapshot->nominal_slots,
		(unsigned long long) stats->failed);
}

// An endpoint read is a single point measurement, so it reports presence rather
// than a rate: absent capability, a valid reading, or a read that failed.
static void endpoint_health(FILE *f, const char *name, uint32_t capability,
		const struct collector_snapshot *snapshot, bool valid) {
	if (!(snapshot->capabilities & capability))
		fprintf(f, ", %s unsupported", name);
	else
		fprintf(f, ", %s %s", name, valid ? "ok" : "failed");
}

static int dump_line(FILE *f, const struct engine_masks *masks,
		const struct radeontop_capture_metadata *metadata,
		const struct collector_snapshot *snapshot, unsigned char bus) {
	char buffer[16];

	// The timestamp labels the window's scheduled end, which the collector
	// derives from the publication lag.  A dump-time stamp would date the
	// measurement by however long the dump writer was blocked.
	fprintf(f, "%llu.%06llu: ",
		(unsigned long long) snapshot->scheduled_end_realtime.tv_sec,
		(unsigned long long) (snapshot->scheduled_end_realtime.tv_nsec / 1000));

	fprintf(f, "bus %02x, ", bus);

	fprintf(f, "gpu %s, ", format_percent(buffer, sizeof(buffer),
		collector_lane_fraction(snapshot, COLLECTOR_LANE_GUI)));
	fprintf(f, "ee %s, ", format_percent(buffer, sizeof(buffer),
		collector_lane_fraction(snapshot, COLLECTOR_LANE_EE)));
	fprintf(f, "vgt %s", format_percent(buffer, sizeof(buffer),
		collector_lane_fraction(snapshot, COLLECTOR_LANE_VGT)));

	lane_field(f, masks, snapshot, COLLECTOR_LANE_TA, "ta", ", ");
	lane_field(f, masks, snapshot, COLLECTOR_LANE_TC, "tc", ", ");
	lane_field(f, masks, snapshot, COLLECTOR_LANE_SX, "sx", ", ");
	lane_field(f, masks, snapshot, COLLECTOR_LANE_SH, "sh", ", ");
	lane_field(f, masks, snapshot, COLLECTOR_LANE_SPI, "spi", ", ");
	lane_field(f, masks, snapshot, COLLECTOR_LANE_SMX, "smx", ", ");
	lane_field(f, masks, snapshot, COLLECTOR_LANE_CR, "cr", ", ");
	lane_field(f, masks, snapshot, COLLECTOR_LANE_SC, "sc", ", ");
	lane_field(f, masks, snapshot, COLLECTOR_LANE_PA, "pa", ", ");
	lane_field(f, masks, snapshot, COLLECTOR_LANE_DB, "db", ", ");
	lane_field(f, masks, snapshot, COLLECTOR_LANE_CB, "cb", ", ");
	lane_field(f, masks, snapshot, COLLECTOR_LANE_CP, "cp", ", ");
	lane_field(f, masks, snapshot, COLLECTOR_LANE_E2, "e2", ", ");
	lane_field(f, masks, snapshot, COLLECTOR_LANE_RB2D, "rb2d", ", ");
	lane_field(f, masks, snapshot, COLLECTOR_LANE_CF, "cf", ", ");
	lane_field(f, masks, snapshot, COLLECTOR_LANE_UVD, "uvd", ", ");
	lane_field(f, masks, snapshot, COLLECTOR_LANE_VCE0, "vce0", ", ");

	// VRAM and GTT are point measurements at the window endpoint, so they
	// carry their own validity rather than the status coverage.
	if (snapshot->vram_valid && vramsize)
		fprintf(f, ", vram %.2f%% %.2fmb",
			100.0 * (double) snapshot->vram / (double) vramsize,
			(double) snapshot->vram / 1024.0 / 1024.0);

	if (snapshot->gtt_valid && gttsize)
		fprintf(f, ", gtt %.2f%% %.2fmb",
			100.0 * (double) snapshot->gtt / (double) gttsize,
			(double) snapshot->gtt / 1024.0 / 1024.0);

	// Each clock is a mean over its own valid readings, so each is gated on
	// its own validity.  Rendering the memory clock because the shader clock
	// validated would report a figure no read produced, and a zero maximum
	// would divide by zero.
	if (snapshot->mclk.valid && mclk_max)
		fprintf(f, ", mclk %.2f%% %.3fghz",
			100.0 * snapshot->mclk_mean_khz / (double) mclk_max,
			snapshot->mclk_mean_khz / 1e6);

	if (snapshot->sclk.valid && sclk_max)
		fprintf(f, ", sclk %.2f%% %.3fghz",
			100.0 * snapshot->sclk_mean_khz / (double) sclk_max,
			snapshot->sclk_mean_khz / 1e6);

	// Coverage and the schedule accounting travel with every value on the
	// line.  Read failures can correlate with load, so a duty figure without
	// its denominator and its missed-slot count cannot be interpreted after
	// the fact.
	fprintf(f, ", gen %llu, cov %s, missed %llu, late %llu, maxlate %lluus, maxread %lluus, lag %lldus",
		(unsigned long long) snapshot->generation,
		format_percent(buffer, sizeof(buffer),
			collector_status_coverage(snapshot)),
		(unsigned long long) snapshot->missed_slots,
		(unsigned long long) snapshot->late_wakeups,
		(unsigned long long) (snapshot->max_lateness_ns / 1000),
		(unsigned long long) (snapshot->max_read_latency_ns / 1000),
		(long long) (collector_timespec_delta_ns(&snapshot->window_end,
			&snapshot->published) / 1000));

	// Every enabled measurement class serializes its own denominator, so a
	// per-lane figure can be interpreted without assuming status health
	// applies to the register the lane actually came from.
	signal_health(f, "status", COLLECTOR_CAP_STATUS, snapshot, &snapshot->status);
	signal_health(f, "uvd", COLLECTOR_CAP_UVD, snapshot, &snapshot->uvd);
	signal_health(f, "vce", COLLECTOR_CAP_VCE, snapshot, &snapshot->vce);
	signal_health(f, "sclkread", COLLECTOR_CAP_SCLK, snapshot, &snapshot->sclk);
	signal_health(f, "mclkread", COLLECTOR_CAP_MCLK, snapshot, &snapshot->mclk);
	endpoint_health(f, "vramread", COLLECTOR_CAP_VRAM, snapshot, snapshot->vram_valid);
	endpoint_health(f, "gttread", COLLECTOR_CAP_GTT, snapshot, snapshot->gtt_valid);

	if (radeontop_capture_write_snapshot_evidence(f, metadata->run_id,
			masks, snapshot))
		return -1;

	return fputc('\n', f) == EOF ? -1 : 0;
}

int dumpdata(struct collector *collector, const struct engine_masks *masks,
		const struct radeontop_capture_metadata *metadata,
		const char file[], const unsigned int limit, const unsigned char bus) {

#ifdef ENABLE_NLS
	// This is a data format, so disable decimal point localization
	setlocale(LC_NUMERIC, "C");
#endif

	fprintf(stderr, _("Dumping to %s, "), file);

	if (limit)
		fprintf(stderr, _("line limit %u.\n"), limit);
	else
		fputs(_("until termination.\n"), stderr);

	// Check the file can be output to
	FILE *f = NULL;
	if (radeontop_capture_path_is_stdout(file))
		f = stdout;
	else
		f = fopen(file, "a");

	if (!f)
		die(_("Can't open file for writing."));
	// Descriptor type controls locking.  stdout can name a regular append
	// redirection, and two `-d -` processes must not interleave on that file.
	if (radeontop_capture_lock_stream(f)) {
		fprintf(stderr, _("Another capture owns the output file.\n"));
		if (f != stdout)
			fclose(f);
		return 1;
	}

	if (radeontop_capture_write_header(f, metadata)) {
		fprintf(stderr, _("Failed writing the capture provenance header.\n"));
		radeontop_capture_unlock_stream(f);
		if (f != stdout)
			fclose(f);
		return 1;
	}

	if (rs480_gart_observed.valid) {
		fprintf(f,
			"# bus %02x, rs480 candidate_gart_mc agp_base_2 0x%08x, gart_feature_id 0x%08x, gart_base 0x%08x\n",
			bus,
			rs480_gart_observed.agp_base_2,
			rs480_gart_observed.gart_feature_id,
			rs480_gart_observed.gart_base);
	}

	uint64_t last_generation = 0;
	unsigned int printed = 0;
	int status = 0;
	const char *end_reason = "signal";
	struct collector_snapshot terminal_snapshot;
	bool terminal_snapshot_valid = false;

	while (!terminate_requested) {
		struct collector_snapshot snapshot;
		struct timespec deadline;

		// A bounded wait is what lets the signal flag be observed; the
		// handler itself may only set a volatile sig_atomic_t.
		if (clock_gettime(CLOCK_MONOTONIC, &deadline)) {
			end_reason = "clock-error";
			status = 1;
			break;
		}
		deadline.tv_nsec += DUMP_POLL_MS * 1000000L;
		if (deadline.tv_nsec >= 1000000000L) {
			deadline.tv_nsec -= 1000000000L;
			deadline.tv_sec++;
		}

		const int wait = collector_wait_next_contiguous(collector, last_generation,
			&deadline, &snapshot);

		if (wait == COLLECTOR_WAIT_TIMEOUT)
			continue;

		if (wait == COLLECTOR_WAIT_FATAL) {
			fprintf(stderr, _("The collector lost the device, stopping.\n"));
			end_reason = "collector-fatal";
			terminal_snapshot = snapshot;
			terminal_snapshot_valid = true;
			status = 1;
			break;
		}

		if (wait == COLLECTOR_WAIT_FINISHED) {
			end_reason = "collector-finished";
			terminal_snapshot = snapshot;
			terminal_snapshot_valid = true;
			break;
		}
		if (wait == COLLECTOR_WAIT_ERROR) {
			fprintf(stderr, _("The collector wait failed, stopping.\n"));
			end_reason = "wait-error";
			status = 1;
			break;
		}
		if (wait == COLLECTOR_WAIT_GAP) {
			fprintf(stderr,
				_("The capture lost a completed measurement window, stopping.\n"));
			end_reason = "generation-gap";
			terminal_snapshot = snapshot;
			terminal_snapshot_valid = true;
			status = 1;
			break;
		}

		last_generation = snapshot.generation;
		if (dump_line(f, masks, metadata, &snapshot, bus)) {
			fprintf(stderr, _("Failed writing the dump output.\n"));
			end_reason = "write-error";
			status = 1;
			break;
		}

		// A disk-full or broken-pipe condition makes the capture
		// incomplete, and a research capture that lost lines is not a
		// successful run.
		if (fflush(f) || ferror(f)) {
			fprintf(stderr, _("Failed writing the dump output.\n"));
			end_reason = "write-error";
			status = 1;
			break;
		}

		printed++;
		if (limit && printed >= limit) {
			end_reason = "line-limit";
			break;
		}
	}

	if (!ferror(f) && radeontop_capture_write_run_end(f, metadata->run_id,
			end_reason, status, last_generation, printed,
			terminal_snapshot_valid ? &terminal_snapshot : NULL))
		status = 1;
	if (radeontop_capture_sync_stream(f))
		status = 1;
	if (radeontop_capture_unlock_stream(f))
		status = 1;
	if (f != stdout) {
		if (fclose(f))
			status = 1;
	}

	return status;
}
