/*
	Copyright (C) 2012 Lauri Kasanen

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

#include "capture.h"

#include <ctype.h>
#include <math.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

static bool uuid_text_valid(const char *text) {
	static const size_t hyphens[] = { 8, 13, 18, 23 };
	const size_t length = strlen(text);

	if (length != RADEONTOP_CAPTURE_UUID_SIZE - 1)
		return false;

	for (size_t index = 0; index < length; index++) {
		bool hyphen = false;

		for (size_t slot = 0; slot < sizeof(hyphens) / sizeof(hyphens[0]); slot++)
			if (index == hyphens[slot])
				hyphen = true;

		if ((hyphen && text[index] != '-') ||
			(!hyphen && !isxdigit((unsigned char) text[index])))
			return false;
	}

	return true;
}

static bool lowercase_hex_text_valid(const char *text, size_t length) {
	if (!text || strlen(text) != length)
		return false;

	for (size_t index = 0; index < length; index++)
		if (!((text[index] >= '0' && text[index] <= '9') ||
			(text[index] >= 'a' && text[index] <= 'f')))
			return false;

	return true;
}

static bool git_object_id_valid(const char *text) {
	const size_t length = text ? strlen(text) : 0;

	return (length == 40 || length == 64) &&
		lowercase_hex_text_valid(text, length);
}

static bool build_identity_valid(
		const struct radeontop_build_identity *identity) {
	if (!identity || !identity->version || !identity->version[0] ||
		!identity->source_commit || !identity->source_state ||
		!identity->source_sha256 || !identity->source_manifest ||
		!identity->manifest_sha256 || !identity->build_manifest)
		return false;

	// A capture is a replayable research record.  A clean object supplies every
	// source byte named by the embedded source manifest; dirty or unknown trees
	// have no immutable object from which a later reader can recover those bytes.
	if (!git_object_id_valid(identity->source_commit))
		return false;
	if (strcmp(identity->source_state, "clean") != 0)
		return false;

	return lowercase_hex_text_valid(identity->source_sha256, 64) &&
		lowercase_hex_text_valid(identity->manifest_sha256, 64);
}

static int read_uuid(const char *path, char out[RADEONTOP_CAPTURE_UUID_SIZE]) {
	char line[64];
	FILE *stream = fopen(path, "r");
	char *line_ending;

	if (!stream)
		return -1;

	if (!fgets(line, sizeof(line), stream)) {
		(void) fclose(stream);
		return -1;
	}

	if (fclose(stream))
		return -1;

	line_ending = strpbrk(line, "\r\n");
	if (line_ending)
		*line_ending = '\0';
	if (!uuid_text_valid(line))
		return -1;

	memcpy(out, line, RADEONTOP_CAPTURE_UUID_SIZE);
	return 0;
}

static bool copy_text(char *destination, size_t size, const char *source) {
	const int written = snprintf(destination, size, "%s", source);

	return written >= 0 && (size_t) written < size;
}

int radeontop_capture_metadata_init(struct radeontop_capture_metadata *metadata,
		const struct radeontop_build_identity *build_identity,
		const char *family_name,
		int argc, char *const argv[],
		const struct radeon_device_identity *identity,
		const struct collector_config *config,
		uint64_t vram_size, uint64_t gtt_size,
		uint32_t sclk_max_khz, uint32_t mclk_max_khz) {
	struct utsname uts;

	if (!metadata || !build_identity_valid(build_identity) || !family_name || argc < 0 ||
		(argc && !argv) || !identity || !config)
		return -1;

	memset(metadata, 0, sizeof(*metadata));
	if (read_uuid("/proc/sys/kernel/random/uuid", metadata->run_id))
		return -2;
	if (read_uuid("/proc/sys/kernel/random/boot_id", metadata->boot_id))
		return -3;
	if (uname(&uts))
		return -4;
	if (clock_gettime(CLOCK_REALTIME, &metadata->started_realtime) ||
		clock_gettime(CLOCK_MONOTONIC, &metadata->started_monotonic))
		return -5;
	if (!copy_text(metadata->kernel_release, sizeof(metadata->kernel_release),
			uts.release) ||
		!copy_text(metadata->source_version, sizeof(metadata->source_version),
			build_identity->version) ||
		!copy_text(metadata->source_commit, sizeof(metadata->source_commit),
			build_identity->source_commit) ||
		!copy_text(metadata->source_state, sizeof(metadata->source_state),
			build_identity->source_state) ||
		!copy_text(metadata->source_sha256, sizeof(metadata->source_sha256),
			build_identity->source_sha256) ||
		!copy_text(metadata->build_manifest_sha256,
			sizeof(metadata->build_manifest_sha256),
			build_identity->manifest_sha256) ||
		!copy_text(metadata->family_name, sizeof(metadata->family_name),
			family_name))
		return -6;

	metadata->argc = argc;
	metadata->argv = argv;
	metadata->source_manifest = build_identity->source_manifest;
	metadata->build_manifest = build_identity->build_manifest;
	metadata->identity = *identity;
	metadata->config = *config;
	metadata->vram_size = vram_size;
	metadata->gtt_size = gtt_size;
	metadata->sclk_max_khz = sclk_max_khz;
	metadata->mclk_max_khz = mclk_max_khz;

	return 0;
}

static int write_json_string(FILE *stream, const char *text) {
	const unsigned char *cursor = (const unsigned char *) text;

	if (fputc('"', stream) == EOF)
		return -1;

	while (*cursor) {
		switch (*cursor) {
			case '"':
				if (fputs("\\\"", stream) == EOF)
					return -1;
				break;
			case '\\':
				if (fputs("\\\\", stream) == EOF)
					return -1;
				break;
			case '\b':
				if (fputs("\\b", stream) == EOF)
					return -1;
				break;
			case '\f':
				if (fputs("\\f", stream) == EOF)
					return -1;
				break;
			case '\n':
				if (fputs("\\n", stream) == EOF)
					return -1;
				break;
			case '\r':
				if (fputs("\\r", stream) == EOF)
					return -1;
				break;
			case '\t':
				if (fputs("\\t", stream) == EOF)
					return -1;
				break;
			default:
				// JSON strings carry Unicode, while argv is an arbitrary byte
				// vector. Escaping every non-ASCII byte keeps the record valid,
				// ASCII-only, and byte-recoverable through the U+00xx value.
				if (*cursor < 0x20 || *cursor >= 0x7f) {
					if (fprintf(stream, "\\u%04x", *cursor) < 0)
						return -1;
				} else if (fputc(*cursor, stream) == EOF) {
					return -1;
				}
		}
		cursor++;
	}

	return fputc('"', stream) == EOF ? -1 : 0;
}

static int write_timespec(FILE *stream, const struct timespec *timestamp) {
	return fprintf(stream, "{\"sec\":%lld,\"nsec\":%ld}",
		(long long) timestamp->tv_sec, timestamp->tv_nsec) < 0 ? -1 : 0;
}

bool radeontop_capture_path_is_stdout(const char *path) {
	return path && !strcmp(path, "-");
}

int radeontop_capture_lock_stream(FILE *stream) {
	struct stat status;

	if (!stream || fstat(fileno(stream), &status))
		return -1;
	if (!S_ISREG(status.st_mode))
		return 0;

	return flock(fileno(stream), LOCK_EX | LOCK_NB) ? -1 : 0;
}

int radeontop_capture_unlock_stream(FILE *stream) {
	struct stat status;

	if (!stream || fstat(fileno(stream), &status))
		return -1;
	if (!S_ISREG(status.st_mode))
		return 0;

	return flock(fileno(stream), LOCK_UN) ? -1 : 0;
}

int radeontop_capture_sync_stream(FILE *stream) {
	struct stat status;

	if (!stream || fflush(stream) || ferror(stream) ||
		fstat(fileno(stream), &status))
		return -1;
	if (S_ISREG(status.st_mode) && fsync(fileno(stream)))
		return -1;

	return 0;
}

int radeontop_capture_write_append_boundary(FILE *stream) {
	struct stat status;

	if (!stream || fflush(stream) || ferror(stream) ||
		fstat(fileno(stream), &status))
		return -1;
	if (!S_ISREG(status.st_mode) || !status.st_size)
		return 0;
	if (fseek(stream, 0, SEEK_END))
		return -1;

	return fputc('\n', stream) == EOF ? -1 : 0;
}

int radeontop_capture_write_header(FILE *stream,
		const struct radeontop_capture_metadata *metadata) {
	const struct radeon_device_identity *identity;

	if (!stream || !metadata || !metadata->source_manifest ||
		!metadata->build_manifest)
		return -1;

	identity = &metadata->identity;
	if (fputs("# radeontop_capture_v1 {\"schema\":\"radeontop_capture_v1\",\"run_id\":", stream) == EOF ||
		write_json_string(stream, metadata->run_id) ||
		fputs(",\"boot_id\":", stream) == EOF ||
		write_json_string(stream, metadata->boot_id) ||
		fputs(",\"build\":{\"version\":", stream) == EOF ||
		write_json_string(stream, metadata->source_version) ||
		fputs(",\"source_commit\":", stream) == EOF ||
		write_json_string(stream, metadata->source_commit) ||
		fputs(",\"source_state\":", stream) == EOF ||
		write_json_string(stream, metadata->source_state) ||
		fputs(",\"source_sha256\":", stream) == EOF ||
		write_json_string(stream, metadata->source_sha256) ||
		fputs(",\"source_manifest_encoding\":\"byte-u00xx\",\"source_manifest\":", stream) == EOF ||
		write_json_string(stream, metadata->source_manifest) ||
		fputs(",\"manifest_sha256\":", stream) == EOF ||
		write_json_string(stream, metadata->build_manifest_sha256) ||
		fputs(",\"manifest_encoding\":\"byte-u00xx\",\"manifest\":", stream) == EOF ||
		write_json_string(stream, metadata->build_manifest) ||
		fputs("}", stream) == EOF ||
		fputs(",\"kernel_release\":", stream) == EOF ||
		write_json_string(stream, metadata->kernel_release) ||
		fputs(",\"started_realtime\":", stream) == EOF ||
		write_timespec(stream, &metadata->started_realtime) ||
		fputs(",\"started_monotonic\":", stream) == EOF ||
		write_timespec(stream, &metadata->started_monotonic) ||
		fputs(",\"argv_encoding\":\"byte-u00xx\",\"argv\":[", stream) == EOF)
		return -1;

	for (int index = 0; index < metadata->argc; index++) {
		if ((index && fputc(',', stream) == EOF) ||
			write_json_string(stream, metadata->argv[index]))
			return -1;
	}

	if (fputs("],\"device\":{\"bdf\":", stream) == EOF)
		return -1;
	if (identity->pci_address_valid) {
		char bdf[16];

		const int bdf_length = snprintf(bdf, sizeof(bdf),
				"%04x:%02x:%02x.%u",
				identity->domain, identity->bus, identity->device,
				identity->function);

		if (bdf_length < 0 || (size_t) bdf_length >= sizeof(bdf))
			return -1;
		if (write_json_string(stream, bdf) ||
			fprintf(stream, ",\"vendor_id\":\"%04x\",\"device_id\":\"%04x\"",
				identity->vendor_id, identity->device_id) < 0)
			return -1;
	} else if (fputs("null,\"vendor_id\":null,\"device_id\":null", stream) == EOF) {
		return -1;
	}

	if (fputs(",\"family\":", stream) == EOF ||
		write_json_string(stream, metadata->family_name) ||
		fputs(",\"drm_driver\":", stream) == EOF)
		return -1;
	if (identity->drm_driver[0]) {
		if (write_json_string(stream, identity->drm_driver) ||
			fprintf(stream, ",\"drm_version\":\"%u.%u.%u\"",
				identity->drm_version_major, identity->drm_version_minor,
				identity->drm_version_patchlevel) < 0)
			return -1;
	} else if (fputs("null,\"drm_version\":null", stream) == EOF) {
		return -1;
	}

	if (fputs(",\"status_source\":", stream) == EOF ||
		write_json_string(stream,
			radeon_status_source_name(identity->status_source)) ||
		fputs(",\"status_register\":", stream) == EOF)
		return -1;
	if (identity->status_register) {
		if (fprintf(stream, "{\"name\":\"%s\",\"offset\":\"0x%08x\"}",
				radeon_status_register_name(identity->status_register),
				identity->status_register) < 0)
			return -1;
	} else if (fputs("null", stream) == EOF) {
		return -1;
	}

	if (fputs(",\"resource\":", stream) == EOF)
		return -1;
	if (identity->resource_index >= 0) {
		if (fprintf(stream, "{\"index\":%d,\"size\":%llu}",
				identity->resource_index,
				(unsigned long long) identity->resource_size) < 0)
			return -1;
	} else if (fputs("null", stream) == EOF) {
		return -1;
	}

	if (fprintf(stream,
			"},\"sampling\":{\"ticks_per_second\":%u,\"window_seconds\":%u,\"dither_seed\":%llu},"
			"\"limits\":{\"vram_bytes\":%llu,\"gtt_bytes\":%llu,\"sclk_max_khz\":%u,\"mclk_max_khz\":%u}}\n",
			metadata->config.ticks, metadata->config.dumpinterval,
			(unsigned long long) metadata->config.dither_seed,
			(unsigned long long) metadata->vram_size,
			(unsigned long long) metadata->gtt_size,
			metadata->sclk_max_khz, metadata->mclk_max_khz) < 0)
		return -1;

	return ferror(stream) ? -1 : 0;
}

static int write_signal(FILE *stream, const char *name, uint32_t capability,
		const struct collector_snapshot *snapshot,
		const struct collector_signal_stats *stats, bool first) {
	return fprintf(stream,
		"%s\"%s\":{\"supported\":%s,\"valid\":%llu,\"failed\":%llu}",
		first ? "" : ",", name,
		(snapshot->capabilities & capability) ? "true" : "false",
		(unsigned long long) stats->valid,
		(unsigned long long) stats->failed) < 0 ? -1 : 0;
}

static int write_endpoint(FILE *stream, const char *name, uint32_t capability,
		const struct collector_snapshot *snapshot, bool valid, uint64_t bytes,
		bool first) {
	if (fprintf(stream, "%s\"%s\":{\"supported\":%s,\"valid\":%s,\"bytes\":",
			first ? "" : ",", name,
			(snapshot->capabilities & capability) ? "true" : "false",
			valid ? "true" : "false") < 0)
		return -1;

	if (valid) {
		if (fprintf(stream, "%llu", (unsigned long long) bytes) < 0)
			return -1;
	} else if (fputs("null", stream) == EOF) {
		return -1;
	}

	return fputc('}', stream) == EOF ? -1 : 0;
}

static int write_clock_mean(FILE *stream, const char *name,
		const struct collector_signal_stats *stats, double mean_khz,
		bool first) {
	if (fprintf(stream, "%s\"%s\":", first ? "" : ",", name) < 0)
		return -1;
	if (!stats->valid)
		return fputs("null", stream) == EOF ? -1 : 0;

	return fprintf(stream, "%.9f", mean_khz) < 0 ? -1 : 0;
}

int radeontop_capture_write_snapshot_evidence(FILE *stream,
		const char *run_id,
		const struct engine_masks *masks,
		const struct collector_snapshot *snapshot) {
	static const char *const lane_names[] = {
		"ee", "vgt", "gpu", "ta", "tc", "sx", "sh", "spi", "smx",
		"sc", "pa", "db", "cb", "cr", "cp", "e2", "rb2d", "cf",
		"uvd", "vce0"
	};
	bool first = true;

	_Static_assert(sizeof(lane_names) / sizeof(lane_names[0]) ==
		COLLECTOR_LANE_COUNT, "capture lane names must cover every lane");

	if (!stream || !run_id || !uuid_text_valid(run_id) || !masks || !snapshot)
		return -1;

	if (fputs(", evidence_v2 {\"run_id\":", stream) == EOF ||
		write_json_string(stream, run_id) ||
		fprintf(stream,
			",\"generation\":%llu,\"capabilities\":%u,\"slots\":{\"nominal\":%llu,\"attempted\":%llu,\"missed\":%llu},"
			"\"time\":{\"window_start_monotonic\":",
			(unsigned long long) snapshot->generation,
			snapshot->capabilities,
			(unsigned long long) snapshot->nominal_slots,
			(unsigned long long) snapshot->attempted_slots,
			(unsigned long long) snapshot->missed_slots) < 0 ||
		write_timespec(stream, &snapshot->window_start) ||
		fputs(",\"window_end_monotonic\":", stream) == EOF ||
		write_timespec(stream, &snapshot->window_end) ||
		fputs(",\"scheduled_end_realtime\":", stream) == EOF ||
		write_timespec(stream, &snapshot->scheduled_end_realtime) ||
		fputs(",\"published_monotonic\":", stream) == EOF ||
		write_timespec(stream, &snapshot->published) ||
		fputs(",\"published_realtime\":", stream) == EOF ||
		write_timespec(stream, &snapshot->published_realtime) ||
		fprintf(stream,
			"},\"timing\":{\"late_wakeups\":%llu,\"max_lateness_ns\":%llu,\"max_read_latency_ns\":%llu,"
			"\"mean_read_latency_ns\":%llu,\"read_latency_samples\":%llu},\"signals\":{",
			(unsigned long long) snapshot->late_wakeups,
			(unsigned long long) snapshot->max_lateness_ns,
			(unsigned long long) snapshot->max_read_latency_ns,
			(unsigned long long) snapshot->mean_read_latency_ns,
			(unsigned long long) snapshot->read_latency_samples) < 0)
		return -1;

	if (write_signal(stream, "status", COLLECTOR_CAP_STATUS, snapshot,
			&snapshot->status, true) ||
		write_signal(stream, "uvd", COLLECTOR_CAP_UVD, snapshot,
			&snapshot->uvd, false) ||
		write_signal(stream, "vce", COLLECTOR_CAP_VCE, snapshot,
			&snapshot->vce, false) ||
		write_signal(stream, "sclk", COLLECTOR_CAP_SCLK, snapshot,
			&snapshot->sclk, false) ||
		write_signal(stream, "mclk", COLLECTOR_CAP_MCLK, snapshot,
			&snapshot->mclk, false) ||
		fputs("},\"clock_means_khz\":{", stream) == EOF ||
		write_clock_mean(stream, "sclk", &snapshot->sclk,
			snapshot->sclk_mean_khz, true) ||
		write_clock_mean(stream, "mclk", &snapshot->mclk,
			snapshot->mclk_mean_khz, false) ||
		fputs("},\"endpoints\":{", stream) == EOF ||
		write_endpoint(stream, "vram", COLLECTOR_CAP_VRAM, snapshot,
			snapshot->vram_valid, snapshot->vram, true) ||
		write_endpoint(stream, "gtt", COLLECTOR_CAP_GTT, snapshot,
			snapshot->gtt_valid, snapshot->gtt, false) ||
		fputs("},\"lanes\":{", stream) == EOF)
		return -1;

	for (int lane = 0; lane < COLLECTOR_LANE_COUNT; lane++) {
		struct collector_lane_bounds bounds;

		if (!masks->lane[lane])
			continue;
		if (!collector_lane_missing_data_bounds(snapshot,
				(enum collector_lane) lane, &bounds))
			return -1;

		if (fprintf(stream,
				"%s\"%s\":{\"busy\":%llu,\"valid\":%llu,\"nominal\":%llu,\"conditional\":",
				first ? "" : ",", lane_names[lane],
				(unsigned long long) bounds.busy,
				(unsigned long long) bounds.valid,
				(unsigned long long) bounds.nominal) < 0)
			return -1;

		if (isnan(bounds.conditional_fraction)) {
			if (fputs("null", stream) == EOF)
				return -1;
		} else if (fprintf(stream, "%.9f", bounds.conditional_fraction) < 0) {
			return -1;
		}

		if (fprintf(stream,
				",\"unconditional\":[%.9f,%.9f]}",
				bounds.unconditional_lower,
				bounds.unconditional_upper) < 0)
			return -1;
		first = false;
	}

	// The per-position census keys by bit index and omits positions that never
	// asserted, so an idle window writes an empty object and a consumer reads
	// an absent key as zero.  The denominator is signals.status.valid, the same
	// one the lanes carry, because both count over the same valid reads.
	if (fputs("},\"status_bits\":{", stream) == EOF)
		return -1;

	first = true;
	for (int bit = 0; bit < COLLECTOR_STATUS_BIT_COUNT; bit++) {
		if (!snapshot->status_bit_busy[bit])
			continue;
		if (fprintf(stream, "%s\"%d\":%llu", first ? "" : ",", bit,
				(unsigned long long) snapshot->status_bit_busy[bit]) < 0)
			return -1;
		first = false;
	}

	return fputs("}}", stream) == EOF || ferror(stream) ? -1 : 0;
}

int radeontop_capture_write_run_end(FILE *stream, const char *run_id,
		const char *reason, int exit_status, uint64_t last_generation,
		unsigned int record_count,
		const struct collector_snapshot *collector_snapshot,
		const struct collector_terminal *terminal) {
	const char *terminal_cause = terminal ?
		collector_terminal_cause_name(terminal->cause) : NULL;
	uint64_t collector_generation = 0;

	if (!stream || !run_id || !uuid_text_valid(run_id) || !reason || !reason[0])
		return -1;
	if (terminal && (terminal->cause == COLLECTOR_TERMINAL_NONE ||
		!strcmp(terminal_cause, "unknown") ||
		(terminal->cause == COLLECTOR_TERMINAL_DEVICE_READ) !=
			terminal->read_result_valid ||
		(terminal->read_result_valid &&
			terminal->read_result != COLLECTOR_READ_FATAL)))
		return -1;
	if (collector_snapshot && terminal &&
		collector_snapshot->generation != terminal->after_generation)
		return -1;
	if (terminal && terminal->after_generation && !collector_snapshot)
		return -1;
	if ((collector_snapshot &&
		collector_snapshot->generation < last_generation) ||
		(terminal && terminal->after_generation < last_generation) ||
		(last_generation && !collector_snapshot && !terminal))
		return -1;
	if (collector_snapshot)
		collector_generation = collector_snapshot->generation;
	else if (terminal)
		collector_generation = terminal->after_generation;

	if (fputs("# radeontop_run_end_v2 {\"run_id\":", stream) == EOF ||
		write_json_string(stream, run_id) ||
		fputs(",\"reason\":", stream) == EOF ||
		write_json_string(stream, reason) ||
		fprintf(stream,
			",\"logical_complete\":%s,\"logical_status\":%d,\"last_generation\":%llu,\"records\":%u,\"collector\":",
			exit_status == 0 ? "true" : "false", exit_status,
			(unsigned long long) last_generation, record_count) < 0)
		return -1;

	if (collector_snapshot || terminal) {
		if (fprintf(stream, "{\"latest_generation\":%llu,\"terminal\":",
				(unsigned long long) collector_generation) < 0)
			return -1;
		if (terminal) {
			if (fputs("{\"after_generation\":", stream) == EOF ||
				fprintf(stream, "%llu,\"cause\":",
					(unsigned long long) terminal->after_generation) < 0 ||
				write_json_string(stream, terminal_cause) ||
				fputs(",\"read_result\":", stream) == EOF)
				return -1;
			if (terminal->read_result_valid) {
				if (fprintf(stream, "%d", terminal->read_result) < 0)
					return -1;
			} else if (fputs("null", stream) == EOF) {
				return -1;
			}
			if (fputc('}', stream) == EOF)
				return -1;
		} else if (fputs("null", stream) == EOF) {
			return -1;
		}
		if (fputc('}', stream) == EOF)
			return -1;
	} else if (fputs("null", stream) == EOF) {
		return -1;
	}

	return fputs("}\n", stream) == EOF || ferror(stream) ? -1 : 0;
}
