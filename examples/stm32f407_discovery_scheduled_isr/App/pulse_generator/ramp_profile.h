#ifndef RAMP_PROFILE_H
#define RAMP_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Trapezoidal constant-acceleration profile, produced one interval at a
   time: accelerate from rest to the cruise rate, hold it, then decelerate
   back to rest along the mirror image of the acceleration.

   Every interval is the difference of two absolute instants rounded to
   ticks, never a rounded interval on its own, so the rounding error does not
   add up along the movement.

   Plain arithmetic: no HAL and no pulse_generator dependency, so it can be
   checked on a host. */
typedef struct {
    uint32_t tick_hz;
    uint32_t cruise_hz;
    float    accel_scale;   /* tick_hz * sqrt(2 / accel): pulse k of the
                               acceleration lands at accel_scale * sqrt(k) */
    uint32_t accel_pulses;  /* pulses to reach cruise_hz; deceleration takes as many */
    uint32_t cruise_pulses;
    uint32_t emitted;       /* intervals handed out so far */
} ramp_profile_t;

/* @param tick_hz       Counting frequency of the timer the intervals are for.
   @param accel_hz_s    Acceleration, in pulses per second squared.
   @param cruise_hz     Cruise rate, in pulses per second.
   @param cruise_pulses Pulses held at cruise_hz, between the two ramps.
   @return false if any rate is 0, or if the ramp would be shorter than one
           pulse. */
bool ramp_profile_init(ramp_profile_t *ramp,
                       uint32_t tick_hz,
                       uint32_t accel_hz_s,
                       uint32_t cruise_hz,
                       uint32_t cruise_pulses);

/* Hands out the next interval, in ticks from the previous pulse (the first
   one from the start of the movement).
   @return false once the whole profile has been handed out. */
bool ramp_profile_next(ramp_profile_t *ramp, uint32_t *interval);

/* Total pulses in the profile: both ramps plus the cruise. */
uint32_t ramp_profile_total_pulses(const ramp_profile_t *ramp);

#ifdef __cplusplus
}
#endif

#endif /* RAMP_PROFILE_H */
