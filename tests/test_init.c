#include <string.h>

#include "unity.h"
#include "pulse_generator.h"
#include "mock_platform.h"

#define ENTRIES         8u
#define WORDS_PER_ENTRY 3u /* the mock's default layout */

static mock_hw_t hw;
static uint32_t  buffer[ENTRIES * WORDS_PER_ENTRY];

/* 2 MHz tick, 8 entries, 1000 us window. Each test changes only what it is
   about. */
static pulse_generator_config_t valid_config(void)
{
    return (pulse_generator_config_t){
        .ops = &g_mock_ops,
        .hw  = &hw,
        .dma = {
            .buffer       = buffer,
            .buffer_words = ENTRIES * WORDS_PER_ENTRY,
            .entries      = ENTRIES,
            .window_us    = 1000,
        },
    };
}

void setUp(void)
{
    mock_hw_init(&hw);
    memset(buffer, 0, sizeof buffer);
}

void tearDown(void) {}

/* --- Instance and platform --- */

static void test_init_with_valid_config_leaves_instance_idle(void)
{
    pulse_generator_t pg;
    pulse_generator_config_t config = valid_config();

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_init(&pg, &config));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));
    TEST_ASSERT_FALSE(pulse_generator_is_busy(&pg));
}

static void test_init_rejects_a_null_instance(void)
{
    pulse_generator_config_t config = valid_config();

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_init(NULL, &config));
}

static void test_init_rejects_a_null_config(void)
{
    pulse_generator_t pg;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_init(&pg, NULL));
}

static void test_init_rejects_a_null_ops_table(void)
{
    pulse_generator_t pg;
    pulse_generator_config_t config = valid_config();
    config.ops = NULL;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_init(&pg, &config));
}

static void test_init_rejects_an_ops_table_with_any_hook_missing(void)
{
    for (int missing = 0; missing < 6; missing++) {
        pulse_generator_ops_t ops = g_mock_ops;

        switch (missing) {
            case 0: ops.get_tick_hz = NULL; break;
            case 1: ops.stream_start = NULL; break;
            case 2: ops.stream_stop = NULL; break;
            case 3: ops.get_stream_remaining = NULL; break;
            case 4: ops.get_period_max = NULL; break;
            default: ops.get_entry_layout = NULL; break;
        }

        pulse_generator_t pg;
        pulse_generator_config_t config = valid_config();
        config.ops = &ops;

        TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_init(&pg, &config));
    }
}

static void test_init_accepts_a_platform_without_per_output_state(void)
{
    pulse_generator_t pg;
    pulse_generator_config_t config = valid_config();

    /* hw == NULL is legal: a platform may keep no per-output state at all. */
    config.hw = NULL;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_init(&pg, &config));
}

static void test_init_calls_no_hook(void)
{
    pulse_generator_t pg;
    pulse_generator_config_t config = valid_config();

    pulse_generator_init(&pg, &config);

    /* The platform is first touched when a movement starts, so init imposes
       no ordering against the integrator's clock and peripheral setup. */
    TEST_ASSERT_EQUAL(0, hw.stream_start_call_count);
    TEST_ASSERT_EQUAL(0, hw.stream_stop_call_count);
    TEST_ASSERT_EQUAL(0, hw.get_stream_remaining_call_count);
    TEST_ASSERT_EQUAL(0, hw.get_tick_hz_call_count);
    TEST_ASSERT_EQUAL(0, hw.get_period_max_call_count);
    TEST_ASSERT_EQUAL(0, hw.get_entry_layout_call_count);
}

static void test_init_clears_state_left_by_a_previous_movement(void)
{
    pulse_generator_t pg;
    pulse_generator_config_t config = valid_config();
    pulse_generator_init(&pg, &config);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg, 10000));
    mock_step(&pg, &hw, 2 * ENTRIES);

    pulse_generator_init(&pg, &config);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));
}

/* --- DMA config --- */

static void test_init_rejects_a_null_buffer(void)
{
    pulse_generator_t pg;
    pulse_generator_config_t config = valid_config();
    config.dma.buffer = NULL;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_init(&pg, &config));
}

static void test_init_rejects_an_odd_number_of_entries(void)
{
    pulse_generator_t pg;
    pulse_generator_config_t config = valid_config();
    config.dma.entries = 7;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_init(&pg, &config));
}

static void test_init_rejects_fewer_than_four_entries(void)
{
    pulse_generator_t pg;
    pulse_generator_config_t config = valid_config();
    config.dma.entries = 2;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_init(&pg, &config));
}

static void test_init_rejects_a_zero_window(void)
{
    pulse_generator_t pg;
    pulse_generator_config_t config = valid_config();
    config.dma.window_us = 0;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_init(&pg, &config));
}

/* --- Pulse shape --- */

static void test_init_rejects_an_unknown_pulse_shape(void)
{
    pulse_generator_t pg;
    pulse_generator_config_t config = valid_config();
    config.pulse.shape = (pulse_generator_pulse_shape_t)2;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_init(&pg, &config));
}

static void test_init_rejects_fixed_width_without_a_width(void)
{
    pulse_generator_t pg;
    pulse_generator_config_t config = valid_config();
    config.pulse.shape = PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_init(&pg, &config));
}

static void test_init_accepts_fixed_width_with_a_width(void)
{
    pulse_generator_t pg;
    pulse_generator_config_t config = valid_config();
    config.pulse.shape = PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH;
    config.pulse.width_ns = 5000;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_init(&pg, &config));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_with_valid_config_leaves_instance_idle);
    RUN_TEST(test_init_rejects_a_null_instance);
    RUN_TEST(test_init_rejects_a_null_config);
    RUN_TEST(test_init_rejects_a_null_ops_table);
    RUN_TEST(test_init_rejects_an_ops_table_with_any_hook_missing);
    RUN_TEST(test_init_accepts_a_platform_without_per_output_state);
    RUN_TEST(test_init_calls_no_hook);
    RUN_TEST(test_init_clears_state_left_by_a_previous_movement);
    RUN_TEST(test_init_rejects_a_null_buffer);
    RUN_TEST(test_init_rejects_an_odd_number_of_entries);
    RUN_TEST(test_init_rejects_fewer_than_four_entries);
    RUN_TEST(test_init_rejects_a_zero_window);
    RUN_TEST(test_init_rejects_an_unknown_pulse_shape);
    RUN_TEST(test_init_rejects_fixed_width_without_a_width);
    RUN_TEST(test_init_accepts_fixed_width_with_a_width);
    return UNITY_END();
}
