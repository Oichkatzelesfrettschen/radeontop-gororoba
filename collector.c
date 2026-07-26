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

	pthread_mutex_lock(&clock->mutex);
	while (!clock->woken) {
		if (pthread_cond_timedwait(&clock->cond, &clock->mutex, deadline) == ETIMEDOUT)
			break;
	}
	pthread_mutex_unlock(&clock->mutex);

	return 0;
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

static void sample_once(struct collector *collector,
		struct collector_accumulator *accumulator) {
	const struct collector_backend *backend = &collector->backend;
	struct timespec before, after;
	uint32_t value;
	int result;

	collector->clock.now(collector->clock.context, &before);

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
	}

	if (backend->capabilities & COLLECTOR_CAP_UVD) {
		value = 0;
		result = backend->read_uvd_status(backend->context, &value);
		note_read(accumulator, &accumulator->uvd, result);
		if (result == COLLECTOR_READ_OK &&
			collector->masks.lane[COLLECTOR_LANE_UVD] &&
			(value & collector->masks.lane[COLLECTOR_LANE_UVD]))
			accumulator->lane_busy[COLLECTOR_LANE_UVD]++;
	}

	if (backend->capabilities & COLLECTOR_CAP_VCE) {
		value = 0;
		result = backend->read_vce_status(backend->context, &value);
		note_read(accumulator, &accumulator->vce, result);
		if (result == COLLECTOR_READ_OK &&
			collector->masks.lane[COLLECTOR_LANE_VCE0] &&
			(value & collector->masks.lane[COLLECTOR_LANE_VCE0]))
			accumulator->lane_busy[COLLECTOR_LANE_VCE0]++;
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

	collector->clock.now(collector->clock.context, &after);

	{
		const int64_t latency = collector_timespec_delta_ns(&before, &after);

		if (latency > 0 && (uint64_t) latency > accumulator->max_read_latency_ns)
			accumulator->max_read_latency_ns = (uint64_t) latency;
	}
}

static void publish(struct collector *collector,
		struct collector_accumulator *accumulator,
		const struct timespec *window_start,
		const struct timespec *window_end,
		uint64_t nominal_slots) {
	const struct collector_backend *backend = &collector->backend;
	struct collector_snapshot snapshot;

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

		if (backend->read_vram(backend->context, &value) == COLLECTOR_READ_OK) {
			snapshot.vram = value;
			snapshot.vram_valid = true;
		}
	}

	if (backend->capabilities & COLLECTOR_CAP_GTT) {
		uint64_t value = 0;

		if (backend->read_gtt(backend->context, &value) == COLLECTOR_READ_OK) {
			snapshot.gtt = value;
			snapshot.gtt_valid = true;
		}
	}

	collector->clock.now(collector->clock.context, &snapshot.published);

	// The realtime stamp labels the window's scheduled end.  It is taken
	// here rather than where a consumer prints, because a blocked writer
	// would otherwise date the measurement by its own delay.
	clock_gettime(CLOCK_REALTIME, &snapshot.window_end_realtime);

	pthread_mutex_lock(&collector->mutex);
	snapshot.generation = collector->snapshot.generation + 1;
	collector->snapshot = snapshot;
	pthread_cond_broadcast(&collector->changed);
	pthread_mutex_unlock(&collector->mutex);
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

static void *collector_worker(void *argument) {
	struct collector *collector = argument;

	const uint32_t ticks = collector->config.ticks;
	const uint64_t nominal_slots = (uint64_t) ticks * collector->config.dumpinterval;

	// The slot grid advances by 1e9/ticks nanoseconds carrying the
	// remainder, so a rate that does not divide a second exactly still lands
	// exactly one second later after `ticks` slots.  Repeatedly adding a
	// truncated period would drift instead.
	const int64_t period_quotient = NS_PER_SEC / ticks;
	const int64_t period_remainder = NS_PER_SEC % ticks;

	struct collector_accumulator accumulator;
	struct timespec window_start, window_end, deadline, now;
	int64_t remainder = 0;
	uint64_t slot = 0;

	memset(&accumulator, 0, sizeof(accumulator));

	if (collector->clock.now(collector->clock.context, &window_start)) {
		publish_fatal(collector, COLLECTOR_READ_FATAL);
		mark_finished(collector);
		return NULL;
	}

	deadline = window_start;
	window_end = window_start;
	timespec_add_ns(&window_end, (int64_t) collector->config.dumpinterval * NS_PER_SEC);

	while (!stop_requested(collector)) {
		collector->clock.wait_until(collector->clock.context, &deadline);

		if (stop_requested(collector))
			break;

		if (collector->clock.now(collector->clock.context, &now))
			break;

		{
			const int64_t lateness = collector_timespec_delta_ns(&deadline, &now);

			if (lateness > 0) {
				accumulator.late_wakeups++;
				if ((uint64_t) lateness > accumulator.max_lateness_ns)
					accumulator.max_lateness_ns = (uint64_t) lateness;
			}
		}

		// Exactly one read per wake-up.  A burst of catch-up reads would
		// bias the duty estimate toward whatever the stall interrupted
		// and would concentrate BAR traffic the hazard policy avoids.
		sample_once(collector, &accumulator);
		accumulator.attempted_slots++;

		if (accumulator.fatal_read_result == COLLECTOR_READ_FATAL) {
			publish_fatal(collector, COLLECTOR_READ_FATAL);
			mark_finished(collector);
			return NULL;
		}

		// Advance to the first deadline still in the future.  Every slot
		// stepped over without a read counts as missed.
		{
			const uint64_t sampled_slot = slot;

			do {
				remainder += period_remainder;
				timespec_add_ns(&deadline, period_quotient);
				if (remainder >= (int64_t) ticks) {
					remainder -= (int64_t) ticks;
					timespec_add_ns(&deadline, 1);
				}
				slot++;
			} while (slot < nominal_slots && timespec_reached(&deadline, &now));

			accumulator.missed_slots += slot - sampled_slot - 1;
		}

		if (slot >= nominal_slots) {
			publish(collector, &accumulator, &window_start, &window_end,
				nominal_slots);

			window_start = window_end;
			timespec_add_ns(&window_end,
				(int64_t) collector->config.dumpinterval * NS_PER_SEC);
			memset(&accumulator, 0, sizeof(accumulator));
			slot = 0;
		}
	}

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

	// The slot count per window is the loop bound and the coverage
	// denominator, so a product that exceeds what the schedule can carry is
	// rejected here rather than wrapping later.
	if ((uint64_t) config->ticks * config->dumpinterval > UINT32_MAX)
		return -2;

	memset(collector, 0, sizeof(*collector));
	collector->config = *config;
	collector->backend = *backend;
	collector->masks = *masks;
	collector->clock = *clock;

	if (pthread_mutex_init(&collector->mutex, NULL))
		return -3;

	if (pthread_condattr_init(&attr)) {
		pthread_mutex_destroy(&collector->mutex);
		return -4;
	}

	if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) ||
		pthread_cond_init(&collector->changed, &attr)) {
		pthread_condattr_destroy(&attr);
		pthread_mutex_destroy(&collector->mutex);
		return -5;
	}

	pthread_condattr_destroy(&attr);
	collector->initialized = true;
	return 0;
}

int collector_start(struct collector *collector) {
	if (!collector->initialized)
		return -1;

	if (pthread_create(&collector->thread, NULL, collector_worker, collector))
		return -2;

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
			if (pthread_cond_timedwait(&collector->changed, &collector->mutex,
					abs_timeout) == ETIMEDOUT) {
				result = COLLECTOR_WAIT_TIMEOUT;
				break;
			}
		} else {
			pthread_cond_wait(&collector->changed, &collector->mutex);
		}
	}

	pthread_mutex_unlock(&collector->mutex);
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

	if (pthread_join(collector->thread, NULL))
		return -1;

	collector->thread_started = false;
	return 0;
}

void collector_destroy(struct collector *collector) {
	if (!collector->initialized)
		return;

	pthread_cond_destroy(&collector->changed);
	pthread_mutex_destroy(&collector->mutex);
	collector->initialized = false;
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

double collector_status_coverage(const struct collector_snapshot *snapshot) {
	if (!snapshot->nominal_slots)
		return NAN;

	return (double) snapshot->status.valid / (double) snapshot->nominal_slots;
}
