#include "pulse_generator.h"

/* Checked once in init(), so the rest of the library can call these without
   guarding every use. The optional hooks are not here: a NULL one means the
   platform does not support the operation needing it. */
static bool has_mandatory_hooks(const pulse_generator_ops_t *ops)
{
    return ops->channel_start != NULL &&
           ops->channel_stop != NULL &&
           ops->set_compare != NULL &&
           ops->get_counter != NULL &&
           ops->get_tick_hz != NULL &&
           ops->get_counter_max != NULL;
}

pulse_generator_status_t pulse_generator_init(
    pulse_generator_t *pg,
    const pulse_generator_config_t *config)
{
    if (pg == NULL || config == NULL || config->ops == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    if (!has_mandatory_hooks(config->ops)) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    *pg = (pulse_generator_t){
        .ops      = config->ops,
        .hw       = config->hw,
        .on_event = config->on_event,
        .user_ctx = config->user_ctx,
    };

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

    return pg->edge_count / 2;
}

bool pulse_generator_is_busy(const pulse_generator_t *pg)
{
    return pulse_generator_get_state(pg) == PULSE_GENERATOR_STATE_RUNNING;
}

void pulse_generator_reset_pulse_count(pulse_generator_t *pg)
{
    if (pg == NULL) {
        return;
    }

    pg->edge_count = 0;
}

/* Reads the callback into locals before invoking it: the instance is already
   in its post-event state, and this way a callback that reconfigures the
   instance cannot pull the fields out from under this call. */
static void emit_event(pulse_generator_t *pg, pulse_generator_event_t event)
{
    pulse_generator_event_cb_t callback = pg->on_event;
    void *user_ctx = pg->user_ctx;

    if (callback != NULL) {
        callback(pg, event, user_ctx);
    }
}

pulse_generator_status_t pulse_generator_stop(pulse_generator_t *pg)
{
    if (pg == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    if (pg->state == PULSE_GENERATOR_STATE_IDLE) {
        return PULSE_GENERATOR_OK;
    }

    pulse_generator_status_t status = pg->ops->channel_stop(pg->hw);
    pg->state = PULSE_GENERATOR_STATE_IDLE;
    pg->edge_count = 0;

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

    uint32_t ticks = (pg->tick_hz / 2) / frequency_hz;
    if (ticks == 0 || ticks > pg->counter_max) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    *half_period_ticks = ticks;
    return PULSE_GENERATOR_OK;
}

/* Snapshots the platform properties that must not change while a movement
   runs, so that neither the compare ISR nor set_frequency() needs a hook
   call to read them back. */
static pulse_generator_status_t capture_timing(pulse_generator_t *pg)
{
    uint32_t counter_max = pg->ops->get_counter_max(pg->hw);

    /* Every compare value is masked with counter_max, which only wraps
       correctly if the counter's range is a power of two. Checking it here
       turns a platform reporting, say, a clamped maximum into a start that
       fails loudly instead of an output that is silently wrong. */
    if (counter_max == 0 || (counter_max & (counter_max + 1u)) != 0) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    pg->counter_max = counter_max;
    pg->tick_hz = pg->ops->get_tick_hz(pg->hw);

    return PULSE_GENERATOR_OK;
}

/* Schedules the toggle after the match being serviced, half a period after
   that match rather than after "now", so interrupt latency does not
   accumulate as drift. */
static pulse_generator_status_t schedule_next_compare(pulse_generator_t *pg)
{
    const uint32_t mask = pg->counter_max;
    const uint32_t half_period_ticks = pg->half_period_ticks;
    const uint32_t previous = pg->last_compare;

    uint32_t next = (previous + half_period_ticks) & mask;
    pulse_generator_status_t status = pg->ops->set_compare(pg->hw, next);

    /* Tracked even when the write failed, so the reference stays the value
       the library asked for rather than a stale one. */
    pg->last_compare = next;
    if (status != PULSE_GENERATOR_OK) {
        return status;
    }

    /* Ticks elapsed since the match being serviced, modulo the counter's
       width. Read once into a local: a second read would see a later counter
       and could disagree with this comparison. If a whole half period has
       already gone by, the counter is at or past `next` and that match would
       not fire until the counter wraps all the way around, so reschedule
       from now instead, losing phase but not a full wrap. */
    const uint32_t counter = pg->ops->get_counter(pg->hw);
    if (((counter - previous) & mask) >= half_period_ticks) {
        next = (counter + half_period_ticks) & mask;
        status = pg->ops->set_compare(pg->hw, next);
        pg->last_compare = next;

        return (status == PULSE_GENERATOR_OK) ? PULSE_GENERATOR_ERROR_MISSED_COMPARE : status;
    }

    return PULSE_GENERATOR_OK;
}

static pulse_generator_status_t start_timer_movement(
    pulse_generator_t *pg,
    pulse_generator_mode_t mode,
    uint32_t frequency_hz,
    uint32_t target_edge_count)
{
    pulse_generator_status_t status = capture_timing(pg);
    if (status != PULSE_GENERATOR_OK) {
        return status;
    }

    uint32_t half_period_ticks;
    status = frequency_to_half_period_ticks(pg, frequency_hz, &half_period_ticks);
    if (status != PULSE_GENERATOR_OK) {
        return status;
    }

    /* The channel is stopped, so the counter is either frozen or running for
       some other channel of the same timer. Reading it here and leaving
       channel_start() to write the compare register as late as it can keeps
       the first half period from being eaten by the arming itself. */
    uint32_t first_compare = (pg->ops->get_counter(pg->hw) + half_period_ticks) & pg->counter_max;

    /* Publish the movement before arming the hardware: with a short half
       period the first compare-match interrupt can fire before
       channel_start() returns, and notify_compare_match() must already see
       the instance as RUNNING to schedule the next toggle. */
    pg->mode = mode;
    pg->half_period_ticks = half_period_ticks;
    pg->edge_count = 0;
    pg->target_edge_count = target_edge_count;
    pg->last_compare = first_compare;
    pg->state = PULSE_GENERATOR_STATE_RUNNING;

    status = pg->ops->channel_start(pg->hw, first_compare);
    if (status != PULSE_GENERATOR_OK) {
        pg->state = PULSE_GENERATOR_STATE_IDLE;
    }

    return status;
}

pulse_generator_status_t pulse_generator_start_fixed_count(
    pulse_generator_t *pg,
    uint32_t frequency_hz,
    uint32_t pulse_count)
{
    if (pg == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    if (pg->state != PULSE_GENERATOR_STATE_IDLE) {
        return PULSE_GENERATOR_ERROR_INVALID_STATE;
    }

    /* Each pulse is two compare matches, so the edge target would overflow
       past half the range. */
    if (pulse_count == 0 || pulse_count > UINT32_MAX / 2u) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    return start_timer_movement(pg, PULSE_GENERATOR_MODE_FIXED_COUNT,
                                frequency_hz, pulse_count * 2u);
}

pulse_generator_status_t pulse_generator_start_continuous(
    pulse_generator_t *pg,
    uint32_t frequency_hz)
{
    if (pg == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    if (pg->state != PULSE_GENERATOR_STATE_IDLE) {
        return PULSE_GENERATOR_ERROR_INVALID_STATE;
    }

    /* No edge target: runs until stop(). */
    return start_timer_movement(pg, PULSE_GENERATOR_MODE_CONTINUOUS, frequency_hz, 0);
}

pulse_generator_status_t pulse_generator_start_profile(
    pulse_generator_t *pg,
    const uint32_t *intervals,
    size_t len)
{
    (void)intervals;
    (void)len;

    if (pg == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    /* A placeholder the scheduled mode (M8) replaces rather than fills in. */
    return PULSE_GENERATOR_ERROR_NOT_SUPPORTED;
}

pulse_generator_status_t pulse_generator_set_frequency(pulse_generator_t *pg, uint32_t frequency_hz)
{
    if (pg == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    if (pg->state != PULSE_GENERATOR_STATE_RUNNING || pg->mode != PULSE_GENERATOR_MODE_CONTINUOUS) {
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

pulse_generator_status_t pulse_generator_notify_compare_match(pulse_generator_t *pg)
{
    if (pg == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    if (pg->state != PULSE_GENERATOR_STATE_RUNNING ||
        (pg->mode != PULSE_GENERATOR_MODE_FIXED_COUNT &&
         pg->mode != PULSE_GENERATOR_MODE_CONTINUOUS)) {
        return PULSE_GENERATOR_OK;
    }

    pg->edge_count++;

    if (pg->target_edge_count == 0 || pg->edge_count < pg->target_edge_count) {
        pulse_generator_status_t status = schedule_next_compare(pg);
        if (status == PULSE_GENERATOR_ERROR_MISSED_COMPARE) {
            emit_event(pg, PULSE_GENERATOR_EVENT_MISSED_COMPARE);
        }

        return status;
    }

    pulse_generator_status_t status = pg->ops->channel_stop(pg->hw);
    pg->state = PULSE_GENERATOR_STATE_IDLE;
    pg->edge_count = 0;

    emit_event(pg, PULSE_GENERATOR_EVENT_COMPLETE);

    return status;
}

pulse_generator_status_t pulse_generator_notify_dma_complete(pulse_generator_t *pg)
{
    if (pg == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    /* A placeholder the scheduled mode (M8) replaces rather than fills in. */
    return PULSE_GENERATOR_ERROR_NOT_SUPPORTED;
}
