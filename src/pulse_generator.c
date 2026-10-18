/* Each output is a timer in PWM mode whose period (ARR) and compare (CCR)
   preload registers are loaded at every update event by a circular DMA
   stream. The buffer holds `entries` entries, one timer cycle each, and is
   refilled half at a time from the stream's half and full transfer
   interrupts. The three modes differ only in the source of periods that
   fills it; a period longer than window_us / entries is split over several
   entries, so that what a refill writes reaches the pin within the window. */

#include "pulse_generator.h"

#define US_PER_S 1000000u
#define NS_PER_S 1000000000u

/* --- Validation --- */

/* Checked once in init(), so that every hook can be called without a NULL
   check. */
static bool ops_are_complete(const pulse_generator_ops_t *ops)
{
    return ops->get_tick_hz != NULL &&
           ops->stream_start != NULL &&
           ops->stream_stop != NULL &&
           ops->get_stream_remaining != NULL &&
           ops->get_period_max != NULL &&
           ops->get_entry_layout != NULL;
}

static bool pulse_shape_is_valid(pulse_generator_pulse_shape_t shape, uint32_t width_ns)
{
    switch (shape) {
    case PULSE_GENERATOR_PULSE_SHAPE_HALF_PERIOD:
        return true;
    case PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH:
        return width_ns != 0;
    default:
        return false;
    }
}

/* Only what can be checked without the platform: whether the buffer is large
   enough depends on the entry layout, read when a movement starts. */
static bool dma_config_is_valid(const pulse_generator_dma_config_t *dma)
{
    return dma->buffer != NULL && dma->entries >= 4 && dma->entries % 2 == 0 && dma->window_us != 0;
}

/* Each value needs a word of its own inside the entry. */
static bool layout_is_valid(const pulse_generator_entry_layout_t *layout)
{
    return layout->words_per_entry > 0 &&
           layout->period_index < layout->words_per_entry &&
           layout->compare_index < layout->words_per_entry &&
           layout->period_index != layout->compare_index;
}

/* --- Timing --- */

/* Rounded up, so that a width or a minimum entry never comes out shorter than
   asked. Saturates rather than wrapping, so that the checks that follow
   reject an absurd value instead of taking it as a small one. */
static uint32_t ns_to_ticks_rounded_up(uint32_t ns, uint32_t tick_hz)
{
    const uint64_t ticks = ((uint64_t)ns * tick_hz + (NS_PER_S - 1u)) / NS_PER_S;

    return (ticks > UINT32_MAX) ? UINT32_MAX : (uint32_t)ticks;
}

/* Reads the platform once per movement and converts the config's times to
   ticks, rejecting what the hardware cannot play. None of it may change
   while the movement runs, so neither the refill nor set_frequency() calls a
   hook to read it again. */
static pulse_generator_status_t capture_timing(pulse_generator_t *pg)
{
    pulse_generator_dma_t *dma = &pg->dma;

    pulse_generator_entry_layout_t layout;
    pulse_generator_status_t status = pg->ops->get_entry_layout(pg->hw, &layout);
    if (status != PULSE_GENERATOR_OK) {
        return status;
    }

    /* Divided rather than multiplied, so that no entry count can overflow. */
    if (!layout_is_valid(&layout) || dma->entries > dma->buffer_words / layout.words_per_entry) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    const uint32_t tick_hz = pg->ops->get_tick_hz(pg->hw);
    const uint32_t period_max = pg->ops->get_period_max(pg->hw);

    /* Longest entry: the window's share, capped so that a compare of
       length + 1 (see compare_is_fall()) still fits the registers. */
    uint64_t segment_max = ((uint64_t)dma->window_us * tick_hz / US_PER_S) / dma->entries;
    const uint64_t register_limit = (period_max > 0u) ? (uint64_t)period_max - 1u : 0u;
    if (segment_max > register_limit) {
        segment_max = register_limit;
    }
    if (segment_max > UINT32_MAX) {
        segment_max = UINT32_MAX;
    }

    /* Two ticks at least: one high, one low. */
    uint32_t min_entry = ns_to_ticks_rounded_up(dma->min_entry_ns, tick_hz);
    if (min_entry < 2u) {
        min_entry = 2u;
    }

    /* Splitting a period can leave pieces of just over half an entry, which
       must still be playable. */
    if (segment_max < 2u * (uint64_t)min_entry) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    pg->tick_hz = tick_hz;
    pg->pulse.width_ticks = (pg->pulse.shape == PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH)
                                ? ns_to_ticks_rounded_up(pg->pulse.width_ns, tick_hz)
                                : 0;
    dma->layout = layout;
    dma->segment_max_ticks = (uint32_t)segment_max;
    dma->min_entry_ticks = min_entry;

    return PULSE_GENERATOR_OK;
}

/* No shorter than the minimum entry and, with a fixed width, long enough for
   the pulse to fall before the next one rises. */
static bool period_is_playable(const pulse_generator_t *pg, uint32_t ticks)
{
    if (ticks < pg->dma.min_entry_ticks) {
        return false;
    }

    return pg->pulse.shape != PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH || ticks > pg->pulse.width_ticks;
}

/* Checked on the whole part of the period: the shortest one it emits. */
static pulse_generator_status_t check_frequency(const pulse_generator_t *pg, uint32_t frequency_hz)
{
    if (frequency_hz == 0 || !period_is_playable(pg, pg->tick_hz / frequency_hz)) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    return PULSE_GENERATOR_OK;
}

/* From a frequency already checked: the whole ticks, plus a remainder that
   next_fixed_rate_period() carries over, starting with no carry. */
static void set_period(pulse_generator_t *pg, uint32_t frequency_hz)
{
    pg->dma.period_ticks = pg->tick_hz / frequency_hz;
    pg->dma.period_remainder = pg->tick_hz % frequency_hz;
    pg->dma.period_divisor = frequency_hz;
    pg->dma.period_carry = 0;
}

static uint32_t pulse_high_ticks(const pulse_generator_t *pg, uint32_t period_ticks)
{
    return (pg->pulse.shape == PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH) ? pg->pulse.width_ticks
                                                                         : period_ticks / 2u;
}

/* --- Scheduled queue --- */

/* Each index is read once, so that the result is never torn while the other
   side moves its own: at worst one event out of date. The capacity need not
   be a power of two, hence the explicit wrap rather than a mask. */
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

/* Consumer side: the slot is read before tail is published, so that the
   producer cannot overwrite it mid-read. */
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

/* --- Period sources --- */

/* SCHEDULED. The period starting at the rise of pulse k runs until the rise
   of k+1, so its length is the next event's interval; the first event's
   interval is a lead-in period with no pulse. With no next event, pulse k is
   the last: it reuses its own interval, and the movement ends COMPLETE or
   UNDERRUN depending on whether finish had been requested by then. */
static bool next_scheduled_period(pulse_generator_t *pg, uint32_t *length, uint32_t *high)
{
    pulse_generator_sched_t *sched = &pg->sched;

    if (sched->source_dry) {
        return false;
    }

    if (sched->lead_in) {
        sched->lead_in = false;
        *length = sched->interval_current;
        *high = 0;
        return true;
    }

    uint32_t interval;
    if (pop_event(pg, &interval)) {
        sched->interval_current = interval;
    } else {
        sched->source_dry = true;
        sched->stop_event = sched->finish_requested ? PULSE_GENERATOR_EVENT_COMPLETE
                                                    : PULSE_GENERATOR_EVENT_UNDERRUN;
    }

    *length = sched->interval_current;
    *high = pulse_high_ticks(pg, sched->interval_current);

    return true;
}

/* FIXED_COUNT and CONTINUOUS: the same period, one tick longer whenever the
   carried remainder adds up to a whole tick, so that the periods average
   tick_hz / frequency_hz exactly. FIXED_COUNT runs dry at its target.
   CONTINUOUS never does, and takes up the frequency set_frequency() left
   only here, where a period starts, so that no period is part old and part
   new. */
static bool next_fixed_rate_period(pulse_generator_t *pg, uint32_t *length, uint32_t *high)
{
    pulse_generator_dma_t *dma = &pg->dma;

    if (pg->mode == PULSE_GENERATOR_MODE_FIXED_COUNT) {
        if (dma->pulses_written >= dma->pulse_target) {
            return false;
        }
    } else {
        /* Read once: set_frequency() may write it again at any moment. */
        const uint32_t requested = dma->requested_frequency_hz;
        if (requested != dma->period_divisor) {
            set_period(pg, requested);
        }
    }

    uint32_t period = dma->period_ticks;
    /* Both terms are below the divisor, so the sum cannot overflow. */
    if (dma->period_remainder >= dma->period_divisor - dma->period_carry) {
        dma->period_carry -= dma->period_divisor - dma->period_remainder;
        period++;
    } else {
        dma->period_carry += dma->period_remainder;
    }

    dma->pulses_written++;
    *length = period;
    *high = pulse_high_ticks(pg, period);

    return true;
}

/* The next period from the mode's source, and its high time. Returns false
   once the source has run dry. */
static bool next_period(pulse_generator_t *pg, uint32_t *length, uint32_t *high)
{
    if (pg->mode == PULSE_GENERATOR_MODE_SCHEDULED) {
        return next_scheduled_period(pg, length, high);
    }

    return next_fixed_rate_period(pg, length, high);
}

/* --- Buffer --- */

/* Whether the pulse falls within an entry of `length` ticks. Any compare from
   the length (ARR + 1) up keeps the pin high throughout, so next_segment()
   gives a fall right at the end the length itself and an entry the pulse
   stays high through length + 1, which tells them apart when pulses are
   counted from the buffer. */
static bool compare_is_fall(uint32_t length, uint32_t compare)
{
    return compare != 0u && compare <= length;
}

/* Cuts each period into as few entries of at most segment_max_ticks as
   possible, differing by one tick at most, the longer ones first. The high
   time is laid from the period's start: the entry it ends in gets the
   remainder as its compare, those before it length + 1, those after it 0
   (see compare_is_fall()). Returns false once the source has run dry. */
static bool next_segment(pulse_generator_t *pg, uint32_t *length, uint32_t *compare)
{
    pulse_generator_dma_t *dma = &pg->dma;

    if (dma->split_index == dma->split_count) {
        uint32_t period;
        uint32_t high;
        if (!next_period(pg, &period, &high)) {
            return false;
        }

        const uint32_t segment_max = dma->segment_max_ticks;
        dma->split_period = period;
        dma->split_high = high;
        dma->split_start = 0;
        dma->split_count = period / segment_max + (period % segment_max != 0u);
        dma->split_index = 0;
    }

    const uint32_t count = dma->split_count;
    const uint32_t piece = dma->split_period / count + (dma->split_index < dma->split_period % count);
    const uint32_t start = dma->split_start;

    uint32_t high_left = (dma->split_high > start) ? dma->split_high - start : 0u;
    *length = piece;
    *compare = (high_left > piece) ? piece + 1u : high_left;

    dma->split_start = start + piece;
    dma->split_index++;

    return true;
}

/* Every word of the entry written exactly once: the period register gets the
   length minus one, the compare register the compare, the rest 0. */
static void write_entry(pulse_generator_t *pg, size_t index, uint32_t length, uint32_t compare)
{
    const pulse_generator_entry_layout_t *layout = &pg->dma.layout;
    volatile uint32_t *entry = pg->dma.buffer + index * layout->words_per_entry;

    for (size_t word = 0; word < layout->words_per_entry; word++) {
        uint32_t value = 0;
        if (word == layout->period_index) {
            value = length - 1u;
        } else if (word == layout->compare_index) {
            value = compare;
        }
        entry[word] = value;
    }
}

/* compare_is_fall(), read back from the buffer. */
static bool entry_has_fall(const pulse_generator_t *pg, size_t index)
{
    const pulse_generator_entry_layout_t *layout = &pg->dma.layout;
    const volatile uint32_t *entry = pg->dma.buffer + index * layout->words_per_entry;
    const uint32_t length = entry[layout->period_index] + 1u;
    const uint32_t compare = entry[layout->compare_index];

    return compare_is_fall(length, compare);
}

/* Once the source has run dry, writes empty entries as long as the window
   allows: the pin stays low and the end takes as few refills as possible.
   Returns whether any entry came from the source rather than this padding. */
static bool fill_entries(pulse_generator_t *pg, size_t first, size_t count)
{
    bool from_source = false;

    for (size_t index = first; index < first + count; index++) {
        uint32_t length;
        uint32_t compare;

        if (next_segment(pg, &length, &compare)) {
            write_entry(pg, index, length, compare);
            from_source = true;
            if (compare_is_fall(length, compare)) {
                pg->dma.falls_written++;
            }
        } else {
            write_entry(pg, index, pg->dma.segment_max_ticks, 0);
        }
    }

    return from_source;
}

/* Fills one half of the buffer and records what pulse counting needs:
   whether it holds anything but padding, the falls written before it, and
   those in the last two entries it overwrites, which can still be playing
   (the stream fetches an entry one period ahead). */
static void fill_half(pulse_generator_t *pg, size_t half_index)
{
    pulse_generator_dma_t *dma = &pg->dma;
    const size_t half = dma->entries / 2u;
    const size_t first = half_index * half;
    const size_t last = first + half - 1u;

    dma->overwritten_tail = (uint8_t)((entry_has_fall(pg, last) ? 2u : 0u) |
                                      (entry_has_fall(pg, last - 1u) ? 1u : 0u));
    dma->falls_before_half[half_index] = dma->falls_written;
    dma->half_has_pulses[half_index] = fill_entries(pg, first, half);
    dma->newer_half = (uint8_t)half_index;
    dma->fill_count++;
}

/* Nothing left to write: every pulse is in the buffer, the last period's
   final entry included. Never the case in CONTINUOUS. */
static bool source_is_dry(const pulse_generator_t *pg)
{
    const bool period_written = pg->dma.split_index == pg->dma.split_count;

    if (pg->mode == PULSE_GENERATOR_MODE_FIXED_COUNT) {
        return pg->dma.pulses_written >= pg->dma.pulse_target && period_written;
    }
    if (pg->mode == PULSE_GENERATOR_MODE_SCHEDULED) {
        return pg->sched.source_dry && period_written;
    }

    return false;
}

/* --- Stream position --- */

/* The entry the stream fetches next, within the lap. An entry caught halfway
   through its burst counts as not fetched yet, which errs on the side of
   calling a refill late. */
static size_t next_fetch_entry(const pulse_generator_t *pg)
{
    const size_t words_per_entry = pg->dma.layout.words_per_entry;
    const size_t remaining = pg->ops->get_stream_remaining(pg->hw);
    const size_t entries_left = (remaining + words_per_entry - 1u) / words_per_entry;

    return (entries_left >= pg->dma.entries) ? 0 : pg->dma.entries - entries_left;
}

/* Pulses whose fall lies in an entry that has finished playing, since the
   movement started. Each entry is fetched at the update event that starts
   the one before it, so the two just behind the next fetch, the one playing
   and the one in preload, have not finished yet. Read again if a refill runs
   meanwhile. */
static uint32_t falls_finished(const pulse_generator_t *pg)
{
    const pulse_generator_dma_t *dma = &pg->dma;
    const size_t entries = dma->entries;
    uint32_t fill_count;
    uint32_t falls;

    do {
        fill_count = dma->fill_count;

        const size_t oldest = (1u - dma->newer_half) * (entries / 2u);
        const size_t next = next_fetch_entry(pg);
        const size_t fetched = (next + entries - oldest) % entries; /* since the oldest entry */

        falls = dma->falls_before_half[1u - dma->newer_half];
        if (fetched >= 2u) {
            for (size_t i = 0; i < fetched - 2u; i++) {
                falls += entry_has_fall(pg, (oldest + i) % entries) ? 1u : 0u;
            }
        } else {
            /* The unfinished entries reach back into the ones the latest
               fill overwrote. */
            const uint8_t tail = dma->overwritten_tail;
            falls -= (tail >> 1) & 1u;
            if (fetched == 0u) {
                falls -= tail & 1u;
            }
        }
    } while (fill_count != dma->fill_count);

    return falls;
}

/* --- Movements --- */

/* Reads the callback into locals first: the instance is already in its
   post-event state, and a callback that reconfigures it cannot pull the
   fields out from under this call. */
static void emit_event(pulse_generator_t *pg, pulse_generator_event_t event)
{
    pulse_generator_event_cb_t callback = pg->on_event;
    void *user_ctx = pg->user_ctx;

    if (callback != NULL) {
        callback(pg, event, user_ctx);
    }
}

/* Fills the whole buffer from the source, already set up by the caller, and
   starts the stream. On failure the caller rolls the instance back. */
static pulse_generator_status_t fill_and_start_stream(pulse_generator_t *pg)
{
    pg->dma.split_count = 0;
    pg->dma.split_index = 0;
    pg->dma.falls_written = 0;
    pg->dma.count_base = 0;
    fill_half(pg, 0);
    fill_half(pg, 1);
    /* What the first fills overwrote was never played. */
    pg->dma.overwritten_tail = 0;

    /* Published before the stream starts: its first half-transfer can come
       before stream_start() returns, and must find the instance RUNNING. */
    pg->state = PULSE_GENERATOR_STATE_RUNNING;

    return pg->ops->stream_start(pg->hw, pg->dma.buffer,
                                 pg->dma.entries * pg->dma.layout.words_per_entry);
}

static pulse_generator_status_t start_movement(
    pulse_generator_t *pg,
    pulse_generator_mode_t mode,
    uint32_t frequency_hz,
    uint32_t pulse_count)
{
    pulse_generator_status_t status = capture_timing(pg);
    if (status != PULSE_GENERATOR_OK) {
        return status;
    }

    status = check_frequency(pg, frequency_hz);
    if (status != PULSE_GENERATOR_OK) {
        return status;
    }

    /* Set before the first fills, which read both. */
    set_period(pg, frequency_hz);
    pg->mode = mode;
    pg->dma.requested_frequency_hz = frequency_hz;

    /* The pulse count fields are FIXED_COUNT's; CONTINUOUS passes 0 and never
       reads them. */
    pg->dma.pulses_written = 0;
    pg->dma.pulses_requested = pulse_count;
    pg->dma.pulse_target = pulse_count;

    status = fill_and_start_stream(pg);
    if (status != PULSE_GENERATOR_OK) {
        pg->state = PULSE_GENERATOR_STATE_IDLE;
    }

    return status;
}

/* The queue exists only between prepare_scheduled() and the next init(); in
   any other mode its fields are zero or stale and must not be read. */
static bool is_scheduled(const pulse_generator_t *pg)
{
    return pg->mode == PULSE_GENERATOR_MODE_SCHEDULED &&
           pg->state != PULSE_GENERATOR_STATE_IDLE;
}

/* Every way out of a SCHEDULED movement, back to ARMED. The events left and
   the finish request belonged to the train that ended, so both are dropped.
   Flushing moves tail up to head and leaves head alone: it stays the
   producer's. */
static void end_scheduled_movement(pulse_generator_t *pg)
{
    pg->state = PULSE_GENERATOR_STATE_ARMED;
    pg->sched.queue_tail = pg->sched.queue_head;
    pg->sched.finish_requested = false;
}

/* Stops the stream if it is running, with the pin forced low and the rest of
   the buffer never played, and leaves the movement: SCHEDULED back to ARMED,
   so that a restart is one more start_scheduled(), the other modes to IDLE.
   Returns the platform's status. */
static pulse_generator_status_t stop_output(pulse_generator_t *pg)
{
    pulse_generator_status_t status = PULSE_GENERATOR_OK;
    if (pg->state == PULSE_GENERATOR_STATE_RUNNING) {
        status = pg->ops->stream_stop(pg->hw);
    }

    if (pg->mode == PULSE_GENERATOR_MODE_SCHEDULED) {
        end_scheduled_movement(pg);
    } else {
        pg->state = PULSE_GENERATOR_STATE_IDLE;
    }

    return status;
}

/* Ends a running movement with `event`, COMPLETE or UNDERRUN, counting an
   underrun in SCHEDULED mode. Returns the matching status, unless stopping
   the stream failed: then the platform's error says more. */
static pulse_generator_status_t stop_movement(pulse_generator_t *pg, pulse_generator_event_t event)
{
    const pulse_generator_status_t stop_status = stop_output(pg);

    if (pg->mode == PULSE_GENERATOR_MODE_SCHEDULED && event == PULSE_GENERATOR_EVENT_UNDERRUN) {
        pg->sched.underrun_count++;
    }

    emit_event(pg, event);

    if (stop_status != PULSE_GENERATOR_OK) {
        return stop_status;
    }

    return (event == PULSE_GENERATOR_EVENT_UNDERRUN) ? PULSE_GENERATOR_ERROR_UNDERRUN : PULSE_GENERATOR_OK;
}

/* Edge-triggered, so that a draining queue is reported once rather than at
   every refill. Silent once finish is requested: the producer has nothing
   more to add. */
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

/* One half of the buffer has been fetched and can be refilled, as long as
   the stream is still in the other one. */
static pulse_generator_status_t on_half_fetched(pulse_generator_t *pg, size_t half_index)
{
    if (pg->state != PULSE_GENERATOR_STATE_RUNNING) {
        return PULSE_GENERATOR_OK;
    }

    /* Done once the source is dry and both halves are padding: every entry
       from the source was fetched before the half now playing, so all of
       them have finished and the pin is low. Both halves, because a
       reset_pulse_count() can put pulses back into the other one after this
       one was padded. Checked before lateness: replayed padding is
       harmless. */
    if (!pg->dma.half_has_pulses[0] && !pg->dma.half_has_pulses[1] && source_is_dry(pg)) {
        /* A SCHEDULED source knows how it ran dry; the others only complete. */
        const pulse_generator_event_t event = (pg->mode == PULSE_GENERATOR_MODE_SCHEDULED)
                                                  ? pg->sched.stop_event
                                                  : PULSE_GENERATOR_EVENT_COMPLETE;
        return stop_movement(pg, event);
    }

    const size_t half = pg->dma.entries / 2u;
    const size_t first = half_index * half;
    const size_t next = next_fetch_entry(pg);

    /* The stream is already reading the half to refill: writing it now would
       tear entries or replay old ones, and extra pulses are the worst failure
       a step output can have. Stop instead, with the pin low. */
    if (next >= first && next < first + half) {
        return stop_movement(pg, PULSE_GENERATOR_EVENT_UNDERRUN);
    }

    fill_half(pg, half_index);

    /* After the refill has taken its events. Not once the source is dry: the
       producer can no longer change how this movement ends. */
    if (pg->mode == PULSE_GENERATOR_MODE_SCHEDULED && !pg->sched.source_dry) {
        signal_low_watermark_if_needed(pg);
    }

    return PULSE_GENERATOR_OK;
}

/* --- Lifecycle --- */

pulse_generator_status_t pulse_generator_init(
    pulse_generator_t *pg,
    const pulse_generator_config_t *config)
{
    if (pg == NULL || config == NULL || config->ops == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    if (!ops_are_complete(config->ops) ||
        !pulse_shape_is_valid(config->pulse.shape, config->pulse.width_ns) ||
        !dma_config_is_valid(&config->dma)) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    *pg = (pulse_generator_t){
        .ops      = config->ops,
        .hw       = config->hw,
        .on_event = config->on_event,
        .user_ctx = config->user_ctx,
        .pulse = {
            .shape    = config->pulse.shape,
            .width_ns = config->pulse.width_ns,
        },
        .dma = {
            .buffer       = config->dma.buffer,
            .buffer_words = config->dma.buffer_words,
            .entries      = config->dma.entries,
            .window_us    = config->dma.window_us,
            .min_entry_ns = config->dma.min_entry_ns,
        },
    };

    return PULSE_GENERATOR_OK;
}

/* Only stored: converted to ticks and checked against the periods when the
   next movement starts, like the config's. */
pulse_generator_status_t pulse_generator_set_pulse_shape(
    pulse_generator_t *pg,
    pulse_generator_pulse_shape_t shape,
    uint32_t width_ns)
{
    if (pg == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    if (pg->state != PULSE_GENERATOR_STATE_IDLE) {
        return PULSE_GENERATOR_ERROR_INVALID_STATE;
    }

    if (!pulse_shape_is_valid(shape, width_ns)) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    pg->pulse.shape = shape;
    pg->pulse.width_ns = width_ns;

    return PULSE_GENERATOR_OK;
}

/* --- Fixed count mode --- */

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

    /* Half the range at most, so that reset_pulse_count() can push the
       target back by a whole pulse_count without overflowing. */
    if (pulse_count == 0 || pulse_count > UINT32_MAX / 2u) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    return start_movement(pg, PULSE_GENERATOR_MODE_FIXED_COUNT, frequency_hz, pulse_count);
}

/* --- Continuous mode --- */

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

    return start_movement(pg, PULSE_GENERATOR_MODE_CONTINUOUS, frequency_hz, 0);
}

pulse_generator_status_t pulse_generator_set_frequency(pulse_generator_t *pg, uint32_t frequency_hz)
{
    if (pg == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    if (pg->state != PULSE_GENERATOR_STATE_RUNNING || pg->mode != PULSE_GENERATOR_MODE_CONTINUOUS) {
        return PULSE_GENERATOR_ERROR_INVALID_STATE;
    }

    pulse_generator_status_t status = check_frequency(pg, frequency_hz);
    if (status == PULSE_GENERATOR_OK) {
        /* Only stored: a single word the refill reads once, so that no lock
           and no hardware access are needed. */
        pg->dma.requested_frequency_hz = frequency_hz;
    }

    return status;
}

/* --- Scheduled mode --- */

pulse_generator_status_t pulse_generator_prepare_scheduled(
    pulse_generator_t *pg,
    const pulse_generator_scheduled_config_t *config)
{
    if (pg == NULL || config == NULL || config->queue == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    /* One slot always stays empty to tell a full queue from an empty one, so
       a capacity below 2 could never hold an event. */
    if (config->queue_capacity < 2 || config->low_watermark >= config->queue_capacity) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    if (pg->state != PULSE_GENERATOR_STATE_IDLE) {
        return PULSE_GENERATOR_ERROR_INVALID_STATE;
    }

    /* Captured here rather than at start, so that queue_events() can check
       every interval as it comes in. */
    pulse_generator_status_t status = capture_timing(pg);
    if (status != PULSE_GENERATOR_OK) {
        return status;
    }

    /* Everything not set here starts at zero: an empty queue (head == tail),
       no finish request, no underruns, and the source's fields, which
       start_scheduled() sets. */
    pg->sched = (pulse_generator_sched_t){
        .queue          = config->queue,
        .queue_capacity = config->queue_capacity,
        .low_watermark  = config->low_watermark,
    };

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
       re-read for every event, since the refill may free slots meanwhile. */
    size_t head = pg->sched.queue_head;
    size_t accepted = 0;

    while (accepted < n) {
        uint32_t interval = intervals[accepted];
        if (!period_is_playable(pg, interval)) {
            break;
        }

        size_t next_head = next_index(pg, head);
        if (next_head == pg->sched.queue_tail) {
            break; /* full */
        }

        /* Slot first, index second: once the refill sees the new head, the
           event behind it must already be in place. */
        pg->sched.queue[head] = interval;
        pg->sched.queue_head = next_head;

        head = next_head;
        accepted++;
    }

    return accepted;
}

pulse_generator_status_t pulse_generator_start_scheduled(pulse_generator_t *pg)
{
    if (pg == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    if (pg->mode != PULSE_GENERATOR_MODE_SCHEDULED || pg->state != PULSE_GENERATOR_STATE_ARMED) {
        return PULSE_GENERATOR_ERROR_INVALID_STATE;
    }

    /* A pulse's period is the next event's interval, so a movement needs one
       event of look-ahead, unless finish already says the first event is
       also the last. */
    const size_t pending = pending_events(pg);
    if (pending == 0 || (pending == 1 && !pg->sched.finish_requested)) {
        return PULSE_GENERATOR_ERROR_INVALID_STATE;
    }

    /* The first event becomes the lead-in period. While ARMED nothing else
       consumes, so that a failed start can move tail back and leave the
       queue as it was. */
    const size_t tail = pg->sched.queue_tail;

    pg->sched.interval_current = pg->sched.queue[tail];
    pg->sched.queue_tail = next_index(pg, tail);
    pg->sched.lead_in = true;
    pg->sched.source_dry = false;
    pg->sched.low_watermark_armed = true;

    pulse_generator_status_t status = fill_and_start_stream(pg);
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

uint32_t pulse_generator_get_underrun_count(const pulse_generator_t *pg)
{
    if (pg == NULL) {
        return 0;
    }

    return pg->sched.underrun_count;
}

/* --- Control and status --- */

pulse_generator_status_t pulse_generator_stop(pulse_generator_t *pg)
{
    if (pg == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    if (pg->state == PULSE_GENERATOR_STATE_IDLE) {
        return PULSE_GENERATOR_OK;
    }

    /* No event: a stop is neither a completion nor an underrun. */
    return stop_output(pg);
}

bool pulse_generator_is_busy(const pulse_generator_t *pg)
{
    return pulse_generator_get_state(pg) == PULSE_GENERATOR_STATE_RUNNING;
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
    /* Counted from the stream's position, so there is nothing to read once
       it has stopped. */
    if (pg == NULL || pg->state != PULSE_GENERATOR_STATE_RUNNING) {
        return 0;
    }

    return falls_finished(pg) - pg->dma.count_base;
}

void pulse_generator_reset_pulse_count(pulse_generator_t *pg)
{
    if (pg == NULL || pg->state != PULSE_GENERATOR_STATE_RUNNING) {
        return;
    }

    /* FIXED_COUNT's end moves with the count: the source writes pulse_count
       more from here. If the end is already in the half being played, the
       new pulses only start after it, leaving a gap. */
    const uint32_t finished = falls_finished(pg);
    pg->dma.count_base = finished;
    if (pg->mode == PULSE_GENERATOR_MODE_FIXED_COUNT) {
        pg->dma.pulse_target = finished + pg->dma.pulses_requested;
    }
}

/* --- DMA interrupt entry points --- */

pulse_generator_status_t pulse_generator_notify_dma_half_complete(pulse_generator_t *pg)
{
    if (pg == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    return on_half_fetched(pg, 0);
}

pulse_generator_status_t pulse_generator_notify_dma_complete(pulse_generator_t *pg)
{
    if (pg == NULL) {
        return PULSE_GENERATOR_ERROR_INVALID_PARAM;
    }

    return on_half_fetched(pg, 1);
}
