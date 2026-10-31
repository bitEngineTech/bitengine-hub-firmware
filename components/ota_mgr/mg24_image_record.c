#include "mg24_image_record.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "mg24_image";

#define NVS_NAMESPACE   "mg24"
#define KEY_SHA         "img_sha"
#define KEY_SIZE        "img_size"
#define KEY_TIME        "img_time"
#define KEY_NVM3C       "nvm3c_max"     /* T9.25：这块芯片刷过的最大 NVM3 缓存条数 */

/* Anything below this is not a real wall-clock time — SNTP had not synced when
   the update ran. Recording 1970 would look like data, so record 0 instead. */
#define PLAUSIBLE_EPOCH 1600000000LL

static mg24_image_record_t s_record;
static bool s_loaded;

/* Load into a local and publish whole, with s_loaded set last: two HTTP
   handlers can call the getter concurrently, and a reader must never catch
   s_record mid-memset with s_loaded already true. ota_mg24_init() warms it at
   boot anyway, so this path normally runs single-threaded. */
static void load_from_nvs(void)
{
    mg24_image_record_t rec;
    memset(&rec, 0, sizeof(rec));

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(rec.sha256);
        if (nvs_get_str(h, KEY_SHA, rec.sha256, &len) == ESP_OK) {
            rec.valid = true;
            nvs_get_u32(h, KEY_SIZE, &rec.size);
            nvs_get_i64(h, KEY_TIME, &rec.flashed_at);
        }
        nvs_close(h);
    }
    /* namespace absent = never updated through the gateway = factory image */

    s_record = rec;
    s_loaded = true;
}

esp_err_t mg24_image_record_save_digest(const uint8_t digest[32], size_t len)
{
    if (!digest || len == 0) return ESP_ERR_INVALID_ARG;

    char hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }

    int64_t now = (int64_t)time(NULL);
    if (now < PLAUSIBLE_EPOCH) now = 0;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Write all three or none: a sha with a stale size/timestamp beside it
       would misreport which image this is. */
    err = nvs_set_str(h, KEY_SHA, hex);
    if (err == ESP_OK) err = nvs_set_u32(h, KEY_SIZE, (uint32_t)len);
    if (err == ESP_OK) err = nvs_set_i64(h, KEY_TIME, now);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "recording image failed: %s", esp_err_to_name(err));
        return err;
    }

    strlcpy(s_record.sha256, hex, sizeof(s_record.sha256));
    s_record.size       = (uint32_t)len;
    s_record.flashed_at = now;
    s_record.valid      = true;
    s_loaded            = true;

    ESP_LOGI(TAG, "recorded MG24 image sha256=%.16s… (%u bytes)",
             hex, (unsigned)len);
    return ESP_OK;
}

esp_err_t mg24_image_record_forget(void)
{
    /* 先对外撤掉"有效"，再动 NVS：读者宁可看到"没有记录"，也别看到半截 */
    s_record.valid = false;
    s_record.sha256[0] = '\0';
    s_record.size = 0;
    s_record.flashed_at = 0;
    s_loaded = true;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    /* 只删指纹三件，NVM3 底线（KEY_NVM3C）描述的是芯片，留着 */
    const char *keys[] = { KEY_SHA, KEY_SIZE, KEY_TIME };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        esp_err_t e = nvs_erase_key(h, keys[i]);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND && err == ESP_OK) err = e;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "forgetting image record failed: %s", esp_err_to_name(err));
    }
    return err;
}

const mg24_image_record_t *mg24_image_record_get(void)
{
    if (!s_loaded) load_from_nvs();
    return &s_record;
}

int32_t mg24_image_record_nvm3_floor(void)
{
    uint32_t v = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u32(h, KEY_NVM3C, &v);           /* 没有这一条 = 只刷过出厂的 */
        nvs_close(h);
    }
    return (v > MG24_NVM3C_FLOOR_DEFAULT && v < 1000000u) ? (int32_t)v : MG24_NVM3C_FLOOR_DEFAULT;
}

esp_err_t mg24_image_record_note_nvm3(int32_t nvm3c)
{
    if (nvm3c <= mg24_image_record_nvm3_floor()) return ESP_OK;   /* 只升不降 */
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u32(h, KEY_NVM3C, (uint32_t)nvm3c);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) {
        /* 写不进去：底线停在旧值。只会放过"比这次低、比旧底线高"的镜像，记下来别吞掉 */
        ESP_LOGW(TAG, "recording NVM3 cache floor %d failed: %s", (int)nvm3c, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "NVM3 cache floor raised to %d", (int)nvm3c);
    return ESP_OK;
}
