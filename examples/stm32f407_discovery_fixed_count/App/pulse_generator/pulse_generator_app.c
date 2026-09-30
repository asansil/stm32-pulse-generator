#include "pulse_generator_app.h"

#include "main.h"
#include "pulse_generator_platform.h"

static pulse_generator_t pg;

void pulse_generator_app_init(void)
{
    pulse_generator_init(&pg, &g_stm32f4_platform);

    /* Slow and low on purpose (2 Hz, 10 pulses): visible by eye on the
       green LED (PD12/TIM4_CH1) for the M5 hardware validation. */
    pulse_generator_start_fixed_count(&pg, PULSE_GENERATOR_BACKEND_TIMER, 2, 10);
}

void HAL_TIM_OC_DelayElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM4) {
        pulse_generator_notify_compare_match(&pg);
    }
}
