#include <string.h>

#include "unity.h"
#include "pulse_generator.h"
#include "mock_platform.h"

#define ENTRIES         8u
#define WORDS_PER_ENTRY 3u  /* the mock's default layout */

static mock_dma_hw_t hw;
static uint32_t      buffer[ENTRIES * 4u]; /* room for a four-word layout too */

static int                     s_complete_count;
static int                     s_underrun_count;
static pulse_generator_state_t s_state_at_event;

/* Reading the state from the callback is a test-only liberty: the header
   allows only the queue functions in there. */
static void record_event(pulse_generator_t *pg, pulse_generator_event_t event, void *user_ctx)
{
    (void)user_ctx;

    s_state_at_event = pulse_generator_get_state(pg);

    if (event == PULSE_GENERATOR_EVENT_COMPLETE) {
        s_complete_count++;
    } else if (event == PULSE_GENERATOR_EVENT_UNDERRUN) {
        s_underrun_count++;
    }
}

/* 2 MHz tick, 8 entries, 1000 us window: no entry longer than 250 ticks. */
static pulse_generator_config_t dma_config(void)
{
    return (pulse_generator_config_t){
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
}

/* Asserted, so that a failing init stops the test here instead of letting
   it go on with an uninitialized instance. */
static void init_dma_instance(pulse_generator_t *pg)
{
    pulse_generator_config_t config = dma_config();
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_init(pg, &config));
}

static void init_with(pulse_generator_t *pg, const pulse_generator_config_t *config)
{
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_init(pg, config));
}

void setUp(void)
{
    mock_dma_hw_init(&hw);
    memset(buffer, 0, sizeof buffer);
    s_complete_count = 0;
    s_underrun_count = 0;
    s_state_at_event = PULSE_GENERATOR_STATE_RUNNING;
}

void tearDown(void) {}

/* --- init and validation --- */

static void test_init_with_valid_dma_config_leaves_instance_idle(void)
{
    pulse_generator_t pg;
    pulse_generator_config_t config = dma_config();

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_init(&pg, &config));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_FALSE(pulse_generator_is_busy(&pg));
}

static void test_init_with_dma_table_calls_no_hook(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);

    TEST_ASSERT_EQUAL(0, hw.stream_start_call_count);
    TEST_ASSERT_EQUAL(0, hw.stream_stop_call_count);
    TEST_ASSERT_EQUAL(0, hw.get_stream_remaining_call_count);
    TEST_ASSERT_EQUAL(0, hw.get_tick_hz_call_count);
    TEST_ASSERT_EQUAL(0, hw.get_period_max_call_count);
    TEST_ASSERT_EQUAL(0, hw.get_entry_layout_call_count);
}

static void test_init_rejects_a_null_buffer(void)
{
    pulse_generator_t pg;
    pulse_generator_config_t config = dma_config();
    config.dma.buffer = NULL;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_init(&pg, &config));
}

static void test_init_rejects_an_odd_number_of_entries(void)
{
    pulse_generator_t pg;
    pulse_generator_config_t config = dma_config();
    config.dma.entries = 7;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_init(&pg, &config));
}

static void test_init_rejects_fewer_than_four_entries(void)
{
    pulse_generator_t pg;
    pulse_generator_config_t config = dma_config();
    config.dma.entries = 2;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_init(&pg, &config));
}

static void test_init_rejects_a_zero_window(void)
{
    pulse_generator_t pg;
    pulse_generator_config_t config = dma_config();
    config.dma.window_us = 0;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_init(&pg, &config));
}

static void test_init_rejects_an_unknown_pulse_shape(void)
{
    pulse_generator_t pg;
    pulse_generator_config_t config = dma_config();
    config.dma.pulse_shape = (pulse_generator_pulse_shape_t)2;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_init(&pg, &config));
}

static void test_init_rejects_fixed_width_without_a_width(void)
{
    pulse_generator_t pg;
    pulse_generator_config_t config = dma_config();
    config.dma.pulse_shape = PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_init(&pg, &config));
}

static void test_init_accepts_fixed_width_with_a_width(void)
{
    pulse_generator_t pg;
    pulse_generator_config_t config = dma_config();
    config.dma.pulse_shape = PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH;
    config.dma.width_ns = 5000;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_init(&pg, &config));
}

/* --- modes the DMA engine does not offer yet --- */

static void test_start_continuous_is_not_supported(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_NOT_SUPPORTED, pulse_generator_start_continuous(&pg, 1000));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(0, hw.stream_start_call_count);
}

static void test_set_frequency_is_not_supported(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_NOT_SUPPORTED, pulse_generator_set_frequency(&pg, 1000));
}

static void test_prepare_scheduled_is_not_supported(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);
    uint32_t queue[4];

    pulse_generator_status_t status = pulse_generator_prepare_scheduled(
        &pg, &(pulse_generator_scheduled_config_t){ .queue = queue, .queue_capacity = 4 });

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_NOT_SUPPORTED, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
}

static void test_get_now_ticks_reads_zero(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);

    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_now_ticks(&pg));
}

static void test_notify_compare_match_is_a_no_op(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_notify_compare_match(&pg));
    TEST_ASSERT_EQUAL(0, hw.stream_stop_call_count);
}

/* --- start: platform, encoding, pulse shape --- */

static void test_start_fixed_count_arms_the_stream_with_the_whole_buffer(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 100));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(1, hw.stream_start_call_count);
    TEST_ASSERT_EQUAL_PTR(buffer, hw.buffer);
    TEST_ASSERT_EQUAL(ENTRIES * WORDS_PER_ENTRY, hw.words);
}

static void test_start_fixed_count_reads_the_platform_once(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 100));

    TEST_ASSERT_EQUAL(1, hw.get_tick_hz_call_count);
    TEST_ASSERT_EQUAL(1, hw.get_period_max_call_count);
    TEST_ASSERT_EQUAL(1, hw.get_entry_layout_call_count);
}

static void test_first_fill_encodes_length_minus_one_and_compare(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);
    memset(buffer, 0xA5, sizeof buffer); /* the unused word must come back as 0 */

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 100));

    for (size_t i = 0; i < ENTRIES; i++) {
        TEST_ASSERT_EQUAL_UINT32(199, mock_dma_entry_word(&hw, i, 0)); /* ARR */
        TEST_ASSERT_EQUAL_UINT32(0,   mock_dma_entry_word(&hw, i, 1)); /* RCR */
        TEST_ASSERT_EQUAL_UINT32(100, mock_dma_entry_word(&hw, i, 2)); /* CCR1 */
    }
}

static void test_half_period_pulses_on_the_timeline(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 100));

    mock_dma_advance(&hw, 3);

    const uint32_t rises[] = { 0, 200, 400 };
    const uint32_t falls[] = { 100, 300, 500 };
    TEST_ASSERT_EQUAL(3, hw.rise_count);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(rises, hw.rise_ticks, 3);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(falls, hw.fall_ticks, 3);
}

static void test_layout_reported_by_the_platform_is_honoured(void)
{
    /* Channel 2 on STM32F4: ARR, RCR, CCR1, CCR2. */
    hw.layout = (pulse_generator_entry_layout_t){ .words_per_entry = 4, .period_index = 0, .compare_index = 3 };
    pulse_generator_config_t config = dma_config();
    config.dma.buffer_words = ENTRIES * 4u;
    pulse_generator_t pg;
    init_with(&pg, &config);
    memset(buffer, 0xA5, sizeof buffer);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 100));

    TEST_ASSERT_EQUAL(ENTRIES * 4u, hw.words);
    for (size_t i = 0; i < ENTRIES; i++) {
        TEST_ASSERT_EQUAL_UINT32(199, mock_dma_entry_word(&hw, i, 0));
        TEST_ASSERT_EQUAL_UINT32(0,   mock_dma_entry_word(&hw, i, 1));
        TEST_ASSERT_EQUAL_UINT32(0,   mock_dma_entry_word(&hw, i, 2));
        TEST_ASSERT_EQUAL_UINT32(100, mock_dma_entry_word(&hw, i, 3));
    }
}

static void test_fixed_width_sets_the_high_time_of_every_pulse(void)
{
    pulse_generator_config_t config = dma_config();
    config.dma.pulse_shape = PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH;
    config.dma.width_ns = 5000; /* 10 ticks at 2 MHz */
    pulse_generator_t pg;
    init_with(&pg, &config);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 100));
    mock_dma_advance(&hw, 2);

    const uint32_t rises[] = { 0, 200 };
    const uint32_t falls[] = { 10, 210 };
    TEST_ASSERT_EQUAL_UINT32_ARRAY(rises, hw.rise_ticks, 2);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(falls, hw.fall_ticks, 2);
}

static void test_fixed_width_is_rounded_up_to_whole_ticks(void)
{
    pulse_generator_config_t config = dma_config();
    config.dma.pulse_shape = PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH;
    config.dma.width_ns = 5100; /* 10.2 ticks */
    pulse_generator_t pg;
    init_with(&pg, &config);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 100));

    TEST_ASSERT_EQUAL_UINT32(11, mock_dma_entry_word(&hw, 0, 2));
}

static void test_stop_padding_follows_the_last_pulse(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 3));

    for (size_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_UINT32(199, mock_dma_entry_word(&hw, i, 0));
        TEST_ASSERT_EQUAL_UINT32(100, mock_dma_entry_word(&hw, i, 2));
    }
    /* Empty entries as long as the window allows, so the end costs as few
       refills as possible. */
    for (size_t i = 3; i < ENTRIES; i++) {
        TEST_ASSERT_EQUAL_UINT32(249, mock_dma_entry_word(&hw, i, 0));
        TEST_ASSERT_EQUAL_UINT32(0,   mock_dma_entry_word(&hw, i, 2));
    }
}

/* --- start: validation --- */

static void assert_start_rejected(pulse_generator_t *pg, uint32_t frequency_hz,
                                  pulse_generator_status_t expected)
{
    TEST_ASSERT_EQUAL(expected, pulse_generator_start_fixed_count(pg, frequency_hz, 100));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(pg));
}

static void test_start_rejects_a_buffer_too_small_for_the_layout(void)
{
    pulse_generator_config_t config = dma_config();
    config.dma.buffer_words = ENTRIES * WORDS_PER_ENTRY - 1;
    pulse_generator_t pg;
    init_with(&pg, &config);

    assert_start_rejected(&pg, 10000, PULSE_GENERATOR_ERROR_INVALID_PARAM);
    TEST_ASSERT_EQUAL(0, hw.stream_start_call_count);
}

static void test_start_rejects_an_invalid_layout(void)
{
    const pulse_generator_entry_layout_t layouts[] = {
        { .words_per_entry = 0, .period_index = 0, .compare_index = 0 }, /* no words */
        { .words_per_entry = 3, .period_index = 3, .compare_index = 2 }, /* period out of the entry */
        { .words_per_entry = 3, .period_index = 0, .compare_index = 3 }, /* compare out of the entry */
        { .words_per_entry = 3, .period_index = 2, .compare_index = 2 }, /* both in one word */
    };

    for (size_t i = 0; i < sizeof layouts / sizeof layouts[0]; i++) {
        hw.layout = layouts[i];
        pulse_generator_t pg;
        init_dma_instance(&pg);

        assert_start_rejected(&pg, 10000, PULSE_GENERATOR_ERROR_INVALID_PARAM);
    }
}

static void test_start_returns_the_platform_error_reading_the_layout(void)
{
    hw.get_entry_layout_result = PULSE_GENERATOR_ERROR;
    pulse_generator_t pg;
    init_dma_instance(&pg);

    assert_start_rejected(&pg, 10000, PULSE_GENERATOR_ERROR);
}

static void test_start_rejects_a_window_under_four_ticks_per_entry(void)
{
    pulse_generator_config_t config = dma_config();
    config.dma.window_us = 15; /* 30 ticks over 8 entries: 3 each, under twice the 2-tick minimum */
    pulse_generator_t pg;
    init_with(&pg, &config);

    assert_start_rejected(&pg, 1000, PULSE_GENERATOR_ERROR_INVALID_PARAM);
}

static void test_start_rejects_a_window_under_the_minimum_entry_per_entry(void)
{
    pulse_generator_config_t config = dma_config();
    config.dma.min_entry_ns = 130000; /* 260 ticks, over the 250 a window entry may last */
    pulse_generator_t pg;
    init_with(&pg, &config);

    assert_start_rejected(&pg, 1000, PULSE_GENERATOR_ERROR_INVALID_PARAM);
}

static void test_start_rejects_a_window_under_twice_the_minimum_entry_per_entry(void)
{
    pulse_generator_config_t config = dma_config();
    config.dma.min_entry_ns = 63000; /* 126 ticks: two of them exceed the 250 an entry may last */
    pulse_generator_t pg;
    init_with(&pg, &config);

    assert_start_rejected(&pg, 1000, PULSE_GENERATOR_ERROR_INVALID_PARAM);
}

static void test_start_rejects_a_period_under_the_minimum_entry(void)
{
    pulse_generator_config_t config = dma_config();
    config.dma.min_entry_ns = 50000; /* 100 ticks */
    pulse_generator_t pg;
    init_with(&pg, &config);

    assert_start_rejected(&pg, 25000, PULSE_GENERATOR_ERROR_INVALID_PARAM); /* 80 ticks */
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 20000, 100)); /* 100 */
}

static void test_start_rejects_a_period_under_two_ticks(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);

    assert_start_rejected(&pg, 2000000, PULSE_GENERATOR_ERROR_INVALID_PARAM); /* 1 tick */
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 1000000, 100)); /* 2 */
}

static void test_start_rejects_a_zero_frequency(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);

    assert_start_rejected(&pg, 0, PULSE_GENERATOR_ERROR_INVALID_PARAM);
}

static void test_fixed_width_rejects_a_period_not_longer_than_the_width(void)
{
    pulse_generator_config_t config = dma_config();
    config.dma.pulse_shape = PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH;
    config.dma.width_ns = 50000; /* 100 ticks */
    pulse_generator_t pg;
    init_with(&pg, &config);

    assert_start_rejected(&pg, 20000, PULSE_GENERATOR_ERROR_INVALID_PARAM); /* 100 ticks: never falls */
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 100)); /* 200 */
}

static void test_start_rejects_a_zero_pulse_count(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_start_fixed_count(&pg, 10000, 0));
}

static void test_start_when_running_returns_invalid_state(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 100));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, pulse_generator_start_fixed_count(&pg, 10000, 100));
    TEST_ASSERT_EQUAL(1, hw.stream_start_call_count);
}

static void test_start_returns_the_platform_error_starting_the_stream(void)
{
    hw.stream_start_result = PULSE_GENERATOR_ERROR;
    pulse_generator_t pg;
    init_dma_instance(&pg);

    assert_start_rejected(&pg, 10000, PULSE_GENERATOR_ERROR);
}

/* --- splitting --- */

static void test_a_long_period_is_split_into_equal_entries(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 1000, 100));

    for (size_t i = 0; i < ENTRIES; i++) {
        TEST_ASSERT_EQUAL_UINT32(249, mock_dma_entry_word(&hw, i, 0));
    }
    /* High for 1000 ticks: the first four entries high throughout (compare
       past ARR), the first three going on into the next (length + 1), the
       fourth falling right at its end (length). The rest empty. */
    for (size_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_UINT32(251, mock_dma_entry_word(&hw, i, 2));
    }
    TEST_ASSERT_EQUAL_UINT32(250, mock_dma_entry_word(&hw, 3, 2));
    for (size_t i = 4; i < ENTRIES; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, mock_dma_entry_word(&hw, i, 2));
    }
}

static void test_a_split_pulse_has_one_rise_and_one_fall(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 1000, 100));

    mock_dma_advance(&hw, ENTRIES);

    TEST_ASSERT_EQUAL(1, hw.rise_count);
    TEST_ASSERT_EQUAL(1, hw.fall_count);
    TEST_ASSERT_EQUAL_UINT32(0, hw.rise_ticks[0]);
    TEST_ASSERT_EQUAL_UINT32(1000, hw.fall_ticks[0]);
    TEST_ASSERT_EQUAL_UINT32(2000, hw.now_tick); /* the period adds up exactly */
}

static void test_an_uneven_split_differs_by_at_most_one_tick(void)
{
    /* 2.001 MHz: the window gives 2001 / 8 = 250-tick entries, and 1 kHz a
       2001-tick period, split into 9 entries: 223 x 3, then 222 x 6. */
    hw.tick_hz = 2001000;
    pulse_generator_t pg;
    init_dma_instance(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 1000, 100));

    const uint32_t arr[ENTRIES] = { 222, 222, 222, 221, 221, 221, 221, 221 };
    /* High for 1000 ticks: entries starting at 0, 223, 446 and 669 are high
       throughout and on into the next (length + 1); the one starting at 891
       falls 109 ticks in. */
    const uint32_t ccr[ENTRIES] = { 224, 224, 224, 223, 109, 0, 0, 0 };
    for (size_t i = 0; i < ENTRIES; i++) {
        TEST_ASSERT_EQUAL_UINT32(arr[i], mock_dma_entry_word(&hw, i, 0));
        TEST_ASSERT_EQUAL_UINT32(ccr[i], mock_dma_entry_word(&hw, i, 2));
    }
}

static void test_entries_fit_the_timer_registers(void)
{
    /* Registers up to 99, under the window's 250: entries of at most 98
       ticks, so that a compare of length + 1 still fits. A 200-tick period
       becomes 67 + 67 + 66. */
    hw.period_max = 99;
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 100));

    mock_dma_advance(&hw, 6);

    for (size_t i = 0; i < ENTRIES; i++) {
        TEST_ASSERT_TRUE(mock_dma_entry_word(&hw, i, 0) <= 99);
        TEST_ASSERT_TRUE(mock_dma_entry_word(&hw, i, 2) <= 99);
    }
    const uint32_t arr[] = { 66, 66, 65 };
    for (size_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_UINT32(arr[i], mock_dma_entry_word(&hw, i, 0));
    }
    const uint32_t rises[] = { 0, 200 };
    const uint32_t falls[] = { 100, 300 };
    TEST_ASSERT_EQUAL_UINT32_ARRAY(rises, hw.rise_ticks, 2);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(falls, hw.fall_ticks, 2);
}

static void test_a_fixed_width_spans_entries(void)
{
    pulse_generator_config_t config = dma_config();
    config.dma.pulse_shape = PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH;
    config.dma.width_ns = 150000; /* 300 ticks: over one 250-tick entry */
    pulse_generator_t pg;
    init_with(&pg, &config);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 1000, 100));

    TEST_ASSERT_EQUAL_UINT32(251, mock_dma_entry_word(&hw, 0, 2)); /* high on into the next entry */
    TEST_ASSERT_EQUAL_UINT32(50,  mock_dma_entry_word(&hw, 1, 2));
    TEST_ASSERT_EQUAL_UINT32(0,   mock_dma_entry_word(&hw, 2, 2));

    mock_dma_advance(&hw, 3);
    TEST_ASSERT_EQUAL_UINT32(300, hw.fall_ticks[0]);
}

/* --- refill --- */

static void test_refills_carry_a_movement_past_the_buffer(void)
{
    /* 10 pulses through an 8-entry buffer: without refills it would replay
       the first 8 forever. */
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 10));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, mock_dma_step(&pg, &hw, 3 * ENTRIES));

    TEST_ASSERT_EQUAL(10, hw.rise_count);
    TEST_ASSERT_EQUAL(10, hw.fall_count);
    for (size_t i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL_UINT32(i * 200, hw.rise_ticks[i]);
        TEST_ASSERT_EQUAL_UINT32(i * 200 + 100, hw.fall_ticks[i]);
    }
}

static void test_a_split_period_continues_across_refills(void)
{
    /* 1 kHz: each period takes the whole 8-entry buffer, so every one of
       them straddles a refill. */
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 1000, 3));

    mock_dma_step(&pg, &hw, 4 * ENTRIES);

    const uint32_t rises[] = { 0, 2000, 4000 };
    const uint32_t falls[] = { 1000, 3000, 5000 };
    TEST_ASSERT_EQUAL(3, hw.rise_count);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(rises, hw.rise_ticks, 3);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(falls, hw.fall_ticks, 3);
}

static void test_a_refill_serviced_late_but_in_time_is_used(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 100));

    /* Half-transfer was raised at entry 4; the stream is at entry 7, still in
       the second half. */
    mock_dma_advance(&hw, 6);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_notify_dma_half_complete(&pg));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(0, s_underrun_count);
}

static void test_a_late_half_transfer_stops_with_underrun_and_writes_nothing(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 100));

    /* A whole lap without servicing anything: the stream is back in the
       first half, the one half-transfer was about. */
    mock_dma_advance(&hw, ENTRIES);
    uint32_t before[ENTRIES * WORDS_PER_ENTRY];
    memcpy(before, buffer, sizeof before);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_UNDERRUN, pulse_generator_notify_dma_half_complete(&pg));

    TEST_ASSERT_EQUAL_UINT32_ARRAY(before, buffer, ENTRIES * WORDS_PER_ENTRY);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(1, hw.stream_stop_call_count);
    TEST_ASSERT_FALSE(hw.running);
    TEST_ASSERT_FALSE(hw.pin_high);
    TEST_ASSERT_EQUAL(1, s_underrun_count);
    TEST_ASSERT_EQUAL(0, s_complete_count);
}

static void test_a_late_transfer_complete_stops_with_underrun(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 100));
    mock_dma_step(&pg, &hw, 3); /* half-transfer serviced on time */

    /* Transfer-complete raised at the end of the lap, but serviced only once
       the stream has reached the second half again. */
    mock_dma_advance(&hw, ENTRIES);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_UNDERRUN, pulse_generator_notify_dma_complete(&pg));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(1, s_underrun_count);
}

static void test_a_movement_can_start_again_after_an_underrun(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 100));
    mock_dma_advance(&hw, ENTRIES);
    pulse_generator_notify_dma_half_complete(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 100));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));
}

static void test_dma_notifications_when_idle_are_a_no_op(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_notify_dma_half_complete(&pg));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_notify_dma_complete(&pg));
    TEST_ASSERT_EQUAL(0, hw.get_stream_remaining_call_count);
    TEST_ASSERT_EQUAL(0, hw.stream_stop_call_count);
}

static void test_dma_notifications_on_the_compare_engine_are_a_no_op(void)
{
    mock_hw_t compare_hw;
    mock_hw_init(&compare_hw);
    pulse_generator_t pg;
    init_with(&pg, &(pulse_generator_config_t){ .ops = &g_mock_ops, .hw = &compare_hw });
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg, 1000));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_notify_dma_half_complete(&pg));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_notify_dma_complete(&pg));
    TEST_ASSERT_TRUE(compare_hw.channel_running);
    TEST_ASSERT_EQUAL(0, compare_hw.channel_stop_call_count);
}

static void test_dma_notifications_with_a_null_instance_return_invalid_param(void)
{
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_notify_dma_half_complete(NULL));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_notify_dma_complete(NULL));
}

/* --- clean end --- */

/* Plays a movement until the stream stops, well past any end it may have. */
static pulse_generator_status_t play_to_the_end(pulse_generator_t *pg)
{
    return mock_dma_step(pg, &hw, 100 * ENTRIES);
}

static void test_fixed_count_completes_after_its_last_pulse(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 3));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, play_to_the_end(&pg));

    TEST_ASSERT_FALSE(hw.running);
    TEST_ASSERT_EQUAL(1, hw.stream_stop_call_count);
    TEST_ASSERT_EQUAL(1, s_complete_count);
    TEST_ASSERT_EQUAL(0, s_underrun_count);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, s_state_at_event);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));

    /* Exactly the pulses asked for, none of them cut by the stop. */
    const uint32_t rises[] = { 0, 200, 400 };
    const uint32_t falls[] = { 100, 300, 500 };
    TEST_ASSERT_EQUAL(3, hw.rise_count);
    TEST_ASSERT_EQUAL(3, hw.fall_count);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(rises, hw.rise_ticks, 3);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(falls, hw.fall_ticks, 3);
    TEST_ASSERT_FALSE(hw.pin_high);
}

static void test_complete_comes_within_one_window_of_the_last_pulse(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 3));

    play_to_the_end(&pg);

    TEST_ASSERT_TRUE(hw.stop_tick >= hw.fall_ticks[2]);
    TEST_ASSERT_TRUE(hw.stop_tick - hw.fall_ticks[2] <= 2000); /* window: 1000 us at 2 MHz */
}

static void test_movements_ending_on_a_half_boundary_complete(void)
{
    /* Half the buffer, the whole buffer, and one pulse past it. */
    const uint32_t counts[] = { ENTRIES / 2, ENTRIES, ENTRIES + 1 };

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
        TEST_ASSERT_FALSE(hw.running);
    }
}

static void test_a_single_pulse_completes(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 1));

    play_to_the_end(&pg);

    TEST_ASSERT_EQUAL(1, hw.rise_count);
    TEST_ASSERT_EQUAL(1, s_complete_count);
}

static void test_a_split_last_pulse_is_not_cut_by_the_end(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 1000, 2));

    play_to_the_end(&pg);

    const uint32_t falls[] = { 1000, 3000 };
    TEST_ASSERT_EQUAL(2, hw.rise_count);
    TEST_ASSERT_EQUAL(2, hw.fall_count);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(falls, hw.fall_ticks, 2);
    TEST_ASSERT_EQUAL(1, s_complete_count);
}

static void test_complete_is_reported_once(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 3));
    play_to_the_end(&pg);

    /* Interrupts still pending when the stream stopped. */
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_notify_dma_half_complete(&pg));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_notify_dma_complete(&pg));

    TEST_ASSERT_EQUAL(1, s_complete_count);
    TEST_ASSERT_EQUAL(1, hw.stream_stop_call_count);
}

static void test_a_movement_can_start_again_after_complete(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 3));
    play_to_the_end(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 5));
    play_to_the_end(&pg);

    TEST_ASSERT_EQUAL(5, hw.rise_count); /* the timeline restarts with the stream */
    TEST_ASSERT_EQUAL(2, s_complete_count);
}

/* --- stop --- */

static void test_stop_halts_the_stream_at_once_without_an_event(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 100));
    mock_dma_step(&pg, &hw, 5);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_stop(&pg));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(1, hw.stream_stop_call_count);
    TEST_ASSERT_FALSE(hw.running);
    TEST_ASSERT_EQUAL(0, s_complete_count);
    TEST_ASSERT_EQUAL(0, s_underrun_count);
}

static void test_stop_forces_a_high_pin_low(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 1000, 100));
    mock_dma_advance(&hw, 2); /* inside the first pulse, high for 1000 ticks */
    TEST_ASSERT_TRUE(hw.pin_high);

    pulse_generator_stop(&pg);

    TEST_ASSERT_FALSE(hw.pin_high);
    TEST_ASSERT_EQUAL(1, hw.fall_count);
    TEST_ASSERT_EQUAL_UINT32(250, hw.fall_ticks[0]); /* cut where the stop landed */
}

static void test_interrupts_pending_at_a_stop_are_ignored(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 100));
    mock_dma_advance(&hw, 3);
    pulse_generator_stop(&pg);
    const int remaining_reads = hw.get_stream_remaining_call_count;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_notify_dma_half_complete(&pg));

    TEST_ASSERT_EQUAL(remaining_reads, hw.get_stream_remaining_call_count);
    TEST_ASSERT_EQUAL(1, hw.stream_stop_call_count);
}

static void test_stop_when_idle_touches_no_hardware(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_stop(&pg));
    TEST_ASSERT_EQUAL(0, hw.stream_stop_call_count);
}

static void test_a_movement_can_start_again_after_a_stop(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 100));
    mock_dma_step(&pg, &hw, 5);
    pulse_generator_stop(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 3));
    play_to_the_end(&pg);

    TEST_ASSERT_EQUAL(3, hw.rise_count);
    TEST_ASSERT_EQUAL(1, s_complete_count);
}

/* --- pulse count --- */

/* What get_pulse_count() should read, taken from the timeline rather than
   from the library: the pulses whose fall had already happened when the
   period being played began, i.e. whose fall lies in a finished entry. */
static uint32_t falls_before_the_active_entry(void)
{
    uint32_t falls = 0;

    for (size_t i = 0; i < hw.fall_count && i < MOCK_DMA_MAX_EDGES; i++) {
        if (hw.fall_ticks[i] <= hw.active_start_tick) {
            falls++;
        }
    }

    return falls;
}

/* One update event at a time, across several laps and every refill. */
static void assert_count_follows_the_timeline(pulse_generator_t *pg, size_t entries_to_play)
{
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(pg));

    for (size_t played = 1; played <= entries_to_play; played++) {
        mock_dma_step(pg, &hw, 1);
        TEST_ASSERT_EQUAL_UINT32(falls_before_the_active_entry(), pulse_generator_get_pulse_count(pg));
    }
}

static void test_pulse_count_follows_one_entry_per_pulse(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 1000));

    assert_count_follows_the_timeline(&pg, 5 * ENTRIES);
    TEST_ASSERT_EQUAL_UINT32(5 * ENTRIES - 1, pulse_generator_get_pulse_count(&pg));
}

static void test_pulse_count_follows_split_pulses(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 1000, 1000));

    assert_count_follows_the_timeline(&pg, 6 * ENTRIES);
}

static void test_pulse_count_follows_an_uneven_split(void)
{
    hw.tick_hz = 2001000; /* 2001-tick periods in 9 entries of 222 or 223 */
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 1000, 1000));

    assert_count_follows_the_timeline(&pg, 6 * ENTRIES);
}

static void test_pulse_count_follows_a_fixed_width_spanning_entries(void)
{
    pulse_generator_config_t config = dma_config();
    config.dma.pulse_shape = PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH;
    config.dma.width_ns = 125000; /* 250 ticks: the fall lands right at an entry's end */
    pulse_generator_t pg;
    init_with(&pg, &config);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 1000, 1000));

    assert_count_follows_the_timeline(&pg, 6 * ENTRIES);
}

static void test_pulse_count_holds_while_a_refill_is_pending(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 1000));

    mock_dma_advance(&hw, 6); /* half-transfer raised at entry 4, not serviced */

    TEST_ASSERT_EQUAL_UINT32(5, pulse_generator_get_pulse_count(&pg));
}

static void test_pulse_count_reads_zero_once_a_movement_ends(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 3));
    play_to_the_end(&pg);
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 100));
    mock_dma_step(&pg, &hw, 10);
    pulse_generator_stop(&pg);
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));
}

static void test_reset_pulse_count_restarts_the_count(void)
{
    pulse_generator_t pg;
    init_dma_instance(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg, 10000, 1000));
    mock_dma_step(&pg, &hw, 10);
    const uint32_t at_reset = falls_before_the_active_entry();

    pulse_generator_reset_pulse_count(&pg);
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));

    for (size_t played = 1; played <= 3 * ENTRIES; played++) {
        mock_dma_step(&pg, &hw, 1);
        TEST_ASSERT_EQUAL_UINT32(falls_before_the_active_entry() - at_reset,
                                 pulse_generator_get_pulse_count(&pg));
    }
}

/* --- several instances --- */

static mock_dma_hw_t hw_b;
static uint32_t      buffer_b[ENTRIES * WORDS_PER_ENTRY];

static void test_two_dma_instances_with_their_own_buffers_do_not_interfere(void)
{
    mock_dma_hw_init(&hw_b);
    pulse_generator_t a;
    pulse_generator_t b;
    init_dma_instance(&a);
    pulse_generator_config_t config_b = dma_config();
    config_b.hw = &hw_b;
    config_b.dma.buffer = buffer_b;
    init_with(&b, &config_b);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&a, 10000, 5));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&b, 1000, 3));

    /* Interleaved update events, as two timers would raise them. */
    for (size_t i = 0; i < 100 * ENTRIES; i++) {
        mock_dma_step(&a, &hw, 1);
        mock_dma_step(&b, &hw_b, 1);
    }

    const uint32_t falls_b[] = { 1000, 3000, 5000 };
    TEST_ASSERT_EQUAL(5, hw.rise_count);
    TEST_ASSERT_EQUAL(3, hw_b.rise_count);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(falls_b, hw_b.fall_ticks, 3);
    TEST_ASSERT_FALSE(hw.running);
    TEST_ASSERT_FALSE(hw_b.running);
    TEST_ASSERT_EQUAL(2, s_complete_count);
}

static void test_a_dma_instance_and_a_compare_instance_coexist(void)
{
    mock_hw_t compare_hw;
    mock_hw_init(&compare_hw);
    pulse_generator_t compare_pg;
    init_with(&compare_pg, &(pulse_generator_config_t){ .ops = &g_mock_ops, .hw = &compare_hw });
    pulse_generator_t dma_pg;
    init_dma_instance(&dma_pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&compare_pg, 1000));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&dma_pg, 10000, 3));

    for (int i = 0; i < 10; i++) {
        mock_hw_fire_and_notify(&compare_pg, &compare_hw);
    }
    play_to_the_end(&dma_pg);

    TEST_ASSERT_EQUAL_UINT32(5, pulse_generator_get_pulse_count(&compare_pg));
    TEST_ASSERT_TRUE(compare_hw.channel_running);
    TEST_ASSERT_EQUAL(3, hw.rise_count);
    TEST_ASSERT_EQUAL(1, s_complete_count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_with_valid_dma_config_leaves_instance_idle);
    RUN_TEST(test_init_with_dma_table_calls_no_hook);
    RUN_TEST(test_init_rejects_a_null_buffer);
    RUN_TEST(test_init_rejects_an_odd_number_of_entries);
    RUN_TEST(test_init_rejects_fewer_than_four_entries);
    RUN_TEST(test_init_rejects_a_zero_window);
    RUN_TEST(test_init_rejects_an_unknown_pulse_shape);
    RUN_TEST(test_init_rejects_fixed_width_without_a_width);
    RUN_TEST(test_init_accepts_fixed_width_with_a_width);
    RUN_TEST(test_start_continuous_is_not_supported);
    RUN_TEST(test_set_frequency_is_not_supported);
    RUN_TEST(test_prepare_scheduled_is_not_supported);
    RUN_TEST(test_get_now_ticks_reads_zero);
    RUN_TEST(test_notify_compare_match_is_a_no_op);
    RUN_TEST(test_start_fixed_count_arms_the_stream_with_the_whole_buffer);
    RUN_TEST(test_start_fixed_count_reads_the_platform_once);
    RUN_TEST(test_first_fill_encodes_length_minus_one_and_compare);
    RUN_TEST(test_half_period_pulses_on_the_timeline);
    RUN_TEST(test_layout_reported_by_the_platform_is_honoured);
    RUN_TEST(test_fixed_width_sets_the_high_time_of_every_pulse);
    RUN_TEST(test_fixed_width_is_rounded_up_to_whole_ticks);
    RUN_TEST(test_stop_padding_follows_the_last_pulse);
    RUN_TEST(test_start_rejects_a_buffer_too_small_for_the_layout);
    RUN_TEST(test_start_rejects_an_invalid_layout);
    RUN_TEST(test_start_returns_the_platform_error_reading_the_layout);
    RUN_TEST(test_start_rejects_a_window_under_four_ticks_per_entry);
    RUN_TEST(test_start_rejects_a_window_under_twice_the_minimum_entry_per_entry);
    RUN_TEST(test_start_rejects_a_window_under_the_minimum_entry_per_entry);
    RUN_TEST(test_start_rejects_a_period_under_the_minimum_entry);
    RUN_TEST(test_start_rejects_a_period_under_two_ticks);
    RUN_TEST(test_start_rejects_a_zero_frequency);
    RUN_TEST(test_fixed_width_rejects_a_period_not_longer_than_the_width);
    RUN_TEST(test_start_rejects_a_zero_pulse_count);
    RUN_TEST(test_start_when_running_returns_invalid_state);
    RUN_TEST(test_start_returns_the_platform_error_starting_the_stream);
    RUN_TEST(test_a_long_period_is_split_into_equal_entries);
    RUN_TEST(test_a_split_pulse_has_one_rise_and_one_fall);
    RUN_TEST(test_an_uneven_split_differs_by_at_most_one_tick);
    RUN_TEST(test_entries_fit_the_timer_registers);
    RUN_TEST(test_a_fixed_width_spans_entries);
    RUN_TEST(test_refills_carry_a_movement_past_the_buffer);
    RUN_TEST(test_a_split_period_continues_across_refills);
    RUN_TEST(test_a_refill_serviced_late_but_in_time_is_used);
    RUN_TEST(test_a_late_half_transfer_stops_with_underrun_and_writes_nothing);
    RUN_TEST(test_a_late_transfer_complete_stops_with_underrun);
    RUN_TEST(test_a_movement_can_start_again_after_an_underrun);
    RUN_TEST(test_dma_notifications_when_idle_are_a_no_op);
    RUN_TEST(test_dma_notifications_on_the_compare_engine_are_a_no_op);
    RUN_TEST(test_dma_notifications_with_a_null_instance_return_invalid_param);
    RUN_TEST(test_fixed_count_completes_after_its_last_pulse);
    RUN_TEST(test_complete_comes_within_one_window_of_the_last_pulse);
    RUN_TEST(test_movements_ending_on_a_half_boundary_complete);
    RUN_TEST(test_a_single_pulse_completes);
    RUN_TEST(test_a_split_last_pulse_is_not_cut_by_the_end);
    RUN_TEST(test_complete_is_reported_once);
    RUN_TEST(test_a_movement_can_start_again_after_complete);
    RUN_TEST(test_stop_halts_the_stream_at_once_without_an_event);
    RUN_TEST(test_stop_forces_a_high_pin_low);
    RUN_TEST(test_interrupts_pending_at_a_stop_are_ignored);
    RUN_TEST(test_stop_when_idle_touches_no_hardware);
    RUN_TEST(test_a_movement_can_start_again_after_a_stop);
    RUN_TEST(test_pulse_count_follows_one_entry_per_pulse);
    RUN_TEST(test_pulse_count_follows_split_pulses);
    RUN_TEST(test_pulse_count_follows_an_uneven_split);
    RUN_TEST(test_pulse_count_follows_a_fixed_width_spanning_entries);
    RUN_TEST(test_pulse_count_holds_while_a_refill_is_pending);
    RUN_TEST(test_pulse_count_reads_zero_once_a_movement_ends);
    RUN_TEST(test_reset_pulse_count_restarts_the_count);
    RUN_TEST(test_two_dma_instances_with_their_own_buffers_do_not_interfere);
    RUN_TEST(test_a_dma_instance_and_a_compare_instance_coexist);
    return UNITY_END();
}
