#ifndef PULSE_GENERATOR_PLATFORM_H
#define PULSE_GENERATOR_PLATFORM_H

#include "main.h"

#include "pulse_generator.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One Output Compare channel, driven as a free-running counter at its full
   width (16 or 32 bits, detected from the timer instance) with the library
   writing an absolute compare value on every match.
   Pass a pointer to one of these as pulse_generator_config_t::hw.

   Everything below the first two fields is derived by
   stm32f4_pg_hw_init(), so the hooks stay plain register accesses with no
   per-channel branching in the interrupt path. */
typedef struct {
    TIM_HandleTypeDef *htim;
    uint32_t           channel;     /* TIM_CHANNEL_1 .. TIM_CHANNEL_4 */

    volatile uint32_t *ccr;         /* this channel's compare register */
    volatile uint32_t *ccmr;        /* CCMR1 for channels 1-2, CCMR2 for 3-4 */
    uint32_t           ocm_mask;    /* the OCxM field within *ccmr */
    uint32_t           ocm_shift;   /* bits to shift a TIM_OCMODE_* value by */
    uint32_t           cc_flag;     /* this channel's TIM_FLAG_CCx */
    uint32_t           counter_max;
    uint32_t           tick_hz;     /* counting frequency, after the prescaler */
} stm32f4_pg_hw_t;

/* Binds one channel of a timer CubeMX has already set up. Reads the timer's
   instance and prescaler, so it must run after MX_TIMx_Init().
   @return PULSE_GENERATOR_ERROR_INVALID_PARAM for a null argument, a channel
           the timer does not have, or a timer whose clock this platform
           cannot resolve — a failure worth catching at startup, rather than
           a tick rate of 0 Hz that would make every frequency look out of
           range later. */
pulse_generator_status_t stm32f4_pg_hw_init(stm32f4_pg_hw_t *hw,
                                            TIM_HandleTypeDef *htim,
                                            uint32_t channel);

/* Shared by every channel and every timer: all the per-output state lives in
   stm32f4_pg_hw_t, so one table in flash serves as many axes as needed. */
extern const pulse_generator_ops_t g_stm32f4_pg_ops;

#ifdef __cplusplus
}
#endif

#endif /* PULSE_GENERATOR_PLATFORM_H */
