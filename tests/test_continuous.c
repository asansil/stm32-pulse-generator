#include <string.h>

#include "unity.h"
#include "pulse_generator.h"
#include "mock_platform.h"

#define ENTRIES         8u
#define WORDS_PER_ENTRY 3u
#define WINDOW_TICKS    2000u /* 1000 us at 2 MHz */
#define SEGMENT_MAX     250u  /* WINDOW_TICKS / ENTRIES */

static mock_hw_t hw;
static uint32_t  buffer[ENTRIES * WORDS_PER_ENTRY];
static int       s_event_count;

static void record_event(pulse_generator_t *pg, pulse_generator_event_t event, void *user_ctx)
{
    (void)pg;
    (void)event;
    (void)user_ctx;

    s_event_count++;
}

static void init_with_shape(pulse_generator_t *pg, pulse_generator_pulse_shape_t shape, uint32_t width_ns)
{
    pulse_generator_config_t config = {
        .ops      = &g_mock_ops,
        .hw       = &hw,
        .on_event = record_event,
        .dma = {
            .buffer       = buffer,
            .buffer_words = ENTRIES * WORDS_PER_ENTRY,
            .entries      = ENTRIES,
            .window_us    = 1000,
            .pulse_shape  = shape,
            .width_ns     = width_ns,
        },
    };
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_init(pg, &config));
}

/* 2 MHz tick, 8 entries, 1000 us window: no entry longer than 250 ticks. */
static void init_instance(pulse_generator_t *pg)
{
    init_with_shape(pg, PULSE_GENERATOR_PULSE_SHAPE_HALF_PERIOD, 0);
}

static uint32_t rise_interval(size_t i)
{
    return hw.rise_ticks[i] - hw.rise_ticks[i - 1];
}

/* Index of the first rise that does not follow the one before it by
   `period`, i.e. the end of the first period played at another frequency. */
static size_t first_rise_not_after(uint32_t period)
{
    size_t i = 1;
    while (i < hw.rise_count && rise_interval(i) == period) {
        i++;
    }
    TEST_ASSERT_TRUE_MESSAGE(i < hw.rise_count, "the frequency never changed");
    return i;
}

void setUp(void)
{
    mock_hw_init(&hw);
    memset(buffer, 0, sizeof buffer);
    s_event_count = 0;
}

void tearDown(void) {}

/* --- start and lifecycle --- */

static void test_runs_until_stopped_without_any_event(void)
{
    /* 10 kHz = 200 ticks, one entry per period, 1000 laps of the buffer. */
    pulse_generator_t pg;
    init_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg, 10000));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(1, hw.stream_start_call_count);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, mock_step(&pg, &hw, 1000 * ENTRIES));

    TEST_ASSERT_TRUE(hw.running);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(0, s_event_count);
    TEST_ASSERT_EQUAL(1000 * ENTRIES, hw.rise_count);
    for (size_t i = 0; i < MOCK_MAX_EDGES; i++) {
        TEST_ASSERT_EQUAL_UINT32(200u * i, hw.rise_ticks[i]);
        TEST_ASSERT_EQUAL_UINT32(200u * i + 100u, hw.fall_ticks[i]);
    }
}

static void test_start_rejects_a_frequency_out_of_range(void)
{
    /* 0 Hz, and 1.5 MHz: one tick, below the two-tick minimum entry. */
    const uint32_t frequencies[] = { 0, 1500000 };

    for (size_t i = 0; i < sizeof frequencies / sizeof frequencies[0]; i++) {
        pulse_generator_t pg;
        init_instance(&pg);

        TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM,
                          pulse_generator_start_continuous(&pg, frequencies[i]));
        TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    }
    TEST_ASSERT_EQUAL(0, hw.stream_start_call_count);
}

static void test_start_rejects_a_period_not_longer_than_the_fixed_width(void)
{
    /* 50 us = 100 ticks high; 20 kHz is a 100-tick period. */
    pulse_generator_t pg;
    init_with_shape(&pg, PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH, 50000);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_start_continuous(&pg, 20000));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(0, hw.stream_start_call_count);
}

static void test_start_while_running_is_invalid_state(void)
{
    pulse_generator_t pg;
    init_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg, 10000));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, pulse_generator_start_continuous(&pg, 10000));
    TEST_ASSERT_EQUAL(1, hw.stream_start_call_count);
}

static void test_stop_ends_the_movement_and_allows_a_restart(void)
{
    pulse_generator_t pg;
    init_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg, 10000));
    mock_step(&pg, &hw, 20);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_stop(&pg));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(1, hw.stream_stop_call_count);
    TEST_ASSERT_FALSE(hw.running);
    TEST_ASSERT_EQUAL(hw.rise_count, hw.fall_count);
    TEST_ASSERT_EQUAL(0, s_event_count);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg, 10000));
    TEST_ASSERT_EQUAL(2, hw.stream_start_call_count);
}

/* --- exact average frequency --- */

static void test_a_non_integer_period_keeps_the_average_frequency_exact(void)
{
    /* 2 MHz / 3 kHz = 666.67 ticks, split in 3 entries: every third rise
       lands exactly on a multiple of 2000 ticks, over many refills. */
    pulse_generator_t pg;
    init_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg, 3000));

    mock_step(&pg, &hw, 3 * 300);

    TEST_ASSERT_EQUAL(300, hw.rise_count);
    for (size_t k = 0; k < 100; k++) {
        TEST_ASSERT_EQUAL_UINT32(2000u * k, hw.rise_ticks[3 * k]);
    }
    for (size_t i = 1; i < 300; i++) {
        TEST_ASSERT_TRUE(rise_interval(i) == 666 || rise_interval(i) == 667);
    }
}

static void test_a_non_integer_split_period_keeps_the_average_frequency_exact(void)
{
    /* 2 MHz / 300 Hz = 6666.67 ticks, 27 entries each. */
    pulse_generator_t pg;
    init_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg, 300));

    mock_step(&pg, &hw, 27 * 30);

    TEST_ASSERT_EQUAL(30, hw.rise_count);
    for (size_t k = 0; k < 10; k++) {
        TEST_ASSERT_EQUAL_UINT32(20000u * k, hw.rise_ticks[3 * k]);
    }
}

/* --- set_frequency --- */

static void test_set_frequency_reaches_the_pin_within_one_window(void)
{
    /* 10 kHz (200 ticks) to 5 kHz (400 ticks, two entries). */
    pulse_generator_t pg;
    init_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg, 10000));
    mock_step(&pg, &hw, 10);
    const uint32_t call_tick = hw.now_tick;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_set_frequency(&pg, 5000));
    mock_step(&pg, &hw, 200);

    /* Whole periods only: 200 up to the change, 400 from it on, and the
       first 400 starts no later than a window (plus the entry in preload)
       after the call. */
    const size_t change = first_rise_not_after(200);
    for (size_t i = change; i < hw.rise_count; i++) {
        TEST_ASSERT_EQUAL_UINT32(400, rise_interval(i));
    }
    TEST_ASSERT_TRUE(hw.rise_ticks[change - 1] >= call_tick);
    TEST_ASSERT_TRUE(hw.rise_ticks[change - 1] <= call_tick + WINDOW_TICKS + SEGMENT_MAX);
    TEST_ASSERT_TRUE(pulse_generator_is_busy(&pg));
    TEST_ASSERT_EQUAL(0, s_event_count);
}

static void test_set_frequency_lets_a_split_period_finish_whole(void)
{
    /* 500 Hz = 4000 ticks, 16 entries: twice the buffer. The change comes
       while the first period is still being written. */
    pulse_generator_t pg;
    init_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg, 500));
    mock_step(&pg, &hw, 1);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_set_frequency(&pg, 10000));
    mock_step(&pg, &hw, 100);

    TEST_ASSERT_EQUAL_UINT32(0, hw.rise_ticks[0]);
    TEST_ASSERT_EQUAL_UINT32(2000, hw.fall_ticks[0]);
    TEST_ASSERT_EQUAL_UINT32(4000, hw.rise_ticks[1]);
    for (size_t i = 2; i < hw.rise_count; i++) {
        TEST_ASSERT_EQUAL_UINT32(200, rise_interval(i));
    }
}

static void test_set_frequency_keeps_the_average_exact_after_the_change(void)
{
    /* 10 kHz to 3 kHz: from the first new period on, every third rise is
       exactly 2000 ticks after the one three before. */
    pulse_generator_t pg;
    init_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg, 10000));
    mock_step(&pg, &hw, 10);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_set_frequency(&pg, 3000));
    /* Room for the old periods still in the buffer, then 100+ new ones. */
    mock_step(&pg, &hw, 3 * 320);

    const size_t first = first_rise_not_after(200) - 1;
    const uint32_t origin = hw.rise_ticks[first];
    TEST_ASSERT_TRUE(hw.rise_count > first + 3 * 99);
    for (size_t k = 0; k < 100; k++) {
        TEST_ASSERT_EQUAL_UINT32(origin + 2000u * k, hw.rise_ticks[first + 3 * k]);
    }
}

static void test_the_last_of_several_set_frequency_calls_wins(void)
{
    pulse_generator_t pg;
    init_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg, 10000));
    mock_step(&pg, &hw, 10);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_set_frequency(&pg, 2000));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_set_frequency(&pg, 5000));
    mock_step(&pg, &hw, 200);

    /* No 1000-tick period ever plays. */
    const size_t change = first_rise_not_after(200);
    for (size_t i = change; i < hw.rise_count; i++) {
        TEST_ASSERT_EQUAL_UINT32(400, rise_interval(i));
    }
}

static void test_set_frequency_out_of_range_keeps_the_old_one(void)
{
    pulse_generator_t pg;
    init_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg, 10000));
    mock_step(&pg, &hw, 10);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_set_frequency(&pg, 0));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_set_frequency(&pg, 1500000));
    mock_step(&pg, &hw, 100);

    for (size_t i = 1; i < hw.rise_count; i++) {
        TEST_ASSERT_EQUAL_UINT32(200, rise_interval(i));
    }
}

static void test_set_frequency_rejects_a_period_not_longer_than_the_fixed_width(void)
{
    /* 50 us = 100 ticks high: 10 kHz (200 ticks) plays, 20 kHz does not. */
    pulse_generator_t pg;
    init_with_shape(&pg, PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH, 50000);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg, 10000));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_set_frequency(&pg, 20000));
    mock_step(&pg, &hw, 100);

    for (size_t i = 1; i < hw.rise_count; i++) {
        TEST_ASSERT_EQUAL_UINT32(200, rise_interval(i));
    }
}

static void test_set_frequency_without_a_continuous_movement_is_invalid_state(void)
{
    pulse_generator_t pg;
    init_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, pulse_generator_set_frequency(&pg, 1000));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 100));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, pulse_generator_set_frequency(&pg, 1000));
}

/* --- pulse count --- */

static void test_set_frequency_does_not_reset_the_pulse_count(void)
{
    pulse_generator_t pg;
    init_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg, 10000));
    mock_step(&pg, &hw, 10);
    const uint32_t before = pulse_generator_get_pulse_count(&pg);
    TEST_ASSERT_EQUAL_UINT32(9, before);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_set_frequency(&pg, 5000));
    uint32_t previous = before;
    for (size_t i = 0; i < 50; i++) {
        mock_step(&pg, &hw, 1);
        const uint32_t count = pulse_generator_get_pulse_count(&pg);
        TEST_ASSERT_TRUE(count >= previous);
        TEST_ASSERT_TRUE(count <= hw.fall_count && hw.fall_count - count <= 1);
        previous = count;
    }
}

static void test_reset_pulse_count_rebases_the_count_and_keeps_running(void)
{
    pulse_generator_t pg;
    init_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg, 10000));
    mock_step(&pg, &hw, 10);

    pulse_generator_reset_pulse_count(&pg);
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));
    mock_step(&pg, &hw, 10);

    TEST_ASSERT_EQUAL_UINT32(10, pulse_generator_get_pulse_count(&pg));
    TEST_ASSERT_TRUE(hw.running);
    TEST_ASSERT_EQUAL(0, s_event_count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_runs_until_stopped_without_any_event);
    RUN_TEST(test_start_rejects_a_frequency_out_of_range);
    RUN_TEST(test_start_rejects_a_period_not_longer_than_the_fixed_width);
    RUN_TEST(test_start_while_running_is_invalid_state);
    RUN_TEST(test_stop_ends_the_movement_and_allows_a_restart);
    RUN_TEST(test_a_non_integer_period_keeps_the_average_frequency_exact);
    RUN_TEST(test_a_non_integer_split_period_keeps_the_average_frequency_exact);
    RUN_TEST(test_set_frequency_reaches_the_pin_within_one_window);
    RUN_TEST(test_set_frequency_lets_a_split_period_finish_whole);
    RUN_TEST(test_set_frequency_keeps_the_average_exact_after_the_change);
    RUN_TEST(test_the_last_of_several_set_frequency_calls_wins);
    RUN_TEST(test_set_frequency_out_of_range_keeps_the_old_one);
    RUN_TEST(test_set_frequency_rejects_a_period_not_longer_than_the_fixed_width);
    RUN_TEST(test_set_frequency_without_a_continuous_movement_is_invalid_state);
    RUN_TEST(test_set_frequency_does_not_reset_the_pulse_count);
    RUN_TEST(test_reset_pulse_count_rebases_the_count_and_keeps_running);
    return UNITY_END();
}
