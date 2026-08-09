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

#include "radeontop.h"

#include <math.h>
#include <ncurses.h>
#include <stdarg.h>
#include <stdio.h>

static void printcenter(const unsigned int y, const unsigned int width,
				const char * const fmt, ...) {

	char *ptr;
	va_list ap;
	va_start(ap, fmt);

	// vasprintf leaves ptr indeterminate when it fails.  The failed-allocation
	// branch drops the line before mbstowcs, mvprintw, or free receives ptr.
#ifdef ENABLE_NLS
	if (vasprintf(&ptr, fmt, ap) < 0) {
		va_end(ap);
		return;
	}
	const unsigned int len = mbstowcs(NULL, ptr, 0);
#else
	const int written = vasprintf(&ptr, fmt, ap);
	if (written < 0) {
		va_end(ap);
		return;
	}
	const unsigned int len = written;
#endif

	unsigned x = (width - len)/2;
	if (len > width) x = 0;

	mvprintw(y, x, "%s", ptr);

	va_end(ap);
	free(ptr);
}

static void printright(const unsigned int y, const unsigned int width,
				const char * const fmt, ...) {

	char *ptr;
	va_list ap;
	va_start(ap, fmt);

	// vasprintf leaves ptr indeterminate when it fails.  The failed-allocation
	// branch drops the line before mbstowcs, mvprintw, or free receives ptr.
#ifdef ENABLE_NLS
	if (vasprintf(&ptr, fmt, ap) < 0) {
		va_end(ap);
		return;
	}
	const unsigned int len = mbstowcs(NULL, ptr, 0);
#else
	const int written = vasprintf(&ptr, fmt, ap);
	if (written < 0) {
		va_end(ap);
		return;
	}
	const unsigned int len = written;
#endif

	unsigned x = (width - len);
	if (len > width) x = 0;

	mvprintw(y, x, "%s", ptr);

	va_end(ap);
	free(ptr);
}

// A lane with no valid read has an undefined duty.  It draws no bar, and the
// label beside it renders nan, so an unreadable block never looks idle.
static void percentage(const unsigned int y, const unsigned int w, const float p) {

	if (isnan(p))
		return;

	const unsigned int x = (w/2) + 2;
	unsigned int len = w - x - 1;

	len = roundf(len * (p / 100.0f));

	attron(A_REVERSE);
	mvhline(y, x, ' ', len);
	attroff(A_REVERSE);
}

// Waits in bounded steps so a termination signal is observed while waiting.
// Returns a collector_wait_result.
static int wait_bounded(struct collector *collector, uint64_t after,
		struct collector_snapshot *snapshot_out,
		struct collector_terminal *terminal_out) {
	struct timespec deadline;

	if (clock_gettime(CLOCK_MONOTONIC, &deadline))
		return COLLECTOR_WAIT_ERROR;

	deadline.tv_nsec += 200000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_nsec -= 1000000000L;
		deadline.tv_sec++;
	}

	return collector_wait_next(collector, after, &deadline, snapshot_out,
		terminal_out);
}

static void report_terminal(enum collector_terminal_cause cause) {
	if (cause == COLLECTOR_TERMINAL_DEVICE_READ)
		fprintf(stderr, _("The collector lost the device, stopping.\n"));
	else if (collector_terminal_cause_is_clock(cause))
		fprintf(stderr, _("The collector clock failed, stopping.\n"));
	else
		fprintf(stderr, _("The collector schedule failed, stopping.\n"));
}

int present(struct collector *collector, const struct engine_masks *masks,
		const char card[], unsigned int color,
		unsigned int transparency, const unsigned char bus) {

	const unsigned int ticks = collector->config.ticks;
	const unsigned int dumpinterval = collector->config.dumpinterval;
	struct collector_snapshot snapshot;
	struct collector_terminal terminal;
	int status = 0;

	printf(_("Collecting data, please wait....\n"));

	memset(&snapshot, 0, sizeof(snapshot));

	// Draw nothing until one whole window completes; a partial window is not
	// a measurement.
	for (;;) {
		const int wait = wait_bounded(collector, 0, &snapshot, &terminal);

		if (wait == COLLECTOR_WAIT_SNAPSHOT)
			break;

		if (wait == COLLECTOR_WAIT_FATAL) {
			report_terminal(terminal.cause);
			return 1;
		}
		if (wait == COLLECTOR_WAIT_ERROR) {
			fprintf(stderr, _("The collector wait failed, stopping.\n"));
			return 1;
		}

		if (wait == COLLECTOR_WAIT_FINISHED || terminate_requested)
			return 0;
	}

	SCREEN *screen = newterm(NULL, stderr, stdin);
	if (!screen) {
		fprintf(stderr, _("Failed to initialize the terminal interface.\n"));
		return 1;
	}
	noecho();
	halfdelay(10);
	curs_set(0);
	clear();

	start_color();
	if (transparency) {
		use_default_colors();
		init_pair(1, COLOR_GREEN, -1);
		init_pair(2, COLOR_RED, -1);
		init_pair(3, COLOR_CYAN, -1);
		init_pair(4, COLOR_MAGENTA, -1);
		init_pair(5, COLOR_YELLOW, -1);
	} else {
		init_pair(1, COLOR_GREEN, COLOR_BLACK);
		init_pair(2, COLOR_RED, COLOR_BLACK);
		init_pair(3, COLOR_CYAN, COLOR_BLACK);
		init_pair(4, COLOR_MAGENTA, COLOR_BLACK);
		init_pair(5, COLOR_YELLOW, COLOR_BLACK);
	}

	const unsigned int bigh = 26;

	// Screen dimensions. (Re)calculated only when resize is non-zero.
	unsigned int h = 1, w = 1, hw = 1;
	int resize = 1;

	while (!terminate_requested) {
		if (resize) {
			resize = 0;
			getmaxyx(stdscr, h, w);
			hw = w/2;
		}

		//draw the header
		move(0,0);
		attron(A_REVERSE);
		mvhline(0, 0, ' ', w);
		printcenter(0, w, _("radeontop %s, running on %s bus %02x, %u samples/sec"),
			    VERSION, card, bus, ticks);
		attroff(A_REVERSE);

		move(1,0);
		clrtobot();

		// One published generation is one completed measurement window.
		// The snapshot is copied whole under the collector's mutex, so
		// every displayed figure comes from one window rather than from a
		// structure the collector is still writing.
		collector_peek(collector, &snapshot);
		const bool terminal_observed = collector_terminal_peek(collector, &terminal);

		struct timespec drawn_at;
		if (clock_gettime(CLOCK_MONOTONIC, &drawn_at)) {
			fprintf(stderr, _("Failed to read the display clock.\n"));
			status = 1;
			break;
		}

		const double age_s =
			collector_timespec_delta_ns(&snapshot.published, &drawn_at) / 1e9;

		float ee = 100.0f * (float) collector_lane_fraction(&snapshot, COLLECTOR_LANE_EE);
		float vgt = 100.0f * (float) collector_lane_fraction(&snapshot, COLLECTOR_LANE_VGT);
		float gui = 100.0f * (float) collector_lane_fraction(&snapshot, COLLECTOR_LANE_GUI);
		float ta = 100.0f * (float) collector_lane_fraction(&snapshot, COLLECTOR_LANE_TA);
		float tc = 100.0f * (float) collector_lane_fraction(&snapshot, COLLECTOR_LANE_TC);
		float sx = 100.0f * (float) collector_lane_fraction(&snapshot, COLLECTOR_LANE_SX);
		float sh = 100.0f * (float) collector_lane_fraction(&snapshot, COLLECTOR_LANE_SH);
		float spi = 100.0f * (float) collector_lane_fraction(&snapshot, COLLECTOR_LANE_SPI);
		float smx = 100.0f * (float) collector_lane_fraction(&snapshot, COLLECTOR_LANE_SMX);
		float sc = 100.0f * (float) collector_lane_fraction(&snapshot, COLLECTOR_LANE_SC);
		float pa = 100.0f * (float) collector_lane_fraction(&snapshot, COLLECTOR_LANE_PA);
		float db = 100.0f * (float) collector_lane_fraction(&snapshot, COLLECTOR_LANE_DB);
		float cr = 100.0f * (float) collector_lane_fraction(&snapshot, COLLECTOR_LANE_CR);
		float cb = 100.0f * (float) collector_lane_fraction(&snapshot, COLLECTOR_LANE_CB);
		float uvd = 100.0f * (float) collector_lane_fraction(&snapshot, COLLECTOR_LANE_UVD);
		float vce0 = 100.0f * (float) collector_lane_fraction(&snapshot, COLLECTOR_LANE_VCE0);
		float cp = 100.0f * (float) collector_lane_fraction(&snapshot, COLLECTOR_LANE_CP);
		float e2 = 100.0f * (float) collector_lane_fraction(&snapshot, COLLECTOR_LANE_E2);
		float rb2d = 100.0f * (float) collector_lane_fraction(&snapshot, COLLECTOR_LANE_RB2D);
		float cf = 100.0f * (float) collector_lane_fraction(&snapshot, COLLECTOR_LANE_CF);

		// VRAM and GTT are point measurements at the window endpoint.
		float vram = vramsize ? 100.0f * snapshot.vram / vramsize : 0;
		float vrammb = snapshot.vram / 1024.0f / 1024.0f;
		float vramsizemb = vramsize / 1024.0f / 1024.0f;
		float gtt = gttsize ? 100.0f * snapshot.gtt / gttsize : 0;
		float gttmb = snapshot.gtt / 1024.0f / 1024.0f;
		float gttsizemb = gttsize / 1024.0f / 1024.0f;

		// The clock figures are means over their own valid readings, not
		// over the sample slots.
		float mclk = mclk_max ? 100.0f * (float) snapshot.mclk_mean_khz / mclk_max : 0;
		float sclk = sclk_max ? 100.0f * (float) snapshot.sclk_mean_khz / sclk_max : 0;
		float mclk_ghz = (float) snapshot.mclk_mean_khz / 1e6f;
		float sclk_ghz = (float) snapshot.sclk_mean_khz / 1e6f;

		// A reader must be able to tell a fresh measurement from a
		// still-current one, a stalled collector from a failed one.  The
		// generation counts completed windows, the age is time since that
		// window published, and the coverage is valid status reads over
		// nominal slots.
		if (terminal_observed) {
			if (terminal.cause == COLLECTOR_TERMINAL_DEVICE_READ)
				mvprintw(1, 0, "%s", _("collector: device lost"));
			else if (collector_terminal_cause_is_clock(terminal.cause))
				mvprintw(1, 0, "%s", _("collector: clock failed"));
			else
				mvprintw(1, 0, "%s", _("collector: schedule failed"));
			status = 1;
		} else if (age_s > 2.0 * dumpinterval) {
			mvprintw(1, 0, _("gen %llu  age %.1fs  STALE"),
				(unsigned long long) snapshot.generation, age_s);
		} else {
			mvprintw(1, 0,
				_("gen %llu  age %.1fs  coverage %.1f%%  missed %llu  failed %llu"),
				(unsigned long long) snapshot.generation, age_s,
				100.0 * collector_status_coverage(&snapshot),
				(unsigned long long) snapshot.missed_slots,
				(unsigned long long) snapshot.status.failed);
		}

		mvhline(3, 0, ACS_HLINE, w);
		mvvline(1, (w/2) + 1, ACS_VLINE, h);
		mvaddch(3, (w/2) + 1, ACS_PLUS);

		if (color) attron(COLOR_PAIR(1));
		percentage(2, w, gui);
		printright(2, hw, _("Graphics pipe %6.2f%%"), gui);
		if (color) attroff(COLOR_PAIR(1));

		unsigned int start = 4;

		percentage(start, w, ee);
		printright(start++, hw, _("Event Engine %6.2f%%"), ee);

		// Enough height?
		if (h > bigh) start++;

		if (color) attron(COLOR_PAIR(2));
		percentage(start, w, vgt);
		printright(start++, hw, _("Vertex Grouper + Tesselator %6.2f%%"), vgt);
		if (color) attroff(COLOR_PAIR(2));

		// Enough height?
		if (h > bigh) start++;

		// A zero mask means the block is absent on the selected family, or its
		// exposure on the target remains unconfirmed and the lane stays
		// unexposed until an observation supports it, so the row is
		// dropped rather than rendered as a perpetual 0.00%.
		if (color) attron(COLOR_PAIR(3));
		if (masks->lane[COLLECTOR_LANE_TA]) {
			percentage(start, w, ta);
			printright(start++, hw, _("Texture Addresser %6.2f%%"), ta);
		}

		// This is only present on R600
		if (masks->lane[COLLECTOR_LANE_TC]) {
			percentage(start, w, tc);
			printright(start++, hw, _("Texture Cache %6.2f%%"), tc);
		}
		if (color) attroff(COLOR_PAIR(3));

		// Enough height?
		if (h > bigh) start++;

		if (color) attron(COLOR_PAIR(4));
		if (masks->lane[COLLECTOR_LANE_SX]) {
			percentage(start, w, sx);
			printright(start++, hw, _("Shader Export %6.2f%%"), sx);
		}

		if (masks->lane[COLLECTOR_LANE_SH]) {
			percentage(start, w, sh);
			printright(start++, hw, _("Sequencer Instruction Cache %6.2f%%"), sh);
		}

		if (masks->lane[COLLECTOR_LANE_SPI]) {
			percentage(start, w, spi);
			printright(start++, hw, _("Shader Interpolator %6.2f%%"), spi);
		}

		// only on R600
		if (masks->lane[COLLECTOR_LANE_SMX]) {
			percentage(start, w, smx);
			printright(start++, hw, _("Shader Memory Exchange %6.2f%%"), smx);
		}
		if (color) attroff(COLOR_PAIR(4));

		// Enough height?
		if (h > bigh) start++;

		if (masks->lane[COLLECTOR_LANE_SC]) {
			percentage(start, w, sc);
			printright(start++, hw, _("Scan Converter %6.2f%%"), sc);
		}

		percentage(start, w, pa);
		printright(start++, hw, _("Primitive Assembly %6.2f%%"), pa);

		// Enough height?
		if (h > bigh) start++;

		if (color) attron(COLOR_PAIR(5));
		if (masks->lane[COLLECTOR_LANE_DB]) {
			percentage(start, w, db);
			printright(start++, hw, _("Depth Block %6.2f%%"), db);
		}

		if (masks->lane[COLLECTOR_LANE_CB]) {
			percentage(start, w, cb);
			printright(start++, hw, _("Color Block %6.2f%%"), cb);
		}

		// Only present on R600
		if (masks->lane[COLLECTOR_LANE_CR]) {
			percentage(start, w, cr);
			printright(start++, hw, _("Clip Rectangle %6.2f%%"), cr);
		}
		if (color) attroff(COLOR_PAIR(5));

		if (masks->lane[COLLECTOR_LANE_CP]) {
			percentage(start, w, cp);
			printright(start++, hw, _("Command Stream %6.2f%%"), cp);
		}
		if (masks->lane[COLLECTOR_LANE_E2]) {
			percentage(start, w, e2);
			printright(start++, hw, _("2D Engine %6.2f%%"), e2);
		}
		if (masks->lane[COLLECTOR_LANE_RB2D]) {
			percentage(start, w, rb2d);
			printright(start++, hw, _("2D Backend %6.2f%%"), rb2d);
		}
		if (masks->lane[COLLECTOR_LANE_CF]) {
			percentage(start, w, cf);
			printright(start++, hw, _("Cmd Fetch %6.2f%%"), cf);
		}
		if (masks->lane[COLLECTOR_LANE_UVD]) {
			percentage(start, w, uvd);
			printright(start++, hw, _("UVD %6.2f%%"), uvd);
		}
		if (masks->lane[COLLECTOR_LANE_VCE0]) {
			percentage(start, w, vce0);
			printright(start++, hw, _("VCE %6.2f%%"), vce0);
		}

		if (snapshot.vram_valid || snapshot.gtt_valid) {
			// Enough height?
			if (h > bigh) start++;

			if (snapshot.vram_valid) {
				if (color) attron(COLOR_PAIR(2));
				percentage(start, w, vram);
				printright(start++, hw, _("%.0fM / %.0fM VRAM %6.2f%%"),
						vrammb, vramsizemb, vram);
				if (color) attroff(COLOR_PAIR(2));
			}

			if (snapshot.gtt_valid) {
				if (color) attron(COLOR_PAIR(2));
				percentage(start, w, gtt);
				printright(start++, hw, _("%.0fM / %.0fM GTT %6.2f%%"),
						gttmb, gttsizemb, gtt);
				if (color) attroff(COLOR_PAIR(2));
			}
		}

		// Each clock row is gated on its own signal.  A part whose memory
		// clock is unsupported, or whose every memory-clock read failed,
		// renders no memory row rather than a figure carried by the shader
		// clock's validity.
		if (mclk_max != 0 && snapshot.mclk.valid) {
			if (color) attron(COLOR_PAIR(3));
			percentage(start, w, mclk);
			printright(start++, hw, _("%.2fG / %.2fG Memory Clock %6.2f%%"),
					mclk_ghz, mclk_max * 1e-6f, mclk);
			if (color) attroff(COLOR_PAIR(3));
		}

		if (sclk_max != 0 && snapshot.sclk.valid) {
			if (color) attron(COLOR_PAIR(3));
			percentage(start, w, sclk);
			printright(start++, hw, _("%.2fG / %.2fG Shader Clock %6.2f%%"),
					sclk_ghz, sclk_max * 1e-6f, sclk);
			if (color) attroff(COLOR_PAIR(3));
		}

		//move the cursor away to fix some resizing artifacts on some terminals
		move(0,0);

		refresh();

		// halfdelay(10) bounds getch at one second, so the termination
		// flag and a device loss are both observed without a separate
		// timer.
		int c = getch();
		if (c == 'q' || c == 'Q') break;
		if (c == 'c' || c == 'C') color = !color;
		if (c == KEY_RESIZE) resize = 1;

		if (terminal_observed)
			break;
	}

	endwin();
	delscreen(screen);

	return status;
}
