#include "pulse_generator_app.h"

#include "pulse_generator_platform.h"

extern TIM_HandleTypeDef htim4;

static stm32f4_pg_hw_t   pg_hw;
static pulse_generator_t pg;

/* Late compare-match interrupts reported by the library; watch it in the
   debugger during hardware validation (should stay 0). */
static volatile uint32_t missed_compare_count;

/* Movements completed by the library on its own; one per
   start_fixed_count() that runs to its target. */
static volatile uint32_t completed_movement_count;

static void on_pulse_generator_event(pulse_generator_t *instance,
                                     pulse_generator_event_t event,
                                     void *user_ctx)
{
    (void)instance;
    (void)user_ctx;

    switch (event) {
        case PULSE_GENERATOR_EVENT_COMPLETE:
            completed_movement_count++;
            break;
        case PULSE_GENERATOR_EVENT_MISSED_COMPARE:
            missed_compare_count++;
            break;
        default:
            break;
    }
}

void pulse_generator_app_init(void)
{
    /* The board wiring lives here, not in the platform: TIM4_CH1 is PD12 on
       the Discovery. */
    stm32f4_pg_hw_init(&pg_hw, &htim4, TIM_CHANNEL_1);

    pulse_generator_init(&pg, &(pulse_generator_config_t){
        .ops = &g_stm32f4_pg_ops,
        .hw = &pg_hw,
        .on_event = on_pulse_generator_event,
    });

    /* 25 kHz, 1000 pulses on PD12/TIM4_CH1: a compare interrupt every
       20 us, close to the expected per-axis ceiling, to check that the
       toggle ISR keeps up (missed_compare_count should stay 0). */
    pulse_generator_start_fixed_count(&pg, PULSE_GENERATOR_BACKEND_TIMER, 25000, 1000);
}

void HAL_TIM_OC_DelayElapsedCallback(TIM_HandleTypeDef *htim)
{
    /* Filtered by channel as well as by timer: several instances can share
       one timer, each owning its own channel. HAL_TIM_IRQHandler sets
       ->Channel before calling this. */
    if (htim->Instance == TIM4 && htim->Channel == HAL_TIM_ACTIVE_CHANNEL_1) {
        pulse_generator_notify_compare_match(&pg);
    }
}
