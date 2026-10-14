#ifndef PULSE_GENERATOR_UTIL_H
#define PULSE_GENERATOR_UTIL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Optional helpers for building the intervals a SCHEDULED queue takes.
 *
 * Plain tick arithmetic on top of pulse_generator_queue_events(): nothing
 * here touches an instance or the platform, and the core library does not
 * depend on this header. The tick rate is always passed in explicitly — the
 * same value the platform's get_tick_hz hook reports.
 *
 * Every conversion rounds to the nearest tick (halves up) and returns false,
 * leaving its output untouched, when an argument is invalid or the result
 * does not fit in 32 bits.
 */

/* --- Unit conversions --- */

/**
 * @brief Convert a duration in µs to timer ticks.
 * @param tick_hz Counter frequency in Hz, as get_tick_hz reports it. Non-zero.
 * @param us      Duration in µs.
 * @param ticks   Receives the duration in ticks, rounded to the nearest.
 * @return true on success; false if ticks is NULL, tick_hz is 0, or the
 *         result does not fit in a uint32_t.
 * @note Rounding each interval on its own lets the errors add up along a
 *       sequence: for a sequence of instants use pulse_generator_timeline_t.
 */
bool pulse_generator_us_to_ticks(uint32_t tick_hz, uint32_t us, uint32_t *ticks);

/**
 * @brief Convert a duration in ns to timer ticks.
 * @param tick_hz Counter frequency in Hz. Non-zero.
 * @param ns      Duration in ns.
 * @param ticks   Receives the duration in ticks, rounded to the nearest.
 * @return Same as pulse_generator_us_to_ticks().
 */
bool pulse_generator_ns_to_ticks(uint32_t tick_hz, uint32_t ns, uint32_t *ticks);

/**
 * @brief Convert a duration in timer ticks to µs.
 * @param tick_hz Counter frequency in Hz. Non-zero.
 * @param ticks   Duration in ticks.
 * @param us      Receives the duration in µs, rounded to the nearest.
 * @return true on success; false if us is NULL, tick_hz is 0, or the result
 *         does not fit in a uint32_t.
 */
bool pulse_generator_ticks_to_us(uint32_t tick_hz, uint32_t ticks, uint32_t *us);

/**
 * @brief Convert a pulse frequency to the interval between pulses, in ticks.
 * @param tick_hz      Counter frequency in Hz. Non-zero.
 * @param frequency_hz Pulse frequency in Hz. Non-zero.
 * @param ticks        Receives tick_hz / frequency_hz, rounded to the nearest.
 * @return true on success; false if ticks is NULL, either frequency is 0, or
 *         the interval rounds to 0 ticks.
 * @note For building SCHEDULED intervals. FIXED_COUNT and CONTINUOUS do not
 *       need it: they carry the remainder from one period to the next, so
 *       their average frequency is exact. A train built by repeating this
 *       one rounded interval is not; spread it with pulse_generator_spread_t.
 */
bool pulse_generator_hz_to_ticks(uint32_t tick_hz, uint32_t frequency_hz, uint32_t *ticks);

/* --- Spreading n pulses over a span --- */

/**
 * @brief Iterator that spreads a number of pulses evenly over a span of
 *        ticks, as the intervals pulse_generator_queue_events() takes.
 *
 * Pulse k (1..pulses) rises floor(k * ticks / pulses) ticks after the start
 * of the span, so the intervals add up to exactly `ticks` and differ from one
 * another by at most one tick: chained spans never drift. One division at
 * init, none per interval.
 *
 * Fields are private: use the pulse_generator_spread_* functions only.
 * Defined here so the caller can allocate it without the heap.
 *
 * Typical streaming use, resuming where the queue filled up:
 * @code
 *   uint32_t interval;
 *   bool held = false; // an interval the queue did not take yet
 *   while (held || pulse_generator_spread_next(&spread, &interval)) {
 *       held = true;
 *       if (pulse_generator_queue_events(&pg, &interval, 1) != 1) break;
 *       held = false;
 *   }
 * @endcode
 */
typedef struct {
    uint32_t base;      /* ticks / pulses */
    uint32_t remainder; /* ticks % pulses */
    uint32_t pulses;    /* pulses in the span */
    uint32_t error;     /* remainder accumulated so far, below pulses */
    uint32_t left;      /* intervals not returned yet */
} pulse_generator_spread_t;

/**
 * @brief Start spreading `pulses` pulses over `ticks` ticks.
 * @param spread Iterator to initialize.
 * @param pulses Pulses in the span. Non-zero.
 * @param ticks  Length of the span, from its start to the last pulse's rise.
 *               At least `pulses`, so that no interval is 0.
 * @return true on success; false on a NULL iterator, pulses 0 or
 *         ticks < pulses, with the iterator untouched.
 * @note Says nothing about the queue's own limits: an interval shorter than
 *       the minimum entry is still rejected by pulse_generator_queue_events().
 */
bool pulse_generator_spread_init(pulse_generator_spread_t *spread, uint32_t pulses, uint32_t ticks);

/**
 * @brief Take the next interval of the span.
 * @param spread   Iterator.
 * @param interval Receives the interval, in ticks.
 * @return true if an interval was produced; false once all of them have
 *         been, or on a NULL argument.
 */
bool pulse_generator_spread_next(pulse_generator_spread_t *spread, uint32_t *interval);

/**
 * @brief Report how many intervals the span has left.
 * @param spread Iterator.
 * @return Intervals not taken yet; 0 if spread is NULL.
 */
uint32_t pulse_generator_spread_remaining(const pulse_generator_spread_t *spread);

/* --- Intervals from absolute instants --- */

/**
 * @brief Turns absolute pulse instants into intervals without drift.
 *
 * Each instant is rounded to the nearest tick on its own, and the interval
 * is the difference from the previous rounded instant, so rounding errors
 * never add up: after any number of pulses, the intervals returned add up to
 * the last instant, rounded. This is the recipe the queue asks for (see
 * pulse_generator_queue_events()), for a producer that knows when each pulse
 * should happen, e.g. from a motion profile.
 *
 * Fields are private: use the pulse_generator_timeline_* functions only.
 */
typedef struct {
    uint32_t tick_hz;   /* counter frequency */
    uint64_t last_tick; /* previous instant, rounded to ticks */
} pulse_generator_timeline_t;

/**
 * @brief Start a timeline at instant 0, the start of the movement.
 * @param timeline Timeline to initialize.
 * @param tick_hz  Counter frequency in Hz. Non-zero.
 * @return true on success; false on a NULL timeline or tick_hz 0.
 */
bool pulse_generator_timeline_init(pulse_generator_timeline_t *timeline, uint32_t tick_hz);

/**
 * @brief Take the interval to the next pulse, given its absolute instant.
 * @param timeline   Timeline.
 * @param instant_ns Instant of the pulse's rise, in ns from the start of the
 *                   movement. Need not be a whole number of ticks.
 * @param interval   Receives the interval from the previous pulse (from the
 *                   start, for the first one), in ticks.
 * @return true on success; false on a NULL argument, an instant that rounds
 *         to the previous one's tick or earlier, or an interval that does
 *         not fit in a uint32_t. On failure the timeline is left untouched,
 *         so the next instant is still measured from the last one accepted.
 */
bool pulse_generator_timeline_next(pulse_generator_timeline_t *timeline, uint64_t instant_ns, uint32_t *interval);

#ifdef __cplusplus
}
#endif

#endif /* PULSE_GENERATOR_UTIL_H */
