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
    PULSE_GENERATOR_ERROR_NOT_SUPPORTED, /* the platform leaves the hook this operation needs unimplemented */
} pulse_generator_status_t;

/* --- Modes and state --- */

/**
 * @brief Pulse pattern an instance is currently configured to generate.
 *
 * Every mode is driven by the same hardware mechanism, an Output Compare
 * channel in toggle mode; they differ only in how the instants of the
 * toggles are decided.
 */
typedef enum {
    PULSE_GENERATOR_MODE_FIXED_COUNT, /* N pulses at a fixed frequency, auto-stop */
    PULSE_GENERATOR_MODE_CONTINUOUS,  /* fixed frequency, runs until stop() */
} pulse_generator_mode_t;

/**
 * @brief Coarse-grained lifecycle state of an instance.
 */
typedef enum {
    PULSE_GENERATOR_STATE_IDLE,    /* no movement in progress */
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
    /** A FIXED_COUNT movement reached its target and stopped itself. The
        instance is already IDLE by the time the callback runs. */
    PULSE_GENERATOR_EVENT_COMPLETE,
    /** A compare match was serviced so late that the counter had already
        passed the next one, which was therefore rescheduled from the current
        counter value: one edge comes late, the output keeps running. Also
        reported as the return value of
        pulse_generator_notify_compare_match() — the same information by two
        routes, so an integrator can use whichever fits better. */
    PULSE_GENERATOR_EVENT_MISSED_COMPARE,
} pulse_generator_event_t;

/**
 * @brief Signature for the event notification callback.
 * @param pg       Instance reporting the event.
 * @param event    What happened.
 * @param user_ctx Opaque pointer supplied in pulse_generator_config_t,
 *                 passed back unchanged.
 * @warning May be invoked from interrupt context, e.g. from within
 *          pulse_generator_notify_compare_match(). Must be short,
 *          non-blocking, and must not call any pulse_generator_* function
 *          on the same instance from within it.
 */
typedef void (*pulse_generator_event_cb_t)(pulse_generator_t *pg,
                                           pulse_generator_event_t event,
                                           void *user_ctx);

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
     * @note Mandatory hook. Must NOT reset the counter: the library computed
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
     * @note Mandatory hook. Must not stop the counter itself while other
     *       channels sharing it are still active.
     */
    pulse_generator_status_t (*channel_stop)(void *hw);

    /**
     * @brief Write the channel's compare register.
     * @param hw      Opaque per-output handle.
     * @param compare Absolute counter value of the next pin toggle, already
     *                wrapped to the counter's width by the library.
     * @return PULSE_GENERATOR_OK on success, an error code otherwise.
     * @note Mandatory hook. A plain register write is all this is meant to
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
     * @note Mandatory hook. Also called from interrupt context: the library
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
     *         itself. Used to convert a requested pulse frequency into a
     *         half period in timer ticks.
     * @note Mandatory hook. Read once per movement, when it is started, and
     *       never from interrupt context, so it may be computed rather than
     *       cached. Must not change while a movement is in progress.
     */
    uint32_t (*get_tick_hz)(void *hw);

    /**
     * @brief Report the counter's maximum value.
     * @param hw Opaque per-output handle.
     * @return The largest value the counter reaches before wrapping, e.g.
     *         0xFFFF for a 16-bit timer or 0xFFFFFFFF for a 32-bit one.
     * @note Mandatory hook. The library uses this for two things: as the
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
     * @brief Start an asynchronous DMA burst over the given buffer of
     *        compare values.
     * @param hw     Opaque per-output handle.
     * @param buffer Precomputed values. Must remain valid and unmodified for
     *               the entire duration of the transfer — the platform does
     *               not copy it. Ownership stays with the caller.
     * @param len    Number of elements in buffer.
     * @return PULSE_GENERATOR_OK on success, an error code if the transfer
     *         could not be started (e.g. DMA channel already busy).
     * @note Optional hook: leave it NULL on a platform without DMA and the
     *       operations needing it return
     *       PULSE_GENERATOR_ERROR_NOT_SUPPORTED. Completion must be signaled
     *       by the integrator calling pulse_generator_notify_dma_complete()
     *       from their own DMA-complete ISR — this hook does not block until
     *       the transfer finishes.
     */
    pulse_generator_status_t (*dma_start)(void *hw, const uint32_t *buffer, size_t len);

    /**
     * @brief Cancel an in-progress DMA burst started by dma_start.
     * @param hw Opaque per-output handle.
     * @return PULSE_GENERATOR_OK on success, an error code otherwise.
     * @note Optional hook, see dma_start.
     */
    pulse_generator_status_t (*dma_stop)(void *hw);
} pulse_generator_ops_t;

/* --- Configuration --- */

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
} pulse_generator_config_t;

/* --- Instance handle --- */

/**
 * @brief A single pulse generator instance (one per axis/output).
 *
 * Fully defined here rather than opaque, so callers can allocate it
 * statically (no heap) and multi-instance is just "one variable per axis".
 * All fields below are private: interact with an instance only through the
 * pulse_generator_* functions, never by reading or writing these fields
 * directly.
 *
 * Fields shared between the compare ISR and ordinary code are volatile. That
 * buys visibility, not atomicity, which is enough here: each is a 32-bit
 * aligned word with a single writer at any given time.
 */
struct pulse_generator_s {
    const pulse_generator_ops_t *ops;
    void                        *hw;
    pulse_generator_event_cb_t   on_event;
    void                        *user_ctx;

    volatile pulse_generator_state_t state;
    pulse_generator_mode_t           mode;          /* meaningful only while state == RUNNING */

    /* Captured from the platform when a movement starts and constant while
       it runs, so the compare ISR and set_frequency() need no hook call to
       read them back. */
    uint32_t tick_hz;
    uint32_t counter_max;

    volatile uint32_t half_period_ticks; /* ticks between toggles; meaningful only while RUNNING */
    volatile uint32_t last_compare;      /* absolute counter value of the compare match currently
                                              armed, the reference the next one is measured from */
    volatile uint32_t edge_count;        /* pin toggles observed via notify_compare_match() during
                                              the movement in progress; two edges = one full pulse;
                                              always 0 while IDLE */
    uint32_t target_edge_count;          /* edges after which a FIXED_COUNT movement stops itself;
                                              0 means no target, i.e. a CONTINUOUS movement */
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
 *         config->ops is NULL, or if any mandatory hook in it is NULL.
 * @note Calls no hook, so it imposes no ordering against the integrator's
 *       clock and peripheral setup. The platform is first touched when a
 *       movement is started.
 */
pulse_generator_status_t pulse_generator_init(
    pulse_generator_t *pg,
    const pulse_generator_config_t *config);

/**
 * @brief Start a movement of exactly pulse_count pulses at a fixed
 *        frequency, stopping itself when the last one has been emitted.
 * @param pg          Instance.
 * @param frequency_hz Pulse frequency in Hz. Must map to a half period
 *                    between 1 tick and the counter's maximum, given the
 *                    platform's tick rate.
 * @param pulse_count Number of pulses to emit. Must be between 1 and
 *                    UINT32_MAX / 2 (each pulse is two compare matches).
 * @return PULSE_GENERATOR_OK if the movement was started;
 *         PULSE_GENERATOR_ERROR_INVALID_STATE if a movement is already in
 *         progress;
 *         PULSE_GENERATOR_ERROR_INVALID_PARAM on a null instance, an out of
 *         range frequency or pulse count, or a platform reporting a counter
 *         maximum that is not of the form 2^n - 1;
 *         otherwise whatever the platform returned while being armed, with
 *         the instance left IDLE.
 * @note Returns as soon as the hardware is armed: the pulses are emitted in
 *       the background. Completion is reported as
 *       PULSE_GENERATOR_EVENT_COMPLETE.
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

/**
 * @brief Start a variable-frequency movement over a precomputed buffer.
 * @param pg        Instance.
 * @param intervals Precomputed buffer. Ownership and lifetime stay with the
 *                  caller, which must keep it unmodified until the transfer
 *                  ends.
 * @param len       Number of elements in intervals.
 * @return PULSE_GENERATOR_ERROR_NOT_SUPPORTED: always.
 * @deprecated A placeholder with no implementation behind it. The scheduled
 *             mode (M8) replaces it with a streaming event queue, of which a
 *             precomputed buffer is just the case where the queue is filled
 *             once, so this signature will be removed rather than
 *             implemented. Callers must not depend on it.
 */
pulse_generator_status_t pulse_generator_start_profile(
    pulse_generator_t *pg,
    const uint32_t *intervals,
    size_t len);

/**
 * @brief Stop the movement in progress immediately, leaving the output at
 *        whatever level it currently holds.
 * @param pg Instance.
 * @return PULSE_GENERATOR_OK, including when already idle (this is
 *         idempotent); PULSE_GENERATOR_ERROR_INVALID_PARAM on a null
 *         instance; otherwise whatever the platform returned while being
 *         disarmed, with the instance left IDLE regardless.
 * @note Resets the pulse counter and does not report
 *       PULSE_GENERATOR_EVENT_COMPLETE: that event means "the requested
 *       pulses were emitted", which an explicit stop by definition is not.
 * @note A compare interrupt already in flight can still write the compare
 *       register just after this returns. That is harmless — the channel is
 *       disabled, so the match drives nothing, and the next start overwrites
 *       the register — but an integrator who wants the sequence airtight
 *       should call this with the compare interrupt masked.
 */
pulse_generator_status_t pulse_generator_stop(pulse_generator_t *pg);

/**
 * @brief Report whether a movement is currently in progress.
 * @param pg Instance.
 * @return true while a movement is running, false when idle or pg is NULL.
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
 */
uint32_t pulse_generator_get_pulse_count(const pulse_generator_t *pg);

/**
 * @brief Reset the pulse counter of the movement in progress to zero.
 * @param pg Instance.
 * @note For a FIXED_COUNT movement this also pushes back the point at which
 *       it stops itself, since the target is counted from the counter.
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
 *         the movement keeps running;
 *         PULSE_GENERATOR_ERROR_INVALID_PARAM on a null instance;
 *         otherwise whatever the platform returned.
 * @note Counts the edge, schedules the next one and, for a FIXED_COUNT
 *       movement that has reached its target, stops the channel and reports
 *       PULSE_GENERATOR_EVENT_COMPLETE.
 * @note A no-op when no timer-driven movement is in progress, so a late or
 *       spurious interrupt after a stop is harmless.
 */
pulse_generator_status_t pulse_generator_notify_compare_match(pulse_generator_t *pg);

/**
 * @brief Report the end of a DMA burst to the library, from the
 *        integrator's DMA ISR.
 * @param pg Instance.
 * @return PULSE_GENERATOR_ERROR_NOT_SUPPORTED: always.
 * @deprecated Counterpart of pulse_generator_start_profile(), and removed
 *             with it when the scheduled mode arrives: a streaming queue
 *             needs to hear about half transfers too, so the notification
 *             is reworked rather than kept.
 */
pulse_generator_status_t pulse_generator_notify_dma_complete(pulse_generator_t *pg);

#ifdef __cplusplus
}
#endif

#endif /* PULSE_GENERATOR_H */
