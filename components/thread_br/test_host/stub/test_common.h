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

static inline int srp_test_summary(const char *suite)
{
    printf("%s: %d passed, %d failed\n", suite, g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

/* 可控桩的实体定义。每个测试二进制引一次。 */
#define SRP_TEST_GLOBALS \
    int64_t g_fake_us = 0; \
    int     g_fake_spiram_fail = 0; \
    int     g_fake_br_running = 1; \
    int     g_fake_task_create_ok = 1; \
    int     g_delay_total_ms = 0;
