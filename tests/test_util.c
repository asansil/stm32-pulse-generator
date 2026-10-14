#include <string.h>

#include "unity.h"
#include "pulse_generator_util.h"

/* Written into outputs before a call that must fail, to check it was left
   untouched. */
#define UNTOUCHED 0xDEADBEEFu

void setUp(void) {}
void tearDown(void) {}

/* --- Unit conversions --- */

static void test_us_to_ticks_converts_whole_ticks(void)
{
    uint32_t ticks = 0;

    TEST_ASSERT_TRUE(pulse_generator_us_to_ticks(1000000u, 250u, &ticks));
    TEST_ASSERT_EQUAL_UINT32(250u, ticks);
    TEST_ASSERT_TRUE(pulse_generator_us_to_ticks(84000000u, 1u, &ticks));
    TEST_ASSERT_EQUAL_UINT32(84u, ticks);
    TEST_ASSERT_TRUE(pulse_generator_us_to_ticks(1000000u, 0u, &ticks));
    TEST_ASSERT_EQUAL_UINT32(0u, ticks);
}

static void test_us_to_ticks_rounds_halves_up(void)
{
    uint32_t ticks = 0;

    /* 3 Hz: 500000 us is 1.5 ticks, 499999 us just under. */
    TEST_ASSERT_TRUE(pulse_generator_us_to_ticks(3u, 500000u, &ticks));
    TEST_ASSERT_EQUAL_UINT32(2u, ticks);
    TEST_ASSERT_TRUE(pulse_generator_us_to_ticks(3u, 499999u, &ticks));
    TEST_ASSERT_EQUAL_UINT32(1u, ticks);
}

static void test_us_to_ticks_accepts_the_largest_result(void)
{
    uint32_t ticks = 0;

    TEST_ASSERT_TRUE(pulse_generator_us_to_ticks(UINT32_MAX, 1000000u, &ticks));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, ticks);
}

static void test_us_to_ticks_rejects_a_result_that_does_not_fit(void)
{
    uint32_t ticks = UNTOUCHED;

    TEST_ASSERT_FALSE(pulse_generator_us_to_ticks(84000000u, UINT32_MAX, &ticks));
    TEST_ASSERT_EQUAL_HEX32(UNTOUCHED, ticks);
}

static void test_us_to_ticks_rejects_bad_arguments(void)
{
    uint32_t ticks = UNTOUCHED;

    TEST_ASSERT_FALSE(pulse_generator_us_to_ticks(0u, 100u, &ticks));
    TEST_ASSERT_EQUAL_HEX32(UNTOUCHED, ticks);
    TEST_ASSERT_FALSE(pulse_generator_us_to_ticks(1000000u, 100u, NULL));
}

static void test_ns_to_ticks_converts_and_rounds(void)
{
    uint32_t ticks = 0;

    TEST_ASSERT_TRUE(pulse_generator_ns_to_ticks(84000000u, 1000u, &ticks));
    TEST_ASSERT_EQUAL_UINT32(84u, ticks);
    /* 84 MHz: 6 ns is 0.504 ticks, 5 ns is 0.42. */
    TEST_ASSERT_TRUE(pulse_generator_ns_to_ticks(84000000u, 6u, &ticks));
    TEST_ASSERT_EQUAL_UINT32(1u, ticks);
    TEST_ASSERT_TRUE(pulse_generator_ns_to_ticks(84000000u, 5u, &ticks));
    TEST_ASSERT_EQUAL_UINT32(0u, ticks);
}

static void test_ns_to_ticks_rejects_overflow_and_bad_arguments(void)
{
    uint32_t ticks = UNTOUCHED;

    TEST_ASSERT_FALSE(pulse_generator_ns_to_ticks(UINT32_MAX, UINT32_MAX, &ticks));
    TEST_ASSERT_FALSE(pulse_generator_ns_to_ticks(0u, 1000u, &ticks));
    TEST_ASSERT_EQUAL_HEX32(UNTOUCHED, ticks);
    TEST_ASSERT_FALSE(pulse_generator_ns_to_ticks(84000000u, 1000u, NULL));
}

static void test_ticks_to_us_converts_and_rounds(void)
{
    uint32_t us = 0;

    TEST_ASSERT_TRUE(pulse_generator_ticks_to_us(84000000u, 84u, &us));
    TEST_ASSERT_EQUAL_UINT32(1u, us);
    /* 84 MHz: 42 ticks is 0.5 us, 41 just under. */
    TEST_ASSERT_TRUE(pulse_generator_ticks_to_us(84000000u, 42u, &us));
    TEST_ASSERT_EQUAL_UINT32(1u, us);
    TEST_ASSERT_TRUE(pulse_generator_ticks_to_us(84000000u, 41u, &us));
    TEST_ASSERT_EQUAL_UINT32(0u, us);
}

static void test_ticks_to_us_rejects_overflow_and_bad_arguments(void)
{
    uint32_t us = UNTOUCHED;

    /* 1 Hz: every tick is a whole second. */
    TEST_ASSERT_FALSE(pulse_generator_ticks_to_us(1u, UINT32_MAX, &us));
    TEST_ASSERT_FALSE(pulse_generator_ticks_to_us(0u, 84u, &us));
    TEST_ASSERT_EQUAL_HEX32(UNTOUCHED, us);
    TEST_ASSERT_FALSE(pulse_generator_ticks_to_us(84000000u, 84u, NULL));
}

static void test_hz_to_ticks_converts_and_rounds(void)
{
    uint32_t ticks = 0;

    TEST_ASSERT_TRUE(pulse_generator_hz_to_ticks(84000000u, 1000u, &ticks));
    TEST_ASSERT_EQUAL_UINT32(84000u, ticks);
    /* 333333.3 and 142857.1 round down, 1.5 rounds up. */
    TEST_ASSERT_TRUE(pulse_generator_hz_to_ticks(1000000u, 3u, &ticks));
    TEST_ASSERT_EQUAL_UINT32(333333u, ticks);
    TEST_ASSERT_TRUE(pulse_generator_hz_to_ticks(1000000u, 7u, &ticks));
    TEST_ASSERT_EQUAL_UINT32(142857u, ticks);
    TEST_ASSERT_TRUE(pulse_generator_hz_to_ticks(3u, 2u, &ticks));
    TEST_ASSERT_EQUAL_UINT32(2u, ticks);
}

static void test_hz_to_ticks_accepts_half_a_tick_as_one(void)
{
    uint32_t ticks = 0;

    TEST_ASSERT_TRUE(pulse_generator_hz_to_ticks(1000000u, 2000000u, &ticks));
    TEST_ASSERT_EQUAL_UINT32(1u, ticks);
}

static void test_hz_to_ticks_rejects_an_interval_of_zero_ticks(void)
{
    uint32_t ticks = UNTOUCHED;

    TEST_ASSERT_FALSE(pulse_generator_hz_to_ticks(1000000u, 3000000u, &ticks));
    TEST_ASSERT_EQUAL_HEX32(UNTOUCHED, ticks);
}

static void test_hz_to_ticks_rejects_bad_arguments(void)
{
    uint32_t ticks = UNTOUCHED;

    TEST_ASSERT_FALSE(pulse_generator_hz_to_ticks(0u, 1000u, &ticks));
    TEST_ASSERT_FALSE(pulse_generator_hz_to_ticks(1000000u, 0u, &ticks));
    TEST_ASSERT_EQUAL_HEX32(UNTOUCHED, ticks);
    TEST_ASSERT_FALSE(pulse_generator_hz_to_ticks(1000000u, 1000u, NULL));
}

/* --- Spread --- */

/* Takes up to `steps` intervals and checks each prefix sum against
   floor(k * ticks / pulses), the intervals differing by at most one tick. */
static void check_spread(uint32_t pulses, uint32_t ticks, uint32_t steps)
{
    pulse_generator_spread_t spread;
    uint64_t sum = 0;
    uint32_t shortest = UINT32_MAX;
    uint32_t longest = 0;

    TEST_ASSERT_TRUE(pulse_generator_spread_init(&spread, pulses, ticks));
    for (uint32_t k = 1; k <= steps; k++) {
        uint32_t interval = 0;

        TEST_ASSERT_TRUE(pulse_generator_spread_next(&spread, &interval));
        sum += interval;
        TEST_ASSERT_EQUAL_UINT64((uint64_t)k * ticks / pulses, sum);
        if (interval < shortest) shortest = interval;
        if (interval > longest) longest = interval;
    }
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(1u, longest - shortest);
}

static void test_spread_seven_pulses_over_one_hundred_ticks(void)
{
    static const uint32_t expected[] = {14, 14, 14, 15, 14, 14, 15};
    pulse_generator_spread_t spread;
    uint32_t got[7];

    TEST_ASSERT_TRUE(pulse_generator_spread_init(&spread, 7u, 100u));
    for (size_t i = 0; i < 7; i++) {
        TEST_ASSERT_TRUE(pulse_generator_spread_next(&spread, &got[i]));
    }
    TEST_ASSERT_EQUAL_UINT32_ARRAY(expected, got, 7);
}

static void test_spread_lands_every_pulse_on_its_rounded_instant(void)
{
    check_spread(7u, 100u, 7u);
    check_spread(3u, 3u, 3u);
    check_spread(1u, 123456u, 1u);
    check_spread(1000u, 999999u, 1000u);
    check_spread(997u, 1000000u, 997u);
}

static void test_spread_handles_the_full_32_bit_range(void)
{
    /* Only the first 1000 of each: enough to exercise the error term near
       its limits without iterating billions of times. */
    check_spread(0xC0000000u, UINT32_MAX, 1000u);
    check_spread(1000u, UINT32_MAX, 1000u);
}

static void test_spread_counts_down_and_then_runs_dry(void)
{
    pulse_generator_spread_t spread;
    uint32_t interval = 0;

    TEST_ASSERT_TRUE(pulse_generator_spread_init(&spread, 2u, 10u));
    TEST_ASSERT_EQUAL_UINT32(2u, pulse_generator_spread_remaining(&spread));
    TEST_ASSERT_TRUE(pulse_generator_spread_next(&spread, &interval));
    TEST_ASSERT_EQUAL_UINT32(1u, pulse_generator_spread_remaining(&spread));
    TEST_ASSERT_TRUE(pulse_generator_spread_next(&spread, &interval));
    TEST_ASSERT_EQUAL_UINT32(0u, pulse_generator_spread_remaining(&spread));

    interval = UNTOUCHED;
    TEST_ASSERT_FALSE(pulse_generator_spread_next(&spread, &interval));
    TEST_ASSERT_EQUAL_HEX32(UNTOUCHED, interval);
}

static void test_spread_init_rejects_bad_arguments(void)
{
    pulse_generator_spread_t spread;
    pulse_generator_spread_t before;

    memset(&spread, 0xA5, sizeof spread);
    before = spread;

    TEST_ASSERT_FALSE(pulse_generator_spread_init(&spread, 0u, 100u));
    TEST_ASSERT_FALSE(pulse_generator_spread_init(&spread, 10u, 9u));
    TEST_ASSERT_EQUAL_MEMORY(&before, &spread, sizeof spread);
    TEST_ASSERT_FALSE(pulse_generator_spread_init(NULL, 1u, 1u));
}

static void test_spread_tolerates_null_arguments(void)
{
    pulse_generator_spread_t spread;

    TEST_ASSERT_TRUE(pulse_generator_spread_init(&spread, 1u, 1u));
    TEST_ASSERT_FALSE(pulse_generator_spread_next(&spread, NULL));
    TEST_ASSERT_EQUAL_UINT32(1u, pulse_generator_spread_remaining(&spread));
    TEST_ASSERT_FALSE(pulse_generator_spread_next(NULL, &(uint32_t){0}));
    TEST_ASSERT_EQUAL_UINT32(0u, pulse_generator_spread_remaining(NULL));
}

/* --- Timeline --- */

static void test_timeline_rounds_each_instant_not_each_interval(void)
{
    pulse_generator_timeline_t tl;
    uint32_t interval = 0;

    /* 1 MHz: 1.5, 3.0 and 4.5 ticks round to 2, 3 and 5. */
    TEST_ASSERT_TRUE(pulse_generator_timeline_init(&tl, 1000000u));
    TEST_ASSERT_TRUE(pulse_generator_timeline_next(&tl, 1500u, &interval));
    TEST_ASSERT_EQUAL_UINT32(2u, interval);
    TEST_ASSERT_TRUE(pulse_generator_timeline_next(&tl, 3000u, &interval));
    TEST_ASSERT_EQUAL_UINT32(1u, interval);
    TEST_ASSERT_TRUE(pulse_generator_timeline_next(&tl, 4500u, &interval));
    TEST_ASSERT_EQUAL_UINT32(2u, interval);
}

static void test_timeline_does_not_drift_over_a_million_pulses(void)
{
    pulse_generator_timeline_t tl;
    uint64_t sum = 0;

    /* 84 MHz, one pulse every 142857 ns (~7 kHz): 11999.988 ticks each.
       Rounding every interval on its own would give 12000 each, 12000
       ticks too many after a million pulses. */
    TEST_ASSERT_TRUE(pulse_generator_timeline_init(&tl, 84000000u));
    for (uint64_t k = 1; k <= 1000000u; k++) {
        uint32_t interval = 0;

        TEST_ASSERT_TRUE(pulse_generator_timeline_next(&tl, k * 142857u, &interval));
        sum += interval;
    }
    TEST_ASSERT_EQUAL_UINT64(11999988000ull, sum);
}

static void test_timeline_reaches_hours_without_overflow(void)
{
    pulse_generator_timeline_t tl;

    /* 84 MHz, a pulse every 50 s for 10 hours: 4.2e9 ticks each, just
       under the 32-bit limit, at instants whose ticks no longer fit. */
    TEST_ASSERT_TRUE(pulse_generator_timeline_init(&tl, 84000000u));
    for (uint64_t k = 1; k <= 720u; k++) {
        uint32_t interval = 0;

        TEST_ASSERT_TRUE(pulse_generator_timeline_next(&tl, k * 50000000000ull, &interval));
        TEST_ASSERT_EQUAL_UINT32(4200000000u, interval);
    }
}

static void test_timeline_rejects_an_interval_that_does_not_fit(void)
{
    pulse_generator_timeline_t tl;
    uint32_t interval = UNTOUCHED;

    /* 84 MHz: 52 s is 4.368e9 ticks. */
    TEST_ASSERT_TRUE(pulse_generator_timeline_init(&tl, 84000000u));
    TEST_ASSERT_FALSE(pulse_generator_timeline_next(&tl, 52000000000ull, &interval));
    TEST_ASSERT_EQUAL_HEX32(UNTOUCHED, interval);
    /* Nothing was accepted: the next instant is still measured from 0. */
    TEST_ASSERT_TRUE(pulse_generator_timeline_next(&tl, 1000u, &interval));
    TEST_ASSERT_EQUAL_UINT32(84u, interval);
}

static void test_timeline_rejects_an_instant_whose_ticks_overflow(void)
{
    pulse_generator_timeline_t tl;
    uint32_t interval = UNTOUCHED;

    TEST_ASSERT_TRUE(pulse_generator_timeline_init(&tl, UINT32_MAX));
    TEST_ASSERT_FALSE(pulse_generator_timeline_next(&tl, UINT64_MAX, &interval));
    TEST_ASSERT_EQUAL_HEX32(UNTOUCHED, interval);
}

static void test_timeline_rejects_instants_that_do_not_move_forward(void)
{
    pulse_generator_timeline_t tl;
    uint32_t interval = 0;

    TEST_ASSERT_TRUE(pulse_generator_timeline_init(&tl, 1000000u));
    TEST_ASSERT_TRUE(pulse_generator_timeline_next(&tl, 2000u, &interval));

    interval = UNTOUCHED;
    TEST_ASSERT_FALSE(pulse_generator_timeline_next(&tl, 1000u, &interval)); /* earlier */
    TEST_ASSERT_FALSE(pulse_generator_timeline_next(&tl, 2400u, &interval)); /* same tick */
    TEST_ASSERT_EQUAL_HEX32(UNTOUCHED, interval);

    /* Still measured from tick 2. */
    TEST_ASSERT_TRUE(pulse_generator_timeline_next(&tl, 3000u, &interval));
    TEST_ASSERT_EQUAL_UINT32(1u, interval);
}

static void test_timeline_rejects_bad_arguments(void)
{
    pulse_generator_timeline_t tl;

    TEST_ASSERT_FALSE(pulse_generator_timeline_init(NULL, 1000000u));
    TEST_ASSERT_FALSE(pulse_generator_timeline_init(&tl, 0u));
    TEST_ASSERT_TRUE(pulse_generator_timeline_init(&tl, 1000000u));
    TEST_ASSERT_FALSE(pulse_generator_timeline_next(&tl, 1000u, NULL));
    TEST_ASSERT_FALSE(pulse_generator_timeline_next(NULL, 1000u, &(uint32_t){0}));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_us_to_ticks_converts_whole_ticks);
    RUN_TEST(test_us_to_ticks_rounds_halves_up);
    RUN_TEST(test_us_to_ticks_accepts_the_largest_result);
    RUN_TEST(test_us_to_ticks_rejects_a_result_that_does_not_fit);
    RUN_TEST(test_us_to_ticks_rejects_bad_arguments);
    RUN_TEST(test_ns_to_ticks_converts_and_rounds);
    RUN_TEST(test_ns_to_ticks_rejects_overflow_and_bad_arguments);
    RUN_TEST(test_ticks_to_us_converts_and_rounds);
    RUN_TEST(test_ticks_to_us_rejects_overflow_and_bad_arguments);
    RUN_TEST(test_hz_to_ticks_converts_and_rounds);
    RUN_TEST(test_hz_to_ticks_accepts_half_a_tick_as_one);
    RUN_TEST(test_hz_to_ticks_rejects_an_interval_of_zero_ticks);
    RUN_TEST(test_hz_to_ticks_rejects_bad_arguments);
    RUN_TEST(test_spread_seven_pulses_over_one_hundred_ticks);
    RUN_TEST(test_spread_lands_every_pulse_on_its_rounded_instant);
    RUN_TEST(test_spread_handles_the_full_32_bit_range);
    RUN_TEST(test_spread_counts_down_and_then_runs_dry);
    RUN_TEST(test_spread_init_rejects_bad_arguments);
    RUN_TEST(test_spread_tolerates_null_arguments);
    RUN_TEST(test_timeline_rounds_each_instant_not_each_interval);
    RUN_TEST(test_timeline_does_not_drift_over_a_million_pulses);
    RUN_TEST(test_timeline_reaches_hours_without_overflow);
    RUN_TEST(test_timeline_rejects_an_interval_that_does_not_fit);
    RUN_TEST(test_timeline_rejects_an_instant_whose_ticks_overflow);
    RUN_TEST(test_timeline_rejects_instants_that_do_not_move_forward);
    RUN_TEST(test_timeline_rejects_bad_arguments);
    return UNITY_END();
}
