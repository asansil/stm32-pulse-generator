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

/* Toggle mode: two compare matches (two toggles) make one full pulse, so
   the time between toggles is half the pulse period. Dividing twice
   instead of by (2 * frequency_hz) avoids overflowing the product. */
static pulse_generator_status_t frequency_to_half_period_ticks(
    const pulse_generator_t *pg,
    uint32_t frequency_hz,
    uint32_t *half_period_ticks)
{
    if (frequency_hz == 0) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    uint32_t timer_main_clk = pg->platform->get_timer_main_clk(pg->platform->ctx);
    uint32_t ticks = (timer_main_clk / 2) / frequency_hz;
    if (ticks == 0 || ticks > pg->platform->get_max_ticks(pg->platform->ctx)) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    *half_period_ticks = ticks;
    return PULSE_GENERATOR_OK;
}

static pulse_generator_status_t start_timer_movement(
    pulse_generator_t *pg,
    pulse_generator_mode_t mode,
    uint32_t frequency_hz)
{
    uint32_t half_period_ticks;
    pulse_generator_status_t status = frequency_to_half_period_ticks(pg, frequency_hz, &half_period_ticks);
    if (status != PULSE_GENERATOR_OK) {
        return status;
    }

    /* Publish the movement before arming the hardware: with a short half
       period the first compare-match interrupt can fire before
       timer_start() returns, and notify_compare_match() must already see
       the instance as RUNNING to schedule the next toggle. */
    pg->mode = mode;
    pg->half_period_ticks = half_period_ticks;
    pg->toggle_count = 0;
    pg->state = PULSE_GENERATOR_STATE_RUNNING;

    status = pg->platform->timer_start(pg->platform->ctx, half_period_ticks);
    if (status != PULSE_GENERATOR_OK) {
        pg->state = PULSE_GENERATOR_STATE_IDLE;
    }

    return status;
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

    if (pulse_count == 0) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    pg->target_pulse_count = pulse_count;

    return start_timer_movement(pg, PULSE_GENERATOR_MODE_FIXED_COUNT_TIMER, frequency_hz);
}

pulse_generator_status_t pulse_generator_start_continuous(
    pulse_generator_t *pg,
    pulse_generator_backend_t backend,
    uint32_t frequency_hz)
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

    return start_timer_movement(pg, PULSE_GENERATOR_MODE_CONTINUOUS_TIMER, frequency_hz);
}

pulse_generator_status_t pulse_generator_set_frequency(pulse_generator_t *pg, uint32_t frequency_hz)
{
    if (pg == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    if (pg->state != PULSE_GENERATOR_STATE_RUNNING || pg->mode != PULSE_GENERATOR_MODE_CONTINUOUS_TIMER) {
        return PULSE_GENERATOR_ERROR_INVALID_STATE;
    }

    uint32_t half_period_ticks;
    pulse_generator_status_t status = frequency_to_half_period_ticks(pg, frequency_hz, &half_period_ticks);
    if (status != PULSE_GENERATOR_OK) {
        return status;
    }

    pg->half_period_ticks = half_period_ticks;

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

    if (pg->state != PULSE_GENERATOR_STATE_RUNNING ||
        (pg->mode != PULSE_GENERATOR_MODE_FIXED_COUNT_TIMER &&
         pg->mode != PULSE_GENERATOR_MODE_CONTINUOUS_TIMER)) {
        return PULSE_GENERATOR_OK;
    }

    pg->toggle_count++;

    if (pg->mode == PULSE_GENERATOR_MODE_CONTINUOUS_TIMER ||
        pg->toggle_count < pg->target_pulse_count * 2) {
        return pg->platform->advance_compare(pg->platform->ctx, pg->half_period_ticks);
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
