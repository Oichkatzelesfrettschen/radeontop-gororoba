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

// The engine-status register is 32 bits wide on every family this tool reads,
// so a per-position census spans exactly that many counters.
#define COLLECTOR_STATUS_BIT_COUNT 32

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

// The schedule divides one second into `ticks` slots, so a rate finer than one
// slot per nanosecond leaves the period quotient at zero and no grid to place;
// collector_init rejects everything above COLLECTOR_TICKS_ARITHMETIC_MAX for
// that reason.  COLLECTOR_TICKS_MAX is the tighter ceiling the command line
// admits, one sample per microsecond.  A rate no run reaches is admitted rather
// than refused, because what limits the rate is a property of the host and the
// boot rather than of the part: on RS482 a read costs about 2.9 microseconds
// against a wake-up round trip near 112, so the sampler falls short at rates
// well under this ceiling and for a reason no admission check on the read path
// would name.  Each window publishes its own read cost and read share instead,
// which separates the two causes on the evidence of the run itself.
#define COLLECTOR_TICKS_ARITHMETIC_MAX 1000000000U
#define COLLECTOR_TICKS_MAX 1000000U

struct collector_config {
	uint32_t ticks;        // nominal sample slots per second
	uint32_t dumpinterval; // whole seconds of monotonic time per report window

	// A nonzero seed offsets each sample within its own slot, so a workload
	// periodic at a harmonic of the sampling rate meets a moving phase rather
	// than the same one every period.  Zero keeps the exact grid.  The offset
	// stays inside the slot it belongs to, so slot boundaries, window
	// boundaries, and the attempted-plus-missed identity hold whether or not
	// dithering is on.
	uint64_t dither_seed;
};

// The seed the command line supplies when the option stays out.  Dithering
// carries the default because the exact grid fails on the workload a desktop
// most often runs: a period near one sample slot, where the grid meets the same
// phase of every cycle and reports that phase as the whole duty.  Measured on
// RS482 against a load of 8.280 ms, about one slot at the default 120 ticks,
// the exact grid scatters 3.31 times its own binomial expectation and single
// windows range from 0.008 to 0.717 around a true duty of 0.4628, while the
// dithered grid holds 1.26.  Against a 59.87 ms load, about seven slots, the
// exact grid is the better sampler at 0.25 against 0.47, so the exact grid wins
// where it oversamples and loses where it aliases.  Dithering gives up a slot
// when scheduler lateness carries its offset past the slot end, which cost up
// to four percent of nominal slots under load and stayed inside three percent
// at idle, against an exact grid that retains every slot.  Every window reports
// that as coverage, so the trade spends a figure the output carries for a bias
// no field of it would show.
//
// splitmix64 finalizes its additive counter, so a small seed is as good as a
// large one; 7919 is the seed the validating run on RS482 used, which makes the
// shipped default the configuration that carries the measurement.
#define COLLECTOR_DITHER_SEED_DEFAULT UINT64_C(7919)

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

	// The read cost averaged over the reads that timed, accumulated online so a
	// long window cannot overflow an integral sum.  The maximum answers whether
	// a read ever overran; the mean answers what a read costs, which is the
	// figure collector_read_share turns into the window fraction the device
	// took.  A tail read moves the maximum by two orders and leaves the mean
	// where it was, so the two answer different questions and both are
	// published.  read_latency_samples is the mean's denominator and travels
	// with it.
	double mean_read_latency_ns;
	uint64_t read_latency_samples;

	struct collector_signal_stats status;
	struct collector_signal_stats uvd;
	struct collector_signal_stats vce;
	struct collector_signal_stats sclk;
	struct collector_signal_stats mclk;

	// Busy hits per lane.  The denominator is the valid read count of the
	// register the lane comes from, never the nominal slot count.
	uint64_t lane_busy[COLLECTOR_LANE_COUNT];

	// Set hits per engine-status bit position, over the same valid reads the
	// status signal counts.  A lane whose mask unions several bits reports one
	// number for all of them, so an RS482 rb2d reading of 1.0 leaves open which
	// of RB2D_BUSY and CBA2D_BUSY carried it; this census answers that from the
	// mapped collector.  It covers every position, including those no lane
	// maps, so a bit the family decode leaves unnamed still leaves a record.
	uint64_t status_bit_busy[COLLECTOR_STATUS_BIT_COUNT];

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
	COLLECTOR_WAIT_GAP = 5       // an exact consumer lost windows; outputs hold paired state
};

// abs_timeout is a CLOCK_MONOTONIC deadline, or NULL to wait indefinitely.  A
// consumer that must also observe a signal flag passes a bounded deadline and
// re-tests the flag on COLLECTOR_WAIT_TIMEOUT.  SNAPSHOT and FINISHED write
// snapshot_out, GAP writes both outputs under one lock, FATAL writes terminal_out,
// and TIMEOUT and ERROR write neither.  A GAP terminal can carry NONE when the
// worker remains active.
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

// The nominal slot width, the truncated quotient the schedule advances its grid
// by.  Zero for a rate the schedule cannot place.
uint64_t collector_slot_period_ns(const struct collector_config *config);

// The fraction of the window the device reads themselves occupied, mean read
// cost times the reads that ran over the window's own length.  A window whose
// coverage falls well below one names its cause by this figure: a large share
// puts the shortfall on the device, and a small one puts it on the wake-up path
// that placed the samples.  Coverage continues to carry the magnitude.
//
// On RS482 the mean read cost holds near 2.9 microseconds from 120 through
// 1000000 samples per second while the wake-up round trip reaches roughly 112,
// so the schedule is what binds at every rate the command line admits and the
// share stays small.  NaN when the window has no length or timed no read.
double collector_read_share(const struct collector_snapshot *snapshot,
		const struct collector_config *config);

// Monotonic nanoseconds between two timestamps, for age and lateness.
int64_t collector_timespec_delta_ns(const struct timespec *from,
		const struct timespec *to);

#endif
