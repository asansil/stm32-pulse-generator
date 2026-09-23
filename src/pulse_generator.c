#include "pulse_generator.h"

pulse_generator_status_t pulse_generator_init(
    pulse_generator_t *pg,
    const pulse_generator_platform_t *platform)
{
    if (pg == NULL || platform == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    *pg = (pulse_generator_t){0};
    pg->platform = platform;

    return PULSE_GENERATOR_OK;
}

pulse_generator_state_t pulse_generator_get_state(const pulse_generator_t *pg)
{
    if (pg == NULL) {
        return PULSE_GENERATOR_STATE_IDLE;
    }

    return pg->state;
}

uint32_t pulse_generator_get_pulse_count(const pulse_generator_t *pg)
{
    if (pg == NULL) {
        return 0;
    }

    return pg->pulse_count;
}

/* Remaining functions to be added incrementally, mode by mode. */
