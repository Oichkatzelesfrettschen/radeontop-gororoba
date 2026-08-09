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

// Exercises the collector against a scripted backend and a virtual clock, so
// the arithmetic, the schedule, the failure semantics, the publication order,
// and the shutdown path are all observable without a GPU and without waiting on
// real time.  Only the long-dumpinterval shutdown case binds the real
// monotonic clock, because interrupting a far-future deadline is exactly what
// that case proves.

#include "collector.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int checks_run;
static unsigned int checks_failed;
static const char *current_case = "";

#define CHECK(condition) check_that((condition), #condition, __LINE__)

static void check_that(int condition, const char *text, int line) {
	checks_run++;
	if (condition)
		return;

	checks_failed++;
	fprintf(stderr, "FAIL %s:%d  %s\n", current_case, line, text);
}

static void check_equal_u64(uint64_t got, uint64_t want, const char *what, int line) {
	checks_run++;
	if (got == want)
		return;

	checks_failed++;
	fprintf(stderr, "FAIL %s:%d  %s: got %llu, want %llu\n", current_case, line,
		what, (unsigned long long) got, (unsigned long long) want);
}

#define CHECK_U64(got, want) check_equal_u64((got), (want), #got, __LINE__)

static void check_close(double got, double want, const char *what, int line) {
	checks_run++;
	if (fabs(got - want) < 1e-9)
		return;

	checks_failed++;
	fprintf(stderr, "FAIL %s:%d  %s: got %.12f, want %.12f\n", current_case, line,
		what, got, want);
}

#define CHECK_CLOSE(got, want) check_close((got), (want), #got, __LINE__)

#define NS_PER_SEC 1000000000LL

// The scripted backend.

// A read may consume virtual time, which is what reproduces a device slower
// than the slot period.  Scripting the delay inside wait_until instead would
// only ever move the clock before a read, leaving the post-read schedule
// untested.
struct fake_clock;
static void fake_clock_consume(struct fake_clock *clock, int64_t ns);
static int fake_now(void *context, struct timespec *ts);

// Dithered test windows run at eight slots per second.  A read past the
// capacity is dropped rather than counted, so an overrun
// shows up as a short sequence instead of a corrupted one.
#define SAMPLE_TIME_CAP 32

struct fake_backend {
	pthread_mutex_t mutex;

	// Set to the harness clock when a read must take measurable time.
	struct fake_clock *clock;
	int64_t read_delay_ns;

	// Set after join.  Every reader asserts against it, so a read that
	// outlived the join is a failure rather than a silent use-after-free.
	bool torn_down;
	bool read_after_teardown;

	// Records the virtual time of each status read, which is where a sample
	// actually landed inside its slot.
	bool record_times;
	struct timespec sample_times[SAMPLE_TIME_CAP];
	size_t sample_time_count;

	uint64_t status_calls;
	uint64_t uvd_calls;
	uint64_t vce_calls;
	uint64_t sclk_calls;
	uint64_t mclk_calls;
	uint64_t vram_calls;
	uint64_t gtt_calls;

	const uint32_t *status_values;
	size_t status_values_len;
	const int *status_results;
	size_t status_results_len;

	const uint32_t *sclk_values;
	size_t sclk_values_len;
	const int *sclk_results;
	size_t sclk_results_len;

	uint32_t uvd_value;
	uint32_t vce_value;
	uint32_t mclk_value;
	uint64_t vram_value;
	uint64_t gtt_value;

	int uvd_result;
	int vce_result;
	int mclk_result;
	int vram_result;
	int gtt_result;
};

// A sequence shorter than the window repeats its last entry, so a test scripts
// only the prefix it cares about.
static uint32_t pick_u32(const uint32_t *sequence, size_t length, uint64_t index,
		uint32_t fallback) {
	if (!sequence || !length)
		return fallback;

	return sequence[index < length ? index : length - 1];
}

static int pick_result(const int *sequence, size_t length, uint64_t index) {
	if (!sequence || !length)
		return COLLECTOR_READ_OK;

	return sequence[index < length ? index : length - 1];
}

static void note_call(struct fake_backend *fake) {
	if (fake->torn_down)
		fake->read_after_teardown = true;

	// note_call takes the clock mutex under the backend mutex.  No path acquires
	// the same pair in reverse order, so the nesting introduces no cycle.
	if (fake->clock && fake->record_times &&
			fake->sample_time_count < SAMPLE_TIME_CAP)
		fake_now(fake->clock, &fake->sample_times[fake->sample_time_count++]);

	if (fake->clock && fake->read_delay_ns)
		fake_clock_consume(fake->clock, fake->read_delay_ns);
}

static int fake_read_status(void *context, uint32_t *value) {
	struct fake_backend *fake = context;
	int result;

	pthread_mutex_lock(&fake->mutex);
	note_call(fake);
	*value = pick_u32(fake->status_values, fake->status_values_len,
		fake->status_calls, 0);
	result = pick_result(fake->status_results, fake->status_results_len,
		fake->status_calls);
	fake->status_calls++;
	pthread_mutex_unlock(&fake->mutex);

	return result;
}

static int fake_read_uvd(void *context, uint32_t *value) {
	struct fake_backend *fake = context;
	int result;

	pthread_mutex_lock(&fake->mutex);
	note_call(fake);
	*value = fake->uvd_value;
	result = fake->uvd_result;
	fake->uvd_calls++;
	pthread_mutex_unlock(&fake->mutex);

	return result;
}

static int fake_read_vce(void *context, uint32_t *value) {
	struct fake_backend *fake = context;
	int result;

	pthread_mutex_lock(&fake->mutex);
	note_call(fake);
	*value = fake->vce_value;
	result = fake->vce_result;
	fake->vce_calls++;
	pthread_mutex_unlock(&fake->mutex);

	return result;
}

static int fake_read_sclk(void *context, uint32_t *value) {
	struct fake_backend *fake = context;
	int result;

	pthread_mutex_lock(&fake->mutex);
	note_call(fake);
	*value = pick_u32(fake->sclk_values, fake->sclk_values_len,
		fake->sclk_calls, 0);
	result = pick_result(fake->sclk_results, fake->sclk_results_len,
		fake->sclk_calls);
	fake->sclk_calls++;
	pthread_mutex_unlock(&fake->mutex);

	return result;
}

static int fake_read_mclk(void *context, uint32_t *value) {
	struct fake_backend *fake = context;
	int result;

	pthread_mutex_lock(&fake->mutex);
	note_call(fake);
	*value = fake->mclk_value;
	result = fake->mclk_result;
	fake->mclk_calls++;
	pthread_mutex_unlock(&fake->mutex);

	return result;
}

static int fake_read_vram(void *context, uint64_t *value) {
	struct fake_backend *fake = context;
	int result;

	pthread_mutex_lock(&fake->mutex);
	note_call(fake);
	*value = fake->vram_value;
	result = fake->vram_result;
	fake->vram_calls++;
	pthread_mutex_unlock(&fake->mutex);

	return result;
}

static int fake_read_gtt(void *context, uint64_t *value) {
	struct fake_backend *fake = context;
	int result;

	pthread_mutex_lock(&fake->mutex);
	note_call(fake);
	*value = fake->gtt_value;
	result = fake->gtt_result;
	fake->gtt_calls++;
	pthread_mutex_unlock(&fake->mutex);

	return result;
}

static void fake_backend_init(struct fake_backend *fake) {
	memset(fake, 0, sizeof(*fake));
	pthread_mutex_init(&fake->mutex, NULL);
}

static void fake_backend_destroy(struct fake_backend *fake) {
	pthread_mutex_destroy(&fake->mutex);
}

static struct collector_backend fake_backend_ops(struct fake_backend *fake,
		uint32_t capabilities) {
	struct collector_backend backend;

	memset(&backend, 0, sizeof(backend));
	backend.context = fake;
	backend.capabilities = capabilities;
	backend.read_status = fake_read_status;
	backend.read_uvd_status = fake_read_uvd;
	backend.read_vce_status = fake_read_vce;
	backend.read_sclk = fake_read_sclk;
	backend.read_mclk = fake_read_mclk;
	backend.read_vram = fake_read_vram;
	backend.read_gtt = fake_read_gtt;

	return backend;
}

// The virtual clock.  wait_until jumps straight to the deadline, so a window of
// any length completes without waiting.  It also single-steps: the worker may
// perform one wait per granted credit and parks otherwise, so a test observes an
// exact number of samples rather than racing a worker that would otherwise spin
// through thousands of windows before the first assertion ran.

struct fake_clock {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	struct timespec now;
	bool wake_pending;
	bool parked;
	uint64_t budget;

	const int64_t *extra_delay_ns;
	size_t extra_delay_len;
	uint64_t waits;
	uint64_t wait_calls;
	uint64_t now_calls;
	uint64_t fail_now_call;
	uint64_t realtime_calls;
	uint64_t fail_realtime_call;
	uint64_t fail_wait_call;
};

static void fake_clock_init(struct fake_clock *clock) {
	memset(clock, 0, sizeof(*clock));
	pthread_mutex_init(&clock->mutex, NULL);
	pthread_cond_init(&clock->cond, NULL);
}

static void fake_clock_destroy(struct fake_clock *clock) {
	pthread_cond_destroy(&clock->cond);
	pthread_mutex_destroy(&clock->mutex);
}

static void advance(struct timespec *ts, int64_t ns) {
	ts->tv_sec += (time_t) (ns / NS_PER_SEC);
	ts->tv_nsec += (long) (ns % NS_PER_SEC);
	if (ts->tv_nsec >= NS_PER_SEC) {
		ts->tv_nsec -= NS_PER_SEC;
		ts->tv_sec++;
	}
}

// Moves virtual time forward from inside a backend read, so the worker observes
// a post-read timestamp later than the one it took before the read.
static void fake_clock_consume(struct fake_clock *clock, int64_t ns) {
	pthread_mutex_lock(&clock->mutex);
	advance(&clock->now, ns);
	pthread_mutex_unlock(&clock->mutex);
}

static int fake_now(void *context, struct timespec *ts) {
	struct fake_clock *clock = context;
	int result = 0;

	pthread_mutex_lock(&clock->mutex);
	clock->now_calls++;
	if (clock->fail_now_call == clock->now_calls) {
		result = -1;
		clock->parked = true;
		pthread_cond_broadcast(&clock->cond);
	} else {
		*ts = clock->now;
	}
	pthread_mutex_unlock(&clock->mutex);

	return result;
}

static int fake_realtime_now(void *context, struct timespec *ts) {
	struct fake_clock *clock = context;
	int result = 0;

	pthread_mutex_lock(&clock->mutex);
	clock->realtime_calls++;
	if (clock->fail_realtime_call == clock->realtime_calls) {
		result = -1;
		clock->parked = true;
		pthread_cond_broadcast(&clock->cond);
	} else {
		*ts = clock->now;
		ts->tv_sec += 1700000000;
	}
	pthread_mutex_unlock(&clock->mutex);

	return result;
}

static int fake_wait_until(void *context, const struct timespec *deadline) {
	struct fake_clock *clock = context;

	pthread_mutex_lock(&clock->mutex);

	clock->wait_calls++;
	clock->parked = true;
	pthread_cond_broadcast(&clock->cond);
	if (clock->fail_wait_call == clock->wait_calls) {
		pthread_mutex_unlock(&clock->mutex);
		return -1;
	}

	while (!clock->budget && !clock->wake_pending)
		pthread_cond_wait(&clock->cond, &clock->mutex);

	clock->parked = false;

	if (clock->wake_pending) {
		clock->wake_pending = false;
		pthread_cond_broadcast(&clock->cond);
		pthread_mutex_unlock(&clock->mutex);
		return 0;
	}

	clock->budget--;

	if (collector_timespec_delta_ns(&clock->now, deadline) > 0)
		clock->now = *deadline;

	// A scripted extra delay reproduces a wake-up that arrives after several
	// slot deadlines have already passed.
	if (clock->extra_delay_ns && clock->waits < clock->extra_delay_len)
		advance(&clock->now, clock->extra_delay_ns[clock->waits]);

	clock->waits++;
	pthread_cond_broadcast(&clock->cond);
	pthread_mutex_unlock(&clock->mutex);

	return 0;
}

static void fake_wake(void *context) {
	struct fake_clock *clock = context;

	pthread_mutex_lock(&clock->mutex);
	clock->wake_pending = true;
	pthread_cond_broadcast(&clock->cond);
	pthread_mutex_unlock(&clock->mutex);
}

// Grants one wait and returns once the worker has consumed it and parked again,
// so the sample and any publication it triggered are complete.  Returns false
// when the worker stopped parking, which is what a fatal exit looks like.
static bool fake_clock_step(struct fake_clock *clock) {
	struct timespec limit;
	bool stepped = true;

	clock_gettime(CLOCK_REALTIME, &limit);
	limit.tv_sec += 5;

	pthread_mutex_lock(&clock->mutex);
	clock->budget++;
	pthread_cond_broadcast(&clock->cond);

	while (clock->budget || !clock->parked) {
		if (pthread_cond_timedwait(&clock->cond, &clock->mutex, &limit) == ETIMEDOUT) {
			stepped = false;
			break;
		}
	}

	pthread_mutex_unlock(&clock->mutex);
	return stepped;
}

static struct collector_clock fake_clock_ops(struct fake_clock *clock) {
	struct collector_clock ops;

	ops.context = clock;
	ops.now = fake_now;
	ops.realtime_now = fake_realtime_now;
	ops.wait_until = fake_wait_until;
	ops.wake = fake_wake;

	return ops;
}

// Harness.

struct harness {
	struct fake_backend backend;
	struct fake_clock clock;
	struct collector collector;
	struct engine_masks masks;
};

static void harness_start_seeded(struct harness *harness, uint32_t ticks,
		uint32_t dumpinterval, uint32_t capabilities, uint64_t dither_seed) {
	const struct collector_config config = { ticks, dumpinterval, dither_seed };
	const struct collector_backend backend =
		fake_backend_ops(&harness->backend, capabilities);
	const struct collector_clock clock = fake_clock_ops(&harness->clock);

	CHECK(collector_init(&harness->collector, &config, &backend,
		&harness->masks, &clock) == 0);
	CHECK(collector_start(&harness->collector) == 0);
}

// Seed zero, so every case that does not name a seed runs on the exact grid.
static void harness_start(struct harness *harness, uint32_t ticks,
		uint32_t dumpinterval, uint32_t capabilities) {
	harness_start_seeded(harness, ticks, dumpinterval, capabilities, 0);
}

// Stops and joins.  Marking the backend torn down after the join makes a late
// read observable: production unmaps the BAR at the same lifecycle boundary.
static void harness_join(struct harness *harness) {
	collector_request_stop(&harness->collector);
	CHECK(collector_join(&harness->collector) == 0);

	pthread_mutex_lock(&harness->backend.mutex);
	harness->backend.torn_down = true;
	pthread_mutex_unlock(&harness->backend.mutex);
}

// The collector's own mutex stays valid until destroy, so a consumer may still
// query it between the join and the teardown.
static void harness_stop(struct harness *harness) {
	harness_join(harness);
	collector_destroy(&harness->collector);
}

static void harness_init(struct harness *harness) {
	memset(harness, 0, sizeof(*harness));
	fake_backend_init(&harness->backend);
	fake_clock_init(&harness->clock);
}

static void harness_destroy(struct harness *harness) {
	CHECK(harness->backend.read_after_teardown == false);
	fake_backend_destroy(&harness->backend);
	fake_clock_destroy(&harness->clock);
}

// Steps the virtual clock until the next snapshot or terminal event.  The step
// count is bounded so a defect stops the run instead of hanging it.
static int next_event(struct harness *harness, uint64_t after,
		struct collector_snapshot *snapshot,
		struct collector_terminal *terminal) {
	const uint64_t limit = 16 * ((uint64_t) harness->collector.config.ticks *
		harness->collector.config.dumpinterval + 8);
	const struct timespec expired = { 0, 0 };

	memset(snapshot, 0, sizeof(*snapshot));
	memset(terminal, 0, sizeof(*terminal));

	for (uint64_t step = 0; step < limit; step++) {
		int event = collector_wait_next(&harness->collector, after, &expired,
			snapshot, terminal);

		if (event != COLLECTOR_WAIT_TIMEOUT)
			return event;

		const bool stepped = fake_clock_step(&harness->clock);

		event = collector_wait_next(&harness->collector, after, &expired,
			snapshot, terminal);
		if (event != COLLECTOR_WAIT_TIMEOUT)
			return event;

		if (!stepped)
			return COLLECTOR_WAIT_FINISHED;
	}

	return COLLECTOR_WAIT_TIMEOUT;
}

static int next_snapshot(struct harness *harness, uint64_t after,
		struct collector_snapshot *out) {
	struct collector_terminal terminal;

	return next_event(harness, after, out, &terminal);
}

static int next_terminal(struct harness *harness, uint64_t after,
		struct collector_terminal *out) {
	struct collector_snapshot snapshot;

	return next_event(harness, after, &snapshot, out);
}

// Cases.

#define GUI_BIT (1u << 31)
#define CP_BIT  (1u << 16)
#define RB2D_A  (1u << 18)
#define RB2D_B  (1u << 27)

static void case_alternating_status(void) {
	static const uint32_t values[] = { GUI_BIT, 0, GUI_BIT, 0, GUI_BIT,
		0, GUI_BIT, 0, GUI_BIT, 0 };
	struct harness harness;
	struct collector_snapshot snapshot;

	current_case = "alternating_status";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;
	harness.backend.status_values = values;
	harness.backend.status_values_len = sizeof(values) / sizeof(values[0]);

	harness_start(&harness, 10, 1, COLLECTOR_CAP_STATUS);
	CHECK(next_snapshot(&harness, 0, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);

	CHECK_U64(snapshot.nominal_slots, 10);
	CHECK_U64(snapshot.status.valid, 10);
	CHECK_U64(snapshot.status.failed, 0);
	CHECK_U64(snapshot.lane_busy[COLLECTOR_LANE_GUI], 5);
	CHECK_CLOSE(collector_lane_fraction(&snapshot, COLLECTOR_LANE_GUI), 0.5);
	CHECK_CLOSE(collector_status_coverage(&snapshot), 1.0);

	harness_stop(&harness);
	harness_destroy(&harness);
}

// The virtual clock starts at zero, so slot i of the first window spans
// [i * period, (i + 1) * period) in absolute virtual nanoseconds.
static int64_t sample_offset_in_slot(const struct timespec *sample, size_t slot,
		int64_t period) {
	const int64_t at = (int64_t) sample->tv_sec * NS_PER_SEC + sample->tv_nsec;

	return at - (int64_t) slot * period;
}

// A bound that consumes just over half the uint64_t range makes the rejected
// prefix large enough for a deterministic control.  Seed 3 rejects the first
// splitmix64 word and accepts the second; a bare modulo returns a different
// value and leaves the state with one fewer draw.
static void case_dither_rejects_modulo_bias(void) {
	uint64_t state = UINT64_C(3);
	uint64_t value = 0;

	current_case = "dither_rejects_modulo_bias";
	CHECK(collector_dither_uniform_below(&state,
		UINT64_C(9223372036854775809), &value));
	CHECK_U64(state, UINT64_C(4354685564936845357));
	CHECK_U64(value, UINT64_C(3694763184872335752));
	CHECK(!collector_dither_uniform_below(NULL, 3, &value));
	CHECK(!collector_dither_uniform_below(&state, 0, &value));
	CHECK(!collector_dither_uniform_below(&state, 3, NULL));
}

// A three-slot second contains two 333333333 ns slots and one 333333334 ns
// slot.  Every seeded sample remains inside its exact rational-grid slot,
// including the carried final nanosecond before the one-second boundary.
static void case_dither_uses_exact_fractional_slot_width(void) {
	static const uint32_t values[] = { GUI_BIT };
	struct harness harness;
	struct collector_snapshot snapshot;

	current_case = "dither_uses_exact_fractional_slot_width";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;
	harness.backend.status_values = values;
	harness.backend.status_values_len = 1;
	harness.backend.clock = &harness.clock;
	harness.backend.record_times = true;

	harness_start_seeded(&harness, 3, 1, COLLECTOR_CAP_STATUS, 0x5eed);
	CHECK(next_snapshot(&harness, 0, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);
	CHECK_U64(snapshot.attempted_slots, 3);
	CHECK(harness.backend.sample_time_count >= 3);

	for (size_t slot = 0; slot < 3; slot++) {
		const int64_t sample_ns =
			(int64_t) harness.backend.sample_times[slot].tv_sec * NS_PER_SEC +
			harness.backend.sample_times[slot].tv_nsec;
		const int64_t slot_start = (int64_t) slot * NS_PER_SEC / 3;
		const int64_t slot_end = (int64_t) (slot + 1) * NS_PER_SEC / 3;

		CHECK(sample_ns >= slot_start);
		CHECK(sample_ns < slot_end);
	}

	harness_stop(&harness);
	harness_destroy(&harness);
}

// Every dithered sample stays inside the slot it belongs to, and the offsets
// vary rather than settling on one phase.  Membership follows the grid, so a
// dithered window still carries the slot count an exact one carries.
static void case_dither_keeps_samples_in_their_slots(void) {
	static const uint32_t values[] = { GUI_BIT };
	const int64_t period = NS_PER_SEC / 8;
	struct harness harness;
	struct collector_snapshot snapshot;
	size_t distinct = 0;

	current_case = "dither_keeps_samples_in_their_slots";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;
	harness.backend.status_values = values;
	harness.backend.status_values_len = 1;
	harness.backend.clock = &harness.clock;
	harness.backend.record_times = true;

	harness_start_seeded(&harness, 8, 1, COLLECTOR_CAP_STATUS, 0x5eed);
	CHECK(next_snapshot(&harness, 0, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);

	CHECK_U64(snapshot.nominal_slots, 8);
	CHECK_U64(snapshot.attempted_slots, 8);
	CHECK_U64(snapshot.missed_slots, 0);
	CHECK(harness.backend.sample_time_count >= 8);

	for (size_t slot = 0; slot < 8; slot++) {
		const int64_t offset = sample_offset_in_slot(
			&harness.backend.sample_times[slot], slot, period);

		CHECK(offset >= 0);
		CHECK(offset < period);

		if (offset != 0)
			distinct++;
	}

	// Bounds alone admit a constant generator.  Distinct offsets assert the
	// phase movement that dithering provides.
	CHECK(distinct >= 6);

	// The window boundary is a grid fact, so the dither leaves it alone.
	CHECK_U64((uint64_t) snapshot.window_start.tv_sec, 0);
	CHECK_U64((uint64_t) snapshot.window_start.tv_nsec, 0);
	CHECK_U64((uint64_t) snapshot.window_end.tv_sec, 1);
	CHECK_U64((uint64_t) snapshot.window_end.tv_nsec, 0);

	harness_stop(&harness);
	harness_destroy(&harness);
}

// Seed zero is the default, and it places every sample exactly on its grid
// point, so a run that names no seed reproduces another exactly.
static void case_unseeded_schedule_stays_exact(void) {
	static const uint32_t values[] = { GUI_BIT };
	const int64_t period = NS_PER_SEC / 8;
	struct harness harness;
	struct collector_snapshot snapshot;

	current_case = "unseeded_schedule_stays_exact";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;
	harness.backend.status_values = values;
	harness.backend.status_values_len = 1;
	harness.backend.clock = &harness.clock;
	harness.backend.record_times = true;

	harness_start_seeded(&harness, 8, 1, COLLECTOR_CAP_STATUS, 0);
	CHECK(next_snapshot(&harness, 0, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);

	CHECK_U64(snapshot.attempted_slots, 8);
	CHECK_U64(snapshot.missed_slots, 0);
	CHECK(harness.backend.sample_time_count >= 8);

	for (size_t slot = 0; slot < 8; slot++)
		CHECK(sample_offset_in_slot(&harness.backend.sample_times[slot],
			slot, period) == 0);

	harness_stop(&harness);
	harness_destroy(&harness);
}

// A seed reproduces its own run: two collectors given the same seed place their
// samples at the same offsets, which is what lets one capture be compared with
// another.
static void case_dither_seed_reproduces_offsets(void) {
	static const uint32_t values[] = { GUI_BIT };
	const int64_t period = NS_PER_SEC / 8;
	struct timespec first[8];
	struct harness harness;
	struct collector_snapshot snapshot;

	current_case = "dither_seed_reproduces_offsets";

	for (int run = 0; run < 2; run++) {
		harness_init(&harness);
		harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;
		harness.backend.status_values = values;
		harness.backend.status_values_len = 1;
		harness.backend.clock = &harness.clock;
		harness.backend.record_times = true;

		harness_start_seeded(&harness, 8, 1, COLLECTOR_CAP_STATUS, 0xa5a5);
		CHECK(next_snapshot(&harness, 0, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);
		CHECK(harness.backend.sample_time_count >= 8);

		for (size_t slot = 0; slot < 8; slot++) {
			const int64_t offset = sample_offset_in_slot(
				&harness.backend.sample_times[slot], slot, period);

			if (run == 0)
				first[slot] = harness.backend.sample_times[slot];
			else
				CHECK(offset == sample_offset_in_slot(&first[slot],
					slot, period));
		}

		harness_stop(&harness);
		harness_destroy(&harness);
	}
}

// A slot is given up when its own sample point is already behind, not when its
// grid point is.  With a read costing half a period and an offset drawn
// uniformly from the whole period, the grid-point rule gives up a slot whenever
// the previous offset exceeded half a period -- around half of them -- while the
// sample-point rule needs the offset to fall by half a period between
// consecutive slots, which is rarer by the same factor.  A quarter of the slots
// separates the two.
static void case_dither_skips_only_passed_sample_points(void) {
	static const uint32_t values[] = { GUI_BIT };
	struct harness harness;
	struct collector_snapshot snapshot;

	uint64_t missed = 0, nominal = 0;

	current_case = "dither_skips_only_passed_sample_points";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;
	harness.backend.status_values = values;
	harness.backend.status_values_len = 1;
	harness.backend.clock = &harness.clock;
	harness.backend.read_delay_ns = NS_PER_SEC / 16;

	harness_start_seeded(&harness, 8, 1, COLLECTOR_CAP_STATUS, 0x5eed);

	// Several windows, because whether a given slot is given up depends on two
	// consecutive offsets and one window is too short to separate the rules.
	for (uint64_t generation = 0; generation < 6; generation++) {
		CHECK(next_snapshot(&harness, generation, &snapshot) ==
			COLLECTOR_WAIT_SNAPSHOT);
		CHECK_U64(snapshot.attempted_slots + snapshot.missed_slots, 8);
		missed += snapshot.missed_slots;
		nominal += snapshot.nominal_slots;
	}

	CHECK(missed * 4 <= nominal);

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_failed_status_between_valid(void) {
	static const uint32_t values[] = { GUI_BIT };
	static const int results[] = { COLLECTOR_READ_OK, COLLECTOR_READ_OK,
		COLLECTOR_READ_OK, COLLECTOR_READ_OK, COLLECTOR_READ_OK,
		COLLECTOR_READ_TRANSIENT, COLLECTOR_READ_OK, COLLECTOR_READ_OK,
		COLLECTOR_READ_OK, COLLECTOR_READ_OK };
	struct harness harness;
	struct collector_snapshot snapshot;

	current_case = "failed_status_between_valid";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;
	harness.backend.status_values = values;
	harness.backend.status_values_len = 1;
	harness.backend.status_results = results;
	harness.backend.status_results_len = sizeof(results) / sizeof(results[0]);

	harness_start(&harness, 10, 1, COLLECTOR_CAP_STATUS);
	CHECK(next_snapshot(&harness, 0, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);

	// The failed read validates no lane, so it enters neither the numerator
	// nor the denominator, and the duty stays 100 percent rather than
	// dropping to 9/10 as a nominal-slot denominator would give.
	CHECK_U64(snapshot.status.valid, 9);
	CHECK_U64(snapshot.status.failed, 1);
	CHECK_U64(snapshot.lane_busy[COLLECTOR_LANE_GUI], 9);
	CHECK_CLOSE(collector_lane_fraction(&snapshot, COLLECTOR_LANE_GUI), 1.0);
	CHECK_CLOSE(collector_status_coverage(&snapshot), 0.9);

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_rb2d_union(void) {
	// Only the second bit of the pair is set; the union mask must still
	// count the lane busy.
	static const uint32_t values[] = { RB2D_B };
	struct harness harness;
	struct collector_snapshot snapshot;

	current_case = "rb2d_union";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_RB2D] = RB2D_A | RB2D_B;
	harness.backend.status_values = values;
	harness.backend.status_values_len = 1;

	harness_start(&harness, 4, 1, COLLECTOR_CAP_STATUS);
	CHECK(next_snapshot(&harness, 0, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);

	CHECK_U64(snapshot.lane_busy[COLLECTOR_LANE_RB2D], 4);
	CHECK_CLOSE(collector_lane_fraction(&snapshot, COLLECTOR_LANE_RB2D), 1.0);

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_absent_uvd(void) {
	struct harness harness;
	struct collector_snapshot snapshot;

	current_case = "absent_uvd";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;

	harness_start(&harness, 4, 1, COLLECTOR_CAP_STATUS);
	CHECK(next_snapshot(&harness, 0, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);

	// An unsupported signal produces neither a successful nor a failed read,
	// which is what separates it from one that is present and broken.
	CHECK_U64(snapshot.uvd.valid, 0);
	CHECK_U64(snapshot.uvd.failed, 0);
	CHECK(isnan(collector_lane_fraction(&snapshot, COLLECTOR_LANE_UVD)));
	CHECK((snapshot.capabilities & COLLECTOR_CAP_UVD) == 0);

	pthread_mutex_lock(&harness.backend.mutex);
	CHECK_U64(harness.backend.uvd_calls, 0);
	pthread_mutex_unlock(&harness.backend.mutex);

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_failed_uvd(void) {
	struct harness harness;
	struct collector_snapshot snapshot;

	current_case = "failed_uvd";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;
	harness.masks.lane[COLLECTOR_LANE_UVD] = (1u << 19);
	harness.backend.uvd_result = COLLECTOR_READ_TRANSIENT;

	harness_start(&harness, 4, 1, COLLECTOR_CAP_STATUS | COLLECTOR_CAP_UVD);
	CHECK(next_snapshot(&harness, 0, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);

	// UVD failing does not touch the status lanes, because the two come from
	// separate reads with separate validity.
	CHECK_U64(snapshot.uvd.valid, 0);
	CHECK_U64(snapshot.uvd.failed, 4);
	CHECK_U64(snapshot.status.valid, 4);
	CHECK(isnan(collector_lane_fraction(&snapshot, COLLECTOR_LANE_UVD)));
	CHECK(!isnan(collector_lane_fraction(&snapshot, COLLECTOR_LANE_GUI)));

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_failed_clock_no_stale_reuse(void) {
	static const uint32_t values[] = { 100, 200, 999, 999 };
	static const int results[] = { COLLECTOR_READ_OK, COLLECTOR_READ_OK,
		COLLECTOR_READ_TRANSIENT, COLLECTOR_READ_TRANSIENT };
	struct harness harness;
	struct collector_snapshot snapshot;

	current_case = "failed_clock_no_stale_reuse";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;
	harness.backend.sclk_values = values;
	harness.backend.sclk_values_len = 4;
	harness.backend.sclk_results = results;
	harness.backend.sclk_results_len = 4;

	harness_start(&harness, 4, 1, COLLECTOR_CAP_STATUS | COLLECTOR_CAP_SCLK);
	CHECK(next_snapshot(&harness, 0, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);

	// The mean is over the two readings that succeeded.  Carrying the last
	// good value forward would give 350, and dividing by four would give 75.
	CHECK_U64(snapshot.sclk.valid, 2);
	CHECK_U64(snapshot.sclk.failed, 2);
	CHECK_CLOSE(snapshot.sclk_mean_khz, 150.0);

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_zero_valid_status(void) {
	static const int results[] = { COLLECTOR_READ_TRANSIENT };
	struct harness harness;
	struct collector_snapshot snapshot;

	current_case = "zero_valid_status";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;
	harness.backend.status_results = results;
	harness.backend.status_results_len = 1;

	harness_start(&harness, 8, 1, COLLECTOR_CAP_STATUS);
	CHECK(next_snapshot(&harness, 0, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);

	// A window with no valid read renders N/A.  Returning 0.0 would be
	// indistinguishable from a confirmed-idle GPU.
	CHECK_U64(snapshot.status.valid, 0);
	CHECK_U64(snapshot.status.failed, 8);
	CHECK(isnan(collector_lane_fraction(&snapshot, COLLECTOR_LANE_GUI)));
	CHECK_CLOSE(collector_status_coverage(&snapshot), 0.0);

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_endpoint_validity(void) {
	struct harness harness;
	struct collector_snapshot snapshot;

	current_case = "endpoint_validity";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;
	harness.backend.vram_value = 4096;
	harness.backend.vram_result = COLLECTOR_READ_OK;
	harness.backend.gtt_result = COLLECTOR_READ_TRANSIENT;

	harness_start(&harness, 5, 1,
		COLLECTOR_CAP_STATUS | COLLECTOR_CAP_VRAM | COLLECTOR_CAP_GTT);
	CHECK(next_snapshot(&harness, 0, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);

	CHECK(snapshot.vram_valid);
	CHECK_U64(snapshot.vram, 4096);
	CHECK(!snapshot.gtt_valid);

	// Endpoint measurements are read once per window, not once per slot.
	pthread_mutex_lock(&harness.backend.mutex);
	CHECK_U64(harness.backend.vram_calls, 1);
	CHECK_U64(harness.backend.status_calls, 5);
	pthread_mutex_unlock(&harness.backend.mutex);

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_on_time_window(void) {
	struct harness harness;
	struct collector_snapshot snapshot;

	current_case = "on_time_window";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;

	harness_start(&harness, 10, 1, COLLECTOR_CAP_STATUS);
	CHECK(next_snapshot(&harness, 0, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);

	CHECK_U64(snapshot.attempted_slots, 10);
	CHECK_U64(snapshot.missed_slots, 0);
	CHECK_U64(snapshot.late_wakeups, 0);
	CHECK_U64((uint64_t) collector_timespec_delta_ns(&snapshot.window_start,
		&snapshot.window_end), (uint64_t) NS_PER_SEC);

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_fractional_period(void) {
	struct harness harness;
	struct collector_snapshot first, second;

	current_case = "fractional_period";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;

	// 1e9 / 120 is not an integer.  Carrying the remainder is what keeps 120
	// slots landing on exactly one second; adding a truncated 8333333 ns
	// would fall 40 microseconds short every window and drift without bound.
	harness_start(&harness, 120, 1, COLLECTOR_CAP_STATUS);
	CHECK(next_snapshot(&harness, 0, &first) == COLLECTOR_WAIT_SNAPSHOT);
	CHECK(next_snapshot(&harness, first.generation, &second) == COLLECTOR_WAIT_SNAPSHOT);

	CHECK_U64(first.nominal_slots, 120);
	CHECK_U64(first.attempted_slots, 120);
	CHECK_U64(first.missed_slots, 0);
	CHECK_U64((uint64_t) collector_timespec_delta_ns(&first.window_start,
		&first.window_end), (uint64_t) NS_PER_SEC);
	CHECK_U64((uint64_t) collector_timespec_delta_ns(&first.window_start,
		&second.window_end), (uint64_t) (2 * NS_PER_SEC));

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_late_wakeup_no_catchup(void) {
	// One wake-up arrives three and a half slot periods late.
	static const int64_t extra[] = { 0, 0, 350000000LL };
	struct harness harness;
	struct collector_snapshot snapshot;

	current_case = "late_wakeup_no_catchup";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;
	harness.clock.extra_delay_ns = extra;
	harness.clock.extra_delay_len = sizeof(extra) / sizeof(extra[0]);

	harness_start(&harness, 10, 1, COLLECTOR_CAP_STATUS);
	CHECK(next_snapshot(&harness, 0, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);

	// The stall is absorbed by skipping slots, never by firing a burst of
	// catch-up reads.  Attempted plus missed still accounts for every
	// nominal slot, and the backend saw exactly the attempted count.
	CHECK(snapshot.missed_slots >= 3);
	CHECK(snapshot.late_wakeups >= 1);
	CHECK(snapshot.max_lateness_ns >= 350000000ULL);
	CHECK_U64(snapshot.attempted_slots + snapshot.missed_slots,
		snapshot.nominal_slots);

	pthread_mutex_lock(&harness.backend.mutex);
	CHECK_U64(harness.backend.status_calls, snapshot.attempted_slots);
	pthread_mutex_unlock(&harness.backend.mutex);

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_stall_crosses_window_boundary(void) {
	// A stall longer than a whole report window.
	static const int64_t extra[] = { 0, 2500000000LL };
	struct harness harness;
	struct collector_snapshot snapshot;

	current_case = "stall_crosses_window_boundary";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;
	harness.clock.extra_delay_ns = extra;
	harness.clock.extra_delay_len = sizeof(extra) / sizeof(extra[0]);

	harness_start(&harness, 10, 1, COLLECTOR_CAP_STATUS);
	CHECK(next_snapshot(&harness, 0, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);

	// The window still publishes, and it accounts for every nominal slot
	// rather than silently shortening its denominator.
	CHECK_U64(snapshot.attempted_slots + snapshot.missed_slots,
		snapshot.nominal_slots);
	CHECK(snapshot.missed_slots > 0);
	CHECK(collector_status_coverage(&snapshot) < 1.0);

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_generations_monotonic(void) {
	struct harness harness;
	struct collector_snapshot snapshot;
	uint64_t last = 0;

	current_case = "generations_monotonic";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;

	harness_start(&harness, 4, 1, COLLECTOR_CAP_STATUS);

	for (int i = 0; i < 25; i++) {
		CHECK(next_snapshot(&harness, last, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);

		// wait_next never returns a generation already seen, so a
		// consumer counting lines counts completed windows.
		CHECK(snapshot.generation > last);

		// Internal consistency proves the copy was not torn: these
		// identities hold within one window and would not survive a
		// mixture of two.
		CHECK_U64(snapshot.attempted_slots + snapshot.missed_slots,
			snapshot.nominal_slots);
		CHECK_U64(snapshot.status.valid + snapshot.status.failed,
			snapshot.attempted_slots);

		last = snapshot.generation;
	}

	harness_stop(&harness);
	harness_destroy(&harness);
}

static int injected_join_failure(pthread_t thread, void **result) {
	(void) thread;
	(void) result;
	return EDEADLK;
}

static void case_join_failure_preserves_lifecycle_owners(void) {
	struct fake_backend backend;
	struct fake_clock clock;
	struct collector collector;
	struct engine_masks masks;
	const struct collector_config config = { 1, 1, 0 };
	struct collector_backend backend_ops;
	struct collector_clock clock_ops;

	current_case = "join_failure_preserves_lifecycle_owners";
	fake_backend_init(&backend);
	fake_clock_init(&clock);
	memset(&masks, 0, sizeof(masks));
	backend_ops = fake_backend_ops(&backend, 0);
	clock_ops = fake_clock_ops(&clock);

	CHECK(collector_init(&collector, &config, &backend_ops, &masks,
		&clock_ops) == 0);
	collector.join_thread = injected_join_failure;
	collector.thread_started = true;
	CHECK(collector_join(&collector) != 0);
	CHECK(collector.thread_started);
	CHECK(collector_destroy(&collector) != 0);
	CHECK(collector.initialized);

	collector.thread_started = false;
	CHECK(collector_destroy(&collector) == 0);
	fake_clock_destroy(&clock);
	fake_backend_destroy(&backend);
}

static void case_contiguous_wait_detects_lost_window(void) {
	struct harness harness;
	struct collector_snapshot snapshot;
	struct collector_terminal terminal;

	current_case = "contiguous_wait_detects_lost_window";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;
	harness_start(&harness, 2, 1, COLLECTOR_CAP_STATUS);

	CHECK(next_snapshot(&harness, 0, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);
	CHECK_U64(snapshot.generation, 1);
	CHECK(next_snapshot(&harness, 1, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);
	CHECK_U64(snapshot.generation, 2);
	CHECK(next_snapshot(&harness, 2, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);
	CHECK_U64(snapshot.generation, 3);

	CHECK(collector_wait_next_contiguous(&harness.collector, 0, NULL,
		&snapshot, &terminal) == COLLECTOR_WAIT_GAP);
	CHECK_U64(snapshot.generation, 3);
	CHECK(collector_wait_next_contiguous(&harness.collector, 2, NULL,
		&snapshot, &terminal) == COLLECTOR_WAIT_SNAPSHOT);
	CHECK_U64(snapshot.generation, 3);
	CHECK(collector_wait_next_contiguous(&harness.collector, UINT64_MAX,
		NULL, &snapshot, &terminal) == COLLECTOR_WAIT_GAP);
	CHECK_U64(snapshot.generation, 3);

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_old_copy_survives(void) {
	struct harness harness;
	struct collector_snapshot held, latest, again;

	current_case = "old_copy_survives";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;

	harness_start(&harness, 4, 1, COLLECTOR_CAP_STATUS);
	CHECK(next_snapshot(&harness, 0, &held) == COLLECTOR_WAIT_SNAPSHOT);

	again = held;

	// Publication replaces a whole value under the mutex, so a reader's
	// private copy cannot be rewritten two generations later the way a
	// reused double buffer behind a swapped pointer would be.
	for (int i = 0; i < 200; i++)
		CHECK(next_snapshot(&harness, 0, &latest) == COLLECTOR_WAIT_SNAPSHOT);

	CHECK(memcmp(&held, &again, sizeof(held)) == 0);

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_fatal_before_first_generation(void) {
	static const int results[] = { COLLECTOR_READ_FATAL };
	struct harness harness;
	struct collector_snapshot snapshot;
	struct collector_terminal terminal;

	current_case = "fatal_before_first_generation";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;
	harness.backend.status_results = results;
	harness.backend.status_results_len = 1;

	harness_start(&harness, 10, 1, COLLECTOR_CAP_STATUS);

	// A consumer must never spin forever waiting for a first generation that
	// will not arrive, so the fatal state itself ends the wait.
	CHECK(next_terminal(&harness, 0, &terminal) == COLLECTOR_WAIT_FATAL);
	CHECK(terminal.cause == COLLECTOR_TERMINAL_DEVICE_READ);
	CHECK_U64(terminal.after_generation, 0);
	CHECK(terminal.read_result_valid);
	CHECK(terminal.read_result == COLLECTOR_READ_FATAL);
	CHECK(!collector_peek(&harness.collector, &snapshot));

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_stop_while_waiting(void) {
	struct harness harness;
	struct collector_snapshot snapshot;
	struct collector_terminal terminal;

	current_case = "stop_while_waiting";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;

	harness_start(&harness, 4, 1, COLLECTOR_CAP_STATUS);
	CHECK(next_snapshot(&harness, 0, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);

	// The worker is parked in wait_until with no credit, so only the wake
	// path can end it; a relative sleep could not be interrupted at all.
	harness_join(&harness);

	// A consumer that asks after the worker exited is told so rather than
	// waiting for a generation that will never publish.
	CHECK(collector_wait_next(&harness.collector, snapshot.generation, NULL,
		&snapshot, &terminal) == COLLECTOR_WAIT_FINISHED);

	collector_destroy(&harness.collector);
	harness_destroy(&harness);
}

struct reader_args {
	struct harness *harness;
	uint64_t seen;
	int failures;
	bool done;
};

#define READER_TARGET 50

static void *reader_thread(void *argument) {
	struct reader_args *args = argument;
	struct collector_snapshot snapshot;
	struct collector_terminal terminal;
	uint64_t last = 0;

	while (last < READER_TARGET) {
		if (collector_wait_next(&args->harness->collector, last, NULL,
				&snapshot, &terminal) != COLLECTOR_WAIT_SNAPSHOT) {
			args->failures++;
			break;
		}

		// A snapshot copied under the mutex is internally consistent.
		// These identities hold within one window and would not survive
		// a mixture of two.
		if (snapshot.generation <= last ||
			snapshot.attempted_slots + snapshot.missed_slots !=
			snapshot.nominal_slots ||
			snapshot.status.valid + snapshot.status.failed !=
			snapshot.attempted_slots)
			args->failures++;

		last = snapshot.generation;
		args->seen++;
	}

	__atomic_store_n(&args->done, true, __ATOMIC_RELEASE);
	return NULL;
}

static void case_multiple_readers(void) {
	struct harness harness;
	struct reader_args first, second;
	pthread_t a, b;

	current_case = "multiple_readers";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;

	harness_start(&harness, 4, 1, COLLECTOR_CAP_STATUS);

	memset(&first, 0, sizeof(first));
	memset(&second, 0, sizeof(second));
	first.harness = &harness;
	second.harness = &harness;

	CHECK(pthread_create(&a, NULL, reader_thread, &first) == 0);
	CHECK(pthread_create(&b, NULL, reader_thread, &second) == 0);

	// The main thread drives the clock while both readers consume the same
	// published stream.
	for (int step = 0; step < 4 * (READER_TARGET + 4); step++) {
		if (__atomic_load_n(&first.done, __ATOMIC_ACQUIRE) &&
			__atomic_load_n(&second.done, __ATOMIC_ACQUIRE))
			break;
		fake_clock_step(&harness.clock);
	}

	pthread_join(a, NULL);
	pthread_join(b, NULL);

	CHECK(first.failures == 0);
	CHECK(second.failures == 0);
	CHECK(first.seen > 0);
	CHECK(second.seen > 0);

	harness_stop(&harness);
	harness_destroy(&harness);
}

// The one case that binds the real monotonic clock: a collector waiting on a
// deadline an hour away must still stop promptly, which is what the clock's
// wake path exists for.  A relative sleep of that length could not be
// interrupted at all.
static void case_long_interval_prompt_shutdown(void) {
	struct fake_backend backend;
	struct collector_monotonic_clock clock;
	struct collector collector;
	struct engine_masks masks;
	struct timespec before, after;
	const struct collector_config config = { 1, 3600, 0 };

	current_case = "long_interval_prompt_shutdown";
	fake_backend_init(&backend);
	memset(&masks, 0, sizeof(masks));
	masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;

	CHECK(collector_monotonic_clock_init(&clock) == 0);

	{
		const struct collector_backend ops =
			fake_backend_ops(&backend, COLLECTOR_CAP_STATUS);
		const struct collector_clock clock_ops =
			collector_monotonic_clock_ops(&clock);

		CHECK(collector_init(&collector, &config, &ops, &masks, &clock_ops) == 0);
	}

	CHECK(collector_start(&collector) == 0);

	clock_gettime(CLOCK_MONOTONIC, &before);
	collector_request_stop(&collector);
	CHECK(collector_join(&collector) == 0);
	clock_gettime(CLOCK_MONOTONIC, &after);

	CHECK(collector_timespec_delta_ns(&before, &after) < 2 * NS_PER_SEC);

	pthread_mutex_lock(&backend.mutex);
	backend.torn_down = true;
	pthread_mutex_unlock(&backend.mutex);

	collector_destroy(&collector);
	collector_monotonic_clock_destroy(&clock);
	CHECK(backend.read_after_teardown == false);
	fake_backend_destroy(&backend);
}

static void case_rejects_impossible_configuration(void) {
	struct fake_backend backend;
	struct fake_clock clock;
	struct collector collector;
	struct engine_masks masks;
	struct collector_config config;

	current_case = "rejects_impossible_configuration";
	fake_backend_init(&backend);
	fake_clock_init(&clock);
	memset(&masks, 0, sizeof(masks));

	{
		const struct collector_backend ops =
			fake_backend_ops(&backend, COLLECTOR_CAP_STATUS);
		const struct collector_clock clock_ops = fake_clock_ops(&clock);

		config.ticks = 0;
		config.dumpinterval = 1;
		CHECK(collector_init(&collector, &config, &ops, &masks, &clock_ops) != 0);

		config.ticks = 1;
		config.dumpinterval = 0;
		CHECK(collector_init(&collector, &config, &ops, &masks, &clock_ops) != 0);

		// The slot count is the loop bound and the coverage denominator.
		config.ticks = 1000000;
		config.dumpinterval = 86400;
		CHECK(collector_init(&collector, &config, &ops, &masks, &clock_ops) != 0);

		// More than one slot per nanosecond cannot produce distinct deadlines.
		config.ticks = 1000000001U;
		config.dumpinterval = 1;
		CHECK(collector_init(&collector, &config, &ops, &masks, &clock_ops) != 0);
	}

	fake_clock_destroy(&clock);
	fake_backend_destroy(&backend);
}

static void case_rejects_double_start(void) {
	struct harness harness;

	current_case = "rejects_double_start";
	harness_init(&harness);
	harness_start(&harness, 10, 1, COLLECTOR_CAP_STATUS);
	CHECK(collector_start(&harness.collector) != 0);
	harness_join(&harness);
	CHECK(collector_start(&harness.collector) != 0);
	collector_destroy(&harness.collector);
	harness_destroy(&harness);
}

static void case_publication_monotonic_clock_failure_records_terminal(void) {
	struct harness harness;
	struct collector_snapshot snapshot;
	struct collector_terminal terminal;

	current_case = "publication_monotonic_clock_failure_records_terminal";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;

	// One-slot publication reaches the injected clock in execution order: worker
	// start, pre-sample, read start, read end, post-sample, publication.
	harness.clock.fail_now_call = 6;
	harness_start(&harness, 1, 1, COLLECTOR_CAP_STATUS);
	CHECK(next_terminal(&harness, 0, &terminal) == COLLECTOR_WAIT_FATAL);
	CHECK(terminal.cause ==
		COLLECTOR_TERMINAL_CLOCK_PUBLICATION_MONOTONIC);
	CHECK_U64(terminal.after_generation, 0);
	CHECK(!terminal.read_result_valid);
	CHECK(!collector_peek(&harness.collector, &snapshot));
	CHECK(!strcmp(collector_terminal_cause_name(terminal.cause),
		"clock-publication-monotonic"));

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_publication_realtime_clock_failure_records_terminal(void) {
	struct harness harness;
	struct collector_snapshot snapshot;
	struct collector_terminal terminal;

	current_case = "publication_realtime_clock_failure_records_terminal";
	harness_init(&harness);
	harness.clock.fail_realtime_call = 1;
	harness_start(&harness, 1, 1, COLLECTOR_CAP_STATUS);

	CHECK(next_terminal(&harness, 0, &terminal) == COLLECTOR_WAIT_FATAL);
	CHECK(terminal.cause ==
		COLLECTOR_TERMINAL_CLOCK_PUBLICATION_REALTIME);
	CHECK_U64(terminal.after_generation, 0);
	CHECK(!terminal.read_result_valid);
	CHECK(!collector_peek(&harness.collector, &snapshot));

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_start_clock_failure_records_terminal(void) {
	struct harness harness;
	struct collector_terminal terminal;

	current_case = "start_clock_failure_records_terminal";
	harness_init(&harness);
	harness.clock.fail_now_call = 1;
	harness_start(&harness, 1, 1, COLLECTOR_CAP_STATUS);

	CHECK(next_terminal(&harness, 0, &terminal) == COLLECTOR_WAIT_FATAL);
	CHECK(terminal.cause == COLLECTOR_TERMINAL_CLOCK_START);
	CHECK_U64(terminal.after_generation, 0);
	CHECK(!terminal.read_result_valid);
	CHECK(!strcmp(collector_terminal_cause_name(
		(enum collector_terminal_cause) 999), "unknown"));
	CHECK(collector_terminal_cause_is_clock(COLLECTOR_TERMINAL_CLOCK_START));
	CHECK(collector_terminal_cause_is_clock(
		COLLECTOR_TERMINAL_CLOCK_PUBLICATION_REALTIME));
	CHECK(!collector_terminal_cause_is_clock(COLLECTOR_TERMINAL_DEVICE_READ));
	CHECK(!collector_terminal_cause_is_clock(COLLECTOR_TERMINAL_SCHEDULE));

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_sample_clock_failure_records_terminal(void) {
	struct harness harness;
	struct collector_snapshot snapshot;
	struct collector_terminal terminal;

	current_case = "sample_clock_failure_records_terminal";
	harness_init(&harness);
	harness.clock.fail_now_call = 3;
	harness_start(&harness, 1, 1, COLLECTOR_CAP_STATUS);

	CHECK(next_terminal(&harness, 0, &terminal) == COLLECTOR_WAIT_FATAL);
	CHECK(terminal.cause == COLLECTOR_TERMINAL_CLOCK_SAMPLE);
	CHECK_U64(terminal.after_generation, 0);
	CHECK(!terminal.read_result_valid);
	CHECK(!collector_peek(&harness.collector, &snapshot));

	pthread_mutex_lock(&harness.backend.mutex);
	CHECK_U64(harness.backend.status_calls, 0);
	pthread_mutex_unlock(&harness.backend.mutex);

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_device_failure_precedes_sample_clock_failure(void) {
	static const int results[] = { COLLECTOR_READ_FATAL };
	struct harness harness;
	struct collector_snapshot snapshot;
	struct collector_terminal terminal;

	current_case = "device_failure_precedes_sample_clock_failure";
	harness_init(&harness);
	harness.backend.status_results = results;
	harness.backend.status_results_len = 1;
	// The fourth monotonic read measures latency after the backend has already
	// reported device loss.  The terminal record preserves execution order.
	harness.clock.fail_now_call = 4;
	harness_start(&harness, 1, 1, COLLECTOR_CAP_STATUS);

	CHECK(next_terminal(&harness, 0, &terminal) == COLLECTOR_WAIT_FATAL);
	CHECK(terminal.cause == COLLECTOR_TERMINAL_DEVICE_READ);
	CHECK_U64(terminal.after_generation, 0);
	CHECK(terminal.read_result_valid);
	CHECK(!collector_peek(&harness.collector, &snapshot));

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_device_failure_precedes_publication_clock_failure(void) {
	struct harness harness;
	struct collector_snapshot snapshot;
	struct collector_terminal terminal;

	current_case = "device_failure_precedes_publication_clock_failure";
	harness_init(&harness);
	harness.backend.vram_result = COLLECTOR_READ_FATAL;
	// The endpoint read reports device loss before the sixth monotonic call
	// fails while publication stamps the completed window.
	harness.clock.fail_now_call = 6;
	harness_start(&harness, 1, 1, COLLECTOR_CAP_STATUS | COLLECTOR_CAP_VRAM);

	CHECK(next_terminal(&harness, 0, &terminal) == COLLECTOR_WAIT_FATAL);
	CHECK(terminal.cause == COLLECTOR_TERMINAL_DEVICE_READ);
	CHECK_U64(terminal.after_generation, 0);
	CHECK(terminal.read_result_valid);
	CHECK(!collector_peek(&harness.collector, &snapshot));

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_terminal_after_publication_preserves_snapshot(void) {
	struct harness harness;
	struct collector_snapshot published, retained, after_terminal;
	struct collector_terminal terminal, peeked, sentinel, sentinel_copy;

	current_case = "terminal_after_publication_preserves_snapshot";
	harness_init(&harness);
	harness.clock.fail_wait_call = 2;
	harness_start(&harness, 1, 1, COLLECTOR_CAP_STATUS);

	CHECK(next_snapshot(&harness, 0, &published) == COLLECTOR_WAIT_SNAPSHOT);
	retained = published;
	CHECK(next_terminal(&harness, published.generation, &terminal) ==
		COLLECTOR_WAIT_FATAL);
	CHECK(terminal.cause == COLLECTOR_TERMINAL_CLOCK_WAIT);
	CHECK_U64(terminal.after_generation, published.generation);
	CHECK(!terminal.read_result_valid);
	CHECK(collector_state_peek(&harness.collector, &after_terminal,
		&peeked) == 0);
	CHECK(!memcmp(&retained, &after_terminal, sizeof(retained)));
	CHECK(!memcmp(&terminal, &peeked, sizeof(terminal)));

	// Snapshot delivery writes only snapshot_out even when the terminal record
	// already exists; the caller can therefore distinguish the active result.
	memset(&sentinel, 0x5a, sizeof(sentinel));
	sentinel_copy = sentinel;
	CHECK(collector_wait_next(&harness.collector, 0, NULL,
		&after_terminal, &sentinel) == COLLECTOR_WAIT_SNAPSHOT);
	CHECK(!memcmp(&sentinel, &sentinel_copy, sizeof(sentinel)));
	CHECK(!memcmp(&retained, &after_terminal, sizeof(retained)));

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_state_peek_pairs_terminal_with_bound_generation(void) {
	struct harness harness;
	struct collector_snapshot stale, latest, state_snapshot;
	struct collector_terminal terminal, state_terminal;

	current_case = "state_peek_pairs_terminal_with_bound_generation";
	harness_init(&harness);
	harness.clock.fail_wait_call = 3;
	harness_start(&harness, 1, 1, COLLECTOR_CAP_STATUS);

	CHECK(next_snapshot(&harness, 0, &stale) == COLLECTOR_WAIT_SNAPSHOT);
	CHECK(next_snapshot(&harness, stale.generation, &latest) ==
		COLLECTOR_WAIT_SNAPSHOT);
	CHECK(next_terminal(&harness, latest.generation, &terminal) ==
		COLLECTOR_WAIT_FATAL);
	CHECK_U64(stale.generation, 1);
	CHECK_U64(latest.generation, 2);

	// The stale generation and later terminal calibrate the pair that two
	// independent reads could expose to the UI.
	CHECK(stale.generation < terminal.after_generation);
	CHECK(collector_state_peek(&harness.collector, &state_snapshot,
		&state_terminal) == 0);
	CHECK_U64(state_snapshot.generation, latest.generation);
	CHECK_U64(state_terminal.after_generation, state_snapshot.generation);
	CHECK(!memcmp(&terminal, &state_terminal, sizeof(terminal)));

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_consumer_wait_error_is_reported(void) {
	struct fake_backend backend;
	struct fake_clock clock;
	struct collector collector;
	struct engine_masks masks;
	struct collector_snapshot snapshot;
	struct collector_snapshot unchanged;
	struct collector_terminal terminal;
	struct collector_terminal terminal_unchanged;
	struct timespec invalid_deadline = { 0, NS_PER_SEC };
	const struct collector_config config = { 10, 1, 0 };

	current_case = "consumer_wait_error_is_reported";
	fake_backend_init(&backend);
	fake_clock_init(&clock);
	memset(&masks, 0, sizeof(masks));

	{
		const struct collector_backend ops =
			fake_backend_ops(&backend, COLLECTOR_CAP_STATUS);
		const struct collector_clock clock_ops = fake_clock_ops(&clock);

		CHECK(collector_init(&collector, &config, &ops, &masks,
			&clock_ops) == 0);
	}

	memset(&snapshot, 0xa5, sizeof(snapshot));
	unchanged = snapshot;
	memset(&terminal, 0x5a, sizeof(terminal));
	terminal_unchanged = terminal;
	CHECK(collector_wait_next(&collector, 0, &invalid_deadline,
		&snapshot, &terminal) == COLLECTOR_WAIT_ERROR);
	CHECK(!memcmp(&snapshot, &unchanged, sizeof(snapshot)));
	CHECK(!memcmp(&terminal, &terminal_unchanged, sizeof(terminal)));
	collector_destroy(&collector);
	fake_clock_destroy(&clock);
	fake_backend_destroy(&backend);
}

static void case_sample_belongs_to_its_own_window(void) {
	// One wake-up arrives after the first report window already ended.  The
	// stall stops inside the second window, so the window that expired is the
	// one a reader still observes; a longer stall would publish a later
	// generation over it and hide the attribution.
	static const int64_t extra[] = { 0, 1500000000LL };
	struct harness harness;
	struct collector_snapshot snapshot;

	current_case = "sample_belongs_to_its_own_window";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;
	harness.clock.extra_delay_ns = extra;
	harness.clock.extra_delay_len = sizeof(extra) / sizeof(extra[0]);

	harness_start(&harness, 10, 1, COLLECTOR_CAP_STATUS);
	CHECK(next_snapshot(&harness, 0, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);

	// The window holds only the sample taken at its own start.  The sample
	// taken after the stall measures a device state absent from the window, so
	// it belongs to the window containing its timestamp; counting it in the
	// first window would report a real measurement against the wrong second.
	CHECK_U64(snapshot.generation, 1);
	CHECK_U64(snapshot.attempted_slots, 1);
	CHECK_U64(snapshot.missed_slots, 9);
	CHECK_U64(snapshot.nominal_slots, 10);
	CHECK_U64(snapshot.status.valid, 1);

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_stall_leaves_expired_windows_empty(void) {
	// A single wake-up arrives two and a half report windows late.
	static const int64_t extra[] = { 0, 2500000000LL };
	struct harness harness;
	struct collector_snapshot snapshot;

	current_case = "stall_leaves_expired_windows_empty";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;
	harness.clock.extra_delay_ns = extra;
	harness.clock.extra_delay_len = sizeof(extra) / sizeof(extra[0]);

	harness_start(&harness, 10, 1, COLLECTOR_CAP_STATUS);

	// A window that both began and ended inside the stall publishes with no
	// attempt of its own rather than being skipped or shortened.
	CHECK(next_snapshot(&harness, 0, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);
	CHECK_U64(snapshot.generation, 2);
	CHECK_U64(snapshot.attempted_slots, 0);
	CHECK_U64(snapshot.missed_slots, 10);
	CHECK_U64(snapshot.nominal_slots, 10);

	// A lane whose register validated no read is undefined, not idle.
	CHECK(isnan(collector_lane_fraction(&snapshot, COLLECTOR_LANE_GUI)));

	// Two wake-ups, so two reads: one at the window start and one after the
	// stall.  Sampling once per expired window would show ten or more.
	pthread_mutex_lock(&harness.backend.mutex);
	CHECK_U64(harness.backend.status_calls, 2);
	pthread_mutex_unlock(&harness.backend.mutex);

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_backend_latency_returns_to_grid(void) {
	struct harness harness;
	struct collector_snapshot snapshot;

	current_case = "backend_latency_returns_to_grid";
	harness_init(&harness);
	harness.masks.lane[COLLECTOR_LANE_GUI] = GUI_BIT;

	// A read two and a half slot periods long, so every read finishes with
	// deadlines already behind it.
	harness.backend.clock = &harness.clock;
	harness.backend.read_delay_ns = 250000000LL;

	harness_start(&harness, 10, 1, COLLECTOR_CAP_STATUS);
	CHECK(next_snapshot(&harness, 0, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);

	// Reads land at 0.0, 0.3, 0.6 and 0.9: each returns to the grid at the
	// first deadline after the read finished, giving up the two slots the read
	// itself crossed.  Skipping on the pre-read timestamp instead would leave
	// the next deadline already past, and the worker would read back to back
	// for as long as the device stayed slow.
	CHECK_U64(snapshot.attempted_slots, 4);
	CHECK_U64(snapshot.missed_slots, 6);
	CHECK_U64(snapshot.attempted_slots + snapshot.missed_slots,
		snapshot.nominal_slots);
	CHECK(snapshot.max_read_latency_ns >= 250000000ULL);

	pthread_mutex_lock(&harness.backend.mutex);
	CHECK_U64(harness.backend.status_calls, snapshot.attempted_slots);
	pthread_mutex_unlock(&harness.backend.mutex);

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_fatal_status_stops_the_slot(void) {
	static const int results[] = { COLLECTOR_READ_FATAL };
	struct harness harness;
	struct collector_terminal terminal;

	current_case = "fatal_status_stops_the_slot";
	harness_init(&harness);
	harness.backend.status_results = results;
	harness.backend.status_results_len = 1;

	harness_start(&harness, 10, 1, COLLECTOR_CAP_STATUS | COLLECTOR_CAP_UVD |
		COLLECTOR_CAP_VCE | COLLECTOR_CAP_SCLK | COLLECTOR_CAP_MCLK);
	CHECK(next_terminal(&harness, 0, &terminal) == COLLECTOR_WAIT_FATAL);
	CHECK(terminal.cause == COLLECTOR_TERMINAL_DEVICE_READ);
	CHECK_U64(terminal.after_generation, 0);

	// A device that reported itself gone answers no later register, so the
	// remaining reads in the slot are never issued.
	pthread_mutex_lock(&harness.backend.mutex);
	CHECK_U64(harness.backend.status_calls, 1);
	CHECK_U64(harness.backend.uvd_calls, 0);
	CHECK_U64(harness.backend.vce_calls, 0);
	CHECK_U64(harness.backend.sclk_calls, 0);
	CHECK_U64(harness.backend.mclk_calls, 0);
	pthread_mutex_unlock(&harness.backend.mutex);

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_fatal_sclk_stops_before_mclk(void) {
	static const int results[] = { COLLECTOR_READ_FATAL };
	struct harness harness;
	struct collector_terminal terminal;

	current_case = "fatal_sclk_stops_before_mclk";
	harness_init(&harness);
	harness.backend.sclk_results = results;
	harness.backend.sclk_results_len = 1;

	harness_start(&harness, 10, 1, COLLECTOR_CAP_STATUS | COLLECTOR_CAP_SCLK |
		COLLECTOR_CAP_MCLK);
	CHECK(next_terminal(&harness, 0, &terminal) == COLLECTOR_WAIT_FATAL);
	CHECK(terminal.cause == COLLECTOR_TERMINAL_DEVICE_READ);
	CHECK_U64(terminal.after_generation, 0);

	pthread_mutex_lock(&harness.backend.mutex);
	CHECK_U64(harness.backend.status_calls, 1);
	CHECK_U64(harness.backend.sclk_calls, 1);
	CHECK_U64(harness.backend.mclk_calls, 0);
	pthread_mutex_unlock(&harness.backend.mutex);

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_fatal_endpoint_finishes_the_collector(void) {
	struct harness harness;
	struct collector_snapshot snapshot, retained, state_snapshot;
	struct collector_terminal terminal, state_terminal;

	current_case = "fatal_endpoint_finishes_the_collector";
	harness_init(&harness);
	harness.backend.vram_result = COLLECTOR_READ_FATAL;

	harness_start(&harness, 2, 1, COLLECTOR_CAP_STATUS | COLLECTOR_CAP_VRAM |
		COLLECTOR_CAP_GTT);

	// The endpoint reads run at publication, so the complete window reaches the
	// consumer before the separate device terminal record.
	CHECK(next_snapshot(&harness, 0, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);
	retained = snapshot;
	CHECK(snapshot.vram_valid == false);
	CHECK(next_terminal(&harness, snapshot.generation, &terminal) ==
		COLLECTOR_WAIT_FATAL);
	CHECK(terminal.cause == COLLECTOR_TERMINAL_DEVICE_READ);
	CHECK_U64(terminal.after_generation, snapshot.generation);
	CHECK(terminal.read_result_valid);
	CHECK(terminal.read_result == COLLECTOR_READ_FATAL);
	CHECK(collector_state_peek(&harness.collector, &state_snapshot,
		&state_terminal) == 0);
	CHECK(!memcmp(&retained, &state_snapshot, sizeof(retained)));
	CHECK(!memcmp(&terminal, &state_terminal, sizeof(terminal)));

	// A fatal VRAM result already established the device is gone.
	pthread_mutex_lock(&harness.backend.mutex);
	CHECK_U64(harness.backend.vram_calls, 1);
	CHECK_U64(harness.backend.gtt_calls, 0);
	pthread_mutex_unlock(&harness.backend.mutex);

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_transient_endpoint_continues(void) {
	struct harness harness;
	struct collector_snapshot snapshot;

	current_case = "transient_endpoint_continues";
	harness_init(&harness);
	harness.backend.vram_result = COLLECTOR_READ_TRANSIENT;
	harness.backend.gtt_result = COLLECTOR_READ_OK;
	harness.backend.gtt_value = 4096;

	harness_start(&harness, 2, 1, COLLECTOR_CAP_STATUS | COLLECTOR_CAP_VRAM |
		COLLECTOR_CAP_GTT);

	// A transient endpoint failure invalidates its own point measurement and
	// leaves the collector running and the other endpoint readable.
	CHECK(next_snapshot(&harness, 0, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);
	CHECK(snapshot.vram_valid == false);
	CHECK(snapshot.gtt_valid);
	CHECK_U64(snapshot.gtt, 4096);

	CHECK(next_snapshot(&harness, snapshot.generation, &snapshot) ==
		COLLECTOR_WAIT_SNAPSHOT);

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_signal_validity_is_independent(void) {
	struct harness harness;
	struct collector_snapshot snapshot;

	current_case = "signal_validity_is_independent";
	harness_init(&harness);
	harness.backend.mclk_result = COLLECTOR_READ_TRANSIENT;

	harness_start(&harness, 4, 1, COLLECTOR_CAP_STATUS | COLLECTOR_CAP_SCLK |
		COLLECTOR_CAP_MCLK);
	CHECK(next_snapshot(&harness, 0, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);

	// Every memory-clock read failed while every shader-clock read succeeded.
	// The two carry separate counts, which is what lets a consumer render one
	// row and withhold the other instead of gating both on one signal.
	CHECK_U64(snapshot.sclk.valid, 4);
	CHECK_U64(snapshot.sclk.failed, 0);
	CHECK_U64(snapshot.mclk.valid, 0);
	CHECK_U64(snapshot.mclk.failed, 4);

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_scheduled_end_tracks_publication_lag(void) {
	struct harness harness;
	struct collector_snapshot snapshot;

	current_case = "scheduled_end_tracks_publication_lag";
	harness_init(&harness);

	harness_start(&harness, 4, 1, COLLECTOR_CAP_STATUS);
	CHECK(next_snapshot(&harness, 0, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);

	// The wall-clock label is the realtime stamp less the publication lag
	// measured on the monotonic clock, so the two differences agree whichever
	// side of the scheduled end publication falls on.  Reading CLOCK_REALTIME
	// at publication and calling it the scheduled end would instead move the
	// label by however long the last slot and the endpoint reads took.
	{
		const int64_t monotonic_lag = collector_timespec_delta_ns(
			&snapshot.window_end, &snapshot.published);
		const int64_t realtime_lag = collector_timespec_delta_ns(
			&snapshot.scheduled_end_realtime, &snapshot.published_realtime);

		CHECK(monotonic_lag == realtime_lag);

		// The last slot of a window sits one period before the scheduled end,
		// so an unstalled window publishes early rather than late.
		CHECK(monotonic_lag < 0);
	}

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_late_publication_keeps_scheduled_end(void) {
	struct harness harness;
	struct collector_snapshot snapshot;

	current_case = "late_publication_keeps_scheduled_end";
	harness_init(&harness);

	// Reads long enough that the last slot of the window finishes after the
	// scheduled end, which is the case the label has to survive.
	harness.backend.clock = &harness.clock;
	harness.backend.read_delay_ns = 250000000LL;

	harness_start(&harness, 10, 1, COLLECTOR_CAP_STATUS);
	CHECK(next_snapshot(&harness, 0, &snapshot) == COLLECTOR_WAIT_SNAPSHOT);

	{
		const int64_t monotonic_lag = collector_timespec_delta_ns(
			&snapshot.window_end, &snapshot.published);
		const int64_t realtime_lag = collector_timespec_delta_ns(
			&snapshot.scheduled_end_realtime, &snapshot.published_realtime);

		CHECK(monotonic_lag > 0);
		CHECK(monotonic_lag == realtime_lag);
	}

	harness_stop(&harness);
	harness_destroy(&harness);
}

static void case_rejects_incomplete_interfaces(void) {
	struct fake_backend backend;
	struct fake_clock clock;
	struct collector collector;
	struct engine_masks masks;
	const struct collector_config config = { 10, 1, 0 };

	current_case = "rejects_incomplete_interfaces";
	fake_backend_init(&backend);
	fake_clock_init(&clock);
	memset(&masks, 0, sizeof(masks));

	// A capability asserts its register is readable, so the callback that
	// reads it is part of the assertion; a null one would fault on the first
	// sample rather than at construction.
	{
		struct collector_backend ops =
			fake_backend_ops(&backend, COLLECTOR_CAP_STATUS);
		const struct collector_clock clock_ops = fake_clock_ops(&clock);

		ops.read_status = NULL;
		CHECK(collector_init(&collector, &config, &ops, &masks, &clock_ops) != 0);
	}

	{
		const struct collector_backend ops =
			fake_backend_ops(&backend, COLLECTOR_CAP_STATUS |
				COLLECTOR_CAP_UVD);
		struct collector_backend broken = ops;
		const struct collector_clock clock_ops = fake_clock_ops(&clock);

		broken.read_uvd_status = NULL;
		CHECK(collector_init(&collector, &config, &broken, &masks, &clock_ops) != 0);
	}

	// The worker requires monotonic scheduling and a publication wall clock.
	{
		const struct collector_backend ops =
			fake_backend_ops(&backend, COLLECTOR_CAP_STATUS);
		struct collector_clock clock_ops = fake_clock_ops(&clock);

		clock_ops.now = NULL;
		CHECK(collector_init(&collector, &config, &ops, &masks, &clock_ops) != 0);

		clock_ops = fake_clock_ops(&clock);
		clock_ops.wait_until = NULL;
		CHECK(collector_init(&collector, &config, &ops, &masks, &clock_ops) != 0);

		clock_ops = fake_clock_ops(&clock);
		clock_ops.realtime_now = NULL;
		CHECK(collector_init(&collector, &config, &ops, &masks, &clock_ops) != 0);
	}

	fake_clock_destroy(&clock);
	fake_backend_destroy(&backend);
}

static void case_missing_data_bounds(void) {
	struct collector_snapshot snapshot;
	struct collector_lane_bounds bounds;

	current_case = "missing_data_bounds";
	memset(&snapshot, 0, sizeof(snapshot));
	snapshot.nominal_slots = 10;
	snapshot.status.valid = 8;
	snapshot.lane_busy[COLLECTOR_LANE_GUI] = 3;

	CHECK(collector_lane_missing_data_bounds(&snapshot,
		COLLECTOR_LANE_GUI, &bounds));
	CHECK_U64(bounds.busy, 3);
	CHECK_U64(bounds.valid, 8);
	CHECK_U64(bounds.nominal, 10);
	CHECK_CLOSE(bounds.conditional_fraction, 3.0 / 8.0);
	CHECK_CLOSE(bounds.unconditional_lower, 3.0 / 10.0);
	CHECK_CLOSE(bounds.unconditional_upper, 5.0 / 10.0);

	snapshot.status.valid = 0;
	snapshot.lane_busy[COLLECTOR_LANE_GUI] = 0;
	CHECK(collector_lane_missing_data_bounds(&snapshot,
		COLLECTOR_LANE_GUI, &bounds));
	CHECK(isnan(bounds.conditional_fraction));
	CHECK_CLOSE(bounds.unconditional_lower, 0.0);
	CHECK_CLOSE(bounds.unconditional_upper, 1.0);

	snapshot.uvd.valid = 5;
	snapshot.lane_busy[COLLECTOR_LANE_UVD] = 4;
	CHECK(collector_lane_missing_data_bounds(&snapshot,
		COLLECTOR_LANE_UVD, &bounds));
	CHECK_U64(bounds.valid, 5);
	CHECK_CLOSE(bounds.unconditional_lower, 0.4);
	CHECK_CLOSE(bounds.unconditional_upper, 0.9);

	snapshot.status.valid = 11;
	CHECK(!collector_lane_missing_data_bounds(&snapshot,
		COLLECTOR_LANE_GUI, &bounds));
	snapshot.status.valid = 2;
	snapshot.lane_busy[COLLECTOR_LANE_GUI] = 3;
	CHECK(!collector_lane_missing_data_bounds(&snapshot,
		COLLECTOR_LANE_GUI, &bounds));
	CHECK(!collector_lane_missing_data_bounds(&snapshot,
		(enum collector_lane) -1, &bounds));
	CHECK(!collector_lane_missing_data_bounds(&snapshot,
		COLLECTOR_LANE_COUNT, &bounds));
	CHECK(!collector_lane_missing_data_bounds(NULL,
		COLLECTOR_LANE_GUI, &bounds));
	CHECK(!collector_lane_missing_data_bounds(&snapshot,
		COLLECTOR_LANE_GUI, NULL));

	snapshot.nominal_slots = 0;
	CHECK(!collector_lane_missing_data_bounds(&snapshot,
		COLLECTOR_LANE_GUI, &bounds));
}

int main(void) {
	case_alternating_status();
	case_failed_status_between_valid();
	case_rb2d_union();
	case_absent_uvd();
	case_failed_uvd();
	case_failed_clock_no_stale_reuse();
	case_zero_valid_status();
	case_endpoint_validity();
	case_on_time_window();
	case_fractional_period();
	case_late_wakeup_no_catchup();
	case_stall_crosses_window_boundary();
	case_sample_belongs_to_its_own_window();
	case_stall_leaves_expired_windows_empty();
	case_backend_latency_returns_to_grid();
	case_fatal_status_stops_the_slot();
	case_fatal_sclk_stops_before_mclk();
	case_fatal_endpoint_finishes_the_collector();
	case_transient_endpoint_continues();
	case_signal_validity_is_independent();
	case_scheduled_end_tracks_publication_lag();
	case_late_publication_keeps_scheduled_end();
	case_rejects_incomplete_interfaces();
	case_missing_data_bounds();
	case_generations_monotonic();
	case_join_failure_preserves_lifecycle_owners();
	case_contiguous_wait_detects_lost_window();
	case_old_copy_survives();
	case_fatal_before_first_generation();
	case_stop_while_waiting();
	case_multiple_readers();
	case_long_interval_prompt_shutdown();
	case_rejects_impossible_configuration();
	case_rejects_double_start();
	case_publication_monotonic_clock_failure_records_terminal();
	case_publication_realtime_clock_failure_records_terminal();
	case_start_clock_failure_records_terminal();
	case_sample_clock_failure_records_terminal();
	case_device_failure_precedes_sample_clock_failure();
	case_device_failure_precedes_publication_clock_failure();
	case_terminal_after_publication_preserves_snapshot();
	case_state_peek_pairs_terminal_with_bound_generation();
	case_consumer_wait_error_is_reported();
	case_dither_rejects_modulo_bias();
	case_dither_uses_exact_fractional_slot_width();
	case_dither_keeps_samples_in_their_slots();
	case_unseeded_schedule_stays_exact();
	case_dither_seed_reproduces_offsets();
	case_dither_skips_only_passed_sample_points();

	printf("collector: %u checks, %u failed\n", checks_run, checks_failed);

	return checks_failed ? 1 : 0;
}
