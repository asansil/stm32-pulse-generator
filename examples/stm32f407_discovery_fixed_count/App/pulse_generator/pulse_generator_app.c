#include "pulse_generator_app.h"

#include <stdbool.h>

#include "pulse_generator_platform.h"

extern TIM_HandleTypeDef htim4;

/* 32 entries refilled 16 at a time within a 2 ms window: no entry lasts
   longer than 62.5 us (5250 ticks at 84 MHz), so a refill comes at least
   every 1 ms whatever the frequency. */
#define PG_DMA_ENTRIES   32U
#define PG_DMA_WINDOW_US 2000U

/* Words per entry of the widest burst (channel 4: ARR, RCR, CCR1..CCR4), so
   the buffer fits whichever channel the board wiring picks. */
#define PG_DMA_MAX_WORDS_PER_ENTRY 6U

/* Shortest entry the DMA burst can keep up with, measured on the board.
   0 leaves only the library's structural minimum of 2 ticks. */
#define PG_MIN_ENTRY_NS 0U

/* Gap between movements, so each one stands apart on the analyzer. */
#define PG_PAUSE_MS 500U

typedef struct {
    uint32_t frequency_hz;
    uint32_t pulse_count;
} pg_test_case_t;

/* Hardware validation sequence, run in a loop. The first two are the M5
   cases; the last one has 4.2 M-tick periods, split into 800 entries each
   with the high time spanning 400 of them. */
static const pg_test_case_t test_cases[] = {
    {  1000U,   10U }, /* 84000-tick periods, beyond the 16-bit ARR: 16 entries each */
    { 25000U, 1000U }, /* one entry per period, a refill every 640 us */
    {    20U,   10U },
};

#define PG_TEST_CASE_COUNT (sizeof(test_cases) / sizeof(test_cases[0]))

static stm32f4_pg_hw_t   pg_hw;
static pulse_generator_t pg;

/* Read by the DMA stream: must stay in SRAM, not CCM RAM. */
static uint32_t pg_dma_buffer[PG_DMA_ENTRIES * PG_DMA_MAX_WORDS_PER_ENTRY];

static size_t            next_test_case;
static volatile bool     movement_ended = true;
static volatile uint32_t movement_ended_ms;

/* Watch these in the debugger during hardware validation. */
static pulse_generator_status_t hw_init_status;
static pulse_generator_status_t pg_init_status;
static pulse_generator_status_t last_start_status;
static volatile uint32_t completed_movement_count;
static volatile uint32_t underrun_count;     /* should stay 0 */
static volatile uint32_t notify_error_count; /* notify_dma_* returned an error; should stay 0 */
static volatile uint32_t live_pulse_count;   /* refreshed on every pass of the main loop */
static uint32_t          last_movement_pulse_count;

static void end_movement(void)
{
    HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_RESET);
    movement_ended_ms = HAL_GetTick();
    movement_ended = true;
}

static void on_pulse_generator_event(pulse_generator_t *instance,
                                     pulse_generator_event_t event,
                                     void *user_ctx)
{
    (void)instance;
    (void)user_ctx;

    switch (event) {
        case PULSE_GENERATOR_EVENT_COMPLETE:
            completed_movement_count++;
            end_movement();
            break;
        case PULSE_GENERATOR_EVENT_UNDERRUN:
            underrun_count++;
            end_movement();
            break;
        default:
            break;
    }
}

static void start_next_test_case(void)
{
    const pg_test_case_t *test_case = &test_cases[next_test_case];
    next_test_case = (next_test_case + 1U) % PG_TEST_CASE_COUNT;

    /* LD3 (PD13) is high from the start until the library reports the end,
       so the analyzer shows how long after the last pulse that happens
       (up to one window). */
    movement_ended = false;
    HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_SET);

    last_start_status = pulse_generator_start_fixed_count(&pg, test_case->frequency_hz,
                                                          test_case->pulse_count);
    if (last_start_status != PULSE_GENERATOR_OK) {
        end_movement();
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
        .dma = {
            .buffer = pg_dma_buffer,
            .buffer_words = sizeof(pg_dma_buffer) / sizeof(pg_dma_buffer[0]),
            .entries = PG_DMA_ENTRIES,
            .window_us = PG_DMA_WINDOW_US,
            .pulse_shape = PULSE_GENERATOR_PULSE_SHAPE_HALF_PERIOD,
            .min_entry_ns = PG_MIN_ENTRY_NS,
        },
    });

    if (hw_init_status == PULSE_GENERATOR_OK && pg_init_status == PULSE_GENERATOR_OK) {
        start_next_test_case();
    }
}

void pulse_generator_app_process(void)
{
    if (hw_init_status != PULSE_GENERATOR_OK || pg_init_status != PULSE_GENERATOR_OK) {
        return;
    }

    live_pulse_count = pulse_generator_get_pulse_count(&pg);

    if (!movement_ended || (HAL_GetTick() - movement_ended_ms) < PG_PAUSE_MS) {
        return;
    }

    last_movement_pulse_count = live_pulse_count;
    start_next_test_case();
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
