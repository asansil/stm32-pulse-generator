#include "mock_platform.h"

static pulse_generator_status_t mock_channel_start(void *hw_ptr, uint32_t first_compare)
{
    mock_hw_t *hw = (mock_hw_t *)hw_ptr;

    hw->channel_start_call_count++;
    hw->first_compare = first_compare;

    if (hw->channel_start_result != PULSE_GENERATOR_OK) {
        return hw->channel_start_result;
    }

    hw->channel_running = true;
    hw->compare = first_compare;

    return PULSE_GENERATOR_OK;
}

static pulse_generator_status_t mock_channel_stop(void *hw_ptr)
{
    mock_hw_t *hw = (mock_hw_t *)hw_ptr;

    hw->channel_stop_call_count++;
    hw->channel_running = false;

    return PULSE_GENERATOR_OK;
}

static pulse_generator_status_t mock_set_compare(void *hw_ptr, uint32_t compare)
{
    mock_hw_t *hw = (mock_hw_t *)hw_ptr;

    hw->set_compare_call_count++;
    hw->last_compare = compare;

    if (hw->set_compare_result != PULSE_GENERATOR_OK) {
        return hw->set_compare_result;
    }

    hw->compare = compare;

    return PULSE_GENERATOR_OK;
}

static uint32_t mock_get_counter(void *hw_ptr)
{
    mock_hw_t *hw = (mock_hw_t *)hw_ptr;

    hw->get_counter_call_count++;

    return hw->counter;
}

static uint32_t mock_get_tick_hz(void *hw_ptr)
{
    mock_hw_t *hw = (mock_hw_t *)hw_ptr;

    hw->get_tick_hz_call_count++;

    return hw->tick_hz;
}

static uint32_t mock_get_counter_max(void *hw_ptr)
{
    mock_hw_t *hw = (mock_hw_t *)hw_ptr;

    hw->get_counter_max_call_count++;

    return hw->counter_max;
}

const pulse_generator_ops_t g_mock_ops = {
    .channel_start   = mock_channel_start,
    .channel_stop    = mock_channel_stop,
    .set_compare     = mock_set_compare,
    .get_counter     = mock_get_counter,
    .get_tick_hz     = mock_get_tick_hz,
    .get_counter_max = mock_get_counter_max,
};

void mock_hw_init(mock_hw_t *hw)
{
    *hw = (mock_hw_t){0};

    hw->counter_max = 0xFFFF;

    /* 2 MHz: an arbitrary simulated tick rate that keeps the tests'
       frequency_hz -> half_period_ticks expectations readable (1000 Hz ->
       1000 ticks). */
    hw->tick_hz = 2000000;
}

void mock_hw_fire(mock_hw_t *hw)
{
    hw->counter = (hw->compare + hw->isr_latency_ticks) & hw->counter_max;
}

pulse_generator_status_t mock_hw_fire_and_notify(pulse_generator_t *pg, mock_hw_t *hw)
{
    mock_hw_fire(hw);

    return pulse_generator_notify_compare_match(pg);
}

/* --- Simulated DMA stream --- */

static size_t mock_dma_entries(const mock_dma_hw_t *hw)
{
    return hw->words / hw->layout.words_per_entry;
}

static void record_rise(mock_dma_hw_t *hw, uint32_t tick)
{
    if (hw->rise_count < MOCK_DMA_MAX_EDGES) {
        hw->rise_ticks[hw->rise_count] = tick;
    }
    hw->rise_count++;
}

static void record_fall(mock_dma_hw_t *hw, uint32_t tick)
{
    if (hw->fall_count < MOCK_DMA_MAX_EDGES) {
        hw->fall_ticks[hw->fall_count] = tick;
    }
    hw->fall_count++;
}

/* Copies the next entry into the preload registers, wrapping at the end of
   the buffer as a circular stream reloads its transfer count. */
static void fetch_entry(mock_dma_hw_t *hw)
{
    const volatile uint32_t *entry = hw->buffer + hw->next_fetch * hw->layout.words_per_entry;

    hw->preload_period = entry[hw->layout.period_index];
    hw->preload_compare = entry[hw->layout.compare_index];

    hw->next_fetch++;
    if (hw->next_fetch == mock_dma_entries(hw)) {
        hw->next_fetch = 0;
    }
}

/* The preload becomes the active period. PWM mode 1, upcounting: the pin is
   high while the counter is below the compare value, so 0 means no pulse,
   a compare within the period means a rise at its start and a fall at the
   compare, and a compare past the period means high throughout. */
static void play_preload(mock_dma_hw_t *hw)
{
    const uint32_t start = hw->now_tick;
    const uint32_t length = hw->preload_period + 1u;
    const uint32_t compare = hw->preload_compare;

    if (compare == 0) {
        if (hw->pin_high) {
            record_fall(hw, start);
        }
        hw->pin_high = false;
    } else {
        if (!hw->pin_high) {
            record_rise(hw, start);
        }
        if (compare < length) {
            record_fall(hw, start + compare);
            hw->pin_high = false;
        } else {
            hw->pin_high = true;
        }
    }

    hw->active_start_tick = start;
    hw->now_tick = start + length;
    hw->entries_played++;
}

typedef enum {
    MOCK_DMA_IRQ_NONE,
    MOCK_DMA_IRQ_HALF,
    MOCK_DMA_IRQ_COMPLETE,
} mock_dma_irq_t;

/* One update event: the entry fetched last starts playing and the stream
   fetches the next. Returns the interrupt that transfer raised, if any. */
static mock_dma_irq_t update_event(mock_dma_hw_t *hw)
{
    play_preload(hw);
    fetch_entry(hw);

    if (hw->next_fetch == mock_dma_entries(hw) / 2) {
        return MOCK_DMA_IRQ_HALF;
    }
    if (hw->next_fetch == 0) {
        return MOCK_DMA_IRQ_COMPLETE;
    }
    return MOCK_DMA_IRQ_NONE;
}

/* The mock delivers its interrupts right after the update event that raised
   them, so a stop lands at the start of the period being played. Forcing the
   pin low there cuts that period short: a fall still ahead moves to the
   stop, and a pin left high falls there. */
static void force_pin_low(mock_dma_hw_t *hw)
{
    const uint32_t stop = hw->active_start_tick;
    const size_t last_fall = hw->fall_count - 1;

    if (hw->fall_count > 0 && last_fall < MOCK_DMA_MAX_EDGES && hw->fall_ticks[last_fall] > stop) {
        hw->fall_ticks[last_fall] = stop;
    }
    if (hw->pin_high) {
        record_fall(hw, stop);
        hw->pin_high = false;
    }

    hw->stop_tick = stop;
}

static pulse_generator_status_t mock_stream_start(void *hw_ptr, const volatile uint32_t *buffer, size_t words)
{
    mock_dma_hw_t *hw = (mock_dma_hw_t *)hw_ptr;

    hw->stream_start_call_count++;

    if (hw->stream_start_result != PULSE_GENERATOR_OK) {
        return hw->stream_start_result;
    }

    hw->buffer = buffer;
    hw->words = words;
    hw->next_fetch = 0;
    hw->entries_played = 0;
    hw->now_tick = 0;
    hw->active_start_tick = 0;
    hw->pin_high = false;
    hw->rise_count = 0;
    hw->fall_count = 0;
    hw->running = true;

    /* Priming: entry 0 waits in the preload registers and becomes the
       first period at the first update event. */
    fetch_entry(hw);

    return PULSE_GENERATOR_OK;
}

static pulse_generator_status_t mock_stream_stop(void *hw_ptr)
{
    mock_dma_hw_t *hw = (mock_dma_hw_t *)hw_ptr;

    hw->stream_stop_call_count++;

    if (hw->running) {
        force_pin_low(hw);
    }
    hw->running = false;

    return PULSE_GENERATOR_OK;
}

static size_t mock_get_stream_remaining(void *hw_ptr)
{
    mock_dma_hw_t *hw = (mock_dma_hw_t *)hw_ptr;

    hw->get_stream_remaining_call_count++;

    return (mock_dma_entries(hw) - hw->next_fetch) * hw->layout.words_per_entry;
}

static uint32_t mock_dma_get_tick_hz(void *hw_ptr)
{
    mock_dma_hw_t *hw = (mock_dma_hw_t *)hw_ptr;

    hw->get_tick_hz_call_count++;

    return hw->tick_hz;
}

static uint32_t mock_get_period_max(void *hw_ptr)
{
    mock_dma_hw_t *hw = (mock_dma_hw_t *)hw_ptr;

    hw->get_period_max_call_count++;

    return hw->period_max;
}

static pulse_generator_status_t mock_get_entry_layout(void *hw_ptr, pulse_generator_entry_layout_t *layout)
{
    mock_dma_hw_t *hw = (mock_dma_hw_t *)hw_ptr;

    hw->get_entry_layout_call_count++;

    if (hw->get_entry_layout_result != PULSE_GENERATOR_OK) {
        return hw->get_entry_layout_result;
    }

    *layout = hw->layout;

    return PULSE_GENERATOR_OK;
}

const pulse_generator_ops_t g_mock_dma_ops = {
    .get_tick_hz          = mock_dma_get_tick_hz,
    .stream_start         = mock_stream_start,
    .stream_stop          = mock_stream_stop,
    .get_stream_remaining = mock_get_stream_remaining,
    .get_period_max       = mock_get_period_max,
    .get_entry_layout     = mock_get_entry_layout,
};

void mock_dma_hw_init(mock_dma_hw_t *hw)
{
    *hw = (mock_dma_hw_t){0};

    hw->tick_hz = 2000000;
    hw->period_max = 0xFFFF;
    hw->layout = (pulse_generator_entry_layout_t){
        .words_per_entry = 3,
        .period_index    = 0,
        .compare_index   = 2,
    };
}

static pulse_generator_status_t play(pulse_generator_t *pg, mock_dma_hw_t *hw, size_t k)
{
    pulse_generator_status_t status = PULSE_GENERATOR_OK;

    for (size_t i = 0; i < k && hw->running; i++) {
        const mock_dma_irq_t irq = update_event(hw);

        if (pg == NULL) {
            continue;
        }
        if (irq == MOCK_DMA_IRQ_HALF) {
            status = pulse_generator_notify_dma_half_complete(pg);
        } else if (irq == MOCK_DMA_IRQ_COMPLETE) {
            status = pulse_generator_notify_dma_complete(pg);
        }
    }

    return status;
}

pulse_generator_status_t mock_dma_step(pulse_generator_t *pg, mock_dma_hw_t *hw, size_t k)
{
    return play(pg, hw, k);
}

void mock_dma_advance(mock_dma_hw_t *hw, size_t k)
{
    (void)play(NULL, hw, k);
}

uint32_t mock_dma_entry_word(const mock_dma_hw_t *hw, size_t entry, size_t word)
{
    return hw->buffer[entry * hw->layout.words_per_entry + word];
}
