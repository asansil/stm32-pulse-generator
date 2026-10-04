#include "unity.h"
#include "pulse_generator.h"
#include "mock_platform.h"

/* Several instances driven through one shared ops table, which is the whole
   point of keeping the function pointers out of the per-output state: one
   const table in flash, one hw struct per axis. */

static mock_hw_t hw_a;
static mock_hw_t hw_b;

static int s_complete_a;
static int s_complete_b;

static void count_completions(pulse_generator_t *pg, pulse_generator_event_t event, void *user_ctx)
{
    (void)pg;

    if (event == PULSE_GENERATOR_EVENT_COMPLETE) {
        (*(int *)user_ctx)++;
    }
}

static void init_pair(pulse_generator_t *pg_a, pulse_generator_t *pg_b)
{
    pulse_generator_init(pg_a, &(pulse_generator_config_t){
        .ops = &g_mock_ops, .hw = &hw_a, .on_event = count_completions, .user_ctx = &s_complete_a,
    });
    pulse_generator_init(pg_b, &(pulse_generator_config_t){
        .ops = &g_mock_ops, .hw = &hw_b, .on_event = count_completions, .user_ctx = &s_complete_b,
    });
}

void setUp(void)
{
    mock_hw_init(&hw_a);
    mock_hw_init(&hw_b);
    s_complete_a = 0;
    s_complete_b = 0;
}

void tearDown(void) {}

static void test_two_instances_arm_their_own_output(void)
{
    pulse_generator_t pg_a;
    pulse_generator_t pg_b;
    init_pair(&pg_a, &pg_b);

    pulse_generator_start_continuous(&pg_a, 1000);

    TEST_ASSERT_TRUE(hw_a.channel_running);
    TEST_ASSERT_FALSE(hw_b.channel_running);
    TEST_ASSERT_EQUAL(1, hw_a.channel_start_call_count);
    TEST_ASSERT_EQUAL(0, hw_b.channel_start_call_count);

    pulse_generator_start_continuous(&pg_b, 1000);

    TEST_ASSERT_TRUE(hw_b.channel_running);
    TEST_ASSERT_EQUAL(1, hw_a.channel_start_call_count);
}

static void test_edges_on_one_instance_do_not_disturb_the_other(void)
{
    pulse_generator_t pg_a;
    pulse_generator_t pg_b;
    init_pair(&pg_a, &pg_b);
    pulse_generator_start_continuous(&pg_a, 1000);
    pulse_generator_start_continuous(&pg_b, 1000);

    for (int i = 0; i < 10 * 2; i++) {
        mock_hw_fire_and_notify(&pg_a, &hw_a);
    }

    TEST_ASSERT_EQUAL_UINT32(10, pulse_generator_get_pulse_count(&pg_a));
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg_b));
    TEST_ASSERT_EQUAL(0, hw_b.set_compare_call_count);
}

static void test_instances_can_run_at_different_frequencies_at_once(void)
{
    pulse_generator_t pg_a;
    pulse_generator_t pg_b;
    init_pair(&pg_a, &pg_b);

    pulse_generator_start_continuous(&pg_a, 1000);
    pulse_generator_start_continuous(&pg_b, 2000);

    TEST_ASSERT_EQUAL_UINT32(1000, hw_a.first_compare);
    TEST_ASSERT_EQUAL_UINT32(500, hw_b.first_compare);

    mock_hw_fire_and_notify(&pg_a, &hw_a);
    mock_hw_fire_and_notify(&pg_b, &hw_b);

    TEST_ASSERT_EQUAL_UINT32(2000, hw_a.last_compare);
    TEST_ASSERT_EQUAL_UINT32(1000, hw_b.last_compare);
}

static void test_completing_one_movement_leaves_the_other_running(void)
{
    pulse_generator_t pg_a;
    pulse_generator_t pg_b;
    init_pair(&pg_a, &pg_b);
    pulse_generator_start_fixed_count(&pg_a, 1000, 2);
    pulse_generator_start_continuous(&pg_b, 1000);

    for (int i = 0; i < 2 * 2; i++) {
        mock_hw_fire_and_notify(&pg_a, &hw_a);
    }

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg_a));
    TEST_ASSERT_EQUAL(1, s_complete_a);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg_b));
    TEST_ASSERT_TRUE(hw_b.channel_running);
    TEST_ASSERT_EQUAL(0, s_complete_b);
    TEST_ASSERT_EQUAL(0, hw_b.channel_stop_call_count);
}

static void test_stopping_one_instance_does_not_touch_the_other_output(void)
{
    pulse_generator_t pg_a;
    pulse_generator_t pg_b;
    init_pair(&pg_a, &pg_b);
    pulse_generator_start_continuous(&pg_a, 1000);
    pulse_generator_start_continuous(&pg_b, 1000);

    pulse_generator_stop(&pg_a);

    TEST_ASSERT_FALSE(hw_a.channel_running);
    TEST_ASSERT_EQUAL(1, hw_a.channel_stop_call_count);
    TEST_ASSERT_TRUE(hw_b.channel_running);
    TEST_ASSERT_EQUAL(0, hw_b.channel_stop_call_count);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg_b));
}

static void test_a_failed_start_on_one_instance_does_not_affect_the_other(void)
{
    pulse_generator_t pg_a;
    pulse_generator_t pg_b;
    init_pair(&pg_a, &pg_b);
    hw_a.channel_start_result = PULSE_GENERATOR_ERROR;

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR,
                      pulse_generator_start_continuous(&pg_a, 1000));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK,
                      pulse_generator_start_continuous(&pg_b, 1000));

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg_a));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg_b));

    mock_hw_fire_and_notify(&pg_b, &hw_b);
    TEST_ASSERT_EQUAL_UINT32(2000, hw_b.last_compare);
    TEST_ASSERT_EQUAL(0, hw_a.set_compare_call_count);
}

static void test_instances_can_use_counters_of_different_widths(void)
{
    pulse_generator_t pg_a;
    pulse_generator_t pg_b;
    hw_b.counter_max = 0xFFFFFFFF;
    hw_b.counter = 0xFFFFFF00;
    init_pair(&pg_a, &pg_b);

    pulse_generator_start_continuous(&pg_a, 1000);
    pulse_generator_start_continuous(&pg_b, 1000);

    /* Each instance wraps at its own counter's width, from the same ops. */
    TEST_ASSERT_EQUAL_UINT32(1000, hw_a.first_compare);
    TEST_ASSERT_EQUAL_UINT32(744, hw_b.first_compare);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_two_instances_arm_their_own_output);
    RUN_TEST(test_edges_on_one_instance_do_not_disturb_the_other);
    RUN_TEST(test_instances_can_run_at_different_frequencies_at_once);
    RUN_TEST(test_completing_one_movement_leaves_the_other_running);
    RUN_TEST(test_stopping_one_instance_does_not_touch_the_other_output);
    RUN_TEST(test_a_failed_start_on_one_instance_does_not_affect_the_other);
    RUN_TEST(test_instances_can_use_counters_of_different_widths);
    return UNITY_END();
}
