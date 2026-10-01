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
 *        and by pulse_generator_platform_t hooks.
 */
typedef enum {
    PULSE_GENERATOR_OK = 0,
    PULSE_GENERATOR_ERROR,               /* generic/unspecified hardware fault */
    PULSE_GENERATOR_ERROR_BUSY,          /* peripheral already running */
    PULSE_GENERATOR_ERROR_INVALID_PARAM, /* null pointer, bad config, etc. */
    PULSE_GENERATOR_ERROR_INVALID_STATE, /* operation not valid in current mode/state */
    PULSE_GENERATOR_ERROR_MISSED_COMPARE, /* a compare match was serviced too late and had to be rescheduled */
} pulse_generator_status_t;

/* --- Modes and state --- */

/**
 * @brief Pulse pattern and hardware backend an instance is currently
 *        configured to generate.
 *
 * Flattened on purpose instead of two orthogonal pattern/backend enums:
 * DMA_PROFILE has no bit-bang variant, so that invalid combination simply
 * has no value here to represent it, instead of being rejected at runtime.
 */
typedef enum {
    PULSE_GENERATOR_MODE_FIXED_COUNT_TIMER,   /* Output Compare, N pulses, auto-stop */
    PULSE_GENERATOR_MODE_FIXED_COUNT_BITBANG, /* GPIO toggling, N pulses, auto-stop */
    PULSE_GENERATOR_MODE_CONTINUOUS_TIMER,    /* Output Compare, runs until stop() */
    PULSE_GENERATOR_MODE_CONTINUOUS_BITBANG,  /* GPIO toggling, runs until stop() */
    PULSE_GENERATOR_MODE_DMA_PROFILE,         /* DMA burst over a precomputed buffer */
} pulse_generator_mode_t;

/**
 * @brief Coarse-grained lifecycle state of an instance.
 */
typedef enum {
    PULSE_GENERATOR_STATE_IDLE,    /* no movement in progress */
    PULSE_GENERATOR_STATE_RUNNING, /* a movement is currently in progress */
} pulse_generator_state_t;

/**
 * @brief Hardware mechanism used to drive a FIXED_COUNT or CONTINUOUS
 *        movement. Not applicable to DMA_PROFILE, which is always
 *        timer+DMA driven.
 */
typedef enum {
    PULSE_GENERATOR_BACKEND_TIMER,   /* Output Compare timer */
    PULSE_GENERATOR_BACKEND_BITBANG, /* manual GPIO toggling, driven by pulse_generator_tick() */
} pulse_generator_backend_t;

/**
 * @brief Signature for the end-of-movement notification callback.
 * @param user_ctx Opaque pointer supplied at
 *                 pulse_generator_set_complete_callback() time, passed
 *                 back unchanged.
 * @warning May be invoked from interrupt context (e.g. right after a DMA
 *          completion notification, or from within pulse_generator_tick()
 *          if that is itself called from an ISR). Must be short,
 *          non-blocking, and must not call any pulse_generator_* function
 *          on the same instance from within it.
 */
typedef void (*pulse_generator_complete_cb_t)(void *user_ctx);

/* --- Platform abstraction --- */

/**
 * @brief Function-pointer table an integrator implements to bind the
 *        library to real hardware.
 *
 * The library never includes vendor HAL headers or touches registers
 * directly — every hardware access goes through this table. Each
 * pulse_generator_t instance owns its own platform, which is what makes
 * multi-instance (multiple axes) possible: every hook below receives the
 * ctx belonging to that specific instance's platform, never a global.
 */
typedef struct {
    /**
     * @brief Start the Output Compare timer in free-running toggle mode:
     *        the counter runs over its full range and the first compare
     *        match is scheduled half_period_ticks after the current
     *        counter value.
     * @param ctx               Opaque per-instance context (see ctx member
     *                          below).
     * @param half_period_ticks Ticks until the first pin toggle. Never 0
     *                          and never above get_max_ticks().
     * @return PULSE_GENERATOR_OK on success, an error code if the timer
     *         could not be started (e.g. PULSE_GENERATOR_ERROR_BUSY if
     *         already running).
     */
    pulse_generator_status_t (*timer_start)(void *ctx, uint32_t half_period_ticks);

    /**
     * @brief Stop the Output Compare timer immediately. No further pulses
     *        are generated until timer_start is called again.
     * @param ctx Opaque per-instance context.
     * @return PULSE_GENERATOR_OK on success, an error code otherwise.
     */
    pulse_generator_status_t (*timer_stop)(void *ctx);

    /**
     * @brief Schedule the next compare match half_period_ticks after the
     *        previous one (CCR += half_period_ticks, wrapping at the
     *        counter's width).
     * @param ctx               Opaque per-instance context.
     * @param half_period_ticks Ticks until the next pin toggle, measured
     *                          from the previous compare match rather than
     *                          from now, so interrupt latency does not
     *                          accumulate as drift. Same range as in
     *                          timer_start.
     * @return PULSE_GENERATOR_OK on success;
     *         PULSE_GENERATOR_ERROR_MISSED_COMPARE if the counter had
     *         already passed the new compare value when it was written
     *         (the interrupt was serviced more than half_period_ticks
     *         late). In that case the platform must reschedule the match
     *         half_period_ticks after the current counter value, so the
     *         output resumes after one late edge instead of after a full
     *         counter wrap-around.
     * @note Called by the library from pulse_generator_notify_compare_match(),
     *       i.e. from interrupt context: must be short and non-blocking.
     */
    pulse_generator_status_t (*advance_compare)(void *ctx, uint32_t half_period_ticks);

    /**
     * @brief Start an asynchronous DMA burst over the given interval
     *        buffer.
     * @param ctx    Opaque per-instance context.
     * @param buffer Precomputed interval values. Must remain valid and
     *               unmodified for the entire duration of the transfer —
     *               the platform does not copy it. Ownership stays with
     *               the caller.
     * @param len    Number of elements in buffer.
     * @return PULSE_GENERATOR_OK on success, an error code if the
     *         transfer could not be started (e.g. DMA channel already
     *         busy).
     * @note Completion must be signaled by the integrator calling
     *       pulse_generator_notify_dma_complete() from their own
     *       DMA-complete ISR — this function does not block until the
     *       transfer finishes.
     */
    pulse_generator_status_t (*dma_start)(void *ctx, const uint32_t *buffer, size_t len);

    /**
     * @brief Cancel an in-progress DMA burst started by dma_start.
     * @param ctx Opaque per-instance context.
     * @return PULSE_GENERATOR_OK on success, an error code otherwise.
     */
    pulse_generator_status_t (*dma_stop)(void *ctx);

    /**
     * @brief Drive the pulse output pin high. Only used by the bit-bang
     *        backend, invoked from pulse_generator_tick().
     * @param ctx Opaque per-instance context.
     * @return PULSE_GENERATOR_OK on success, an error code otherwise.
     */
    pulse_generator_status_t (*gpio_set)(void *ctx);

    /**
     * @brief Drive the pulse output pin low. Only used by the bit-bang
     *        backend, invoked from pulse_generator_tick().
     * @param ctx Opaque per-instance context.
     * @return PULSE_GENERATOR_OK on success, an error code otherwise.
     */
    pulse_generator_status_t (*gpio_clear)(void *ctx);

    /**
     * @brief Report the Output Compare timer's counting frequency, in Hz.
     * @param ctx Opaque per-instance context.
     * @return The timer's tick rate: how many times per second its counter
     *         increments, i.e. the frequency already divided by the
     *         prescaler. NOT the raw peripheral/bus clock (e.g. PCLK1) —
     *         the library has no way to apply a prescaler on top of that
     *         itself, so this must already be the post-prescaler counting
     *         frequency. Used to convert a requested pulse frequency into
     *         a half period in timer ticks.
     */
    uint32_t (*get_timer_main_clk)(void *ctx);

    /**
     * @brief Report the largest half_period_ticks the timer can schedule.
     * @param ctx Opaque per-instance context.
     * @return The counter's maximum value, e.g. 0xFFFF for a 16-bit timer
     *         or 0xFFFFFFFF for a 32-bit one. Used to reject frequencies
     *         too low to fit in the timer's range.
     */
    uint32_t (*get_max_ticks)(void *ctx);

    /**
     * @brief Opaque pointer passed unchanged to every hook above. Typically
     *        holds whatever the integrator's HAL calls need to identify
     *        the concrete peripheral (timer handle, DMA channel, GPIO
     *        pin), since the library itself never touches vendor types.
     */
    void *ctx;
} pulse_generator_platform_t;

/* --- Instance handle --- */

/**
 * @brief A single pulse generator instance (one per axis/output).
 *
 * Fully defined here rather than opaque, so callers can allocate it
 * statically (no heap) and multi-instance is just "one variable per
 * axis". All fields below are private: interact with an instance only
 * through the pulse_generator_* functions, never by reading or writing
 * these fields directly.
 */
typedef struct {
    const pulse_generator_platform_t *platform;

    pulse_generator_state_t state;
    pulse_generator_mode_t  mode;                   /* meaningful only while state == RUNNING */
    uint32_t                half_period_ticks;      /* ticks between toggles, passed to advance_compare()
                                                         on every compare match; meaningful only while
                                                         state == RUNNING */
    uint32_t                toggle_count;           /* raw pin toggles observed via notify_compare_match()
                                                         during the movement in progress; two toggles =
                                                         one full pulse; always 0 while IDLE */
    uint32_t                target_pulse_count;     /* only meaningful for FIXED_COUNT modes */

    pulse_generator_complete_cb_t complete_cb;
    void                          *complete_cb_user_ctx;
} pulse_generator_t;

/* --- Public API --- */

/**
 * @brief Initialize an instance with the given platform.
 * @param pg       Instance to initialize.
 * @param platform Platform implementation this instance will use for its
 *                 entire lifetime. Must remain valid for as long as pg is
 *                 used, and must have every hook populated.
 * @return PULSE_GENERATOR_OK on success, PULSE_GENERATOR_ERROR_INVALID_PARAM
 *         if pg or platform is NULL.
 * @post pg's state is PULSE_GENERATOR_STATE_IDLE and its pulse count is 0.
 */
pulse_generator_status_t pulse_generator_init(
    pulse_generator_t *pg,
    const pulse_generator_platform_t *platform);

/**
 * @brief Start a movement that generates exactly pulse_count pulses at
 *        frequency_hz, then stops automatically.
 * @param pg           Instance to start.
 * @param backend      Hardware mechanism to use (timer or bit-bang).
 * @param frequency_hz Pulse frequency, in Hz.
 * @param pulse_count  Exact number of pulses to generate.
 * @return PULSE_GENERATOR_OK on success, PULSE_GENERATOR_ERROR_INVALID_PARAM
 *         if pg is NULL, if backend is PULSE_GENERATOR_BACKEND_BITBANG (not
 *         yet supported), if frequency_hz is 0 or out of the timer's range
 *         (see pulse_generator_set_frequency), or if pulse_count is 0;
 *         PULSE_GENERATOR_ERROR_INVALID_STATE if pg is not currently
 *         PULSE_GENERATOR_STATE_IDLE.
 * @note Non-blocking: returns immediately after arming the hardware.
 *       Completion is detected by pulse_generator_notify_compare_match() and reported via
 *       the registered complete callback (see
 *       pulse_generator_set_complete_callback), and observable through
 *       pulse_generator_is_busy() / pulse_generator_get_state().
 * @post pulse_count is reset to 0.
 */
pulse_generator_status_t pulse_generator_start_fixed_count(
    pulse_generator_t *pg,
    pulse_generator_backend_t backend,
    uint32_t frequency_hz,
    uint32_t pulse_count);

/**
 * @brief Start continuous pulse generation at frequency_hz. Runs until
 *        pulse_generator_stop() is called.
 * @param pg           Instance to start.
 * @param backend      Hardware mechanism to use (timer or bit-bang).
 * @param frequency_hz Initial pulse frequency, in Hz. May be changed while
 *                      running via pulse_generator_set_frequency().
 * @return PULSE_GENERATOR_OK on success, PULSE_GENERATOR_ERROR_INVALID_PARAM
 *         if pg is NULL, if backend is PULSE_GENERATOR_BACKEND_BITBANG (not
 *         yet supported), or if frequency_hz is 0 or out of the timer's
 *         range (see pulse_generator_set_frequency);
 *         PULSE_GENERATOR_ERROR_INVALID_STATE if pg is not currently
 *         PULSE_GENERATOR_STATE_IDLE.
 * @note Non-blocking: returns immediately after arming the hardware.
 *       Pulses are counted through pulse_generator_notify_compare_match();
 *       the complete callback is never invoked in this mode.
 * @post pulse_count is reset to 0.
 */
pulse_generator_status_t pulse_generator_start_continuous(
    pulse_generator_t *pg,
    pulse_generator_backend_t backend,
    uint32_t frequency_hz);

/**
 * @brief Start DMA-driven playback of a precomputed interval buffer.
 * @param pg        Instance to start.
 * @param intervals Precomputed interval values. Must remain valid and
 *                  unmodified until completion is signaled (see
 *                  pulse_generator_notify_dma_complete) — ownership stays
 *                  with the caller, the library does not copy it.
 * @param len       Number of elements in intervals.
 * @return PULSE_GENERATOR_OK on success, PULSE_GENERATOR_ERROR_INVALID_STATE
 *         if pg is not currently PULSE_GENERATOR_STATE_IDLE.
 * @note Non-blocking, and always uses the timer+DMA backend — there is no
 *       bit-bang variant of this mode.
 * @post pulse_count is reset to 0.
 */
pulse_generator_status_t pulse_generator_start_profile(
    pulse_generator_t *pg,
    const uint32_t *intervals,
    size_t len);

/**
 * @brief Stop the current movement immediately, regardless of mode.
 * @param pg Instance to stop.
 * @return PULSE_GENERATOR_OK on success, an error code if the underlying
 *         platform call failed.
 * @note Idempotent: calling this while already PULSE_GENERATOR_STATE_IDLE
 *       is a harmless no-op that returns PULSE_GENERATOR_OK.
 * @note Also resets the pulse counter to 0 when transitioning to IDLE.
 */
pulse_generator_status_t pulse_generator_stop(pulse_generator_t *pg);

/**
 * @brief Query whether a movement is currently in progress.
 * @param pg Instance to query.
 * @return true if pg's state is PULSE_GENERATOR_STATE_RUNNING, false
 *         otherwise. Shorthand for
 *         pulse_generator_get_state(pg) == PULSE_GENERATOR_STATE_RUNNING.
 */
bool pulse_generator_is_busy(const pulse_generator_t *pg);

/**
 * @brief Query the current lifecycle state of an instance.
 * @param pg Instance to query.
 * @return Current state.
 */
pulse_generator_state_t pulse_generator_get_state(const pulse_generator_t *pg);

/**
 * @brief Query the number of pulses generated so far.
 * @param pg Instance to query.
 * @return Number of full pulses generated during the movement currently
 *         in progress. Always 0 while PULSE_GENERATOR_STATE_IDLE (a
 *         movement resets it to 0 on start, and it returns to 0 again as
 *         soon as it finishes or is stopped — read it before calling
 *         pulse_generator_stop() if you need the final count).
 * @note In continuous modes the count wraps around to 0 after 2^31 pulses
 *       (the underlying toggle counter is 32-bit).
 */
uint32_t pulse_generator_get_pulse_count(const pulse_generator_t *pg);

/**
 * @brief Reset the pulse counter to 0.
 * @param pg Instance to reset.
 * @note May be called regardless of state. Purely internal bookkeeping —
 *       cannot fail, hence no status return.
 */
void pulse_generator_reset_pulse_count(pulse_generator_t *pg);

/**
 * @brief Update the frequency of an already-running continuous movement,
 *        without stopping it or losing the accumulated pulse count.
 * @param pg           Instance to update.
 * @param frequency_hz New pulse frequency, in Hz.
 * @return PULSE_GENERATOR_OK on success, PULSE_GENERATOR_ERROR_INVALID_PARAM
 *         if pg is NULL, if frequency_hz is 0, or if it is out of the
 *         timer's range: its half period, timer_main_clk / (2 *
 *         frequency_hz), must be at least 1 tick (frequency too high) and
 *         at most get_max_ticks() (frequency too low);
 *         PULSE_GENERATOR_ERROR_INVALID_STATE if pg is not currently
 *         running in PULSE_GENERATOR_MODE_CONTINUOUS_TIMER or
 *         PULSE_GENERATOR_MODE_CONTINUOUS_BITBANG. On any error the
 *         previous frequency stays in effect.
 * @note Does not touch the hardware: it only updates the half period the
 *       library passes to advance_compare(), so the new frequency takes
 *       effect from the next pin toggle on. The toggle already scheduled
 *       keeps the old timing, hence no truncated or stretched half period.
 */
pulse_generator_status_t pulse_generator_set_frequency(pulse_generator_t *pg, uint32_t frequency_hz);

/**
 * @brief Register a callback to be invoked when the current movement
 *        completes (fixed-count target reached, or profile buffer
 *        exhausted).
 * @param pg        Instance to configure.
 * @param callback  Function to invoke on completion, or NULL to unregister.
 * @param user_ctx  Opaque pointer passed back unchanged to callback.
 * @return PULSE_GENERATOR_OK on success, PULSE_GENERATOR_ERROR_INVALID_PARAM
 *         if pg is NULL.
 * @warning See pulse_generator_complete_cb_t: the callback may run from
 *          interrupt context.
 */
pulse_generator_status_t pulse_generator_set_complete_callback(
    pulse_generator_t *pg,
    pulse_generator_complete_cb_t callback,
    void *user_ctx);

/**
 * @brief Periodic driver call for bit-bang timing.
 * @param pg         Instance to service.
 * @param elapsed_us Microseconds elapsed since the previous call to this
 *                    function for this instance (approximate is fine).
 * @return PULSE_GENERATOR_OK on success, PULSE_GENERATOR_ERROR_INVALID_PARAM
 *         if pg is NULL, an error code if an underlying platform call
 *         failed.
 * @note Safe to call regardless of pg's current mode/state. Currently a
 *       no-op for every mode: bit-bang backends are not implemented yet
 *       (see PULSE_GENERATOR_BACKEND_BITBANG). The integrator is expected
 *       to call this periodically (e.g. from a general-purpose timer ISR
 *       or the main loop) so that it is already wired in once bit-bang
 *       support lands.
 */
pulse_generator_status_t pulse_generator_tick(pulse_generator_t *pg, uint32_t elapsed_us);

/**
 * @brief Notify the library that the Output Compare timer's compare-match
 *        interrupt has fired, toggling the pulse output pin.
 * @param pg Instance to notify.
 * @return PULSE_GENERATOR_OK on success, PULSE_GENERATOR_ERROR_INVALID_PARAM
 *         if pg is NULL, PULSE_GENERATOR_ERROR_MISSED_COMPARE if
 *         advance_compare() reported a late interrupt (the toggle is
 *         still counted and the movement keeps running), or another
 *         error code if an underlying platform call failed.
 * @note Must be called by the integrator from their own compare-match ISR,
 *       once per interrupt. In Output Compare Toggle mode each compare
 *       match only flips the pin, so two calls correspond to one full
 *       pulse — the caller does not need to account for this, it is
 *       handled internally.
 * @note Safe to call regardless of pg's current mode/state: it is a no-op
 *       whenever pg is not currently running in
 *       PULSE_GENERATOR_MODE_FIXED_COUNT_TIMER or
 *       PULSE_GENERATOR_MODE_CONTINUOUS_TIMER (including a late/stray
 *       call right after completion or an explicit stop()).
 * @note While running, each call schedules the next toggle through the
 *       platform's advance_compare() hook, except for the final toggle of
 *       a fixed-count movement.
 * @note In PULSE_GENERATOR_MODE_CONTINUOUS_TIMER it never stops the timer
 *       nor invokes the complete callback.
 * @note In PULSE_GENERATOR_MODE_FIXED_COUNT_TIMER, when the target pulse
 *       count is reached, this stops the timer,
 *       transitions pg to PULSE_GENERATOR_STATE_IDLE, resets the pulse
 *       counter to 0, and invokes the registered complete callback (see
 *       pulse_generator_set_complete_callback). May itself run in
 *       interrupt context.
 */
pulse_generator_status_t pulse_generator_notify_compare_match(pulse_generator_t *pg);

/**
 * @brief Notify the library that an in-progress DMA profile transfer has
 *        finished.
 * @param pg Instance to notify.
 * @return PULSE_GENERATOR_OK on success,
 *         PULSE_GENERATOR_ERROR_INVALID_STATE if pg is not currently
 *         running in PULSE_GENERATOR_MODE_DMA_PROFILE.
 * @note Must be called by the integrator from their own DMA-complete ISR
 *       (see pulse_generator_platform_t::dma_start). May itself run in
 *       interrupt context, and may in turn invoke the registered complete
 *       callback.
 */
pulse_generator_status_t pulse_generator_notify_dma_complete(pulse_generator_t *pg);

#ifdef __cplusplus
}
#endif

#endif /* PULSE_GENERATOR_H */
