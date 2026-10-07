#include <string.h>

#include "unity.h"
#include "pulse_generator.h"
#include "mock_platform.h"

#define ENTRIES         8u
#define WORDS_PER_ENTRY 3u

static mock_dma_hw_t hw;
static uint32_t      buffer[ENTRIES * WORDS_PER_ENTRY];
static int           s_complete_count;

static void record_event(pulse_generator_t *pg, pulse_generator_event_t event, void *user_ctx)
{
    (void)pg;
    (void)user_ctx;

    if (event == PULSE_GENERATOR_EVENT_COMPLETE) {
        s_complete_count++;
    }
}

/* 2 MHz tick, 8 entries, 1000 us window: no entry longer than 250 ticks. */
static void init_dma_instance(pulse_generator_t *pg)
{
    pulse_generator_config_t config = {
        .ops      = &g_mock_dma_ops,
        .hw       = &hw,
        .on_event = record_event,
        .dma = {
            .buffer       = buffer,
            .buffer_words = ENTRIES * WORDS_PER_ENTRY,
            .entries      = ENTRIES,
            .window_us    = 1000,
        },
    };
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_init(pg, &config));
}

/* Plays a movement until the stream stops, well past any end it may have. */
static void play_to_the_end(pulse_generator_t *pg)
{
    mock_dma_step(pg, &hw, 1000 * ENTRIES);
    TEST_ASSERT_FALSE(hw.running);
}

void setUp(void)
{
    mock_dma_hw_init(&hw);
    memset(buffer, 0, sizeof buffer);
    s_complete_count = 0;
}

void tearDown(void) {}

/* --- exact count --- */

static void test_emits_exactly_the_pulses_asked_for(void)
{
    const uint32_t counts[] = { 1, 2, 3, 7, 8, 9, 15, 16, 17, 100 };

    for (size_t i = 0; i < sizeof counts / sizeof counts[0]; i++) {
        mock_dma_hw_init(&hw);
        s_complete_count = 0;
        pulse_generator_t pg;
        init_dma_instance(&pg);
        TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, counts[i]));

        play_to_the_end(&pg);

        TEST_ASSERT_EQUAL(counts[i], hw.rise_count);
        TEST_ASSERT_EQUAL(counts[i], hw.fall_count);
        TEST_ASSERT_EQUAL(1, s_complete_count);
    }
}

static void test_emits_exactly_the_pulses_asked_for_when_split(void)
{
    const uint32_t counts[] = { 1, 2, 5 };

    for (size_t i = 0; i < sizeof counts / sizeof counts[0]; i++) {
        mock_dma_hw_init(&hw);
        s_complete_count = 0;
        pulse_generator_t pg;
        init_dma_instance(&pg);
        TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 300, counts[i]));

        play_to_the_end(&pg);

        TEST_ASSERT_EQUAL(counts[i], hw.rise_count);
        TEST_ASSERT_EQUAL(counts[i], hw.fall_count);
        TEST_ASSERT_EQUAL(1, s_complete_count);
    }
}

/* --- exact average frequency --- */

static void test_a_non_integer_period_keeps_the_average_frequency_exact(void)
{
    /* 2 MHz / 3 kHz = 666.67 ticks: periods of 666 and 667 in turn, every
       third rise landing exactly on a multiple of 2000 ticks. */
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 3000, 300));

    play_to_the_end(&pg);

    TEST_ASSERT_EQUAL(300, hw.rise_count);
    for (size_t k = 0; k < 100; k++) {
        TEST_ASSERT_EQUAL_UINT32(2000u * k, hw.rise_ticks[3 * k]);
    }
    for (size_t i = 1; i < 300; i++) {
        const uint32_t period = hw.rise_ticks[i] - hw.rise_ticks[i - 1];
        TEST_ASSERT_TRUE(period == 666 || period == 667);
    }
}

static void test_a_non_integer_split_period_keeps_the_average_frequency_exact(void)
{
    /* 2 MHz / 300 Hz = 6666.67 ticks, split into entries of at most 250. */
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 300, 30));

    play_to_the_end(&pg);

    TEST_ASSERT_EQUAL(30, hw.rise_count);
    for (size_t k = 0; k < 10; k++) {
        TEST_ASSERT_EQUAL_UINT32(20000u * k, hw.rise_ticks[3 * k]);
    }
}

/* --- reset pushes the end back --- */

static void test_reset_pulse_count_pushes_the_end_back(void)
{
    /* Reset at pulse 4 of 20, long before the end has been written. */
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 20));
    mock_dma_step(&pg, &hw, 5);
    const uint32_t at_reset = pulse_generator_get_pulse_count(&pg);
    TEST_ASSERT_EQUAL_UINT32(4, at_reset);

    pulse_generator_reset_pulse_count(&pg);
    play_to_the_end(&pg);

    /* The 4 counted before the reset, then the 20 asked for, back to back. */
    TEST_ASSERT_EQUAL(at_reset + 20, hw.rise_count);
    for (size_t i = 0; i < at_reset + 20; i++) {
        TEST_ASSERT_EQUAL_UINT32(200u * i, hw.rise_ticks[i]);
    }
    TEST_ASSERT_EQUAL(1, s_complete_count);
}

static void test_a_reset_once_the_end_is_in_the_buffer_leaves_a_gap(void)
{
    /* Reset at pulse 4 of 10: the half-transfer at entry 4 has already
       written pulses 9 and 10 and then padding into the half to be played
       next. */
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 10));
    mock_dma_step(&pg, &hw, 5);
    TEST_ASSERT_EQUAL_UINT32(4, pulse_generator_get_pulse_count(&pg));

    pulse_generator_reset_pulse_count(&pg);
    play_to_the_end(&pg);

    /* 10 pulses, a gap, then the 4 that complete the 10 counted from the
       reset. */
    TEST_ASSERT_EQUAL(14, hw.rise_count);
    for (size_t i = 1; i < 10; i++) {
        TEST_ASSERT_EQUAL_UINT32(200, hw.rise_ticks[i] - hw.rise_ticks[i - 1]);
    }
    TEST_ASSERT_TRUE(hw.rise_ticks[10] - hw.rise_ticks[9] > 200);
    for (size_t i = 11; i < 14; i++) {
        TEST_ASSERT_EQUAL_UINT32(200, hw.rise_ticks[i] - hw.rise_ticks[i - 1]);
    }
    TEST_ASSERT_EQUAL(1, s_complete_count);
}

static void test_a_reset_once_the_end_is_playing_leaves_a_gap(void)
{
    /* 3 pulses fill entries 0-2 and padding the rest. By entry 5 the
       half-transfer has refilled entries 0-3 with padding too: the end is
       written, and part of it already playing. */
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 3));
    mock_dma_step(&pg, &hw, 6);
    TEST_ASSERT_EQUAL_UINT32(3, pulse_generator_get_pulse_count(&pg));

    pulse_generator_reset_pulse_count(&pg);
    play_to_the_end(&pg);

    /* 3 pulses, a gap without any, then the 3 asked for again. */
    TEST_ASSERT_EQUAL(6, hw.rise_count);
    TEST_ASSERT_EQUAL(6, hw.fall_count);
    TEST_ASSERT_TRUE(hw.rise_ticks[3] - hw.rise_ticks[2] > 200);
    TEST_ASSERT_EQUAL_UINT32(200, hw.rise_ticks[4] - hw.rise_ticks[3]);
    TEST_ASSERT_EQUAL_UINT32(200, hw.rise_ticks[5] - hw.rise_ticks[4]);
    TEST_ASSERT_EQUAL(1, s_complete_count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_emits_exactly_the_pulses_asked_for);
    RUN_TEST(test_emits_exactly_the_pulses_asked_for_when_split);
    RUN_TEST(test_a_non_integer_period_keeps_the_average_frequency_exact);
    RUN_TEST(test_a_non_integer_split_period_keeps_the_average_frequency_exact);
    RUN_TEST(test_reset_pulse_count_pushes_the_end_back);
    RUN_TEST(test_a_reset_once_the_end_is_in_the_buffer_leaves_a_gap);
    RUN_TEST(test_a_reset_once_the_end_is_playing_leaves_a_gap);
    return UNITY_END();
}
