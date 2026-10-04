#include "unity.h"
#include "pulse_generator.h"
#include "mock_platform.h"

/* Operations that are still placeholders, and platforms that leave the
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

static void test_a_platform_without_the_optional_hooks_drives_the_timer_normally(void)
{
    pulse_generator_t pg;
    init_instance(&pg, &g_mock_ops_minimal);

    /* Leaving dma_* NULL costs the platform nothing: no stubs to write, and
       the two implemented modes are unaffected. */
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK,
                      pulse_generator_start_fixed_count(&pg, 1000, 2));

    for (int i = 0; i < 2 * 2; i++) {
        mock_hw_fire_and_notify(&pg, &hw);
    }

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_FALSE(hw.channel_running);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_start_profile_is_not_supported_yet);
    RUN_TEST(test_start_profile_with_null_pg_returns_invalid_param);
    RUN_TEST(test_notify_dma_complete_is_not_supported_yet);
    RUN_TEST(test_a_platform_without_the_optional_hooks_drives_the_timer_normally);
    return UNITY_END();
}
