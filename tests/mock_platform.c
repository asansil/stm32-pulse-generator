#include "mock_platform.h"

mock_platform_ctx_t g_mock_platform_ctx;

static pulse_generator_status_t mock_timer_start(void *ctx)
{
    mock_platform_ctx_t *mock_ctx = (mock_platform_ctx_t *)ctx;
    mock_ctx->timer_running = true;
    return PULSE_GENERATOR_OK;
}

static pulse_generator_status_t mock_timer_stop(void *ctx)
{
    mock_platform_ctx_t *mock_ctx = (mock_platform_ctx_t *)ctx;
    mock_ctx->timer_running = false;
    return PULSE_GENERATOR_OK;
}

static pulse_generator_status_t mock_set_compare(void *ctx, uint32_t ccr)
{
    mock_platform_ctx_t *mock_ctx = (mock_platform_ctx_t *)ctx;
    mock_ctx->last_ccr = ccr;
    return PULSE_GENERATOR_OK;
}

static uint32_t mock_get_counter(void *ctx)
{
    mock_platform_ctx_t *mock_ctx = (mock_platform_ctx_t *)ctx;
    return mock_ctx->counter_value;
}

static pulse_generator_status_t mock_dma_start(void *ctx, const uint32_t *buffer, size_t len)
{
    mock_platform_ctx_t *mock_ctx = (mock_platform_ctx_t *)ctx;
    mock_ctx->dma_running = true;
    mock_ctx->dma_buffer = buffer;
    mock_ctx->dma_len = len;
    return PULSE_GENERATOR_OK;
}

static pulse_generator_status_t mock_dma_stop(void *ctx)
{
    mock_platform_ctx_t *mock_ctx = (mock_platform_ctx_t *)ctx;
    mock_ctx->dma_running = false;
    return PULSE_GENERATOR_OK;
}

static pulse_generator_status_t mock_gpio_set(void *ctx)
{
    mock_platform_ctx_t *mock_ctx = (mock_platform_ctx_t *)ctx;
    mock_ctx->gpio_state = true;
    return PULSE_GENERATOR_OK;
}

static pulse_generator_status_t mock_gpio_clear(void *ctx)
{
    mock_platform_ctx_t *mock_ctx = (mock_platform_ctx_t *)ctx;
    mock_ctx->gpio_state = false;
    return PULSE_GENERATOR_OK;
}

const pulse_generator_platform_t g_mock_platform = {
    .timer_start = mock_timer_start,
    .timer_stop = mock_timer_stop,
    .set_compare = mock_set_compare,
    .get_counter = mock_get_counter,
    .dma_start = mock_dma_start,
    .dma_stop = mock_dma_stop,
    .gpio_set = mock_gpio_set,
    .gpio_clear = mock_gpio_clear,
    .ctx = &g_mock_platform_ctx,
};

void mock_platform_reset(void)
{
    g_mock_platform_ctx.timer_running = false;
    g_mock_platform_ctx.last_ccr = 0;
    g_mock_platform_ctx.counter_value = 0;
    g_mock_platform_ctx.dma_running = false;
    g_mock_platform_ctx.dma_buffer = NULL;
    g_mock_platform_ctx.dma_len = 0;
    g_mock_platform_ctx.gpio_state = false;
}
