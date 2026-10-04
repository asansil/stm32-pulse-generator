#include "unity.h"
#include "pulse_generator.h"
#include "mock_platform.h"

static mock_hw_t hw;

void setUp(void)
{
    mock_hw_init(&hw);
}

void tearDown(void) {}

static void test_init_with_valid_config_leaves_instance_idle(void)
{
    pulse_generator_t pg;

    pulse_generator_status_t status = pulse_generator_init(
        &pg, &(pulse_generator_config_t){ .ops = &g_mock_ops, .hw = &hw });

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));
    TEST_ASSERT_FALSE(pulse_generator_is_busy(&pg));
}

static void test_init_with_null_pg_returns_invalid_param(void)
{
    pulse_generator_status_t status = pulse_generator_init(
        NULL, &(pulse_generator_config_t){ .ops = &g_mock_ops, .hw = &hw });

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
}

static void test_init_with_null_config_returns_invalid_param(void)
{
    pulse_generator_t pg;

    pulse_generator_status_t status = pulse_generator_init(&pg, NULL);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
}

static void test_init_with_null_ops_returns_invalid_param(void)
{
    pulse_generator_t pg;

    pulse_generator_status_t status = pulse_generator_init(
        &pg, &(pulse_generator_config_t){ .ops = NULL, .hw = &hw });

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
}

static void test_init_with_any_mandatory_hook_missing_is_rejected(void)
{
    for (int missing = 0; missing < 6; missing++) {
        pulse_generator_ops_t ops = g_mock_ops;

        switch (missing) {
            case 0: ops.channel_start = NULL; break;
            case 1: ops.channel_stop = NULL; break;
            case 2: ops.set_compare = NULL; break;
            case 3: ops.get_counter = NULL; break;
            case 4: ops.get_tick_hz = NULL; break;
            default: ops.get_counter_max = NULL; break;
        }

        pulse_generator_t pg;
        pulse_generator_status_t status = pulse_generator_init(
            &pg, &(pulse_generator_config_t){ .ops = &ops, .hw = &hw });

        TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
    }
}

static void test_init_accepts_ops_without_the_optional_hooks(void)
{
    pulse_generator_t pg;

    pulse_generator_status_t status = pulse_generator_init(
        &pg, &(pulse_generator_config_t){ .ops = &g_mock_ops_minimal, .hw = &hw });

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, status);
}

static void test_init_accepts_a_platform_without_per_output_state(void)
{
    pulse_generator_t pg;

    /* hw == NULL is legal: a platform may keep no per-output state at all. */
    pulse_generator_status_t status = pulse_generator_init(
        &pg, &(pulse_generator_config_t){ .ops = &g_mock_ops, .hw = NULL });

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, status);
}

static void test_init_calls_no_hook(void)
{
    pulse_generator_t pg;

    pulse_generator_init(&pg, &(pulse_generator_config_t){ .ops = &g_mock_ops, .hw = &hw });

    /* The platform is first touched when a movement starts, so init imposes
       no ordering against the integrator's clock and peripheral setup. */
    TEST_ASSERT_EQUAL(0, hw.channel_start_call_count);
    TEST_ASSERT_EQUAL(0, hw.channel_stop_call_count);
    TEST_ASSERT_EQUAL(0, hw.set_compare_call_count);
    TEST_ASSERT_EQUAL(0, hw.get_counter_call_count);
    TEST_ASSERT_EQUAL(0, hw.get_tick_hz_call_count);
    TEST_ASSERT_EQUAL(0, hw.get_counter_max_call_count);
}

static void test_init_clears_state_left_by_a_previous_movement(void)
{
    pulse_generator_t pg;
    pulse_generator_init(&pg, &(pulse_generator_config_t){ .ops = &g_mock_ops, .hw = &hw });
    pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000);
    mock_hw_fire_and_notify(&pg, &hw);
    mock_hw_fire_and_notify(&pg, &hw);

    pulse_generator_init(&pg, &(pulse_generator_config_t){ .ops = &g_mock_ops, .hw = &hw });

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_with_valid_config_leaves_instance_idle);
    RUN_TEST(test_init_with_null_pg_returns_invalid_param);
    RUN_TEST(test_init_with_null_config_returns_invalid_param);
    RUN_TEST(test_init_with_null_ops_returns_invalid_param);
    RUN_TEST(test_init_with_any_mandatory_hook_missing_is_rejected);
    RUN_TEST(test_init_accepts_ops_without_the_optional_hooks);
    RUN_TEST(test_init_accepts_a_platform_without_per_output_state);
    RUN_TEST(test_init_calls_no_hook);
    RUN_TEST(test_init_clears_state_left_by_a_previous_movement);
    return UNITY_END();
}
