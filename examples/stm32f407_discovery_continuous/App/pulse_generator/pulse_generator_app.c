#include "pulse_generator_app.h"

#include "pulse_generator_platform.h"

extern TIM_HandleTypeDef htim4;

#define ARRAY_LEN(a) (sizeof (a) / sizeof (a)[0])

/* 32 entries refilled 16 at a time within a 2 ms window: no entry lasts
   longer than 62.5 us (5250 ticks at 84 MHz), so a frequency change reaches
   the pin within 2 ms, unless the period in progress is longer than that. */
#define PG_DMA_ENTRIES   32U
#define PG_DMA_WINDOW_US 2000U

/* Words per entry of the widest burst (channel 4: ARR, RCR, CCR1..CCR4), so
   the buffer fits whichever channel the board wiring picks. */
#define PG_DMA_MAX_WORDS_PER_ENTRY 6U

/* Shortest entry the DMA burst keeps up with, measured with the fixed-count
   example: 333 ns holds, 250 ns does not. 500 ns keeps a 1.5x margin. */
#define PG_MIN_ENTRY_NS 500U

/* Visited in order and then from the start again, each held for
   FREQUENCY_STEP_MS. The first three take at most 16 entries per period.
   20 Hz is a 50 ms period, 800 entries: the change that leaves it waits for
   that period to end, up to 50 ms rather than one window. The step is half
   a 20 Hz period off a multiple of it, so that this change lands mid-period
   instead of, timer and SysTick sharing one clock, always at a boundary. */
static const uint32_t demo_frequencies_hz[] = {1000U, 5000U, 25000U, 20U};

#define FREQUENCY_STEP_MS 2025U

static stm32f4_pg_hw_t   pg_hw;
static pulse_generator_t pg;

/* Read by the DMA stream: must stay in SRAM, not CCM RAM. */
static uint32_t pg_dma_buffer[PG_DMA_ENTRIES * PG_DMA_MAX_WORDS_PER_ENTRY];

static uint32_t frequency_index;
static uint32_t last_step_ms;

/* Watch these in the debugger during hardware validation. Volatile so that
   an optimized build keeps them: most are only written. */
static volatile pulse_generator_status_t hw_init_status;
static volatile pulse_generator_status_t pg_init_status;
static volatile pulse_generator_status_t start_status;
static volatile uint32_t rejected_frequency_count; /* should stay 0 */
static volatile uint32_t underrun_count;           /* should stay 0 */
static volatile uint32_t notify_error_count;       /* notify_dma_* returned an error; should stay 0 */
static volatile uint32_t live_pulse_count;         /* refreshed on every pass of the main loop */

static void on_pulse_generator_event(pulse_generator_t *instance,
                                     pulse_generator_event_t event,
                                     void *user_ctx)
{
    (void)instance;
    (void)user_ctx;

    /* A continuous movement never completes; it only ends on stop() or,
       if a refill comes too late, with an underrun. */
    if (event == PULSE_GENERATOR_EVENT_UNDERRUN) {
        underrun_count++;
    }
}

void pulse_generator_app_init(void)
{
    /* The board wiring lives here, not in the platform: TIM4_CH1 is PD12 on
       the Discovery. */
    hw_init_status = stm32f4_pg_hw_init(&pg_hw, &htim4, TIM_CHANNEL_1);

    pg_init_status = pulse_generator_init(&pg, &(pulse_generator_config_t){
        .ops = &g_stm32f4_pg_ops,
        .hw = &pg_hw,
        .on_event = on_pulse_generator_event,
        .pulse = {
            .shape = PULSE_GENERATOR_PULSE_SHAPE_HALF_PERIOD,
        },
        .dma = {
            .buffer = pg_dma_buffer,
            .buffer_words = ARRAY_LEN(pg_dma_buffer),
            .entries = PG_DMA_ENTRIES,
            .window_us = PG_DMA_WINDOW_US,
            .min_entry_ns = PG_MIN_ENTRY_NS,
        },
    });

    if (hw_init_status != PULSE_GENERATOR_OK || pg_init_status != PULSE_GENERATOR_OK) {
        return;
    }

    frequency_index = 0U;
    last_step_ms = HAL_GetTick();
    start_status = pulse_generator_start_continuous(&pg, demo_frequencies_hz[frequency_index]);
}

void pulse_generator_app_process(void)
{
    if (hw_init_status != PULSE_GENERATOR_OK || pg_init_status != PULSE_GENERATOR_OK ||
        start_status != PULSE_GENERATOR_OK) {
        return;
    }

    live_pulse_count = pulse_generator_get_pulse_count(&pg);

    /* Unsigned subtraction keeps working across the 32-bit wrap of the
       millisecond tick. */
    if (HAL_GetTick() - last_step_ms < FREQUENCY_STEP_MS) {
        return;
    }
    last_step_ms += FREQUENCY_STEP_MS;

    frequency_index = (frequency_index + 1U) % ARRAY_LEN(demo_frequencies_hz);

    /* LD3 (PD13) toggles right before every change, so the analyzer shows
       how long the new frequency takes to reach PD12. */
    HAL_GPIO_TogglePin(LD3_GPIO_Port, LD3_Pin);
    if (pulse_generator_set_frequency(&pg, demo_frequencies_hz[frequency_index])
            != PULSE_GENERATOR_OK) {
        rejected_frequency_count++;
    }
}

/* The DMA burst's half-transfer and transfer-complete interrupts, which HAL
   reports as these two timer callbacks (HAL_TIM_DMABurst_MultiWriteStart()
   installs them on the update DMA stream). */
void HAL_TIM_PeriodElapsedHalfCpltCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM4 &&
        pulse_generator_notify_dma_half_complete(&pg) != PULSE_GENERATOR_OK) {
        notify_error_count++;
    }
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM4 &&
        pulse_generator_notify_dma_complete(&pg) != PULSE_GENERATOR_OK) {
        notify_error_count++;
    }
}
