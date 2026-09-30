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
    if (mock_ctx->set_compare_result != PULSE_GENERATOR_OK) {
        return mock_ctx->set_compare_result;
    }
    mock_ctx->last_ccr = ccr;
    return PULSE_GENERATOR_OK;
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

static uint32_t mock_get_timer_main_clk(void *ctx)
{
    mock_platform_ctx_t *mock_ctx = (mock_platform_ctx_t *)ctx;
    return mock_ctx->timer_main_clk;
}

const pulse_generator_platform_t g_mock_platform = {
    .timer_start = mock_timer_start,
    .timer_stop = mock_timer_stop,
    .set_compare = mock_set_compare,
    .dma_start = mock_dma_start,
    .dma_stop = mock_dma_stop,
    .gpio_set = mock_gpio_set,
    .gpio_clear = mock_gpio_clear,
    .get_timer_main_clk = mock_get_timer_main_clk,
    .ctx = &g_mock_platform_ctx,
};

void mock_platform_reset(void)
{
    g_mock_platform_ctx.timer_running = false;
    g_mock_platform_ctx.last_ccr = 0;
    g_mock_platform_ctx.set_compare_result = PULSE_GENERATOR_OK;
    g_mock_platform_ctx.dma_running = false;
    g_mock_platform_ctx.dma_buffer = NULL;
    g_mock_platform_ctx.dma_len = 0;
    g_mock_platform_ctx.gpio_state = false;
    /* 2 MHz: arbitrary simulated tick rate that keeps existing tests'
       frequency_hz -> last_ccr expectations (e.g. 1000 Hz -> ccr 1000)
       unchanged. */
    g_mock_platform_ctx.timer_main_clk = 2000000;
}
