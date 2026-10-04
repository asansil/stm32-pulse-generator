#ifndef MOCK_PLATFORM_H
#define MOCK_PLATFORM_H

#include "pulse_generator.h"

/**
 * @brief A simulated Output Compare channel.
 *
 * Holds the registers a real channel would (counter, compare) plus a record
 * of what the library did with them, so a test can drive the hardware and
 * assert on the calls at the same time. One of these per instance, all
 * sharing a single ops table, which is what lets tests exercise several
 * instances at once.
 */
typedef struct {
    /* --- Simulated registers --- */
    bool     channel_running;
    uint32_t counter;
    uint32_t compare;

    /* --- Simulated platform properties, set before starting a movement --- */
    uint32_t counter_max;  /* mock_hw_init() defaults it to 0xFFFF */
    uint32_t tick_hz;      /* mock_hw_init() defaults it to 2 MHz */

    /* Interrupt latency mock_hw_fire() adds on top of the match that fired,
       in ticks. 0 means the ISR runs instantly; a half period or more makes
       the library detect a missed compare. */
    uint32_t isr_latency_ticks;

    /* --- Injected failures: returned instead of acting --- */
    pulse_generator_status_t channel_start_result;
    pulse_generator_status_t set_compare_result;

    /* --- Call record --- */
    int      channel_start_call_count;
    uint32_t first_compare;  /* compare value passed to the last channel_start */
    int      channel_stop_call_count;
    int      set_compare_call_count;
    uint32_t last_compare;   /* value passed to the last set_compare */
    int      get_counter_call_count;
    int      get_tick_hz_call_count;
    int      get_counter_max_call_count;

    /* --- Optional hooks --- */
    bool            dma_running;
    const uint32_t *dma_buffer;
    size_t          dma_len;
} mock_hw_t;

/** Every hook implemented. */
extern const pulse_generator_ops_t g_mock_ops;

/** Only the mandatory hooks; every optional one is NULL. */
extern const pulse_generator_ops_t g_mock_ops_minimal;

/** Resets a simulated channel to a known state, with 16-bit counter and a
    2 MHz tick rate, chosen so that 1000 Hz maps to 1000 ticks and the
    assertions stay readable. */
void mock_hw_init(mock_hw_t *hw);

/**
 * @brief Simulate the armed match firing.
 *
 * Moves the counter to the compare value that just matched, plus the
 * configured ISR latency. Call this before
 * pulse_generator_notify_compare_match(): without it the counter stays put
 * while the library advances the compare, so every edge would look late and
 * the library would rightly report a missed compare.
 */
void mock_hw_fire(mock_hw_t *hw);

/** mock_hw_fire() followed by pulse_generator_notify_compare_match(), which
    is the pair every edge-stepping test needs. */
pulse_generator_status_t mock_hw_fire_and_notify(pulse_generator_t *pg, mock_hw_t *hw);

#endif /* MOCK_PLATFORM_H */
