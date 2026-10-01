#ifndef MOCK_PLATFORM_H
#define MOCK_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pulse_generator.h"

typedef struct {
    bool timer_running;
    pulse_generator_status_t timer_start_result;         /* returned by timer_start; tests set it to inject a failure */
    uint32_t start_ticks;                                /* half_period_ticks passed to the last timer_start */
    int advance_compare_call_count;
    uint32_t last_advance_ticks;
    pulse_generator_status_t advance_compare_result;     /* returned by advance_compare; tests set it to inject a missed compare */
    uint32_t max_ticks;                                  /* reported by get_max_ticks */
    bool dma_running;
    const uint32_t *dma_buffer;
    size_t dma_len;
    bool gpio_state;
    uint32_t timer_main_clk;
} mock_platform_ctx_t;

extern mock_platform_ctx_t g_mock_platform_ctx;
extern const pulse_generator_platform_t g_mock_platform;

void mock_platform_reset(void);

#endif /* MOCK_PLATFORM_H */
