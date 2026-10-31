/*
 * 真复位原因（T9.27）。
 *
 * main.c 在非干净复位（panic、看门狗、欠压……）之后会补一次 esp_restart()，规避 5 GHz 射频
 * 卡死；于是第二次开机 esp_reset_reason() 恒为 ESP_RST_SW，真原因丢了 —— /status 报 SOFTWARE，
 * check_boot_reason() 认不出异常，webhook / MQTT 的异常开机告警从来发不出去。
 *
 * 做法：补重启之前把真原因写进 RTC_NOINIT（软复位保留），开机早期 latch 一次取回并清掉。
 * 只在这次开机的复位原因确实是 SW 时才采信记录：掉电后 RTC_NOINIT 是随机值，靠魔数 + 这个
 * 条件两道把关。之后所有人一律用 wd_boot_reason_get()，别再直接调 esp_reset_reason()。
 *
 * network_mgr 的 s_reset_reason 刻意不改：它判的是"这次开机走的是不是软复位路径、墙钟能不能
 * 继承"（T9.20），要的正是 esp_reset_reason() 的原值。
 */

#include "watchdog.h"

#include "esp_attr.h"
#include "esp_log.h"

static const char *TAG = "wd_boot";

#define BOOT_REASON_MAGIC 0xB0070000u      /* 高 16 位魔数，低 16 位 esp_reset_reason_t */

static RTC_NOINIT_ATTR uint32_t s_rtc_noted;
static esp_reset_reason_t s_reason = ESP_RST_UNKNOWN;
static bool s_latched;

void wd_boot_reason_note(esp_reset_reason_t original)
{
    s_rtc_noted = BOOT_REASON_MAGIC | ((uint32_t)original & 0xFFFFu);
}

void wd_boot_reason_latch(void)
{
    if (s_latched) return;
    esp_reset_reason_t now = esp_reset_reason();
    s_reason = now;
    if (now == ESP_RST_SW && (s_rtc_noted & 0xFFFF0000u) == BOOT_REASON_MAGIC) {
        s_reason = (esp_reset_reason_t)(s_rtc_noted & 0xFFFFu);
        ESP_LOGW(TAG, "上一次开机是非干净复位（reason=%d），之后补重启了一次 —— 按 %d 上报",
                 (int)s_reason, (int)s_reason);
    }
    s_rtc_noted = 0;          /* 只用一次：下一次正常的软重启不能再被认成它 */
    s_latched = true;
}

esp_reset_reason_t wd_boot_reason_get(void)
{
    /* main.c 开机第一时间就 latch 了；这里兜底只为调用顺序被改坏时不读到 UNKNOWN */
    if (!s_latched) wd_boot_reason_latch();
    return s_reason;
}

/* ---- 异常开机事件的 cause 串（T9.30 ④）---- */

/* 只放行 [a-z0-9_:/.-]，大写转小写，其余换成 '_'：cause 会被原样拼进 webhook / MQTT 的 JSON。
   detail 来自代码里的常量（"thread"、任务名……），按理不会有引号，但这里不赌。 */
static void cause_append(char *out, size_t cap, size_t *len, const char *src)
{
    for (; src && *src && *len + 1 < cap; src++) {
        char c = *src;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == ':' || c == '/' || c == '.' || c == '-';
        out[(*len)++] = ok ? c : '_';
    }
    out[*len] = '\0';
}

void wd_abnormal_cause_watchdog(wd_abnormal_boot_t *ev, wd_fault_class_t cls, const char *detail)
{
    if (!ev) return;
    size_t len = 0;
    ev->cause[0] = '\0';
    cause_append(ev->cause, sizeof(ev->cause), &len, "watchdog:");
    cause_append(ev->cause, sizeof(ev->cause), &len, wd_fault_class_name(cls));
    cause_append(ev->cause, sizeof(ev->cause), &len, "/");
    cause_append(ev->cause, sizeof(ev->cause), &len, detail ? detail : "unknown");
}

void wd_abnormal_cause_reset(wd_abnormal_boot_t *ev, esp_reset_reason_t r)
{
    if (!ev) return;
    const char *s;
    switch (r) {
    case ESP_RST_PANIC:    s = "panic";    break;
    case ESP_RST_INT_WDT:  s = "int_wdt";  break;
    case ESP_RST_TASK_WDT: s = "task_wdt"; break;
    case ESP_RST_WDT:      s = "wdt";      break;
    case ESP_RST_BROWNOUT: s = "brownout"; break;
    case ESP_RST_PWR_GLITCH: s = "pwr_glitch"; break;
    case ESP_RST_CPU_LOCKUP: s = "cpu_lockup"; break;
    case ESP_RST_POWERON:  s = "poweron";  break;
    case ESP_RST_SW:       s = "software"; break;
    case ESP_RST_EXT:      s = "ext";      break;
    default:               s = "other";    break;
    }
    size_t len = 0;
    ev->cause[0] = '\0';
    cause_append(ev->cause, sizeof(ev->cause), &len, s);
}
