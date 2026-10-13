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
    PULSE_GENERATOR_ERROR_UNDERRUN,      /* a movement stopped: it ran out of events, or a DMA refill came too late */
} pulse_generator_status_t;

/* --- Modes and state --- */

/**
 * @brief Pulse pattern an instance is currently configured to generate.
 *
 * Every mode runs on the same engine (see pulse_generator_ops_t); they
 * differ only in what fills its DMA buffer.
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
        ARMED). Reported from a refill interrupt, up to one window after the
        last period has finished playing. */
    PULSE_GENERATOR_EVENT_COMPLETE,
    /** The events pending in a SCHEDULED queue dropped below the configured
        low watermark: refill it now. Edge-triggered, so a draining queue
        reports it once, and again only after a refill has brought it back
        to the watermark or above. Not reported once
        pulse_generator_finish_scheduled() has been called: the producer has
        nothing more to add. The queue is checked once per refill, right
        after that refill has taken its events. */
    PULSE_GENERATOR_EVENT_LOW_WATERMARK,
    /** A SCHEDULED movement stopped because its queue ran dry without
        pulse_generator_finish_scheduled(). The dry queue is found by the
        refill that needed the next event, up to one window ahead of the pin:
        the pulses already written are played, the last one falling halfway
        through its own interval, and the event comes up to one window after
        it ends, with the output low. The instance is already back in ARMED,
        with the queue flushed.
        Also reported when a refill interrupt is serviced too late to be
        used, in any mode: the output is stopped at once with the pin low (a
        pulse in progress is cut short), FIXED_COUNT and CONTINUOUS go back to
        IDLE, SCHEDULED to ARMED with the queue flushed. In both cases the
        return value of the pulse_generator_notify_dma_* call is
        PULSE_GENERATOR_ERROR_UNDERRUN. */
    PULSE_GENERATOR_EVENT_UNDERRUN,
} pulse_generator_event_t;

/**
 * @brief Signature for the event notification callback.
 * @param pg       Instance reporting the event.
 * @param event    What happened.
 * @param user_ctx Opaque pointer supplied in pulse_generator_config_t,
 *                 passed back unchanged.
 * @warning May be invoked from interrupt context, from within the
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
 * converting a frequency to ticks, splitting long periods, whether a refill
 * interrupt arrived too late to use — lives in the library, where the host
 * test suite can reach it.
 *
 * Each output is a whole timer in PWM mode, its period and compare fed from
 * a circular DMA buffer, with one interrupt per half buffer. Every hook is
 * mandatory.
 */
typedef struct {
    /**
     * @brief Report the counter's counting frequency, in Hz.
     * @param hw Opaque per-output handle (see pulse_generator_config_t::hw).
     * @return The tick rate: how many times per second the counter
     *         increments, i.e. the frequency already divided by the
     *         prescaler. NOT the raw peripheral/bus clock (e.g. PCLK1) —
     *         the library has no way to apply a prescaler on top of that
     *         itself. Used to convert requested frequencies and times into
     *         timer ticks.
     * @note Read once per movement, when it is started (in SCHEDULED mode,
     *       when it is prepared), and never from interrupt context, so it
     *       may be computed rather than cached. Must not change while a
     *       movement is in progress.
     */
    uint32_t (*get_tick_hz)(void *hw);

    /**
     * @brief Start emitting a movement from a circular DMA buffer.
     * @param hw     Opaque per-output handle.
     * @param buffer First word of the buffer, already filled by the library
     *               with entries laid out as get_entry_layout() describes.
     * @param words  Length of the buffer in words (entries times
     *               words_per_entry): the transfer count of one lap.
     * @return PULSE_GENERATOR_OK on success; an error code otherwise, with
     *         nothing left running.
     * @note Sets up the stream in circular mode with half-transfer and
     *       transfer-complete interrupts, writing one entry into the timer's
     *       preload registers on each update request (DMA burst), and starts
     *       the timer. Entry 0 must be the first period of the
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
     * @note Called from pulse_generator_stop() and also from the stream's
     *       interrupt context when a movement ends by itself: must be short
     *       and non-blocking, and harmless on an output that is already
     *       stopped.
     */
    pulse_generator_status_t (*stream_stop)(void *hw);

    /**
     * @brief Report how far the stream has got in the current lap of the
     *        buffer.
     * @param hw Opaque per-output handle.
     * @return Words still to be transferred before the end of the buffer (on
     *         STM32, the stream's NDTR register): `words` at the start of a
     *         lap, counting down to 1 before it reloads.
     * @note A plain register read. Called from interrupt context, to tell a
     *       refill serviced in time from a late one, and from
     *       pulse_generator_get_pulse_count().
     */
    size_t (*get_stream_remaining)(void *hw);

    /**
     * @brief Report the largest value the timer's period and compare
     *        registers accept.
     * @param hw Opaque per-output handle.
     * @return E.g. 0xFFFF for a 16-bit timer or 0xFFFFFFFF for a 32-bit one.
     * @note Read once per movement. Need not be 2^n - 1: it is only a
     *       ceiling, and longer periods are split into several entries. The library keeps every entry at
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
     * @note Read once per movement. A hook rather than a constant because
     *       the layout depends on the output: on STM32 a burst
     *       writes consecutive registers, so channel 1 takes ARR, RCR, CCR1
     *       (three words, period at 0, compare at 2) and channel 2 takes
     *       four.
     */
    pulse_generator_status_t (*get_entry_layout)(void *hw, pulse_generator_entry_layout_t *layout);
} pulse_generator_ops_t;

/* --- Configuration --- */

/**
 * @brief Shape of each pulse.
 */
typedef enum {
    PULSE_GENERATOR_PULSE_SHAPE_HALF_PERIOD = 0, /* high for half of each period; the default */
    PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH,     /* high for width_ns, whatever the period */
} pulse_generator_pulse_shape_t;

/**
 * @brief DMA buffer and timing part of pulse_generator_config_t.
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

    /** DMA buffer and timing settings. Mandatory. */
    pulse_generator_dma_config_t dma;
} pulse_generator_config_t;

/* --- Scheduled mode --- */

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
    volatile uint32_t *queue;
    size_t             queue_capacity;
    size_t             low_watermark;

    /* Single-producer / single-consumer ring buffer: head is written only by
       queue_events(), tail only by the DMA refill, and each side writes its
       slot before publishing its index, which is what makes it lock-free. */
    volatile size_t   queue_head;
    volatile size_t   queue_tail;
    volatile bool     finish_requested; /* set by finish_scheduled(), read by the refill */
    volatile uint32_t underrun_count;   /* reset by prepare_scheduled(), not by a restart */

    /* The source, written only by the refill (and by start_scheduled()
       before the stream starts). */
    uint32_t                interval_current;    /* interval of the last pulse written */
    pulse_generator_event_t stop_event;          /* how the movement ends once the source is
                                                    dry: COMPLETE or UNDERRUN */
    bool                    low_watermark_armed; /* edge-triggers EVENT_LOW_WATERMARK */
    bool                    lead_in;             /* the next period is the one before the first rise */
    bool                    source_dry;          /* the last period has been written: no more from
                                                    the queue */
} pulse_generator_sched_t;

/**
 * @brief DMA buffer part of pulse_generator_t. Private, like every field of
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
       frequency is exact. FIXED_COUNT runs dry at pulse_target; CONTINUOUS
       never does, and switches to a new frequency only where a period
       starts, with no carry. */
    uint32_t period_ticks;     /* whole part of the period */
    uint32_t period_remainder; /* tick_hz % frequency_hz */
    uint32_t period_divisor;   /* frequency_hz */
    uint32_t period_carry;     /* remainder accumulated so far, below period_divisor */
    uint32_t pulses_written;   /* pulses put in the buffer since the movement started */
    uint32_t pulses_requested; /* FIXED_COUNT: the pulse_count it was started with */
    volatile uint32_t pulse_target; /* FIXED_COUNT: pulses after which the source runs dry;
                                       pushed back by reset_pulse_count() */
    volatile uint32_t requested_frequency_hz; /* CONTINUOUS: set by set_frequency(), taken
                                                 by the source where the next period starts */

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
 * Fields shared between the DMA stream's ISR and ordinary code are
 * volatile. That buys visibility, not atomicity, which is enough here: each
 * is a single naturally aligned word or byte, with a single writer at any
 * given time.
 */
struct pulse_generator_s {
    const pulse_generator_ops_t *ops;
    void                        *hw;
    pulse_generator_event_cb_t   on_event;
    void                        *user_ctx;

    volatile pulse_generator_state_t state;
    pulse_generator_mode_t           mode; /* meaningful only while state != IDLE */

    /* Captured from the platform when a movement starts (in SCHEDULED mode,
       when it is prepared) and constant from then on, so set_frequency()
       needs no hook call to read it back. */
    uint32_t tick_hz;

    /* SCHEDULED mode only; meaningless in the other modes. */
    pulse_generator_sched_t sched;

    /* The DMA buffer and the source filling it, shared by every mode. */
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
 *         config->ops is NULL or lacks any hook, or if config->dma is
 *         invalid: a NULL buffer, entries odd or below 4, window_us 0, an
 *         unknown pulse shape, or width_ns 0 with
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
 * @param frequency_hz Pulse frequency in Hz. The period (tick rate /
 *                    frequency, exact on average) must be at least the
 *                    minimum entry and, with
 *                    PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH, longer than
 *                    the pulse width; there is no lower limit, since long
 *                    periods are split into several entries.
 * @param pulse_count Number of pulses to emit. Must be between 1 and
 *                    UINT32_MAX / 2, which leaves
 *                    pulse_generator_reset_pulse_count() room to push the
 *                    end of the movement back.
 * @return PULSE_GENERATOR_OK if the movement was started;
 *         PULSE_GENERATOR_ERROR_INVALID_STATE if the instance is not IDLE:
 *         a movement is in progress, or it is prepared for SCHEDULED mode;
 *         PULSE_GENERATOR_ERROR_INVALID_PARAM on a null instance, an out of
 *         range frequency or pulse count, a buffer too small for the
 *         platform's entry layout, an invalid layout, or a window whose
 *         entries come out shorter than twice the minimum entry;
 *         otherwise whatever the platform returned while being armed, with
 *         the instance left IDLE.
 * @note Returns as soon as the hardware is armed: the pulses are emitted in
 *       the background. Completion is reported as
 *       PULSE_GENERATOR_EVENT_COMPLETE, up to one window after the last
 *       period ends, i.e. after the low half that follows the last pulse.
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
 *         about the pulse count.
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
 * @param config Queue storage and low watermark.
 *               See pulse_generator_scheduled_config_t. Not retained, but
 *               config->queue is used in place.
 * @return PULSE_GENERATOR_OK if the instance is now ARMED with an empty queue;
 *         PULSE_GENERATOR_ERROR_INVALID_PARAM on a null instance, config or
 *         queue, a capacity below 2, a low watermark not below the capacity,
 *         or the buffer, layout and window errors of
 *         pulse_generator_start_fixed_count();
 *         PULSE_GENERATOR_ERROR_INVALID_STATE if the instance is not IDLE;
 *         otherwise whatever the platform returned when asked for its entry
 *         layout.
 * @note Reads the platform's timing here rather than at start (tick rate,
 *       period maximum and entry layout), so that
 *       pulse_generator_queue_events() can validate intervals against them.
 *       Not for interrupt context.
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
 *                  first pulse of a movement is measured from its start,
 *                  see pulse_generator_start_scheduled()). Copied into the
 *                  queue.
 * @param n         Number of elements in intervals.
 * @return How many events were accepted, taken from the front of intervals.
 *         Fewer than n means that either the queue filled up or
 *         intervals[returned value] was rejected, and nothing after it was
 *         taken; pulse_generator_get_free_space() tells the two apart.
 *         0 on a null argument or if the instance is not prepared for
 *         SCHEDULED mode.
 * @note An interval is rejected if it is shorter than the minimum entry or,
 *       with PULSE_GENERATOR_PULSE_SHAPE_FIXED_WIDTH, not longer than the
 *       pulse width. There is no upper limit: long intervals are split into
 *       several entries.
 * @note Compute intervals from rounded absolute instants rather than by
 *       rounding each one on its own, or the rounding errors add up along
 *       the sequence. The library executes the integers it is given as-is.
 * @note Single producer: call it from one context only (the main loop, or an
 *       interrupt such as the event callback), never from both. It is one of
 *       the functions the event callback may call, see
 *       pulse_generator_event_cb_t.
 * @note Stop queueing once the movement ends (PULSE_GENERATOR_EVENT_COMPLETE,
 *       PULSE_GENERATOR_EVENT_UNDERRUN, or after pulse_generator_stop()),
 *       and recompute from the next start. An event queued while the
 *       library is flushing the queue can outlive the flush, and would then
 *       open the next movement.
 */
size_t pulse_generator_queue_events(
    pulse_generator_t *pg,
    const uint32_t *intervals,
    size_t n);

/**
 * @brief Start emitting the events queued in an ARMED instance.
 * @param pg Instance.
 * @return PULSE_GENERATOR_OK if the instance is now RUNNING;
 *         PULSE_GENERATOR_ERROR_INVALID_STATE if the instance is not ARMED,
 *         or if fewer than two events are queued (one is enough after
 *         pulse_generator_finish_scheduled()), since a pulse needs the next
 *         one's interval to place its fall;
 *         PULSE_GENERATOR_ERROR_INVALID_PARAM on a null instance;
 *         otherwise whatever the platform returned while being armed.
 *         On any failure the instance stays ARMED with its queue untouched.
 * @note Returns as soon as the hardware is armed. The movement ends with
 *       PULSE_GENERATOR_EVENT_COMPLETE or PULSE_GENERATOR_EVENT_UNDERRUN,
 *       back in ARMED, ready for the next start.
 * @note The movement starts at the call: the first rise comes the first
 *       interval after it, plus one priming period without a pulse (see
 *       pulse_generator_ops_t::stream_start). The whole buffer is filled
 *       from the queue before the stream starts, and from then on each
 *       refill takes events up to one window ahead of the pin. When
 *       streaming, queue at least a window's worth of events before
 *       starting and keep the queue that far ahead: a queue found empty
 *       without a finish request ends the movement as an underrun.
 */
pulse_generator_status_t pulse_generator_start_scheduled(pulse_generator_t *pg);

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
 *       underrun. That happens at the refill that needs the next event, up
 *       to one window before the pin would reach it.
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
 * @brief Report how many SCHEDULED movements have ended in an underrun.
 * @param pg Instance.
 * @return The count since the instance was last prepared; 0 if pg is NULL.
 * @note A diagnostic. Reset by pulse_generator_prepare_scheduled(), not by
 *       a restart.
 */
uint32_t pulse_generator_get_underrun_count(const pulse_generator_t *pg);

/**
 * @brief Stop the movement in progress immediately. The output is forced
 *        low, and the entries left in the buffer are discarded.
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
 *         last reset, 0 if pg is NULL. A pulse counts once it has fallen.
 * @note Reads 0 again once a movement finishes or is stopped: it counts the
 *       movement in progress, not a lifetime total.
 * @note The count is worked out from the stream's position, so it may lag
 *       the pin by up to one entry.
 */
uint32_t pulse_generator_get_pulse_count(const pulse_generator_t *pg);

/**
 * @brief Reset the pulse counter of the movement in progress to zero.
 * @param pg Instance.
 * @note For a FIXED_COUNT movement this also pushes back the point at which
 *       it stops itself, since the target is counted from the counter.
 * @note A reset that comes once the end of the movement
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
 *         of range frequency, with the previous frequency kept.
 * @note Touches no hardware, and is cheap enough to call from a control
 *       loop on every cycle.
 * @note The frequency is only stored, and the source takes it at the start
 *       of the next period it writes into the buffer; a period already
 *       begun is always finished at the old frequency. The change therefore
 *       reaches the pin within one window, plus, when the period being
 *       written then is split over several entries, whatever of it remains.
 *       Of several calls made before then, the last one wins. The average
 *       frequency is exact from the change on.
 */
pulse_generator_status_t pulse_generator_set_frequency(pulse_generator_t *pg, uint32_t frequency_hz);

/**
 * @brief Report that the DMA stream has transferred the first half of the
 *        buffer, from the integrator's half-transfer ISR.
 * @param pg Instance owning the stream.
 * @return PULSE_GENERATOR_OK normally;
 *         PULSE_GENERATOR_ERROR_UNDERRUN if the movement has just stopped
 *         as an underrun, mirroring PULSE_GENERATOR_EVENT_UNDERRUN: either
 *         the interrupt was serviced so late that the stream was already
 *         reading the half to be refilled (nothing was written), or a
 *         SCHEDULED queue ran dry without pulse_generator_finish_scheduled()
 *         and its last period has finished playing;
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
