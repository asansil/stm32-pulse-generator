#include "unity.h"
#include "pulse_generator.h"
#include "mock_platform.h"

/* SCHEDULED mode driven by the ISR engine, against the simulated channel. */

static mock_hw_t hw;
static pulse_generator_t pg;
static uint32_t queue[8];

#define MAX_EVENTS 16

typedef struct {
    pulse_generator_event_t event;
    pulse_generator_state_t state; /* the instance's state as the callback saw it */
} recorded_event_t;

static recorded_event_t events[MAX_EVENTS];
static int event_count;

/* When set, LOW_WATERMARK refills the queue from inside the callback, the
   pattern the header allows for exactly this. */
static const uint32_t *refill;
static size_t refill_len;

static void record_event(pulse_generator_t *instance, pulse_generator_event_t event, void *user_ctx)
{
    (void)user_ctx;

    if (event_count < MAX_EVENTS) {
        /* get_state() is a plain read, so harmless here, and the only way
           to see the state the callback runs in. */
        events[event_count] = (recorded_event_t){ event, pulse_generator_get_state(instance) };
    }
    event_count++;

    if (event == PULSE_GENERATOR_EVENT_LOW_WATERMARK && refill != NULL) {
        pulse_generator_queue_events(instance, refill, refill_len);
    }
}

void setUp(void)
{
    mock_hw_init(&hw);
    event_count = 0;
    refill = NULL;
    pulse_generator_init(&pg, &(pulse_generator_config_t){
        .ops = &g_mock_ops, .hw = &hw, .on_event = record_event });
}

void tearDown(void) {}

/* A valid ISR-engine preparation: 7 usable slots, no low watermark, no
   minimum interval. Each test changes only what it is about. */
static pulse_generator_scheduled_config_t default_config(void)
{
    return (pulse_generator_scheduled_config_t){
        .queue          = queue,
        .queue_capacity = 8,
        .engine         = PULSE_GENERATOR_ENGINE_ISR,
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

static size_t queue_events(const uint32_t *intervals, size_t n)
{
    return pulse_generator_queue_events(&pg, intervals, n);
}

static void queue_one(uint32_t interval)
{
    TEST_ASSERT_EQUAL(1, queue_events(&interval, 1));
}

/* Records the compare values of the first n edges: the one channel_start()
   armed, then the one the library armed in response to each edge firing.
   The last one recorded is armed but not fired. */
static void record_edges(uint32_t *compares, size_t n)
{
    compares[0] = hw.first_compare;
    for (size_t i = 1; i < n; i++) {
        TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, mock_hw_fire_and_notify(&pg, &hw));
        compares[i] = hw.compare;
    }
}

/* Fires n edges in a row and returns the status of the last one. */
static pulse_generator_status_t fire_edges(int n)
{
    pulse_generator_status_t status = PULSE_GENERATOR_OK;
    for (int i = 0; i < n; i++) {
        status = mock_hw_fire_and_notify(&pg, &hw);
    }

    return status;
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
    pulse_generator_scheduled_config_t config = default_config();

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, prepare(&config));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, pulse_generator_get_state(&pg));
    TEST_ASSERT_FALSE(pulse_generator_is_busy(&pg));
    TEST_ASSERT_EQUAL(0, hw.channel_start_call_count);
}

static void test_prepare_reads_the_platform_timing_once(void)
{
    pulse_generator_scheduled_config_t config = default_config();

    prepare(&config);

    /* Read here rather than at start, so queue_events() can check intervals
       against counter_max before anything runs. */
    TEST_ASSERT_EQUAL(1, hw.get_counter_max_call_count);
    TEST_ASSERT_EQUAL(1, hw.get_tick_hz_call_count);
}

static void test_prepare_with_a_null_argument_returns_invalid_param(void)
{
    pulse_generator_scheduled_config_t config = default_config();
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM,
                      pulse_generator_prepare_scheduled(NULL, &config));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, prepare(NULL));

    config.queue = NULL;
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, prepare(&config));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
}

static void test_prepare_rejects_a_capacity_below_two(void)
{
    pulse_generator_scheduled_config_t config = default_config();

    /* One slot always stays empty, so a capacity of 1 could never hold an
       event. */
    config.queue_capacity = 0;
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, prepare(&config));
    config.queue_capacity = 1;
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, prepare(&config));

    config.queue_capacity = 2;
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, prepare(&config));
}

static void test_prepare_rejects_a_low_watermark_not_below_the_capacity(void)
{
    pulse_generator_scheduled_config_t config = default_config();

    config.low_watermark = 8;
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, prepare(&config));

    config.low_watermark = 7;
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, prepare(&config));
}

static void test_prepare_rejects_an_unknown_engine(void)
{
    pulse_generator_scheduled_config_t config = default_config();
    config.engine = (pulse_generator_engine_t)99;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, prepare(&config));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
}

static void test_prepare_with_the_dma_engine_is_not_supported_yet(void)
{
    pulse_generator_scheduled_config_t config = default_config();
    config.engine = PULSE_GENERATOR_ENGINE_DMA;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_NOT_SUPPORTED, prepare(&config));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
}

static void test_prepare_rejects_a_counter_max_that_is_not_a_power_of_two_minus_one(void)
{
    pulse_generator_scheduled_config_t config = default_config();
    hw.counter_max = 1000;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, prepare(&config));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
}

static void test_a_rejected_config_touches_no_hook(void)
{
    pulse_generator_scheduled_config_t config = default_config();
    config.queue_capacity = 1;

    prepare(&config);

    TEST_ASSERT_EQUAL(0, hw.get_counter_max_call_count);
    TEST_ASSERT_EQUAL(0, hw.get_tick_hz_call_count);
}

static void test_prepare_when_already_armed_returns_invalid_state(void)
{
    pulse_generator_scheduled_config_t config = default_config();
    prepare(&config);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, prepare(&config));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, pulse_generator_get_state(&pg));
}

static void test_prepare_while_another_mode_runs_returns_invalid_state(void)
{
    pulse_generator_scheduled_config_t config = default_config();
    pulse_generator_start_continuous(&pg, 1000);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, prepare(&config));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));
}

static void test_the_other_modes_cannot_start_while_armed(void)
{
    pulse_generator_scheduled_config_t config = default_config();
    prepare(&config);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE,
                      pulse_generator_start_fixed_count(&pg, 1000, 10));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE,
                      pulse_generator_start_continuous(&pg, 1000));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(0, hw.channel_start_call_count);
}

static void test_set_frequency_is_rejected_while_armed(void)
{
    pulse_generator_scheduled_config_t config = default_config();
    prepare(&config);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE,
                      pulse_generator_set_frequency(&pg, 1000));
}

static void test_init_takes_an_armed_instance_back_to_idle(void)
{
    pulse_generator_scheduled_config_t config = default_config();
    prepare(&config);

    pulse_generator_init(&pg, &(pulse_generator_config_t){ .ops = &g_mock_ops, .hw = &hw });

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK,
                      pulse_generator_start_continuous(&pg, 1000));
}

/* --- Queue --- */

static void test_a_prepared_queue_is_empty(void)
{
    prepare_default();

    TEST_ASSERT_EQUAL(0, pulse_generator_get_pending_events(&pg));
    TEST_ASSERT_EQUAL(7, pulse_generator_get_free_space(&pg));
}

static void test_the_queue_holds_capacity_minus_one_events(void)
{
    static const uint32_t intervals[10] = {
        1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000,
    };
    prepare_default();

    TEST_ASSERT_EQUAL(7, queue_events(intervals, 10));

    TEST_ASSERT_EQUAL(7, pulse_generator_get_pending_events(&pg));
    TEST_ASSERT_EQUAL(0, pulse_generator_get_free_space(&pg));
}

static void test_queue_events_takes_what_fits_and_says_how_much(void)
{
    static const uint32_t intervals[5] = { 1000, 1000, 1000, 1000, 1000 };
    prepare_default();

    TEST_ASSERT_EQUAL(5, queue_events(intervals, 5));
    TEST_ASSERT_EQUAL(2, queue_events(intervals, 5));
    TEST_ASSERT_EQUAL(0, queue_events(intervals, 5));

    TEST_ASSERT_EQUAL(7, pulse_generator_get_pending_events(&pg));
}

static void test_an_interval_below_two_ticks_stops_the_queueing_at_its_index(void)
{
    prepare_default();

    /* Its half would be 0 ticks, and a compare that does not move never
       fires. 2 is the shortest that splits into two non-empty halves. */
    TEST_ASSERT_EQUAL(2, queue_events((const uint32_t[]){ 1000, 2, 1, 1000 }, 4));
    TEST_ASSERT_EQUAL(0, queue_events((const uint32_t[]){ 0 }, 1));

    TEST_ASSERT_EQUAL(2, pulse_generator_get_pending_events(&pg));
}

static void test_an_interval_with_halves_below_the_minimum_is_rejected(void)
{
    pulse_generator_scheduled_config_t config = default_config();
    config.min_interval_ticks = 100;
    prepare(&config);

    /* 200 splits into 100 + 100; 199 into 99 + 100, one half too short. */
    TEST_ASSERT_EQUAL(1, queue_events((const uint32_t[]){ 200, 199, 300 }, 3));
}

static void test_an_interval_above_the_counter_max_is_rejected(void)
{
    prepare_default();

    TEST_ASSERT_EQUAL(1, queue_events((const uint32_t[]){ 0xFFFF, 0x10000, 1000 }, 3));
}

static void test_queue_events_with_a_null_argument_takes_nothing(void)
{
    prepare_default();

    TEST_ASSERT_EQUAL(0, pulse_generator_queue_events(NULL, (const uint32_t[]){ 1000 }, 1));
    TEST_ASSERT_EQUAL(0, queue_events(NULL, 1));

    TEST_ASSERT_EQUAL(0, pulse_generator_get_pending_events(&pg));
}

static void test_the_queue_needs_a_prepared_instance(void)
{
    /* IDLE: the queue fields are all zero, so an unguarded free space would
       come out as 0 - 1, i.e. SIZE_MAX. */
    TEST_ASSERT_EQUAL(0, queue_events((const uint32_t[]){ 1000 }, 1));
    TEST_ASSERT_EQUAL(0, pulse_generator_get_free_space(&pg));
    TEST_ASSERT_EQUAL(0, pulse_generator_get_pending_events(&pg));

    /* RUNNING, but in another mode. */
    pulse_generator_start_continuous(&pg, 1000);
    TEST_ASSERT_EQUAL(0, queue_events((const uint32_t[]){ 1000 }, 1));
    TEST_ASSERT_EQUAL(0, pulse_generator_get_free_space(&pg));
}

static void test_the_queue_accessors_with_a_null_instance_return_zero(void)
{
    TEST_ASSERT_EQUAL(0, pulse_generator_get_free_space(NULL));
    TEST_ASSERT_EQUAL(0, pulse_generator_get_pending_events(NULL));
}

/* --- start / finish / now --- */

static void test_now_ticks_reads_the_counter_in_any_state(void)
{
    hw.counter = 1234;
    TEST_ASSERT_EQUAL_UINT32(1234, pulse_generator_get_now_ticks(&pg)); /* IDLE */

    prepare_default();
    hw.counter = 4321;
    TEST_ASSERT_EQUAL_UINT32(4321, pulse_generator_get_now_ticks(&pg)); /* ARMED */

    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_now_ticks(NULL));
}

static void test_finish_needs_a_prepared_instance(void)
{
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_finish_scheduled(NULL));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, pulse_generator_finish_scheduled(&pg));

    pulse_generator_start_continuous(&pg, 1000);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, pulse_generator_finish_scheduled(&pg));
}

static void test_start_arms_the_first_rise_at_start_tick_plus_the_first_interval(void)
{
    prepare_default();
    queue_events((const uint32_t[]){ 1000, 600 }, 2);
    hw.counter = 100;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_scheduled(&pg, 500));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));
    TEST_ASSERT_TRUE(pulse_generator_is_busy(&pg));
    TEST_ASSERT_EQUAL(1, hw.channel_start_call_count);
    TEST_ASSERT_EQUAL_UINT32(1500, hw.first_compare);
    TEST_ASSERT_TRUE(hw.channel_running);
}

static void test_start_takes_the_first_event_off_the_queue(void)
{
    prepare_default();
    queue_events((const uint32_t[]){ 1000, 600, 800 }, 3);

    pulse_generator_start_scheduled(&pg, 0);

    TEST_ASSERT_EQUAL(2, pulse_generator_get_pending_events(&pg));
}

static void test_start_needs_a_second_event_to_place_the_first_fall(void)
{
    prepare_default();
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, pulse_generator_start_scheduled(&pg, 0));

    queue_events((const uint32_t[]){ 1000 }, 1);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, pulse_generator_start_scheduled(&pg, 0));

    TEST_ASSERT_EQUAL(0, hw.channel_start_call_count);
    TEST_ASSERT_EQUAL(1, pulse_generator_get_pending_events(&pg));
}

static void test_start_with_a_single_event_is_accepted_once_finished(void)
{
    prepare_default();
    queue_events((const uint32_t[]){ 1000 }, 1);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_finish_scheduled(&pg));

    /* With no successor, the last pulse's fall uses its own interval. */
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_scheduled(&pg, 0));
}

static void test_start_with_an_empty_queue_is_rejected_even_once_finished(void)
{
    prepare_default();
    pulse_generator_finish_scheduled(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, pulse_generator_start_scheduled(&pg, 0));
    TEST_ASSERT_EQUAL(0, hw.channel_start_call_count);
}

static void test_start_needs_an_armed_instance(void)
{
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_start_scheduled(NULL, 0));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, pulse_generator_start_scheduled(&pg, 0));

    prepare_default();
    queue_events((const uint32_t[]){ 1000, 1000, 1000, 1000 }, 4);
    pulse_generator_start_scheduled(&pg, 0);

    /* Already RUNNING. */
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, pulse_generator_start_scheduled(&pg, 0));
    TEST_ASSERT_EQUAL(1, hw.channel_start_call_count);
}

static void test_start_rejects_a_first_rise_the_counter_has_already_reached(void)
{
    prepare_default();
    queue_events((const uint32_t[]){ 1000, 1000 }, 2);

    hw.counter = 5000; /* rise at 3000 + 1000 = 4000: already behind */
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_start_scheduled(&pg, 3000));

    hw.counter = 4000; /* right on it: not ahead either */
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_start_scheduled(&pg, 3000));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(2, pulse_generator_get_pending_events(&pg));
    TEST_ASSERT_EQUAL(0, hw.channel_start_call_count);
}

static void test_a_first_rise_over_half_the_counter_range_ahead_reads_as_gone(void)
{
    prepare_default();
    queue_events((const uint32_t[]){ 0x8000, 1000 }, 2);

    /* Modulo the counter, 0x8000 ahead and 0x8000 behind are the same
       value: from half the range on, the rise is taken as already gone. */
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_start_scheduled(&pg, 0));
}

static void test_start_wraps_the_first_rise_to_the_counter_width(void)
{
    prepare_default();
    queue_events((const uint32_t[]){ 0x200, 1000 }, 2);
    hw.counter = 0xFF00;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_scheduled(&pg, 0xFF00));
    TEST_ASSERT_EQUAL_UINT32(0x0100, hw.first_compare);
}

static void test_a_failed_channel_start_leaves_the_instance_armed_with_its_queue(void)
{
    prepare_default();
    queue_events((const uint32_t[]){ 700, 1000 }, 2);
    hw.channel_start_result = PULSE_GENERATOR_ERROR_BUSY;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_BUSY, pulse_generator_start_scheduled(&pg, 0));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(2, pulse_generator_get_pending_events(&pg));

    /* Once the platform recovers, a retry starts from that same event. */
    hw.channel_start_result = PULSE_GENERATOR_OK;
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_scheduled(&pg, 0));
    TEST_ASSERT_EQUAL_UINT32(700, hw.first_compare);
}

/* --- ISR engine: edge timing --- */

static void test_the_worked_example_lands_every_edge_on_its_instant(void)
{
    const uint32_t S = 100;
    prepare_default();
    queue_events((const uint32_t[]){ 1000, 1000, 400 }, 3);
    pulse_generator_finish_scheduled(&pg);
    pulse_generator_start_scheduled(&pg, S);

    /* Each rise lands on its event; each fall halfway to the next rise, and
       the last one halfway through its own interval. */
    const uint32_t expected[6] = {
        S + 1000, S + 1500,   /* pulse 1: falls with half of the next interval, 1000 */
        S + 2000, S + 2200,   /* pulse 2: falls with half of the next interval, 400 */
        S + 2400, S + 2600,   /* pulse 3: the last, falls with half of its own */
    };
    uint32_t compares[6];
    record_edges(compares, 6);

    TEST_ASSERT_EQUAL_UINT32_ARRAY(expected, compares, 6);
}

static void test_a_fall_never_lands_after_the_next_rise_when_the_rate_climbs(void)
{
    prepare_default();
    queue_events((const uint32_t[]){ 1000, 400, 100, 40 }, 4);
    pulse_generator_start_scheduled(&pg, 0);

    /* Halving the previous interval instead would put pulse 1's fall at
       1500, after pulse 2's rise at 1400, and invert the pin from there. */
    const uint32_t expected[7] = { 1000, 1200, 1400, 1450, 1500, 1520, 1540 };
    uint32_t compares[7];
    record_edges(compares, 7);

    TEST_ASSERT_EQUAL_UINT32_ARRAY(expected, compares, 7);
}

static void test_a_single_finished_event_falls_halfway_through_its_own_interval(void)
{
    prepare_default();
    queue_one(1000);
    pulse_generator_finish_scheduled(&pg);
    pulse_generator_start_scheduled(&pg, 0);

    uint32_t compares[2];
    record_edges(compares, 2);

    TEST_ASSERT_EQUAL_UINT32_ARRAY(((const uint32_t[]){ 1000, 1500 }), compares, 2);
}

static void test_the_pulse_count_counts_complete_pulses(void)
{
    prepare_default();
    queue_events((const uint32_t[]){ 1000, 1000, 1000, 1000 }, 4);
    pulse_generator_start_scheduled(&pg, 0);

    mock_hw_fire_and_notify(&pg, &hw); /* rise 1 */
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));
    mock_hw_fire_and_notify(&pg, &hw); /* fall 1 */
    TEST_ASSERT_EQUAL_UINT32(1, pulse_generator_get_pulse_count(&pg));
    mock_hw_fire_and_notify(&pg, &hw); /* rise 2 */
    mock_hw_fire_and_notify(&pg, &hw); /* fall 2 */
    TEST_ASSERT_EQUAL_UINT32(2, pulse_generator_get_pulse_count(&pg));
}

/* Varied, so that an event taken out of order would show. */
static uint32_t streamed_interval(uint32_t k)
{
    return 400u + 20u * (k % 5u);
}

static void test_events_queued_while_running_continue_the_train_without_a_seam(void)
{
    uint32_t queued = 0;
    prepare_default();
    while (queued < 3) {
        queue_one(streamed_interval(queued++));
    }

    /* Started near the top of the counter so the edges also wrap past
       0xFFFF on the way. */
    hw.counter = 0xF000;
    pulse_generator_start_scheduled(&pg, 0xF000);
    uint32_t rise = (0xF000 + streamed_interval(0)) & 0xFFFF;
    TEST_ASSERT_EQUAL_UINT32(rise, hw.first_compare);

    /* 20 pulses through 8 slots: the ring wraps round more than twice, with
       the producer topping it up by one event per pulse. */
    for (uint32_t k = 0; k < 20; k++) {
        const uint32_t next = streamed_interval(k + 1);

        TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, mock_hw_fire_and_notify(&pg, &hw)); /* rise k */
        TEST_ASSERT_EQUAL_UINT32((rise + next / 2) & 0xFFFF, hw.compare);

        TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, mock_hw_fire_and_notify(&pg, &hw)); /* fall k */
        rise = (rise + next) & 0xFFFF;
        TEST_ASSERT_EQUAL_UINT32(rise, hw.compare);

        queue_one(streamed_interval(queued++));
    }
}

/* --- ISR engine: how a movement ends --- */

static void test_a_finished_movement_completes_at_the_end_of_its_last_pulse(void)
{
    prepare_default();
    queue_events((const uint32_t[]){ 1000, 1000, 400 }, 3);
    pulse_generator_finish_scheduled(&pg);
    pulse_generator_start_scheduled(&pg, 0);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, fire_edges(5));
    TEST_ASSERT_EQUAL(0, event_count);

    /* The sixth edge is the last pulse's fall: an even number of toggles,
       so the pin is left low. */
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, fire_edges(1));

    TEST_ASSERT_FALSE(hw.channel_running);
    TEST_ASSERT_EQUAL(5, hw.set_compare_call_count); /* nothing armed after it */
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(1, event_count);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_EVENT_COMPLETE, events[0].event);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, events[0].state);
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_underrun_count(&pg));
}

static void test_each_movement_needs_its_own_finish(void)
{
    prepare_default();
    queue_one(1000);
    pulse_generator_finish_scheduled(&pg);
    pulse_generator_start_scheduled(&pg, 0);
    fire_edges(2);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, pulse_generator_get_state(&pg));

    /* That finish belonged to the movement that just ended. */
    queue_one(1000);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE,
                      pulse_generator_start_scheduled(&pg, pulse_generator_get_now_ticks(&pg)));
}

static void test_a_queue_drained_without_finish_ends_in_an_underrun(void)
{
    prepare_default();
    queue_events((const uint32_t[]){ 1000, 1000 }, 2);
    pulse_generator_start_scheduled(&pg, 0);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, fire_edges(3));

    /* Pulse 2 found no successor at its rise, yet still falls cleanly,
       halfway through its own interval; only then does the movement stop. */
    TEST_ASSERT_EQUAL_UINT32(2500, hw.compare);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_UNDERRUN, fire_edges(1));

    TEST_ASSERT_FALSE(hw.channel_running);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(1, event_count);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_EVENT_UNDERRUN, events[0].event);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, events[0].state);
    TEST_ASSERT_EQUAL_UINT32(1, pulse_generator_get_underrun_count(&pg));
}

static void test_a_movement_that_underran_can_be_restarted(void)
{
    prepare_default();
    queue_events((const uint32_t[]){ 1000, 1000 }, 2);
    pulse_generator_start_scheduled(&pg, 0);
    fire_edges(4);

    queue_events((const uint32_t[]){ 500, 500 }, 2);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK,
                      pulse_generator_start_scheduled(&pg, pulse_generator_get_now_ticks(&pg)));

    TEST_ASSERT_EQUAL(2, hw.channel_start_call_count);
    TEST_ASSERT_EQUAL_UINT32(2500 + 500, hw.first_compare);
    /* A restart is not a new preparation: the diagnostic keeps counting. */
    TEST_ASSERT_EQUAL_UINT32(1, pulse_generator_get_underrun_count(&pg));
}

static void test_prepare_resets_the_underrun_count(void)
{
    prepare_default();
    queue_events((const uint32_t[]){ 1000, 1000 }, 2);
    pulse_generator_start_scheduled(&pg, 0);
    fire_edges(4);
    TEST_ASSERT_EQUAL_UINT32(1, pulse_generator_get_underrun_count(&pg));

    pulse_generator_init(&pg, &(pulse_generator_config_t){ .ops = &g_mock_ops, .hw = &hw });
    prepare_default();

    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_underrun_count(&pg));
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_underrun_count(NULL));
}

/* --- ISR engine: low watermark --- */

static void prepare_with_low_watermark(size_t low_watermark)
{
    pulse_generator_scheduled_config_t config = default_config();
    config.low_watermark = low_watermark;
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, prepare(&config));
}

static void test_the_low_watermark_fires_once_as_the_queue_drains(void)
{
    prepare_with_low_watermark(3);
    queue_events((const uint32_t[]){ 1000, 1000, 1000, 1000, 1000, 1000 }, 6);
    pulse_generator_start_scheduled(&pg, 0); /* 5 pending */

    fire_edges(4); /* pulses 1-2: 3 pending, not below the watermark yet */
    TEST_ASSERT_EQUAL(0, event_count);

    fire_edges(2); /* pulse 3: 2 pending */
    TEST_ASSERT_EQUAL(1, event_count);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_EVENT_LOW_WATERMARK, events[0].event);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, events[0].state);

    fire_edges(2); /* pulse 4: 1 pending, still low but not reported again */
    TEST_ASSERT_EQUAL(1, event_count);
}

static void test_the_low_watermark_rearms_once_a_refill_reaches_it(void)
{
    prepare_with_low_watermark(3);
    queue_events((const uint32_t[]){ 1000, 1000, 1000, 1000, 1000, 1000 }, 6);
    pulse_generator_start_scheduled(&pg, 0);
    fire_edges(6); /* reported once, 2 pending */

    queue_events((const uint32_t[]){ 1000, 1000, 1000 }, 3); /* 5 pending */
    fire_edges(4); /* pulses 4-5: back to 3, re-armed on the way */
    TEST_ASSERT_EQUAL(1, event_count);

    fire_edges(2); /* pulse 6: 2 pending */
    TEST_ASSERT_EQUAL(2, count_events(PULSE_GENERATOR_EVENT_LOW_WATERMARK));
}

static void test_a_zero_low_watermark_never_fires(void)
{
    prepare_default();
    queue_events((const uint32_t[]){ 1000, 1000, 1000 }, 3);
    pulse_generator_finish_scheduled(&pg);
    pulse_generator_start_scheduled(&pg, 0);

    fire_edges(6);

    TEST_ASSERT_EQUAL(0, count_events(PULSE_GENERATOR_EVENT_LOW_WATERMARK));
}

static void test_the_low_watermark_is_not_reported_once_finish_is_requested(void)
{
    prepare_with_low_watermark(3);
    queue_events((const uint32_t[]){ 1000, 1000, 1000, 1000 }, 4);
    pulse_generator_finish_scheduled(&pg);
    pulse_generator_start_scheduled(&pg, 0);

    fire_edges(8);

    /* The producer has said it has nothing more to add, so a warning that
       the queue is running dry would only be noise. */
    TEST_ASSERT_EQUAL(1, event_count);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_EVENT_COMPLETE, events[0].event);
}

static void test_the_queue_can_be_refilled_from_the_low_watermark_callback(void)
{
    static const uint32_t more[3] = { 500, 500, 500 };
    prepare_with_low_watermark(3);
    queue_events((const uint32_t[]){ 500, 500, 500, 500 }, 4);
    refill = more;
    refill_len = 3;
    pulse_generator_start_scheduled(&pg, 0);

    /* 30 pulses, far more than the queue ever holds at once. */
    for (int i = 0; i < 60; i++) {
        TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, mock_hw_fire_and_notify(&pg, &hw));
    }

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));
    TEST_ASSERT_TRUE(count_events(PULSE_GENERATOR_EVENT_LOW_WATERMARK) > 0);
    TEST_ASSERT_EQUAL(0, count_events(PULSE_GENERATOR_EVENT_UNDERRUN));
}

/* --- ISR engine: late edges --- */

static void test_a_rise_armed_too_late_stops_the_movement_at_once(void)
{
    prepare_default();
    queue_events((const uint32_t[]){ 1000, 1000, 1000 }, 3);
    pulse_generator_start_scheduled(&pg, 0);
    fire_edges(1); /* rise 1 at 1000, fall armed at 1500 */

    /* Fall 1 serviced 600 ticks late: the counter is at 2100, past rise 2
       at 2000. The pin is low, so stopping here is a clean pulse boundary. */
    hw.isr_latency_ticks = 600;
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_UNDERRUN, fire_edges(1));

    TEST_ASSERT_FALSE(hw.channel_running);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(0, pulse_generator_get_pending_events(&pg)); /* flushed */
    TEST_ASSERT_EQUAL(1, event_count);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_EVENT_UNDERRUN, events[0].event);
    TEST_ASSERT_EQUAL_UINT32(1, pulse_generator_get_underrun_count(&pg));
}

static void test_an_edge_armed_one_tick_ahead_is_not_late(void)
{
    prepare_default();
    queue_events((const uint32_t[]){ 1000, 1000, 1000 }, 3);
    pulse_generator_start_scheduled(&pg, 0);
    fire_edges(1);

    /* Counter at 1999, rise 2 at 2000: still ahead, if only just. At 500
       the counter would sit on 2000 itself, which already counts as late. */
    hw.isr_latency_ticks = 499;
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, fire_edges(1));

    TEST_ASSERT_TRUE(hw.channel_running);
    TEST_ASSERT_EQUAL_UINT32(2000, hw.compare);
}

static void test_a_late_fall_is_moved_to_now_so_the_pin_still_ends_low(void)
{
    prepare_default();
    queue_events((const uint32_t[]){ 1000, 1000, 1000 }, 3);
    pulse_generator_start_scheduled(&pg, 0);

    /* Rise 1 serviced 600 ticks late: the counter is at 1600, past fall 1
       at 1500. The pin is high, so stopping now would strand it there. */
    hw.isr_latency_ticks = 600;
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, fire_edges(1));

    TEST_ASSERT_EQUAL_UINT32(1601, hw.compare); /* now + 1, no minimum set */
    TEST_ASSERT_TRUE(hw.channel_running);
    TEST_ASSERT_EQUAL(0, event_count);

    /* That fall is the movement's last edge: the pulse came out too long,
       but the pin ends low and the caller is told. */
    hw.isr_latency_ticks = 0;
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_UNDERRUN, fire_edges(1));

    TEST_ASSERT_FALSE(hw.channel_running);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(1, event_count);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_EVENT_UNDERRUN, events[0].event);
    TEST_ASSERT_EQUAL(0, count_events(PULSE_GENERATOR_EVENT_MISSED_COMPARE));
}

static void test_a_late_fall_keeps_the_minimum_interval_from_now(void)
{
    pulse_generator_scheduled_config_t config = default_config();
    config.min_interval_ticks = 100;
    prepare(&config);
    queue_events((const uint32_t[]){ 1000, 1000, 1000 }, 3);
    pulse_generator_start_scheduled(&pg, 0);

    hw.isr_latency_ticks = 600;
    fire_edges(1);

    /* Closer than the minimum, the ISR could not be back in time for it. */
    TEST_ASSERT_EQUAL_UINT32(1600 + 100, hw.compare);
}

static void test_a_failing_set_compare_stops_the_movement_with_the_platform_error(void)
{
    prepare_default();
    queue_events((const uint32_t[]){ 1000, 1000, 1000 }, 3);
    pulse_generator_start_scheduled(&pg, 0);
    fire_edges(1);

    hw.set_compare_result = PULSE_GENERATOR_ERROR;
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR, fire_edges(1));

    TEST_ASSERT_FALSE(hw.channel_running);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(1, event_count);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_EVENT_UNDERRUN, events[0].event);
    TEST_ASSERT_EQUAL_UINT32(1, pulse_generator_get_underrun_count(&pg));
}

/* --- stop() --- */

static void test_stop_mid_movement_lands_in_armed_with_the_queue_flushed(void)
{
    prepare_default();
    queue_events((const uint32_t[]){ 1000, 1000, 1000, 1000 }, 4);
    pulse_generator_finish_scheduled(&pg);
    pulse_generator_start_scheduled(&pg, 0);
    fire_edges(3);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_stop(&pg));

    TEST_ASSERT_FALSE(hw.channel_running);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(0, pulse_generator_get_pending_events(&pg));
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));

    /* An explicit stop is neither a completion nor an underrun. */
    TEST_ASSERT_EQUAL(0, event_count);
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_underrun_count(&pg));

    /* The finish request went with the movement. */
    queue_one(1000);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE,
                      pulse_generator_start_scheduled(&pg, pulse_generator_get_now_ticks(&pg)));
}

static void test_stop_while_armed_flushes_the_queue_without_touching_the_hardware(void)
{
    prepare_default();
    queue_events((const uint32_t[]){ 1000, 1000, 1000 }, 3);
    pulse_generator_finish_scheduled(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_stop(&pg));

    TEST_ASSERT_EQUAL(0, hw.channel_stop_call_count);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL(0, pulse_generator_get_pending_events(&pg));

    queue_one(1000);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_STATE, pulse_generator_start_scheduled(&pg, 0));
}

static void test_a_stopped_movement_can_be_restarted(void)
{
    prepare_default();
    queue_events((const uint32_t[]){ 1000, 1000, 1000 }, 3);
    pulse_generator_start_scheduled(&pg, 0);
    fire_edges(1);
    pulse_generator_stop(&pg);

    queue_events((const uint32_t[]){ 400, 400 }, 2);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK,
                      pulse_generator_start_scheduled(&pg, pulse_generator_get_now_ticks(&pg)));
    TEST_ASSERT_EQUAL_UINT32(1000 + 400, hw.first_compare);
}

static void test_a_compare_interrupt_after_stop_is_ignored(void)
{
    prepare_default();
    queue_events((const uint32_t[]){ 1000, 1000, 1000 }, 3);
    pulse_generator_start_scheduled(&pg, 0);
    pulse_generator_stop(&pg);
    const int set_compare_calls = hw.set_compare_call_count;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_notify_compare_match(&pg));

    TEST_ASSERT_EQUAL(set_compare_calls, hw.set_compare_call_count);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_ARMED, pulse_generator_get_state(&pg));
}

/* --- Multi-instance --- */

static void test_two_instances_with_their_own_queues_do_not_interfere(void)
{
    static mock_hw_t hw2;
    static pulse_generator_t pg2;
    static uint32_t queue2[4];

    mock_hw_init(&hw2);
    pulse_generator_init(&pg2, &(pulse_generator_config_t){ .ops = &g_mock_ops, .hw = &hw2 });
    prepare_default();
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_prepare_scheduled(&pg2,
        &(pulse_generator_scheduled_config_t){ .queue = queue2, .queue_capacity = 4,
                                               .engine = PULSE_GENERATOR_ENGINE_ISR }));

    queue_events((const uint32_t[]){ 1000, 1000, 1000 }, 3);
    TEST_ASSERT_EQUAL(3, pulse_generator_queue_events(&pg2, (const uint32_t[]){ 300, 300, 300 }, 3));
    pulse_generator_start_scheduled(&pg, 0);
    pulse_generator_start_scheduled(&pg2, 0);

    /* Interleaved, as two channels' interrupts would be. */
    mock_hw_fire_and_notify(&pg, &hw);
    mock_hw_fire_and_notify(&pg2, &hw2);
    mock_hw_fire_and_notify(&pg2, &hw2);
    mock_hw_fire_and_notify(&pg, &hw);

    TEST_ASSERT_EQUAL_UINT32(2000, hw.compare);  /* rise 2 of 1000-tick pulses */
    TEST_ASSERT_EQUAL_UINT32(600, hw2.compare);  /* rise 2 of 300-tick pulses */
    TEST_ASSERT_EQUAL(1, pulse_generator_get_pending_events(&pg));
    TEST_ASSERT_EQUAL(1, pulse_generator_get_pending_events(&pg2));

    pulse_generator_stop(&pg);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg2));
    TEST_ASSERT_TRUE(hw2.channel_running);
    TEST_ASSERT_EQUAL(1, pulse_generator_get_pending_events(&pg2));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_prepare_moves_the_instance_from_idle_to_armed);
    RUN_TEST(test_prepare_reads_the_platform_timing_once);
    RUN_TEST(test_prepare_with_a_null_argument_returns_invalid_param);
    RUN_TEST(test_prepare_rejects_a_capacity_below_two);
    RUN_TEST(test_prepare_rejects_a_low_watermark_not_below_the_capacity);
    RUN_TEST(test_prepare_rejects_an_unknown_engine);
    RUN_TEST(test_prepare_with_the_dma_engine_is_not_supported_yet);
    RUN_TEST(test_prepare_rejects_a_counter_max_that_is_not_a_power_of_two_minus_one);
    RUN_TEST(test_a_rejected_config_touches_no_hook);
    RUN_TEST(test_prepare_when_already_armed_returns_invalid_state);
    RUN_TEST(test_prepare_while_another_mode_runs_returns_invalid_state);
    RUN_TEST(test_the_other_modes_cannot_start_while_armed);
    RUN_TEST(test_set_frequency_is_rejected_while_armed);
    RUN_TEST(test_init_takes_an_armed_instance_back_to_idle);
    RUN_TEST(test_a_prepared_queue_is_empty);
    RUN_TEST(test_the_queue_holds_capacity_minus_one_events);
    RUN_TEST(test_queue_events_takes_what_fits_and_says_how_much);
    RUN_TEST(test_an_interval_below_two_ticks_stops_the_queueing_at_its_index);
    RUN_TEST(test_an_interval_with_halves_below_the_minimum_is_rejected);
    RUN_TEST(test_an_interval_above_the_counter_max_is_rejected);
    RUN_TEST(test_queue_events_with_a_null_argument_takes_nothing);
    RUN_TEST(test_the_queue_needs_a_prepared_instance);
    RUN_TEST(test_the_queue_accessors_with_a_null_instance_return_zero);
    RUN_TEST(test_now_ticks_reads_the_counter_in_any_state);
    RUN_TEST(test_finish_needs_a_prepared_instance);
    RUN_TEST(test_start_arms_the_first_rise_at_start_tick_plus_the_first_interval);
    RUN_TEST(test_start_takes_the_first_event_off_the_queue);
    RUN_TEST(test_start_needs_a_second_event_to_place_the_first_fall);
    RUN_TEST(test_start_with_a_single_event_is_accepted_once_finished);
    RUN_TEST(test_start_with_an_empty_queue_is_rejected_even_once_finished);
    RUN_TEST(test_start_needs_an_armed_instance);
    RUN_TEST(test_start_rejects_a_first_rise_the_counter_has_already_reached);
    RUN_TEST(test_a_first_rise_over_half_the_counter_range_ahead_reads_as_gone);
    RUN_TEST(test_start_wraps_the_first_rise_to_the_counter_width);
    RUN_TEST(test_a_failed_channel_start_leaves_the_instance_armed_with_its_queue);
    RUN_TEST(test_the_worked_example_lands_every_edge_on_its_instant);
    RUN_TEST(test_a_fall_never_lands_after_the_next_rise_when_the_rate_climbs);
    RUN_TEST(test_a_single_finished_event_falls_halfway_through_its_own_interval);
    RUN_TEST(test_the_pulse_count_counts_complete_pulses);
    RUN_TEST(test_events_queued_while_running_continue_the_train_without_a_seam);
    RUN_TEST(test_a_finished_movement_completes_at_the_end_of_its_last_pulse);
    RUN_TEST(test_each_movement_needs_its_own_finish);
    RUN_TEST(test_a_queue_drained_without_finish_ends_in_an_underrun);
    RUN_TEST(test_a_movement_that_underran_can_be_restarted);
    RUN_TEST(test_prepare_resets_the_underrun_count);
    RUN_TEST(test_the_low_watermark_fires_once_as_the_queue_drains);
    RUN_TEST(test_the_low_watermark_rearms_once_a_refill_reaches_it);
    RUN_TEST(test_a_zero_low_watermark_never_fires);
    RUN_TEST(test_the_low_watermark_is_not_reported_once_finish_is_requested);
    RUN_TEST(test_the_queue_can_be_refilled_from_the_low_watermark_callback);
    RUN_TEST(test_a_rise_armed_too_late_stops_the_movement_at_once);
    RUN_TEST(test_an_edge_armed_one_tick_ahead_is_not_late);
    RUN_TEST(test_a_late_fall_is_moved_to_now_so_the_pin_still_ends_low);
    RUN_TEST(test_a_late_fall_keeps_the_minimum_interval_from_now);
    RUN_TEST(test_a_failing_set_compare_stops_the_movement_with_the_platform_error);
    RUN_TEST(test_stop_mid_movement_lands_in_armed_with_the_queue_flushed);
    RUN_TEST(test_stop_while_armed_flushes_the_queue_without_touching_the_hardware);
    RUN_TEST(test_a_stopped_movement_can_be_restarted);
    RUN_TEST(test_a_compare_interrupt_after_stop_is_ignored);
    RUN_TEST(test_two_instances_with_their_own_queues_do_not_interfere);
    return UNITY_END();
}
