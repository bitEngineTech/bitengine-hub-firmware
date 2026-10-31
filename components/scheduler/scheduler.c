#include "scheduler.h"
#include "watchdog.h"
#include "config_mgr.h"
#include "led_ctrl.h"
#include "webhook.h"
#include "spectrum.h"

#include <time.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psram_task.h"   /* 任务栈放 PSRAM，省内部 SRAM */

static int s_wd_slot_scheduler = -1;

static const char *TAG = "scheduler";

static bool     s_night_active = false;
static uint32_t s_last_backup_time = 0;
static uint32_t s_last_spectrum_time = 0;

/* 单个 bool 的跨 task 读取：scheduler_task 写、httpd task 读。bool 读写在
   RISC-V 上是单指令，读到的要么是新值要么是旧值，两者都是合法答案（最坏
   情况是状态晚一个 60s 周期），不值得为它上锁。 */
bool scheduler_night_active(void)
{
    return s_night_active;
}

static bool is_time_valid(void)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    return t.tm_year > (2024 - 1900);
}

static void check_night_mode(void)
{
    const led_config_t *cfg = config_mgr_get_led();

    if (!cfg->night_mode_enabled) {
        if (s_night_active) {
            s_night_active = false;
            led_ctrl_set_night(false);
            led_ctrl_set_eth_led(true);
            ESP_LOGI(TAG, "Night mode disabled, restoring normal");
        }
        return;
    }

    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    uint16_t current = t.tm_hour * 100 + t.tm_min;
    uint16_t start   = cfg->night_start_hour * 100 + cfg->night_start_min;
    uint16_t end     = cfg->night_end_hour * 100 + cfg->night_end_min;

    bool in_night;
    if (start == end) {
        in_night = false;  // zero-duration = disabled
    } else if (start < end) {
        in_night = (current >= start && current < end);
    } else {
        // Cross midnight (e.g., 22:00-07:00)
        in_night = (current >= start || current < end);
    }

    if (in_night && !s_night_active) {
        s_night_active = true;
        led_ctrl_set_night(true);
        led_ctrl_set_eth_led(false);
        ESP_LOGI(TAG, "Night mode ON (%02d:%02d-%02d:%02d)",
                 cfg->night_start_hour, cfg->night_start_min,
                 cfg->night_end_hour, cfg->night_end_min);
    } else if (!in_night && s_night_active) {
        s_night_active = false;
        led_ctrl_set_night(false);
        led_ctrl_set_eth_led(true);
        ESP_LOGI(TAG, "Night mode OFF");
    }
}

static void check_auto_backup(void)
{
    const webhook_config_t *cfg = config_mgr_get_webhook();
    if (!cfg->enabled || cfg->backup_interval == 0) return;

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);

    // First check after boot: initialize timestamp
    if (s_last_backup_time == 0) {
        s_last_backup_time = now;
        return;
    }

    if (now - s_last_backup_time >= cfg->backup_interval) {
        s_last_backup_time = now;

        /* 不再在这里拼内容（T3.7）。以前这里发的是一句占位符
           {"hostname":..,"note":"use /api/v1/config/export"}，因为 webhook 队列项的
           data 只有 128 字节装不下完整配置 —— 于是这个"自动备份"是个空壳，
           收到通知的人还得自己再去拉一次。

           现在完整配置由 webhook 任务在**发送那一刻**生成（见 webhook.c 里
           WEBHOOK_EVENT_CONFIG_BACKUP 那段）：队列项不必变大，备份也反映发出
           时的配置而不是入队时的。这里只负责"到点了，该备份了"。 */
        webhook_post_event(WEBHOOK_EVENT_CONFIG_BACKUP, NULL);
        ESP_LOGI(TAG, "自动配置备份已入队（完整配置在发送时生成）");
    }
}

static void scheduler_task(void *arg)
{
    while (1) {
        wd_kick(s_wd_slot_scheduler);
        vTaskDelay(pdMS_TO_TICKS(60000));  // 1 minute

        if (is_time_valid()) {
            check_night_mode();
            check_auto_backup();
        } else {
            // SNTP not synced yet — skip time-dependent tasks
        }

        // Spectrum background check (every 30 minutes, no time dependency)
        uint32_t uptime = (uint32_t)(esp_timer_get_time() / 1000000);
        if (uptime - s_last_spectrum_time >= 1800) {
            spectrum_background_check();
            s_last_spectrum_time = uptime;
        }

        // OTA check would go here (uptime-based, no time dependency)
    }
}

esp_err_t scheduler_init(void)
{
    static const wd_task_spec_t wd_spec = {
        .name = "scheduler", .deadline_ms = 180000, .severity = WD_SEV_OBSERVE,
    };
    s_wd_slot_scheduler = wd_register(&wd_spec);
    /* 栈放 PSRAM。psram_task.h 原先把 scheduler 列为排除项，理由是"自动备份写 NVS"
       —— 该理由已过期：check_auto_backup() 只是发一个 webhook 事件
       （全组件 grep nvs_/esp_partition_ 零命中）。
       ⚠️ 2026-09-11(T3.7) 起完整配置由 **webhook 任务**在发送时生成
       （config_mgr_export_json()，只读内存里的 g_config，不碰 NVS），
       所以这里仍然不写 NVS，结论不变。
       ⚠️ scheduler.c 里那行 "OTA check would go here" 一旦接上云端 OTA、要写 NVS 记录
       检查时间，就必须改回 xTaskCreate。 */
    psram_task_create(scheduler_task, "scheduler", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "Scheduler started (60s interval)");
    return ESP_OK;
}
