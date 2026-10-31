/**
 * Spectrum Energy Scan — 802.15.4 energy detection on 16 channels (ch11-ch26)
 *
 * Radio ownership (H1):
 *   - OpenThread builds: the OT port owns the esp_ieee802154_* driver
 *     callbacks (strong symbols). Raw driver calls would either never report
 *     results (our weak callbacks are not linked) or trip the OT port's
 *     asserts. All scanning goes through otLinkEnergyScan via
 *     thread_br_energy_scan().
 *   - Non-OT builds: raw esp_ieee802154_* driver, callbacks defined here.
 *     esp_ieee802154_energy_detect(duration) takes SYMBOL units (16 µs),
 *     not milliseconds.
 */

#include "spectrum.h"
#include "thread_br.h"
#include "config_mgr.h"

#include <string.h>
#include "esp_log.h"
#include "esp_ieee802154.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "spectrum_scan";

static volatile bool   s_energy_done = false;
static volatile int8_t s_energy_power = -100;
static bool s_initialized = false;
static SemaphoreHandle_t s_scan_lock = NULL;  /* serializes all spectrum ops */

#ifndef CONFIG_OPENTHREAD_ENABLED
/* ISR callback — only safe to define when the OT port doesn't own the radio */
void esp_ieee802154_energy_detect_done(int8_t power)
{
    s_energy_power = power;
    s_energy_done = true;
}
#endif

/* Exposed to beacon_scan.c so concurrent operations are serialized */
bool spectrum_scan_lock_take(uint32_t timeout_ms)
{
    if (!s_scan_lock) return false;
    return xSemaphoreTake(s_scan_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void spectrum_scan_lock_give(void)
{
    if (s_scan_lock) xSemaphoreGive(s_scan_lock);
}

bool spectrum_is_initialized(void)
{
    return s_initialized;
}

// ---- Init ----
esp_err_t spectrum_init(void)
{
    if (!s_scan_lock) {
        s_scan_lock = xSemaphoreCreateMutex();
        if (!s_scan_lock) return ESP_ERR_NO_MEM;
    }

#ifdef CONFIG_OPENTHREAD_ENABLED
    /* Never touch the raw radio in OT builds — the OT port owns the driver
       callbacks regardless of whether Thread is currently enabled. */
    s_initialized = true;
    ESP_LOGI(TAG, "Spectrum engine initialized (scans via OpenThread)");
    return ESP_OK;
#else
    esp_err_t err = esp_ieee802154_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "802.15.4 enable failed: %s", esp_err_to_name(err));
        return err;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "Spectrum engine initialized (raw 802.15.4 radio)");
    return ESP_OK;
#endif
}

// ---- Energy Scan ----
esp_err_t spectrum_energy_scan(uint16_t duration_per_ch_ms,
                               spectrum_scan_result_t *result)
{
    return spectrum_energy_scan_ex(duration_per_ch_ms, 0, result);
}

esp_err_t spectrum_energy_scan_ex(uint16_t duration_per_ch_ms, uint8_t passes,
                                  spectrum_scan_result_t *result)
{
    if (!result) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    /* ⚠️ 默认值不能再是 100 —— 本平台上 100ms 驻留会被 WiFi 共存 100% 拒绝
       （实测 0/16，见 thread_br.h 的驻留-成功率曲线）。这行曾经把调用方传的
       "0=用默认值"改写成 100，结果无论怎么调都跑在必然失败的参数上。 */
#ifdef CONFIG_OPENTHREAD_ENABLED
    if (duration_per_ch_ms == 0) duration_per_ch_ms = THREAD_BR_SCAN_DEFAULT_DWELL_MS;
    if (passes == 0)             passes = THREAD_BR_SCAN_DEFAULT_PASSES;
#else
    if (duration_per_ch_ms == 0) duration_per_ch_ms = 100;   /* 裸驱动路径无共存仲裁 */
    passes = 1;                                              /* 裸驱动路径不做多轮 */
#endif

    if (!spectrum_scan_lock_take(30000)) return ESP_ERR_TIMEOUT;

    memset(result, 0, sizeof(*result));
    result->scan_duration_ms = duration_per_ch_ms;
    result->passes           = passes;
    result->timestamp = (uint32_t)(esp_timer_get_time() / 1000000);

    esp_err_t ret;

#ifdef CONFIG_OPENTHREAD_ENABLED
    /* 短驻留多轮 max-hold：本平台上长驻留会被 WiFi 共存 100% 拒绝
       （实测 100ms → 0/16，1ms → 15~16/16，见 thread_br.h 的曲线）。
       部分信道拿不到读数是常态，用 valid_mask 如实表达，不再整批丢弃。 */
    ret = thread_br_energy_scan_ex(duration_per_ch_ms, passes,
                                   result->energy, &result->valid_mask);
    if (ret != ESP_OK) {
        /* 区分两种失败，否则会把"共存拒绝"误导成"Thread 没跑"：
           INVALID_STATE = Thread BR 确实没运行；
           TIMEOUT       = 16 个信道的 ED 全被共存仲裁器拒了，一个读数都没有。 */
        ESP_LOGW(TAG, "OT energy scan failed (%s) — %s", esp_err_to_name(ret),
                 ret == ESP_ERR_TIMEOUT ? "ED blocked by WiFi coex, no valid readings"
                                        : "Thread BR not running");
        result->valid_mask = 0;
        for (int i = 0; i < SPECTRUM_NUM_CHANNELS; i++) result->energy[i] = -100;
    } else if (result->valid_mask != 0xFFFF) {
        ESP_LOGI(TAG, "部分信道被共存拒绝：有效 %d/16 (mask 0x%04x)",
                 __builtin_popcount(result->valid_mask), result->valid_mask);
    }
#else
    ret = ESP_OK;
    for (int i = 0; i < SPECTRUM_NUM_CHANNELS; i++) {
        uint8_t ch = SPECTRUM_FIRST_CHANNEL + i;

        esp_err_t err = esp_ieee802154_set_channel(ch);
        if (err != ESP_OK) {
            result->energy[i] = -100;
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(5));

        s_energy_done = false;
        s_energy_power = -100;

        /* H1: duration is in symbol units (16 µs) — convert from ms */
        uint32_t duration_symbols = ((uint32_t)duration_per_ch_ms * 1000) / 16;
        err = esp_ieee802154_energy_detect(duration_symbols);
        if (err != ESP_OK) {
            result->energy[i] = -100;
            continue;
        }

        uint32_t timeout_ms = duration_per_ch_ms + 500;
        uint32_t waited = 0;
        while (!s_energy_done && waited < timeout_ms) {
            vTaskDelay(pdMS_TO_TICKS(10));
            waited += 10;
        }

        if (s_energy_done) {
            result->energy[i] = s_energy_power;
            result->valid_mask |= (uint16_t)(1u << i);   /* 真实读数才置位 */
        } else {
            result->energy[i] = -100;
        }
    }
    if (result->valid_mask == 0) ret = ESP_ERR_TIMEOUT;
#endif

    spectrum_scan_lock_give();

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Energy scan complete (%d ms/ch)", duration_per_ch_ms);
    }
    return ret;
}
