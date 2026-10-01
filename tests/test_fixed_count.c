#include "unity.h"
#include "pulse_generator.h"
#include "mock_platform.h"

static int s_complete_cb_call_count;
static void *s_complete_cb_user_ctx;

static void test_complete_callback(void *user_ctx)
{
    s_complete_cb_call_count++;
    s_complete_cb_user_ctx = user_ctx;
}

void setUp(void)
{
    mock_platform_reset();
    s_complete_cb_call_count = 0;
    s_complete_cb_user_ctx = NULL;
}

void tearDown(void) {}

static void test_is_busy_returns_false_when_idle(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);

    TEST_ASSERT_FALSE(pulse_generator_is_busy(&pg));
}

static void test_set_complete_callback_with_null_pg_returns_invalid_param(void)
{
    pulse_generator_status_t status = pulse_generator_set_complete_callback(NULL, NULL, NULL);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
}

static void test_stop_when_idle_is_idempotent(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);

    pulse_generator_status_t status = pulse_generator_stop(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
}

static void test_start_fixed_count_when_idle_arms_hardware_and_returns_ok(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);

    pulse_generator_status_t status =
        pulse_generator_start_fixed_count(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000, 10);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));
    TEST_ASSERT_TRUE(g_mock_platform_ctx.timer_running);
    TEST_ASSERT_EQUAL_UINT32(1000, g_mock_platform_ctx.start_ticks);
}

static void test_start_fixed_count_when_already_running_returns_invalid_state(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);
    pulse_generator_start_fixed_count(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000, 10);

    pulse_generator_status_t status =
        pulse_generator_start_fixed_count(&pg, PULSE_GENERATOR_BACKEND_TIMER, 500, 5);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, status);
    TEST_ASSERT_EQUAL_UINT32(1000, g_mock_platform_ctx.start_ticks);
}

static void test_start_fixed_count_with_bitbang_backend_is_rejected(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);

    pulse_generator_status_t status =
        pulse_generator_start_fixed_count(&pg, PULSE_GENERATOR_BACKEND_BITBANG, 1000, 10);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_FALSE(g_mock_platform_ctx.timer_running);
}

static void test_start_fixed_count_with_zero_frequency_is_rejected(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);

    pulse_generator_status_t status =
        pulse_generator_start_fixed_count(&pg, PULSE_GENERATOR_BACKEND_TIMER, 0, 10);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
}

static void test_start_fixed_count_with_zero_pulse_count_is_rejected(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);

    pulse_generator_status_t status =
        pulse_generator_start_fixed_count(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000, 0);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
}

static void test_stop_during_running_movement_does_not_invoke_callback(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);
    pulse_generator_set_complete_callback(&pg, test_complete_callback, NULL);
    pulse_generator_start_fixed_count(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000, 10);

    pulse_generator_status_t status = pulse_generator_stop(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_FALSE(g_mock_platform_ctx.timer_running);
    TEST_ASSERT_EQUAL(0, s_complete_cb_call_count);
}

static void test_notify_compare_match_two_toggles_count_as_one_pulse(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);
    pulse_generator_start_fixed_count(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000, 10);

    pulse_generator_notify_compare_match(&pg);
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));

    pulse_generator_notify_compare_match(&pg);
    TEST_ASSERT_EQUAL_UINT32(1, pulse_generator_get_pulse_count(&pg));
}

static void test_notify_compare_match_is_noop_when_idle(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);

    pulse_generator_status_t status = pulse_generator_notify_compare_match(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));
}

static void test_notify_compare_match_reaching_target_completes_movement(void)
{
    int dummy_user_ctx;
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);
    pulse_generator_set_complete_callback(&pg, test_complete_callback, &dummy_user_ctx);
    pulse_generator_start_fixed_count(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000, 3);

    for (int i = 0; i < 3 * 2; i++) {
        pulse_generator_notify_compare_match(&pg);
    }

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_FALSE(g_mock_platform_ctx.timer_running);
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));
    TEST_ASSERT_EQUAL(1, s_complete_cb_call_count);
    TEST_ASSERT_EQUAL_PTR(&dummy_user_ctx, s_complete_cb_user_ctx);
}

static void test_reset_pulse_count_during_running_delays_completion(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);
    pulse_generator_set_complete_callback(&pg, test_complete_callback, NULL);
    pulse_generator_start_fixed_count(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000, 2);

    pulse_generator_notify_compare_match(&pg);
    pulse_generator_notify_compare_match(&pg);
    TEST_ASSERT_EQUAL_UINT32(1, pulse_generator_get_pulse_count(&pg));

    pulse_generator_reset_pulse_count(&pg);
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));

    /* sin el reset, estos 2 toggles habrian completado el movimiento */
    pulse_generator_notify_compare_match(&pg);
    pulse_generator_notify_compare_match(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(0, s_complete_cb_call_count);

    /* ahora si, 2 toggles mas completan el movimiento */
    pulse_generator_notify_compare_match(&pg);
    pulse_generator_notify_compare_match(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(1, s_complete_cb_call_count);
}

static void test_get_pulse_count_is_zero_right_after_explicit_stop(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);
    pulse_generator_start_fixed_count(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000, 10);
    pulse_generator_notify_compare_match(&pg);
    pulse_generator_notify_compare_match(&pg);
    TEST_ASSERT_EQUAL_UINT32(1, pulse_generator_get_pulse_count(&pg));

    pulse_generator_stop(&pg);

    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));
}

static void test_start_fixed_count_with_frequency_below_range_is_rejected(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);

    /* 2 MHz / (2 * 15 Hz) = 66666 ticks > 0xFFFF */
    pulse_generator_status_t status =
        pulse_generator_start_fixed_count(&pg, PULSE_GENERATOR_BACKEND_TIMER, 15, 10);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_FALSE(g_mock_platform_ctx.timer_running);
}

static void test_start_fixed_count_with_frequency_above_range_is_rejected(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);

    /* 2 MHz / (2 * 2 MHz) = 0 ticks */
    pulse_generator_status_t status =
        pulse_generator_start_fixed_count(&pg, PULSE_GENERATOR_BACKEND_TIMER, 2000000, 10);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_FALSE(g_mock_platform_ctx.timer_running);
}

static void test_notify_compare_match_schedules_next_toggle(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);
    pulse_generator_start_fixed_count(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000, 10);

    pulse_generator_notify_compare_match(&pg);

    TEST_ASSERT_EQUAL(1, g_mock_platform_ctx.advance_compare_call_count);
    TEST_ASSERT_EQUAL_UINT32(1000, g_mock_platform_ctx.last_advance_ticks);
}

static void test_final_toggle_does_not_schedule_another(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);
    pulse_generator_start_fixed_count(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000, 3);

    for (int i = 0; i < 3 * 2; i++) {
        pulse_generator_notify_compare_match(&pg);
    }

    TEST_ASSERT_EQUAL(3 * 2 - 1, g_mock_platform_ctx.advance_compare_call_count);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_is_busy_returns_false_when_idle);
    RUN_TEST(test_set_complete_callback_with_null_pg_returns_invalid_param);
    RUN_TEST(test_stop_when_idle_is_idempotent);
    RUN_TEST(test_start_fixed_count_when_idle_arms_hardware_and_returns_ok);
    RUN_TEST(test_start_fixed_count_when_already_running_returns_invalid_state);
    RUN_TEST(test_start_fixed_count_with_bitbang_backend_is_rejected);
    RUN_TEST(test_start_fixed_count_with_zero_frequency_is_rejected);
    RUN_TEST(test_start_fixed_count_with_zero_pulse_count_is_rejected);
    RUN_TEST(test_stop_during_running_movement_does_not_invoke_callback);
    RUN_TEST(test_notify_compare_match_two_toggles_count_as_one_pulse);
    RUN_TEST(test_notify_compare_match_is_noop_when_idle);
    RUN_TEST(test_notify_compare_match_reaching_target_completes_movement);
    RUN_TEST(test_reset_pulse_count_during_running_delays_completion);
    RUN_TEST(test_get_pulse_count_is_zero_right_after_explicit_stop);
    RUN_TEST(test_start_fixed_count_with_frequency_below_range_is_rejected);
    RUN_TEST(test_start_fixed_count_with_frequency_above_range_is_rejected);
    RUN_TEST(test_notify_compare_match_schedules_next_toggle);
    RUN_TEST(test_final_toggle_does_not_schedule_another);
    return UNITY_END();
}
