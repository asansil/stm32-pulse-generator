#ifndef MOCK_PLATFORM_H
#define MOCK_PLATFORM_H

#include "pulse_generator.h"

/** Most edges of each kind a mock_hw_t stores; later ones are counted
    but not stored. */
#define MOCK_MAX_EDGES 4096

/**
 * @brief A simulated timer fed by a circular DMA stream.
 *
 * Plays the promise of pulse_generator_ops_t::stream_start: entry 0 is the
 * first period, and entry k+1 is fetched into the preload registers at the
 * update event that starts entry k. Half-transfer fires once the first half
 * of the buffer has been fetched, transfer-complete once the second has.
 * Rather than registers, it keeps the pin's timeline, the tick of every rise
 * and fall, so tests assert edges instead of encodings.
 */
typedef struct {
    /* --- Simulated platform properties, set before starting a movement --- */
    uint32_t                       tick_hz;    /* mock_hw_init() defaults it to 2 MHz */
    uint32_t                       period_max; /* defaults to 0xFFFF */
    pulse_generator_entry_layout_t layout;     /* defaults to channel 1 on STM32F4:
                                                  3 words, ARR at 0, CCR at 2 */

    /* --- Injected failures: returned instead of acting --- */
    pulse_generator_status_t stream_start_result;
    pulse_generator_status_t get_entry_layout_result;

    /* --- Simulated stream --- */
    bool                     running;
    const volatile uint32_t *buffer;          /* as passed to stream_start */
    size_t                   words;
    size_t                   next_fetch;      /* entry the next update event fetches, within the lap */
    uint32_t                 preload_period;  /* entry fetched last: active at the next update event */
    uint32_t                 preload_compare;
    uint32_t                 entries_played;  /* update events since stream_start */

    /* --- Pin timeline, in ticks since the last stream_start --- */
    uint32_t now_tick;          /* start of the next period */
    uint32_t active_start_tick; /* start of the period being played */
    bool     pin_high;          /* level the period being played ends at */
    uint32_t stop_tick;         /* where the last stream_stop forced the pin low */
    size_t   rise_count;
    size_t   fall_count;
    uint32_t rise_ticks[MOCK_MAX_EDGES];
    uint32_t fall_ticks[MOCK_MAX_EDGES];

    /* --- Call record --- */
    int stream_start_call_count;
    int stream_stop_call_count;
    int get_stream_remaining_call_count;
    int get_tick_hz_call_count;
    int get_period_max_call_count;
    int get_entry_layout_call_count;
} mock_hw_t;

/** The mock's operations table, shared by every simulated output. */
extern const pulse_generator_ops_t g_mock_ops;

/** Resets a simulated output to a known state: 2 MHz tick rate, 16-bit
    period register, channel-1 layout, stream stopped. */
void mock_hw_init(mock_hw_t *hw);

/**
 * @brief Play k update events, delivering half-transfer and
 *        transfer-complete to pg as the stream raises them, the way the
 *        integrator's DMA ISR would.
 * @return The status of the last notification delivered; PULSE_GENERATOR_OK
 *         if none was.
 * @note Stops early once the stream is stopped, so a large k plays a
 *       movement to its end.
 */
pulse_generator_status_t mock_step(pulse_generator_t *pg, mock_hw_t *hw, size_t k);

/** Same as mock_step() without delivering any interrupt: they are left
    for the test to deliver late, by calling the notify functions itself. */
void mock_advance(mock_hw_t *hw, size_t k);

/** Word `word` of entry `entry` of the buffer last passed to stream_start. */
uint32_t mock_entry_word(const mock_hw_t *hw, size_t entry, size_t word);

#endif /* MOCK_PLATFORM_H */
