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

    volatile uint32_t *ccr;
    volatile uint32_t *ccmr;
    uint32_t ocm_mask;
    uint32_t ocm_shift;
    uint32_t cc_flag;

    /* The TIM_OCMODE_* constants are expressed in OC1M's position, so the
       channels living in the upper half of a CCMR register need them shifted
       by 8. CCMR1 holds channels 1-2, CCMR2 holds 3-4. */
    switch (channel) {
        case TIM_CHANNEL_1:
            ccr = &tim->CCR1; ccmr = &tim->CCMR1;
            ocm_mask = TIM_CCMR1_OC1M; ocm_shift = 0U; cc_flag = TIM_FLAG_CC1;
            break;
        case TIM_CHANNEL_2:
            ccr = &tim->CCR2; ccmr = &tim->CCMR1;
            ocm_mask = TIM_CCMR1_OC2M; ocm_shift = 8U; cc_flag = TIM_FLAG_CC2;
            break;
        case TIM_CHANNEL_3:
            ccr = &tim->CCR3; ccmr = &tim->CCMR2;
            ocm_mask = TIM_CCMR2_OC3M; ocm_shift = 0U; cc_flag = TIM_FLAG_CC3;
            break;
        case TIM_CHANNEL_4:
            ccr = &tim->CCR4; ccmr = &tim->CCMR2;
            ocm_mask = TIM_CCMR2_OC4M; ocm_shift = 8U; cc_flag = TIM_FLAG_CC4;
            break;
        default:
            return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    if (!IS_TIM_CCX_INSTANCE(tim, channel)) {
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
    hw->cc_flag = cc_flag;
    hw->counter_max = IS_TIM_32B_COUNTER_INSTANCE(tim) ? 0xFFFFFFFFU : 0xFFFFU;
    hw->tick_hz = tick_hz;

    return PULSE_GENERATOR_OK;
}

static pulse_generator_status_t stm32f4_channel_start(void *hw_ptr, uint32_t first_compare)
{
    stm32f4_pg_hw_t *hw = (stm32f4_pg_hw_t *)hw_ptr;

    /* The library schedules every match relative to the previous one and
       wraps the arithmetic at counter_max, which requires the counter to run
       over its full range whatever Counter Period was configured. */
    __HAL_TIM_SET_AUTORELOAD(hw->htim, hw->counter_max);

    /* Force OCxREF low before (re)enabling the output: a stop() in the
       middle of a pulse leaves it high, which would make the pin jump high
       as soon as the channel is enabled and invert every edge the library
       counts afterwards. */
    MODIFY_REG(*hw->ccmr, hw->ocm_mask, TIM_OCMODE_FORCED_INACTIVE << hw->ocm_shift);
    MODIFY_REG(*hw->ccmr, hw->ocm_mask, TIM_OCMODE_TOGGLE << hw->ocm_shift);

    /* As late as possible: the library measured first_compare from the
       counter just before calling, so everything done in between eats into
       the first half period. */
    *hw->ccr = first_compare;

    /* Drop any compare flag left over from a previous movement, so enabling
       the interrupt does not report a toggle that never happened. */
    __HAL_TIM_CLEAR_FLAG(hw->htim, hw->cc_flag);

    return (HAL_TIM_OC_Start_IT(hw->htim, hw->channel) == HAL_OK) ? PULSE_GENERATOR_OK
                                                                  : PULSE_GENERATOR_ERROR;
}

static pulse_generator_status_t stm32f4_channel_stop(void *hw_ptr)
{
    stm32f4_pg_hw_t *hw = (stm32f4_pg_hw_t *)hw_ptr;

    /* Leaves the counter running if other channels of the same timer are
       still active, which is what HAL_TIM_OC_Stop_IT already does. */
    return (HAL_TIM_OC_Stop_IT(hw->htim, hw->channel) == HAL_OK) ? PULSE_GENERATOR_OK
                                                                 : PULSE_GENERATOR_ERROR;
}

static pulse_generator_status_t stm32f4_set_compare(void *hw_ptr, uint32_t compare)
{
    stm32f4_pg_hw_t *hw = (stm32f4_pg_hw_t *)hw_ptr;

    *hw->ccr = compare;

    return PULSE_GENERATOR_OK;
}

static uint32_t stm32f4_get_counter(void *hw_ptr)
{
    stm32f4_pg_hw_t *hw = (stm32f4_pg_hw_t *)hw_ptr;

    return __HAL_TIM_GET_COUNTER(hw->htim);
}

static uint32_t stm32f4_get_tick_hz(void *hw_ptr)
{
    return ((const stm32f4_pg_hw_t *)hw_ptr)->tick_hz;
}

static uint32_t stm32f4_get_counter_max(void *hw_ptr)
{
    return ((const stm32f4_pg_hw_t *)hw_ptr)->counter_max;
}

const pulse_generator_ops_t g_stm32f4_pg_ops = {
    .channel_start   = stm32f4_channel_start,
    .channel_stop    = stm32f4_channel_stop,
    .set_compare     = stm32f4_set_compare,
    .get_counter     = stm32f4_get_counter,
    .get_tick_hz     = stm32f4_get_tick_hz,
    .get_counter_max = stm32f4_get_counter_max,
    /* dma_* and gpio_* stay NULL: this example drives the timer backend
       only, and the library reports NOT_SUPPORTED for the rest. */
};
