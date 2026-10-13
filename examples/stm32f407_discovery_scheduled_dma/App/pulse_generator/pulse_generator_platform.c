#include "pulse_generator_platform.h"

#define IS_APB1_TIMER(inst) ((inst) == TIM2 || (inst) == TIM3 || (inst) == TIM4 || (inst) == TIM5 || \
                              (inst) == TIM6 || (inst) == TIM7 || (inst) == TIM12 || \
                              (inst) == TIM13 || (inst) == TIM14)

#define IS_APB2_TIMER(inst) ((inst) == TIM1 || (inst) == TIM8 || (inst) == TIM9 || \
                              (inst) == TIM10 || (inst) == TIM11)

/* Counting frequency of the timer, i.e. the bus clock after the APB timer
   multiplier and the prescaler. Returns 0 for a timer instance this helper
   does not know, which stm32f4_pg_hw_init() turns into a failed bind. */
static uint32_t resolve_tick_hz(const TIM_HandleTypeDef *htim)
{
    uint32_t tim_clk;
    uint32_t pclk;

    if (IS_APB1_TIMER(htim->Instance)) {
        pclk = HAL_RCC_GetPCLK1Freq();
        tim_clk = ((RCC->CFGR & RCC_CFGR_PPRE1) == RCC_HCLK_DIV1) ? pclk : pclk * 2U;
    } else if (IS_APB2_TIMER(htim->Instance)) {
        pclk = HAL_RCC_GetPCLK2Freq();
        tim_clk = ((RCC->CFGR & RCC_CFGR_PPRE2) == RCC_HCLK_DIV1) ? pclk : pclk * 2U;
    } else {
        return 0;
    }

    return tim_clk / (htim->Init.Prescaler + 1U);
}

pulse_generator_status_t stm32f4_pg_hw_init(stm32f4_pg_hw_t *hw,
                                            TIM_HandleTypeDef *htim,
                                            uint32_t channel)
{
    if (hw == NULL || htim == NULL || htim->Instance == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    TIM_TypeDef *tim = htim->Instance;
    const DMA_HandleTypeDef *hdma = htim->hdma[TIM_DMA_ID_UPDATE];

    volatile uint32_t *ccr;
    volatile uint32_t *ccmr;
    uint32_t ocm_mask;
    uint32_t ocm_shift;
    uint32_t ccer_mask;
    uint32_t burst_length;
    size_t   words_per_entry;

    /* The TIM_OCMODE_* constants and OC1PE are expressed in OC1's position,
       so the channels living in the upper half of a CCMR register need them
       shifted by 8. CCMR1 holds channels 1-2, CCMR2 holds 3-4.
       A DMA burst writes consecutive registers from its base, so an entry
       starting at ARR runs ARR, RCR, CCR1, ... up to this channel's CCR:
       three words for channel 1 and one more per channel after it. RCR is
       reserved on TIM2-TIM5 and only ever gets the library's 0. */
    switch (channel) {
        case TIM_CHANNEL_1:
            ccr = &tim->CCR1; ccmr = &tim->CCMR1;
            ocm_mask = TIM_CCMR1_OC1M; ocm_shift = 0U; ccer_mask = TIM_CCER_CC1E;
            burst_length = TIM_DMABURSTLENGTH_3TRANSFERS; words_per_entry = 3U;
            break;
        case TIM_CHANNEL_2:
            ccr = &tim->CCR2; ccmr = &tim->CCMR1;
            ocm_mask = TIM_CCMR1_OC2M; ocm_shift = 8U; ccer_mask = TIM_CCER_CC2E;
            burst_length = TIM_DMABURSTLENGTH_4TRANSFERS; words_per_entry = 4U;
            break;
        case TIM_CHANNEL_3:
            ccr = &tim->CCR3; ccmr = &tim->CCMR2;
            ocm_mask = TIM_CCMR2_OC3M; ocm_shift = 0U; ccer_mask = TIM_CCER_CC3E;
            burst_length = TIM_DMABURSTLENGTH_5TRANSFERS; words_per_entry = 5U;
            break;
        case TIM_CHANNEL_4:
            ccr = &tim->CCR4; ccmr = &tim->CCMR2;
            ocm_mask = TIM_CCMR2_OC4M; ocm_shift = 8U; ccer_mask = TIM_CCER_CC4E;
            burst_length = TIM_DMABURSTLENGTH_6TRANSFERS; words_per_entry = 6U;
            break;
        default:
            return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    if (!IS_TIM_CCX_INSTANCE(tim, channel) || !IS_TIM_DMABURST_INSTANCE(tim)) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    /* Without a circular stream the DMA would stop after one lap of the
       buffer and the library would never hear about it. */
    if (hdma == NULL || hdma->Init.Mode != DMA_CIRCULAR) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    uint32_t tick_hz = resolve_tick_hz(htim);
    if (tick_hz == 0) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    hw->htim = htim;
    hw->channel = channel;
    hw->ccr = ccr;
    hw->ccmr = ccmr;
    hw->ocm_mask = ocm_mask;
    hw->ocm_shift = ocm_shift;
    hw->ccer_mask = ccer_mask;
    hw->burst_length = burst_length;
    hw->layout.words_per_entry = words_per_entry;
    hw->layout.period_index = 0U;
    hw->layout.compare_index = words_per_entry - 1U;
    hw->period_max = IS_TIM_32B_COUNTER_INSTANCE(tim) ? 0xFFFFFFFFU : 0xFFFFU;
    hw->tick_hz = tick_hz;

    return PULSE_GENERATOR_OK;
}

static pulse_generator_status_t stm32f4_stream_start(void *hw_ptr,
                                                     const volatile uint32_t *buffer,
                                                     size_t words)
{
    stm32f4_pg_hw_t *hw = (stm32f4_pg_hw_t *)hw_ptr;
    TIM_TypeDef *tim = hw->htim->Instance;

    /* NDTR, the stream's transfer count, is 16 bits wide. */
    if (buffer == NULL || words == 0U || words > 0xFFFFU) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    /* Counter stopped and OCxREF held low while priming. URS cleared: HAL's
       TIM_Base_SetConfig() leaves it set, and with it the UG below would
       raise no DMA request. ARR and CCR preload on whatever the .ioc says,
       so that every entry takes effect a whole period at a time. */
    CLEAR_BIT(tim->CR1, TIM_CR1_CEN | TIM_CR1_URS);
    SET_BIT(tim->CR1, TIM_CR1_ARPE);
    MODIFY_REG(*hw->ccmr, hw->ocm_mask | (TIM_CCMR1_OC1PE << hw->ocm_shift),
               (TIM_OCMODE_FORCED_INACTIVE | TIM_CCMR1_OC1PE) << hw->ocm_shift);

    /* Priming period: as long as entry 0, so it is playable, with no pulse. */
    tim->ARR = buffer[hw->layout.period_index];
    *hw->ccr = 0U;

    if (HAL_TIM_DMABurst_MultiWriteStart(hw->htim, TIM_DMABASE_ARR, TIM_DMA_UPDATE,
                                         (const uint32_t *)buffer, hw->burst_length,
                                         (uint32_t)words) != HAL_OK) {
        /* A failed stream start leaves the HAL's burst state BUSY, which
           would refuse every later start. */
        (void)HAL_TIM_DMABurst_WriteStop(hw->htim, TIM_DMA_UPDATE);
        return PULSE_GENERATOR_ERROR;
    }

    /* UG moves the priming period into the active registers and resets the
       counter, and its DMA request writes entry 0 into the preload ones.
       The first update event then starts entry 0 and fetches entry 1: entry
       k+1 is fetched at the update event that starts entry k, as the
       contract requires. */
    tim->EGR = TIM_EGR_UG;

    /* Only now, with CCR = 0 active, can OCxREF follow the PWM: switching
       earlier would compare against whatever the last movement left. */
    MODIFY_REG(*hw->ccmr, hw->ocm_mask, TIM_OCMODE_PWM1 << hw->ocm_shift);
    SET_BIT(tim->CCER, hw->ccer_mask);
    SET_BIT(tim->CR1, TIM_CR1_CEN);

    return PULSE_GENERATOR_OK;
}

static pulse_generator_status_t stm32f4_stream_stop(void *hw_ptr)
{
    stm32f4_pg_hw_t *hw = (stm32f4_pg_hw_t *)hw_ptr;
    TIM_TypeDef *tim = hw->htim->Instance;

    /* Pin low at once, mid-pulse included. The channel stays enabled with
       OCxREF forced inactive, so the pin is driven low rather than left to
       the pull-down. */
    MODIFY_REG(*hw->ccmr, hw->ocm_mask, TIM_OCMODE_FORCED_INACTIVE << hw->ocm_shift);

    /* HAL_DMA_Abort_IT() underneath only clears the stream's enable bit, so
       this is safe from the stream's own interrupt: HAL_DMA_IRQHandler()
       completes the abort afterwards. On a stream already stopped it does
       nothing. */
    (void)HAL_TIM_DMABurst_WriteStop(hw->htim, TIM_DMA_UPDATE);

    CLEAR_BIT(tim->CR1, TIM_CR1_CEN);

    return PULSE_GENERATOR_OK;
}

static size_t stm32f4_get_stream_remaining(void *hw_ptr)
{
    stm32f4_pg_hw_t *hw = (stm32f4_pg_hw_t *)hw_ptr;

    return __HAL_DMA_GET_COUNTER(hw->htim->hdma[TIM_DMA_ID_UPDATE]);
}

static uint32_t stm32f4_get_tick_hz(void *hw_ptr)
{
    return ((const stm32f4_pg_hw_t *)hw_ptr)->tick_hz;
}

static uint32_t stm32f4_get_period_max(void *hw_ptr)
{
    return ((const stm32f4_pg_hw_t *)hw_ptr)->period_max;
}

static pulse_generator_status_t stm32f4_get_entry_layout(void *hw_ptr,
                                                         pulse_generator_entry_layout_t *layout)
{
    *layout = ((const stm32f4_pg_hw_t *)hw_ptr)->layout;

    return PULSE_GENERATOR_OK;
}

const pulse_generator_ops_t g_stm32f4_pg_ops = {
    .get_tick_hz          = stm32f4_get_tick_hz,
    .stream_start         = stm32f4_stream_start,
    .stream_stop          = stm32f4_stream_stop,
    .get_stream_remaining = stm32f4_get_stream_remaining,
    .get_period_max       = stm32f4_get_period_max,
    .get_entry_layout     = stm32f4_get_entry_layout,
};
