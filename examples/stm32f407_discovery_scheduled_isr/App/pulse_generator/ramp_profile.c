#include "ramp_profile.h"

#include <math.h>
#include <stddef.h>

/* Instant of pulse k of the acceleration, in ticks from its start:
   k = a * t^2 / 2 solved for t. */
static uint32_t accel_instant(const ramp_profile_t *ramp, uint32_t k)
{
    return (uint32_t)(ramp->accel_scale * sqrtf((float)k) + 0.5f);
}

/* Instant of pulse j of the cruise, in ticks from its start, rounded to the
   nearest tick. 64-bit because j * tick_hz overflows 32 bits within seconds. */
static uint32_t cruise_instant(const ramp_profile_t *ramp, uint32_t j)
{
    uint64_t num = (uint64_t)j * ramp->tick_hz * 2u + ramp->cruise_hz;

    return (uint32_t)(num / (2u * (uint64_t)ramp->cruise_hz));
}

bool ramp_profile_init(ramp_profile_t *ramp,
                       uint32_t tick_hz,
                       uint32_t accel_hz_s,
                       uint32_t cruise_hz,
                       uint32_t cruise_pulses)
{
    if (ramp == NULL || tick_hz == 0 || accel_hz_s == 0 || cruise_hz == 0) {
        return false;
    }

    /* v^2 = 2 * a * k: pulses needed to reach the cruise rate, rounded. */
    uint64_t accel_pulses = ((uint64_t)cruise_hz * cruise_hz + accel_hz_s) / (2u * (uint64_t)accel_hz_s);
    if (accel_pulses == 0 || accel_pulses > UINT32_MAX / 4u) {
        return false;
    }

    ramp->tick_hz = tick_hz;
    ramp->cruise_hz = cruise_hz;
    ramp->accel_scale = (float)tick_hz * sqrtf(2.0f / (float)accel_hz_s);
    ramp->accel_pulses = (uint32_t)accel_pulses;
    ramp->cruise_pulses = cruise_pulses;
    ramp->emitted = 0;

    return true;
}

bool ramp_profile_next(ramp_profile_t *ramp, uint32_t *interval)
{
    uint32_t n = ramp->accel_pulses;
    uint32_t i = ramp->emitted;

    if (i >= ramp_profile_total_pulses(ramp)) {
        return false;
    }

    if (i < n) {
        uint32_t k = i + 1u;
        *interval = accel_instant(ramp, k) - accel_instant(ramp, k - 1u);
    } else if (i < n + ramp->cruise_pulses) {
        uint32_t j = i - n + 1u;
        *interval = cruise_instant(ramp, j) - cruise_instant(ramp, j - 1u);
    } else {
        /* The acceleration played backwards: its intervals in reverse order,
           ending with the long one it started from. */
        uint32_t k = ramp_profile_total_pulses(ramp) - i;
        *interval = accel_instant(ramp, k) - accel_instant(ramp, k - 1u);
    }

    ramp->emitted++;
    return true;
}

uint32_t ramp_profile_total_pulses(const ramp_profile_t *ramp)
{
    return 2u * ramp->accel_pulses + ramp->cruise_pulses;
}
