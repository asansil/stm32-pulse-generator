#ifndef MOCK_PLATFORM_H
#define MOCK_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pulse_generator.h"

typedef struct {
    bool timer_running;
    uint32_t last_ccr;
    uint32_t counter_value;
    bool dma_running;
    const uint32_t *dma_buffer;
    size_t dma_len;
    bool gpio_state;
} mock_platform_ctx_t;

extern mock_platform_ctx_t g_mock_platform_ctx;
extern const pulse_generator_platform_t g_mock_platform;

void mock_platform_reset(void);

#endif /* MOCK_PLATFORM_H */
