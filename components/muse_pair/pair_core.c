/* pair_core —— 见 pair_core.h。只依赖 libc 与 muse_cfg_core。 */
#include "pair_core.h"

#include <stdio.h>
#include <string.h>

#include "muse_cfg_core.h"

const char *pair_res_str(pair_res_t r)
{
    switch (r) {
    case PAIR_RES_NONE:            return "none";
    case PAIR_RES_OK:              return "ok";
    case PAIR_RES_WINDOW_TIMEOUT:  return "window_timeout";
    case PAIR_RES_CANCELLED:       return "cancelled";
    case PAIR_RES_DECRYPT_FAILED:  return "decrypt_failed";
    case PAIR_RES_CONFIRM_TIMEOUT: return "confirm_timeout";
    case PAIR_RES_AUTH_FAILED:     return "auth_failed";
    case PAIR_RES_STORAGE_FAILED:  return "error_storage";
    case PAIR_RES_NO_SDK_TOKEN:    return "no_sdk_token";
    case PAIR_RES_API_UNREACHABLE: return "api_unreachable";
    case PAIR_RES_BT_FAILED:       return "bt_failed";
    case PAIR_RES_NO_NETWORK:      return "no_network";
    case PAIR_RES_INTERNAL:        return "internal";
    case PAIR_RES_INTERRUPTED:     return "interrupted";
    case PAIR_RES_MAX:             break;
    }
    return "unknown";
}

void pair_res_encode(pair_res_t r, uint32_t epoch_s, uint8_t out[PAIR_RES_BLOB_LEN])
{
    if (!out) return;
    out[0] = (uint8_t)r;
    out[1] = (uint8_t)(epoch_s & 0xFF);
    out[2] = (uint8_t)((epoch_s >> 8) & 0xFF);
    out[3] = (uint8_t)((epoch_s >> 16) & 0xFF);
    out[4] = (uint8_t)((epoch_s >> 24) & 0xFF);
}

bool pair_res_decode(const uint8_t *in, size_t len, pair_res_t *r, uint32_t *epoch_s)
{
    if (!in || len < PAIR_RES_BLOB_LEN || in[0] == PAIR_RES_NONE || in[0] >= PAIR_RES_MAX) return false;
    if (r) *r = (pair_res_t)in[0];
    if (epoch_s) {
        *epoch_s = (uint32_t)in[1] | ((uint32_t)in[2] << 8) | ((uint32_t)in[3] << 16) | ((uint32_t)in[4] << 24);
    }
    return true;
}

bool pair_provision_ok(bool ssid_is_str, bool password_is_str, const char *access_token,
                       const char *refresh_token, const char *token_type)
{
    if (!ssid_is_str || !password_is_str) return false;
    if (!token_type || strcmp(token_type, "device") != 0) return false;
    return muse_token_valid(access_token, MUSE_ACCESS_TOKEN_MAX)
           && muse_token_valid(refresh_token, MUSE_REFRESH_TOKEN_MAX);
}

bool pair_json_depth_valid(const uint8_t *data, size_t len)
{
    if (!data) return false;
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < len; i++) {
        const uint8_t c = data[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') {
            quoted = true;
        } else if (c == '{' || c == '[') {
            if (++depth > 32) return false;
        } else if ((c == '}' || c == ']') && depth) {
            depth--;
        }
    }
    return true;
}

void pair_identity_format(const uint8_t mac[6], char *node_id, size_t node_cap, char *ble_name, size_t ble_cap,
                          char *mac_str, size_t mac_cap, char *device_id, size_t dev_cap)
{
    static const uint8_t zero[6] = {0};
    const uint8_t *m = mac ? mac : zero;
    if (node_id && node_cap) snprintf(node_id, node_cap, "homelink-%02x%02x%02x", m[3], m[4], m[5]);
    if (ble_name && ble_cap) snprintf(ble_name, ble_cap, "MuseGadget-%02X%02X%02X", m[3], m[4], m[5]);
    char ms[18];
    snprintf(ms, sizeof(ms), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    if (mac_str && mac_cap) snprintf(mac_str, mac_cap, "%s", ms);
    if (device_id && dev_cap) snprintf(device_id, dev_cap, "hatch-link:%s", ms);
}

size_t pair_wifi_scan_json(bool online, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    const char *json = online
        ? "{\"type\":\"wifi_scan_result\",\"networks\":[{\"ssid\":\"Use current connection\",\"rssi\":-40,\"secure\":false}]}"
        : "{\"type\":\"wifi_scan_result\",\"networks\":[]}";
    const size_t n = strlen(json);
    if (n + 1 > cap) {
        out[0] = '\0';
        return 0;
    }
    memcpy(out, json, n + 1);
    return n;
}

const char *pair_effective_api_url(const char *api_url_v2)
{
    return (api_url_v2 && *api_url_v2 && muse_api_url_valid(api_url_v2)) ? api_url_v2 : "";
}
