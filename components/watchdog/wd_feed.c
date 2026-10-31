/* 层 F — 硬件后备与条件喂狗（docs/04_watchdog_design_v2.md 第九节）
 *
 * 这个任务是整个看门狗体系里优先级最高的一环（pri 22），职责有两件：
 *   1. 每 500ms 扫描一遍任务心跳注册表（层 B）
 *   2. 根据扫描结果决定喂不喂 RS706
 *
 * 为什么扫描要放在这里：它必须比被监视的任务优先级都高，否则"任务被饿死"
 * 的场景里扫描自己也被饿死，永远发现不了。
 *
 * ⚠️ 条件喂狗只在 RS706 的 WDO 真的能拉复位时才有意义。AM1 板上 WDO 经
 *    R55/R57 分压后是 1.16-1.42V，远高于 MR 的 VIL=0.33V，**喂不喂都不会
 *    复位**（待确认清单 X7/A6）。所以现状下由软件承担后备：扫描发现问题
 *    直接走恢复引擎。用 CONFIG_BITENGINE_RS706_WDO_FUNCTIONAL 切换两种形态，
 *    架构本身不变。
 */

#include "watchdog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#ifdef CONFIG_BITENGINE_HAS_HW_WATCHDOG
#include "driver/gpio.h"
#include "devkit_pins.h"
#else
#include "esp_task_wdt.h"
#endif

static const char *TAG = "wdt_feed";

static void wdt_feed_task(void *arg)
{
    (void)arg;
#ifndef CONFIG_BITENGINE_HAS_HW_WATCHDOG
    /* 无 RS706：退回 IDF Task WDT，本任务自己订阅进去。
       trigger_panic=true 才会真的复位；CONFIG_ESP_TASK_WDT_INIT=y 时 WDT 已在
       启动阶段初始化过（默认不 panic），此时 init() 返回 INVALID_STATE，
       必须改用 reconfigure()。 */
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms      = 5000,
        .idle_core_mask  = 0,
        .trigger_panic   = true,
    };
    esp_err_t err = esp_task_wdt_init(&wdt_cfg);
    if (err == ESP_ERR_INVALID_STATE) err = esp_task_wdt_reconfigure(&wdt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Task WDT config failed: %s — no panic on hang!",
                 esp_err_to_name(err));
    }
    if ((err = esp_task_wdt_add(NULL)) != ESP_OK) {
        ESP_LOGE(TAG, "Task WDT subscribe failed: %s", esp_err_to_name(err));
    }
#endif

    bool level = false;
    for (;;) {
        /* 层 B：全部 RECOVER/FATAL 任务是否都在按时 kick */
        bool all_hard_ok = wd_registry_scan();

#ifdef CONFIG_BITENGINE_HAS_HW_WATCHDOG
# if CONFIG_BITENGINE_RS706_WDO_FUNCTIONAL
        /* A6 修复后：不健康就停止喂狗，让 RS706 在 ~1.6s 后拉复位。
           这是唯一一条"固件完全卡死也能复位"的路径。 */
        if (all_hard_ok) {
            level = !level;
            gpio_set_level(WDT_WDI_PIN, level);
        }
# else
        /* AM1：WDO 接不动 MR，喂不喂都一样。保持翻转是为了示波器上能验证
           这条链路本身在工作（A6 改版时要用），后备由恢复引擎承担——
           wd_registry_scan() 内部已经 wd_report 了，这里不必重复上报。 */
        (void)all_hard_ok;
        level = !level;
        gpio_set_level(WDT_WDI_PIN, level);
# endif
#else
        (void)all_hard_ok;
        esp_task_wdt_reset();
#endif
        vTaskDelay(pdMS_TO_TICKS(WDT_FEED_INTERVAL_MS));
    }
}

void wdt_feed_start(void)
{
#ifdef CONFIG_BITENGINE_HAS_HW_WATCHDOG
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << WDT_WDI_PIN,
        .mode         = GPIO_MODE_OUTPUT,
    };
    /* 分步日志 + 刷新延时：板 1 曾在这一步整板掉电（焊桥串进 RS706
       RESET/EN 网络），这些延时保证"最后打印出来的那一步就是杀死板子的
       那条指令"。每次启动约 80ms，为现场诊断保留。 */
    ESP_LOGI(TAG, "step: gpio_config(WDI=%d)", WDT_WDI_PIN);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_config(&io);
    ESP_LOGI(TAG, "step: drive WDI(%d) low", WDT_WDI_PIN);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(WDT_WDI_PIN, 0);
# if CONFIG_BITENGINE_RS706_WDO_FUNCTIONAL
    ESP_LOGI(TAG, "RS706 WDI pin %d configured（条件喂狗：不健康即停喂）", WDT_WDI_PIN);
# else
    ESP_LOGW(TAG, "RS706 WDI pin %d configured，但 WDO 接不动 MR（A6 未修复）——"
             "硬件复位不可用，后备由恢复引擎承担", WDT_WDI_PIN);
# endif
#else
    ESP_LOGI(TAG, "No HW watchdog — using ESP-IDF Task WDT fallback");
#endif

    /* ⚠️ 内部栈（不用 psram_task_create）：这是整个系统的最后一道软件防线，
       不能因为 PSRAM 或 cache 的任何异常而失效。 */
    xTaskCreate(wdt_feed_task, "wdt_feed", 2560, NULL, 22, NULL);
    ESP_LOGI(TAG, "wdt_feed_task started (pri 22, %dms, 含心跳扫描)",
             WDT_FEED_INTERVAL_MS);
}
