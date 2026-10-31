/* muse_cfg.c 对存储错误的处理（M3 第四步审计 ❌）。编的是产品源文件 ../muse_cfg.c，hub_store 换成下面的内存假实现，
 * 可以按键注入错误。钉住的不变量：
 *   - 只有"确实没有可用的令牌"才报 ESP_ERR_NOT_FOUND（会话层据此走刷新 / 撤销 → 清凭据）；
 *     存储超时、没就绪、缺内存原样返回 —— 一时读不到绝不能被当成"凭据没了"；
 *   - 开机读开关 / 配对状态（muse_cfg_load_flags）读不到就报错、不改输出，不能当成"没开 / 没配对"；
 *   - 清配对凭据的读回确认：读回超时不算"已删掉"。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_identity.h"
#include "esp_mac.h"
#include "hub_store.h"
#include "mbedtls/platform_util.h"
#include "muse_cfg.h"

static int s_fail, s_pass;
#define CHECK(cond, ...) do { if (cond) { s_pass++; } else { s_fail++; printf("❌ %s:%d ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------------------------------------------------------- 假 hub_store */

#define SLOTS 16
static struct { char key[16]; uint8_t ver; uint8_t data[4096]; size_t len; int used; } s_kv[SLOTS];
static const char *s_err_key;        /* 只对这个键注入（NULL = 所有键） */
static esp_err_t   s_get_err;        /* get 注入的错误（0 = 不注入） */
static esp_err_t   s_flush_err;
static int         s_erase_drops;    /* > 0：erase 假装成功但不删（模拟没落下去），读回确认要抓住 */

static int find(const char *key)
{
    for (int i = 0; i < SLOTS; i++) if (s_kv[i].used && strcmp(s_kv[i].key, key) == 0) return i;
    return -1;
}

static void reset_store(void)
{
    memset(s_kv, 0, sizeof(s_kv));
    s_err_key = NULL;
    s_get_err = ESP_OK;
    s_flush_err = ESP_OK;
    s_erase_drops = 0;
}

esp_err_t hub_store_put(const char *ns, const char *key, uint8_t ver, const void *data, size_t len)
{
    (void)ns;
    if (len > sizeof(s_kv[0].data)) return ESP_ERR_INVALID_ARG;
    int i = find(key);
    for (int j = 0; i < 0 && j < SLOTS; j++) if (!s_kv[j].used) i = j;
    if (i < 0) return ESP_ERR_NO_MEM;
    snprintf(s_kv[i].key, sizeof(s_kv[i].key), "%s", key);
    s_kv[i].ver = ver;
    memcpy(s_kv[i].data, data, len);
    s_kv[i].len = len;
    s_kv[i].used = 1;
    return ESP_OK;
}

/* 测试里直接摆一个值（可以是任意字节、任意版本） */
static void put_raw(const char *key, uint8_t ver, const void *data, size_t len)
{
    hub_store_put("muse", key, 1, data, len);
    s_kv[find(key)].ver = ver;
}

esp_err_t hub_store_erase(const char *ns, const char *key)
{
    (void)ns;
    if (s_erase_drops > 0) { s_erase_drops--; return ESP_OK; }
    const int i = find(key);
    if (i >= 0) s_kv[i].used = 0;
    return ESP_OK;
}

esp_err_t hub_store_get(const char *ns, const char *key, uint8_t max_ver, uint8_t *ver, void *buf, size_t *len_inout,
                        uint32_t timeout_ms)
{
    (void)ns;
    (void)timeout_ms;
    if (s_get_err != ESP_OK && (!s_err_key || strcmp(s_err_key, key) == 0)) return s_get_err;
    const int i = find(key);
    if (i < 0) return ESP_ERR_NOT_FOUND;
    if (s_kv[i].ver > max_ver) return ESP_ERR_INVALID_VERSION;
    *ver = s_kv[i].ver;
    if (s_kv[i].len > *len_inout) {
        *len_inout = s_kv[i].len;
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(buf, s_kv[i].data, s_kv[i].len);
    *len_inout = s_kv[i].len;
    return ESP_OK;
}

esp_err_t hub_store_flush(uint32_t timeout_ms) { (void)timeout_ms; return s_flush_err; }

/* muse_cfg.c 用到的其余外部符号 */
bool device_identity_valid(void) { return false; }
const char *device_identity_serial(void) { return NULL; }
esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type) { (void)type; memset(mac, 0x11, 6); return ESP_OK; }
void mbedtls_platform_zeroize(void *buf, size_t len) { volatile unsigned char *p = buf; while (len--) *p++ = 0; }

/* ---------------------------------------------------------------- 用例 */

#define ACC "eyJ.access.token-1"
#define REF "eyJ.refresh.token-1"

static void seed_paired(void)
{
    reset_store();
    hub_store_put("muse", "acc_tok", 1, ACC, strlen(ACC));
    hub_store_put("muse", "ref_tok", 1, REF, strlen(REF));
}

static void test_token_read_errors(void)
{
    static char out[MUSE_ACCESS_TOKEN_MAX + 1];      /* 两种令牌都装得下 */
    const esp_err_t transient[] = { ESP_ERR_TIMEOUT, ESP_ERR_INVALID_STATE, ESP_ERR_NO_MEM, ESP_FAIL };
    for (size_t k = 0; k < sizeof(transient) / sizeof(transient[0]); k++) {
        seed_paired();
        s_get_err = transient[k];
        CHECK(muse_cfg_get_refresh_token(out, sizeof(out)) == transient[k],
              "refresh token：存储错误 0x%x 原样返回，不变成 NOT_FOUND（会被当成撤销清凭据）", transient[k]);
        CHECK(muse_cfg_get_access_token(out, sizeof(out)) == transient[k], "access token：同上 0x%x", transient[k]);
    }

    seed_paired();
    CHECK(muse_cfg_get_refresh_token(out, sizeof(out)) == ESP_OK && strcmp(out, REF) == 0, "正常读出");

    reset_store();
    CHECK(muse_cfg_get_refresh_token(out, sizeof(out)) == ESP_ERR_NOT_FOUND, "没存 → NOT_FOUND");

    reset_store();
    put_raw("ref_tok", 2, REF, strlen(REF));
    CHECK(muse_cfg_get_refresh_token(out, sizeof(out)) == ESP_ERR_NOT_FOUND, "版本比本固件新 → 当作没有");

    reset_store();
    put_raw("ref_tok", 1, "has space", 9);
    CHECK(muse_cfg_get_refresh_token(out, sizeof(out)) == ESP_ERR_NOT_FOUND && out[0] == '\0', "内容不合格 → 当作没有、缓冲清零");

    reset_store();
    put_raw("ref_tok", 1, "ab\0cd", 5);
    CHECK(muse_cfg_get_refresh_token(out, sizeof(out)) == ESP_ERR_NOT_FOUND, "中间带 NUL → 当作没有");

    reset_store();
    {
        static char big[MUSE_REFRESH_TOKEN_MAX + 10];
        memset(big, 'a', sizeof(big));
        put_raw("ref_tok", 1, big, sizeof(big));
    }
    CHECK(muse_cfg_get_refresh_token(out, sizeof(out)) == ESP_ERR_NOT_FOUND, "超长 → 当作没有");

    CHECK(muse_cfg_get_refresh_token(out, 8) == ESP_ERR_INVALID_ARG, "缓冲太小 → INVALID_ARG（不是 NOT_FOUND）");
}

static void test_load_flags(void)
{
    bool en = true, paired = true;
    reset_store();
    CHECK(muse_cfg_load_flags(&en, &paired) == ESP_OK && !en && !paired, "什么都没存：确定的 关 / 没配对");

    seed_paired();
    const uint8_t one = 1;
    hub_store_put("muse", "en", 1, &one, 1);
    CHECK(muse_cfg_load_flags(&en, &paired) == ESP_OK && en && paired, "开 + 已配对");

    en = paired = true;
    s_get_err = ESP_ERR_TIMEOUT;
    s_err_key = "en";
    CHECK(muse_cfg_load_flags(&en, &paired) == ESP_ERR_TIMEOUT && en && paired, "开关读超时：报错、输出不动");

    en = paired = true;
    s_err_key = "acc_tok";
    CHECK(muse_cfg_load_flags(&en, &paired) == ESP_ERR_TIMEOUT && en && paired, "配对状态读超时：报错、输出不动");
    CHECK(!muse_cfg_paired(), "显示用的 muse_cfg_paired 读不到算 false（只给网页看，不驱动状态机）");

    s_get_err = ESP_OK;
    s_err_key = NULL;
    const uint8_t two[2] = { 1, 1 };
    hub_store_put("muse", "en", 1, two, 2);
    CHECK(muse_cfg_load_flags(&en, &paired) == ESP_OK && !en, "开关长度不对：确定的关");
    CHECK(muse_cfg_load_flags(NULL, &paired) == ESP_ERR_INVALID_ARG, "NULL");
}

static void test_clear_pairing(void)
{
    seed_paired();
    CHECK(muse_cfg_clear_pairing(100) == ESP_OK && find("acc_tok") < 0 && find("ref_tok") < 0, "正常清除");

    seed_paired();
    s_erase_drops = 1;
    CHECK(muse_cfg_clear_pairing(100) != ESP_OK, "删了没落下去：读回确认抓住");

    seed_paired();
    s_flush_err = ESP_FAIL;
    CHECK(muse_cfg_clear_pairing(100) != ESP_OK, "flush 失败 → 不算删掉");

    /* 删掉之后读回超时：不能当成"确认不在了"（device.unpair 据此才回 accepted） */
    seed_paired();
    s_get_err = ESP_ERR_TIMEOUT;
    CHECK(muse_cfg_clear_pairing(100) != ESP_OK, "读回超时 → 不算确认");
    const char *keys[] = { "acc_tok", "ref_tok", "vm_id" };      /* 三个键各自单独超时也一样 */
    for (size_t k = 0; k < 3; k++) {
        seed_paired();
        s_get_err = ESP_ERR_TIMEOUT;
        s_err_key = keys[k];
        CHECK(muse_cfg_clear_pairing(100) != ESP_OK, "只有 %s 读回超时 → 不算确认", keys[k]);
    }
}

static void test_save_tokens(void)
{
    seed_paired();
    CHECK(muse_cfg_save_tokens("eyJ.new.a", "eyJ.new.r", 100) == ESP_OK, "存新令牌");
    static char out[MUSE_ACCESS_TOKEN_MAX + 1];
    CHECK(muse_cfg_get_access_token(out, sizeof(out)) == ESP_OK && strcmp(out, "eyJ.new.a") == 0, "读回是新的");
    s_get_err = ESP_ERR_TIMEOUT;
    CHECK(muse_cfg_save_tokens("eyJ.new.a2", "eyJ.new.r2", 100) != ESP_OK, "读回超时 → 不报成功");
    s_get_err = ESP_OK;
    CHECK(muse_cfg_save_tokens("bad token", "eyJ.r", 100) == ESP_ERR_INVALID_ARG, "不合格的令牌不存");
}

#include "hub_registry.h"

/* 配置值（第二轮审计 ❌）：一时读不到不许拿默认值顶 */
static void test_config_values(void)
{
    char out[MUSE_URL_MAX + 1];
    reset_store();
    hub_store_put("muse", "api_url2", 1, "https://api2.example", 20);
    strcpy(out, "PREV");
    s_get_err = ESP_ERR_TIMEOUT;
    CHECK(muse_cfg_get_api_url(out, sizeof(out)) == ESP_ERR_TIMEOUT && strcmp(out, "PREV") == 0,
          "api_url 读超时：报错、不写 out（不拿默认值顶）");
    CHECK(muse_cfg_get_noise_host(out, sizeof(out)) == ESP_ERR_TIMEOUT && strcmp(out, "PREV") == 0, "noise_host 同上");
    s_get_err = ESP_OK;
    CHECK(muse_cfg_get_api_url(out, sizeof(out)) == ESP_OK && strcmp(out, "https://api2.example") == 0, "存着的");
    reset_store();
    CHECK(muse_cfg_get_api_url(out, sizeof(out)) == ESP_OK && strcmp(out, "https://api.example") == 0, "确实没有 → Kconfig 默认");
    CHECK(muse_cfg_get_noise_host(out, sizeof(out)) == ESP_OK && strcmp(out, "noise.example") == 0, "noise_host 默认");
    put_raw("api_url2", 1, "http://plain.example", 20);
    CHECK(muse_cfg_get_api_url(out, sizeof(out)) == ESP_OK && strcmp(out, "https://api.example") == 0, "存着的不合格（非 https）→ 默认");
    put_raw("api_url2", 2, "https://newer.example", 21);
    CHECK(muse_cfg_get_api_url(out, sizeof(out)) == ESP_OK && strcmp(out, "https://api.example") == 0, "版本更新 → 默认");

    static char tok[MUSE_SDK_TOKEN_LEN + 1];
    reset_store();
    s_get_err = ESP_ERR_INVALID_STATE;
    CHECK(muse_cfg_get_sdk_token(tok, sizeof(tok)) == ESP_ERR_INVALID_STATE, "SDK token：hub_store 没就绪原样返回");
    s_get_err = ESP_OK;
    CHECK(muse_cfg_get_sdk_token(tok, sizeof(tok)) == ESP_ERR_NOT_FOUND, "SDK token 没存 → NOT_FOUND");

    char vm[MUSE_VM_ID_MAX + 1];
    s_get_err = ESP_ERR_TIMEOUT;
    CHECK(muse_cfg_get_vm_id(vm, sizeof(vm)) == ESP_ERR_TIMEOUT, "vm_id 读超时原样返回（不当成没选、悄悄换到默认 VM）");
    s_get_err = ESP_OK;
}

static void test_perm(void)
{
    reset_store();
    CHECK(muse_cfg_get_perm() == HUB_PERM_DEFAULT, "没设过 → 出厂默认");
    const uint8_t p = HUB_PERMBIT_CONTROL | HUB_PERMBIT_SENSITIVE;
    hub_store_put("muse", "perm", 1, &p, 1);
    CHECK(muse_cfg_get_perm() == p, "设过的");
    s_get_err = ESP_ERR_TIMEOUT;
    CHECK(muse_cfg_get_perm() == 0, "读超时 → 只读（不放开成默认，N4）");
    s_get_err = ESP_OK;
    const uint8_t bad = 0x80;
    hub_store_put("muse", "perm", 1, &bad, 1);
    CHECK(muse_cfg_get_perm() == 0, "带不认识的位 → 只读");
    const uint8_t two[2] = { 1, 1 };
    hub_store_put("muse", "perm", 1, two, 2);
    CHECK(muse_cfg_get_perm() == 0, "长度不对 → 只读");
    put_raw("perm", 2, &p, 1);
    CHECK(muse_cfg_get_perm() == 0, "版本比本固件新（降级过）→ 只读，不回落成默认");
    uint8_t out = 0xFF;
    s_get_err = ESP_ERR_TIMEOUT;
    CHECK(muse_cfg_read_perm(&out) == ESP_ERR_TIMEOUT && out == 0, "read_perm 读超时：报错、给 0");
    s_get_err = ESP_OK;
    reset_store();
    CHECK(muse_cfg_read_perm(&out) == ESP_OK && out == HUB_PERM_DEFAULT, "read_perm 没设过：ESP_OK + 默认");
}

static void test_chat_sid_store(void)
{
    char sid[MUSE_CHAT_SID_LEN + 1];
    reset_store();
    CHECK(muse_cfg_get_chat_sid(sid, sizeof(sid)) == ESP_ERR_NOT_FOUND, "没存 → NOT_FOUND（调用方生成）");
    CHECK(muse_cfg_set_chat_sid("6b1f0a2c-0d3e-4f50-8a6b-7c8d9e0f1a2b") == ESP_OK, "存");
    CHECK(muse_cfg_get_chat_sid(sid, sizeof(sid)) == ESP_OK && strcmp(sid, "6b1f0a2c-0d3e-4f50-8a6b-7c8d9e0f1a2b") == 0, "读回");
    s_get_err = ESP_ERR_TIMEOUT;
    CHECK(muse_cfg_get_chat_sid(sid, sizeof(sid)) == ESP_ERR_TIMEOUT, "读超时原样返回（不当成没有：否则会另开一个侧边会话）");
    s_get_err = ESP_OK;
    CHECK(muse_cfg_set_chat_sid("not-a-uuid") == ESP_ERR_INVALID_ARG, "不合格不存");
    put_raw("chat_sid", 1, "garbage", 7);
    CHECK(muse_cfg_get_chat_sid(sid, sizeof(sid)) == ESP_ERR_NOT_FOUND, "存着的不合格 → 当作没有");
}

static void test_notify(void)
{
    uint16_t m = 0xFFFF;
    reset_store();
    CHECK(muse_cfg_read_notify(&m) == ESP_OK && m == HUB_NOTIFY_DEFAULT, "没存过 → 默认（全开：SECURITY 由每台设备的开关把关）");
    CHECK(muse_cfg_set_notify((uint16_t)(1u << HUB_EVC_GATEWAY)) == ESP_OK, "存");
    CHECK(muse_cfg_read_notify(&m) == ESP_OK && m == (1u << HUB_EVC_GATEWAY), "读回");
    m = 0x1234;
    s_get_err = ESP_ERR_TIMEOUT;
    CHECK(muse_cfg_read_notify(&m) == ESP_ERR_TIMEOUT && m == 0x1234, "读超时：报错、不写 out（不回落成默认，关掉的类别不会又推起来）");
    s_get_err = ESP_OK;
    CHECK(muse_cfg_set_notify(0xFFFF) == ESP_OK && muse_cfg_read_notify(&m) == ESP_OK && m == HUB_NOTIFY_ALL, "不认识的位丢掉");
    put_raw("notify", 1, "abc", 3);
    CHECK(muse_cfg_read_notify(&m) == ESP_OK && m == HUB_NOTIFY_DEFAULT, "长度不对 → 默认");
}

int main(void)
{
    test_token_read_errors();
    test_load_flags();
    test_clear_pairing();
    test_save_tokens();
    test_config_values();
    test_perm();
    test_chat_sid_store();
    test_notify();
    printf("%s muse_cfg（存储错误）：%d 通过，%d 失败\n", s_fail ? "❌" : "✅", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
