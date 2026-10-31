/* 层 C/D 的调度者 + 模块协调（v2）
 *
 * v1 的 check_heap_health() 已整体移出到 wd_resource.c —— 它比的是
 * esp_get_free_heap_size() 这个含 8MB PSRAM 的聚合值，四条判定路径全部
 * 不可能触发（详见 docs/04_watchdog_design_v2.md 第〇节）。
 * v1 的 check_mg24_health() 移出到 wd_probes.c —— 它在 LAN 模式无客户端时
 * 直接假定存活，等于不检测。
 *
 * 留在这里的是三个"只上报不恢复"的周期检查（温度/RSSI/UART 错误率）和
 * 启动原因判定，加上 s_status 的维护（WebUI 与 LED 都读它）。
 */

#include "watchdog.h"
#include "ezsp_bridge.h"
#include "network_mgr.h"
#include "config_mgr.h"    /* config_mgr_flush：重启前把防抖中的配置写进去 */

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "driver/temperature_sensor.h"
#include "psram_task.h"

static const char *TAG = "health_mon";

// ---- Shared health status ----
static health_status_t s_status;

// ---- 心跳槽（层 B）----
static int s_wd_slot = -1;

// ---- Boot-time heap marks (see watchdog.h) ----
static heap_mark_t s_marks[HEAP_MARK_MAX];
static size_t      s_mark_count = 0;

void watchdog_heap_mark(const char *step)
{
    uint32_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT);
    uint32_t dma      = heap_caps_get_free_size(MALLOC_CAP_DMA);

    ESP_LOGI(TAG, "heapmark %-10s internal=%6" PRIu32 " dma=%6" PRIu32,
             step, internal, dma);

    /* 只在启动阶段调用（app_main 单线程），满了就丢弃后续标记 */
    if (s_mark_count >= HEAP_MARK_MAX) return;
    s_marks[s_mark_count].step          = step;
    s_marks[s_mark_count].internal_free = internal;
    s_marks[s_mark_count].dma_free      = dma;
    s_marks[s_mark_count].uptime_ms     = (uint32_t)(esp_timer_get_time() / 1000);
    s_mark_count++;
}

size_t watchdog_heap_marks(const heap_mark_t **out)
{
    if (out) *out = s_marks;
    return s_mark_count;
}

// ---- Temperature sensor ----
static temperature_sensor_handle_t s_temp_handle = NULL;

// ---- Accessors for mg24_reset.c ----
extern uint32_t mg24_get_last_reset_time(void);
extern uint32_t mg24_get_total_resets(void);

// ---- 温度 ----

static void check_temperature(void)
{
    if (!s_temp_handle) return;

    float temp;
    if (temperature_sensor_get_celsius(s_temp_handle, &temp) != ESP_OK) {
        return;
    }
    s_status.temperature = temp;

    /* latch：跨过阈值时报一次，别 10s 一条刷屏 */
    static uint8_t s_level;   /* 0 正常 1 高 2 危急 */
    uint8_t lv = temp >= TEMP_CRITICAL_THRESHOLD ? 2
               : temp >= TEMP_WARN_THRESHOLD     ? 1 : 0;
    if (lv == s_level) return;
    s_level = lv;

    if (lv == 2) {
        ESP_LOGE(TAG, "CRITICAL: chip temperature %.1f°C (threshold %.0f°C)",
                 temp, TEMP_CRITICAL_THRESHOLD);
        esp_event_post(WATCHDOG_EVENT, WATCHDOG_EVENT_TEMP_CRITICAL,
                       &temp, sizeof(temp), 0);
    } else if (lv == 1) {
        ESP_LOGW(TAG, "High temperature: %.1f°C (warn > %.0f°C)",
                 temp, TEMP_WARN_THRESHOLD);
        esp_event_post(WATCHDOG_EVENT, WATCHDOG_EVENT_TEMP_HIGH,
                       &temp, sizeof(temp), 0);
    } else {
        ESP_LOGI(TAG, "Temperature back to normal: %.1f°C", temp);
    }
}

// ---- WiFi RSSI ----

static void check_wifi_rssi(void)
{
#if CONFIG_BITENGINE_NET_ETH_ONLY
    /* 纯以太网剖面（docs/15 设计 2.2）：没有 Wi-Fi，也就没有 RSSI；不碰驱动 = 不把 Wi-Fi 库链接进来 */
    s_status.wifi_rssi = 0;
#else
    /* 没关联就别问驱动：STA 已配置但未关联时（凭据指向的 AP 不存在、正在重连）
       这个 ioctl 每次都让驱动打一条 `Haven't to connect to a suitable AP now!`，
       本函数 10s 一次，串口就一直在刷。没关联时 RSSI 本来也只能是 0。 */
    if (!network_mgr_sta_connected()) {
        s_status.wifi_rssi = 0;
        return;
    }

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        s_status.wifi_rssi = 0;
        return;
    }
    s_status.wifi_rssi = ap.rssi;

    static bool s_warned;
    bool low = ap.rssi < WIFI_RSSI_WARN_THRESHOLD && ap.rssi != 0;
    if (low && !s_warned) {
        s_warned = true;
        ESP_LOGW(TAG, "WiFi RSSI low: %d dBm (warn < %d)",
                 ap.rssi, WIFI_RSSI_WARN_THRESHOLD);
        int8_t rssi = ap.rssi;
        esp_event_post(WATCHDOG_EVENT, WATCHDOG_EVENT_WIFI_RSSI_LOW,
                       &rssi, sizeof(rssi), 0);
    } else if (!low) {
        s_warned = false;
    }
#endif /* CONFIG_BITENGINE_NET_ETH_ONLY */
}

// ---- UART 错误率 ----

static void check_uart_error_rate(void)
{
    const bridge_stats_t *stats = ezsp_bridge_get_stats();
    if (!stats) return;

    /* Rate over the window since the last check, not since boot — cumulative
       counters would mask an error burst after days of clean uptime. */
    static uint32_t prev_rx = 0, prev_err = 0;
    uint32_t d_rx  = stats->uart_rx_bytes - prev_rx;
    uint32_t d_err = stats->uart_rx_errors - prev_err;
    prev_rx  = stats->uart_rx_bytes;
    prev_err = stats->uart_rx_errors;

    uint32_t total = d_rx + d_err;
    if (total == 0) {
        s_status.uart_error_rate_pct = 0;
        return;
    }

    uint8_t rate = (uint8_t)((uint64_t)d_err * 100 / total);
    s_status.uart_error_rate_pct = rate;

    if (rate >= UART_ERROR_RATE_WARN_PCT) {
        ESP_LOGW(TAG, "UART error rate high: %d%% (errors=%"PRIu32" total=%"PRIu32" in window)",
                 rate, d_err, total);
        esp_event_post(WATCHDOG_EVENT, WATCHDOG_EVENT_UART_ERROR_RATE_HIGH,
                       &rate, sizeof(rate), 0);
    }
}

// ---- 启动原因 ----

static bool s_abnormal_boot_pending = false;
static wd_abnormal_boot_t s_abnormal_boot;     /* 开机判出的异常原因，补发时原样带出去 */

static void check_boot_reason(void)
{
    /* 真原因：main.c 非干净复位后补的那次重启会把 esp_reset_reason() 变成 SW（T9.27） */
    esp_reset_reason_t reason = wd_boot_reason_get();
    s_status.last_reset_reason = (uint8_t)reason;

    const char *reason_str;
    switch (reason) {
    case ESP_RST_POWERON:  reason_str = "POWER_ON";  break;
    case ESP_RST_SW:       reason_str = "SOFTWARE";   break;
    case ESP_RST_PANIC:    reason_str = "PANIC";      break;
    case ESP_RST_INT_WDT:  reason_str = "INT_WDT";    break;
    case ESP_RST_TASK_WDT: reason_str = "TASK_WDT";   break;
    case ESP_RST_WDT:      reason_str = "WDT";        break;
    case ESP_RST_BROWNOUT: reason_str = "BROWNOUT";    break;
    case ESP_RST_PWR_GLITCH: reason_str = "PWR_GLITCH"; break;
    case ESP_RST_CPU_LOCKUP: reason_str = "CPU_LOCKUP"; break;   /* 双重异常 */
    default:               reason_str = "OTHER";       break;
    }

    ESP_LOGI(TAG, "Last reset reason: %s (%d)", reason_str, reason);

    /* 看门狗自愈重启的 reset reason 是 SW，和用户主动重启无法区分——取证记录
       才分得清。这里合并判定，避免自愈重启被当成正常软复位放过去。 */
    wd_forensic_rec_t rec;
    bool self_heal = wd_forensics_last_was_self_heal(&rec);
    if (self_heal) {
        ESP_LOGE(TAG, "上次是看门狗自愈重启: class=%u detail=%s "
                 "(internal=%"PRIu32" dma=%"PRIu32" fails=%"PRIu32")",
                 rec.fault_class, rec.detail,
                 rec.internal_free, rec.dma_free, rec.alloc_fails);
    }

    if (self_heal || reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
        reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT ||
        reason == ESP_RST_BROWNOUT || reason == ESP_RST_PWR_GLITCH ||
        reason == ESP_RST_CPU_LOCKUP) {
        ESP_LOGW(TAG, "Abnormal boot detected: %s", reason_str);
        s_abnormal_boot.reset_reason = (uint8_t)reason;
        if (self_heal) {
            /* rec 来自 RTC / NVS，不赌它以 NUL 结尾 */
            char detail[sizeof(rec.detail) + 1];
            memcpy(detail, rec.detail, sizeof(rec.detail));
            detail[sizeof(rec.detail)] = '\0';
            wd_abnormal_cause_watchdog(&s_abnormal_boot, (wd_fault_class_t)rec.fault_class, detail);
        } else {
            wd_abnormal_cause_reset(&s_abnormal_boot, reason);
        }
        /* Defer the event: subscribers (webhook, MQTT) register only after
           the network is up — posting now would be delivered to nobody. */
        s_abnormal_boot_pending = true;
    }
}

// ---- 主循环 ----

static void health_task(void *arg)
{
    (void)arg;
    check_boot_reason();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(HEALTH_CHECK_INTERVAL_MS));
        wd_kick(s_wd_slot);

        s_status.uptime_seconds = (uint32_t)(esp_timer_get_time() / 1000000);
        s_status.mg24_last_reset_time = mg24_get_last_reset_time();
        s_status.mg24_total_resets = mg24_get_total_resets();

        /* 兼容字段：/api/v1/status 的 free_heap / min_free_heap 沿用聚合值
           （前端已在用）。⚠️ 它含 8MB PSRAM，**不作任何判定依据**——判定看
           wd_resource（分堆水位 + 分配失败）。新增字段见 diag/watchdog。 */
        s_status.free_heap     = esp_get_free_heap_size();
        s_status.min_free_heap = esp_get_minimum_free_heap_size();

        /* 层 D + 层 C */
        wd_resource_tick();
        wd_probes_tick();

        /* MG24 存活结论现在由探针给出，这里只镜像给 WebUI / LED */
        wd_probe_view_t pv;
        wd_probes_get(&pv);
        s_status.mg24_alive = pv.mg24_ok ? 1 : 0;

        /* Late-post the abnormal-boot event once subscribers had a chance
           to register (webhook/MQTT init after network-up, ~tens of s). */
        if (s_abnormal_boot_pending && s_status.uptime_seconds >= 60) {
            s_abnormal_boot_pending = false;
            esp_event_post(WATCHDOG_EVENT, WATCHDOG_EVENT_ABNORMAL_BOOT,
                           &s_abnormal_boot, sizeof(s_abnormal_boot), 0);
        }

        check_temperature();
        check_wifi_rssi();
        check_uart_error_rate();
    }
}

// ---- Public ----

void health_monitor_start(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.mg24_alive = 1;

    // Init temperature sensor. Range must cover TEMP_CRITICAL_THRESHOLD
    // (85°C) — with the default -10..80 range the critical alarm could
    // never fire because 85°C is outside the measurable window.
    temperature_sensor_config_t temp_cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    if (temperature_sensor_install(&temp_cfg, &s_temp_handle) == ESP_OK) {
        temperature_sensor_enable(s_temp_handle);
        ESP_LOGI(TAG, "Temperature sensor initialized");
    } else {
        ESP_LOGW(TAG, "Temperature sensor init failed (non-fatal)");
        s_temp_handle = NULL;
    }

    /* health 停转 = 整套周期性检查全停，包括资源守卫和探针，所以是 FATAL。
       deadline 取周期的 3 倍。 */
    static const wd_task_spec_t spec = {
        .name = "health", .deadline_ms = 30000, .severity = WD_SEV_FATAL,
    };
    s_wd_slot = wd_register(&spec);

    /* 3072 而非 2048：v2 起 health_task 还要跑 wd_resource_tick 与
       wd_probes_tick（后者在栈上放 WD_MAX_TASKS 个 wd_task_view_t，每个 24 B）。
       2048 时 /api/v1/diag/watchdog 实测余量只剩 720 字节。 */
    psram_task_create(health_task, "health", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "health_task started (pri 5, %dms interval)",
             HEALTH_CHECK_INTERVAL_MS);
}

const health_status_t *health_monitor_get_status(void)
{
    return &s_status;
}

// ============================================================
// watchdog.h public API implementations (coordinator)
// ============================================================

ESP_EVENT_DEFINE_BASE(WATCHDOG_EVENT);

esp_err_t watchdog_init(void)
{
    ESP_LOGI(TAG, "Initializing watchdog module (v2)");

    /* 顺序有讲究：
       1. 取证先起——后面任何一层都可能要写记录，且要先读出上次的自愈标记
       2. 资源守卫次之——它注册 IDF 的分配失败回调，越早越能覆盖启动期
       3. 恢复引擎第三——它要读取证里的安全模式标记，且必须先于任何上报存在
       4. 之后才是会产生上报的各层 */
    wd_forensics_init();
    wd_resource_init();
    wd_recovery_init();

    mg24_reset_init();
    wdt_feed_start();
    health_monitor_start();

    ESP_LOGI(TAG, "Watchdog module initialized%s",
             wd_safe_mode() ? "（安全模式）" : "");
    return ESP_OK;
}

const health_status_t *watchdog_get_status(void)
{
    return &s_status;
}

esp_err_t watchdog_reset_mg24(void)
{
    return mg24_do_reset();
}

void watchdog_restart_esp32(void)
{
    ESP_LOGW(TAG, "ESP32 restart requested");
    (void)config_mgr_flush(500);     /* 按键重启：刚在网页上改的设置可能还在 2 s 防抖里 */
    vTaskDelay(pdMS_TO_TICKS(500));  // Allow log to flush
    esp_restart();
}
