#include "unity.h"
#include "pulse_generator.h"
#include "mock_platform.h"

static mock_hw_t hw;

static int   s_complete_count;
static int   s_missed_count;
static void *s_event_user_ctx;

static void test_event_callback(pulse_generator_t *pg, pulse_generator_event_t event, void *user_ctx)
{
    (void)pg;

    s_event_user_ctx = user_ctx;

    if (event == PULSE_GENERATOR_EVENT_COMPLETE) {
        s_complete_count++;
    } else {
        s_missed_count++;
    }
}

static void init_instance(pulse_generator_t *pg, void *user_ctx)
{
    pulse_generator_init(pg, &(pulse_generator_config_t){
        .ops = &g_mock_ops,
        .hw = &hw,
        .on_event = test_event_callback,
        .user_ctx = user_ctx,
    });
}

void setUp(void)
{
    mock_hw_init(&hw);
    s_complete_count = 0;
    s_missed_count = 0;
    s_event_user_ctx = NULL;
}

void tearDown(void) {}

static void test_is_busy_returns_false_when_idle(void)
{
    pulse_generator_t pg;
    init_instance(&pg, NULL);

    TEST_ASSERT_FALSE(pulse_generator_is_busy(&pg));
}

static void test_stop_when_idle_is_idempotent(void)
{
    pulse_generator_t pg;
    init_instance(&pg, NULL);

    pulse_generator_status_t status = pulse_generator_stop(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(0, hw.channel_stop_call_count);
}

static void test_start_fixed_count_when_idle_arms_hardware_and_returns_ok(void)
{
    pulse_generator_t pg;
    init_instance(&pg, NULL);

    pulse_generator_status_t status =
        pulse_generator_start_fixed_count(&pg, 1000, 10);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));
    TEST_ASSERT_TRUE(hw.channel_running);

    /* 2 MHz / (2 * 1000 Hz) = 1000 ticks, counted from the counter's current
       value, which mock_hw_init() left at 0. */
    TEST_ASSERT_EQUAL_UINT32(1000, hw.first_compare);
}

static void test_start_fixed_count_when_already_running_returns_invalid_state(void)
{
    pulse_generator_t pg;
    init_instance(&pg, NULL);
    pulse_generator_start_fixed_count(&pg, 1000, 10);

    pulse_generator_status_t status =
        pulse_generator_start_fixed_count(&pg, 500, 5);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, status);
    TEST_ASSERT_EQUAL(1, hw.channel_start_call_count);
    TEST_ASSERT_EQUAL_UINT32(1000, hw.first_compare);
}

static void test_start_fixed_count_with_zero_frequency_is_rejected(void)
{
    pulse_generator_t pg;
    init_instance(&pg, NULL);

    pulse_generator_status_t status =
        pulse_generator_start_fixed_count(&pg, 0, 10);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
}

static void test_start_fixed_count_with_zero_pulse_count_is_rejected(void)
{
    pulse_generator_t pg;
    init_instance(&pg, NULL);

    pulse_generator_status_t status =
        pulse_generator_start_fixed_count(&pg, 1000, 0);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
}

static void test_start_fixed_count_with_pulse_count_overflowing_the_edge_target_is_rejected(void)
{
    pulse_generator_t pg;
    init_instance(&pg, NULL);

    /* Two edges per pulse, so anything past half the range has no edge
       target that fits. */
    pulse_generator_status_t status =
        pulse_generator_start_fixed_count(&pg, 1000,
                                          UINT32_MAX / 2u + 1u);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_FALSE(hw.channel_running);
}

static void test_stop_during_running_movement_does_not_report_completion(void)
{
    pulse_generator_t pg;
    init_instance(&pg, NULL);
    pulse_generator_start_fixed_count(&pg, 1000, 10);

    pulse_generator_status_t status = pulse_generator_stop(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_FALSE(hw.channel_running);
    TEST_ASSERT_EQUAL(0, s_complete_count);
}

static void test_notify_compare_match_two_edges_count_as_one_pulse(void)
{
    pulse_generator_t pg;
    init_instance(&pg, NULL);
    pulse_generator_start_fixed_count(&pg, 1000, 10);

    mock_hw_fire_and_notify(&pg, &hw);
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));

    mock_hw_fire_and_notify(&pg, &hw);
    TEST_ASSERT_EQUAL_UINT32(1, pulse_generator_get_pulse_count(&pg));
}

static void test_notify_compare_match_is_noop_when_idle(void)
{
    pulse_generator_t pg;
    init_instance(&pg, NULL);

    pulse_generator_status_t status = pulse_generator_notify_compare_match(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));
    TEST_ASSERT_EQUAL(0, hw.set_compare_call_count);
}

static void test_notify_compare_match_reaching_target_completes_movement(void)
{
    int dummy_user_ctx;
    pulse_generator_t pg;
    init_instance(&pg, &dummy_user_ctx);
    pulse_generator_start_fixed_count(&pg, 1000, 3);

    for (int i = 0; i < 3 * 2; i++) {
        mock_hw_fire_and_notify(&pg, &hw);
    }

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_FALSE(hw.channel_running);
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));
    TEST_ASSERT_EQUAL(1, s_complete_count);
    TEST_ASSERT_EQUAL_PTR(&dummy_user_ctx, s_event_user_ctx);
}

static void test_completion_without_a_callback_is_harmless(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &(pulse_generator_config_t){ .ops = &g_mock_ops, .hw = &hw });
    pulse_generator_start_fixed_count(&pg, 1000, 1);

    mock_hw_fire_and_notify(&pg, &hw);
    mock_hw_fire_and_notify(&pg, &hw);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(0, s_complete_count);
}

static void test_reset_pulse_count_during_running_delays_completion(void)
{
    pulse_generator_t pg;
    init_instance(&pg, NULL);
    pulse_generator_start_fixed_count(&pg, 1000, 2);

    mock_hw_fire_and_notify(&pg, &hw);
    mock_hw_fire_and_notify(&pg, &hw);
    TEST_ASSERT_EQUAL_UINT32(1, pulse_generator_get_pulse_count(&pg));

    pulse_generator_reset_pulse_count(&pg);
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));

    /* Without the reset, these two edges would have completed the movement. */
    mock_hw_fire_and_notify(&pg, &hw);
    mock_hw_fire_and_notify(&pg, &hw);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(0, s_complete_count);

    /* Now two more do complete it. */
    mock_hw_fire_and_notify(&pg, &hw);
    mock_hw_fire_and_notify(&pg, &hw);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(1, s_complete_count);
}

static void test_get_pulse_count_is_zero_right_after_explicit_stop(void)
{
    pulse_generator_t pg;
    init_instance(&pg, NULL);
    pulse_generator_start_fixed_count(&pg, 1000, 10);
    mock_hw_fire_and_notify(&pg, &hw);
    mock_hw_fire_and_notify(&pg, &hw);
    TEST_ASSERT_EQUAL_UINT32(1, pulse_generator_get_pulse_count(&pg));

    pulse_generator_stop(&pg);

    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));
}

static void test_start_fixed_count_with_frequency_below_range_is_rejected(void)
{
    pulse_generator_t pg;
    init_instance(&pg, NULL);

    /* 2 MHz / (2 * 15 Hz) = 66666 ticks > 0xFFFF */
    pulse_generator_status_t status =
        pulse_generator_start_fixed_count(&pg, 15, 10);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_FALSE(hw.channel_running);
}

static void test_start_fixed_count_with_frequency_above_range_is_rejected(void)
{
    pulse_generator_t pg;
    init_instance(&pg, NULL);

    /* 2 MHz / (2 * 2 MHz) = 0 ticks */
    pulse_generator_status_t status =
        pulse_generator_start_fixed_count(&pg, 2000000, 10);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_FALSE(hw.channel_running);
}

static void test_notify_compare_match_schedules_next_edge(void)
{
    pulse_generator_t pg;
    init_instance(&pg, NULL);
    pulse_generator_start_fixed_count(&pg, 1000, 10);

    mock_hw_fire_and_notify(&pg, &hw);

    TEST_ASSERT_EQUAL(1, hw.set_compare_call_count);

    /* Half a period after the match being serviced, not after "now": the
       first match was at 1000, so the next one is at 2000. */
    TEST_ASSERT_EQUAL_UINT32(2000, hw.last_compare);
}

static void test_final_edge_does_not_schedule_another(void)
{
    pulse_generator_t pg;
    init_instance(&pg, NULL);
    pulse_generator_start_fixed_count(&pg, 1000, 3);

    for (int i = 0; i < 3 * 2; i++) {
        mock_hw_fire_and_notify(&pg, &hw);
    }

    TEST_ASSERT_EQUAL(3 * 2 - 1, hw.set_compare_call_count);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_is_busy_returns_false_when_idle);
    RUN_TEST(test_stop_when_idle_is_idempotent);
    RUN_TEST(test_start_fixed_count_when_idle_arms_hardware_and_returns_ok);
    RUN_TEST(test_start_fixed_count_when_already_running_returns_invalid_state);
    RUN_TEST(test_start_fixed_count_with_zero_frequency_is_rejected);
    RUN_TEST(test_start_fixed_count_with_zero_pulse_count_is_rejected);
    RUN_TEST(test_start_fixed_count_with_pulse_count_overflowing_the_edge_target_is_rejected);
    RUN_TEST(test_stop_during_running_movement_does_not_report_completion);
    RUN_TEST(test_notify_compare_match_two_edges_count_as_one_pulse);
    RUN_TEST(test_notify_compare_match_is_noop_when_idle);
    RUN_TEST(test_notify_compare_match_reaching_target_completes_movement);
    RUN_TEST(test_completion_without_a_callback_is_harmless);
    RUN_TEST(test_reset_pulse_count_during_running_delays_completion);
    RUN_TEST(test_get_pulse_count_is_zero_right_after_explicit_stop);
    RUN_TEST(test_start_fixed_count_with_frequency_below_range_is_rejected);
    RUN_TEST(test_start_fixed_count_with_frequency_above_range_is_rejected);
    RUN_TEST(test_notify_compare_match_schedules_next_edge);
    RUN_TEST(test_final_edge_does_not_schedule_another);
    return UNITY_END();
}
