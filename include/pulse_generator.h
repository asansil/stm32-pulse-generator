#ifndef PULSE_GENERATOR_H
#define PULSE_GENERATOR_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Status codes --- */

/**
 * @brief Result/error codes returned by most pulse_generator_* functions
 *        and by pulse_generator_ops_t hooks.
 */
typedef enum {
    PULSE_GENERATOR_OK = 0,
    PULSE_GENERATOR_ERROR,               /* generic/unspecified hardware fault */
    PULSE_GENERATOR_ERROR_BUSY,          /* peripheral already running */
    PULSE_GENERATOR_ERROR_INVALID_PARAM, /* null pointer, bad config, etc. */
    PULSE_GENERATOR_ERROR_INVALID_STATE, /* operation not valid in current mode/state */
    PULSE_GENERATOR_ERROR_MISSED_COMPARE, /* a compare match was serviced too late and had to be rescheduled */
    PULSE_GENERATOR_ERROR_NOT_SUPPORTED, /* the operation or engine requested is not available yet */
    PULSE_GENERATOR_ERROR_UNDERRUN,      /* a movement stopped: it ran out of usable events, or a DMA refill came too late */
} pulse_generator_status_t;

/* --- Modes and state --- */

/**
 * @brief Pulse pattern an instance is currently configured to generate.
 *
 * The mode decides when the pulses come; the engine, chosen at init by the
 * hook group the ops table provides (see pulse_generator_ops_t), decides
 * how the hardware produces them.
 */
typedef enum {
    PULSE_GENERATOR_MODE_FIXED_COUNT, /* N pulses at a fixed frequency, auto-stop */
    PULSE_GENERATOR_MODE_CONTINUOUS,  /* fixed frequency, runs until stop() */
    PULSE_GENERATOR_MODE_SCHEDULED,   /* caller-timed pulses streamed through a queue */
} pulse_generator_mode_t;

/**
 * @brief Coarse-grained lifecycle state of an instance.
 */
typedef enum {
    PULSE_GENERATOR_STATE_IDLE,    /* no mode configured, no movement in progress */
    PULSE_GENERATOR_STATE_ARMED,   /* SCHEDULED mode prepared: the queue accepts events, nothing is emitted */
    PULSE_GENERATOR_STATE_RUNNING, /* a movement is currently in progress */
} pulse_generator_state_t;

/* --- Events --- */

/**
 * @brief A single pulse generator instance. Defined further down; declared
 *        here because an event callback receives the instance reporting it.
 */
typedef struct pulse_generator_s pulse_generator_t;

/**
 * @brief Things an instance reports to its owner as they happen.
 *
 * One callback carrying an event code, rather than one callback field per
 * notification: a new notification becomes one more value here, with no new
 * struct field and no new registration function.
 */
typedef enum {
    /** A movement reached its end and stopped itself: a FIXED_COUNT one
        emitted its last pulse (the instance is already IDLE when the callback
        runs), or a SCHEDULED one drained its queue after
        pulse_generator_finish_scheduled() (the instance is already back in
        ARMED). */
    PULSE_GENERATOR_EVENT_COMPLETE,
    /** A compare match was serviced so late that the counter had already
        passed the next one, which was therefore rescheduled from the current
        counter value: one edge comes late, the output keeps running. Also
        reported as the return value of
        pulse_generator_notify_compare_match() — the same information by two
        routes, so an integrator can use whichever fits better. Never reported
        in SCHEDULED mode, where a late edge ends the movement instead (see
        PULSE_GENERATOR_EVENT_UNDERRUN). */
    PULSE_GENERATOR_EVENT_MISSED_COMPARE,
    /** The events pending in a SCHEDULED queue dropped below the configured
        low watermark: refill it now. Edge-triggered, so a draining queue
        reports it once, and again only after a refill has brought it back
        to the watermark or above. Not reported once
        pulse_generator_finish_scheduled() has been called: the producer has
        nothing more to add. */
    PULSE_GENERATOR_EVENT_LOW_WATERMARK,
    /** A SCHEDULED movement stopped without being told to finish: the queue
        ran dry, an edge was serviced too late to land at its instant, or the
        platform failed to arm one. In the first two cases it stops at the end
        of a complete pulse with the output low; in the last, the output may
        be left high. The instance is already back in ARMED, with the queue
        flushed. Also reported through the return value of
        pulse_generator_notify_compare_match(): PULSE_GENERATOR_ERROR_UNDERRUN,
        or the platform's own error in the last case.
        On the DMA engine, also reported when a refill interrupt is serviced
        too late to be used, in any mode: the output is stopped at once with
        the pin low (a pulse in progress is cut short), FIXED_COUNT and
        CONTINUOUS go back to IDLE, and the return value of the
        pulse_generator_notify_dma_* call is
        PULSE_GENERATOR_ERROR_UNDERRUN. */
    PULSE_GENERATOR_EVENT_UNDERRUN,
} pulse_generator_event_t;

/**
 * @brief Signature for the event notification callback.
 * @param pg       Instance reporting the event.
 * @param event    What happened.
 * @param user_ctx Opaque pointer supplied in pulse_generator_config_t,
 *                 passed back unchanged.
 * @warning May be invoked from interrupt context, e.g. from within
 *          pulse_generator_notify_compare_match() or the
 *          pulse_generator_notify_dma_* functions. Must be short,
 *          non-blocking, and must not call any pulse_generator_* function
 *          on the same instance from within it — with one exception:
 *          pulse_generator_queue_events(), pulse_generator_get_free_space()
 *          and pulse_generator_get_pending_events() may be called on it, so
 *          that a SCHEDULED queue can be refilled straight from
 *          PULSE_GENERATOR_EVENT_LOW_WATERMARK (as long as that is the
 *          queue's only producer).
 */
typedef void (*pulse_generator_event_cb_t)(pulse_generator_t *pg,
                                           pulse_generator_event_t event,
                                           void *user_ctx);

/* --- DMA buffer layout --- */

/**
 * @brief Where the library writes each value within one entry of a DMA
 *        buffer. Reported by the platform, see
 *        pulse_generator_ops_t::get_entry_layout.
 *
 * Words of an entry not named here are written as 0.
 */
typedef struct {
    size_t words_per_entry; /* words the DMA writes on each update event */
    size_t period_index;    /* word holding the period length minus one (ARR) */
    size_t compare_index;   /* word holding the compare value (CCR) */
} pulse_generator_entry_layout_t;

/* --- Platform abstraction --- */

/**
 * @brief Function-pointer table an integrator implements to bind the
 *        library to real hardware.
 *
 * The library never includes vendor HAL headers or touches registers
 * directly — every hardware access goes through this table.
 *
 * The table holds no per-instance data: every hook takes an opaque `hw`
 * pointer saying which output to act on, so one const table in flash serves
 * every channel of a platform, and a single firmware can mix platforms
 * (a real timer and a mock, say) without duplicating it.
 *
 * The hooks are deliberately dumb register accessors. All the arithmetic —
 * converting a frequency to ticks, where the next compare value lands,
 * whether an interrupt arrived too late to use it — lives in the library,
 * where the host test suite can reach it.
 *
 * The hooks come in groups. get_tick_hz is common and always mandatory; on
 * top of it a table provides exactly one complete group, and that group
 * selects the engine the instance runs on:
 *  - compare group: channel_start, channel_stop, set_compare, get_counter,
 *    get_counter_max. A free-running counter, one compare interrupt per
 *    edge.
 *  - DMA group: stream_start, stream_stop, get_stream_remaining,
 *    get_period_max, get_entry_layout. A whole timer per output in PWM
 *    mode, its period and compare fed from a circular DMA buffer, one
 *    interrupt per half buffer.
 * The hooks of the other group are left NULL.
 */
typedef struct {
    /**
     * @brief Start the Output Compare channel in free-running toggle mode,
     *        with the first compare match armed at exactly `first_compare`.
     * @param hw            Opaque per-output handle (see
     *                      pulse_generator_config_t::hw).
     * @param first_compare Absolute counter value at which the first pin
     *                      toggle must happen, already wrapped to the
     *                      counter's width by the library, so it can be
     *                      written to the compare register as-is.
     * @return PULSE_GENERATOR_OK on success, an error code if the channel
     *         could not be started (e.g. PULSE_GENERATOR_ERROR_BUSY if
     *         already running).
     * @note Compare group. Must NOT reset the counter: the library computed
     *       `first_compare` from get_counter() just before calling, and
     *       other channels of the same counter may be running. Write the
     *       compare register as late as possible before enabling the
     *       channel — everything between that get_counter() and this write
     *       eats into the first half period.
     */
    pulse_generator_status_t (*channel_start)(void *hw, uint32_t first_compare);

    /**
     * @brief Stop the Output Compare channel immediately. No further pulses
     *        are generated until channel_start is called again.
     * @param hw Opaque per-output handle.
     * @return PULSE_GENERATOR_OK on success, an error code otherwise.
     * @note Compare group. Must not stop the counter itself while other
     *       channels sharing it are still active.
     */
    pulse_generator_status_t (*channel_stop)(void *hw);

    /**
     * @brief Write the channel's compare register.
     * @param hw      Opaque per-output handle.
     * @param compare Absolute counter value of the next pin toggle, already
     *                wrapped to the counter's width by the library.
     * @return PULSE_GENERATOR_OK on success, an error code otherwise.
     * @note Compare group. A plain register write is all this is meant to
     *       be: it must not read the counter, decide anything or reschedule.
     *       The library picks the value and detects a missed match itself.
     *       Called from pulse_generator_notify_compare_match(), i.e. from
     *       interrupt context: must be short and non-blocking.
     */
    pulse_generator_status_t (*set_compare)(void *hw, uint32_t compare);

    /**
     * @brief Read the channel's counter.
     * @param hw Opaque per-output handle.
     * @return The current counter value, between 0 and get_counter_max().
     * @note Compare group. Also called from interrupt context: the library
     *       uses it to tell whether the compare it just scheduled has
     *       already been passed.
     */
    uint32_t (*get_counter)(void *hw);

    /**
     * @brief Report the counter's counting frequency, in Hz.
     * @param hw Opaque per-output handle.
     * @return The tick rate: how many times per second the counter
     *         increments, i.e. the frequency already divided by the
     *         prescaler. NOT the raw peripheral/bus clock (e.g. PCLK1) —
     *         the library has no way to apply a prescaler on top of that
     *         itself. Used to convert requested frequencies and times into
     *         timer ticks.
     * @note Mandatory hook, common to both groups. Read once per movement,
     *       when it is started, and never from interrupt context, so it may
     *       be computed rather than cached. Must not change while a movement
     *       is in progress.
     */
    uint32_t (*get_tick_hz)(void *hw);

    /**
     * @brief Report the counter's maximum value.
     * @param hw Opaque per-output handle.
     * @return The largest value the counter reaches before wrapping, e.g.
     *         0xFFFF for a 16-bit timer or 0xFFFFFFFF for a 32-bit one.
     * @note Compare group. The library uses this for two things: as the
     *       upper bound on a half period (frequencies too low to fit are
     *       rejected), and as the bit mask of its modular compare
     *       arithmetic. That second use means it MUST be exactly
     *       2^width - 1 — a value clamped for any other reason would
     *       silently corrupt every compare — and the channel must be left
     *       free-running over that full range (on STM32, auto-reload at the
     *       counter's maximum). Read once per movement; must not change
     *       while one is in progress.
     */
    uint32_t (*get_counter_max)(void *hw);

    /**
     * @brief Start emitting a movement from a circular DMA buffer.
     * @param hw     Opaque per-output handle.
     * @param buffer First word of the buffer, already filled by the library
     *               with entries laid out as get_entry_layout() describes.
     * @param words  Length of the buffer in words (entries times
     *               words_per_entry): the transfer count of one lap.
     * @return PULSE_GENERATOR_OK on success; an error code otherwise, with
     *         nothing left running.
     * @note DMA group. Sets up the stream in circular mode with half-transfer
     *       and transfer-complete interrupts, writing one entry into the
     *       timer's preload registers on each update request (DMA burst),
     *       and starts the timer. Entry 0 must be the first period of the
     *       movement, and entry k+1 is fetched at the update event that
     *       starts entry k. Priming may put one short period without a pulse
     *       before entry 0; the pin stays low until entry 0 starts.
     */
    pulse_generator_status_t (*stream_start)(void *hw, const volatile uint32_t *buffer, size_t words);

    /**
     * @brief Stop the output immediately: channel disabled with the pin low,
     *        DMA stream and timer stopped.
     * @param hw Opaque per-output handle.
     * @return PULSE_GENERATOR_OK on success, an error code otherwise.
     * @note DMA group. Called from pulse_generator_stop() and also from the
     *       stream's interrupt context when a movement ends by itself: must
     *       be short and non-blocking, and harmless on an output that is
     *       already stopped.
     */
    pulse_generator_status_t (*stream_stop)(void *hw);

    /**
     * @brief Report how far the stream has got in the current lap of the
     *        buffer.
     * @param hw Opaque per-output handle.
     * @return Words still to be transferred before the end of the buffer (on
     *         STM32, the stream's NDTR register): `words` at the start of a
     *         lap, counting down to 1 before it reloads.
     * @note DMA group. A plain register read. Called from interrupt context,
     *       to tell a refill serviced in time from a late one, and from
     *       pulse_generator_get_pulse_count().
     */
    size_t (*get_stream_remaining)(void *hw);

    /**
     * @brief Report the largest value the timer's period and compare
     *        registers accept.
     * @param hw Opaque per-output handle.
     * @return E.g. 0xFFFF for a 16-bit timer or 0xFFFFFFFF for a 32-bit one.
     * @note DMA group. Read once per movement. Unlike get_counter_max(), it
     *       need not be 2^n - 1: it is only a ceiling, and longer periods
     *       are split into several entries. The library keeps every entry at
     *       most this value minus one ticks long, so that a compare of the
     *       entry's length plus one, which holds the pin high throughout,
     *       still fits the register.
     */
    uint32_t (*get_period_max)(void *hw);

    /**
     * @brief Describe where the period and compare values go within one
     *        entry of the buffer.
     * @param hw     Opaque per-output handle.
     * @param layout Filled in by the hook.
     * @return PULSE_GENERATOR_OK on success, an error code if this output
     *         cannot be driven from a DMA buffer.
     * @note DMA group. Read once per movement. A hook rather than a constant
     *       because the layout depends on the output: on STM32 a burst
     *       writes consecutive registers, so channel 1 takes ARR, RCR, CCR1
     *       (three words, period at 0, compare at 2) and channel 2 takes
     *       four.
     */
    pulse_generator_status_t (*get_entry_layout)(void *hw, pulse_generator_entry_layout_t *layout);
} pulse_generator_ops_t;

/* --- Configuration --- */

/**
 * @brief Shape of each pulse emitted by the DMA engine.
 */
typedef enum {
    PULSE_GENERATOR_PULSE_SHAPE_HALF_PERIOD = 0, /* high for half of each period; the default */
    PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH,     /* high for width_ns, whatever the period */
} pulse_generator_pulse_shape_t;

/**
 * @brief DMA engine part of pulse_generator_config_t. Read only when the ops
 *        table provides the DMA group; ignored otherwise.
 *
 * Times are given in µs and ns rather than in ticks, since init() calls no
 * hook and the tick rate is only known once a movement starts: they are
 * converted then, and a value that does not fit is rejected there.
 */
typedef struct {
    /** Caller-owned storage the DMA stream reads from. Mandatory. Must
        outlive the instance, and must not be touched by the caller while a
        movement runs. */
    uint32_t *buffer;

    /** Words in `buffer`. Must hold `entries` entries of the platform's
        layout, which is only known, and so only checked, when a movement
        starts. */
    size_t buffer_words;

    /** Entries in the buffer (N). Even, since it is refilled half at a time,
        and at least 4. */
    size_t entries;

    /** Latency bound in µs, mandatory: no entry lasts longer than
        window_us / entries, so what a refill writes reaches the pin within
        the window. Longer periods are split into several entries. */
    uint32_t window_us;

    /** Shape of each pulse. Zero (unset) means half period. */
    pulse_generator_pulse_shape_t pulse_shape;

    /** High time of every pulse with PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH,
        in ns, rounded up to whole ticks. Must be non-zero with that shape;
        ignored with the other. */
    uint32_t width_ns;

    /** Shortest entry the hardware can play, in ns: how long the DMA burst
        takes to land after an update event, measured on the board. Rounded
        up to whole ticks. 0 leaves only the structural minimum of 2 ticks
        (one high, one low). window_us / entries must be at least twice
        this: a period longer than one entry is split into pieces of just
        over half an entry, which must still be playable. */
    uint32_t min_entry_ns;
} pulse_generator_dma_config_t;

/**
 * @brief Everything pulse_generator_init() needs to bind an instance to one
 *        hardware output.
 *
 * Passed as a struct rather than as loose parameters so later options can be
 * added without breaking the signature: a field nobody sets reads as zero,
 * which every field here treats as "not configured".
 *
 * pulse_generator_init() copies what it needs and does not keep the struct,
 * so a compound literal on the stack is fine.
 */
typedef struct {
    /** Operations table for this output's platform. Mandatory, and must
        outlive the instance — typically a const global in flash. */
    const pulse_generator_ops_t *ops;

    /** Opaque handle passed unchanged to every hook in `ops`, saying which
        output they act on. Typically holds whatever the integrator's HAL
        calls need (timer handle, channel, DMA stream), since the library
        never touches vendor types. This is what makes multi-instance just
        "one hw struct per axis". May be NULL for a platform that needs no
        per-output state. Must outlive the instance. */
    void *hw;

    /** Called when the instance reports an event. Optional: NULL disables
        event notifications entirely. */
    pulse_generator_event_cb_t on_event;

    /** Opaque pointer passed back unchanged to on_event. */
    void *user_ctx;

    /** DMA engine settings. Read only when `ops` provides the DMA group. */
    pulse_generator_dma_config_t dma;
} pulse_generator_config_t;

/* --- Scheduled mode --- */

/**
 * @brief What executes a SCHEDULED movement. The mode behaves the same with
 *        either; only the cost per pulse differs.
 */
typedef enum {
    PULSE_GENERATOR_ENGINE_ISR, /* one compare interrupt per edge; low and medium rates */
    PULSE_GENERATOR_ENGINE_DMA, /* internal double buffer fed by DMA; not available yet */
} pulse_generator_engine_t;

/**
 * @brief Everything pulse_generator_prepare_scheduled() needs.
 *
 * Copied by prepare_scheduled() and not retained, like
 * pulse_generator_config_t — but the array `queue` points to is used in
 * place for as long as the instance stays prepared.
 */
typedef struct {
    /** Caller-owned storage for the event queue, a ring buffer of intervals
        in ticks. Mandatory. Must outlive the preparation, and must not be
        touched by the caller while the instance uses it. */
    uint32_t *queue;

    /** Elements in `queue`. One slot is always left empty to tell a full
        queue from an empty one, so at most queue_capacity - 1 events are
        pending at a time. Must be at least 2; need not be a power of two. */
    size_t queue_capacity;

    /** When the events pending drop below this many,
        PULSE_GENERATOR_EVENT_LOW_WATERMARK is reported. 0 disables the
        notification. Must be less than queue_capacity. */
    size_t low_watermark;

    /** Shortest half pulse the engine can honour, in ticks: what the
        integrator measured the compare ISR (or the DMA) to cost.
        pulse_generator_queue_events() rejects any interval whose halves
        would be shorter. 0 disables the check.
        Also how far ahead a late fall is moved to end the movement with the
        output low (see PULSE_GENERATOR_EVENT_UNDERRUN). At 0 that margin is
        a single tick, which on real hardware has usually gone by before the
        compare register is written, leaving the channel silent until the
        counter wraps round: set it to the measured ISR cost. */
    uint32_t min_interval_ticks;

    /** Engine executing the movement. */
    pulse_generator_engine_t engine;
} pulse_generator_scheduled_config_t;

/* --- Instance handle --- */

/**
 * @brief SCHEDULED-mode part of pulse_generator_t. Private, like every
 *        field of the instance: named only so the library can reset it as
 *        a whole.
 */
typedef struct {
    /* Copied from pulse_generator_scheduled_config_t by prepare_scheduled()
       and constant until the next init(). The slots are volatile like the
       indices: the compiler orders volatile accesses only among
       themselves, so a plain slot write could legally be moved past the
       head that publishes it. */
    volatile uint32_t       *queue;
    size_t                   queue_capacity;
    size_t                   low_watermark;
    uint32_t                 min_interval_ticks;
    pulse_generator_engine_t engine;

    /* Single-producer / single-consumer ring buffer: head is written only by
       queue_events(), tail only by the compare ISR, and each side writes its
       slot before publishing its index, which is what makes it lock-free. */
    volatile size_t   queue_head;
    volatile size_t   queue_tail;
    volatile bool     finish_requested; /* set by finish_scheduled(), read by the ISR */
    volatile uint32_t underrun_count;   /* reset by prepare_scheduled(), not by a restart */

    /* Written only by the compare ISR (and by start_scheduled() before the
       channel is armed). */
    uint32_t                interval_current;    /* interval that produced the rise last armed */
    uint32_t                interval_next;       /* interval to the next pulse, popped at the rise */
    uint32_t                rise_tick;           /* absolute tick of the last rise, masked */
    bool                    fall_armed;          /* the compare armed is a fall, not a rise */
    bool                    stop_after_fall;     /* the next fall is the last edge of the movement */
    pulse_generator_event_t stop_event;          /* what to report then: COMPLETE or UNDERRUN */
    bool                    low_watermark_armed; /* edge-triggers EVENT_LOW_WATERMARK */
} pulse_generator_sched_t;

/**
 * @brief DMA-engine part of pulse_generator_t. Private, like every field of
 *        the instance: named only so the library can reset it as a whole.
 */
typedef struct {
    /* Copied from pulse_generator_dma_config_t by init() and constant until
       the next init(). The buffer is volatile because the library rewrites
       it while the DMA stream is reading it. */
    volatile uint32_t            *buffer;
    size_t                        buffer_words;
    size_t                        entries;
    uint32_t                      window_us;
    pulse_generator_pulse_shape_t pulse_shape;
    uint32_t                      width_ns;
    uint32_t                      min_entry_ns;

    /* Captured from the platform and converted to ticks when a movement
       starts; constant while it runs. */
    pulse_generator_entry_layout_t layout;
    uint32_t segment_max_ticks; /* longest entry: window / entries, capped by the registers */
    uint32_t min_entry_ticks;   /* shortest entry the hardware can play */
    uint32_t width_ticks;       /* high time with FIXED_WIDTH; 0 with HALF_PERIOD */

    /* Source state: set when a movement starts, advanced by every fill. The
       period is tick_hz / frequency_hz, kept as a whole part plus a
       remainder carried from one period to the next, so that the average
       frequency is exact. */
    uint32_t period_ticks;     /* whole part of the period */
    uint32_t period_remainder; /* tick_hz % frequency_hz */
    uint32_t period_divisor;   /* frequency_hz */
    uint32_t period_carry;     /* remainder accumulated so far, below period_divisor */
    uint32_t pulses_written;   /* pulses put in the buffer since the movement started */
    uint32_t pulses_requested; /* FIXED_COUNT: the pulse_count it was started with */
    volatile uint32_t pulse_target; /* FIXED_COUNT: pulses after which the source runs dry;
                                       pushed back by reset_pulse_count() */

    /* Splitting state: the period being cut into entries, which may
       straddle two fills. */
    uint32_t split_period; /* length of the period being split */
    uint32_t split_high;   /* its high time */
    uint32_t split_start;  /* ticks of it already written */
    uint32_t split_count;  /* entries it is cut into */
    uint32_t split_index;  /* entries of it already written; split_count once done */

    /* Whether each half of the buffer, as last filled, holds any entry from
       the source rather than only stop padding: with neither holding one and
       the source dry, the movement has finished playing. */
    bool half_has_pulses[2];

    /* Pulse counting, from the buffer and the stream's position. Fills go
       in order, so the buffer always holds the last `entries` entries
       written, the older half's first: falls_before_half says how many
       falls were written before each half's content, and the falls in
       entries from there on are read back from the buffer. The two entries
       a fill overwrites while they may still be playing leave theirs in
       overwritten_tail. Volatile because get_pulse_count() reads them
       outside the stream interrupt; fill_count, bumped by every fill, tells
       it to read again if one ran meanwhile. */
    volatile uint32_t falls_written;        /* falls written since the movement started */
    volatile uint32_t falls_before_half[2]; /* falls_written when each half was last filled */
    volatile uint8_t  newer_half;           /* the half filled last */
    volatile uint8_t  overwritten_tail;     /* falls in the last two entries the latest fill
                                               overwrote: bit 1 the last one, bit 0 the one before */
    volatile uint32_t fill_count;
    volatile uint32_t count_base;           /* falls finished at the last reset_pulse_count() */
} pulse_generator_dma_t;

/**
 * @brief A single pulse generator instance (one per axis/output).
 *
 * Fully defined here rather than opaque, so callers can allocate it
 * statically (no heap) and multi-instance is just "one variable per axis".
 * All fields below are private: interact with an instance only through the
 * pulse_generator_* functions, never by reading or writing these fields
 * directly.
 *
 * Fields shared between an ISR (compare or DMA) and ordinary code are
 * volatile. That buys visibility, not atomicity, which is enough here: each
 * is a single naturally aligned word or byte, with a single writer at any
 * given time.
 */
struct pulse_generator_s {
    const pulse_generator_ops_t *ops;
    void                        *hw;
    pulse_generator_event_cb_t   on_event;
    void                        *user_ctx;
    bool                         dma_engine; /* the ops table provides the DMA group, not the compare one */

    volatile pulse_generator_state_t state;
    pulse_generator_mode_t           mode;          /* meaningful only while state != IDLE */

    /* Captured from the platform when a movement starts (in SCHEDULED mode,
       when it is prepared) and constant from then on, so neither the compare
       ISR nor set_frequency() needs a hook call to read them back. */
    uint32_t tick_hz;
    uint32_t counter_max;

    volatile uint32_t half_period_ticks; /* ticks between toggles; meaningful only while RUNNING */
    volatile uint32_t last_compare;      /* absolute counter value of the compare match currently
                                              armed, the reference the next one is measured from */
    volatile uint32_t edge_count;        /* pin toggles observed via notify_compare_match() during
                                              the movement in progress; two edges = one full pulse;
                                              always 0 while no movement is RUNNING */
    uint32_t target_edge_count;          /* edges after which a FIXED_COUNT movement stops itself;
                                              0 means no target, i.e. a CONTINUOUS movement */

    /* SCHEDULED mode only; meaningless in the other modes. */
    pulse_generator_sched_t sched;

    /* DMA engine only; meaningless on the compare engine. */
    pulse_generator_dma_t dma;
};

/* --- Public API --- */

/**
 * @brief Initialize an instance against one hardware output.
 * @param pg     Instance to initialize. Its previous contents are
 *               overwritten, so this must not be called on an instance with
 *               a movement in progress.
 * @param config Platform ops, hardware handle and optional event callback.
 *               See pulse_generator_config_t. Not retained.
 * @return PULSE_GENERATOR_OK on success;
 *         PULSE_GENERATOR_ERROR_INVALID_PARAM if pg or config is NULL, if
 *         config->ops is NULL, if it lacks get_tick_hz, or if it does not
 *         provide exactly one complete hook group; with the DMA group, also
 *         if config->dma is invalid: a NULL buffer, entries odd or below 4,
 *         window_us 0, an unknown pulse shape, or width_ns 0 with
 *         PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH.
 * @note Calls no hook, so it imposes no ordering against the integrator's
 *       clock and peripheral setup. The platform is first touched when a
 *       movement is started (or, in SCHEDULED mode, prepared).
 * @note Also the way back from ARMED to IDLE: a prepared instance with no
 *       movement RUNNING may be initialized again.
 */
pulse_generator_status_t pulse_generator_init(
    pulse_generator_t *pg,
    const pulse_generator_config_t *config);

/**
 * @brief Start a movement of exactly pulse_count pulses at a fixed
 *        frequency, stopping itself when the last one has been emitted.
 * @param pg          Instance.
 * @param frequency_hz Pulse frequency in Hz. On the compare engine, must map
 *                    to a half period between 1 tick and the counter's
 *                    maximum, given the platform's tick rate. On the DMA
 *                    engine, the period (tick rate / frequency, exact on
 *                    average) must be at least the minimum entry and, with
 *                    PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH, longer than
 *                    the pulse width; there is no lower limit, since long
 *                    periods are split into several entries.
 * @param pulse_count Number of pulses to emit. Must be between 1 and
 *                    UINT32_MAX / 2 (each pulse is two compare matches).
 * @return PULSE_GENERATOR_OK if the movement was started;
 *         PULSE_GENERATOR_ERROR_INVALID_STATE if the instance is not IDLE:
 *         a movement is in progress, or it is prepared for SCHEDULED mode;
 *         PULSE_GENERATOR_ERROR_INVALID_PARAM on a null instance, an out of
 *         range frequency or pulse count, or a platform reporting a counter
 *         maximum that is not of the form 2^n - 1; on the DMA engine, also
 *         a buffer too small for the platform's entry layout, an invalid
 *         layout, or a window whose entries come out shorter than twice the
 *         minimum entry;
 *         otherwise whatever the platform returned while being armed, with
 *         the instance left IDLE.
 * @note Returns as soon as the hardware is armed: the pulses are emitted in
 *       the background. Completion is reported as
 *       PULSE_GENERATOR_EVENT_COMPLETE; on the DMA engine, up to one window
 *       after the last period ends, i.e. after the low half that follows
 *       the last pulse.
 */
pulse_generator_status_t pulse_generator_start_fixed_count(
    pulse_generator_t *pg,
    uint32_t frequency_hz,
    uint32_t pulse_count);

/**
 * @brief Start an open-ended movement at a fixed frequency, running until
 *        pulse_generator_stop() is called.
 * @param pg           Instance.
 * @param frequency_hz Pulse frequency in Hz, same range as in
 *                     pulse_generator_start_fixed_count(). Can be changed
 *                     while running with pulse_generator_set_frequency().
 * @return Same codes as pulse_generator_start_fixed_count(), minus the ones
 *         about the pulse count;
 *         PULSE_GENERATOR_ERROR_NOT_SUPPORTED on an instance running on the
 *         DMA engine, which does not offer this mode yet.
 * @note Returns as soon as the hardware is armed. Never completes on its
 *       own, so PULSE_GENERATOR_EVENT_COMPLETE is never reported for this
 *       mode.
 */
pulse_generator_status_t pulse_generator_start_continuous(
    pulse_generator_t *pg,
    uint32_t frequency_hz);

/* --- Scheduled mode --- */

/**
 * @brief Prepare an instance for a SCHEDULED movement: bind its event queue
 *        and move it from IDLE to ARMED.
 * @param pg     Instance.
 * @param config Queue storage, low watermark, minimum interval and engine.
 *               See pulse_generator_scheduled_config_t. Not retained, but
 *               config->queue is used in place.
 * @return PULSE_GENERATOR_OK if the instance is now ARMED with an empty queue;
 *         PULSE_GENERATOR_ERROR_INVALID_PARAM on a null instance, config or
 *         queue, a capacity below 2, a low watermark not below the capacity,
 *         an unknown engine, or a platform reporting a counter maximum that
 *         is not of the form 2^n - 1;
 *         PULSE_GENERATOR_ERROR_INVALID_STATE if the instance is not IDLE;
 *         PULSE_GENERATOR_ERROR_NOT_SUPPORTED for PULSE_GENERATOR_ENGINE_DMA,
 *         which is not available yet, or on an instance running on the DMA
 *         engine, which does not offer this mode yet.
 * @note Reads the platform's tick rate and counter maximum here rather than
 *       at start, so that pulse_generator_queue_events() can validate
 *       intervals against them. Not for interrupt context.
 * @note The instance stays ARMED from one movement to the next; only
 *       pulse_generator_init() takes it back to IDLE.
 */
pulse_generator_status_t pulse_generator_prepare_scheduled(
    pulse_generator_t *pg,
    const pulse_generator_scheduled_config_t *config);

/**
 * @brief Append events to the queue of a SCHEDULED instance. Never blocks.
 * @param pg        Instance, ARMED or RUNNING.
 * @param intervals Events to append, each one pulse given as the interval in
 *                  ticks from the previous pulse's rise to its own (the
 *                  first pulse of a movement is measured from its start
 *                  tick). Copied into the queue.
 * @param n         Number of elements in intervals.
 * @return How many events were accepted, taken from the front of intervals.
 *         Fewer than n means that either the queue filled up or
 *         intervals[returned value] was rejected, and nothing after it was
 *         taken; pulse_generator_get_free_space() tells the two apart.
 *         0 on a null argument or if the instance is not prepared for
 *         SCHEDULED mode.
 * @note An interval is rejected if it is below 2 ticks, if its halves would
 *       be shorter than the configured min_interval_ticks, or if it exceeds
 *       the counter's maximum. Staying well below that maximum is
 *       recommended: the headroom is what lets a late edge be told from one
 *       still to come.
 * @note Compute intervals from rounded absolute instants rather than by
 *       rounding each one on its own, or the rounding errors add up along
 *       the sequence. The library executes the integers it is given as-is.
 * @note Single producer: call it from one context only (the main loop, or an
 *       interrupt such as the event callback), never from both. It is one of
 *       the functions the event callback may call, see
 *       pulse_generator_event_cb_t.
 * @note Stop queueing once the movement ends (PULSE_GENERATOR_EVENT_COMPLETE,
 *       PULSE_GENERATOR_EVENT_UNDERRUN, or after pulse_generator_stop()),
 *       and recompute from the next start tick. An event queued while the
 *       library is flushing the queue can outlive the flush, and would then
 *       open the next movement.
 */
size_t pulse_generator_queue_events(
    pulse_generator_t *pg,
    const uint32_t *intervals,
    size_t n);

/**
 * @brief Start emitting the events queued in an ARMED instance.
 * @param pg         Instance.
 * @param start_tick Absolute counter value the first interval is measured
 *                   from, typically pulse_generator_get_now_ticks() plus a
 *                   margin. Being absolute rather than a delay, the same
 *                   value given to several channels of one timer starts them
 *                   in step. Not itself an edge: the first rise lands at
 *                   start_tick plus the first interval.
 * @return PULSE_GENERATOR_OK if the instance is now RUNNING;
 *         PULSE_GENERATOR_ERROR_INVALID_STATE if the instance is not ARMED,
 *         or if fewer than two events are queued (one is enough after
 *         pulse_generator_finish_scheduled()), since a pulse needs the next
 *         one's interval to place its fall;
 *         PULSE_GENERATOR_ERROR_INVALID_PARAM on a null instance, or if the
 *         first rise is no longer ahead of the counter;
 *         otherwise whatever the platform returned while being armed.
 *         On any failure the instance stays ARMED with its queue untouched.
 * @note Returns as soon as the hardware is armed. The movement ends with
 *       PULSE_GENERATOR_EVENT_COMPLETE or PULSE_GENERATOR_EVENT_UNDERRUN,
 *       back in ARMED, ready for the next start.
 */
pulse_generator_status_t pulse_generator_start_scheduled(
    pulse_generator_t *pg,
    uint32_t start_tick);

/**
 * @brief Declare that no more events will be queued, so that the movement
 *        ends with PULSE_GENERATOR_EVENT_COMPLETE once the queue drains,
 *        rather than with PULSE_GENERATOR_EVENT_UNDERRUN.
 * @param pg Instance, ARMED or RUNNING.
 * @return PULSE_GENERATOR_OK;
 *         PULSE_GENERATOR_ERROR_INVALID_PARAM on a null instance;
 *         PULSE_GENERATOR_ERROR_INVALID_STATE if the instance is not prepared
 *         for SCHEDULED mode.
 * @note Valid before the start: queue a whole precomputed sequence, finish,
 *       then start, and that sequence is played once.
 * @note Must be called before the queue runs dry. Once the engine has found
 *       the queue empty it is too late, and the movement ends as an
 *       underrun.
 * @note Cleared whenever a movement ends: each movement needs its own.
 */
pulse_generator_status_t pulse_generator_finish_scheduled(pulse_generator_t *pg);

/**
 * @brief Report how many more events the queue can take right now.
 * @param pg Instance.
 * @return Free slots in the queue; 0 if pg is NULL or the instance is not
 *         prepared for SCHEDULED mode.
 * @note May be called from the event callback, see
 *       pulse_generator_event_cb_t.
 */
size_t pulse_generator_get_free_space(const pulse_generator_t *pg);

/**
 * @brief Report how many queued events the engine has not taken yet.
 * @param pg Instance.
 * @return Events pending in the queue; 0 if pg is NULL or the instance is
 *         not prepared for SCHEDULED mode.
 * @note May be called from the event callback, see
 *       pulse_generator_event_cb_t.
 */
size_t pulse_generator_get_pending_events(const pulse_generator_t *pg);

/**
 * @brief Read the instance's counter, to build a start tick from.
 * @param pg Instance.
 * @return The current counter value; 0 if pg is NULL or the instance runs
 *         on the DMA engine, which has no free-running counter to read.
 * @note Goes straight to the platform's get_counter hook, so it works in any
 *       state once the instance is initialized.
 */
uint32_t pulse_generator_get_now_ticks(const pulse_generator_t *pg);

/**
 * @brief Report how many SCHEDULED movements have ended in an underrun.
 * @param pg Instance.
 * @return The count since the instance was last prepared; 0 if pg is NULL.
 * @note A diagnostic. Reset by pulse_generator_prepare_scheduled(), not by
 *       a restart.
 */
uint32_t pulse_generator_get_underrun_count(const pulse_generator_t *pg);

/**
 * @brief Stop the movement in progress immediately. On the compare engine
 *        the output is left at whatever level it holds; on the DMA engine it
 *        is forced low, and the entries left in the buffer are discarded.
 * @param pg Instance.
 * @return PULSE_GENERATOR_OK, including when nothing is running (this is
 *         idempotent); PULSE_GENERATOR_ERROR_INVALID_PARAM on a null
 *         instance; otherwise whatever the platform returned while being
 *         disarmed, with the instance left stopped regardless.
 * @note FIXED_COUNT and CONTINUOUS go back to IDLE. SCHEDULED goes back to
 *       ARMED with its queue flushed and any finish request cleared, since
 *       the events left were timed against a train that no longer exists;
 *       an instance that was only ARMED gets the same flush, without the
 *       hardware being touched.
 * @note Resets the pulse counter and reports no event: COMPLETE means "the
 *       requested pulses were emitted" and UNDERRUN "the movement ran out of
 *       events", neither of which an explicit stop is.
 * @note Compare engine: a compare interrupt already in flight can still
 *       write the compare register just after this returns. That is
 *       harmless — the channel is disabled, so the match drives nothing, and
 *       the next start overwrites the register — but an integrator who wants
 *       the sequence airtight should call this with the compare interrupt
 *       masked.
 */
pulse_generator_status_t pulse_generator_stop(pulse_generator_t *pg);

/**
 * @brief Report whether a movement is currently in progress.
 * @param pg Instance.
 * @return true while a movement is RUNNING; false otherwise, including
 *         while ARMED, or if pg is NULL.
 */
bool pulse_generator_is_busy(const pulse_generator_t *pg);

/**
 * @brief Report the instance's lifecycle state.
 * @param pg Instance.
 * @return The current state, or PULSE_GENERATOR_STATE_IDLE if pg is NULL.
 */
pulse_generator_state_t pulse_generator_get_state(const pulse_generator_t *pg);

/**
 * @brief Report how many complete pulses the movement in progress has
 *        emitted.
 * @param pg Instance.
 * @return Pulses emitted since the movement started or since the counter was
 *         last reset, 0 if pg is NULL. Two compare matches make one pulse,
 *         so a half-emitted pulse is not counted yet.
 * @note Reads 0 again once a movement finishes or is stopped: it counts the
 *       movement in progress, not a lifetime total.
 * @note On the DMA engine the count is worked out from the stream's
 *       position, so it may lag the pin by up to one entry.
 */
uint32_t pulse_generator_get_pulse_count(const pulse_generator_t *pg);

/**
 * @brief Reset the pulse counter of the movement in progress to zero.
 * @param pg Instance.
 * @note For a FIXED_COUNT movement this also pushes back the point at which
 *       it stops itself, since the target is counted from the counter.
 * @note On the DMA engine, a reset that comes once the end of the movement
 *       has already been written into the buffer, i.e. within its last
 *       window, leaves a gap without pulses, up to one window long, before
 *       the extra pulses start.
 */
void pulse_generator_reset_pulse_count(pulse_generator_t *pg);

/**
 * @brief Change the frequency of a CONTINUOUS movement while it runs,
 *        without disturbing the pulse count.
 * @param pg           Instance.
 * @param frequency_hz New pulse frequency in Hz, same range as at start
 *                     time.
 * @return PULSE_GENERATOR_OK if the new frequency was accepted;
 *         PULSE_GENERATOR_ERROR_INVALID_STATE if no CONTINUOUS movement is
 *         in progress;
 *         PULSE_GENERATOR_ERROR_INVALID_PARAM on a null instance or an out
 *         of range frequency, with the previous frequency kept;
 *         PULSE_GENERATOR_ERROR_NOT_SUPPORTED on an instance running on the
 *         DMA engine, which does not offer this mode yet.
 * @note Touches no hardware: the new half period is applied by the next
 *       compare match, so the edge already scheduled still lands where it
 *       was going to. Cheap enough to call from a control loop on every
 *       cycle.
 */
pulse_generator_status_t pulse_generator_set_frequency(pulse_generator_t *pg, uint32_t frequency_hz);

/**
 * @brief Report a compare match to the library, from the integrator's timer
 *        ISR.
 * @param pg Instance owning the channel whose match fired.
 * @return PULSE_GENERATOR_OK normally;
 *         PULSE_GENERATOR_ERROR_MISSED_COMPARE if this match was serviced so
 *         late that the next one had already been passed and had to be
 *         rescheduled from the current counter value — one edge comes late,
 *         the movement keeps running (FIXED_COUNT and CONTINUOUS only);
 *         PULSE_GENERATOR_ERROR_UNDERRUN if a SCHEDULED movement has just
 *         stopped because it ran out of events or an edge was serviced too
 *         late, mirroring PULSE_GENERATOR_EVENT_UNDERRUN;
 *         PULSE_GENERATOR_ERROR_INVALID_PARAM on a null instance;
 *         otherwise whatever the platform returned.
 * @note Counts the edge and schedules the next one. A FIXED_COUNT movement
 *       that has reached its target, or a SCHEDULED one whose last pulse has
 *       just ended, stops the channel and reports its ending event.
 * @note A no-op when no timer-driven movement is in progress, or on an
 *       instance running on the DMA engine, so a late or spurious interrupt
 *       is harmless.
 */
pulse_generator_status_t pulse_generator_notify_compare_match(pulse_generator_t *pg);

/**
 * @brief Report that the DMA stream has transferred the first half of the
 *        buffer, from the integrator's half-transfer ISR.
 * @param pg Instance owning the stream.
 * @return PULSE_GENERATOR_OK normally;
 *         PULSE_GENERATOR_ERROR_UNDERRUN if the interrupt was serviced so
 *         late that the stream was already reading the half to be refilled:
 *         nothing was written and the output has been stopped, mirroring
 *         PULSE_GENERATOR_EVENT_UNDERRUN;
 *         PULSE_GENERATOR_ERROR_INVALID_PARAM on a null instance;
 *         otherwise whatever the platform returned.
 * @note Refills the half just transferred with the next entries of the
 *       movement. A movement whose last period has finished stops here and
 *       reports PULSE_GENERATOR_EVENT_COMPLETE, up to one window after that
 *       period.
 * @note A no-op when no DMA movement is in progress, so a late or spurious
 *       interrupt after a stop is harmless.
 */
pulse_generator_status_t pulse_generator_notify_dma_half_complete(pulse_generator_t *pg);

/**
 * @brief Report that the DMA stream has transferred the second half of the
 *        buffer, from the integrator's transfer-complete ISR.
 * @param pg Instance owning the stream.
 * @return Same as pulse_generator_notify_dma_half_complete().
 * @note Same behaviour, for the second half.
 */
pulse_generator_status_t pulse_generator_notify_dma_complete(pulse_generator_t *pg);

#ifdef __cplusplus
}
#endif

#endif /* PULSE_GENERATOR_H */
