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

#ifndef COLLECTOR_H
#define COLLECTOR_H

// This header names libc and pthread only, so collector.c builds and tests
// without ncurses, libdrm, libpciaccess, or a GPU.  The register masks, the
// device reads, and the clock all arrive through the injectable structures
// below, which is what lets a synthetic backend and a virtual clock exercise
// the accumulator and the schedule deterministically.

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// Lanes derived from the main status word come first, so one loop covers them
// and the two separately-read lanes keep their own validity.
enum collector_lane {
	COLLECTOR_LANE_EE,
	COLLECTOR_LANE_VGT,
	COLLECTOR_LANE_GUI,
	COLLECTOR_LANE_TA,
	COLLECTOR_LANE_TC,
	COLLECTOR_LANE_SX,
	COLLECTOR_LANE_SH,
	COLLECTOR_LANE_SPI,
	COLLECTOR_LANE_SMX,
	COLLECTOR_LANE_SC,
	COLLECTOR_LANE_PA,
	COLLECTOR_LANE_DB,
	COLLECTOR_LANE_CB,
	COLLECTOR_LANE_CR,
	COLLECTOR_LANE_CP,
	COLLECTOR_LANE_E2,
	COLLECTOR_LANE_RB2D,
	COLLECTOR_LANE_CF,
	COLLECTOR_STATUS_LANE_COUNT,
	COLLECTOR_LANE_UVD = COLLECTOR_STATUS_LANE_COUNT,
	COLLECTOR_LANE_VCE0,
	COLLECTOR_LANE_COUNT
};

// A mask selects the bits within its source register.  A multi-bit mask reads
// as a union: the lane is busy when any of its bits is set, which is what the
// R300 rb2d pair (RB2D|CBA2D) and the R600 vgt pair need.  A zero mask leaves
// the lane unexposed.
struct engine_masks {
	uint32_t lane[COLLECTOR_LANE_COUNT];
};

// A capability distinguishes a signal the part does not have from a signal it
// has that failed to read.  An absent capability produces neither a valid nor a
// failed count, so a coverage figure stays meaningful.
enum {
	COLLECTOR_CAP_STATUS = 1u << 0,
	COLLECTOR_CAP_UVD    = 1u << 1,
	COLLECTOR_CAP_VCE    = 1u << 2,
	COLLECTOR_CAP_SCLK   = 1u << 3,
	COLLECTOR_CAP_MCLK   = 1u << 4,
	COLLECTOR_CAP_VRAM   = 1u << 5,
	COLLECTOR_CAP_GTT    = 1u << 6
};

// A backend read classifies its own failure, because an ioctl, an MMIO load,
// and a libdrm_amdgpu query report through different conventions and only the
// adapter knows which of them means the device is gone.
enum collector_read_result {
	COLLECTOR_READ_OK = 0,
	COLLECTOR_READ_TRANSIENT = 1,
	COLLECTOR_READ_FATAL = 2
};

struct collector_backend {
	void *context;
	uint32_t capabilities;

	int (*read_status)(void *context, uint32_t *value);
	int (*read_uvd_status)(void *context, uint32_t *value);
	int (*read_vce_status)(void *context, uint32_t *value);
	int (*read_sclk)(void *context, uint32_t *value);
	int (*read_mclk)(void *context, uint32_t *value);
	int (*read_vram)(void *context, uint64_t *value);
	int (*read_gtt)(void *context, uint64_t *value);
};

// wait_until returns when the deadline passes or when wake() interrupts it, and
// the caller re-tests its stop predicate either way.  Production binds this to
// a CLOCK_MONOTONIC condition variable; a test binds it to a virtual clock that
// advances without waiting.
struct collector_clock {
	void *context;
	int (*now)(void *context, struct timespec *ts);
	int (*wait_until)(void *context, const struct timespec *deadline);
	void (*wake)(void *context);
};

// The monotonic clock a production collector uses.  It owns the condition
// variable that collector_request_stop interrupts, so a collector waiting on a
// deadline a full dumpinterval away still stops promptly.
struct collector_monotonic_clock {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	bool woken;
	bool ready;
};

int collector_monotonic_clock_init(struct collector_monotonic_clock *clock);
void collector_monotonic_clock_destroy(struct collector_monotonic_clock *clock);
struct collector_clock collector_monotonic_clock_ops(struct collector_monotonic_clock *clock);

struct collector_config {
	uint32_t ticks;        // nominal sample slots per second
	uint32_t dumpinterval; // whole seconds of monotonic time per report window
};

// Each measurement class counts its own reads, because a status read and a
// clock read succeed and fail independently.
struct collector_signal_stats {
	uint64_t valid;
	uint64_t failed;
};

// One published generation is one completed measurement window.  Every field is
// a whole value; a reader copies the structure under the mutex and then uses
// only its private copy.
struct collector_snapshot {
	uint64_t generation;

	struct timespec window_start;           // monotonic, scheduled
	struct timespec window_end;             // monotonic, scheduled
	struct timespec published;              // monotonic, actual
	struct timespec published_realtime;     // wall clock, read at publication
	// Wall clock at the scheduled end, derived by subtracting the publication
	// lag from the near-simultaneous monotonic/realtime pair above.  Reading
	// CLOCK_REALTIME at publication and calling it the scheduled end dates the
	// window by however long the endpoint reads took.  The monotonic fields
	// remain authoritative for cadence, because realtime steps.
	struct timespec scheduled_end_realtime;

	uint64_t nominal_slots;
	uint64_t attempted_slots;
	uint64_t missed_slots;
	uint64_t late_wakeups;
	uint64_t max_lateness_ns;
	uint64_t max_read_latency_ns;

	struct collector_signal_stats status;
	struct collector_signal_stats uvd;
	struct collector_signal_stats vce;
	struct collector_signal_stats sclk;
	struct collector_signal_stats mclk;

	// Busy hits per lane.  The denominator is the valid read count of the
	// register the lane comes from, never the nominal slot count.
	uint64_t lane_busy[COLLECTOR_LANE_COUNT];

	// Means over their own valid readings, accumulated online so a long
	// window cannot overflow an integral sum.
	double sclk_mean_khz;
	double mclk_mean_khz;

	// Endpoint point measurements, read once at publication time.
	uint64_t vram;
	uint64_t gtt;
	bool vram_valid;
	bool gtt_valid;

	uint32_t capabilities;

	bool fatal;
	int fatal_read_result;
};

struct collector {
	struct collector_config config;
	struct collector_backend backend;
	struct engine_masks masks;
	struct collector_clock clock;

	pthread_mutex_t mutex;
	pthread_cond_t changed;
	pthread_t thread;

	struct collector_snapshot snapshot;

	bool stop_requested;
	bool thread_started;
	bool finished;
	bool initialized;
};

// Return values: 0 succeeds, and a negative value reports the failing step.
int collector_init(struct collector *collector,
		const struct collector_config *config,
		const struct collector_backend *backend,
		const struct engine_masks *masks,
		const struct collector_clock *clock);

int collector_start(struct collector *collector);

enum collector_wait_result {
	COLLECTOR_WAIT_SNAPSHOT = 0, // out holds a generation newer than requested
	COLLECTOR_WAIT_TIMEOUT = 1,  // the deadline passed with no new generation
	COLLECTOR_WAIT_FINISHED = 2, // the worker exited and publishes no more
	COLLECTOR_WAIT_FATAL = 3     // the worker lost the backend; out holds why
};

// abs_timeout is a CLOCK_MONOTONIC deadline, or NULL to wait indefinitely.  A
// consumer that must also observe a signal flag passes a bounded deadline and
// re-tests the flag on COLLECTOR_WAIT_TIMEOUT.
int collector_wait_next(struct collector *collector, uint64_t after_generation,
		const struct timespec *abs_timeout,
		struct collector_snapshot *out);

// Copies the current snapshot without waiting.  Returns false before the first
// generation publishes.
bool collector_peek(struct collector *collector, struct collector_snapshot *out);

void collector_request_stop(struct collector *collector);
int collector_join(struct collector *collector);
void collector_destroy(struct collector *collector);

// A lane's duty over the reads that actually validated it.  Returns NaN when
// the window holds no valid read of that lane's source register, so a caller
// renders N/A rather than 0.00%.
double collector_lane_fraction(const struct collector_snapshot *snapshot,
		enum collector_lane lane);

// Valid status reads over nominal slots.  Read failures can correlate with
// load, so this travels with every status-derived figure.
double collector_status_coverage(const struct collector_snapshot *snapshot);

// Monotonic nanoseconds between two timestamps, for age and lateness.
int64_t collector_timespec_delta_ns(const struct timespec *from,
		const struct timespec *to);

#endif
