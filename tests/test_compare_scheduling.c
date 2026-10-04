#include "unity.h"
#include "pulse_generator.h"
#include "mock_platform.h"

/* The compare arithmetic the library took over from the platform: where the
   next match lands, how it wraps at the counter's width, and when an
   interrupt was serviced too late to use the value it just wrote. None of
   this was reachable from a host test while it lived in the platform. */

static mock_hw_t hw;

static int s_missed_count;

static void test_event_callback(pulse_generator_t *pg, pulse_generator_event_t event, void *user_ctx)
{
    (void)pg;
    (void)user_ctx;

    if (event == PULSE_GENERATOR_EVENT_MISSED_COMPARE) {
        s_missed_count++;
    }
}

static void init_instance(pulse_generator_t *pg)
{
    pulse_generator_init(pg, &(pulse_generator_config_t){
        .ops = &g_mock_ops,
        .hw = &hw,
        .on_event = test_event_callback,
    });
}

void setUp(void)
{
    mock_hw_init(&hw);
    s_missed_count = 0;
}

void tearDown(void) {}

static void test_first_compare_is_half_a_period_past_the_current_counter(void)
{
    pulse_generator_t pg;
    hw.counter = 5000;
    init_instance(&pg);

    pulse_generator_start_continuous(&pg, 1000);

    TEST_ASSERT_EQUAL_UINT32(6000, hw.first_compare);
}

static void test_first_compare_wraps_at_the_counter_width(void)
{
    pulse_generator_t pg;
    hw.counter = 0xFFC0;
    init_instance(&pg);

    pulse_generator_start_continuous(&pg, 1000);

    /* 0xFFC0 + 1000 = 0x103A8, which must come out as 0x03A8 on a 16-bit
       counter rather than as a value the register cannot hold. */
    TEST_ASSERT_EQUAL_UINT32(0x03A8, hw.first_compare);
}

static void test_compares_keep_their_spacing_across_the_counter_wrap(void)
{
    pulse_generator_t pg;
    hw.counter = 0xFF00;
    init_instance(&pg);
    pulse_generator_start_continuous(&pg, 1000);

    uint32_t previous = hw.first_compare;
    for (int edge = 0; edge < 200; edge++) {
        mock_hw_fire_and_notify(&pg, &hw);

        TEST_ASSERT_EQUAL_UINT32((previous + 1000) & 0xFFFF, hw.last_compare);
        previous = hw.last_compare;
    }

    /* Crossing the wrap is not a missed compare: the arithmetic is modular,
       so a compare value smaller than the previous one is expected. */
    TEST_ASSERT_EQUAL(0, s_missed_count);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));
}

static void test_compares_wrap_on_a_32_bit_counter(void)
{
    pulse_generator_t pg;
    hw.counter_max = 0xFFFFFFFF;
    hw.counter = 0xFFFFFF00;
    init_instance(&pg);

    pulse_generator_start_continuous(&pg, 1000);

    /* 0xFFFFFF00 + 1000 = 0x1000002E8, i.e. 744 once wrapped. */
    TEST_ASSERT_EQUAL_UINT32(744, hw.first_compare);

    mock_hw_fire_and_notify(&pg, &hw);
    TEST_ASSERT_EQUAL_UINT32(1744, hw.last_compare);
    TEST_ASSERT_EQUAL(0, s_missed_count);
}

static void test_latency_just_under_a_half_period_is_not_a_missed_compare(void)
{
    pulse_generator_t pg;
    init_instance(&pg);
    pulse_generator_start_continuous(&pg, 1000);

    /* The counter reaches one tick short of the compare just written: that
       match still fires, so nothing has to be rescheduled. */
    hw.isr_latency_ticks = 999;

    pulse_generator_status_t status = mock_hw_fire_and_notify(&pg, &hw);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, status);
    TEST_ASSERT_EQUAL(0, s_missed_count);
    TEST_ASSERT_EQUAL(1, hw.set_compare_call_count);
    TEST_ASSERT_EQUAL_UINT32(2000, hw.last_compare);
}

static void test_latency_of_exactly_a_half_period_is_a_missed_compare(void)
{
    pulse_generator_t pg;
    init_instance(&pg);
    pulse_generator_start_continuous(&pg, 1000);

    /* The counter has exactly reached the compare just written, so that match
       would not fire until the counter wrapped all the way around. */
    hw.isr_latency_ticks = 1000;

    pulse_generator_status_t status = mock_hw_fire_and_notify(&pg, &hw);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_MISSED_COMPARE, status);
    TEST_ASSERT_EQUAL(1, s_missed_count);

    /* Written twice: once optimistically, then again from the counter. */
    TEST_ASSERT_EQUAL(2, hw.set_compare_call_count);
    TEST_ASSERT_EQUAL_UINT32(3000, hw.last_compare);
}

static void test_the_edge_after_a_missed_compare_is_measured_from_the_new_one(void)
{
    pulse_generator_t pg;
    init_instance(&pg);
    pulse_generator_start_continuous(&pg, 1000);

    hw.isr_latency_ticks = 1000;
    mock_hw_fire_and_notify(&pg, &hw);
    TEST_ASSERT_EQUAL_UINT32(3000, hw.last_compare);

    /* Back to a prompt interrupt: the next compare must be half a period past
       the rescheduled one, not past the stale value that was missed. */
    hw.isr_latency_ticks = 0;
    pulse_generator_status_t status = mock_hw_fire_and_notify(&pg, &hw);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, status);
    TEST_ASSERT_EQUAL_UINT32(4000, hw.last_compare);
    TEST_ASSERT_EQUAL(1, s_missed_count);
}

static void test_a_missed_compare_still_counts_its_edge(void)
{
    pulse_generator_t pg;
    init_instance(&pg);
    pulse_generator_start_continuous(&pg, 1000);
    hw.isr_latency_ticks = 1000;

    mock_hw_fire_and_notify(&pg, &hw);
    mock_hw_fire_and_notify(&pg, &hw);

    TEST_ASSERT_EQUAL(2, s_missed_count);
    TEST_ASSERT_EQUAL_UINT32(1, pulse_generator_get_pulse_count(&pg));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));
}

static void test_a_failed_compare_write_is_propagated_and_the_movement_survives(void)
{
    pulse_generator_t pg;
    init_instance(&pg);
    pulse_generator_start_continuous(&pg, 1000);
    hw.set_compare_result = PULSE_GENERATOR_ERROR;

    pulse_generator_status_t status = mock_hw_fire_and_notify(&pg, &hw);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));

    /* Gave up after the failed write instead of reading the counter and
       writing again. */
    TEST_ASSERT_EQUAL(1, hw.set_compare_call_count);
    TEST_ASSERT_EQUAL(0, s_missed_count);
}

static void test_a_counter_range_that_is_not_a_power_of_two_is_rejected(void)
{
    pulse_generator_t pg;

    /* The library masks compare values with this, so a range that is not
       2^n - 1 cannot wrap correctly and must not be accepted silently. */
    hw.counter_max = 999;
    init_instance(&pg);

    pulse_generator_status_t status =
        pulse_generator_start_continuous(&pg, 1000);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_FALSE(hw.channel_running);
}

static void test_a_zero_counter_range_is_rejected(void)
{
    pulse_generator_t pg;
    hw.counter_max = 0;
    init_instance(&pg);

    pulse_generator_status_t status =
        pulse_generator_start_continuous(&pg, 1000);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
    TEST_ASSERT_FALSE(hw.channel_running);
}

static void test_a_fixed_count_movement_completes_across_the_counter_wrap(void)
{
    pulse_generator_t pg;

    /* 8-bit counter, 50 ticks per half period, starting late enough that the
       second edge only happens after the counter has wrapped. */
    hw.counter_max = 0xFF;
    hw.tick_hz = 100;
    hw.counter = 200;
    init_instance(&pg);

    pulse_generator_start_fixed_count(&pg, 1, 1);
    TEST_ASSERT_EQUAL_UINT32(250, hw.first_compare);

    mock_hw_fire_and_notify(&pg, &hw);
    TEST_ASSERT_EQUAL_UINT32(44, hw.last_compare);  /* (250 + 50) & 0xFF */
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_RUNNING, pulse_generator_get_state(&pg));

    mock_hw_fire_and_notify(&pg, &hw);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_FALSE(hw.channel_running);
    TEST_ASSERT_EQUAL(0, s_missed_count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_first_compare_is_half_a_period_past_the_current_counter);
    RUN_TEST(test_first_compare_wraps_at_the_counter_width);
    RUN_TEST(test_compares_keep_their_spacing_across_the_counter_wrap);
    RUN_TEST(test_compares_wrap_on_a_32_bit_counter);
    RUN_TEST(test_latency_just_under_a_half_period_is_not_a_missed_compare);
    RUN_TEST(test_latency_of_exactly_a_half_period_is_a_missed_compare);
    RUN_TEST(test_the_edge_after_a_missed_compare_is_measured_from_the_new_one);
    RUN_TEST(test_a_missed_compare_still_counts_its_edge);
    RUN_TEST(test_a_failed_compare_write_is_propagated_and_the_movement_survives);
    RUN_TEST(test_a_counter_range_that_is_not_a_power_of_two_is_rejected);
    RUN_TEST(test_a_zero_counter_range_is_rejected);
    RUN_TEST(test_a_fixed_count_movement_completes_across_the_counter_wrap);
    return UNITY_END();
}
