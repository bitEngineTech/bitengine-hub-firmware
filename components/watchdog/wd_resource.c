/* 层 D — 资源守卫（docs/04_watchdog_design_v2.md 第七节）
 *
 * 对付 F4「资源耗尽」。v1 在这里栽了最大的跟头：它比的是
 * esp_get_free_heap_size()，那是 MALLOC_CAP_DEFAULT 各堆的**累加值**，8MB
 * PSRAM 在里面，实测常驻 8.2MB——20KB 告警线和 15KB 重启线永远不可能触发。
 * 与此同时内部 SRAM 已经见底、DMA 可用内存最低 299 字节、WiFi/LWIP 缓冲区
 * 分配 10 分钟失败 2245 次（≈4 次/秒，协议栈级丢包）。
 *
 * 两条对应的铁律：
 *   - 不看聚合量。本文件禁止出现 esp_get_free_heap_size / min_free_heap，
 *     VERIFY.sh 会断言这一点。
 *   - 以"分配是否真的失败"为主判据，水位只作辅助告警。失败计数不需要为不同
 *     PSRAM 配置猜阈值，0 就是健康；而水位的"正常值"随固件功能变化。
 *
 * ⚠️ 水位低不重启。实测正常内部空闲就在 15-21KB，把 v1 的 15KB 重启线直接
 *    指向内部堆会立刻变成重启循环。重启只由持续的分配失败驱动。
 */

#include "watchdog.h"

#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"

static const char *TAG = "wd_resource";

/* ---- 阈值（来源见设计文档第十二节）---- */
#define ALLOC_FAIL_WARN_PER_MIN   1        /* 实测健康值为 0，非 0 即异常 */
#define ALLOC_FAIL_CRIT_PER_MIN   10       /* 实测故障期约 240/min */
#define ALLOC_FAIL_CRIT_WINDOWS   3        /* 连续 3 个窗口 → L3 */
#define ALLOC_FAIL_REBOOT_WINDOWS 5        /* 连续 5 个窗口(5 分钟) → L4 */
#define INTERNAL_FREE_WARN        (8 * 1024)   /* 实测健康 min 15,948 B */
#define DMA_FREE_WARN             (4 * 1024)   /* 实测健康 min 13,939 B */
#define DMA_LARGEST_WARN          (2 * 1024)   /* 一个 MTU 缓冲约 1,690 B */
#define STACK_HEADROOM_WARN       512
#define WINDOW_TICKS              6            /* 6 × 10s = 60s */

/* ---- 分配失败回调（IDF heap 钩子）---- */
/* 回调可能在 ISR 上下文触发：只做计数，绝不打日志、不取锁。 */
static volatile uint32_t s_fail_total;
static volatile uint32_t s_fail_last_size;
static volatile uint32_t s_fail_last_caps;

static void IRAM_ATTR alloc_failed_cb(size_t size, uint32_t caps, const char *fn)
{
    (void)fn;
    s_fail_total++;
    s_fail_last_size = size;
    s_fail_last_caps = caps;
}

void watchdog_get_alloc_failures(uint32_t *count, uint32_t *last_size,
                                 uint32_t *last_caps)
{
    if (count)     *count     = s_fail_total;
    if (last_size) *last_size = s_fail_last_size;
    if (last_caps) *last_caps = s_fail_last_caps;
}

/* ---- 窗口统计 ---- */
static uint32_t s_window_base;        /* 本窗口开始时的累计失败数 */
static uint8_t  s_window_tick;
static uint32_t s_window_fails;       /* 上一个完整窗口的失败数 */
static uint8_t  s_crit_windows;       /* 连续超标窗口数 */
static bool     s_warn_active;

static wd_resource_t s_view;

void wd_resource_init(void)
{
    heap_caps_register_failed_alloc_callback(alloc_failed_cb);
    s_window_base = 0;
    s_window_tick = 0;
    ESP_LOGI(TAG, "资源守卫就绪（判据：分配失败计数，水位仅告警）");
}

/* 按失败的 caps 推断该重启谁。caps 是 IDF 的位掩码：
   MALLOC_CAP_DMA=1<<3, MALLOC_CAP_SPIRAM=1<<10, MALLOC_CAP_INTERNAL=1<<11 */
static const char *blame_from_caps(uint32_t caps)
{
    if (caps & MALLOC_CAP_DMA)      return "dma";       /* WiFi/以太网/UART 缓冲 */
    if (caps & MALLOC_CAP_INTERNAL) return "internal";  /* 任务栈/驱动 */
    if (caps & MALLOC_CAP_SPIRAM)   return "spiram";
    return "generic";
}

void wd_resource_tick(void)
{
    /* ---- 水位快照（分堆，绝不聚合）---- */
    s_view.internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT);
    s_view.internal_min  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT);
    s_view.dma_free      = heap_caps_get_free_size(MALLOC_CAP_DMA);
    s_view.dma_min       = heap_caps_get_minimum_free_size(MALLOC_CAP_DMA);
    s_view.dma_largest   = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    s_view.alloc_fail_total     = s_fail_total;
    s_view.alloc_fail_last_size = s_fail_last_size;
    s_view.alloc_fail_last_caps = s_fail_last_caps;

    wd_registry_stack_low(&s_view.stack_low_task, &s_view.stack_low_bytes);

    /* ---- 主判据：分配失败窗口 ---- */
    uint32_t in_window = s_fail_total - s_window_base;
    s_view.alloc_fail_window = in_window;

    if (++s_window_tick >= WINDOW_TICKS) {
        s_window_tick = 0;
        s_window_fails = in_window;
        s_window_base  = s_fail_total;

        if (s_window_fails >= ALLOC_FAIL_CRIT_PER_MIN) {
            s_crit_windows++;
            const char *blame = blame_from_caps(s_fail_last_caps);
            ESP_LOGE(TAG, "分配失败 %"PRIu32"/min（连续 %u 个窗口），"
                     "最后一次 %"PRIu32"B caps=0x%"PRIx32" [%s]",
                     s_window_fails, s_crit_windows,
                     s_fail_last_size, s_fail_last_caps, blame);

            /* 升级由恢复引擎按次数决定；这里只负责"报几次" */
            if (s_crit_windows >= ALLOC_FAIL_REBOOT_WINDOWS) {
                uint32_t n = s_window_fails;
                esp_event_post(WATCHDOG_EVENT, WATCHDOG_EVENT_HEAP_CRITICAL,
                               &n, sizeof(n), 0);
                wd_report(WD_FAULT_RESOURCE, WD_SEV_FATAL, "alloc_fail_sustained", NULL);
            } else if (s_crit_windows >= ALLOC_FAIL_CRIT_WINDOWS) {
                wd_report(WD_FAULT_RESOURCE, WD_SEV_RECOVER, blame, NULL);
            } else {
                wd_report(WD_FAULT_RESOURCE, WD_SEV_OBSERVE, blame, NULL);
            }
        } else {
            if (s_crit_windows > 0) {
                ESP_LOGI(TAG, "分配失败率回落（本窗口 %"PRIu32"）", s_window_fails);
                wd_report_recovered(WD_FAULT_RESOURCE, "alloc_fail");
            }
            s_crit_windows = 0;

            if (s_window_fails >= ALLOC_FAIL_WARN_PER_MIN) {
                ESP_LOGW(TAG, "分配失败 %"PRIu32"/min（最后 %"PRIu32"B caps=0x%"PRIx32"）",
                         s_window_fails, s_fail_last_size, s_fail_last_caps);
                uint32_t n = s_window_fails;
                esp_event_post(WATCHDOG_EVENT, WATCHDOG_EVENT_HEAP_LOW,
                               &n, sizeof(n), 0);
            }
        }
    }

    /* ---- 辅助判据：水位。只告警，不参与重启决策 ---- */
    bool low = (s_view.internal_free < INTERNAL_FREE_WARN) ||
               (s_view.dma_free      < DMA_FREE_WARN)      ||
               (s_view.dma_largest   < DMA_LARGEST_WARN);

    if (low && !s_warn_active) {
        s_warn_active = true;
        ESP_LOGW(TAG, "内存水位低: internal=%"PRIu32" dma=%"PRIu32" 最大块=%"PRIu32,
                 s_view.internal_free, s_view.dma_free, s_view.dma_largest);
        uint32_t v = s_view.internal_free;
        esp_event_post(WATCHDOG_EVENT, WATCHDOG_EVENT_HEAP_LOW, &v, sizeof(v), 0);
    } else if (!low && s_warn_active) {
        s_warn_active = false;
        ESP_LOGI(TAG, "内存水位恢复");
    }

    /* ---- 栈余量。用 latch 保证只在跳变时报，否则 10s 一条刷屏 ---- */
    static bool s_stack_warned;
    bool stack_low = s_view.stack_low_task &&
                     s_view.stack_low_bytes < STACK_HEADROOM_WARN;
    if (stack_low && !s_stack_warned) {
        s_stack_warned = true;
        ESP_LOGW(TAG, "任务栈余量不足: %s 仅剩 %"PRIu32" 字节",
                 s_view.stack_low_task, s_view.stack_low_bytes);
        uint32_t left = s_view.stack_low_bytes;
        esp_event_post(WATCHDOG_EVENT, WATCHDOG_EVENT_HEAP_LEAK_SUSPECTED,
                       &left, sizeof(left), 0);
    } else if (!stack_low) {
        s_stack_warned = false;
    }
}

void wd_resource_get(wd_resource_t *out)
{
    if (out) *out = s_view;
}
