#include "unity.h"
#include "pulse_generator.h"
#include "mock_platform.h"

static int s_complete_cb_call_count;

static void test_complete_callback(void *user_ctx)
{
    (void)user_ctx;
    s_complete_cb_call_count++;
}

void setUp(void)
{
    mock_platform_reset();
    s_complete_cb_call_count = 0;
}

void tearDown(void) {}

static void test_start_continuous_when_idle_arms_hardware_and_returns_ok(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);

    pulse_generator_status_t status =
        pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));
    TEST_ASSERT_TRUE(g_mock_platform_ctx.timer_running);
    TEST_ASSERT_EQUAL_UINT32(1000, g_mock_platform_ctx.last_ccr);
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
    pulse_generator_init(&pg, &g_mock_platform);
    pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000);

    pulse_generator_status_t status =
        pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 500);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, status);
    TEST_ASSERT_EQUAL_UINT32(1000, g_mock_platform_ctx.last_ccr);
}

static void test_start_continuous_with_bitbang_backend_is_rejected(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);

    pulse_generator_status_t status =
        pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_BITBANG, 1000);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_FALSE(g_mock_platform_ctx.timer_running);
}

static void test_start_continuous_with_zero_frequency_is_rejected(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);

    pulse_generator_status_t status =
        pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 0);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_FALSE(g_mock_platform_ctx.timer_running);
}

static void test_notify_compare_match_counts_pulses_without_auto_stop(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);
    pulse_generator_set_complete_callback(&pg, test_complete_callback, NULL);
    pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000);

    for (int i = 0; i < 100 * 2; i++) {
        pulse_generator_notify_compare_match(&pg);
    }

    TEST_ASSERT_EQUAL_UINT32(100, pulse_generator_get_pulse_count(&pg));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));
    TEST_ASSERT_TRUE(g_mock_platform_ctx.timer_running);
    TEST_ASSERT_EQUAL(0, s_complete_cb_call_count);
}

static void test_set_frequency_while_running_updates_compare(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);
    pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000);

    pulse_generator_status_t status = pulse_generator_set_frequency(&pg, 2000);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, status);
    TEST_ASSERT_EQUAL_UINT32(500, g_mock_platform_ctx.last_ccr);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));
    TEST_ASSERT_TRUE(g_mock_platform_ctx.timer_running);
}

static void test_set_frequency_preserves_pulse_count(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);
    pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000);
    for (int i = 0; i < 3 * 2; i++) {
        pulse_generator_notify_compare_match(&pg);
    }

    pulse_generator_set_frequency(&pg, 2000);
    TEST_ASSERT_EQUAL_UINT32(3, pulse_generator_get_pulse_count(&pg));

    pulse_generator_notify_compare_match(&pg);
    pulse_generator_notify_compare_match(&pg);
    TEST_ASSERT_EQUAL_UINT32(4, pulse_generator_get_pulse_count(&pg));
}

static void test_set_frequency_when_idle_returns_invalid_state(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);

    pulse_generator_status_t status = pulse_generator_set_frequency(&pg, 2000);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, status);
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_platform_ctx.last_ccr);
}

static void test_set_frequency_during_fixed_count_returns_invalid_state(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);
    pulse_generator_start_fixed_count(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000, 10);

    pulse_generator_status_t status = pulse_generator_set_frequency(&pg, 2000);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, status);
    TEST_ASSERT_EQUAL_UINT32(1000, g_mock_platform_ctx.last_ccr);
}

static void test_set_frequency_with_zero_frequency_is_rejected(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);
    pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000);

    pulse_generator_status_t status = pulse_generator_set_frequency(&pg, 0);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
    TEST_ASSERT_EQUAL_UINT32(1000, g_mock_platform_ctx.last_ccr);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));
}

static void test_set_frequency_with_null_pg_returns_invalid_param(void)
{
    pulse_generator_status_t status = pulse_generator_set_frequency(NULL, 2000);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
}

static void test_set_frequency_when_set_compare_fails_keeps_running_and_count(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);
    pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000);
    pulse_generator_notify_compare_match(&pg);
    pulse_generator_notify_compare_match(&pg);
    g_mock_platform_ctx.set_compare_result = PULSE_GENERATOR_ERROR;

    pulse_generator_status_t status = pulse_generator_set_frequency(&pg, 2000);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR, status);
    TEST_ASSERT_EQUAL_UINT32(1000, g_mock_platform_ctx.last_ccr);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));
    TEST_ASSERT_TRUE(g_mock_platform_ctx.timer_running);
    TEST_ASSERT_EQUAL_UINT32(1, pulse_generator_get_pulse_count(&pg));
}

static void test_stop_during_continuous_stops_timer_resets_count_without_callback(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &g_mock_platform);
    pulse_generator_set_complete_callback(&pg, test_complete_callback, NULL);
    pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000);
    pulse_generator_notify_compare_match(&pg);
    pulse_generator_notify_compare_match(&pg);

    pulse_generator_status_t status = pulse_generator_stop(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_FALSE(g_mock_platform_ctx.timer_running);
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));
    TEST_ASSERT_EQUAL(0, s_complete_cb_call_count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_start_continuous_when_idle_arms_hardware_and_returns_ok);
    RUN_TEST(test_start_continuous_with_null_pg_returns_invalid_param);
    RUN_TEST(test_start_continuous_when_already_running_returns_invalid_state);
    RUN_TEST(test_start_continuous_with_bitbang_backend_is_rejected);
    RUN_TEST(test_start_continuous_with_zero_frequency_is_rejected);
    RUN_TEST(test_notify_compare_match_counts_pulses_without_auto_stop);
    RUN_TEST(test_set_frequency_while_running_updates_compare);
    RUN_TEST(test_set_frequency_preserves_pulse_count);
    RUN_TEST(test_set_frequency_when_idle_returns_invalid_state);
    RUN_TEST(test_set_frequency_during_fixed_count_returns_invalid_state);
    RUN_TEST(test_set_frequency_with_zero_frequency_is_rejected);
    RUN_TEST(test_set_frequency_with_null_pg_returns_invalid_param);
    RUN_TEST(test_set_frequency_when_set_compare_fails_keeps_running_and_count);
    RUN_TEST(test_stop_during_continuous_stops_timer_resets_count_without_callback);
    return UNITY_END();
}
