/* 层 G — 取证记录（docs/04_watchdog_design_v2.md 第十节）
 *
 * 两级存储：
 *   RTC_NOINIT  自愈计数与窗口。掉电清零（用户拔电=从头开始，这是想要的），
 *               软复位保留——正好覆盖"看门狗重启自己"的场景。
 *   NVS 环形    最近 8 条 L3/L4 记录，掉电不丢，供事后翻查。
 *               L1/L2 不写 NVS：告警可能每分钟一条，会磨损 Flash。
 */

#include "watchdog.h"

#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wd_forensic";

#define WD_RTC_MAGIC        0x5744F201u   /* 'WD' + version */
#define SELF_HEAL_WINDOW_S  3600
#define SELF_HEAL_MAX       3

/* RTC_NOINIT：不进 .bss，不被启动代码清零，软复位后内容保留 */
typedef struct {
    uint32_t magic;
    uint8_t  self_heal_count;    /* 当前窗口内的自愈重启次数 */
    uint8_t  safe_mode;
    uint8_t  last_fault_class;
    uint8_t  last_level;
    uint32_t window_runtime_s;   /* 窗口内累计运行时长（跨重启累加） */
    uint32_t last_uptime_s;
    char     last_detail[24];
    uint32_t snapshot[3];        /* internal_free / dma_free / alloc_fails */
} wd_rtc_t;

static RTC_NOINIT_ATTR wd_rtc_t s_rtc;

static bool s_last_was_self_heal;

// ---- NVS 环形历史 ----

#define NVS_NS   "wd"
#define NVS_KEY  "hist"

typedef struct {
    uint8_t          head;                     /* 下一条写入位置 */
    uint8_t          count;
    wd_forensic_rec_t rec[WD_HISTORY_MAX];
} wd_hist_blob_t;

static wd_hist_blob_t s_hist;

static void hist_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_hist);
    if (nvs_get_blob(h, NVS_KEY, &s_hist, &len) != ESP_OK || len != sizeof(s_hist)) {
        memset(&s_hist, 0, sizeof(s_hist));   /* 版本不匹配/首次运行 */
    }
    nvs_close(h);
    if (s_hist.head >= WD_HISTORY_MAX) {      /* 防御损坏的持久数据 */
        memset(&s_hist, 0, sizeof(s_hist));
    }
}

static void hist_store(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, NVS_KEY, &s_hist, sizeof(s_hist)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

// ---- Public ----

void wd_forensics_init(void)
{
    if (s_rtc.magic != WD_RTC_MAGIC) {
        /* 上电首次：RTC RAM 是随机值 */
        memset(&s_rtc, 0, sizeof(s_rtc));
        s_rtc.magic = WD_RTC_MAGIC;
        ESP_LOGI(TAG, "RTC record initialized (cold boot)");
    } else if (s_rtc.last_level >= WD_LEVEL_REBOOT) {
        /* 上一次是看门狗把自己重启的 */
        s_last_was_self_heal = true;
        ESP_LOGW(TAG, "上次重启由看门狗触发: class=%u detail=%s "
                 "(窗口内第 %u 次, 累计运行 %"PRIu32"s)",
                 s_rtc.last_fault_class, s_rtc.last_detail,
                 s_rtc.self_heal_count, s_rtc.window_runtime_s);
        /* 消费掉，避免下一次普通重启还报自愈 */
        s_rtc.last_level = WD_LEVEL_NONE;
    }

    hist_load();

    if (s_rtc.safe_mode) {
        ESP_LOGE(TAG, "SAFE MODE 生效：窗口内自愈 %u 次 (上限 %d)",
                 s_rtc.self_heal_count, SELF_HEAL_MAX);
    }
}

void wd_forensics_record(wd_fault_class_t cls, wd_level_t lvl,
                         const char *detail)
{
    /* 防御：任何路径在 wd_forensics_init() 之前上报都不能污染 RTC 记录 */
    if (s_rtc.magic != WD_RTC_MAGIC) {
        memset(&s_rtc, 0, sizeof(s_rtc));
        s_rtc.magic = WD_RTC_MAGIC;
    }

    uint32_t uptime = (uint32_t)(esp_timer_get_time() / 1000000);

    s_rtc.last_fault_class = (uint8_t)cls;
    s_rtc.last_level       = (uint8_t)lvl;
    s_rtc.last_uptime_s    = uptime;
    strlcpy(s_rtc.last_detail, detail ? detail : "?", sizeof(s_rtc.last_detail));

    uint32_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT);
    uint32_t dma      = heap_caps_get_free_size(MALLOC_CAP_DMA);
    uint32_t fails    = 0;
    watchdog_get_alloc_failures(&fails, NULL, NULL);
    s_rtc.snapshot[0] = internal;
    s_rtc.snapshot[1] = dma;
    s_rtc.snapshot[2] = fails;

    if (lvl >= WD_LEVEL_REBOOT) {
        /* 滑动窗口：累计运行时长超过 1 小时就开新窗口 */
        if (s_rtc.window_runtime_s + uptime > SELF_HEAL_WINDOW_S) {
            s_rtc.window_runtime_s = 0;
            s_rtc.self_heal_count  = 1;
        } else {
            s_rtc.window_runtime_s += uptime;
            s_rtc.self_heal_count++;
        }
        if (s_rtc.self_heal_count >= SELF_HEAL_MAX) {
            s_rtc.safe_mode = 1;
            ESP_LOGE(TAG, "自愈过于频繁（%u 次 / %"PRIu32"s）→ 下次启动进入安全模式",
                     s_rtc.self_heal_count, s_rtc.window_runtime_s);
        }
    }

    if (lvl >= WD_LEVEL_SUBSYS) {
        wd_forensic_rec_t *r = &s_hist.rec[s_hist.head];
        r->uptime_s      = uptime;
        r->fault_class   = (uint8_t)cls;
        r->level         = (uint8_t)lvl;
        r->internal_free = internal;
        r->dma_free      = dma;
        r->alloc_fails   = fails;
        strlcpy(r->detail, detail ? detail : "?", sizeof(r->detail));

        s_hist.head = (uint8_t)((s_hist.head + 1) % WD_HISTORY_MAX);
        if (s_hist.count < WD_HISTORY_MAX) s_hist.count++;
        hist_store();   /* ⚠️ 写 Flash：调用方必须是 wd_action 任务（内部栈） */
    }
}

#if CONFIG_BITENGINE_PROFILE_DIRECT
void wd_forensics_record_planned(const char *detail, bool to_history)
{
    /* L3 进 NVS 历史（≥ SUBSYS）、L2 只记 RTC；两种都不加自愈计数（< REBOOT）—— 见 watchdog.h 的 WD_FAULT_PLANNED */
    wd_forensics_record(WD_FAULT_PLANNED, to_history ? WD_LEVEL_SUBSYS : WD_LEVEL_TARGET, detail);
}
#endif

size_t wd_forensics_history(wd_forensic_rec_t *out, size_t max)
{
    if (!out || max == 0) return 0;
    size_t n = s_hist.count < max ? s_hist.count : max;
    /* 从最新往回填 */
    for (size_t i = 0; i < n; i++) {
        int idx = (int)s_hist.head - 1 - (int)i;
        while (idx < 0) idx += WD_HISTORY_MAX;
        out[i] = s_hist.rec[idx];
    }
    return n;
}

bool wd_forensics_last_was_self_heal(wd_forensic_rec_t *out_rec)
{
    if (out_rec && s_last_was_self_heal) {
        memset(out_rec, 0, sizeof(*out_rec));
        out_rec->uptime_s      = s_rtc.last_uptime_s;
        out_rec->fault_class   = s_rtc.last_fault_class;
        out_rec->level         = WD_LEVEL_REBOOT;
        out_rec->internal_free = s_rtc.snapshot[0];
        out_rec->dma_free      = s_rtc.snapshot[1];
        out_rec->alloc_fails   = s_rtc.snapshot[2];
        strlcpy(out_rec->detail, s_rtc.last_detail, sizeof(out_rec->detail));
    }
    return s_last_was_self_heal;
}

// ---- 安全模式状态（存储在 RTC，判定逻辑在 wd_recovery.c）----

bool wd_forensics_safe_mode(void)          { return s_rtc.safe_mode != 0; }
uint8_t wd_forensics_self_heal_count(void) { return s_rtc.self_heal_count; }
const char *wd_forensics_last_detail(void) { return s_rtc.last_detail; }

void wd_forensics_clear_safe_mode(void)
{
    s_rtc.safe_mode        = 0;
    s_rtc.self_heal_count  = 0;
    s_rtc.window_runtime_s = 0;
    ESP_LOGW(TAG, "安全模式记录已清除");
}
