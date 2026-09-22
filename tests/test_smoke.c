#include "unity.h"
#include "pulse_generator.h"

void setUp(void) {}
void tearDown(void) {}

static void test_build_pipeline_works(void)
{
    TEST_ASSERT_TRUE(1);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_build_pipeline_works);
    return UNITY_END();
}
