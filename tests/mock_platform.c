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
