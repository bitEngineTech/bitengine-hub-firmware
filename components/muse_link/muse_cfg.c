/* muse_cfg —— hubdb 命名空间 muse 的读写（docs/15 设计 4.4）。见 include/muse_cfg.h。
 *
 * 每个值都是 hub_store 的一个键，格式版本 1。字符串存时不带结尾 NUL，读出来补上。
 */
#include "muse_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_identity.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "hub_model.h"
#include "hub_registry.h"
#include "hub_store.h"
#include "mbedtls/platform_util.h"
#include "sdkconfig.h"

static const char *TAG = "muse_cfg";

#define VER          1
#define GET_TMO_MS   500      /* 网页（httpd 单任务）会调到这里：hub_store 平时几毫秒就答，卡住了别把 httpd 堵上几秒 */

#define K_EN         "en"
#define K_SDK_TOK    "sdk_tok"
#define K_DISP_NAME  "disp_name"
#define K_ACC_TOK    "acc_tok"
#define K_REF_TOK    "ref_tok"
#define K_API_URL2   "api_url2"
#define K_NOISE_HOST "noise_host"
#define K_VM_ID      "vm_id"
#define K_PERM       "perm"
#define K_CHAT_SID   "chat_sid"
#define K_NOTIFY     "notify"

static esp_err_t put_str(const char *key, const char *s)
{
    return hub_store_put(HUB_STORE_NS_MUSE, key, VER, s, strlen(s));
}

/* 读一个字符串键到 out（补 NUL）。装不下 ESP_ERR_INVALID_SIZE；没有 ESP_ERR_NOT_FOUND。 */
static esp_err_t get_str(const char *key, char *out, size_t cap)
{
    if (!out || cap < 2) return ESP_ERR_INVALID_ARG;
    uint8_t ver = 0;
    size_t len = cap - 1;
    esp_err_t err = hub_store_get(HUB_STORE_NS_MUSE, key, VER, &ver, out, &len, GET_TMO_MS);
    if (err != ESP_OK) {
        out[0] = '\0';
        return err;
    }
    out[len] = '\0';
    if (strlen(out) != len) {      /* 中间带 NUL：不是我们写进去的东西 */
        mbedtls_platform_zeroize(out, cap);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

/* hub_store / get_str 的错误里哪些表示"确实没有可用的值"（没存、版本比本固件新、超长、带 NUL）；其余是一时读不到 */
static bool definitely_absent(esp_err_t err)
{
    return err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_VERSION || err == ESP_ERR_INVALID_SIZE
           || err == ESP_ERR_INVALID_RESPONSE;
}

/* 键在不在：ESP_OK = 在（装不下也是有值）；ESP_ERR_NOT_FOUND = 确定不在（版本比本固件新按 hub_store 的约定当作没有）；
   其余 = 一时读不到（超时、hub_store 没就绪……），调用方不能当成"不在" */
static esp_err_t key_state(const char *key)
{
    uint8_t ver = 0;
    uint8_t probe[1];
    size_t len = sizeof(probe);
    const esp_err_t err = hub_store_get(HUB_STORE_NS_MUSE, key, VER, &ver, probe, &len, GET_TMO_MS);
    mbedtls_platform_zeroize(probe, sizeof(probe));
    if (err == ESP_OK || err == ESP_ERR_INVALID_SIZE) return ESP_OK;
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_VERSION) return ESP_ERR_NOT_FOUND;
    return err;
}

/* ---------------------------------------------------------------- SDK token */

esp_err_t muse_cfg_set_sdk_token(const char *token)
{
    if (!token || !*token) {
        ESP_LOGI(TAG, "SDK token 删除");
        return hub_store_erase(HUB_STORE_NS_MUSE, K_SDK_TOK);
    }
    if (!muse_sdk_token_valid(token)) return ESP_ERR_INVALID_ARG;
    char hint[32];
    muse_secret_hint(token, hint, sizeof(hint));
    ESP_LOGI(TAG, "SDK token 已设置：%s", hint);
    return put_str(K_SDK_TOK, token);
}

esp_err_t muse_cfg_get_sdk_token(char *out, size_t cap)
{
    if (!out || cap < MUSE_SDK_TOKEN_LEN + 1) return ESP_ERR_INVALID_ARG;
    esp_err_t err = get_str(K_SDK_TOK, out, cap);
    if (err != ESP_OK) return definitely_absent(err) ? ESP_ERR_NOT_FOUND : err;
    if (!muse_sdk_token_valid(out)) {
        ESP_LOGW(TAG, "存着的 SDK token 格式不对，当作没有");
        mbedtls_platform_zeroize(out, cap);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

bool muse_cfg_sdk_token_set(void)
{
    char tok[MUSE_SDK_TOKEN_LEN + 1];
    const bool ok = muse_cfg_get_sdk_token(tok, sizeof(tok)) == ESP_OK;
    mbedtls_platform_zeroize(tok, sizeof(tok));
    return ok;
}

/* ---------------------------------------------------------------- 开关、显示名 */

esp_err_t muse_cfg_set_enabled(bool en)
{
    const uint8_t v = en ? 1 : 0;
    return hub_store_put(HUB_STORE_NS_MUSE, K_EN, VER, &v, 1);
}

/* ESP_OK + *en；没存过 / 版本更新 / 长度不对 = 确定的 false；其余错误原样返回 */
static esp_err_t read_enabled(bool *en)
{
    uint8_t ver = 0, v[2] = {0};
    size_t len = sizeof(v);
    const esp_err_t err = hub_store_get(HUB_STORE_NS_MUSE, K_EN, VER, &ver, v, &len, GET_TMO_MS);
    if (err == ESP_OK) {
        *en = len == 1 && v[0] == 1;
        return ESP_OK;
    }
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_VERSION || err == ESP_ERR_INVALID_SIZE) {
        *en = false;
        return ESP_OK;
    }
    return err;
}

bool muse_cfg_enabled(void)
{
    bool en = false;
    return read_enabled(&en) == ESP_OK && en;
}

esp_err_t muse_cfg_set_display_name(const char *name)
{
    if (!name || !*name) return hub_store_erase(HUB_STORE_NS_MUSE, K_DISP_NAME);
    if (!muse_display_name_valid(name)) return ESP_ERR_INVALID_ARG;
    return put_str(K_DISP_NAME, name);
}

void muse_cfg_get_display_name(char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (get_str(K_DISP_NAME, out, cap) == ESP_OK && muse_display_name_valid(out)) return;
    /* 默认（设计 4.3）："bitEngine Hub " + 序列号后 4 位；没有出厂身份时用 MAC 后 4 位 */
    const char *sn = device_identity_valid() ? device_identity_serial() : NULL;
    const size_t sn_len = sn ? strlen(sn) : 0;
    if (sn_len >= 4) {
        snprintf(out, cap, "bitEngine Hub %s", sn + sn_len - 4);
        return;
    }
    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);   /* 不用 esp_efuse_mac_get_default：C5 上越界（fctry-identity-docs-traps） */
    snprintf(out, cap, "bitEngine Hub %02X%02X", mac[4], mac[5]);
}

/* ---------------------------------------------------------------- 配对凭据 */

static void erase_pairing(void)
{
    (void)hub_store_erase(HUB_STORE_NS_MUSE, K_ACC_TOK);
    (void)hub_store_erase(HUB_STORE_NS_MUSE, K_REF_TOK);
    (void)hub_store_erase(HUB_STORE_NS_MUSE, K_API_URL2);
    (void)hub_store_erase(HUB_STORE_NS_MUSE, K_NOISE_HOST);
}

/* 读回比对：长度与内容都要一样（读出的缓冲用完清零）。比的是自己刚写进去的值，不是认证判断，不需要常量时间 */
static bool readback_equals(const char *key, const char *want, size_t cap)
{
    char *buf = calloc(1, cap);
    if (!buf) return false;
    const bool ok = get_str(key, buf, cap) == ESP_OK && strcmp(buf, want) == 0;
    mbedtls_platform_zeroize(buf, cap);
    free(buf);
    return ok;
}

esp_err_t muse_cfg_save_pairing(const muse_cfg_pairing_t *p, uint32_t flush_timeout_ms)
{
    if (!p) return ESP_ERR_INVALID_ARG;
    const char *api = p->api_url_v2 ? p->api_url_v2 : "";
    const char *noise = p->noise_host ? p->noise_host : "";
    if (!muse_token_valid(p->access_token, MUSE_ACCESS_TOKEN_MAX)
        || !muse_token_valid(p->refresh_token, MUSE_REFRESH_TOKEN_MAX)
        || !muse_api_url_valid(api) || !muse_noise_host_valid(noise)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = put_str(K_ACC_TOK, p->access_token);
    if (err == ESP_OK) err = put_str(K_REF_TOK, p->refresh_token);
    if (err == ESP_OK) err = *api ? put_str(K_API_URL2, api) : hub_store_erase(HUB_STORE_NS_MUSE, K_API_URL2);
    if (err == ESP_OK) err = *noise ? put_str(K_NOISE_HOST, noise) : hub_store_erase(HUB_STORE_NS_MUSE, K_NOISE_HOST);
    /* put 有 2 s 防抖：上游要求 commit 回调同步提交，所以立刻 flush、再逐个读回（"先写后回"，同 device.unpair） */
    if (err == ESP_OK) err = hub_store_flush(flush_timeout_ms);
    if (err == ESP_OK) {
        const bool same = readback_equals(K_ACC_TOK, p->access_token, MUSE_ACCESS_TOKEN_MAX + 1)
                          && readback_equals(K_REF_TOK, p->refresh_token, MUSE_REFRESH_TOKEN_MAX + 1)
                          && (!*api || readback_equals(K_API_URL2, api, MUSE_URL_MAX + 1))
                          && (!*noise || readback_equals(K_NOISE_HOST, noise, MUSE_URL_MAX + 1));
        if (!same) err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配对凭据落盘失败: %s —— 删掉已写的，不留半套", esp_err_to_name(err));
        erase_pairing();
        (void)hub_store_flush(flush_timeout_ms);
        return err;
    }
    char hint[32];
    muse_secret_hint(p->access_token, hint, sizeof(hint));
    ESP_LOGI(TAG, "配对凭据已落盘：access token %s、api_url2 %s、noise_host %s", hint,
             *api ? "已下发" : "默认", *noise ? "已下发" : "默认");
    return ESP_OK;
}

bool muse_cfg_paired(void)
{
    return key_state(K_ACC_TOK) == ESP_OK;
}

esp_err_t muse_cfg_load_flags(bool *enabled, bool *paired)
{
    if (!enabled || !paired) return ESP_ERR_INVALID_ARG;
    bool en = false;
    esp_err_t err = read_enabled(&en);
    if (err != ESP_OK) return err;
    err = key_state(K_ACC_TOK);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) return err;
    *enabled = en;
    *paired = err == ESP_OK;
    return ESP_OK;
}

/* ---------------------------------------------------------------- 会话层用（M3） */

/* 只有"确实没有可用的"才给 NOT_FOUND（见头文件）；超时之类原样返回 */
static esp_err_t get_token(const char *key, char *out, size_t cap, size_t max)
{
    if (!out || cap < max + 1) return ESP_ERR_INVALID_ARG;
    const esp_err_t err = get_str(key, out, cap);
    if (definitely_absent(err)) {
        if (err != ESP_ERR_NOT_FOUND) ESP_LOGW(TAG, "存着的 %s 读不成可用的令牌（%s），当作没有", key, esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) return err;
    if (!muse_token_valid(out, max)) {
        ESP_LOGW(TAG, "存着的 %s 不合格，当作没有", key);
        mbedtls_platform_zeroize(out, cap);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t muse_cfg_get_access_token(char *out, size_t cap)
{
    return get_token(K_ACC_TOK, out, cap, MUSE_ACCESS_TOKEN_MAX);
}

esp_err_t muse_cfg_get_refresh_token(char *out, size_t cap)
{
    return get_token(K_REF_TOK, out, cap, MUSE_REFRESH_TOKEN_MAX);
}

esp_err_t muse_cfg_save_tokens(const char *access_token, const char *refresh_token, uint32_t flush_timeout_ms)
{
    if (!muse_token_valid(access_token, MUSE_ACCESS_TOKEN_MAX) || !muse_token_valid(refresh_token, MUSE_REFRESH_TOKEN_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = put_str(K_ACC_TOK, access_token);
    if (err == ESP_OK) err = put_str(K_REF_TOK, refresh_token);
    if (err == ESP_OK) err = hub_store_flush(flush_timeout_ms);
    if (err == ESP_OK) {
        const bool same = readback_equals(K_ACC_TOK, access_token, MUSE_ACCESS_TOKEN_MAX + 1)
                          && readback_equals(K_REF_TOK, refresh_token, MUSE_REFRESH_TOKEN_MAX + 1);
        if (!same) err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "新令牌落盘失败: %s", esp_err_to_name(err));
        return err;
    }
    char hint[32];
    muse_secret_hint(access_token, hint, sizeof(hint));
    ESP_LOGI(TAG, "设备令牌已更新：access token %s", hint);
    return ESP_OK;
}

esp_err_t muse_cfg_clear_pairing(uint32_t flush_timeout_ms)
{
    esp_err_t err = hub_store_erase(HUB_STORE_NS_MUSE, K_ACC_TOK);
    if (err == ESP_OK) err = hub_store_erase(HUB_STORE_NS_MUSE, K_REF_TOK);
    if (err == ESP_OK) err = hub_store_erase(HUB_STORE_NS_MUSE, K_VM_ID);
    if (err == ESP_OK) err = hub_store_flush(flush_timeout_ms);
    /* 读回确认：三个键都**确定**不在了才算删掉（4.13：确认后才回 accepted，避免"App 以为删了、设备还连着"）；
       读回超时不算确认 */
    if (err == ESP_OK && (key_state(K_ACC_TOK) != ESP_ERR_NOT_FOUND || key_state(K_REF_TOK) != ESP_ERR_NOT_FOUND
                          || key_state(K_VM_ID) != ESP_ERR_NOT_FOUND)) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "清配对凭据失败: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "配对凭据已清除（SDK token 与设置保留）");
    return ESP_OK;
}

/* 存着的合格就用它；确实没有（或不合格）用默认；一时读不到返回错误、不写 out（见头文件） */
static esp_err_t get_or_default(const char *key, bool (*valid)(const char *), const char *dflt, char *out, size_t cap)
{
    if (!out || cap < 2) return ESP_ERR_INVALID_ARG;
    char *tmp = calloc(1, cap);
    if (!tmp) return ESP_ERR_NO_MEM;
    const esp_err_t err = get_str(key, tmp, cap);
    if (err != ESP_OK && !definitely_absent(err)) {
        free(tmp);
        return err;
    }
    if (err == ESP_OK && tmp[0] && valid(tmp)) snprintf(out, cap, "%s", tmp);
    else snprintf(out, cap, "%s", dflt);
    free(tmp);
    return ESP_OK;
}

esp_err_t muse_cfg_get_api_url(char *out, size_t cap)
{
    return get_or_default(K_API_URL2, muse_api_url_valid, CONFIG_BITENGINE_MUSE_API_URL, out, cap);
}

esp_err_t muse_cfg_get_noise_host(char *out, size_t cap)
{
    return get_or_default(K_NOISE_HOST, muse_noise_host_valid, CONFIG_BITENGINE_MUSE_NOISE_HOST, out, cap);
}

esp_err_t muse_cfg_get_vm_id(char *out, size_t cap)
{
    if (!out || cap < MUSE_VM_ID_MAX + 1) return ESP_ERR_INVALID_ARG;
    esp_err_t err = get_str(K_VM_ID, out, cap);
    if (err != ESP_OK) return definitely_absent(err) ? ESP_ERR_NOT_FOUND : err;
    if (!muse_vm_id_valid(out)) {
        out[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t muse_cfg_get_chat_sid(char *out, size_t cap)
{
    if (!out || cap < MUSE_CHAT_SID_LEN + 1) return ESP_ERR_INVALID_ARG;
    const esp_err_t err = get_str(K_CHAT_SID, out, cap);
    if (err != ESP_OK) return definitely_absent(err) ? ESP_ERR_NOT_FOUND : err;
    if (!muse_chat_sid_valid(out)) {
        out[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t muse_cfg_set_chat_sid(const char *sid)
{
    if (!muse_chat_sid_valid(sid)) return ESP_ERR_INVALID_ARG;
    return put_str(K_CHAT_SID, sid);
}

esp_err_t muse_cfg_set_vm_id(const char *vm_id)
{
    if (!vm_id || !*vm_id) return hub_store_erase(HUB_STORE_NS_MUSE, K_VM_ID);
    if (!muse_vm_id_valid(vm_id)) return ESP_ERR_INVALID_ARG;
    return put_str(K_VM_ID, vm_id);
}

esp_err_t muse_cfg_read_perm(uint8_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    uint8_t ver = 0, v[2] = {0};
    size_t len = sizeof(v);
    const esp_err_t err = hub_store_get(HUB_STORE_NS_MUSE, K_PERM, VER, &ver, v, &len, GET_TMO_MS);
    if (err == ESP_ERR_NOT_FOUND) {            /* 没设过：出厂默认 */
        *out = HUB_PERM_DEFAULT;
        return ESP_OK;
    }
    if (err == ESP_OK && len == 1 && (v[0] & ~HUB_PERMBIT_ALL) == 0) {
        *out = v[0];
        return ESP_OK;
    }
    /* 读不到、版本比本固件新（降级过：不知道用户设的是什么）、长度不对、带了不认识的位：往严里取（N4） */
    ESP_LOGW(TAG, "权限位读不准（%s）：按只读算", esp_err_to_name(err));
    *out = 0;
    return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
}

uint8_t muse_cfg_get_perm(void)
{
    uint8_t p = 0;
    (void)muse_cfg_read_perm(&p);
    return p;
}

esp_err_t muse_cfg_read_notify(uint16_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    uint8_t ver = 0, v[3] = {0};
    size_t len = sizeof(v);
    const esp_err_t err = hub_store_get(HUB_STORE_NS_MUSE, K_NOTIFY, VER, &ver, v, &len, GET_TMO_MS);
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_VERSION || err == ESP_ERR_INVALID_SIZE || (err == ESP_OK && len != 2)) {
        *out = HUB_NOTIFY_DEFAULT;                 /* 没存过 / 存的不是这个格式：出厂默认 */
        return ESP_OK;
    }
    if (err != ESP_OK) return err;
    *out = (uint16_t)((v[0] | (v[1] << 8)) & HUB_NOTIFY_ALL);   /* 小端；不认识的位丢掉 */
    return ESP_OK;
}

esp_err_t muse_cfg_set_notify(uint16_t mask)
{
    mask &= HUB_NOTIFY_ALL;
    const uint8_t v[2] = { (uint8_t)(mask & 0xFF), (uint8_t)(mask >> 8) };
    return hub_store_put(HUB_STORE_NS_MUSE, K_NOTIFY, VER, v, sizeof(v));
}

esp_err_t muse_cfg_set_perm(uint8_t perm)
{
    if (perm & ~HUB_PERMBIT_ALL) return ESP_ERR_INVALID_ARG;
    return hub_store_put(HUB_STORE_NS_MUSE, K_PERM, VER, &perm, 1);
}
