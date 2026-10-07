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

/* --- Scheduled mode --- */

/* The queue exists only between prepare_scheduled() and the next init(); in
   any other mode its fields are zero or stale and must not be read. */
static bool is_scheduled(const pulse_generator_t *pg)
{
    return pg->mode == PULSE_GENERATOR_MODE_SCHEDULED &&
           pg->state != PULSE_GENERATOR_STATE_IDLE;
}

/* Each index is read once, so the result is consistent even while the other
   side moves its own: at worst one event out of date, never torn. The
   capacity need not be a power of two, hence the explicit wrap rather than
   a mask. */
static size_t pending_events(const pulse_generator_t *pg)
{
    size_t head = pg->sched.queue_head;
    size_t tail = pg->sched.queue_tail;

    return (head >= tail) ? head - tail : pg->sched.queue_capacity - tail + head;
}

static size_t next_index(const pulse_generator_t *pg, size_t index)
{
    return (index + 1 == pg->sched.queue_capacity) ? 0 : index + 1;
}

/* The pulse is high for interval / 2 and low for the rest, which is never
   shorter, so checking the first half covers both. */
static bool interval_is_valid(const pulse_generator_t *pg, uint32_t interval)
{
    return interval >= 2 &&
           interval / 2 >= pg->sched.min_interval_ticks &&
           interval <= pg->counter_max;
}

/* Consumer side of the queue. The slot is read before tail is published,
   so the producer cannot overwrite it while it is still being read. */
static bool pop_event(pulse_generator_t *pg, uint32_t *interval)
{
    const size_t tail = pg->sched.queue_tail;
    if (tail == pg->sched.queue_head) {
        return false;
    }

    *interval = pg->sched.queue[tail];
    pg->sched.queue_tail = next_index(pg, tail);

    return true;
}

/* Every way out of a SCHEDULED movement lands here, back in ARMED. The
   events left were timed against a train that no longer exists, so they
   are dropped, and so is the finish request, which belonged to it.
   Flushing moves tail up to head and leaves head alone: it stays the
   producer's. */
static void end_movement(pulse_generator_t *pg)
{
    pg->state = PULSE_GENERATOR_STATE_ARMED;
    pg->sched.queue_tail = pg->sched.queue_head;
    pg->sched.finish_requested = false;
    pg->edge_count = 0;
}

/* Stops the channel, lands back in ARMED and reports `event`, counting it if
   it is an underrun. `status` is what the ISR returns, unless it was only
   going to say OK or UNDERRUN and stopping the channel fails: then the
   platform's error says more, as the header's "otherwise whatever the
   platform returned" promises. */
static pulse_generator_status_t stop_movement(pulse_generator_t *pg,
                                              pulse_generator_event_t event,
                                              pulse_generator_status_t status)
{
    const pulse_generator_status_t stop_status = pg->ops->channel_stop(pg->hw);
    end_movement(pg);

    if (event == PULSE_GENERATOR_EVENT_UNDERRUN) {
        pg->sched.underrun_count++;
    }

    emit_event(pg, event);

    if (stop_status != PULSE_GENERATOR_OK &&
        (status == PULSE_GENERATOR_OK || status == PULSE_GENERATOR_ERROR_UNDERRUN)) {
        return stop_status;
    }

    return status;
}

/* The fall being armed is already behind the counter, with the pin high:
   stopping now would strand it there. Move it to the nearest instant the
   ISR can still honour and make it the movement's last edge. The pulse
   comes out too long, but the pin ends low and the caller is told. */
static pulse_generator_status_t reschedule_late_fall(pulse_generator_t *pg)
{
    const uint32_t margin = (pg->sched.min_interval_ticks != 0) ? pg->sched.min_interval_ticks : 1u;
    const uint32_t fall = (pg->ops->get_counter(pg->hw) + margin) & pg->counter_max;

    pg->sched.stop_after_fall = true;
    pg->sched.stop_event = PULSE_GENERATOR_EVENT_UNDERRUN;

    pulse_generator_status_t status = pg->ops->set_compare(pg->hw, fall);
    pg->last_compare = fall;
    if (status != PULSE_GENERATOR_OK) {
        return stop_movement(pg, PULSE_GENERATOR_EVENT_UNDERRUN, status);
    }

    return PULSE_GENERATOR_OK;
}

/* Arms the next SCHEDULED edge at an absolute instant, then checks it is
   still ahead of the counter: a compare left behind would not fire until the
   counter wrapped all the way round. Here the instant is the contract, so a
   late edge is never quietly moved and the train carried on, as
   schedule_next_compare() does; the movement ends instead. fall_armed says
   which edge this is, and so where the pin stands. */
static pulse_generator_status_t arm_edge(pulse_generator_t *pg, uint32_t previous, uint32_t target)
{
    pulse_generator_status_t status = pg->ops->set_compare(pg->hw, target);
    pg->last_compare = target;
    if (status != PULSE_GENERATOR_OK) {
        /* Whichever edge it was, there is nothing left to arm it with. If
           it was a fall, the pin may be left high. */
        return stop_movement(pg, PULSE_GENERATOR_EVENT_UNDERRUN, status);
    }

    /* Same test as schedule_next_compare(): ticks elapsed since the match
       being serviced, against ticks from it to the target. */
    const uint32_t mask = pg->counter_max;
    const uint32_t delta = (target - previous) & mask;
    const uint32_t elapsed = (pg->ops->get_counter(pg->hw) - previous) & mask;
    if (elapsed < delta) {
        return PULSE_GENERATOR_OK;
    }

    if (pg->sched.fall_armed) {
        return reschedule_late_fall(pg);
    }

    /* A late rise, with the pin low: a clean pulse boundary to stop at. */
    return stop_movement(pg, PULSE_GENERATOR_EVENT_UNDERRUN, PULSE_GENERATOR_ERROR_UNDERRUN);
}

/* Edge-triggered, so a draining queue is reported once rather than on every
   pulse. Silent once finish is requested: the producer has nothing more to
   add. */
static void signal_low_watermark_if_needed(pulse_generator_t *pg)
{
    if (pg->sched.low_watermark == 0 || pg->sched.finish_requested) {
        return;
    }

    const size_t pending = pending_events(pg);
    if (pg->sched.low_watermark_armed && pending < pg->sched.low_watermark) {
        pg->sched.low_watermark_armed = false;
        emit_event(pg, PULSE_GENERATOR_EVENT_LOW_WATERMARK);
    } else if (!pg->sched.low_watermark_armed && pending >= pg->sched.low_watermark) {
        pg->sched.low_watermark_armed = true;
    }
}

/* One SCHEDULED edge has just fired. Pulse k rises on its own event and
   falls halfway to the rise of pulse k+1, so its rise is where the next
   event is taken off the queue: the fall needs it. */
static pulse_generator_status_t on_scheduled_edge(pulse_generator_t *pg)
{
    const uint32_t mask = pg->counter_max;
    const uint32_t previous = pg->last_compare; /* the match being serviced */
    pg->edge_count++;

    if (!pg->sched.fall_armed) {
        /* The rise of pulse k: place its fall. */
        uint32_t half;
        if (pop_event(pg, &pg->sched.interval_next)) {
            half = pg->sched.interval_next / 2;
        } else {
            /* No successor: the last pulse falls halfway through its own
               interval, and its fall ends the movement. */
            half = pg->sched.interval_current / 2;
            pg->sched.stop_after_fall = true;
            pg->sched.stop_event = pg->sched.finish_requested ? PULSE_GENERATOR_EVENT_COMPLETE
                                                              : PULSE_GENERATOR_EVENT_UNDERRUN;
        }

        pg->sched.fall_armed = true;
        return arm_edge(pg, previous, (pg->sched.rise_tick + half) & mask);
    }

    /* The fall of pulse k: the pulse is complete. */
    if (pg->sched.stop_after_fall) {
        const pulse_generator_event_t event = pg->sched.stop_event;
        return stop_movement(pg, event,
                             event == PULSE_GENERATOR_EVENT_UNDERRUN ? PULSE_GENERATOR_ERROR_UNDERRUN
                                                                     : PULSE_GENERATOR_OK);
    }

    /* Place the rise of k+1. Armed before the watermark is checked, so a
       callback that refills the queue in place does not delay it; the rise
       already has its interval, popped at the previous rise. */
    pg->sched.rise_tick = (pg->sched.rise_tick + pg->sched.interval_next) & mask;
    pg->sched.interval_current = pg->sched.interval_next;
    pg->sched.fall_armed = false;

    pulse_generator_status_t status = arm_edge(pg, previous, pg->sched.rise_tick);
    if (status == PULSE_GENERATOR_OK) {
        signal_low_watermark_if_needed(pg);
    }

    return status;
}

pulse_generator_status_t pulse_generator_prepare_scheduled(
    pulse_generator_t *pg,
    const pulse_generator_scheduled_config_t *config)
{
    if (pg == NULL || config == NULL || config->queue == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    /* One slot always stays empty to tell a full queue from an empty one,
       so a capacity below 2 could never hold an event. */
    if (config->queue_capacity < 2 || config->low_watermark >= config->queue_capacity) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    if (pg->state != PULSE_GENERATOR_STATE_IDLE) {
        return PULSE_GENERATOR_ERROR_INVALID_STATE;
    }

    switch (config->engine) {
    case PULSE_GENERATOR_ENGINE_ISR:
        break;
    case PULSE_GENERATOR_ENGINE_DMA:
        return PULSE_GENERATOR_ERROR_NOT_SUPPORTED;
    default:
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    /* Captured here rather than at start, so that queue_events() can check
       every interval against counter_max as it comes in. */
    pulse_generator_status_t status = capture_timing(pg);
    if (status != PULSE_GENERATOR_OK) {
        return status;
    }

    /* Everything not set here starts at zero: an empty queue (head ==
       tail), no finish request, no underruns, and the ISR's own fields,
       which start_scheduled() sets before it arms anything. */
    pg->sched = (pulse_generator_sched_t){
        .queue              = config->queue,
        .queue_capacity     = config->queue_capacity,
        .low_watermark      = config->low_watermark,
        .min_interval_ticks = config->min_interval_ticks,
        .engine             = config->engine,
    };

    pg->edge_count = 0;
    pg->mode = PULSE_GENERATOR_MODE_SCHEDULED;
    pg->state = PULSE_GENERATOR_STATE_ARMED;

    return PULSE_GENERATOR_OK;
}

size_t pulse_generator_queue_events(pulse_generator_t *pg, const uint32_t *intervals, size_t n)
{
    if (pg == NULL || intervals == NULL || !is_scheduled(pg)) {
        return 0;
    }

    /* Only this function writes head, so a local copy stays current; tail is
       re-read on every event, since the ISR may free slots meanwhile. */
    size_t head = pg->sched.queue_head;
    size_t accepted = 0;

    while (accepted < n) {
        uint32_t interval = intervals[accepted];
        if (!interval_is_valid(pg, interval)) {
            break;
        }

        size_t next_head = next_index(pg, head);
        if (next_head == pg->sched.queue_tail) {
            break; /* full */
        }

        /* Slot first, index second: once the ISR sees the new head, the
           event behind it must already be in place. */
        pg->sched.queue[head] = interval;
        pg->sched.queue_head = next_head;

        head = next_head;
        accepted++;
    }

    return accepted;
}

pulse_generator_status_t pulse_generator_start_scheduled(pulse_generator_t *pg, uint32_t start_tick)
{
    if (pg == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    if (pg->mode != PULSE_GENERATOR_MODE_SCHEDULED || pg->state != PULSE_GENERATOR_STATE_ARMED) {
        return PULSE_GENERATOR_ERROR_INVALID_STATE;
    }

    /* A pulse's fall is placed with the next pulse's interval, so a movement
       needs one event of look-ahead, unless finish already says the first
       event is also the last. */
    const size_t pending = pending_events(pg);
    if (pending == 0 || (pending == 1 && !pg->sched.finish_requested)) {
        return PULSE_GENERATOR_ERROR_INVALID_STATE;
    }

    /* Peeked rather than popped, so a start rejected here leaves the queue
       exactly as it was. */
    const size_t tail = pg->sched.queue_tail;
    const uint32_t first_interval = pg->sched.queue[tail];
    const uint32_t mask = pg->counter_max;
    const uint32_t rise = (start_tick + first_interval) & mask;

    /* Modulo the counter, a rise more than half the range ahead cannot be
       told from one already behind, so it is taken as gone. */
    const uint32_t ahead = (rise - pg->ops->get_counter(pg->hw)) & mask;
    if (ahead == 0 || ahead > mask / 2u) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    pg->sched.interval_current = first_interval;
    pg->sched.rise_tick = rise;
    pg->sched.fall_armed = false;
    pg->sched.stop_after_fall = false;
    pg->sched.low_watermark_armed = true;
    pg->edge_count = 0;
    pg->last_compare = rise;

    /* Popped and published before arming: with a short first interval the
       rise can fire inside channel_start(), and its ISR must find the
       instance RUNNING and the queue already past this event. While ARMED
       nothing else consumes, so moving tail here is safe, and so is moving
       it back. */
    pg->sched.queue_tail = next_index(pg, tail);
    pg->state = PULSE_GENERATOR_STATE_RUNNING;

    pulse_generator_status_t status = pg->ops->channel_start(pg->hw, rise);
    if (status != PULSE_GENERATOR_OK) {
        pg->state = PULSE_GENERATOR_STATE_ARMED;
        pg->sched.queue_tail = tail;
    }

    return status;
}

pulse_generator_status_t pulse_generator_finish_scheduled(pulse_generator_t *pg)
{
    if (pg == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    if (!is_scheduled(pg)) {
        return PULSE_GENERATOR_ERROR_INVALID_STATE;
    }

    pg->sched.finish_requested = true;

    return PULSE_GENERATOR_OK;
}

size_t pulse_generator_get_free_space(const pulse_generator_t *pg)
{
    if (pg == NULL || !is_scheduled(pg)) {
        return 0;
    }

    return pg->sched.queue_capacity - 1 - pending_events(pg);
}

size_t pulse_generator_get_pending_events(const pulse_generator_t *pg)
{
    if (pg == NULL || !is_scheduled(pg)) {
        return 0;
    }

    return pending_events(pg);
}

uint32_t pulse_generator_get_now_ticks(const pulse_generator_t *pg)
{
    if (pg == NULL) {
        return 0;
    }

    return pg->ops->get_counter(pg->hw);
}

uint32_t pulse_generator_get_underrun_count(const pulse_generator_t *pg)
{
    if (pg == NULL) {
        return 0;
    }

    return pg->sched.underrun_count;
}

/* --- Entry points shared by every mode --- */

pulse_generator_status_t pulse_generator_stop(pulse_generator_t *pg)
{
    if (pg == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    if (pg->state == PULSE_GENERATOR_STATE_IDLE) {
        return PULSE_GENERATOR_OK;
    }

    if (pg->mode == PULSE_GENERATOR_MODE_SCHEDULED) {
        /* Back to ARMED rather than IDLE, so a restart is one more
           start_scheduled(). An instance that was only ARMED has no channel
           running, but still gets its queue flushed. No event: a stop is
           neither a completion nor an underrun. */
        pulse_generator_status_t status = PULSE_GENERATOR_OK;
        if (pg->state == PULSE_GENERATOR_STATE_RUNNING) {
            status = pg->ops->channel_stop(pg->hw);
        }

        end_movement(pg);
        return status;
    }

    pulse_generator_status_t status = pg->ops->channel_stop(pg->hw);
    pg->state = PULSE_GENERATOR_STATE_IDLE;
    pg->edge_count = 0;

    return status;
}

pulse_generator_status_t pulse_generator_notify_compare_match(pulse_generator_t *pg)
{
    if (pg == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    if (pg->state != PULSE_GENERATOR_STATE_RUNNING) {
        return PULSE_GENERATOR_OK;
    }

    if (pg->mode == PULSE_GENERATOR_MODE_SCHEDULED) {
        return on_scheduled_edge(pg);
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
