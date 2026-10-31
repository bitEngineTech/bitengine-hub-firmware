/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* pair_ble —— Muse 社区配对 v5 的 GATT 外设（docs/15_muse_direct_design_v1.md 5.2）。
 *
 * 改写自上游 muse-gadget-sdk @ e6fe517 的 esp32/main/ble_server.c（登记在 components/muse_link/UPSTREAM.md 的自写清单）。
 * 照抄：服务与特征 UUID、0xFE 分片帧格式与上限（收 8192 B、发 160 B/片）、命令分派的判定顺序（明文 / 加密 / 需要确认）、
 *       明文状态白名单、发送加锁与分片间隔、断开时作废会话。
 * 改动（设计附录 C 那一行）：
 *  - BT 起停交给 ble_window（这里只给服务表与 host 回调）；
 *  - 去掉 BLE OTA、unpair、旧 provision、Wi-Fi 配置、工厂测试服务、伴随服务、静音与常驻广播：ota / device.ota 一律
 *    error_unknown_action；
 *  - provision_v2 的校验照上游 Linux SDK（ssid / password 只要求是字符串 —— 开放网络时密码为空；上游 ESP32 要求非空，
 *    手机选"使用当前连接"时会被拒）；
 *  - 不再为每条命令建一个任务：要花时间的交给 muse_pair 的任务（回调 0 等待投递）；
 *  - 所有等锁都有上限（上游是 portMAX_DELAY）；
 *  - 厂商数据的"已配置"位恒为 0（配对剖面就是为配对开的）。
 */
#include "pair_ble.h"

#include <stdatomic.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "link_pairing.h"
#include "mbedtls/platform_util.h"
#include "nimble/nimble_port.h"
#include "pair_core.h"

static const char *TAG = "pair_ble";

/* 照抄上游 ble_server.c:53-66（程序原样拷贝，别手改；VERIFY.sh 逐字比对） */
// Service UUID 7fdd3d1c-38ea-46cf-8b46-314ecf5f240c (LE byte order)
static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(
    0x0c, 0x24, 0x5f, 0xcf, 0x4e, 0x31, 0x46, 0x8b,
    0xcf, 0x46, 0xea, 0x38, 0x1c, 0x3d, 0xdd, 0x7f);

// RX UUID 4d593029-28a2-4a6e-a1f0-3c2d5e8f9b01
static const ble_uuid128_t RX_UUID = BLE_UUID128_INIT(
    0x01, 0x9b, 0x8f, 0x5e, 0x2d, 0x3c, 0xf0, 0xa1,
    0x6e, 0x4a, 0xa2, 0x28, 0x29, 0x30, 0x59, 0x4d);

// TX UUID d75dc4ca-7b2b-4e9c-8f0a-1d2e3f4a5b6c
static const ble_uuid128_t TX_UUID = BLE_UUID128_INIT(
    0x6c, 0x5b, 0x4a, 0x3f, 0x2e, 0x1d, 0x0a, 0x8f,
    0x9c, 0x4e, 0x2b, 0x7b, 0xca, 0xc4, 0x5d, 0xd7);

#define MAX_NOTIFY_CHUNK   160
#define CHUNK_HEADER_BYTES 3
#define CHUNK_MAGIC        0xFE
#define MAX_RX_TOTAL_BYTES 8192
#define RX_LOCK_MS         1000      /* host 任务里等收包锁的上限（上游 portMAX_DELAY） */
#define TX_LOCK_MS         15000     /* 一条最长 255 片 × 50 ms ≈ 12.8 s */
#define CHUNK_GAP_MS       50

static pair_ble_cb_t     s_cb;
static const char       *s_device_name;
static uint16_t          s_rx_handle, s_tx_handle;
static volatile uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool     s_subscribed;
static volatile bool     s_shutting_down;
static volatile bool     s_bt_dead;          /* ble_window 要强制拆 BT 了（pair_ble_mark_dead）：shutdown 也不许再调 ble_gap_* */
static bool              s_shutdown_done;    /* pair_ble_shutdown 已经跑过（幂等） */
static volatile bool     s_advertising_active;
static volatile bool     s_plaintext_status_blocked;
static volatile uint16_t s_mtu = 23;
static SemaphoreHandle_t s_tx_mutex, s_rx_mutex;
static char              s_last_status[64] = "idle";
static atomic_bool       s_device_info_pending = ATOMIC_VAR_INIT(false);

static uint8_t *s_rx_buf;
static size_t   s_rx_len, s_rx_cap;
static uint8_t  s_rx_total, s_rx_count, s_rx_next_idx;

static void start_advertising(void);

/* ---------------------------------------------------------------- provision 参数 */

static char *dup_str(const char *s)
{
    if (!s) return NULL;
    const size_t n = strlen(s);
    char *out = malloc(n + 1);
    if (out) memcpy(out, s, n + 1);
    return out;
}

static void secure_free_str(char *s)
{
    if (!s) return;
    mbedtls_platform_zeroize(s, strlen(s));
    free(s);
}

void pair_prov_free(pair_prov_t *p)
{
    if (!p) return;
    secure_free_str(p->access_token);
    secure_free_str(p->refresh_token);
    secure_free_str(p->api_url_v2);
    secure_free_str(p->noise_host);
    free(p);
}

static void wipe_json_strings(cJSON *item)
{
    for (cJSON *cur = item; cur; cur = cur->next) {
        if (cur->valuestring) mbedtls_platform_zeroize(cur->valuestring, strlen(cur->valuestring));
        if (cur->child) wipe_json_strings(cur->child);
    }
}

static void delete_command_json(cJSON *root, bool decrypted)
{
    if (decrypted) wipe_json_strings(root);   /* 解密后的命令里有令牌 */
    cJSON_Delete(root);
}

static const char *json_str(cJSON *root, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(root, key);
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : NULL;
}

/* ---------------------------------------------------------------- 发送（照抄上游 652-757，等锁加上限） */

static bool notify_payload(const uint8_t *data, size_t len)
{
    if (s_shutting_down) return false;
    if (!s_subscribed || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return false;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) return false;
    int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_handle, om);
    if (rc != 0) ESP_LOGW(TAG, "notify rc=%d", rc);
    return rc == 0;
}

static bool plaintext_status_allowed(const char *status)
{
    return strcmp(status, "error_encryption_required") == 0
           || strcmp(status, "error_pairing_invalid_hello") == 0
           || strcmp(status, "error_pairing_unavailable") == 0
           || strcmp(status, "error_pairing_decrypt") == 0;
}

/* 调用方持 TX 锁。一条记录的计数器已经用掉，相位变化也要把它发完；只有新的加密会话会让剩下的片作废。 */
static bool send_chunked_locked(const char *data, uint32_t record_generation)
{
    if (!data || s_shutting_down) return false;
    const size_t total_len = strlen(data);
    size_t notify_max = s_mtu;
    if (notify_max <= 3) return false;
    notify_max -= 3;
    if (notify_max > MAX_NOTIFY_CHUNK) notify_max = MAX_NOTIFY_CHUNK;
    if (notify_max <= CHUNK_HEADER_BYTES) return false;
    const size_t usable = notify_max - CHUNK_HEADER_BYTES;
    size_t total_chunks = (total_len + usable - 1) / usable;
    if (total_chunks == 0) total_chunks = 1;
    if (total_chunks > 255) {
        ESP_LOGW(TAG, "TX message exceeds BLE framing limit");
        return false;
    }
    uint8_t buf[MAX_NOTIFY_CHUNK];
    for (size_t i = 0; i < total_chunks; i++) {
        if (record_generation != 0 && !link_pairing_record_session_is_current(record_generation)) return false;
        const size_t off = i * usable;
        const size_t flen = (off + usable > total_len) ? (total_len - off) : usable;
        buf[0] = CHUNK_MAGIC;
        buf[1] = (uint8_t)i;
        buf[2] = (uint8_t)total_chunks;
        memcpy(buf + CHUNK_HEADER_BYTES, data + off, flen);
        if (!notify_payload(buf, flen + CHUNK_HEADER_BYTES)) return false;
        vTaskDelay(pdMS_TO_TICKS(CHUNK_GAP_MS));
    }
    return true;
}

static bool send_status_gen(const char *status, uint32_t generation)
{
    if (!status || !s_tx_mutex || s_shutting_down) return false;
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(TX_LOCK_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "TX 锁等超时，状态 %s 没发", status);
        return false;
    }
    bool sent = false;
    uint32_t record_generation = 0;
    char *encrypted = link_pairing_encrypt_status(status, generation, &record_generation);
    if (encrypted) {
        snprintf(s_last_status, sizeof(s_last_status), "encrypted_status");
        sent = send_chunked_locked(encrypted, record_generation);
        free(encrypted);
    } else if (generation != 0 || s_plaintext_status_blocked || !plaintext_status_allowed(status)) {
        ESP_LOGW(TAG, "plaintext status suppressed: %s", status);
    } else {
        size_t n = strlen(status);
        if (n >= sizeof(s_last_status)) n = sizeof(s_last_status) - 1;
        memcpy(s_last_status, status, n);
        s_last_status[n] = '\0';
        sent = notify_payload((const uint8_t *)s_last_status, n);
    }
    xSemaphoreGive(s_tx_mutex);
    if (sent) ESP_LOGI(TAG, "status: %s", status);
    return sent;
}

bool pair_ble_send_status(const char *status)
{
    return send_status_gen(status, 0);
}

bool pair_ble_send_pairing_status(const char *status, uint32_t generation)
{
    return generation != 0 && send_status_gen(status, generation);
}

bool pair_ble_send_chunked(const char *json)
{
    if (!json || !s_tx_mutex || s_shutting_down) return false;
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(TX_LOCK_MS)) != pdTRUE) return false;
    const bool sent = send_chunked_locked(json, 0);
    xSemaphoreGive(s_tx_mutex);
    return sent;
}

bool pair_ble_send_encrypted_json(const char *json, uint32_t generation)
{
    if (!json || generation == 0 || !s_tx_mutex || s_shutting_down) return false;
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(TX_LOCK_MS)) != pdTRUE) return false;
    uint32_t record_generation = 0;
    char *encrypted = link_pairing_encrypt_json(json, generation, &record_generation);
    const bool sent = encrypted && send_chunked_locked(encrypted, record_generation);
    free(encrypted);
    xSemaphoreGive(s_tx_mutex);
    return sent;
}

/* ---------------------------------------------------------------- 断开 */

/* 关窗之后谁都不许再碰 NimBLE：s_shutting_down 只在 TX 锁里置位（pair_ble_shutdown），下面几处"查标志 + 调 NimBLE"
   也都在 TX 锁里做 —— 不然查完标志、还没调用的那一刻窗口关了，调用就打到已经拆掉的栈上（2026-10-22 审计）。 */
static atomic_bool s_dc_pending = ATOMIC_VAR_INIT(false);   /* 见 pair_ble_delayed_disconnect */

static void delayed_disconnect_task(void *arg)
{
    const uint32_t ms = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(ms));
    if (s_tx_mutex && xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(TX_LOCK_MS)) == pdTRUE) {
        const uint16_t conn = s_conn_handle;
        if (!s_shutting_down && conn != BLE_HS_CONN_HANDLE_NONE) {
            ESP_LOGI(TAG, "delayed disconnect after %u ms", (unsigned)ms);
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        xSemaphoreGive(s_tx_mutex);
    }
    atomic_store(&s_dc_pending, false);
    vTaskDelete(NULL);
}

/* 同一时刻只允许一个延迟断开任务：附近的人连发坏帧，每帧一个 2 KB 任务会把内部堆耗光（上游同形，2026-10-22 审计） */
void pair_ble_delayed_disconnect(uint32_t ms)
{
    if (atomic_exchange(&s_dc_pending, true)) return;      /* 已经有一个在等了 */
    if (xTaskCreate(delayed_disconnect_task, "pair_dc", 2048, (void *)(uintptr_t)ms, 4, NULL) != pdPASS) {
        atomic_store(&s_dc_pending, false);
        const uint16_t conn = s_conn_handle;   /* 任务都建不了就当场断 */
        if (conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

typedef struct {
    struct ble_npl_event event;
    uint32_t generation;
} pairing_disconnect_event_t;

/* 在 host 队列里做（照抄上游 1041-1065）：GAP 事件与 client_hello 分派共用这个队列，条件复位与断开之间不会插进新会话 */
static void disconnect_pairing_session_event(struct ble_npl_event *event)
{
    pairing_disconnect_event_t *work = ble_npl_event_get_arg(event);
    const uint32_t generation = work->generation;
    ble_npl_event_deinit(event);
    free(work);
    if (!s_shutting_down && link_pairing_reset_session(generation)) {
        const uint16_t conn = s_conn_handle;
        if (conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

/* 不能在 NimBLE host 任务里调（要拿 TX 锁；host 任务里直接 link_pairing_reset_session 即可） */
void pair_ble_disconnect_pairing_session(uint32_t generation)
{
    if (generation == 0) return;
    if (!s_tx_mutex || xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(TX_LOCK_MS)) != pdTRUE) {
        (void)link_pairing_reset_session(generation);   /* 拿不到锁也要作废会话密钥 */
        return;
    }
    if (s_shutting_down) {
        xSemaphoreGive(s_tx_mutex);
        (void)link_pairing_reset_session(generation);
        return;
    }
    pairing_disconnect_event_t *work = calloc(1, sizeof(*work));
    if (!work) {
        xSemaphoreGive(s_tx_mutex);
        (void)link_pairing_reset_session(generation);   /* 没内存也要作废会话密钥 */
        ESP_LOGE(TAG, "failed to queue pairing disconnect");
        return;
    }
    work->generation = generation;
    ble_npl_event_init(&work->event, disconnect_pairing_session_event, work);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &work->event);
    xSemaphoreGive(s_tx_mutex);
}

/* ---------------------------------------------------------------- 命令分派（照抄上游 268-504 的判定顺序） */

/* 必须加密、且必须先按键确认的动作。ota / device.ota / unpair / set_* 我们不实现，不在这里：
   加密送来一律 error_unknown_action（设计 5.2）。 */
static bool is_sensitive_setup_action(const char *act)
{
    return strcmp(act, "provision") == 0 || strcmp(act, "provision_v2") == 0 || strcmp(act, "wifi_scan") == 0;
}

static bool is_exact_client_finished(cJSON *root)
{
    cJSON *only = cJSON_IsObject(root) ? root->child : NULL;
    return only && !only->next && only->string && strcmp(only->string, "action") == 0
           && cJSON_IsString(only) && only->valuestring
           && strcmp(only->valuestring, "pairing_client_finished") == 0;
}

static void handle_provision_v2(cJSON *root)
{
    const bool ssid_is_str = cJSON_IsString(cJSON_GetObjectItem(root, "ssid"));
    const bool pass_is_str = cJSON_IsString(cJSON_GetObjectItem(root, "password"));
    const char *at = json_str(root, "access_token");
    const char *rt = json_str(root, "refresh_token");
    if (!pair_provision_ok(ssid_is_str, pass_is_str, at, rt, json_str(root, "token_type"))) {
        pair_ble_send_status("error_missing_credentials");
        return;
    }
    pair_prov_t *p = calloc(1, sizeof(*p));
    if (!p) {
        pair_ble_send_status("error_operation_in_progress");
        return;
    }
    p->access_token = dup_str(at);
    p->refresh_token = dup_str(rt);
    const char *au2 = json_str(root, "api_url_v2");
    const char *nh = json_str(root, "noise_host");
    if (au2 && *au2) p->api_url_v2 = dup_str(au2);
    if (nh && *nh) p->noise_host = dup_str(nh);
    if (!p->access_token || !p->refresh_token || (au2 && *au2 && !p->api_url_v2) || (nh && *nh && !p->noise_host)) {
        pair_prov_free(p);
        pair_ble_send_status("error_operation_in_progress");
        return;
    }
    p->generation = link_pairing_mark_provisioning_active();
    if (p->generation == 0) {
        pair_prov_free(p);
        pair_ble_send_status("error_pairing_confirm_required");
        return;
    }
    const uint32_t generation = p->generation;
    if (!s_cb.on_provision || !s_cb.on_provision(p)) {
        pair_prov_free(p);            /* 没接住（已经在 provisioning 里 / 队列满）：照 Linux SDK 回 operation_in_progress */
        pair_ble_send_pairing_status("error_operation_in_progress", generation);
    }
}

static void dispatch_command_ex(const uint8_t *data, size_t len, bool decrypted)
{
    if (!pair_json_depth_valid(data, len)) {
        pair_ble_send_status("error_invalid_command");
        return;
    }
    cJSON *root = cJSON_ParseWithLength((const char *)data, len);
    if (!root) {
        ESP_LOGW(TAG, "RX invalid JSON (%d bytes)", (int)len);
        pair_ble_send_status("error_invalid_command");
        return;
    }
    const char *act = json_str(root, "action");
    if (!act) act = "";
    ESP_LOGI(TAG, "RX action: %.32s%s", act, decrypted ? " (encrypted)" : "");   /* 攻击者可控，截断 */

    if (!decrypted && strcmp(act, "pairing_client_hello") == 0) {
        char *reply = NULL;
        const char *err = link_pairing_handle_client_hello(root, &reply);
        if (err) {
            pair_ble_send_status(err);
        } else if (reply) {
            s_plaintext_status_blocked = true;
            pair_ble_send_chunked(reply);
            free(reply);
        }
    } else if (!decrypted && strcmp(act, "pairing_encrypted") == 0) {
        char *plain = NULL;
        const char *err = link_pairing_decrypt_command(root, &plain);
        if (err) {
            pair_ble_send_status(err);
            if (s_cb.on_decrypt_failed) s_cb.on_decrypt_failed();
            pair_ble_delayed_disconnect(300);
        } else if (plain) {
            dispatch_command_ex((const uint8_t *)plain, strlen(plain), true);
            mbedtls_platform_zeroize(plain, strlen(plain));
            free(plain);
        }
    } else if (!decrypted && s_plaintext_status_blocked) {
        ESP_LOGW(TAG, "plaintext command suppressed after pairing ready: %s", act);
    } else if (!decrypted && is_sensitive_setup_action(act) && link_pairing_plaintext_setup_blocked()) {
        pair_ble_send_status("error_encryption_required");
    } else if (decrypted && strcmp(act, "pairing_client_finished") == 0) {
        const uint32_t generation = is_exact_client_finished(root) ? link_pairing_handle_client_finished() : 0;
        if (generation) {
            if (s_cb.on_pairing_client_finished) s_cb.on_pairing_client_finished(generation);
        } else {
            pair_ble_send_status("error_pairing_decrypt");
            link_pairing_reset();
            pair_ble_delayed_disconnect(300);
        }
    } else if (decrypted && is_sensitive_setup_action(act) && !link_pairing_session_confirmed()) {
        pair_ble_send_status("error_pairing_confirm_required");
    } else if (decrypted && strcmp(act, "wifi_scan") == 0) {
        if (s_cb.on_wifi_scan) s_cb.on_wifi_scan(link_pairing_session_generation());
    } else if (decrypted && strcmp(act, "provision_v2") == 0) {
        handle_provision_v2(root);
    } else if (decrypted) {
        pair_ble_send_status("error_unknown_action");      /* 含 ota / device.ota / unpair / provision（旧） */
    } else if (strcmp(act, "get_device_info") == 0) {
        if (!atomic_exchange(&s_device_info_pending, true)) {
            if (s_cb.on_get_device_info) s_cb.on_get_device_info();
            else atomic_store(&s_device_info_pending, false);
        }
    } else {
        pair_ble_send_status("error_unknown_action");
    }
    delete_command_json(root, decrypted);
}

void pair_ble_device_info_done(void)
{
    atomic_store(&s_device_info_pending, false);
}

/* ---------------------------------------------------------------- 收包重组（照抄上游 512-586，等锁加上限） */

static void rx_reset_locked(void)
{
    free(s_rx_buf);
    s_rx_buf = NULL;
    s_rx_len = s_rx_cap = 0;
    s_rx_total = s_rx_count = s_rx_next_idx = 0;
}

static void rx_reset(void)
{
    if (!s_rx_mutex) return;
    if (xSemaphoreTake(s_rx_mutex, pdMS_TO_TICKS(RX_LOCK_MS)) != pdTRUE) return;
    rx_reset_locked();
    xSemaphoreGive(s_rx_mutex);
}

static void handle_rx_write_locked(const uint8_t *data, size_t len, uint8_t **complete, size_t *complete_len)
{
    const uint8_t idx = data[1];
    const uint8_t total = data[2];
    const uint8_t *frag = data + CHUNK_HEADER_BYTES;
    const size_t flen = len - CHUNK_HEADER_BYTES;

    if (total == 0) { rx_reset_locked(); return; }
    if (idx == 0 || s_rx_total != total) {
        rx_reset_locked();
        s_rx_total = total;
        s_rx_cap = MAX_RX_TOTAL_BYTES;
        s_rx_buf = malloc(s_rx_cap);
        if (!s_rx_buf) { rx_reset_locked(); return; }
        s_rx_len = 0;
    }
    if (idx != s_rx_next_idx || idx >= s_rx_total) { rx_reset_locked(); return; }
    if (s_rx_len + flen > s_rx_cap) { rx_reset_locked(); return; }   /* S1：恶意分片吃不光内存 */
    memcpy(s_rx_buf + s_rx_len, frag, flen);
    s_rx_len += flen;
    s_rx_count++;
    s_rx_next_idx = idx + 1;
    if (s_rx_count < s_rx_total) return;
    *complete = s_rx_buf;
    *complete_len = s_rx_len;
    s_rx_buf = NULL;
    rx_reset_locked();
}

static void handle_rx_write(const uint8_t *data, size_t len)
{
    if (!s_rx_mutex) return;
    if (len < CHUNK_HEADER_BYTES || data[0] != CHUNK_MAGIC) {
        dispatch_command_ex(data, len, false);
        return;
    }
    uint8_t *complete = NULL;
    size_t complete_len = 0;
    if (xSemaphoreTake(s_rx_mutex, pdMS_TO_TICKS(RX_LOCK_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "RX 锁等超时，丢一片");
        return;
    }
    handle_rx_write_locked(data, len, &complete, &complete_len);
    xSemaphoreGive(s_rx_mutex);
    if (complete) {
        dispatch_command_ex(complete, complete_len, false);
        free(complete);
    }
}

/* ---------------------------------------------------------------- GATT */

static int rx_access_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (s_shutting_down) return BLE_ATT_ERR_UNLIKELY;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    const uint16_t total = OS_MBUF_PKTLEN(ctxt->om);
    if (total == 0) return 0;
    uint8_t *buf = malloc(total);
    if (!buf) return BLE_ATT_ERR_INSUFFICIENT_RES;
    uint16_t out_len = 0;
    const int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, total, &out_len);
    if (rc == 0 && out_len > 0) handle_rx_write(buf, out_len);
    free(buf);
    return 0;
}

static int tx_access_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (s_shutting_down) return BLE_ATT_ERR_UNLIKELY;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    const int rc = os_mbuf_append(ctxt->om, s_last_status, strlen(s_last_status));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_chr_def s_chrs[] = {
    {
        .uuid = &RX_UUID.u,
        .access_cb = rx_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .val_handle = &s_rx_handle,
    },
    {
        .uuid = &TX_UUID.u,
        .access_cb = tx_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_tx_handle,
    },
    { 0 },
};

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = s_chrs,
    },
    { 0 },
};

const struct ble_gatt_svc_def *pair_ble_svcs(void)
{
    return s_svcs;
}

/* ---------------------------------------------------------------- GAP（照抄上游 842-954） */

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (s_shutting_down) break;
        s_advertising_active = false;
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_plaintext_status_blocked = false;
            ESP_LOGI(TAG, "connected conn_handle=%d", event->connect.conn_handle);
        } else {
            ESP_LOGI(TAG, "connect failed status=%d", event->connect.status);
            start_advertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnect reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        s_mtu = 23;
        s_advertising_active = false;
        s_plaintext_status_blocked = false;
        rx_reset();
        link_pairing_reset();
        if (!s_shutting_down && s_cb.on_client_disconnected) s_cb.on_client_disconnected();
        if (!s_shutting_down) start_advertising();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_handle) {
            s_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "subscribe notify=%d", s_subscribed);
            if (s_subscribed && !s_shutting_down && s_cb.on_client_connected) s_cb.on_client_connected();
        }
        break;
    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value;
        ESP_LOGI(TAG, "mtu=%d", event->mtu.value);
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_advertising_active = false;
        break;
    default:
        break;
    }
    return 0;
}

static void start_advertising(void)
{
    if (s_shutting_down || !s_device_name) return;
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE || s_advertising_active) return;

    struct ble_gap_adv_params advp = {0};
    advp.conn_mode = BLE_GAP_CONN_MODE_UND;
    advp.disc_mode = BLE_GAP_DISC_MODE_GEN;

    /* 广播包：flags + 128-bit 服务 UUID + 厂商数据（0xFFFF 测试厂商号 + 1 字节"已配置"，配对剖面里恒为 0） */
    uint8_t mfg_data[] = { 0xFF, 0xFF, 0x00 };
    struct ble_hs_adv_fields adv = {0};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids128 = (ble_uuid128_t *)&SVC_UUID;
    adv.num_uuids128 = 1;
    adv.uuids128_is_complete = 1;
    adv.mfg_data = mfg_data;
    adv.mfg_data_len = sizeof(mfg_data);
    int rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields rc=%d", rc);
        return;
    }
    /* 扫描应答：完整名字（拆出来，UUID 与名字才都放得进 31 字节的传统广播） */
    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (uint8_t *)s_device_name;
    rsp.name_len = strlen(s_device_name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) ESP_LOGW(TAG, "adv_rsp_set_fields rc=%d", rc);

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &advp, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start rc=%d", rc);
    } else {
        s_advertising_active = true;
        ESP_LOGI(TAG, "advertising as %s", s_device_name);
    }
}

void pair_ble_on_sync(void)
{
    start_advertising();
}

void pair_ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset reason=%d", reason);
    s_advertising_active = false;
    s_plaintext_status_blocked = false;
    rx_reset();
    link_pairing_reset();
}

/* ---------------------------------------------------------------- 生命周期 */

esp_err_t pair_ble_init(const char *device_name, const pair_ble_cb_t *cb)
{
    if (!device_name || !*device_name || !cb) return ESP_ERR_INVALID_ARG;
    if (!s_rx_mutex) s_rx_mutex = xSemaphoreCreateMutex();
    if (!s_tx_mutex) s_tx_mutex = xSemaphoreCreateMutex();
    if (!s_rx_mutex || !s_tx_mutex) return ESP_ERR_NO_MEM;
    s_cb = *cb;
    s_device_name = device_name;
    s_shutting_down = false;
    s_bt_dead = false;
    s_shutdown_done = false;
    s_advertising_active = false;
    s_plaintext_status_blocked = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_subscribed = false;
    s_mtu = 23;
    atomic_store(&s_device_info_pending, false);
    return ESP_OK;
}

/* 在 TX 锁里置停止标志：等在途的发送（最长约 13 s）发完；之后的发送 / 断开投递都会看到标志而不碰 NimBLE */
static void set_shutting_down_locked_wait(void)
{
    if (s_shutting_down) return;
    const bool locked = s_tx_mutex && xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(TX_LOCK_MS)) == pdTRUE;
    s_shutting_down = true;
    if (locked) xSemaphoreGive(s_tx_mutex);
    else ESP_LOGW(TAG, "TX 锁等超时，照样置停止标志（在途的发送会看到标志后收手）");
}

void pair_ble_mark_dead(void)
{
    set_shutting_down_locked_wait();
    s_bt_dead = true;
}

void pair_ble_shutdown(void)
{
    if (s_shutdown_done) return;
    set_shutting_down_locked_wait();
    if (!s_bt_dead) {               /* ble_window 已经强制拆了 BT：NimBLE 的状态指针是 NULL，ble_gap_* 一调就 panic */
        int rc = ble_gap_adv_stop();
        if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGD(TAG, "adv_stop rc=%d", rc);
        const uint16_t conn = s_conn_handle;
        if (conn != BLE_HS_CONN_HANDLE_NONE) {
            rc = ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
            if (rc != 0) ESP_LOGD(TAG, "terminate rc=%d", rc);
            vTaskDelay(pdMS_TO_TICKS(200));   /* 让断开报文出去（照上游 shutdown_stack） */
        }
    }
    s_shutdown_done = true;
    rx_reset();
    link_pairing_reset();           /* 不管 BT 死没死都要做：它拿配对锁，等在途的 commit 落完盘（s_committed 靠它才准） */
}

bool pair_ble_has_connection(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}
