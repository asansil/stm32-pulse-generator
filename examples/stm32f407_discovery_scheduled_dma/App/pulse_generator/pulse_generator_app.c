#include "pulse_generator_app.h"

#include "pulse_generator_platform.h"
#include "pulse_generator_util.h"
#include "ramp_profile.h"

extern TIM_HandleTypeDef htim4;

#define ARRAY_LEN(a) (sizeof (a) / sizeof (a)[0])

/* 32 entries refilled 16 at a time within a 2 ms window: no entry lasts
   longer than 62.5 us (5250 ticks at 84 MHz), so an event the refill takes
   from the queue reaches the pin within 2 ms. */
#define PG_DMA_ENTRIES   32U
#define PG_DMA_WINDOW_US 2000U

/* Words per entry of the widest burst (channel 4: ARR, RCR, CCR1..CCR4), so
   the buffer fits whichever channel the board wiring picks. */
#define PG_DMA_MAX_WORDS_PER_ENTRY 6U

/* Shortest entry the DMA burst keeps up with, measured with the fixed-count
   example: 333 ns holds, 250 ns does not. 500 ns keeps a 1.5x margin. */
#define PG_MIN_ENTRY_NS 500U

/* Event queue storage. 63 events fit (one slot always stays empty). The
   watermark is checked once per refill, and a refill takes up to 16 events
   (one per entry), so it is set to cover at least one refill. */
#define QUEUE_CAPACITY 64U
#define LOW_WATERMARK  16U

/* Phases B and C: rest -> 5 kHz -> rest, about 2 s in all. */
#define RAMP_ACCEL_HZ_S    10000U
#define RAMP_CRUISE_HZ     5000U
#define RAMP_CRUISE_PULSES 5000U

/* Intervals pulled from the ramp generator per queue_events() call. */
#define FEED_BATCH_LEN 16U

/* Phase D: a 5 kHz train in which every LATENCY_MARKER_EVERY-th interval is
   a 1 ms one, queued right after a PD13 toggle. The producer keeps only
   LATENCY_LEAD_EVENTS pending, which must stay above what the start fill
   takes (32 entries: 8 periods of 200 us) and what a refill takes
   (16 entries: 4 periods), or the queue would run dry. 10 000 pulses,
   about 2 s. */
#define LATENCY_INTERVAL_US  200U
#define LATENCY_MARKER_US    1000U
#define LATENCY_LEAD_EVENTS  12U
#define LATENCY_MARKER_EVERY 500U
#define LATENCY_TOTAL_PULSES 10000U

/* Phase A: abrupt changes in both directions, so every fall can be checked
   against half of the *next* interval on a logic analyzer. In us, converted
   to ticks when queued. */
static const uint32_t known_sequence_us[] = {1000U, 1000U, 400U, 400U, 2000U, 250U, 250U, 1500U, 600U, 3000U};

typedef enum {
    PHASE_KNOWN_SEQUENCE, /* A: precomputed, queued in one go */
    PHASE_RAMP,           /* B: streamed from the main loop */
    PHASE_RAMP_UNDERRUN,  /* C: as B, but the producer stalls mid-cruise */
    PHASE_LATENCY,        /* D: constant train with marked intervals, short queue */
    PHASE_COUNT,
} demo_phase_t;

/* Idle time before each phase starts, so the phases are easy to tell apart
   on a logic analyzer. */
static const uint32_t pause_before_ms[PHASE_COUNT] = {
    [PHASE_KNOWN_SEQUENCE] = 2000U,
    [PHASE_RAMP]           = 1000U,
    [PHASE_RAMP_UNDERRUN]  = 1000U,
    [PHASE_LATENCY]        = 1000U,
};

typedef enum {
    DEMO_PAUSED,
    DEMO_RUNNING,
} demo_state_t;

/* CPU cost of one movement, with the DWT cycle counter. The refill is timed
   around notify_dma_*() alone, so cycles_per_pulse is the library's share of
   the interrupt, without the HAL dispatch around it. */
typedef struct {
    uint32_t pulses;              /* pulses the phase emits */
    uint32_t refills;             /* HT/TC interrupts serviced */
    uint32_t refill_max_cycles;
    uint32_t refill_total_cycles;
    uint32_t start_cycles;        /* start_scheduled(), which fills the whole buffer */
    uint32_t elapsed_cycles;      /* from the start until the main loop sees the end */
    uint32_t cycles_per_pulse;    /* refill_total_cycles / pulses */
    uint32_t load_permille;       /* refill_total_cycles / elapsed_cycles */
} phase_stats_t;

static stm32f4_pg_hw_t   pg_hw;
static pulse_generator_t pg;
static uint32_t          queue[QUEUE_CAPACITY];

/* Read by the DMA stream: must stay in SRAM, not CCM RAM. */
static uint32_t pg_dma_buffer[PG_DMA_ENTRIES * PG_DMA_MAX_WORDS_PER_ENTRY];

static bool           demo_ready;
static demo_state_t   demo_state;
static demo_phase_t   demo_phase;
static uint32_t       pause_start_ms;
static ramp_profile_t ramp;
static uint32_t       ramp_fed;        /* intervals handed to the library so far */
static uint32_t       ramp_feed_limit; /* the producer hands out no more than this */
static bool           ramp_finished;   /* finish_scheduled() already called */
static uint32_t       latency_fed;     /* phase D: intervals handed to the library so far */
static uint32_t       movement_pulses; /* pulses the movement in progress emits */
static uint32_t       movement_start_cycles;

/* Watch these in the debugger during hardware validation. Volatile so that
   an optimized build keeps them: most are only written. The
   failure counters should stay 0. */
static volatile pulse_generator_status_t hw_init_status;
static volatile pulse_generator_status_t pg_init_status;
static volatile pulse_generator_status_t prepare_status;
static volatile uint32_t rejected_interval_count;
static volatile uint32_t start_failure_count;
static volatile uint32_t notify_error_count; /* notify_dma_* returned an error */
static volatile uint32_t complete_count;
static volatile uint32_t underrun_count;
static volatile uint32_t low_watermark_count;
static volatile phase_stats_t phase_stats[PHASE_COUNT]; /* last movement of each phase */

/* Refill cost of the movement in progress, written by the DMA interrupt. */
static volatile uint32_t refill_count;
static volatile uint32_t refill_max_cycles;
static volatile uint32_t refill_total_cycles;

/* Runs from the DMA interrupt, inside the refill: counts, and drives the
   analyzer markers so that they land exactly where the library reports the
   event. */
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
    (void)pulse_generator_stop(&pg);
    HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_RESET);
}

/* Clears the refill counters, raises the phase marker (PD13) and starts
   whatever has been queued, timing the start: it fills the whole buffer
   from the queue before the stream starts, and the movement starts here.
   pulses is what the movement emits, for the cost per pulse. */
static bool start_movement(uint32_t pulses)
{
    refill_count = 0U;
    refill_max_cycles = 0U;
    refill_total_cycles = 0U;
    movement_pulses = pulses;

    HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_SET);

    movement_start_cycles = DWT->CYCCNT;
    pulse_generator_status_t status = pulse_generator_start_scheduled(&pg);
    phase_stats[demo_phase].start_cycles = DWT->CYCCNT - movement_start_cycles;

    if (status != PULSE_GENERATOR_OK) {
        start_failure_count++;
        return false;
    }

    return true;
}

/* Phase A, the precomputed case: the whole sequence fits in the queue, so
   it is queued, finished and started in one go. */
static bool start_known_sequence(void)
{
    uint32_t intervals[ARRAY_LEN(known_sequence_us)];

    for (size_t n = 0; n < ARRAY_LEN(intervals); n++) {
        if (!pulse_generator_us_to_ticks(pg_hw.tick_hz, known_sequence_us[n], &intervals[n])) {
            rejected_interval_count++;
            return false;
        }
    }

    if (pulse_generator_queue_events(&pg, intervals, ARRAY_LEN(intervals))
            != ARRAY_LEN(intervals)) {
        rejected_interval_count++;
        return false;
    }

    (void)pulse_generator_finish_scheduled(&pg);

    return start_movement(ARRAY_LEN(intervals));
}

/* Tops the queue up from the ramp generator, up to the feed limit, and
   declares the end of the sequence once the whole profile has been queued.
   With a limit short of the whole profile it never does: the producer just
   stops, as one that falls behind would. Returns false if an interval was
   rejected. */
static bool feed_ramp(void)
{
    uint32_t batch[FEED_BATCH_LEN];

    while (ramp_fed < ramp_feed_limit) {
        size_t free_space = pulse_generator_get_free_space(&pg);
        if (free_space == 0U) {
            break;
        }

        size_t wanted = (free_space < FEED_BATCH_LEN) ? free_space : FEED_BATCH_LEN;
        if (wanted > ramp_feed_limit - ramp_fed) {
            wanted = ramp_feed_limit - ramp_fed;
        }

        /* The limit never exceeds the profile, so the generator always has
           this many left: a failure is an interval the timeline rejected. */
        for (size_t n = 0; n < wanted; n++) {
            if (!ramp_profile_next(&ramp, &batch[n])) {
                rejected_interval_count++;
                return false;
            }
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
        (void)pulse_generator_finish_scheduled(&pg);
        ramp_finished = true;
    }

    return true;
}

/* Phases B and C, the streaming case: the queue is filled before the start
   and then kept topped up from the main loop while the movement runs. The
   start fills the whole buffer from the queue, and each refill reaches up
   to one window ahead of the pin, so the full queue (63 events, the first
   of them tens of ms long) is far more than the window's worth needed. In
   phase C the producer stops at the middle of the profile, which is the
   middle of the cruise since the profile is symmetric, and never finishes:
   every event queued is still played, then the movement ends as an
   underrun. */
static bool start_ramp(bool stall_mid_cruise)
{
    if (!ramp_profile_init(&ramp, pg_hw.tick_hz, RAMP_ACCEL_HZ_S, RAMP_CRUISE_HZ, RAMP_CRUISE_PULSES)) {
        start_failure_count++;
        return false;
    }

    const uint32_t total = ramp_profile_total_pulses(&ramp);
    ramp_feed_limit = stall_mid_cruise ? total / 2U : total;
    ramp_fed = 0U;
    ramp_finished = false;

    return feed_ramp() && start_movement(ramp_feed_limit);
}

/* Phase D: tops the queue up to LATENCY_LEAD_EVENTS, one interval at a
   time, and finishes as soon as the whole train has been queued. Every
   marked interval is queued right after a PD13 toggle, so the analyzer
   shows how long an event takes from queue_events() to the pin: the events
   still ahead of it in the queue, plus what the buffer already holds. The
   marked interval is the period of the pulse before it, so it shows up as
   one pulse 500 us wide, and the delay is measured to its rise. Returns
   false if the library rejected an interval. */
static bool feed_latency(void)
{
    while (latency_fed < LATENCY_TOTAL_PULSES &&
           pulse_generator_get_pending_events(&pg) < LATENCY_LEAD_EVENTS) {
        bool marked = (latency_fed % LATENCY_MARKER_EVERY) == LATENCY_MARKER_EVERY - 1U;
        uint32_t interval;

        if (!pulse_generator_us_to_ticks(pg_hw.tick_hz,
                                         marked ? LATENCY_MARKER_US : LATENCY_INTERVAL_US,
                                         &interval)) {
            rejected_interval_count++;
            return false;
        }

        if (marked) {
            HAL_GPIO_TogglePin(LD3_GPIO_Port, LD3_Pin);
        }
        if (pulse_generator_queue_events(&pg, &interval, 1U) != 1U) {
            rejected_interval_count++;
            return false;
        }

        latency_fed++;
        if (latency_fed == LATENCY_TOTAL_PULSES) {
            (void)pulse_generator_finish_scheduled(&pg);
        }
    }

    return true;
}

/* Phase D: the queue starts with LATENCY_LEAD_EVENTS and is kept there from
   the main loop, the short queue a closed loop would keep. */
static bool start_latency(void)
{
    latency_fed = 0U;

    return feed_latency() && start_movement(LATENCY_TOTAL_PULSES);
}

static bool start_phase(demo_phase_t phase)
{
    switch (phase) {
        case PHASE_KNOWN_SEQUENCE: return start_known_sequence();
        case PHASE_RAMP:           return start_ramp(false);
        case PHASE_RAMP_UNDERRUN:  return start_ramp(true);
        case PHASE_LATENCY:        return start_latency();
        default:                   return false;
    }
}

/* Keeps the producer of the phase running. Returns false if an interval was
   rejected. */
static bool feed_phase(demo_phase_t phase)
{
    switch (phase) {
        case PHASE_RAMP:
        case PHASE_RAMP_UNDERRUN: return feed_ramp();
        case PHASE_LATENCY:       return feed_latency();
        default:                  return true; /* phase A was queued whole */
    }
}

/* Snapshot of the movement that has just ended, once the stream is stopped
   and no refill writes the counters any more. */
static void record_phase_stats(void)
{
    volatile phase_stats_t *stats = &phase_stats[demo_phase];

    stats->pulses = movement_pulses;
    stats->refills = refill_count;
    stats->refill_max_cycles = refill_max_cycles;
    stats->refill_total_cycles = refill_total_cycles;
    stats->elapsed_cycles = DWT->CYCCNT - movement_start_cycles;
    stats->cycles_per_pulse = (stats->pulses != 0U) ? stats->refill_total_cycles / stats->pulses : 0U;
    stats->load_permille = (stats->elapsed_cycles != 0U)
        ? (uint32_t)((uint64_t)stats->refill_total_cycles * 1000U / stats->elapsed_cycles)
        : 0U;
}

/* One HT or TC interrupt: times the library's refill with the DWT and
   keeps PD15 high around it, so the analyzer shows how long it takes and
   how often it comes. */
static void service_refill(pulse_generator_status_t (*notify)(pulse_generator_t *))
{
    HAL_GPIO_WritePin(LD6_GPIO_Port, LD6_Pin, GPIO_PIN_SET);
    const uint32_t start = DWT->CYCCNT;

    const pulse_generator_status_t status = notify(&pg);

    const uint32_t cycles = DWT->CYCCNT - start;
    HAL_GPIO_WritePin(LD6_GPIO_Port, LD6_Pin, GPIO_PIN_RESET);

    /* An underrun, late refill or dry queue, is already counted through its
       event: phase C ends with one on purpose. */
    if (status != PULSE_GENERATOR_OK && status != PULSE_GENERATOR_ERROR_UNDERRUN) {
        notify_error_count++;
    }

    refill_count++;
    refill_total_cycles += cycles;
    if (cycles > refill_max_cycles) {
        refill_max_cycles = cycles;
    }
}

/* DWT cycle counter, used to time the refill. Wraps every ~25 s at
   168 MHz, well above the length of any phase. */
static void cycle_counter_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

void pulse_generator_app_init(void)
{
    cycle_counter_init();

    /* The board wiring lives here, not in the platform: TIM4_CH1 is PD12 on
       the Discovery. */
    hw_init_status = stm32f4_pg_hw_init(&pg_hw, &htim4, TIM_CHANNEL_1);
    if (hw_init_status != PULSE_GENERATOR_OK) {
        return;
    }

    pg_init_status = pulse_generator_init(&pg, &(pulse_generator_config_t){
        .ops = &g_stm32f4_pg_ops,
        .hw = &pg_hw,
        .on_event = on_pulse_generator_event,
        .pulse = {
            .shape = PULSE_GENERATOR_PULSE_SHAPE_HALF_PERIOD,
        },
        .dma = {
            .buffer = pg_dma_buffer,
            .buffer_words = ARRAY_LEN(pg_dma_buffer),
            .entries = PG_DMA_ENTRIES,
            .window_us = PG_DMA_WINDOW_US,
            .min_entry_ns = PG_MIN_ENTRY_NS,
        },
    });
    if (pg_init_status != PULSE_GENERATOR_OK) {
        return;
    }

    prepare_status = pulse_generator_prepare_scheduled(&pg, &(pulse_generator_scheduled_config_t){
        .queue = queue,
        .queue_capacity = QUEUE_CAPACITY,
        .low_watermark = LOW_WATERMARK,
    });
    if (prepare_status != PULSE_GENERATOR_OK) {
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
            /* RUNNING is published inside start_scheduled(), and the refill
               takes the instance back to ARMED when the movement ends. */
            if (pulse_generator_is_busy(&pg)) {
                if (!feed_phase(demo_phase)) {
                    abort_movement();
                }
                return;
            }
            record_phase_stats();
            abort_movement();
            demo_phase = (demo_phase_t)((demo_phase + 1) % PHASE_COUNT);
            demo_state = DEMO_PAUSED;
            pause_start_ms = HAL_GetTick();
            break;
    }
}

/* The DMA burst's half-transfer and transfer-complete interrupts, which HAL
   reports as these two timer callbacks (HAL_TIM_DMABurst_MultiWriteStart()
   installs them on the update DMA stream). */
void HAL_TIM_PeriodElapsedHalfCpltCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM4) {
        service_refill(pulse_generator_notify_dma_half_complete);
    }
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM4) {
        service_refill(pulse_generator_notify_dma_complete);
    }
}
