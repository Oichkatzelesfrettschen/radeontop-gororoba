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

#include "collector.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#define NS_PER_SEC 1000000000LL

// Accumulated state for the window in progress.  It never leaves the worker
// thread; publication copies a finished snapshot under the mutex instead.
struct collector_accumulator {
	uint64_t lane_busy[COLLECTOR_LANE_COUNT];

	struct collector_signal_stats status;
	struct collector_signal_stats uvd;
	struct collector_signal_stats vce;
	struct collector_signal_stats sclk;
	struct collector_signal_stats mclk;

	// Online means, so a long or high-rate window cannot overflow the
	// integral sum a running total would need.
	double sclk_mean_khz;
	double mclk_mean_khz;

	uint64_t attempted_slots;
	uint64_t missed_slots;
	uint64_t late_wakeups;
	uint64_t max_lateness_ns;
	uint64_t max_read_latency_ns;

	int fatal_read_result;
};

static void timespec_add_ns(struct timespec *ts, int64_t ns) {
	int64_t sec = ns / NS_PER_SEC;
	int64_t rest = ns % NS_PER_SEC;

	ts->tv_sec += (time_t) sec;
	ts->tv_nsec += (long) rest;

	if (ts->tv_nsec >= NS_PER_SEC) {
		ts->tv_nsec -= NS_PER_SEC;
		ts->tv_sec++;
	} else if (ts->tv_nsec < 0) {
		ts->tv_nsec += NS_PER_SEC;
		ts->tv_sec--;
	}
}

int64_t collector_timespec_delta_ns(const struct timespec *from,
		const struct timespec *to) {
	return ((int64_t) to->tv_sec - (int64_t) from->tv_sec) * NS_PER_SEC +
		((int64_t) to->tv_nsec - (int64_t) from->tv_nsec);
}

static bool timespec_reached(const struct timespec *deadline,
		const struct timespec *now) {
	return collector_timespec_delta_ns(deadline, now) >= 0;
}

// The monotonic clock backing a production collector.

static int monotonic_now(void *context, struct timespec *ts) {
	(void) context;
	return clock_gettime(CLOCK_MONOTONIC, ts);
}

// wake latches: the collector wakes only to observe a stop request, so a wake
// that arrives while nobody waits must still short-circuit the next wait.
static int monotonic_wait_until(void *context, const struct timespec *deadline) {
	struct collector_monotonic_clock *clock = context;
	int result = 0;

	if (pthread_mutex_lock(&clock->mutex))
		return -1;
	while (!clock->woken) {
		const int wait_result =
			pthread_cond_timedwait(&clock->cond, &clock->mutex, deadline);

		if (wait_result == ETIMEDOUT)
			break;
		if (wait_result) {
			result = -1;
			break;
		}
	}
	if (pthread_mutex_unlock(&clock->mutex))
		result = -1;

	return result;
}

static void monotonic_wake(void *context) {
	struct collector_monotonic_clock *clock = context;

	pthread_mutex_lock(&clock->mutex);
	clock->woken = true;
	pthread_cond_broadcast(&clock->cond);
	pthread_mutex_unlock(&clock->mutex);
}

int collector_monotonic_clock_init(struct collector_monotonic_clock *clock) {
	pthread_condattr_t attr;

	memset(clock, 0, sizeof(*clock));

	if (pthread_mutex_init(&clock->mutex, NULL))
		return -1;

	if (pthread_condattr_init(&attr)) {
		pthread_mutex_destroy(&clock->mutex);
		return -2;
	}

	// The deadline passed to pthread_cond_timedwait is a CLOCK_MONOTONIC
	// value, so the condition variable must measure against that clock; the
	// default realtime clock would make a settime jump move every deadline.
	if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) ||
		pthread_cond_init(&clock->cond, &attr)) {
		pthread_condattr_destroy(&attr);
		pthread_mutex_destroy(&clock->mutex);
		return -3;
	}

	pthread_condattr_destroy(&attr);
	clock->ready = true;
	return 0;
}

void collector_monotonic_clock_destroy(struct collector_monotonic_clock *clock) {
	if (!clock->ready)
		return;

	pthread_cond_destroy(&clock->cond);
	pthread_mutex_destroy(&clock->mutex);
	clock->ready = false;
}

struct collector_clock collector_monotonic_clock_ops(struct collector_monotonic_clock *clock) {
	struct collector_clock ops;

	ops.context = clock;
	ops.now = monotonic_now;
	ops.wait_until = monotonic_wait_until;
	ops.wake = monotonic_wake;

	return ops;
}

// Reading and accumulating.

static void note_read(struct collector_accumulator *accumulator,
		struct collector_signal_stats *stats, int result) {
	if (result == COLLECTOR_READ_OK) {
		stats->valid++;
		return;
	}

	stats->failed++;

	// A fatal classification wins over a transient one and stops the window;
	// the adapter, not the collector, decides which a given errno is.
	if (result == COLLECTOR_READ_FATAL)
		accumulator->fatal_read_result = COLLECTOR_READ_FATAL;
}

// Returns 0, or -1 when the clock failed and the latency bounds would be
// indeterminate.  A fatal classification stops the slot at the register that
// reported it: a device that answers "gone" answers no later register either,
// so the remaining reads would be requests to hardware already known absent.
static int sample_once(struct collector *collector,
		struct collector_accumulator *accumulator) {
	const struct collector_backend *backend = &collector->backend;
	struct timespec before, after;
	uint32_t value;
	int result;

	if (collector->clock.now(collector->clock.context, &before))
		return -1;

	if (backend->capabilities & COLLECTOR_CAP_STATUS) {
		value = 0;
		result = backend->read_status(backend->context, &value);
		note_read(accumulator, &accumulator->status, result);

		// A failed status read validates no lane, so it contributes to
		// neither a numerator nor a denominator.
		if (result == COLLECTOR_READ_OK) {
			for (int lane = 0; lane < COLLECTOR_STATUS_LANE_COUNT; lane++) {
				const uint32_t mask = collector->masks.lane[lane];

				// A multi-bit mask is a union: the lane counts
				// busy when any of its bits is set.
				if (mask && (value & mask))
					accumulator->lane_busy[lane]++;
			}
		}

		if (result == COLLECTOR_READ_FATAL)
			goto done;
	}

	if (backend->capabilities & COLLECTOR_CAP_UVD) {
		value = 0;
		result = backend->read_uvd_status(backend->context, &value);
		note_read(accumulator, &accumulator->uvd, result);
		if (result == COLLECTOR_READ_OK &&
			collector->masks.lane[COLLECTOR_LANE_UVD] &&
			(value & collector->masks.lane[COLLECTOR_LANE_UVD]))
			accumulator->lane_busy[COLLECTOR_LANE_UVD]++;

		if (result == COLLECTOR_READ_FATAL)
			goto done;
	}

	if (backend->capabilities & COLLECTOR_CAP_VCE) {
		value = 0;
		result = backend->read_vce_status(backend->context, &value);
		note_read(accumulator, &accumulator->vce, result);
		if (result == COLLECTOR_READ_OK &&
			collector->masks.lane[COLLECTOR_LANE_VCE0] &&
			(value & collector->masks.lane[COLLECTOR_LANE_VCE0]))
			accumulator->lane_busy[COLLECTOR_LANE_VCE0]++;

		if (result == COLLECTOR_READ_FATAL)
			goto done;
	}

	// A failed clock read contributes no value, so the mean stays over the
	// readings that succeeded rather than carrying a stale one forward.
	if (backend->capabilities & COLLECTOR_CAP_SCLK) {
		value = 0;
		result = backend->read_sclk(backend->context, &value);
		note_read(accumulator, &accumulator->sclk, result);
		if (result == COLLECTOR_READ_OK)
			accumulator->sclk_mean_khz +=
				((double) value - accumulator->sclk_mean_khz) /
				(double) accumulator->sclk.valid;

		if (result == COLLECTOR_READ_FATAL)
			goto done;
	}

	if (backend->capabilities & COLLECTOR_CAP_MCLK) {
		value = 0;
		result = backend->read_mclk(backend->context, &value);
		note_read(accumulator, &accumulator->mclk, result);
		if (result == COLLECTOR_READ_OK)
			accumulator->mclk_mean_khz +=
				((double) value - accumulator->mclk_mean_khz) /
				(double) accumulator->mclk.valid;
	}

done:
	if (collector->clock.now(collector->clock.context, &after))
		return -1;

	{
		const int64_t latency = collector_timespec_delta_ns(&before, &after);

		if (latency > 0 && (uint64_t) latency > accumulator->max_read_latency_ns)
			accumulator->max_read_latency_ns = (uint64_t) latency;
	}

	return 0;
}

// Returns COLLECTOR_READ_FATAL when an endpoint read reported the device gone,
// in which case the published snapshot already carries the fatal flag and the
// worker exits rather than reading the same device again.
static int publish(struct collector *collector,
		const struct collector_accumulator *accumulator,
		const struct timespec *window_start,
		const struct timespec *window_end,
		uint64_t nominal_slots) {
	const struct collector_backend *backend = &collector->backend;
	struct collector_snapshot snapshot;
	int endpoint_result = COLLECTOR_READ_OK;

	memset(&snapshot, 0, sizeof(snapshot));

	snapshot.window_start = *window_start;
	snapshot.window_end = *window_end;
	snapshot.nominal_slots = nominal_slots;
	snapshot.attempted_slots = accumulator->attempted_slots;
	snapshot.missed_slots = accumulator->missed_slots;
	snapshot.late_wakeups = accumulator->late_wakeups;
	snapshot.max_lateness_ns = accumulator->max_lateness_ns;
	snapshot.max_read_latency_ns = accumulator->max_read_latency_ns;
	snapshot.status = accumulator->status;
	snapshot.uvd = accumulator->uvd;
	snapshot.vce = accumulator->vce;
	snapshot.sclk = accumulator->sclk;
	snapshot.mclk = accumulator->mclk;
	snapshot.sclk_mean_khz = accumulator->sclk_mean_khz;
	snapshot.mclk_mean_khz = accumulator->mclk_mean_khz;
	snapshot.capabilities = backend->capabilities;

	memcpy(snapshot.lane_busy, accumulator->lane_busy, sizeof(snapshot.lane_busy));

	// VRAM and GTT are point measurements at the window endpoint, not means
	// over the window, and each carries its own validity.
	if (backend->capabilities & COLLECTOR_CAP_VRAM) {
		uint64_t value = 0;
		const int result = backend->read_vram(backend->context, &value);

		if (result == COLLECTOR_READ_OK) {
			snapshot.vram = value;
			snapshot.vram_valid = true;
		} else if (result == COLLECTOR_READ_FATAL) {
			endpoint_result = COLLECTOR_READ_FATAL;
		}
	}

	// A fatal VRAM result already established the device is gone, so the GTT
	// query is not issued.
	if ((backend->capabilities & COLLECTOR_CAP_GTT) &&
		endpoint_result != COLLECTOR_READ_FATAL) {
		uint64_t value = 0;
		const int result = backend->read_gtt(backend->context, &value);

		if (result == COLLECTOR_READ_OK) {
			snapshot.gtt = value;
			snapshot.gtt_valid = true;
		} else if (result == COLLECTOR_READ_FATAL) {
			endpoint_result = COLLECTOR_READ_FATAL;
		}
	}

	if (collector->clock.now(collector->clock.context, &snapshot.published) ||
		clock_gettime(CLOCK_REALTIME, &snapshot.published_realtime)) {
		endpoint_result = COLLECTOR_READ_FATAL;
	} else {
		// The published monotonic/realtime pair is near-simultaneous, so the
		// publication lag on the monotonic clock converts the realtime stamp to the
		// scheduled end.  Dating the window by publication time instead would
		// move the label by however long the endpoint reads took.
		snapshot.scheduled_end_realtime = snapshot.published_realtime;
		timespec_add_ns(&snapshot.scheduled_end_realtime,
			-collector_timespec_delta_ns(window_end, &snapshot.published));
	}

	if (endpoint_result == COLLECTOR_READ_FATAL) {
		snapshot.fatal = true;
		snapshot.fatal_read_result = COLLECTOR_READ_FATAL;
	}

	pthread_mutex_lock(&collector->mutex);
	snapshot.generation = collector->snapshot.generation + 1;
	collector->snapshot = snapshot;
	pthread_cond_broadcast(&collector->changed);
	pthread_mutex_unlock(&collector->mutex);

	return endpoint_result;
}

static void publish_fatal(struct collector *collector, int fatal_read_result) {
	pthread_mutex_lock(&collector->mutex);
	collector->snapshot.fatal = true;
	collector->snapshot.fatal_read_result = fatal_read_result;
	pthread_cond_broadcast(&collector->changed);
	pthread_mutex_unlock(&collector->mutex);
}

static bool stop_requested(struct collector *collector) {
	bool requested;

	pthread_mutex_lock(&collector->mutex);
	requested = collector->stop_requested;
	pthread_mutex_unlock(&collector->mutex);

	return requested;
}

static void mark_finished(struct collector *collector) {
	pthread_mutex_lock(&collector->mutex);
	collector->finished = true;
	pthread_cond_broadcast(&collector->changed);
	pthread_mutex_unlock(&collector->mutex);
}

// splitmix64, whose whole state is one 64-bit word, so the worker draws its own
// offsets without a lock and without a libc generator whose state other threads
// share.  A fixed seed reproduces a run exactly.
static uint64_t dither_next(uint64_t *state) {
	uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;

	return z ^ (z >> 31);
}

bool collector_dither_uniform_below(uint64_t *state, uint64_t bound,
		uint64_t *value) {
	uint64_t candidate;
	uint64_t threshold;

	if (!state || !bound || !value)
		return false;

	// The rejected prefix has 2^64 modulo bound elements.  The remaining
	// generator range contains an integral number of copies of every residue.
	threshold = (UINT64_C(0) - bound) % bound;
	do {
		candidate = dither_next(state);
	} while (candidate < threshold);

	*value = candidate % bound;
	return true;
}

// The slot grid for the window in progress.  Slot `slot` is the next one due,
// `base` is its exact grid point, and `deadline` is where inside the slot the
// sample is taken.  The grid advances by 1e9/ticks nanoseconds carrying the
// remainder, so a rate that does not divide a second exactly still lands exactly
// one second later after `ticks` slots; repeatedly adding a truncated period
// would drift instead.  The carry returns to zero after `ticks` slots, so slot
// `nominal_slots` coincides exactly with `window_end` and the grid never
// diverges from the window boundary.
//
// The dither offset lies in [0, exact slot width), so
// `base <= deadline < next base` and the deadlines stay strictly increasing.
// Slot membership follows `base` alone, which is what keeps a dithered run
// comparable with an exact one.
struct collector_schedule {
	struct timespec window_start;
	struct timespec window_end;
	struct timespec base;
	struct timespec deadline;
	int64_t remainder;
	uint64_t slot;

	uint64_t dither_state;
	bool dither_enabled;
};

// Places the sample point for the slot identified by `base`.
static bool schedule_arm(struct collector_schedule *schedule, uint32_t ticks,
		int64_t period_quotient, int64_t period_remainder) {
	schedule->deadline = schedule->base;

	if (schedule->dither_enabled) {
		uint64_t offset;
		uint64_t slot_width = (uint64_t) period_quotient;

		if (schedule->remainder + period_remainder >= (int64_t) ticks)
			slot_width++;
		if (!collector_dither_uniform_below(&schedule->dither_state,
				slot_width, &offset))
			return false;
		timespec_add_ns(&schedule->deadline, (int64_t) offset);
	}

	return true;
}

static bool schedule_step(struct collector_schedule *schedule, uint32_t ticks,
		int64_t period_quotient, int64_t period_remainder) {
	schedule->remainder += period_remainder;
	timespec_add_ns(&schedule->base, period_quotient);

	if (schedule->remainder >= (int64_t) ticks) {
		schedule->remainder -= (int64_t) ticks;
		timespec_add_ns(&schedule->base, 1);
	}

	schedule->slot++;
	return schedule_arm(schedule, ticks, period_quotient, period_remainder);
}

// The grid point one slot later, without mutating the grid, so the caller can
// test whether the slot it holds is still the due one.  The test reads the grid
// rather than the sample point, because a slot ends where the next one begins
// whatever the dither did inside it.
static struct timespec schedule_next_base(const struct collector_schedule *schedule,
		uint32_t ticks, int64_t period_quotient, int64_t period_remainder) {
	struct timespec next = schedule->base;

	timespec_add_ns(&next, period_quotient);

	if (schedule->remainder + period_remainder >= (int64_t) ticks)
		timespec_add_ns(&next, 1);

	return next;
}

// Publishes and opens the next window once the grid reaches the slot count,
// and does nothing before that.  Returns COLLECTOR_READ_FATAL when an endpoint
// read reported the device gone.
static int schedule_complete_window(struct collector *collector,
		struct collector_accumulator *accumulator,
		struct collector_schedule *schedule, uint64_t nominal_slots) {
	int result;

	if (schedule->slot < nominal_slots)
		return COLLECTOR_READ_OK;

	result = publish(collector, accumulator, &schedule->window_start,
		&schedule->window_end, nominal_slots);

	memset(accumulator, 0, sizeof(*accumulator));

	schedule->window_start = schedule->window_end;
	timespec_add_ns(&schedule->window_end,
		(int64_t) collector->config.dumpinterval * NS_PER_SEC);
	schedule->slot = 0;
	schedule->remainder = 0;

	return result;
}

static void *collector_worker(void *argument) {
	struct collector *collector = argument;

	const uint32_t ticks = collector->config.ticks;
	const uint64_t nominal_slots = (uint64_t) ticks * collector->config.dumpinterval;
	const int64_t period_quotient = NS_PER_SEC / ticks;
	const int64_t period_remainder = NS_PER_SEC % ticks;

	struct collector_accumulator accumulator;
	struct collector_schedule schedule;
	struct timespec now, after;

	memset(&accumulator, 0, sizeof(accumulator));
	memset(&schedule, 0, sizeof(schedule));

	if (collector->clock.now(collector->clock.context, &schedule.window_start)) {
		publish_fatal(collector, COLLECTOR_READ_FATAL);
		mark_finished(collector);
		return NULL;
	}

	schedule.base = schedule.window_start;
	schedule.dither_state = collector->config.dither_seed;
	schedule.dither_enabled = collector->config.dither_seed != 0;
	if (!schedule_arm(&schedule, ticks, period_quotient, period_remainder))
		goto fatal;

	schedule.window_end = schedule.window_start;
	timespec_add_ns(&schedule.window_end,
		(int64_t) collector->config.dumpinterval * NS_PER_SEC);

	while (!stop_requested(collector)) {
		if (collector->clock.wait_until(collector->clock.context, &schedule.deadline))
			goto fatal;

		if (stop_requested(collector))
			break;

		if (collector->clock.now(collector->clock.context, &now))
			goto fatal;

		{
			const int64_t lateness =
				collector_timespec_delta_ns(&schedule.deadline, &now);

			if (lateness > 0) {
				accumulator.late_wakeups++;
				if ((uint64_t) lateness > accumulator.max_lateness_ns)
					accumulator.max_lateness_ns = (uint64_t) lateness;
			}
		}

		// Give up every slot the wake-up already overran, and publish each
		// window whose boundary it crossed before reading.  The post-read
		// timestamp prevents a measurement from entering a window that ended
		// before the read began.  Those windows publish with no attempt of
		// their own.
		for (;;) {
			const struct timespec next = schedule_next_base(&schedule,
				ticks, period_quotient, period_remainder);

			if (!timespec_reached(&next, &now))
				break;

			accumulator.missed_slots++;
			if (!schedule_step(&schedule, ticks, period_quotient,
					period_remainder))
				goto fatal;

			if (schedule_complete_window(collector, &accumulator, &schedule,
					nominal_slots) == COLLECTOR_READ_FATAL)
				goto fatal;
		}

		// Exactly one read per wake-up.  A burst of catch-up reads would bias
		// the duty estimate toward whatever the stall interrupted and would
		// concentrate BAR traffic the hazard policy avoids.
		if (sample_once(collector, &accumulator))
			goto fatal;

		accumulator.attempted_slots++;

		if (accumulator.fatal_read_result == COLLECTOR_READ_FATAL)
			goto fatal;

		if (collector->clock.now(collector->clock.context, &after))
			goto fatal;

		if (!schedule_step(&schedule, ticks, period_quotient,
				period_remainder))
			goto fatal;

		if (schedule_complete_window(collector, &accumulator, &schedule,
				nominal_slots) == COLLECTOR_READ_FATAL)
			goto fatal;

		// A read that outlasted its own period leaves later deadlines already
		// behind.  Discarding the overrun deadlines returns the worker to the grid
		// instead of letting wait_until fall through and start another read
		// immediately for as long as the device stays slow.  The test reads the
		// sample point rather than the grid point: a slot whose sample point is
		// still ahead is due, and giving it up would charge the dither for a
		// miss the device did not cause.
		while (timespec_reached(&schedule.deadline, &after)) {
			accumulator.missed_slots++;
			if (!schedule_step(&schedule, ticks, period_quotient,
					period_remainder))
				goto fatal;

			if (schedule_complete_window(collector, &accumulator, &schedule,
					nominal_slots) == COLLECTOR_READ_FATAL)
				goto fatal;
		}
	}

	mark_finished(collector);
	return NULL;

fatal:
	publish_fatal(collector, COLLECTOR_READ_FATAL);
	mark_finished(collector);
	return NULL;
}

// Lifecycle.

int collector_init(struct collector *collector,
		const struct collector_config *config,
		const struct collector_backend *backend,
		const struct engine_masks *masks,
		const struct collector_clock *clock) {
	pthread_condattr_t attr;

	if (!config->ticks || !config->dumpinterval)
		return -1;
	if (config->ticks > NS_PER_SEC)
		return -2;

	// The slot count per window is the loop bound and the coverage
	// denominator, so a product that exceeds what the schedule can carry is
	// rejected by collector_init rather than wrapping later.
	if ((uint64_t) config->ticks * config->dumpinterval > UINT32_MAX)
		return -3;

	// The worker calls both on every wake-up, so a null one faults on the
	// first iteration rather than at construction.
	if (!clock->now || !clock->wait_until)
		return -4;

	// A capability asserts that its register is readable, which makes the
	// callback that reads it part of the assertion.
	if (((backend->capabilities & COLLECTOR_CAP_STATUS) && !backend->read_status) ||
		((backend->capabilities & COLLECTOR_CAP_UVD) && !backend->read_uvd_status) ||
		((backend->capabilities & COLLECTOR_CAP_VCE) && !backend->read_vce_status) ||
		((backend->capabilities & COLLECTOR_CAP_SCLK) && !backend->read_sclk) ||
		((backend->capabilities & COLLECTOR_CAP_MCLK) && !backend->read_mclk) ||
		((backend->capabilities & COLLECTOR_CAP_VRAM) && !backend->read_vram) ||
		((backend->capabilities & COLLECTOR_CAP_GTT) && !backend->read_gtt))
		return -5;

	memset(collector, 0, sizeof(*collector));
	collector->config = *config;
	collector->backend = *backend;
	collector->masks = *masks;
	collector->clock = *clock;
	collector->join_thread = pthread_join;

	if (pthread_mutex_init(&collector->mutex, NULL))
		return -6;

	if (pthread_condattr_init(&attr)) {
		pthread_mutex_destroy(&collector->mutex);
		return -7;
	}

	if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) ||
		pthread_cond_init(&collector->changed, &attr)) {
		pthread_condattr_destroy(&attr);
		pthread_mutex_destroy(&collector->mutex);
		return -8;
	}

	pthread_condattr_destroy(&attr);
	collector->initialized = true;
	return 0;
}

int collector_start(struct collector *collector) {
	if (!collector->initialized)
		return -1;
	if (collector->thread_started || collector->finished ||
		collector->stop_requested)
		return -2;

	if (pthread_create(&collector->thread, NULL, collector_worker, collector))
		return -3;

	collector->thread_started = true;
	return 0;
}

int collector_wait_next(struct collector *collector, uint64_t after_generation,
		const struct timespec *abs_timeout,
		struct collector_snapshot *out) {
	int result = COLLECTOR_WAIT_TIMEOUT;

	pthread_mutex_lock(&collector->mutex);

	for (;;) {
		if (collector->snapshot.generation > after_generation) {
			*out = collector->snapshot;
			result = COLLECTOR_WAIT_SNAPSHOT;
			break;
		}

		// A fatal worker publishes no further generation, so a consumer
		// that kept waiting would wait forever.
		if (collector->snapshot.fatal) {
			*out = collector->snapshot;
			result = COLLECTOR_WAIT_FATAL;
			break;
		}

		if (collector->finished || collector->stop_requested) {
			*out = collector->snapshot;
			result = COLLECTOR_WAIT_FINISHED;
			break;
		}

		if (abs_timeout) {
			const int wait_result = pthread_cond_timedwait(&collector->changed,
				&collector->mutex, abs_timeout);

			if (wait_result == ETIMEDOUT) {
				result = COLLECTOR_WAIT_TIMEOUT;
				break;
			}
			if (wait_result) {
				result = COLLECTOR_WAIT_ERROR;
				break;
			}
		} else {
			if (pthread_cond_wait(&collector->changed, &collector->mutex)) {
				result = COLLECTOR_WAIT_ERROR;
				break;
			}
		}
	}

	pthread_mutex_unlock(&collector->mutex);
	return result;
}

int collector_wait_next_contiguous(struct collector *collector,
		uint64_t after_generation, const struct timespec *abs_timeout,
		struct collector_snapshot *out) {
	if (after_generation == UINT64_MAX) {
		pthread_mutex_lock(&collector->mutex);
		*out = collector->snapshot;
		pthread_mutex_unlock(&collector->mutex);
		return COLLECTOR_WAIT_GAP;
	}

	const int result = collector_wait_next(collector, after_generation,
		abs_timeout, out);

	if (result == COLLECTOR_WAIT_SNAPSHOT &&
		out->generation != after_generation + 1)
		return COLLECTOR_WAIT_GAP;

	return result;
}

bool collector_peek(struct collector *collector, struct collector_snapshot *out) {
	bool published;

	pthread_mutex_lock(&collector->mutex);
	published = collector->snapshot.generation > 0 || collector->snapshot.fatal;
	if (published)
		*out = collector->snapshot;
	pthread_mutex_unlock(&collector->mutex);

	return published;
}

void collector_request_stop(struct collector *collector) {
	pthread_mutex_lock(&collector->mutex);
	collector->stop_requested = true;
	pthread_cond_broadcast(&collector->changed);
	pthread_mutex_unlock(&collector->mutex);

	// The worker may be waiting on a deadline a whole window away, so the
	// clock is interrupted as well as the predicate set.
	if (collector->clock.wake)
		collector->clock.wake(collector->clock.context);
}

int collector_join(struct collector *collector) {
	if (!collector->thread_started)
		return 0;

	if (!collector->join_thread || collector->join_thread(collector->thread, NULL))
		return -1;

	collector->thread_started = false;
	return 0;
}

int collector_destroy(struct collector *collector) {
	if (!collector->initialized)
		return 0;
	if (collector->thread_started)
		return -1;

	if (pthread_cond_destroy(&collector->changed))
		return -2;
	if (pthread_mutex_destroy(&collector->mutex)) {
		// The condition variable is already gone, so retrying destruction would
		// operate on a destroyed object.  The caller receives the failure while
		// the lifecycle state records that the collector cannot be reused.
		collector->initialized = false;
		return -2;
	}
	collector->initialized = false;
	return 0;
}

double collector_lane_fraction(const struct collector_snapshot *snapshot,
		enum collector_lane lane) {
	uint64_t valid;

	if (lane == COLLECTOR_LANE_UVD)
		valid = snapshot->uvd.valid;
	else if (lane == COLLECTOR_LANE_VCE0)
		valid = snapshot->vce.valid;
	else
		valid = snapshot->status.valid;

	// Zero valid reads leaves the duty undefined.  Returning 0.0 would render
	// an idle block and a dead read path identically.
	if (!valid)
		return NAN;

	return (double) snapshot->lane_busy[lane] / (double) valid;
}

bool collector_lane_missing_data_bounds(const struct collector_snapshot *snapshot,
		enum collector_lane lane, struct collector_lane_bounds *bounds) {
	uint64_t valid;
	uint64_t busy;

	if (!snapshot || !bounds || lane < 0 || lane >= COLLECTOR_LANE_COUNT ||
		!snapshot->nominal_slots)
		return false;

	if (lane == COLLECTOR_LANE_UVD)
		valid = snapshot->uvd.valid;
	else if (lane == COLLECTOR_LANE_VCE0)
		valid = snapshot->vce.valid;
	else
		valid = snapshot->status.valid;

	busy = snapshot->lane_busy[lane];
	if (valid > snapshot->nominal_slots || busy > valid)
		return false;

	bounds->busy = busy;
	bounds->valid = valid;
	bounds->nominal = snapshot->nominal_slots;
	bounds->conditional_fraction = valid ?
		(double) busy / (double) valid : NAN;
	bounds->unconditional_lower =
		(double) busy / (double) snapshot->nominal_slots;
	bounds->unconditional_upper =
		(double) (busy + snapshot->nominal_slots - valid) /
		(double) snapshot->nominal_slots;

	return true;
}

double collector_status_coverage(const struct collector_snapshot *snapshot) {
	if (!snapshot->nominal_slots)
		return NAN;

	return (double) snapshot->status.valid / (double) snapshot->nominal_slots;
}
