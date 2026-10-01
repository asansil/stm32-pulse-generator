#include "pulse_generator_platform.h"

#include "main.h"

extern TIM_HandleTypeDef htim4;

#define IS_APB1_TIMER(inst) ((inst) == TIM2 || (inst) == TIM3 || (inst) == TIM4 || (inst) == TIM5 || \
                              (inst) == TIM6 || (inst) == TIM7 || (inst) == TIM12 || \
                              (inst) == TIM13 || (inst) == TIM14)

#define IS_APB2_TIMER(inst) ((inst) == TIM1 || (inst) == TIM8 || (inst) == TIM9 || \
                              (inst) == TIM10 || (inst) == TIM11)

/* Free-running counter: ARR is kept at the counter's full-width maximum
   and every compare match is scheduled relative to the previous one, so
   all compare arithmetic wraps modulo 2^16 or 2^32 depending on the
   timer. Masking with counter_max() gives that wrap for either width. */
static uint32_t counter_max(const TIM_HandleTypeDef *htim)
{
    return IS_TIM_32B_COUNTER_INSTANCE(htim->Instance) ? 0xFFFFFFFFU : 0xFFFFU;
}

static pulse_generator_status_t stm32f4_timer_start(void *ctx, uint32_t half_period_ticks)
{
    TIM_HandleTypeDef *htim = (TIM_HandleTypeDef *)ctx;
    uint32_t max = counter_max(htim);

    /* The modular compare arithmetic below requires the counter to run
       over its full range, whatever Counter Period was configured. */
    __HAL_TIM_SET_AUTORELOAD(htim, max);

    /* Force OC1REF low before (re)enabling the output: a stop() in the
       middle of a pulse leaves it high, which would make the pin jump high
       as soon as the channel is enabled and invert every edge the library
       counts afterwards. */
    MODIFY_REG(htim->Instance->CCMR1, TIM_CCMR1_OC1M, TIM_OCMODE_FORCED_INACTIVE);
    MODIFY_REG(htim->Instance->CCMR1, TIM_CCMR1_OC1M, TIM_OCMODE_TOGGLE);

    __HAL_TIM_SET_COMPARE(htim, TIM_CHANNEL_1, (__HAL_TIM_GET_COUNTER(htim) + half_period_ticks) & max);

    /* Drop any compare flag left over from a previous movement, so enabling
       the interrupt does not report a toggle that never happened. */
    __HAL_TIM_CLEAR_FLAG(htim, TIM_FLAG_CC1);

    return (HAL_TIM_OC_Start_IT(htim, TIM_CHANNEL_1) == HAL_OK) ? PULSE_GENERATOR_OK : PULSE_GENERATOR_ERROR;
}

static pulse_generator_status_t stm32f4_timer_stop(void *ctx)
{
    TIM_HandleTypeDef *htim = (TIM_HandleTypeDef *)ctx;
    return (HAL_TIM_OC_Stop_IT(htim, TIM_CHANNEL_1) == HAL_OK) ? PULSE_GENERATOR_OK : PULSE_GENERATOR_ERROR;
}

static pulse_generator_status_t stm32f4_advance_compare(void *ctx, uint32_t half_period_ticks)
{
    TIM_HandleTypeDef *htim = (TIM_HandleTypeDef *)ctx;
    uint32_t max = counter_max(htim);

    uint32_t previous = __HAL_TIM_GET_COMPARE(htim, TIM_CHANNEL_1);
    __HAL_TIM_SET_COMPARE(htim, TIM_CHANNEL_1, (previous + half_period_ticks) & max);

    /* Ticks elapsed since the match being serviced, modulo the counter
       width. If a whole half period has already gone by, the counter is at
       or past the new compare value and the match would not fire until the
       counter wraps: reschedule from now instead, losing phase but not a
       full wrap. */
    uint32_t elapsed = (__HAL_TIM_GET_COUNTER(htim) - previous) & max;
    if (elapsed >= half_period_ticks) {
        __HAL_TIM_SET_COMPARE(htim, TIM_CHANNEL_1, (__HAL_TIM_GET_COUNTER(htim) + half_period_ticks) & max);
        return PULSE_GENERATOR_ERROR_MISSED_COMPARE;
    }

    return PULSE_GENERATOR_OK;
}

static uint32_t stm32f4_get_timer_main_clk(void *ctx)
{
    TIM_HandleTypeDef *htim = (TIM_HandleTypeDef *)ctx;
    uint32_t tim_clk;
    uint32_t pclk;

    if (IS_APB1_TIMER(htim->Instance)) {
        pclk = HAL_RCC_GetPCLK1Freq();
        tim_clk = ((RCC->CFGR & RCC_CFGR_PPRE1) == RCC_HCLK_DIV1) ? pclk : pclk * 2U;
    } else if (IS_APB2_TIMER(htim->Instance)) {
        pclk = HAL_RCC_GetPCLK2Freq();
        tim_clk = ((RCC->CFGR & RCC_CFGR_PPRE2) == RCC_HCLK_DIV1) ? pclk : pclk * 2U;
    } else {
        /* Unrecognized timer instance: corrupt ctx, or a timer this
           helper doesn't know about. Report 0 Hz — an obviously broken
           tick rate that makes the library reject every frequency as out
           of range — instead of silently misreporting it as a
           plausible-looking APB2 clock. */
        return 0;
    }

    return tim_clk / (htim->Init.Prescaler + 1U);
}

static uint32_t stm32f4_get_max_ticks(void *ctx)
{
    return counter_max((const TIM_HandleTypeDef *)ctx);
}

static pulse_generator_status_t stm32f4_dma_start_stub(void *ctx, const uint32_t *buffer, size_t len)
{
    (void)ctx;
    (void)buffer;
    (void)len;
    return PULSE_GENERATOR_ERROR;
}

static pulse_generator_status_t stm32f4_dma_stop_stub(void *ctx)
{
    (void)ctx;
    return PULSE_GENERATOR_ERROR;
}

static pulse_generator_status_t stm32f4_gpio_set_stub(void *ctx)
{
    (void)ctx;
    return PULSE_GENERATOR_ERROR;
}

static pulse_generator_status_t stm32f4_gpio_clear_stub(void *ctx)
{
    (void)ctx;
    return PULSE_GENERATOR_ERROR;
}

const pulse_generator_platform_t g_stm32f4_platform = {
    .timer_start         = stm32f4_timer_start,
    .timer_stop          = stm32f4_timer_stop,
    .advance_compare     = stm32f4_advance_compare,
    .dma_start           = stm32f4_dma_start_stub,
    .dma_stop            = stm32f4_dma_stop_stub,
    .gpio_set            = stm32f4_gpio_set_stub,
    .gpio_clear          = stm32f4_gpio_clear_stub,
    .get_timer_main_clk  = stm32f4_get_timer_main_clk,
    .get_max_ticks       = stm32f4_get_max_ticks,
    .ctx                 = &htim4,
};
