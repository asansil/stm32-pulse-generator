#include "ramp_profile.h"

#include <math.h>
#include <stddef.h>

#define NS_PER_S 1000000000U

/* Instant of pulse k of the acceleration, in ns from its start:
   k = a * t^2 / 2 solved for t. */
static uint64_t accel_instant_ns(const ramp_profile_t *ramp, uint32_t k)
{
    return (uint64_t)(ramp->accel_scale_ns * sqrt((double)k) + 0.5);
}

/* Instant of pulse j of the cruise, in ns from its start, rounded to the
   nearest. */
static uint64_t cruise_instant_ns(const ramp_profile_t *ramp, uint32_t j)
{
    return ((uint64_t)j * NS_PER_S + ramp->cruise_hz / 2U) / ramp->cruise_hz;
}

/* Instant of the pulse that interval i (0-based) ends at, in ns from the
   start of the movement. */
static uint64_t pulse_instant_ns(const ramp_profile_t *ramp, uint32_t i)
{
    const uint32_t n = ramp->accel_pulses;

    if (i < n) {
        return accel_instant_ns(ramp, i + 1U);
    }

    const uint64_t accel_end = accel_instant_ns(ramp, n);
    if (i < n + ramp->cruise_pulses) {
        return accel_end + cruise_instant_ns(ramp, i - n + 1U);
    }

    /* The acceleration played backwards: its intervals in reverse order,
       ending with the long one it started from. */
    const uint64_t cruise_end = accel_end + cruise_instant_ns(ramp, ramp->cruise_pulses);
    const uint32_t k = ramp_profile_total_pulses(ramp) - i - 1U;

    return cruise_end + accel_end - accel_instant_ns(ramp, k);
}

bool ramp_profile_init(ramp_profile_t *ramp,
                       uint32_t tick_hz,
                       uint32_t accel_hz_s,
                       uint32_t cruise_hz,
                       uint32_t cruise_pulses)
{
    if (ramp == NULL || accel_hz_s == 0U || cruise_hz == 0U) {
        return false;
    }

    /* v^2 = 2 * a * k: pulses needed to reach the cruise rate, rounded. */
    const uint64_t accel_pulses = ((uint64_t)cruise_hz * cruise_hz + accel_hz_s) / (2U * (uint64_t)accel_hz_s);
    if (accel_pulses == 0U || accel_pulses > UINT32_MAX / 4U) {
        return false;
    }

    /* Rejects a tick_hz of 0. */
    if (!pulse_generator_timeline_init(&ramp->timeline, tick_hz)) {
        return false;
    }

    ramp->cruise_hz = cruise_hz;
    ramp->accel_scale_ns = (double)NS_PER_S * sqrt(2.0 / (double)accel_hz_s);
    ramp->accel_pulses = (uint32_t)accel_pulses;
    ramp->cruise_pulses = cruise_pulses;
    ramp->emitted = 0U;

    return true;
}

bool ramp_profile_next(ramp_profile_t *ramp, uint32_t *interval)
{
    if (ramp->emitted >= ramp_profile_total_pulses(ramp)) {
        return false;
    }

    if (!pulse_generator_timeline_next(&ramp->timeline, pulse_instant_ns(ramp, ramp->emitted), interval)) {
        return false;
    }

    ramp->emitted++;

    return true;
}

uint32_t ramp_profile_total_pulses(const ramp_profile_t *ramp)
{
    return 2U * ramp->accel_pulses + ramp->cruise_pulses;
}
