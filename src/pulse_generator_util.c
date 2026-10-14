#include "pulse_generator_util.h"

#include <stddef.h>

#define US_PER_S 1000000u
#define NS_PER_S 1000000000u

/* --- Common --- */

/* value * mul / div, rounded to the nearest (halves up). Cannot overflow:
   the product of two 32-bit values leaves more than 2^32 of headroom for
   the half added. */
static uint64_t scale_round(uint32_t value, uint32_t mul, uint32_t div)
{
    return ((uint64_t)value * mul + div / 2u) / div;
}

static bool store_u32(uint64_t value, uint32_t *out)
{
    if (value > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

/* --- Unit conversions --- */

bool pulse_generator_us_to_ticks(uint32_t tick_hz, uint32_t us, uint32_t *ticks)
{
    if (ticks == NULL || tick_hz == 0u) {
        return false;
    }
    return store_u32(scale_round(us, tick_hz, US_PER_S), ticks);
}

bool pulse_generator_ns_to_ticks(uint32_t tick_hz, uint32_t ns, uint32_t *ticks)
{
    if (ticks == NULL || tick_hz == 0u) {
        return false;
    }
    return store_u32(scale_round(ns, tick_hz, NS_PER_S), ticks);
}

bool pulse_generator_ticks_to_us(uint32_t tick_hz, uint32_t ticks, uint32_t *us)
{
    if (us == NULL || tick_hz == 0u) {
        return false;
    }
    return store_u32(scale_round(ticks, US_PER_S, tick_hz), us);
}

bool pulse_generator_hz_to_ticks(uint32_t tick_hz, uint32_t frequency_hz, uint32_t *ticks)
{
    if (ticks == NULL || tick_hz == 0u || frequency_hz == 0u) {
        return false;
    }

    uint64_t interval = scale_round(tick_hz, 1u, frequency_hz);
    if (interval == 0u) {
        return false;
    }
    return store_u32(interval, ticks);
}

/* --- Spread --- */

bool pulse_generator_spread_init(pulse_generator_spread_t *spread, uint32_t pulses, uint32_t ticks)
{
    if (spread == NULL || pulses == 0u || ticks < pulses) {
        return false;
    }

    *spread = (pulse_generator_spread_t){
        .base      = ticks / pulses,
        .remainder = ticks % pulses,
        .pulses    = pulses,
        .error     = 0u,
        .left      = pulses,
    };
    return true;
}

bool pulse_generator_spread_next(pulse_generator_spread_t *spread, uint32_t *interval)
{
    if (spread == NULL || interval == NULL || spread->left == 0u) {
        return false;
    }

    /* After k intervals, error is k * remainder % pulses and the extra ticks
       handed out are k * remainder / pulses, so the sum is exactly
       floor(k * ticks / pulses). The sum below cannot overflow: error is
       below pulses, and remainder below ticks - pulses + 1. */
    uint32_t value = spread->base;
    spread->error += spread->remainder;
    if (spread->error >= spread->pulses) {
        spread->error -= spread->pulses;
        value++;
    }

    spread->left--;
    *interval = value;
    return true;
}

uint32_t pulse_generator_spread_remaining(const pulse_generator_spread_t *spread)
{
    return spread == NULL ? 0u : spread->left;
}

/* --- Timeline --- */

/* ns * tick_hz / 1e9, rounded to the nearest. Split into whole seconds and
   the rest, so that the rest's product stays below 1e9 * 2^32 < 2^62; the
   rest contributes at most tick_hz ticks, which the whole seconds must
   leave room for. */
static bool ns_to_tick(uint32_t tick_hz, uint64_t ns, uint64_t *tick)
{
    uint64_t seconds = ns / NS_PER_S;
    uint64_t rest    = ns % NS_PER_S;

    if (seconds > (UINT64_MAX - tick_hz) / tick_hz) {
        return false;
    }
    *tick = seconds * tick_hz + (rest * tick_hz + NS_PER_S / 2u) / NS_PER_S;
    return true;
}

bool pulse_generator_timeline_init(pulse_generator_timeline_t *timeline, uint32_t tick_hz)
{
    if (timeline == NULL || tick_hz == 0u) {
        return false;
    }

    *timeline = (pulse_generator_timeline_t){ .tick_hz = tick_hz, .last_tick = 0u };
    return true;
}

bool pulse_generator_timeline_next(pulse_generator_timeline_t *timeline, uint64_t instant_ns, uint32_t *interval)
{
    if (timeline == NULL || interval == NULL) {
        return false;
    }

    uint64_t tick;
    if (!ns_to_tick(timeline->tick_hz, instant_ns, &tick)
        || tick <= timeline->last_tick
        || tick - timeline->last_tick > UINT32_MAX) {
        return false;
    }

    *interval = (uint32_t)(tick - timeline->last_tick);
    timeline->last_tick = tick;
    return true;
}
