#include "pulse_generator_app.h"

#include "main.h"
#include "pulse_generator_platform.h"

static pulse_generator_t pg;

/* Late compare-match interrupts reported by the library; watch it in the
   debugger during hardware validation (should stay 0). */
static volatile uint32_t missed_compare_count;

void pulse_generator_app_init(void)
{
    pulse_generator_init(&pg, &g_stm32f4_platform);

    /* 1 kHz, 10 pulses on PD12/TIM4_CH1: easy to capture and count on a
       logic analyzer triggered on the first rising edge. */
    pulse_generator_start_fixed_count(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000, 10);
}

void HAL_TIM_OC_DelayElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM4) {
        if (pulse_generator_notify_compare_match(&pg) == PULSE_GENERATOR_ERROR_MISSED_COMPARE) {
            missed_compare_count++;
        }
    }
}
