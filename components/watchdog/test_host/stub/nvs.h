#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef int nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
/* 内存里模拟一块 NVS blob：验证环形历史的持久化语义，不碰真 Flash。 */
extern char   g_nvs_blob[512];
extern size_t g_nvs_len;
extern int    g_nvs_fail;      /* 置 1 模拟 NVS 不可用 */
static inline esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h)
{ (void)ns; (void)m; if (g_nvs_fail) return ESP_FAIL; *h = 1; return ESP_OK; }
static inline void nvs_close(nvs_handle_t h) { (void)h; }
static inline esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
static inline esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *out, size_t *len)
{
    (void)h; (void)k;
    if (g_nvs_len == 0)   return ESP_ERR_NOT_FOUND;
    if (*len < g_nvs_len) return ESP_FAIL;
    memcpy(out, g_nvs_blob, g_nvs_len);
    *len = g_nvs_len;
    return ESP_OK;
}
static inline esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len)
{
    (void)h; (void)k;
    if (len > sizeof(g_nvs_blob)) return ESP_FAIL;
    memcpy(g_nvs_blob, v, len);
    g_nvs_len = len;
    return ESP_OK;
}
