/**
 * Zigbee-to-Matter Bridge — Framework Scaffold
 *
 * Full implementation requires ESP-Matter SDK integration which adds ~80-120KB RAM.
 * This scaffold provides the public API and Z2M互斥 mode switching logic.
 * Actual Matter commissioning and device mapping deferred until:
 *   1. ESP-Matter SDK confirmed compatible with ESP32-C5
 *   2. Memory budget verified (~100KB available after all Phase 2-3 modules)
 *
 * Architecture (Phase 5 V1 — Method A: exclusive EZSP session):
 *   User selects "Matter Mode" in WebUI
 *   → ezsp_bridge_switch_mode(BRIDGE_MODE_DISABLED)  // stop Z2M TCP bridge
 *   → matter_bridge_start()  // take over UART for direct EZSP session
 *   → ESP-Matter SDK commissioning + Zigbee device mapping
 */

#include "matter_bridge.h"
#include "ezsp_bridge.h"

#include "esp_log.h"

static const char *TAG = "matter_bridge";
static bridge_mode_t s_prev_mode = BRIDGE_MODE_LAN;

esp_err_t matter_bridge_init(void)
{
    // Framework only — no Matter SDK linked yet
    ESP_LOGI(TAG, "Matter Bridge framework initialized (SDK integration pending)");
    return ESP_OK;
}

esp_err_t matter_bridge_start(void)
{
    // Step 1: Save current mode, then disable Z2M bridge (mutually exclusive)
    s_prev_mode = ezsp_bridge_get_mode();
    ESP_LOGI(TAG, "Matter Bridge start requested — disabling Z2M bridge (was mode %d)", s_prev_mode);
    esp_err_t err = ezsp_bridge_switch_mode(BRIDGE_MODE_DISABLED);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to disable bridge mode: %s", esp_err_to_name(err));
        return err;
    }

    // Step 2: Take over UART for direct EZSP session
    // TODO: Implement ASH RST → RSTACK → EZSP version query → network form
    // TODO: Initialize ESP-Matter SDK Bridge device
    // TODO: Start Matter commissioning (BLE or on-network)

    /* S3: 走到这里 Zigbee 桥已经被关掉了，直接 return 会把用户的 Z2M/ZHA 连接
       丢在 DISABLED 上再报个错 —— 失败路径必须把已获取/已改动的状态还原。
       stop() 那边一直是对的，start() 的错误路径漏了。 */
    ESP_LOGW(TAG, "Matter SDK not yet integrated — EZSP session management pending");
    esp_err_t restore = ezsp_bridge_switch_mode(s_prev_mode);
    if (restore != ESP_OK) {
        ESP_LOGE(TAG, "Rollback to mode %d failed: %s — bridge left disabled",
                 s_prev_mode, esp_err_to_name(restore));
    } else {
        ESP_LOGI(TAG, "Rolled back to mode %d", s_prev_mode);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t matter_bridge_stop(void)
{
    // TODO: Stop Matter SDK, release UART
    // Restore previous bridge mode (S3: don't assume LAN — could have been USB)
    ESP_LOGI(TAG, "Matter Bridge stopped, restoring mode %d", s_prev_mode);
    return ezsp_bridge_switch_mode(s_prev_mode);
}

esp_err_t matter_bridge_add_device(uint16_t zigbee_short_addr,
                                    uint8_t endpoint,
                                    const char *device_type)
{
    if (!device_type) return ESP_ERR_INVALID_ARG;
    ESP_LOGI(TAG, "Add device 0x%04X ep%d type=%s (pending SDK integration)",
             zigbee_short_addr, endpoint, device_type);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t matter_bridge_remove_device(uint16_t zigbee_short_addr)
{
    ESP_LOGI(TAG, "Remove device 0x%04X (pending SDK integration)",
             zigbee_short_addr);
    return ESP_ERR_NOT_SUPPORTED;
}
