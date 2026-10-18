#ifndef RAMP_PROFILE_H
#define RAMP_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

#include "pulse_generator_util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Trapezoidal constant-acceleration profile, produced one interval at a
   time: accelerate from rest to the cruise rate, hold it, then decelerate
   back to rest along the mirror image of the acceleration.

   Each pulse's absolute instant goes through a pulse_generator_timeline_t,
   so the rounding error does not add up along the movement.

   No HAL dependency, so it can be checked on a host. */
typedef struct {
    pulse_generator_timeline_t timeline;
    uint32_t cruise_hz;
    double   accel_scale_ns; /* 1e9 * sqrt(2 / accel): pulse k of the
                                acceleration lands accel_scale_ns * sqrt(k)
                                ns after its start. Double: the instants
                                reach ~2e9 ns, past the 2^24 a float holds
                                exactly. */
    uint32_t accel_pulses;   /* pulses to reach cruise_hz; deceleration takes as many */
    uint32_t cruise_pulses;
    uint32_t emitted;        /* intervals handed out so far */
} ramp_profile_t;

/* Starts the profile at instant 0. tick_hz is the counting frequency of the
   timer the intervals are for, accel_hz_s the acceleration in pulses per
   second squared, cruise_hz the cruise rate in pulses per second, and
   cruise_pulses the pulses held at it between the two ramps. Returns false
   if any rate is 0, or if the ramp would be shorter than one pulse. */
bool ramp_profile_init(ramp_profile_t *ramp,
                       uint32_t tick_hz,
                       uint32_t accel_hz_s,
                       uint32_t cruise_hz,
                       uint32_t cruise_pulses);

/* Hands out the next interval, in ticks from the previous pulse (the first
   one from the start of the movement). Returns false once the whole profile
   has been handed out, or if the timeline rejects an interval. */
bool ramp_profile_next(ramp_profile_t *ramp, uint32_t *interval);

/* Total pulses in the profile: both ramps plus the cruise. */
uint32_t ramp_profile_total_pulses(const ramp_profile_t *ramp);

#ifdef __cplusplus
}
#endif

#endif /* RAMP_PROFILE_H */
