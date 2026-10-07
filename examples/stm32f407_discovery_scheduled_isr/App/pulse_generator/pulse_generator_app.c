#include "pulse_generator_app.h"

#include "pulse_generator_platform.h"
#include "ramp_profile.h"

extern TIM_HandleTypeDef htim4;

#define ARRAY_LEN(a) (sizeof (a) / sizeof (a)[0])

/* Event queue storage. 63 events fit (one slot always stays empty). */
#define QUEUE_CAPACITY 64u
#define LOW_WATERMARK  16u

/* Measured on the board with the instrumentation in
   HAL_TIM_OC_DelayElapsedCallback(), Debug build (-O0): at most 4 ticks
   from a compare match to the next compare being armed. Twice that, rounded
   up, covers the 1-tick resolution of the measurement and an unlucky
   SysTick; a Release build only widens the margin. */
#define MIN_INTERVAL_TICKS 10u

/* Delay between reading the counter and the start tick: room for
   start_scheduled() to arm the channel. 1 ms at 1 MHz. */
#define START_MARGIN_TICKS 1000u

/* Phases B and C: rest -> 5 kHz -> rest, about 2 s in all. */
#define RAMP_ACCEL_HZ_S    10000u
#define RAMP_CRUISE_HZ     5000u
#define RAMP_CRUISE_PULSES 5000u

/* Intervals pulled from the ramp generator per queue_events() call. */
#define FEED_BATCH_LEN 16u

/* Phase A: abrupt changes in both directions, so every fall can be checked
   against half of the *next* interval on a logic analyzer. Ticks of 1 us. */
static const uint32_t known_sequence[] = {1000, 1000, 400, 400, 2000, 250, 250, 1500, 600, 3000};

typedef enum {
    PHASE_KNOWN_SEQUENCE, /* A: precomputed, queued in one go */
    PHASE_RAMP,           /* B: streamed from the main loop */
    PHASE_RAMP_UNDERRUN,  /* C: as B, but the producer stalls mid-cruise */
    PHASE_COUNT,
} demo_phase_t;

/* Idle time before each phase starts, so the phases are easy to tell apart
   on a logic analyzer. */
static const uint32_t pause_before_ms[PHASE_COUNT] = {
    [PHASE_KNOWN_SEQUENCE] = 2000u,
    [PHASE_RAMP]           = 1000u,
    [PHASE_RAMP_UNDERRUN]  = 1000u,
};

typedef enum {
    DEMO_PAUSED,
    DEMO_RUNNING,
} demo_state_t;

static stm32f4_pg_hw_t   pg_hw;
static pulse_generator_t pg;
static uint32_t          queue[QUEUE_CAPACITY];

static bool           demo_ready;
static demo_state_t   demo_state;
static demo_phase_t   demo_phase;
static uint32_t       pause_start_ms;
static ramp_profile_t ramp;
static uint32_t       ramp_fed;        /* intervals handed to the library so far */
static uint32_t       ramp_feed_limit; /* the producer hands out no more than this */
static bool           ramp_finished;   /* finish_scheduled() already called */

/* Diagnostics for the debugger. The failure counters should stay 0. */
static volatile uint32_t init_failure_count;
static volatile uint32_t rejected_interval_count;
static volatile uint32_t start_failure_count;
static volatile uint32_t complete_count;
static volatile uint32_t underrun_count;
static volatile uint32_t low_watermark_count;

/* ISR cost, for sizing MIN_INTERVAL_TICKS:
   - notify_max_cycles: notify_compare_match() alone, in CPU cycles;
   - match_to_done_max_ticks: from the compare match to the end of the
     library's work, in timer ticks. Interrupt entry, HAL dispatch and the
     library together: the shortest half pulse the engine can honour. */
static volatile uint32_t notify_max_cycles;
static volatile uint32_t match_to_done_max_ticks;

/* Runs from the compare ISR: counts, and drives the analyzer markers so
   that they land exactly where the library reports the event. */
static void on_pulse_generator_event(pulse_generator_t *instance,
                                     pulse_generator_event_t event,
                                     void *user_ctx)
{
    (void)instance;
    (void)user_ctx;

    switch (event) {
        case PULSE_GENERATOR_EVENT_COMPLETE:
            complete_count++;
            HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_RESET);
            break;
        case PULSE_GENERATOR_EVENT_UNDERRUN:
            underrun_count++;
            HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_RESET);
            break;
        case PULSE_GENERATOR_EVENT_LOW_WATERMARK:
            low_watermark_count++;
            HAL_GPIO_TogglePin(LD5_GPIO_Port, LD5_Pin);
            break;
        default:
            break;
    }
}

/* Ends the movement, if any, and flushes the queue. In ARMED this touches
   no hardware; it also clears an event queued just as the movement ended,
   which would otherwise open the next one. */
static void abort_movement(void)
{
    pulse_generator_stop(&pg);
    HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_RESET);
}

/* Raises the phase marker (PD13) and starts whatever has been queued. */
static bool start_movement(void)
{
    HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_SET);

    uint32_t start_tick = pulse_generator_get_now_ticks(&pg) + START_MARGIN_TICKS;
    if (pulse_generator_start_scheduled(&pg, start_tick) != PULSE_GENERATOR_OK) {
        start_failure_count++;
        return false;
    }

    return true;
}

/* Phase A: the whole sequence fits in the queue, so it is queued, finished
   and started in one go — the precomputed case. */
static bool start_known_sequence(void)
{
    if (pulse_generator_queue_events(&pg, known_sequence, ARRAY_LEN(known_sequence))
            != ARRAY_LEN(known_sequence)) {
        rejected_interval_count++;
        return false;
    }

    pulse_generator_finish_scheduled(&pg);
    return start_movement();
}

/* Tops the queue up from the ramp generator, up to the feed limit, and
   declares the end of the sequence once the whole profile has been queued.
   With a limit short of the whole profile it never does: the producer just
   stops, as one that falls behind would.
   @return false if the library rejected an interval. */
static bool feed_ramp(void)
{
    uint32_t batch[FEED_BATCH_LEN];

    while (ramp_fed < ramp_feed_limit) {
        size_t free_space = pulse_generator_get_free_space(&pg);
        if (free_space == 0) {
            break;
        }

        size_t wanted = (free_space < FEED_BATCH_LEN) ? free_space : FEED_BATCH_LEN;
        if (wanted > ramp_feed_limit - ramp_fed) {
            wanted = ramp_feed_limit - ramp_fed;
        }

        /* The limit never exceeds the profile, so the generator always has
           this many left. */
        for (size_t n = 0; n < wanted; n++) {
            ramp_profile_next(&ramp, &batch[n]);
        }

        /* Never asked for more than fits, and this is the only producer, so
           a short count can only mean a rejected interval. */
        if (pulse_generator_queue_events(&pg, batch, wanted) != wanted) {
            rejected_interval_count++;
            return false;
        }
        ramp_fed += wanted;
    }

    if (!ramp_finished && ramp_fed == ramp_profile_total_pulses(&ramp)) {
        pulse_generator_finish_scheduled(&pg);
        ramp_finished = true;
    }

    return true;
}

/* Phases B and C: the queue is filled before the start and then kept topped
   up from the main loop while the movement runs — the streaming case. In
   phase C the producer stops at the middle of the profile, which is the
   middle of the cruise since the profile is symmetric, and never finishes. */
static bool start_ramp(bool stall_mid_cruise)
{
    if (!ramp_profile_init(&ramp, pg_hw.tick_hz, RAMP_ACCEL_HZ_S, RAMP_CRUISE_HZ, RAMP_CRUISE_PULSES)) {
        start_failure_count++;
        return false;
    }

    uint32_t total = ramp_profile_total_pulses(&ramp);
    ramp_feed_limit = stall_mid_cruise ? total / 2u : total;
    ramp_fed = 0;
    ramp_finished = false;

    return feed_ramp() && start_movement();
}

static bool start_phase(demo_phase_t phase)
{
    switch (phase) {
        case PHASE_KNOWN_SEQUENCE: return start_known_sequence();
        case PHASE_RAMP:           return start_ramp(false);
        case PHASE_RAMP_UNDERRUN:  return start_ramp(true);
        default:                   return false;
    }
}

/* DWT cycle counter, used to time the compare ISR. */
static void cycle_counter_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

void pulse_generator_app_init(void)
{
    cycle_counter_init();

    /* The board wiring lives here, not in the platform: TIM4_CH1 is PD12 on
       the Discovery. */
    if (stm32f4_pg_hw_init(&pg_hw, &htim4, TIM_CHANNEL_1) != PULSE_GENERATOR_OK) {
        init_failure_count++;
        return;
    }

    if (pulse_generator_init(&pg, &(pulse_generator_config_t){
            .ops = &g_stm32f4_pg_ops,
            .hw = &pg_hw,
            .on_event = on_pulse_generator_event,
        }) != PULSE_GENERATOR_OK) {
        init_failure_count++;
        return;
    }

    if (pulse_generator_prepare_scheduled(&pg, &(pulse_generator_scheduled_config_t){
            .queue = queue,
            .queue_capacity = QUEUE_CAPACITY,
            .low_watermark = LOW_WATERMARK,
            .min_interval_ticks = MIN_INTERVAL_TICKS,
            .engine = PULSE_GENERATOR_ENGINE_ISR,
        }) != PULSE_GENERATOR_OK) {
        init_failure_count++;
        return;
    }

    demo_ready = true;
    demo_state = DEMO_PAUSED;
    demo_phase = PHASE_KNOWN_SEQUENCE;
    pause_start_ms = HAL_GetTick();
}

void pulse_generator_app_process(void)
{
    if (!demo_ready) {
        return;
    }

    switch (demo_state) {
        case DEMO_PAUSED:
            /* Unsigned subtraction keeps working across the 32-bit wrap of
               the millisecond tick. */
            if (HAL_GetTick() - pause_start_ms < pause_before_ms[demo_phase]) {
                return;
            }
            if (start_phase(demo_phase)) {
                demo_state = DEMO_RUNNING;
            } else {
                /* Same phase again after its pause, from a clean queue. */
                abort_movement();
                pause_start_ms = HAL_GetTick();
            }
            break;

        case DEMO_RUNNING:
            /* RUNNING is published inside start_scheduled(), and the ISR
               takes the instance back to ARMED when the movement ends. */
            if (pulse_generator_is_busy(&pg)) {
                if (demo_phase != PHASE_KNOWN_SEQUENCE && !feed_ramp()) {
                    abort_movement();
                }
                return;
            }
            abort_movement();
            demo_phase = (demo_phase_t)((demo_phase + 1) % PHASE_COUNT);
            demo_state = DEMO_PAUSED;
            pause_start_ms = HAL_GetTick();
            break;
    }
}

void HAL_TIM_OC_DelayElapsedCallback(TIM_HandleTypeDef *htim)
{
    /* Filtered by channel as well as by timer: several instances can share
       one timer, each owning its own channel. HAL_TIM_IRQHandler sets
       ->Channel before calling this. */
    if (htim->Instance == TIM4 && htim->Channel == HAL_TIM_ACTIVE_CHANNEL_1) {
        /* CCR1 still holds the compare that just fired: the library writes
           the next one from inside notify_compare_match(). */
        uint32_t matched_at = *pg_hw.ccr;
        uint32_t start_cycles = DWT->CYCCNT;

        pulse_generator_notify_compare_match(&pg);

        uint32_t cycles = DWT->CYCCNT - start_cycles;
        uint32_t ticks = (__HAL_TIM_GET_COUNTER(htim) - matched_at) & pg_hw.counter_max;

        /* Only edges after which the movement goes on: those arm the next
           compare, which is what MIN_INTERVAL_TICKS protects. The last edge
           of a movement arms nothing, and also stops the counter
           (HAL_TIM_OC_Stop_IT clears CEN once no channel is left enabled),
           which would freeze CNT and under-read the ticks. */
        if (pulse_generator_is_busy(&pg)) {
            if (cycles > notify_max_cycles) {
                notify_max_cycles = cycles;
            }
            if (ticks > match_to_done_max_ticks) {
                match_to_done_max_ticks = ticks;
            }
        }
    }
}
