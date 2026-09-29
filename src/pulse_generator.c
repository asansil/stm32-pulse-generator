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

    return pg->toggle_count / 2;
}

bool pulse_generator_is_busy(const pulse_generator_t *pg)
{
    return pulse_generator_get_state(pg) == PULSE_GENERATOR_STATE_RUNNING;
}

pulse_generator_status_t pulse_generator_set_complete_callback(
    pulse_generator_t *pg,
    pulse_generator_complete_cb_t callback,
    void *user_ctx)
{
    if (pg == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    pg->complete_cb = callback;
    pg->complete_cb_user_ctx = user_ctx;

    return PULSE_GENERATOR_OK;
}

void pulse_generator_reset_pulse_count(pulse_generator_t *pg)
{
    if (pg == NULL) {
        return;
    }

    pg->toggle_count = 0;
}

pulse_generator_status_t pulse_generator_stop(pulse_generator_t *pg)
{
    if (pg == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    if (pg->state == PULSE_GENERATOR_STATE_IDLE) {
        return PULSE_GENERATOR_OK;
    }

    pulse_generator_status_t status = pg->platform->timer_stop(pg->platform->ctx);
    pg->state = PULSE_GENERATOR_STATE_IDLE;
    pg->toggle_count = 0;

    return status;
}

static uint32_t frequency_to_ccr(uint32_t frequency_hz)
{
    /* Placeholder until M5: the platform doesn't expose the timer clock
       yet, so ccr just echoes frequency_hz. Replace with a real Hz ->
       timer-ticks conversion once real hardware exists. */
    return frequency_hz;
}

pulse_generator_status_t pulse_generator_start_fixed_count(
    pulse_generator_t *pg,
    pulse_generator_backend_t backend,
    uint32_t frequency_hz,
    uint32_t pulse_count)
{
    if (pg == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    if (pg->state != PULSE_GENERATOR_STATE_IDLE) {
        return PULSE_GENERATOR_ERROR_INVALID_STATE;
    }

    if (backend == PULSE_GENERATOR_BACKEND_BITBANG) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    if (frequency_hz == 0 || pulse_count == 0) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    pulse_generator_status_t status =
        pg->platform->set_compare(pg->platform->ctx, frequency_to_ccr(frequency_hz));
    if (status != PULSE_GENERATOR_OK) {
        return status;
    }

    status = pg->platform->timer_start(pg->platform->ctx);
    if (status != PULSE_GENERATOR_OK) {
        return status;
    }

    pg->mode = PULSE_GENERATOR_MODE_FIXED_COUNT_TIMER;
    pg->frequency_hz = frequency_hz;
    pg->target_pulse_count = pulse_count;
    pg->toggle_count = 0;
    pg->state = PULSE_GENERATOR_STATE_RUNNING;

    return PULSE_GENERATOR_OK;
}

pulse_generator_status_t pulse_generator_tick(pulse_generator_t *pg, uint32_t elapsed_us)
{
    (void)elapsed_us;

    if (pg == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    /* No-op until bit-bang backends exist (M8). */
    return PULSE_GENERATOR_OK;
}

pulse_generator_status_t pulse_generator_notify_compare_match(pulse_generator_t *pg)
{
    if (pg == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    if (pg->state != PULSE_GENERATOR_STATE_RUNNING || pg->mode != PULSE_GENERATOR_MODE_FIXED_COUNT_TIMER) {
        return PULSE_GENERATOR_OK;
    }

    pg->toggle_count++;

    if (pg->toggle_count < pg->target_pulse_count * 2) {
        return PULSE_GENERATOR_OK;
    }

    pulse_generator_status_t status = pg->platform->timer_stop(pg->platform->ctx);
    pg->state = PULSE_GENERATOR_STATE_IDLE;
    pg->toggle_count = 0;

    pulse_generator_complete_cb_t cb = pg->complete_cb;
    void *user_ctx = pg->complete_cb_user_ctx;
    if (cb != NULL) {
        cb(user_ctx);
    }

    return status;
}
