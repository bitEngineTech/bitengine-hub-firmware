#pragma once
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int g_pass, g_fail;

#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  ❌ %s:%d  ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define CASE(name) do { printf("  · %s\n", name); } while (0)

static inline int wd_test_summary(const char *suite)
{
    printf("%s: %d passed, %d failed\n", suite, g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

/* 全部可控桩的实体定义。每个测试二进制引一次。 */
#define WD_TEST_GLOBALS \
    int64_t  g_fake_us = 0; \
    int      g_restart_count = 0; \
    int      g_event_count = 0; \
    uint32_t g_fake_internal_free = 20000, g_fake_dma_free = 15000; \
    void    *g_fake_current_task = (void *)0x1234; \
    uint32_t g_fake_stack_headroom = 4096; \
    char     g_nvs_blob[512]; \
    size_t   g_nvs_len = 0; \
    int      g_nvs_fail = 0; \
    int      g_reset_reason = 1;   /* ESP_RST_POWERON */

#define ADVANCE_MS(ms) do { g_fake_us += (int64_t)(ms) * 1000; } while (0)
#define SET_MS(ms)     do { g_fake_us  = (int64_t)(ms) * 1000; } while (0)
