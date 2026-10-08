#ifndef PULSE_GENERATOR_PLATFORM_H
#define PULSE_GENERATOR_PLATFORM_H

#include "main.h"

#include "pulse_generator.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One timer output driven by the DMA engine: PWM mode 1 with ARR and CCR
   preload, fed one entry per update event by a DMA burst from a circular
   buffer. Every entry rewrites the timer-wide ARR, so the whole timer
   belongs to this output.
   Pass a pointer to one of these as pulse_generator_config_t::hw.

   Everything below the first two fields is derived by
   stm32f4_pg_hw_init(), so the hooks stay plain register accesses with no
   per-channel branching in the interrupt path. */
typedef struct {
    TIM_HandleTypeDef *htim;
    uint32_t           channel;      /* TIM_CHANNEL_1 .. TIM_CHANNEL_4 */

    volatile uint32_t *ccr;          /* this channel's compare register */
    volatile uint32_t *ccmr;         /* CCMR1 for channels 1-2, CCMR2 for 3-4 */
    uint32_t           ocm_mask;     /* the OCxM field within *ccmr */
    uint32_t           ocm_shift;    /* bits to shift an OC1-position value by */
    uint32_t           ccer_mask;    /* this channel's CCxE bit */
    uint32_t           burst_length; /* TIM_DMABURSTLENGTH_*: ARR up to this CCR */
    pulse_generator_entry_layout_t layout;
    uint32_t           period_max;
    uint32_t           tick_hz;      /* counting frequency, after the prescaler */
} stm32f4_pg_hw_t;

/* Binds one channel of a timer CubeMX has already set up in PWM mode, with
   the timer's update DMA request linked to a circular stream (DMA Settings:
   TIMx_UP, Memory To Peripheral, Circular, Word / Word). Reads the timer's
   instance, prescaler and DMA handle, so it must run after MX_DMA_Init()
   and MX_TIMx_Init().
   @return PULSE_GENERATOR_ERROR_INVALID_PARAM for a null argument, a channel
           the timer does not have, a timer without DMA burst, no circular
           update DMA stream, or a timer whose clock this platform cannot
           resolve — failures worth catching at startup rather than at the
           first movement. */
pulse_generator_status_t stm32f4_pg_hw_init(stm32f4_pg_hw_t *hw,
                                            TIM_HandleTypeDef *htim,
                                            uint32_t channel);

/* Shared by every output: all the per-output state lives in
   stm32f4_pg_hw_t, so one table in flash serves as many axes as needed.
   The buffer in pulse_generator_config_t::dma must be in SRAM: the DMA
   controllers cannot reach the CCM RAM (0x10000000). */
extern const pulse_generator_ops_t g_stm32f4_pg_ops;

#ifdef __cplusplus
}
#endif

#endif /* PULSE_GENERATOR_PLATFORM_H */
