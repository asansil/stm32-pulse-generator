#include "pulse_generator_platform.h"

#include "main.h"

extern TIM_HandleTypeDef htim4;

#define IS_APB1_TIMER(inst) ((inst) == TIM2 || (inst) == TIM3 || (inst) == TIM4 || (inst) == TIM5 || \
                              (inst) == TIM6 || (inst) == TIM7 || (inst) == TIM12 || \
                              (inst) == TIM13 || (inst) == TIM14)

#define IS_APB2_TIMER(inst) ((inst) == TIM1 || (inst) == TIM8 || (inst) == TIM9 || \
                              (inst) == TIM10 || (inst) == TIM11)

static pulse_generator_status_t stm32f4_timer_start(void *ctx)
{
    TIM_HandleTypeDef *htim = (TIM_HandleTypeDef *)ctx;
    return (HAL_TIM_OC_Start_IT(htim, TIM_CHANNEL_1) == HAL_OK) ? PULSE_GENERATOR_OK : PULSE_GENERATOR_ERROR;
}

static pulse_generator_status_t stm32f4_timer_stop(void *ctx)
{
    TIM_HandleTypeDef *htim = (TIM_HandleTypeDef *)ctx;
    return (HAL_TIM_OC_Stop_IT(htim, TIM_CHANNEL_1) == HAL_OK) ? PULSE_GENERATOR_OK : PULSE_GENERATOR_ERROR;
}

static pulse_generator_status_t stm32f4_set_compare(void *ctx, uint32_t ccr)
{
    TIM_HandleTypeDef *htim = (TIM_HandleTypeDef *)ctx;

    /* Toggle mode: the compare match repeats every ARR+1 ticks, so the
       period register has to move together with the compare register —
       not just the compare register on its own (see M5 Fase 2). */
    uint32_t period_ticks = ccr - 1U;
    __HAL_TIM_SET_AUTORELOAD(htim, period_ticks);
    __HAL_TIM_SET_COMPARE(htim, TIM_CHANNEL_1, period_ticks);

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
           tick rate that makes frequency_to_ccr produce ccr = 0 — instead
           of silently misreporting it as a plausible-looking APB2 clock. */
        return 0;
    }

    return tim_clk / (htim->Init.Prescaler + 1U);
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
    .timer_start        = stm32f4_timer_start,
    .timer_stop          = stm32f4_timer_stop,
    .set_compare         = stm32f4_set_compare,
    .dma_start           = stm32f4_dma_start_stub,
    .dma_stop            = stm32f4_dma_stop_stub,
    .gpio_set            = stm32f4_gpio_set_stub,
    .gpio_clear          = stm32f4_gpio_clear_stub,
    .get_timer_main_clk  = stm32f4_get_timer_main_clk,
    .ctx                 = &htim4,
};
