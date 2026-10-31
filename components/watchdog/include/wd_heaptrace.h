#pragma once
/* 诊断专用：抓"一个时间窗里 DMA 内存池被谁吃掉"（2026-10-05，拔网线 failover 低谷排查）。
 * 只在 CONFIG_BITENGINE_DIAG_HEAP_TRACE=y 的诊断固件里有实现；出厂固件不编进去。
 * 原理与限制见 wd_heaptrace.c 文件头。 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    WD_HT_IDLE = 0,     /* 没抓过，或被重置；下一次以太网断线自动开窗 */
    WD_HT_ARMED,        /* 窗口进行中 */
    WD_HT_DONE,         /* 窗口结束，结果可读；不会被下一次断线覆盖，要先重置 */
} wd_ht_state_t;

typedef struct {
    wd_ht_state_t state;
    const char   *trigger;        /* "eth_down" / "manual"；没抓过为 "" */
    uint32_t      window_ms;
    uint32_t      dma_free_at_arm;
    uint32_t      dma_free_min;   /* 窗口内每次被记录的 DMA 分配之后读到的最低值 */
    uint32_t      min_at_ms;      /* 最低值出现在开窗后多少 ms */
    uint32_t      n_entries;      /* 记录了多少次 DMA 分配 */
    bool          overflow;       /* 记录表满了，之后的分配没记上 */
    uint32_t      skipped;        /* ISR 里或 cache 关着时发生、没法记的分配/释放次数 */
} wd_ht_summary_t;

typedef struct {
    uint32_t t_ms;                /* 分配时刻，开窗后 ms */
    uint32_t size;                /* 请求的大小（不含堆块头） */
    uint32_t caps;                /* 请求的 MALLOC_CAP_*（任务栈是 INTERNAL|8BIT，malloc 是 DEFAULT） */
    int32_t  freed_ms;            /* 释放时刻；-1 = 窗口结束时还活着 */
    char     task[12];            /* 发起分配的任务名（只含 [A-Za-z0-9_.-]，其余替换成 ?） */
    bool     alive_at_min;        /* 最低点那一刻还占着 —— 低谷就是这些块堆出来的 */
} wd_ht_entry_t;

/* 在 PSRAM 里备好记录表并订阅以太网断线。必须在 PSRAM 自检之后、网络起来之前调
   （要比 network_mgr 先订阅，断线时才能最先开窗）。 */
esp_err_t wd_heaptrace_init(void);

/* 立即开一个窗口。ESP_ERR_INVALID_STATE = 正在抓或没初始化；
   ESP_ERR_INVALID_ARG = window_ms 不在 [1000, 60000]。 */
esp_err_t wd_heaptrace_arm(const char *trigger, uint32_t window_ms);

/* 把 DONE 置回 IDLE，让下一次以太网断线重新自动开窗。正在抓时什么也不做。 */
void wd_heaptrace_reset(void);

void wd_heaptrace_summary(wd_ht_summary_t *out);

/* 第 i 条记录（0 <= i < n_entries）。窗口进行中一律返回 false。 */
bool wd_heaptrace_entry(uint32_t i, wd_ht_entry_t *out);
