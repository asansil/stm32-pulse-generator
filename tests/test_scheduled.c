#include <string.h>

#include "unity.h"
#include "pulse_generator.h"
#include "mock_platform.h"

/* SCHEDULED mode, asserted on the simulated pin's
   timeline: ticks since stream_start, entry 0 (the lead-in) starting at 0. */

#define ENTRIES         8u
#define WORDS_PER_ENTRY 3u /* the mock's default layout */
#define QUEUE_CAPACITY  64u
#define MAX_EVENTS      16

static mock_hw_t         hw;
static pulse_generator_t pg;
static uint32_t          buffer[ENTRIES * 4u]; /* room for a four-word layout too */
static uint32_t          queue[QUEUE_CAPACITY];

typedef struct {
    pulse_generator_event_t event;
    pulse_generator_state_t state; /* the instance's state as the callback saw it */
} recorded_event_t;

static recorded_event_t events[MAX_EVENTS];
static int event_count;

/* When set, LOW_WATERMARK refills the queue from inside the callback, up to
   refills_left times. */
static const uint32_t *refill;
static size_t refill_len;
static int refills_left;

static void record_event(pulse_generator_t *instance, pulse_generator_event_t event, void *user_ctx)
{
    (void)user_ctx;

    /* get_state() is a test-only liberty: the header allows only the queue
       functions in a callback. */
    if (event_count < MAX_EVENTS) {
        events[event_count] = (recorded_event_t){ event, pulse_generator_get_state(instance) };
    }
    event_count++;

    if (event == PULSE_GENERATOR_EVENT_LOW_WATERMARK && refill != NULL && refills_left > 0) {
        pulse_generator_queue_events(instance, refill, refill_len);
        refills_left--;
    }
}

/* 2 MHz tick, 8 entries. A 1000 us window keeps entries at 250 ticks or
   less, so the intervals of the worked example are split; 8000 us allows
   2000 ticks and leaves them whole. */
static pulse_generator_config_t dma_config(uint32_t window_us)
{
    return (pulse_generator_config_t){
        .ops      = &g_mock_ops,
        .hw       = &hw,
        .on_event = record_event,
        .dma = {
            .buffer       = buffer,
            .buffer_words = ENTRIES * WORDS_PER_ENTRY,
            .entries      = ENTRIES,
            .window_us    = window_us,
        },
    };
}

static void init_with(const pulse_generator_config_t *config)
{
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_init(&pg, config));
}

static void init_unsplit(void)
{
    pulse_generator_config_t config = dma_config(8000);
    init_with(&config);
}

void setUp(void)
{
    mock_hw_init(&hw);
    memset(buffer, 0, sizeof buffer);
    event_count = 0;
    refill = NULL;
    refills_left = 0;

    pulse_generator_config_t config = dma_config(1000);
    init_with(&config);
}

void tearDown(void) {}

/* A valid preparation: 63 usable slots, no low watermark. Each
   test changes only what it is about. */
static pulse_generator_scheduled_config_t default_config(void)
{
    return (pulse_generator_scheduled_config_t){
        .queue          = queue,
        .queue_capacity = QUEUE_CAPACITY,
    };
}

static pulse_generator_status_t prepare(const pulse_generator_scheduled_config_t *config)
{
    return pulse_generator_prepare_scheduled(&pg, config);
}

static void prepare_default(void)
{
    pulse_generator_scheduled_config_t config = default_config();
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, prepare(&config));
}

static void prepare_with_low_watermark(size_t low_watermark)
{
    pulse_generator_scheduled_config_t config = default_config();
    config.low_watermark = low_watermark;
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, prepare(&config));
}

static size_t queue_events(const uint32_t *intervals, size_t n)
{
    return pulse_generator_queue_events(&pg, intervals, n);
}

static void queue_all(const uint32_t *intervals, size_t n)
{
    TEST_ASSERT_EQUAL(n, queue_events(intervals, n));
}

static void queue_repeated(uint32_t interval, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        queue_all(&interval, 1);
    }
}

/* The worked example of dma-engine.md §4.1: start at 0, [1000, 1000, 400]. */
static const uint32_t worked_example[] = { 1000, 1000, 400 };
static const uint32_t worked_rises[]   = { 1000, 2000, 2400 };
static const uint32_t worked_falls[]   = { 1500, 2200, 2600 };

static void start_worked_example(bool finish)
{
    queue_all(worked_example, 3);
    if (finish) {
        TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_finish_scheduled(&pg));
    }
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_scheduled(&pg));
}

/* Plays update events until the movement stops itself. */
static pulse_generator_status_t play_to_the_end(void)
{
    pulse_generator_status_t status = mock_step(&pg, &hw, 100000);
    TEST_ASSERT_FALSE_MESSAGE(hw.running, "the movement never ended");
    return status;
}

static void assert_edges(const uint32_t *rises, const uint32_t *falls, size_t n)
{
    TEST_ASSERT_EQUAL(n, hw.rise_count);
    TEST_ASSERT_EQUAL(n, hw.fall_count);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(rises, hw.rise_ticks, n);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(falls, hw.fall_ticks, n);
}

/* Every pulse `period` after the one before, the first `period` after the
   start, high for half of it. */
static void assert_even_train(uint32_t period, size_t n)
{
    TEST_ASSERT_EQUAL(n, hw.rise_count);
    TEST_ASSERT_EQUAL(n, hw.fall_count);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT32(period * (i + 1u), hw.rise_ticks[i]);
        TEST_ASSERT_EQUAL_UINT32(period * (i + 1u) + period / 2u, hw.fall_ticks[i]);
    }
}

static int count_events(pulse_generator_event_t event)
{
    int count = 0;
    for (int i = 0; i < event_count && i < MAX_EVENTS; i++) {
        if (events[i].event == event) {
            count++;
        }
    }

    return count;
}

/* --- prepare_scheduled --- */

static void test_prepare_moves_the_instance_from_idle_to_armed(void)
{
    prepare_default();

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, pulse_generator_get_state(&pg));
    TEST_ASSERT_FALSE(pulse_generator_is_busy(&pg));
    TEST_ASSERT_EQUAL(0, hw.stream_start_call_count);
}

static void test_prepare_reads_the_platform_once_for_the_whole_preparation(void)
{
    prepare_default();

    /* Read here rather than at start, so queue_events() can check intervals
       against the minimum entry before anything runs. */
    TEST_ASSERT_EQUAL(1, hw.get_entry_layout_call_count);
    TEST_ASSERT_EQUAL(1, hw.get_tick_hz_call_count);
    TEST_ASSERT_EQUAL(1, hw.get_period_max_call_count);

    start_worked_example(true);

    TEST_ASSERT_EQUAL(1, hw.get_entry_layout_call_count);
    TEST_ASSERT_EQUAL(1, hw.get_tick_hz_call_count);
    TEST_ASSERT_EQUAL(1, hw.get_period_max_call_count);
}

static void test_prepare_rejects_a_window_too_short_for_the_minimum_entry(void)
{
    /* 250-tick entries against a 200-tick minimum: below twice of it. */
    pulse_generator_config_t config = dma_config(1000);
    config.dma.min_entry_ns = 100000;
    init_with(&config);

    pulse_generator_scheduled_config_t scheduled = default_config();
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, prepare(&scheduled));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
}

static void test_prepare_rejects_a_buffer_too_small_for_the_layout(void)
{
    hw.layout = (pulse_generator_entry_layout_t){ .words_per_entry = 4, .period_index = 0, .compare_index = 3 };

    pulse_generator_scheduled_config_t config = default_config();
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, prepare(&config));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
}

static void test_prepare_reports_the_platform_layout_error(void)
{
    hw.get_entry_layout_result = PULSE_GENERATOR_ERROR;

    pulse_generator_scheduled_config_t config = default_config();
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR, prepare(&config));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
}

/* --- queue_events --- */

static void test_an_interval_below_the_minimum_entry_stops_the_queueing_at_its_index(void)
{
    /* 5 us = 10 ticks. */
    pulse_generator_config_t config = dma_config(1000);
    config.dma.min_entry_ns = 5000;
    init_with(&config);
    prepare_default();

    const uint32_t intervals[] = { 1000, 9, 1000 };
    TEST_ASSERT_EQUAL(1, queue_events(intervals, 3));

    const uint32_t shortest = 10;
    TEST_ASSERT_EQUAL(1, queue_events(&shortest, 1));
}

static void test_a_fixed_width_interval_not_longer_than_the_width_is_rejected(void)
{
    /* 50 us = 100 ticks high. */
    pulse_generator_config_t config = dma_config(1000);
    config.dma.pulse_shape = PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH;
    config.dma.width_ns = 50000;
    init_with(&config);
    prepare_default();

    const uint32_t intervals[] = { 101, 100, 101 };
    TEST_ASSERT_EQUAL(1, queue_events(intervals, 3));
}

static void test_an_interval_above_the_period_register_is_accepted(void)
{
    prepare_default();

    const uint32_t intervals[] = { 0x20000, 0xFFFFFFFF };
    TEST_ASSERT_EQUAL(2, queue_events(intervals, 2));
}

/* --- start_scheduled --- */

static void test_start_runs_the_stream_over_the_whole_buffer(void)
{
    prepare_default();
    start_worked_example(true);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(1, hw.stream_start_call_count);
    TEST_ASSERT_EQUAL_PTR(buffer, hw.buffer);
    TEST_ASSERT_EQUAL(ENTRIES * WORDS_PER_ENTRY, hw.words);
}

static void test_start_needs_a_second_event_to_place_the_first_fall(void)
{
    prepare_default();
    queue_all(worked_example, 1);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, pulse_generator_start_scheduled(&pg));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(1, pulse_generator_get_pending_events(&pg));
    TEST_ASSERT_EQUAL(0, hw.stream_start_call_count);
}

static void test_start_with_a_single_event_is_accepted_once_finished(void)
{
    prepare_default();
    queue_all(worked_example, 1);
    pulse_generator_finish_scheduled(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_scheduled(&pg));
}

static void test_start_with_an_empty_queue_is_rejected_even_once_finished(void)
{
    prepare_default();
    pulse_generator_finish_scheduled(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, pulse_generator_start_scheduled(&pg));
    TEST_ASSERT_EQUAL(0, hw.stream_start_call_count);
}

static void test_start_needs_an_armed_instance(void)
{
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, pulse_generator_start_scheduled(&pg));

    prepare_default();
    start_worked_example(true);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, pulse_generator_start_scheduled(&pg));
    TEST_ASSERT_EQUAL(1, hw.stream_start_call_count);
}

static void test_a_failed_stream_start_leaves_the_instance_armed_with_its_queue(void)
{
    prepare_default();
    queue_all(worked_example, 3);
    pulse_generator_finish_scheduled(&pg);
    hw.stream_start_result = PULSE_GENERATOR_ERROR;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR, pulse_generator_start_scheduled(&pg));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(3, pulse_generator_get_pending_events(&pg));

    /* The same events play from the top once the platform cooperates. */
    hw.stream_start_result = PULSE_GENERATOR_OK;
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_scheduled(&pg));
    play_to_the_end();
    assert_edges(worked_rises, worked_falls, 3);
}

/* --- timing --- */

static void test_the_worked_example_lands_every_edge_on_its_instant(void)
{
    init_unsplit();
    prepare_default();
    start_worked_example(true);

    play_to_the_end();

    assert_edges(worked_rises, worked_falls, 3);
}

static void test_the_worked_example_split_into_entries_lands_on_the_same_edges(void)
{
    prepare_default();
    start_worked_example(true);

    play_to_the_end();

    assert_edges(worked_rises, worked_falls, 3);
}

static void test_a_fall_never_lands_after_the_next_rise_when_the_rate_climbs(void)
{
    prepare_default();
    const uint32_t intervals[] = { 1000, 1000, 400, 100, 40 };
    queue_all(intervals, 5);
    pulse_generator_finish_scheduled(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_scheduled(&pg));

    play_to_the_end();

    const uint32_t rises[] = { 1000, 2000, 2400, 2500, 2540 };
    const uint32_t falls[] = { 1500, 2200, 2450, 2520, 2560 };
    assert_edges(rises, falls, 5);
}

static void test_a_single_finished_event_falls_halfway_through_its_own_interval(void)
{
    prepare_default();
    const uint32_t interval = 600;
    queue_all(&interval, 1);
    pulse_generator_finish_scheduled(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_scheduled(&pg));

    play_to_the_end();

    const uint32_t rises[] = { 600 };
    const uint32_t falls[] = { 900 };
    assert_edges(rises, falls, 1);
}

static void test_fixed_width_pulses_keep_their_width_whatever_the_interval(void)
{
    /* 10 us = 20 ticks high. */
    pulse_generator_config_t config = dma_config(8000);
    config.dma.pulse_shape = PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH;
    config.dma.width_ns = 10000;
    init_with(&config);
    prepare_default();
    start_worked_example(true);

    play_to_the_end();

    const uint32_t falls[] = { 1020, 2020, 2420 };
    assert_edges(worked_rises, falls, 3);
}

static void test_intervals_longer_than_the_period_register_play_whole(void)
{
    prepare_default();
    const uint32_t intervals[] = { 100000, 100000 };
    queue_all(intervals, 2);
    pulse_generator_finish_scheduled(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_scheduled(&pg));

    play_to_the_end();

    const uint32_t rises[] = { 100000, 200000 };
    const uint32_t falls[] = { 150000, 250000 };
    assert_edges(rises, falls, 2);
}

/* --- end of a movement --- */

static void test_a_finished_movement_completes_back_in_armed_with_the_pin_low(void)
{
    prepare_default();
    start_worked_example(true);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, play_to_the_end());

    TEST_ASSERT_EQUAL(1, event_count);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_EVENT_COMPLETE, events[0].event);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, events[0].state);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(0, pulse_generator_get_pending_events(&pg));
    TEST_ASSERT_EQUAL(1, hw.stream_stop_call_count);
    TEST_ASSERT_EQUAL(hw.rise_count, hw.fall_count);
    TEST_ASSERT_EQUAL(0, pulse_generator_get_underrun_count(&pg));
}

static void test_each_movement_needs_its_own_finish(void)
{
    prepare_default();
    start_worked_example(true);
    play_to_the_end();

    start_worked_example(false);
    play_to_the_end();

    TEST_ASSERT_EQUAL(1, count_events(PULSE_GENERATOR_EVENT_COMPLETE));
    TEST_ASSERT_EQUAL(1, count_events(PULSE_GENERATOR_EVENT_UNDERRUN));
}

static void test_a_queue_drained_without_finish_ends_in_an_underrun(void)
{
    prepare_default();
    start_worked_example(false);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_UNDERRUN, play_to_the_end());

    /* The last pulse still falls halfway through its own interval. */
    assert_edges(worked_rises, worked_falls, 3);
    TEST_ASSERT_EQUAL(1, event_count);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_EVENT_UNDERRUN, events[0].event);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, events[0].state);
    TEST_ASSERT_EQUAL(1, pulse_generator_get_underrun_count(&pg));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, pulse_generator_get_state(&pg));
}

static void test_a_movement_that_underran_can_be_restarted(void)
{
    prepare_default();
    start_worked_example(false);
    play_to_the_end();

    start_worked_example(true);
    play_to_the_end();

    assert_edges(worked_rises, worked_falls, 3);
    TEST_ASSERT_EQUAL(1, count_events(PULSE_GENERATOR_EVENT_COMPLETE));
}

static void test_prepare_resets_the_underrun_count(void)
{
    prepare_default();
    start_worked_example(false);
    play_to_the_end();
    TEST_ASSERT_EQUAL(1, pulse_generator_get_underrun_count(&pg));

    pulse_generator_config_t config = dma_config(1000);
    init_with(&config);
    prepare_default();

    TEST_ASSERT_EQUAL(0, pulse_generator_get_underrun_count(&pg));
}

/* --- streaming --- */

static void test_events_queued_while_running_continue_the_train_without_a_seam(void)
{
    /* 200 ticks: one entry per pulse. The start takes 8 events, the
       half-transfer after 4 update events 4 more; the transfer-complete
       would need the 13th. */
    prepare_default();
    queue_repeated(200, 12);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_scheduled(&pg));
    mock_step(&pg, &hw, 4);
    TEST_ASSERT_EQUAL(0, pulse_generator_get_pending_events(&pg));

    queue_repeated(200, 12);
    pulse_generator_finish_scheduled(&pg);
    play_to_the_end();

    assert_even_train(200, 24);
    TEST_ASSERT_EQUAL(1, count_events(PULSE_GENERATOR_EVENT_COMPLETE));
}

static void test_events_queued_after_the_refill_found_the_queue_empty_come_too_late(void)
{
    prepare_default();
    queue_repeated(200, 12);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_scheduled(&pg));
    mock_step(&pg, &hw, 8); /* the transfer-complete found it empty */

    queue_repeated(200, 12);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_UNDERRUN, play_to_the_end());

    assert_even_train(200, 12);
    TEST_ASSERT_EQUAL(1, count_events(PULSE_GENERATOR_EVENT_UNDERRUN));
    TEST_ASSERT_EQUAL(0, pulse_generator_get_pending_events(&pg));
}

/* --- low watermark --- */

static void test_the_low_watermark_fires_once_as_the_queue_drains(void)
{
    /* Pending after each step: 12 after the start, then 8, 4 (fires), 0. */
    prepare_with_low_watermark(6);
    queue_repeated(200, 20);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_scheduled(&pg));

    play_to_the_end();

    TEST_ASSERT_EQUAL(1, count_events(PULSE_GENERATOR_EVENT_LOW_WATERMARK));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, events[0].state);
}

static void test_the_low_watermark_rearms_once_a_refill_reaches_it(void)
{
    prepare_with_low_watermark(6);
    queue_repeated(200, 20);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_scheduled(&pg));
    mock_step(&pg, &hw, 8);
    TEST_ASSERT_EQUAL(1, count_events(PULSE_GENERATOR_EVENT_LOW_WATERMARK));

    /* 12 pending: the next refill leaves 8, which rearms it, and the one
       after leaves 4. */
    queue_repeated(200, 8);
    mock_step(&pg, &hw, 8);

    TEST_ASSERT_EQUAL(2, count_events(PULSE_GENERATOR_EVENT_LOW_WATERMARK));
}

static void test_a_zero_low_watermark_never_fires(void)
{
    prepare_default();
    queue_repeated(200, 20);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_scheduled(&pg));

    play_to_the_end();

    TEST_ASSERT_EQUAL(0, count_events(PULSE_GENERATOR_EVENT_LOW_WATERMARK));
}

static void test_the_low_watermark_is_not_reported_once_finish_is_requested(void)
{
    prepare_with_low_watermark(6);
    queue_repeated(200, 20);
    pulse_generator_finish_scheduled(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_scheduled(&pg));

    play_to_the_end();

    TEST_ASSERT_EQUAL(0, count_events(PULSE_GENERATOR_EVENT_LOW_WATERMARK));
    TEST_ASSERT_EQUAL(1, count_events(PULSE_GENERATOR_EVENT_COMPLETE));
}

static void test_the_queue_can_be_refilled_from_the_low_watermark_callback(void)
{
    /* 16 events up front and three refills of 8 from the callback: one
       seamless train of 40, then the queue runs dry. */
    static const uint32_t chunk[] = { 200, 200, 200, 200, 200, 200, 200, 200 };
    refill = chunk;
    refill_len = 8;
    refills_left = 3;

    prepare_with_low_watermark(6);
    queue_repeated(200, 16);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_scheduled(&pg));

    play_to_the_end();

    assert_even_train(200, 40);
    TEST_ASSERT_EQUAL(0, refills_left);
    TEST_ASSERT_EQUAL(1, count_events(PULSE_GENERATOR_EVENT_UNDERRUN));
}

/* --- late refill and stop --- */

static void test_a_late_refill_stops_the_movement_with_an_underrun_back_in_armed(void)
{
    prepare_default();
    queue_repeated(200, 20);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_scheduled(&pg));

    /* The half-transfer is left pending until the stream is back in the
       first half. */
    mock_advance(&hw, 9);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_UNDERRUN, pulse_generator_notify_dma_half_complete(&pg));
    TEST_ASSERT_FALSE(hw.running);
    TEST_ASSERT_EQUAL(1, event_count);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_EVENT_UNDERRUN, events[0].event);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, events[0].state);
    TEST_ASSERT_EQUAL(1, pulse_generator_get_underrun_count(&pg));
    TEST_ASSERT_EQUAL(0, pulse_generator_get_pending_events(&pg));
}

static void test_stop_mid_movement_lands_in_armed_with_the_queue_flushed(void)
{
    prepare_default();
    queue_repeated(200, 20);
    pulse_generator_finish_scheduled(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_scheduled(&pg));
    mock_step(&pg, &hw, 6);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_stop(&pg));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(0, pulse_generator_get_pending_events(&pg));
    TEST_ASSERT_EQUAL(1, hw.stream_stop_call_count);
    TEST_ASSERT_FALSE(hw.running);
    TEST_ASSERT_EQUAL(0, event_count);

    /* The finish request went with the movement. */
    start_worked_example(false);
    play_to_the_end();
    TEST_ASSERT_EQUAL(1, count_events(PULSE_GENERATOR_EVENT_UNDERRUN));
}

static void test_stop_while_armed_flushes_the_queue_without_touching_the_hardware(void)
{
    prepare_default();
    queue_all(worked_example, 3);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_stop(&pg));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(0, pulse_generator_get_pending_events(&pg));
    TEST_ASSERT_EQUAL(0, hw.stream_stop_call_count);
}

static void test_a_stopped_movement_can_be_restarted(void)
{
    prepare_default();
    queue_repeated(200, 20);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_scheduled(&pg));
    mock_step(&pg, &hw, 6);
    pulse_generator_stop(&pg);

    start_worked_example(true);
    play_to_the_end();

    assert_edges(worked_rises, worked_falls, 3);
}

static void test_a_dma_interrupt_after_stop_is_ignored(void)
{
    prepare_default();
    queue_repeated(200, 20);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_scheduled(&pg));
    mock_step(&pg, &hw, 2);
    pulse_generator_stop(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_notify_dma_half_complete(&pg));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_notify_dma_complete(&pg));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(1, hw.stream_stop_call_count);
    TEST_ASSERT_EQUAL(0, event_count);
}

/* --- pulse count and multi-instance --- */

static void test_the_pulse_count_follows_the_pin(void)
{
    /* One entry per pulse. After k update events entry k-1 plays and entry
       k waits in preload; entries 0 (the lead-in) to k-2 have finished. */
    prepare_default();
    queue_repeated(200, 40);
    pulse_generator_finish_scheduled(&pg);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_scheduled(&pg));

    for (uint32_t k = 1; k <= 30; k++) {
        mock_step(&pg, &hw, 1);
        TEST_ASSERT_EQUAL_UINT32((k >= 2u) ? k - 2u : 0u, pulse_generator_get_pulse_count(&pg));
    }
}

static void test_two_instances_with_their_own_queues_do_not_interfere(void)
{
    static mock_hw_t         hw2;
    static pulse_generator_t pg2;
    static uint32_t          buffer2[ENTRIES * WORDS_PER_ENTRY];
    static uint32_t          queue2[QUEUE_CAPACITY];

    mock_hw_init(&hw2);
    pulse_generator_config_t config = dma_config(1000);
    config.hw = &hw2;
    config.dma.buffer = buffer2;
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_init(&pg2, &config));
    pulse_generator_scheduled_config_t scheduled = default_config();
    scheduled.queue = queue2;
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_prepare_scheduled(&pg2, &scheduled));

    prepare_default();
    start_worked_example(true);

    for (int i = 0; i < 10; i++) {
        const uint32_t interval = 200;
        TEST_ASSERT_EQUAL(1, pulse_generator_queue_events(&pg2, &interval, 1));
    }
    pulse_generator_finish_scheduled(&pg2);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_scheduled(&pg2));

    for (int i = 0; i < 200; i++) {
        mock_step(&pg, &hw, 1);
        mock_step(&pg2, &hw2, 1);
    }

    assert_edges(worked_rises, worked_falls, 3);
    TEST_ASSERT_EQUAL(10, hw2.rise_count);
    for (size_t i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL_UINT32(200u * (i + 1u), hw2.rise_ticks[i]);
    }
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, pulse_generator_get_state(&pg2));
    TEST_ASSERT_EQUAL(2, count_events(PULSE_GENERATOR_EVENT_COMPLETE));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_prepare_moves_the_instance_from_idle_to_armed);
    RUN_TEST(test_prepare_reads_the_platform_once_for_the_whole_preparation);
    RUN_TEST(test_prepare_rejects_a_window_too_short_for_the_minimum_entry);
    RUN_TEST(test_prepare_rejects_a_buffer_too_small_for_the_layout);
    RUN_TEST(test_prepare_reports_the_platform_layout_error);

    RUN_TEST(test_an_interval_below_the_minimum_entry_stops_the_queueing_at_its_index);
    RUN_TEST(test_a_fixed_width_interval_not_longer_than_the_width_is_rejected);
    RUN_TEST(test_an_interval_above_the_period_register_is_accepted);

    RUN_TEST(test_start_runs_the_stream_over_the_whole_buffer);
    RUN_TEST(test_start_needs_a_second_event_to_place_the_first_fall);
    RUN_TEST(test_start_with_a_single_event_is_accepted_once_finished);
    RUN_TEST(test_start_with_an_empty_queue_is_rejected_even_once_finished);
    RUN_TEST(test_start_needs_an_armed_instance);
    RUN_TEST(test_a_failed_stream_start_leaves_the_instance_armed_with_its_queue);

    RUN_TEST(test_the_worked_example_lands_every_edge_on_its_instant);
    RUN_TEST(test_the_worked_example_split_into_entries_lands_on_the_same_edges);
    RUN_TEST(test_a_fall_never_lands_after_the_next_rise_when_the_rate_climbs);
    RUN_TEST(test_a_single_finished_event_falls_halfway_through_its_own_interval);
    RUN_TEST(test_fixed_width_pulses_keep_their_width_whatever_the_interval);
    RUN_TEST(test_intervals_longer_than_the_period_register_play_whole);

    RUN_TEST(test_a_finished_movement_completes_back_in_armed_with_the_pin_low);
    RUN_TEST(test_each_movement_needs_its_own_finish);
    RUN_TEST(test_a_queue_drained_without_finish_ends_in_an_underrun);
    RUN_TEST(test_a_movement_that_underran_can_be_restarted);
    RUN_TEST(test_prepare_resets_the_underrun_count);

    RUN_TEST(test_events_queued_while_running_continue_the_train_without_a_seam);
    RUN_TEST(test_events_queued_after_the_refill_found_the_queue_empty_come_too_late);

    RUN_TEST(test_the_low_watermark_fires_once_as_the_queue_drains);
    RUN_TEST(test_the_low_watermark_rearms_once_a_refill_reaches_it);
    RUN_TEST(test_a_zero_low_watermark_never_fires);
    RUN_TEST(test_the_low_watermark_is_not_reported_once_finish_is_requested);
    RUN_TEST(test_the_queue_can_be_refilled_from_the_low_watermark_callback);

    RUN_TEST(test_a_late_refill_stops_the_movement_with_an_underrun_back_in_armed);
    RUN_TEST(test_stop_mid_movement_lands_in_armed_with_the_queue_flushed);
    RUN_TEST(test_stop_while_armed_flushes_the_queue_without_touching_the_hardware);
    RUN_TEST(test_a_stopped_movement_can_be_restarted);
    RUN_TEST(test_a_dma_interrupt_after_stop_is_ignored);

    RUN_TEST(test_the_pulse_count_follows_the_pin);
    RUN_TEST(test_two_instances_with_their_own_queues_do_not_interfere);

    return UNITY_END();
}
