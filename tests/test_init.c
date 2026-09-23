#include "unity.h"
#include "pulse_generator.h"
#include "mock_platform.h"

void setUp(void)
{
    mock_platform_reset();
}

void tearDown(void) {}

static void test_init_with_valid_platform_returns_ok(void)
{
    pulse_generator_t pg;

    pulse_generator_status_t status = pulse_generator_init(&pg, &g_mock_platform);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_OK, status);
    TEST_ASSERT_EQUAL(PULSE_GENERATOR_STATE_IDLE, pulse_generator_get_state(&pg));
    TEST_ASSERT_EQUAL_UINT32(0, pulse_generator_get_pulse_count(&pg));
}

static void test_init_with_null_instance_returns_invalid_param(void)
{
    pulse_generator_status_t status = pulse_generator_init(NULL, &g_mock_platform);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
}

static void test_init_with_null_platform_returns_invalid_param(void)
{
    pulse_generator_t pg;

    pulse_generator_status_t status = pulse_generator_init(&pg, NULL);

    TEST_ASSERT_EQUAL(PULSE_GENERATOR_ERROR_INVALID_PARAM, status);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_with_valid_platform_returns_ok);
    RUN_TEST(test_init_with_null_instance_returns_invalid_param);
    RUN_TEST(test_init_with_null_platform_returns_invalid_param);
    return UNITY_END();
}
