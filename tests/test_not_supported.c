#include "unity.h"
#include "pulse_generator.h"
#include "mock_platform.h"

/* Operations whose backend does not exist yet, and platforms that leave the
   optional hooks NULL: both must report NOT_SUPPORTED rather than failing as
   a bad parameter or dereferencing a missing hook. */

static mock_hw_t hw;

void setUp(void)
{
    mock_hw_init(&hw);
}

void tearDown(void) {}

static void init_instance(pulse_generator_t *pg, const pulse_generator_ops_t *ops)
{
    pulse_generator_init(pg, &(pulse_generator_config_t){ .ops = ops, .hw = &hw });
}

static void test_start_profile_is_not_supported_yet(void)
{
    static const uint32_t profile[4] = { 100, 200, 300, 400 };
    pulse_generator_t pg;
    init_instance(&pg, &g_mock_ops);

    pulse_generator_status_t status = pulse_generator_start_profile(&pg, profile, 4);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_NOT_SUPPORTED, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_FALSE(hw.dma_running);
}

static void test_start_profile_with_null_pg_returns_invalid_param(void)
{
    pulse_generator_status_t status = pulse_generator_start_profile(NULL, NULL, 0);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
}

static void test_notify_dma_complete_is_not_supported_yet(void)
{
    pulse_generator_t pg;
    init_instance(&pg, &g_mock_ops);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_NOT_SUPPORTED, pulse_generator_notify_dma_complete(&pg));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_notify_dma_complete(NULL));
}

static void test_the_bitbang_backend_is_not_supported_yet(void)
{
    pulse_generator_t pg;
    init_instance(&pg, &g_mock_ops);

    /* A valid request the library cannot serve yet, which is what
       NOT_SUPPORTED means — as opposed to a malformed one. */
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_NOT_SUPPORTED,
                      pulse_generator_start_fixed_count(&pg, PULSE_GENERATOR_BACKEND_BITBANG, 1000, 10));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_NOT_SUPPORTED,
                      pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_BITBANG, 1000));
    TEST_ASSERT_FALSE(hw.gpio_state);
}

static void test_tick_is_a_noop_for_a_timer_driven_movement(void)
{
    pulse_generator_t pg;
    init_instance(&pg, &g_mock_ops);
    pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, pulse_generator_tick(&pg, 1000));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, pulse_generator_tick(NULL, 1000));

    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));
    TEST_ASSERT_EQUAL(0, hw.set_compare_call_count);
    TEST_ASSERT_FALSE(hw.gpio_state);
}

static void test_a_platform_without_the_optional_hooks_drives_the_timer_normally(void)
{
    pulse_generator_t pg;
    init_instance(&pg, &g_mock_ops_minimal);

    /* Leaving dma_* and gpio_* NULL costs the platform nothing: no stubs to
       write, and the timer-driven modes are unaffected. */
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK,
                      pulse_generator_start_fixed_count(&pg, PULSE_GENERATOR_BACKEND_TIMER, 1000, 2));

    for (int i = 0; i < 2 * 2; i++) {
        mock_hw_fire_and_notify(&pg, &hw);
    }

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_FALSE(hw.channel_running);
}

static void test_a_platform_without_the_optional_hooks_reports_not_supported(void)
{
    pulse_generator_t pg;
    init_instance(&pg, &g_mock_ops_minimal);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_NOT_SUPPORTED,
                      pulse_generator_start_profile(&pg, NULL, 0));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_NOT_SUPPORTED,
                      pulse_generator_notify_dma_complete(&pg));
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_NOT_SUPPORTED,
                      pulse_generator_start_continuous(&pg, PULSE_GENERATOR_BACKEND_BITBANG, 1000));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_start_profile_is_not_supported_yet);
    RUN_TEST(test_start_profile_with_null_pg_returns_invalid_param);
    RUN_TEST(test_notify_dma_complete_is_not_supported_yet);
    RUN_TEST(test_the_bitbang_backend_is_not_supported_yet);
    RUN_TEST(test_tick_is_a_noop_for_a_timer_driven_movement);
    RUN_TEST(test_a_platform_without_the_optional_hooks_drives_the_timer_normally);
    RUN_TEST(test_a_platform_without_the_optional_hooks_reports_not_supported);
    return UNITY_END();
}
