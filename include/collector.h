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

// collector.h names libc and pthread only, so collector.c builds and tests
// without ncurses, libdrm, libpciaccess, or a GPU.  The register masks, the
// device reads, and the clock all arrive through the injectable structures
// in struct collector_backend and struct collector_clock.  Those interfaces
// let a synthetic backend and a virtual clock exercise the accumulator and
// schedule deterministically.

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

// now and wait_until own monotonic scheduling; realtime_now supplies the wall
// stamp paired with the monotonic publication stamp.  wait_until returns when
// the deadline passes or wake() interrupts it, and the caller re-tests its stop
// predicate either way.  Production binds wake to a CLOCK_MONOTONIC condition
// variable; a test binds every clock operation to virtual time.
struct collector_clock {
	void *context;
	int (*now)(void *context, struct timespec *ts);
	int (*realtime_now)(void *context, struct timespec *ts);
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

	// A nonzero seed offsets each sample within its own slot, so a workload
	// periodic at a harmonic of the sampling rate meets a moving phase rather
	// than the same one every period.  Zero keeps the exact grid, which is the
	// default and reproduces a run exactly.  The offset stays inside the slot
	// it belongs to, so slot boundaries, window boundaries, and the
	// attempted-plus-missed identity hold whether or not dithering is on.
	uint64_t dither_seed;
};

// Draws one unbiased value in [0, bound) from the collector's deterministic
// splitmix64 stream.  Rejection removes the modulo bias that appears whenever
// bound does not divide the 64-bit generator range exactly.
bool collector_dither_uniform_below(uint64_t *state, uint64_t bound,
		uint64_t *value);

// Each measurement class counts its own reads, because a status read and a
// clock read succeed and fail independently.
struct collector_signal_stats {
	uint64_t valid;
	uint64_t failed;
};

struct collector_lane_bounds {
	uint64_t busy;
	uint64_t valid;
	uint64_t nominal;
	double conditional_fraction;
	double unconditional_lower;
	double unconditional_upper;
};

// Terminal state belongs to the worker run rather than to a completed
// measurement window.  after_generation binds the cause to the last immutable
// generation that committed before the worker stopped.
enum collector_terminal_cause {
	COLLECTOR_TERMINAL_NONE = 0,
	COLLECTOR_TERMINAL_DEVICE_READ,
	COLLECTOR_TERMINAL_CLOCK_START,
	COLLECTOR_TERMINAL_CLOCK_WAIT,
	COLLECTOR_TERMINAL_CLOCK_SAMPLE,
	COLLECTOR_TERMINAL_CLOCK_PUBLICATION_MONOTONIC,
	COLLECTOR_TERMINAL_CLOCK_PUBLICATION_REALTIME,
	COLLECTOR_TERMINAL_SCHEDULE
};

struct collector_terminal {
	enum collector_terminal_cause cause;
	uint64_t after_generation;
	bool read_result_valid;
	enum collector_read_result read_result;
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
	// lag from published and published_realtime, a near-simultaneous pair.
	// Reading
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
};

struct collector {
	struct collector_config config;
	struct collector_backend backend;
	struct engine_masks masks;
	struct collector_clock clock;

	pthread_mutex_t mutex;
	pthread_cond_t changed;
	pthread_t thread;
	int (*join_thread)(pthread_t thread, void **result);

	struct collector_snapshot snapshot;
	struct collector_terminal terminal;

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
	COLLECTOR_WAIT_SNAPSHOT = 0, // snapshot_out holds a newer generation
	COLLECTOR_WAIT_TIMEOUT = 1,  // the deadline passed; outputs stay unmodified
	COLLECTOR_WAIT_FINISHED = 2, // the worker exited; snapshot_out holds its last state
	COLLECTOR_WAIT_FATAL = 3,    // the worker stopped; terminal_out holds why
	COLLECTOR_WAIT_ERROR = 4,    // the wait primitive failed; outputs stay unmodified
	COLLECTOR_WAIT_GAP = 5       // an exact consumer lost windows; snapshot_out holds latest
};

// abs_timeout is a CLOCK_MONOTONIC deadline, or NULL to wait indefinitely.  A
// consumer that must also observe a signal flag passes a bounded deadline and
// re-tests the flag on COLLECTOR_WAIT_TIMEOUT.  SNAPSHOT, FINISHED, and GAP
// write snapshot_out; FATAL writes terminal_out; TIMEOUT and ERROR write neither.
int collector_wait_next(struct collector *collector, uint64_t after_generation,
		const struct timespec *abs_timeout,
		struct collector_snapshot *snapshot_out,
		struct collector_terminal *terminal_out);

// Dump capture requires every generation.  This variant reports a gap instead
// of silently returning the newest replaceable snapshot.
int collector_wait_next_contiguous(struct collector *collector,
		uint64_t after_generation, const struct timespec *abs_timeout,
		struct collector_snapshot *snapshot_out,
		struct collector_terminal *terminal_out);

// Copies one current snapshot without waiting.  Returns false before the first
// generation publishes.
bool collector_peek(struct collector *collector, struct collector_snapshot *out);

// Copies the current snapshot and terminal record under one lock.  Generation
// zero and COLLECTOR_TERMINAL_NONE represent their respective empty states.
int collector_state_peek(struct collector *collector,
		struct collector_snapshot *snapshot_out,
		struct collector_terminal *terminal_out);
const char *collector_terminal_cause_name(enum collector_terminal_cause cause);
bool collector_terminal_cause_is_clock(enum collector_terminal_cause cause);

void collector_request_stop(struct collector *collector);
int collector_join(struct collector *collector);
int collector_destroy(struct collector *collector);

// A lane's duty over the reads that actually validated it.  Returns NaN when
// the window holds no valid read of that lane's source register, so a caller
// renders N/A rather than 0.00%.
double collector_lane_fraction(const struct collector_snapshot *snapshot,
		enum collector_lane lane);

// The unconditional interval assigns every missing slot idle at its lower end
// and busy at its upper end.  This remains valid when read loss correlates with
// load, while the conditional fraction describes only the successful reads.
bool collector_lane_missing_data_bounds(const struct collector_snapshot *snapshot,
		enum collector_lane lane, struct collector_lane_bounds *bounds);

// Valid status reads over nominal slots.  Read failures can correlate with
// load, so coverage travels with every status-derived figure.
double collector_status_coverage(const struct collector_snapshot *snapshot);

// Monotonic nanoseconds between two timestamps, for age and lateness.
int64_t collector_timespec_delta_ns(const struct timespec *from,
		const struct timespec *to);

#endif
