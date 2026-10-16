#include <string.h>

#include "unity.h"
#include "pulse_generator.h"
#include "mock_platform.h"

#define ENTRIES         8u
#define WORDS_PER_ENTRY 3u  /* the mock's default layout */

static mock_hw_t hw;
static uint32_t  buffer[ENTRIES * WORDS_PER_ENTRY];
static uint32_t  queue[8];

/* 2 MHz tick, 8 entries, 1000 us window: a 10 kHz period is 200 ticks, and
   a 5000 ns width is 10. */
static pulse_generator_config_t config_with_shape(pulse_generator_pulse_shape_t shape, uint32_t width_ns)
{
    return (pulse_generator_config_t){
        .ops = &g_mock_ops,
        .hw  = &hw,
        .pulse = {
            .shape    = shape,
            .width_ns = width_ns,
        },
        .dma = {
            .buffer       = buffer,
            .buffer_words = ENTRIES * WORDS_PER_ENTRY,
            .entries      = ENTRIES,
            .window_us    = 1000,
        },
    };
}

/* Asserted, so that a failing init stops the test here instead of letting
   it go on with an uninitialized instance. */
static void init_with_shape(pulse_generator_t *pg, pulse_generator_pulse_shape_t shape, uint32_t width_ns)
{
    pulse_generator_config_t config = config_with_shape(shape, width_ns);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_init(pg, &config));
}

static void init_half_period(pulse_generator_t *pg)
{
    init_with_shape(pg, PULSE_GENERATOR_PULSE_SHAPE_HALF_PERIOD, 0);
}

/* Starts a 10 kHz movement and checks the falls of its first two pulses,
   which rise at 0 and 200. */
static void assert_first_falls(pulse_generator_t *pg, uint32_t first, uint32_t second)
{
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(pg, 10000, 100));
    mock_advance(&hw, 2);

    const uint32_t falls[] = { first, second };
    TEST_ASSERT_EQUAL(2, hw.fall_count);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(falls, hw.fall_ticks, 2);
}

void setUp(void)
{
    mock_hw_init(&hw);
    memset(buffer, 0, sizeof buffer);
}

void tearDown(void) {}

/* --- Parameters --- */

static void test_rejects_a_null_instance(void)
{
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM,
                      pulse_generator_set_pulse_shape(NULL, PULSE_GENERATOR_PULSE_SHAPE_HALF_PERIOD, 0));
}

static void test_rejects_an_unknown_shape(void)
{
    pulse_generator_t pg;
    init_half_period(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM,
                      pulse_generator_set_pulse_shape(&pg, (pulse_generator_pulse_shape_t)2, 5000));
}

static void test_rejects_fixed_width_without_a_width(void)
{
    pulse_generator_t pg;
    init_half_period(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM,
                      pulse_generator_set_pulse_shape(&pg, PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH, 0));
}

static void test_a_rejected_call_keeps_the_previous_shape(void)
{
    pulse_generator_t pg;
    init_with_shape(&pg, PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH, 5000);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM,
                      pulse_generator_set_pulse_shape(&pg, PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH, 0));

    assert_first_falls(&pg, 10, 210);
}

/* --- Effect on the next movement --- */

static void test_fixed_width_applies_to_the_next_movement(void)
{
    pulse_generator_t pg;
    init_half_period(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK,
                      pulse_generator_set_pulse_shape(&pg, PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH, 5000));

    assert_first_falls(&pg, 10, 210);
}

static void test_half_period_can_be_set_back(void)
{
    pulse_generator_t pg;
    init_with_shape(&pg, PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH, 5000);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK,
                      pulse_generator_set_pulse_shape(&pg, PULSE_GENERATOR_PULSE_SHAPE_HALF_PERIOD, 0));

    assert_first_falls(&pg, 100, 300);
}

static void test_start_rejects_a_period_not_longer_than_the_new_width(void)
{
    pulse_generator_t pg;
    init_half_period(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK,
                      pulse_generator_set_pulse_shape(&pg, PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH, 100000));

    /* 200 ticks, as long as the width. */
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_start_fixed_count(&pg, 10000, 100));
}

static void test_init_restores_the_shape_of_the_config(void)
{
    pulse_generator_t pg;
    init_half_period(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK,
                      pulse_generator_set_pulse_shape(&pg, PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH, 5000));

    init_half_period(&pg);

    assert_first_falls(&pg, 100, 300);
}

/* --- State --- */

static void test_rejected_while_running_and_the_movement_keeps_its_shape(void)
{
    pulse_generator_t pg;
    init_half_period(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 100));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE,
                      pulse_generator_set_pulse_shape(&pg, PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH, 5000));

    mock_advance(&hw, 2);
    const uint32_t falls[] = { 100, 300 };
    TEST_ASSERT_EQUAL_UINT32_ARRAY(falls, hw.fall_ticks, 2);
}

static void test_rejected_while_armed(void)
{
    pulse_generator_t pg;
    init_half_period(&pg);
    const pulse_generator_scheduled_config_t sched = { .queue = queue, .queue_capacity = 8 };
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_prepare_scheduled(&pg, &sched));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE,
                      pulse_generator_set_pulse_shape(&pg, PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH, 5000));
}

static void test_accepted_again_once_a_movement_completes(void)
{
    pulse_generator_t pg;
    init_half_period(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 2));
    mock_step(&pg, &hw, 1000);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK,
                      pulse_generator_set_pulse_shape(&pg, PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH, 5000));
}

static void test_accepted_again_after_stop(void)
{
    pulse_generator_t pg;
    init_half_period(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg, 10000));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_stop(&pg));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK,
                      pulse_generator_set_pulse_shape(&pg, PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH, 5000));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_rejects_a_null_instance);
    RUN_TEST(test_rejects_an_unknown_shape);
    RUN_TEST(test_rejects_fixed_width_without_a_width);
    RUN_TEST(test_a_rejected_call_keeps_the_previous_shape);

    RUN_TEST(test_fixed_width_applies_to_the_next_movement);
    RUN_TEST(test_half_period_can_be_set_back);
    RUN_TEST(test_start_rejects_a_period_not_longer_than_the_new_width);
    RUN_TEST(test_init_restores_the_shape_of_the_config);

    RUN_TEST(test_rejected_while_running_and_the_movement_keeps_its_shape);
    RUN_TEST(test_rejected_while_armed);
    RUN_TEST(test_accepted_again_once_a_movement_completes);
    RUN_TEST(test_accepted_again_after_stop);

    return UNITY_END();
}
