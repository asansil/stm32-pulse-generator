#include "pulse_generator_app.h"

#include "pulse_generator_platform.h"

extern TIM_HandleTypeDef htim4;

/* Visited in order and then from the start again, each held for
   FREQUENCY_STEP_MS, so every change is easy to find on a logic analyzer. */
static const uint32_t demo_frequencies_hz[] = {1000, 5000, 25000};

#define DEMO_FREQUENCY_COUNT (sizeof demo_frequencies_hz / sizeof demo_frequencies_hz[0])
#define FREQUENCY_STEP_MS    2000u

static stm32f4_pg_hw_t   pg_hw;
static pulse_generator_t pg;

static uint32_t frequency_index;
static uint32_t last_step_ms;

/* Late compare-match interrupts reported by the library; watch it in the
   debugger during hardware validation (should stay 0). */
static volatile uint32_t missed_compare_count;

/* Frequency changes the library rejected; should stay 0, since every demo
   value is within range for a 1 MHz tick on a 16-bit counter. */
static volatile uint32_t rejected_frequency_count;

static void on_pulse_generator_event(pulse_generator_t *instance,
                                     pulse_generator_event_t event,
                                     void *user_ctx)
{
    (void)instance;
    (void)user_ctx;

    switch (event) {
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

    frequency_index = 0;
    last_step_ms = HAL_GetTick();
    pulse_generator_start_continuous(&pg, demo_frequencies_hz[frequency_index]);
}

void pulse_generator_app_process(void)
{
    /* Unsigned subtraction keeps working across the 32-bit wrap of the
       millisecond tick (~49 days). */
    if (HAL_GetTick() - last_step_ms < FREQUENCY_STEP_MS) {
        return;
    }
    last_step_ms += FREQUENCY_STEP_MS;

    frequency_index = (frequency_index + 1) % DEMO_FREQUENCY_COUNT;
    if (pulse_generator_set_frequency(&pg, demo_frequencies_hz[frequency_index])
            != PULSE_GENERATOR_OK) {
        rejected_frequency_count++;
    }
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
