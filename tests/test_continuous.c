#include "unity.h"
#include "pulse_generator.h"
#include "mock_platform.h"

static mock_hw_t hw;

static int s_complete_count;
static int s_missed_count;

static void test_event_callback(pulse_generator_t *pg, pulse_generator_event_t event, void *user_ctx)
{
    (void)pg;
    (void)user_ctx;

    if (event == PULSE_GENERATOR_EVENT_COMPLETE) {
        s_complete_count++;
    } else {
        s_missed_count++;
    }
}

static void init_instance(pulse_generator_t *pg)
{
    pulse_generator_init(pg, &(pulse_generator_config_t){
        .ops = &g_mock_ops,
        .hw = &hw,
        .on_event = test_event_callback,
    });
}

void setUp(void)
{
    mock_hw_init(&hw);
    s_complete_count = 0;
    s_missed_count = 0;
}

void tearDown(void) {}

static void test_start_continuous_when_idle_arms_hardware_and_returns_ok(void)
{
    pulse_generator_t pg;
    init_instance(&pg);

    pulse_generator_status_t status =
        pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));
    TEST_ASSERT_TRUE(hw.channel_running);
    TEST_ASSERT_EQUAL_UINT32(1000, hw.first_compare);
}

static void test_start_continuous_with_null_pg_returns_invalid_param(void)
{
    pulse_generator_status_t status =
        pulse_generator_start_continuous(NULL, PULSE_GENERATOR_BACKEND_TIMER, 1000);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
}

static void test_start_continuous_when_already_running_returns_invalid_state(void)
{
    pulse_generator_t pg;
    init_instance(&pg);
    pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000);

    pulse_generator_status_t status =
        pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 500);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, status);
    TEST_ASSERT_EQUAL(1, hw.channel_start_call_count);
    TEST_ASSERT_EQUAL_UINT32(1000, hw.first_compare);
}

static void test_start_continuous_with_bitbang_backend_is_not_supported(void)
{
    pulse_generator_t pg;
    init_instance(&pg);

    pulse_generator_status_t status =
        pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_BITBANG, 1000);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_NOT_SUPPORTED, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_FALSE(hw.channel_running);
}

static void test_start_continuous_with_zero_frequency_is_rejected(void)
{
    pulse_generator_t pg;
    init_instance(&pg);

    pulse_generator_status_t status =
        pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 0);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_FALSE(hw.channel_running);
}

static void test_notify_compare_match_counts_pulses_without_auto_stop(void)
{
    pulse_generator_t pg;
    init_instance(&pg);
    pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000);

    for (int i = 0; i < 100 * 2; i++) {
        mock_hw_fire_and_notify(&pg, &hw);
    }

    TEST_ASSERT_EQUAL_UINT32(100, pulse_generator_get_pulse_count(&pg));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));
    TEST_ASSERT_TRUE(hw.channel_running);
    TEST_ASSERT_EQUAL(0, s_complete_count);

    /* 200 edges a thousand ticks apart, starting at 1000: no missed compare
       anywhere along the way, including across the 16-bit wrap. */
    TEST_ASSERT_EQUAL(0, s_missed_count);
    TEST_ASSERT_EQUAL_UINT32((201u * 1000u) & 0xFFFFu, hw.last_compare);
}

static void test_set_frequency_applies_new_half_period_from_next_edge(void)
{
    pulse_generator_t pg;
    init_instance(&pg);
    pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000);
    int tick_hz_reads_after_start = hw.get_tick_hz_call_count;

    pulse_generator_status_t status = pulse_generator_set_frequency(&pg, 2000);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));

    /* Touches no hardware at all: not the compare register, and not even the
       tick rate, which was captured when the movement started. */
    TEST_ASSERT_EQUAL(0, hw.set_compare_call_count);
    TEST_ASSERT_EQUAL(tick_hz_reads_after_start, hw.get_tick_hz_call_count);

    /* The edge already armed still lands at 1000; the new half period of 500
       applies from there. */
    mock_hw_fire_and_notify(&pg, &hw);
    TEST_ASSERT_EQUAL_UINT32(1500, hw.last_compare);
}

static void test_set_frequency_preserves_pulse_count(void)
{
    pulse_generator_t pg;
    init_instance(&pg);
    pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000);
    for (int i = 0; i < 3 * 2; i++) {
        mock_hw_fire_and_notify(&pg, &hw);
    }

    pulse_generator_set_frequency(&pg, 2000);
    TEST_ASSERT_EQUAL_UINT32(3, pulse_generator_get_pulse_count(&pg));

    mock_hw_fire_and_notify(&pg, &hw);
    mock_hw_fire_and_notify(&pg, &hw);
    TEST_ASSERT_EQUAL_UINT32(4, pulse_generator_get_pulse_count(&pg));
}

static void test_set_frequency_when_idle_returns_invalid_state(void)
{
    pulse_generator_t pg;
    init_instance(&pg);

    pulse_generator_status_t status = pulse_generator_set_frequency(&pg, 2000);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
}

static void test_set_frequency_during_fixed_count_returns_invalid_state(void)
{
    pulse_generator_t pg;
    init_instance(&pg);
    pulse_generator_start_fixed_count(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000, 10);

    pulse_generator_status_t status = pulse_generator_set_frequency(&pg, 2000);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, status);
    mock_hw_fire_and_notify(&pg, &hw);
    TEST_ASSERT_EQUAL_UINT32(2000, hw.last_compare);
}

static void test_set_frequency_with_zero_frequency_is_rejected(void)
{
    pulse_generator_t pg;
    init_instance(&pg);
    pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000);

    pulse_generator_status_t status = pulse_generator_set_frequency(&pg, 0);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));

    mock_hw_fire_and_notify(&pg, &hw);
    TEST_ASSERT_EQUAL_UINT32(2000, hw.last_compare);
}

static void test_set_frequency_with_null_pg_returns_invalid_param(void)
{
    pulse_generator_status_t status = pulse_generator_set_frequency(NULL, 2000);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
}

static void test_a_late_interrupt_is_reported_and_the_movement_keeps_running(void)
{
    pulse_generator_t pg;
    init_instance(&pg);
    pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000);
    mock_hw_fire_and_notify(&pg, &hw);

    /* A full half period of interrupt latency: by the time the library writes
       the next compare, the counter has already reached it. */
    hw.isr_latency_ticks = 1000;

    pulse_generator_status_t status = mock_hw_fire_and_notify(&pg, &hw);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_MISSED_COMPARE, status);
    TEST_ASSERT_EQUAL(1, s_missed_count);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));
    TEST_ASSERT_TRUE(hw.channel_running);
    TEST_ASSERT_EQUAL_UINT32(1, pulse_generator_get_pulse_count(&pg));

    /* Rescheduled half a period after the counter, not after the match that
       was already passed. */
    TEST_ASSERT_EQUAL_UINT32(4000, hw.last_compare);
}

static void test_stop_during_continuous_stops_channel_and_resets_count(void)
{
    pulse_generator_t pg;
    init_instance(&pg);
    pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000);
    mock_hw_fire_and_notify(&pg, &hw);
    mock_hw_fire_and_notify(&pg, &hw);

    pulse_generator_status_t status = pulse_generator_stop(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_FALSE(hw.channel_running);
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));
    TEST_ASSERT_EQUAL(0, s_complete_count);
}

static void test_start_continuous_with_frequency_out_of_range_is_rejected(void)
{
    pulse_generator_t pg;
    init_instance(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM,
                      pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 15));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM,
                      pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 2000000));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_FALSE(hw.channel_running);
}

static void test_start_continuous_at_the_counter_maximum_is_accepted(void)
{
    pulse_generator_t pg;

    /* An 8-bit counter at 510 Hz: 1 Hz maps to exactly 255 ticks, the
       largest half period this counter can schedule. */
    hw.counter_max = 0xFF;
    hw.tick_hz = 510;
    init_instance(&pg);

    pulse_generator_status_t status =
        pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, status);
    TEST_ASSERT_EQUAL_UINT32(255, hw.first_compare);
}

static void test_start_continuous_at_one_tick_is_accepted(void)
{
    pulse_generator_t pg;
    init_instance(&pg);

    /* 2 MHz / (2 * 1 MHz) = 1 tick */
    pulse_generator_status_t status =
        pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000000);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, status);
    TEST_ASSERT_EQUAL_UINT32(1, hw.first_compare);
}

static void test_set_frequency_out_of_range_is_rejected_and_keeps_previous(void)
{
    pulse_generator_t pg;
    init_instance(&pg);
    pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_set_frequency(&pg, 15));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_set_frequency(&pg, 2000000));

    mock_hw_fire_and_notify(&pg, &hw);
    TEST_ASSERT_EQUAL_UINT32(2000, hw.last_compare);
}

static void test_start_continuous_when_channel_start_fails_stays_idle(void)
{
    pulse_generator_t pg;
    init_instance(&pg);
    hw.channel_start_result = PULSE_GENERATOR_ERROR;

    pulse_generator_status_t status =
        pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));

    /* A stray interrupt after the failed start must not schedule anything */
    pulse_generator_notify_compare_match(&pg);
    TEST_ASSERT_EQUAL(0, hw.set_compare_call_count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_start_continuous_when_idle_arms_hardware_and_returns_ok);
    RUN_TEST(test_start_continuous_with_null_pg_returns_invalid_param);
    RUN_TEST(test_start_continuous_when_already_running_returns_invalid_state);
    RUN_TEST(test_start_continuous_with_bitbang_backend_is_not_supported);
    RUN_TEST(test_start_continuous_with_zero_frequency_is_rejected);
    RUN_TEST(test_notify_compare_match_counts_pulses_without_auto_stop);
    RUN_TEST(test_set_frequency_applies_new_half_period_from_next_edge);
    RUN_TEST(test_set_frequency_preserves_pulse_count);
    RUN_TEST(test_set_frequency_when_idle_returns_invalid_state);
    RUN_TEST(test_set_frequency_during_fixed_count_returns_invalid_state);
    RUN_TEST(test_set_frequency_with_zero_frequency_is_rejected);
    RUN_TEST(test_set_frequency_with_null_pg_returns_invalid_param);
    RUN_TEST(test_a_late_interrupt_is_reported_and_the_movement_keeps_running);
    RUN_TEST(test_stop_during_continuous_stops_channel_and_resets_count);
    RUN_TEST(test_start_continuous_with_frequency_out_of_range_is_rejected);
    RUN_TEST(test_start_continuous_at_the_counter_maximum_is_accepted);
    RUN_TEST(test_start_continuous_at_one_tick_is_accepted);
    RUN_TEST(test_set_frequency_out_of_range_is_rejected_and_keeps_previous);
    RUN_TEST(test_start_continuous_when_channel_start_fails_stays_idle);
    return UNITY_END();
}
