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

static void test_init_with_any_compare_hook_missing_is_rejected(void)
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
    pulse_generator_start_continuous(&pg, 1000);
    mock_hw_fire_and_notify(&pg, &hw);
    mock_hw_fire_and_notify(&pg, &hw);

    pulse_generator_init(&pg, &(pulse_generator_config_t){ .ops = &g_mock_ops, .hw = &hw });

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));
}

static uint32_t dma_buffer[4 * 3];

static pulse_generator_dma_config_t valid_dma_config(void)
{
    return (pulse_generator_dma_config_t){
        .buffer       = dma_buffer,
        .buffer_words = 4 * 3,
        .entries      = 4,
        .window_us    = 1000,
    };
}

static void test_init_with_any_dma_hook_missing_is_rejected(void)
{
    mock_dma_hw_t dma_hw;
    mock_dma_hw_init(&dma_hw);

    for (int missing = 0; missing < 6; missing++) {
        pulse_generator_ops_t ops = g_mock_dma_ops;

        switch (missing) {
            case 0: ops.stream_start = NULL; break;
            case 1: ops.stream_stop = NULL; break;
            case 2: ops.get_stream_remaining = NULL; break;
            case 3: ops.get_period_max = NULL; break;
            case 4: ops.get_entry_layout = NULL; break;
            default: ops.get_tick_hz = NULL; break;
        }

        pulse_generator_t pg;
        pulse_generator_status_t status = pulse_generator_init(
            &pg, &(pulse_generator_config_t){ .ops = &ops, .hw = &dma_hw, .dma = valid_dma_config() });

        TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
    }
}

static void test_init_with_both_hook_groups_is_rejected(void)
{
    /* Which engine would run is ambiguous, so neither does. */
    pulse_generator_ops_t ops = g_mock_ops;
    ops.stream_start         = g_mock_dma_ops.stream_start;
    ops.stream_stop          = g_mock_dma_ops.stream_stop;
    ops.get_stream_remaining = g_mock_dma_ops.get_stream_remaining;
    ops.get_period_max       = g_mock_dma_ops.get_period_max;
    ops.get_entry_layout     = g_mock_dma_ops.get_entry_layout;

    pulse_generator_t pg;
    pulse_generator_status_t status = pulse_generator_init(
        &pg, &(pulse_generator_config_t){ .ops = &ops, .hw = &hw, .dma = valid_dma_config() });

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
}

static void test_init_with_no_hook_group_is_rejected(void)
{
    pulse_generator_ops_t ops = { .get_tick_hz = g_mock_ops.get_tick_hz };

    pulse_generator_t pg;
    pulse_generator_status_t status = pulse_generator_init(
        &pg, &(pulse_generator_config_t){ .ops = &ops, .hw = &hw });

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
}

static void test_init_with_compare_table_ignores_the_dma_config(void)
{
    pulse_generator_t pg;

    /* entries = 3 would be rejected on the DMA engine. */
    pulse_generator_status_t status = pulse_generator_init(
        &pg, &(pulse_generator_config_t){ .ops = &g_mock_ops, .hw = &hw, .dma = { .entries = 3 } });

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, status);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_with_valid_config_leaves_instance_idle);
    RUN_TEST(test_init_with_null_pg_returns_invalid_param);
    RUN_TEST(test_init_with_null_config_returns_invalid_param);
    RUN_TEST(test_init_with_null_ops_returns_invalid_param);
    RUN_TEST(test_init_with_any_compare_hook_missing_is_rejected);
    RUN_TEST(test_init_accepts_a_platform_without_per_output_state);
    RUN_TEST(test_init_calls_no_hook);
    RUN_TEST(test_init_clears_state_left_by_a_previous_movement);
    RUN_TEST(test_init_with_any_dma_hook_missing_is_rejected);
    RUN_TEST(test_init_with_both_hook_groups_is_rejected);
    RUN_TEST(test_init_with_no_hook_group_is_rejected);
    RUN_TEST(test_init_with_compare_table_ignores_the_dma_config);
    return UNITY_END();
}
