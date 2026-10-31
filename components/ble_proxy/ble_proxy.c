/**
 * BLE Proxy for Home Assistant
 *
 * Passive BLE scanning → MQTT publish to HA Bluetooth Proxy integration.
 * Uses NimBLE stack. BLE and WiFi 5GHz coexist without conflict.
 * BLE and Thread (802.15.4) share 2.4GHz via ESP-IDF coex (automatic).
 *
 * H1 API contracts verified:
 *   - nimble_port_init() → ble_hs_cfg.sync_cb → nimble_port_freertos_init()
 *   - ble_gap_disc(own_addr, duration, &params, callback, arg) — passive scan
 *   - BLE_GAP_EVENT_DISC → event->disc.addr/.rssi/.data/.length_data
 *   - Callback runs in NimBLE host task context (NOT ISR) — safe for MQTT publish
 */

#include "ble_proxy.h"
#include "bitengine_mqtt.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"

static const char *TAG = "ble_proxy";

static volatile bool s_running = false;

// ---- Rate limiting: per-device max once per 30s ----
#define MAX_TRACKED_DEVICES  64
#define PUBLISH_INTERVAL_US  (30 * 1000000LL)  // 30 seconds

typedef struct {
    uint8_t  addr[6];
    int64_t  last_publish;
} ble_device_tracker_t;

static ble_device_tracker_t s_tracker[MAX_TRACKED_DEVICES];
static int s_tracker_count = 0;

static bool should_publish(const uint8_t *addr)
{
    int64_t now = esp_timer_get_time();

    // Check existing
    for (int i = 0; i < s_tracker_count; i++) {
        if (memcmp(s_tracker[i].addr, addr, 6) == 0) {
            if (now - s_tracker[i].last_publish < PUBLISH_INTERVAL_US) {
                return false;  // rate limited
            }
            s_tracker[i].last_publish = now;
            return true;
        }
    }

    // New device — add to tracker or evict oldest
    if (s_tracker_count < MAX_TRACKED_DEVICES) {
        memcpy(s_tracker[s_tracker_count].addr, addr, 6);
        s_tracker[s_tracker_count].last_publish = now;
        s_tracker_count++;
    } else {
        // LRU eviction: replace the entry with oldest last_publish
        int oldest = 0;
        for (int i = 1; i < MAX_TRACKED_DEVICES; i++) {
            if (s_tracker[i].last_publish < s_tracker[oldest].last_publish) {
                oldest = i;
            }
        }
        memcpy(s_tracker[oldest].addr, addr, 6);
        s_tracker[oldest].last_publish = now;
    }
    return true;
}

// ---- Hex encoding for adv_data ----
static void hex_encode(const uint8_t *data, int len, char *out, int out_max)
{
    int j = 0;
    for (int i = 0; i < len && j + 2 < out_max; i++) {
        sprintf(out + j, "%02x", data[i]);
        j += 2;
    }
    out[j] = '\0';
}

// ---- BLE GAP event callback (NimBLE host task context — safe for MQTT) ----
static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        // P3: validate data before processing
        if (!event->disc.data || event->disc.length_data == 0) break;
        if (!mqtt_client_is_connected()) break;

        const uint8_t *addr = event->disc.addr.val;

        // Rate limit
        if (!should_publish(addr)) break;

        // Format MAC address
        char mac[18];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

        // Hex encode advertising data
        char adv_hex[128];
        int hex_len = event->disc.length_data > 62 ? 62 : event->disc.length_data;
        hex_encode(event->disc.data, hex_len, adv_hex, sizeof(adv_hex));

        // Build MQTT topic and payload
        char topic[48];
        snprintf(topic, sizeof(topic), "bitengine/ble/%s", mac);

        char payload[192];
        snprintf(payload, sizeof(payload),
                 "{\"rssi\":%d,\"adv_data\":\"%s\"}",
                 (int)event->disc.rssi, adv_hex);

        // S2: mqtt_client_publish uses esp_mqtt_client_enqueue (non-blocking)
        //     — safe from NimBLE host task context
        mqtt_client_publish(topic, payload);
        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "BLE scan complete (reason=%d), restarting",
                 event->disc_complete.reason);
        // Restart scan if still running
        if (s_running) {
            struct ble_gap_disc_params params = {0};
            params.passive = 1;
            params.filter_duplicates = 0;  // we do our own rate limiting
            uint8_t own_addr;
            int rc = ble_hs_id_infer_auto(0, &own_addr);
            if (rc == 0) {
                rc = ble_gap_disc(own_addr, BLE_HS_FOREVER, &params,
                                  ble_gap_event_cb, NULL);
            }
            if (rc != 0) {
                /* Don't pretend the proxy is alive when the scan died */
                ESP_LOGE(TAG, "BLE scan restart failed: %d — proxy stopped", rc);
                s_running = false;
            }
        }
        break;

    default:
        break;
    }
    return 0;
}

// ---- NimBLE sync callback: start scanning ----
static void on_sync(void)
{
    ESP_LOGI(TAG, "BLE host synced, starting passive scan");

    uint8_t own_addr;
    int rc = ble_hs_id_infer_auto(0, &own_addr);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }

    struct ble_gap_disc_params params = {0};
    params.passive = 1;              // H1: passive=1 for listen-only scan
    params.filter_duplicates = 0;    // we handle rate limiting ourselves

    rc = ble_gap_disc(own_addr, BLE_HS_FOREVER, &params, ble_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
    }
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset: reason=%d", reason);
}

// ---- NimBLE host task ----
static void ble_host_task(void *arg)
{
    nimble_port_run();  // blocks until nimble_port_stop()
    nimble_port_freertos_deinit();
}

// ---- Public API ----

esp_err_t ble_proxy_init(void)
{
    // S3: prevent double-init without stop (would leak NimBLE host task)
    if (s_running) {
        ESP_LOGW(TAG, "BLE Proxy already running");
        return ESP_OK;
    }

    // NimBLE initializes regardless of MQTT state.
    // Scan callback checks mqtt_client_is_connected() before each publish.

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE init failed: %s", esp_err_to_name(err));
        return err;
    }

    // H1: configure host callbacks before starting
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    // Reset state BEFORE starting host task (S2: prevent race with callback)
    s_tracker_count = 0;
    s_running = true;

    // Start NimBLE host task
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE Proxy started (passive scan → MQTT)");
    return ESP_OK;
}

esp_err_t ble_proxy_stop(void)
{
    if (!s_running) return ESP_OK;

    s_running = false;
    ble_gap_disc_cancel();

    int rc = nimble_port_stop();
    if (rc != 0) {
        ESP_LOGW(TAG, "nimble_port_stop failed: %d", rc);
    }
    nimble_port_deinit();

    ESP_LOGI(TAG, "BLE Proxy stopped");
    return ESP_OK;
}

bool ble_proxy_is_running(void)
{
    return s_running;
}
