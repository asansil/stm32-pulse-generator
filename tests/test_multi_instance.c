#include <string.h>

#include "unity.h"
#include "pulse_generator.h"
#include "mock_platform.h"

/* Several instances driven through one shared ops table, which is the whole
   point of keeping the function pointers out of the per-output state: one
   const table in flash, one hw struct and one DMA buffer per axis. */

#define ENTRIES   8u
#define MAX_WORDS 4u /* room for a four-word layout */

static mock_hw_t hw_a;
static mock_hw_t hw_b;
static uint32_t  buffer_a[ENTRIES * MAX_WORDS];
static uint32_t  buffer_b[ENTRIES * MAX_WORDS];

static int complete_a;
static int complete_b;

static void count_completions(pulse_generator_t *pg, pulse_generator_event_t event, void *user_ctx)
{
    (void)pg;

    if (event == PULSE_GENERATOR_EVENT_COMPLETE) {
        (*(int *)user_ctx)++;
    }
}

/* 2 MHz tick, 8 entries, 1000 us window: no entry longer than 250 ticks. */
static void init_one(pulse_generator_t *pg, mock_hw_t *hw, uint32_t *buffer, int *complete_count)
{
    pulse_generator_config_t config = {
        .ops      = &g_mock_ops,
        .hw       = hw,
        .on_event = count_completions,
        .user_ctx = complete_count,
        .dma = {
            .buffer       = buffer,
            .buffer_words = ENTRIES * MAX_WORDS,
            .entries      = ENTRIES,
            .window_us    = 1000,
        },
    };
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_init(pg, &config));
}

static void init_pair(pulse_generator_t *pg_a, pulse_generator_t *pg_b)
{
    init_one(pg_a, &hw_a, buffer_a, &complete_a);
    init_one(pg_b, &hw_b, buffer_b, &complete_b);
}

/* Interleaved update events, as two timers would raise them. */
static void step_both(pulse_generator_t *pg_a, pulse_generator_t *pg_b, size_t k)
{
    for (size_t i = 0; i < k; i++) {
        mock_step(pg_a, &hw_a, 1);
        mock_step(pg_b, &hw_b, 1);
    }
}

void setUp(void)
{
    mock_hw_init(&hw_a);
    mock_hw_init(&hw_b);
    memset(buffer_a, 0, sizeof buffer_a);
    memset(buffer_b, 0, sizeof buffer_b);
    complete_a = 0;
    complete_b = 0;
}

void tearDown(void) {}

static void test_two_instances_start_their_own_stream(void)
{
    pulse_generator_t pg_a;
    pulse_generator_t pg_b;
    init_pair(&pg_a, &pg_b);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg_a, 1000));

    TEST_ASSERT_TRUE(hw_a.running);
    TEST_ASSERT_FALSE(hw_b.running);
    TEST_ASSERT_EQUAL_PTR(buffer_a, hw_a.buffer);
    TEST_ASSERT_EQUAL(1, hw_a.stream_start_call_count);
    TEST_ASSERT_EQUAL(0, hw_b.stream_start_call_count);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg_b, 1000));

    TEST_ASSERT_TRUE(hw_b.running);
    TEST_ASSERT_EQUAL_PTR(buffer_b, hw_b.buffer);
    TEST_ASSERT_EQUAL(1, hw_a.stream_start_call_count);
}

static void test_two_movements_play_to_their_end_without_interfering(void)
{
    pulse_generator_t pg_a;
    pulse_generator_t pg_b;
    init_pair(&pg_a, &pg_b);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg_a, 10000, 5));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg_b, 1000, 3));

    step_both(&pg_a, &pg_b, 100 * ENTRIES);

    const uint32_t falls_b[] = { 1000, 3000, 5000 };
    TEST_ASSERT_EQUAL(5, hw_a.rise_count);
    TEST_ASSERT_EQUAL(3, hw_b.rise_count);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(falls_b, hw_b.fall_ticks, 3);
    TEST_ASSERT_FALSE(hw_a.running);
    TEST_ASSERT_FALSE(hw_b.running);
    TEST_ASSERT_EQUAL(1, complete_a);
    TEST_ASSERT_EQUAL(1, complete_b);
}

static void test_instances_can_run_at_different_frequencies_at_once(void)
{
    pulse_generator_t pg_a;
    pulse_generator_t pg_b;
    init_pair(&pg_a, &pg_b);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg_a, 10000));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg_b, 5000));

    step_both(&pg_a, &pg_b, 3 * ENTRIES);

    /* 200 and 400 ticks at 2 MHz, one entry per period. */
    const uint32_t rises_a[] = { 0, 200, 400, 600 };
    const uint32_t rises_b[] = { 0, 400, 800, 1200 };
    TEST_ASSERT_EQUAL_UINT32_ARRAY(rises_a, hw_a.rise_ticks, 4);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(rises_b, hw_b.rise_ticks, 4);
}

static void test_completing_one_movement_leaves_the_other_running(void)
{
    pulse_generator_t pg_a;
    pulse_generator_t pg_b;
    init_pair(&pg_a, &pg_b);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_fixed_count(&pg_a, 10000, 2));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg_b, 10000));

    step_both(&pg_a, &pg_b, 10 * ENTRIES);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg_a));
    TEST_ASSERT_EQUAL(1, complete_a);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg_b));
    TEST_ASSERT_TRUE(hw_b.running);
    TEST_ASSERT_EQUAL(0, complete_b);
    TEST_ASSERT_EQUAL(0, hw_b.stream_stop_call_count);
}

static void test_stopping_one_instance_does_not_touch_the_other_output(void)
{
    pulse_generator_t pg_a;
    pulse_generator_t pg_b;
    init_pair(&pg_a, &pg_b);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg_a, 1000));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg_b, 1000));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_stop(&pg_a));

    TEST_ASSERT_FALSE(hw_a.running);
    TEST_ASSERT_EQUAL(1, hw_a.stream_stop_call_count);
    TEST_ASSERT_TRUE(hw_b.running);
    TEST_ASSERT_EQUAL(0, hw_b.stream_stop_call_count);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg_b));
}

static void test_a_failed_start_on_one_instance_does_not_affect_the_other(void)
{
    pulse_generator_t pg_a;
    pulse_generator_t pg_b;
    init_pair(&pg_a, &pg_b);
    hw_a.stream_start_result = PULSE_GENERATOR_ERROR;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR, pulse_generator_start_continuous(&pg_a, 10000));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg_b, 10000));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg_a));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg_b));

    step_both(&pg_a, &pg_b, 2 * ENTRIES);

    TEST_ASSERT_EQUAL(0, hw_a.rise_count);
    TEST_ASSERT_EQUAL(2 * ENTRIES, hw_b.rise_count);
}

static void test_instances_can_use_different_entry_layouts(void)
{
    pulse_generator_t pg_a;
    pulse_generator_t pg_b;
    /* Channel 2 on STM32: ARR, RCR, CCR1, CCR2. */
    hw_b.layout = (pulse_generator_entry_layout_t){
        .words_per_entry = 4,
        .period_index    = 0,
        .compare_index   = 3,
    };
    init_pair(&pg_a, &pg_b);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg_a, 1000));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_start_continuous(&pg_b, 1000));

    /* Each instance encodes its buffer for its own output, from the same ops,
       and both pins come out the same. */
    TEST_ASSERT_EQUAL(ENTRIES * 3u, hw_a.words);
    TEST_ASSERT_EQUAL(ENTRIES * 4u, hw_b.words);

    step_both(&pg_a, &pg_b, 4 * ENTRIES);

    TEST_ASSERT_EQUAL(hw_a.rise_count, hw_b.rise_count);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(hw_a.rise_ticks, hw_b.rise_ticks, hw_a.rise_count);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(hw_a.fall_ticks, hw_b.fall_ticks, hw_a.fall_count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_two_instances_start_their_own_stream);
    RUN_TEST(test_two_movements_play_to_their_end_without_interfering);
    RUN_TEST(test_instances_can_run_at_different_frequencies_at_once);
    RUN_TEST(test_completing_one_movement_leaves_the_other_running);
    RUN_TEST(test_stopping_one_instance_does_not_touch_the_other_output);
    RUN_TEST(test_a_failed_start_on_one_instance_does_not_affect_the_other);
    RUN_TEST(test_instances_can_use_different_entry_layouts);
    return UNITY_END();
}
