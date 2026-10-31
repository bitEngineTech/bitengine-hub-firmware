/* 主机侧测试：配置导入 / 导出的 UTF-8 规则（T9.33）—— 编的是产品源文件本身（../config_export.c）。
 *
 * 规则（2026-10-07 定）：写进配置的字符串必须是合法 UTF-8，WiFi SSID 除外。
 *   · 导出：非 UTF-8 的 SSID 写成 "ssid"（可读，坏字节换 U+FFFD）+ "ssid_hex"（原始字节），
 *     整份备份始终是合法 UTF-8；合法 SSID 不带 ssid_hex，格式与以前一样。
 *   · 导入：除 wifi.ssid 外任何字符串值不合法 → 整份拒绝、配置一个字节都不动；
 *     ssid_hex 优先于 ssid，格式不对整份拒绝；超长值截在字符边界，不切半个中文。
 *
 * 为什么必须有：备份 / 恢复是用户唯一的后悔药，而 ssid_hex 那条路只在 GBK 名的 AP 下才走得到，
 * 上板测要真去改设备的 WiFi 配置 —— 主机上测掉，上板只验"合法输入没被误伤"。 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "config_mgr.h"
#include "cJSON.h"
#include "utf8.h"

/* ---- config_export.c 依赖的 config_mgr.c 那一半 ---- */
gateway_config_t  g_config;
SemaphoreHandle_t s_mutex;
ESP_EVENT_DEFINE_BASE(CONFIG_EVENT);
const gateway_config_t *config_mgr_get_all(void) { return &g_config; }
esp_err_t config_mgr_save_now(void) { return ESP_OK; }
esp_err_t esp_event_post(esp_event_base_t b, int32_t id, const void *d, size_t n, uint32_t t)
{ (void)b; (void)id; (void)d; (void)n; (void)t; return ESP_OK; }
esp_err_t config_mgr_set_extra_host(const char *h)
{ strlcpy(g_config.sys.extra_host, h, sizeof(g_config.sys.extra_host)); return ESP_OK; }

/* ---- 断言 ---- */
static int pass = 0, fail = 0;
static void T(const char *what, int ok, const char *detail)
{
    if (ok) { printf("✅ %s\n", what); pass++; }
    else    { printf("❌ %s%s%s\n", what, detail ? " —— " : "", detail ? detail : ""); fail++; }
}

static void base_config(void)
{
    memset(&g_config, 0, sizeof(g_config));
    g_config.version = CONFIG_VERSION_CURRENT;
    strcpy(g_config.sys.hostname, "bitengine");
    strcpy(g_config.wifi.ssid, "MyWiFi");
    strcpy(g_config.wifi.password, "wifi-secret");
    g_config.wifi.has_credentials = true;
    g_config.thread.channel = 25;
    strcpy(g_config.mqtt.broker, "mqtt.local");
    strcpy(g_config.mqtt.username, "ha");
    strcpy(g_config.mqtt.topic_prefix, "bitengine");
    strcpy(g_config.ntp.server, "pool.ntp.org");
    strcpy(g_config.ntp.timezone, "CST-8");
}

/* 导出并解析；顺带验"整份是合法 UTF-8" */
static cJSON *export_parsed(int *utf8_ok)
{
    char *s = config_mgr_export_json();
    if (!s) { *utf8_ok = 0; return NULL; }
    *utf8_ok = utf8_valid(s);
    cJSON *j = cJSON_Parse(s);
    free(s);
    return j;
}

static const char *wifi_str(const cJSON *root, const char *key)
{
    const cJSON *w = cJSON_GetObjectItem(root, "wifi");
    const cJSON *v = w ? cJSON_GetObjectItem(w, key) : NULL;
    return (v && cJSON_IsString(v)) ? v->valuestring : NULL;
}

/* 导入一段 JSON，期望被整份拒绝且配置一个字节都没动 */
static void expect_rejected(const char *what, const char *json)
{
    base_config();
    gateway_config_t before = g_config;
    esp_err_t e = config_mgr_import_json(json);
    int ok = (e == ESP_ERR_INVALID_ARG) && !memcmp(&before, &g_config, sizeof(g_config));
    T(what, ok, e != ESP_ERR_INVALID_ARG ? "没有被拒绝" : "被拒绝了，但配置已经被改动");
}

#define GBK_ZHONGWEN "\xD6\xD0\xCE\xC4"     /* GBK 编码的「中文」—— 不是 UTF-8 */
#define FFFD         "\xEF\xBF\xBD"

int main(void)
{
    int u8;
    cJSON *j;

    printf("── 导出 ──\n");
    base_config();
    j = export_parsed(&u8);
    T("合法 SSID：导出是合法 UTF-8", j && u8, NULL);
    T("合法 SSID：ssid 原样、不带 ssid_hex",
      j && wifi_str(j, "ssid") && !strcmp(wifi_str(j, "ssid"), "MyWiFi") && !wifi_str(j, "ssid_hex"), NULL);
    cJSON_Delete(j);

    base_config();
    strcpy(g_config.wifi.ssid, GBK_ZHONGWEN);
    j = export_parsed(&u8);
    T("GBK SSID：导出仍是合法 UTF-8", j && u8, NULL);
    T("GBK SSID：ssid 是可读的修正版（4 个 U+FFFD）",
      j && wifi_str(j, "ssid") && !strcmp(wifi_str(j, "ssid"), FFFD FFFD FFFD FFFD), wifi_str(j, "ssid"));
    T("GBK SSID：ssid_hex 是原始字节",
      j && wifi_str(j, "ssid_hex") && !strcmp(wifi_str(j, "ssid_hex"), "d6d0cec4"), wifi_str(j, "ssid_hex"));
    cJSON_Delete(j);

    base_config();
    memset(g_config.wifi.ssid, 0xFF, SSID_MAX_LEN - 1);           /* 32 个坏字节：最长的情形 */
    g_config.wifi.ssid[SSID_MAX_LEN - 1] = '\0';
    j = export_parsed(&u8);
    T("32 字节全非法的 SSID：导出合法、可读版不截断、hex 64 位",
      j && u8 && wifi_str(j, "ssid") && strlen(wifi_str(j, "ssid")) == 32 * 3
        && wifi_str(j, "ssid_hex") && strlen(wifi_str(j, "ssid_hex")) == 64, NULL);
    cJSON_Delete(j);

    printf("── 往返：导出什么，导入就还原成什么 ──\n");
    base_config();
    strcpy(g_config.wifi.ssid, GBK_ZHONGWEN);
    char *exp = config_mgr_export_json();
    base_config();                                                  /* ssid 回到 "MyWiFi" */
    esp_err_t e = config_mgr_import_json(exp);
    T("GBK SSID 的备份导回来，SSID 逐字节还原", e == ESP_OK && !strcmp(g_config.wifi.ssid, GBK_ZHONGWEN), NULL);
    free(exp);

    base_config();
    strcpy(g_config.mqtt.username, "网关用户");
    exp = config_mgr_export_json();
    gateway_config_t want = g_config;
    base_config();
    e = config_mgr_import_json(exp);
    T("合法中文值往返不变", e == ESP_OK && !strcmp(g_config.mqtt.username, want.mqtt.username), NULL);
    T("机密字段导出打码、导入时保留原值（老行为不变）", !strcmp(g_config.wifi.password, "wifi-secret"), NULL);
    free(exp);

    printf("── 导入：SSID 例外 ──\n");
    base_config();
    e = config_mgr_import_json("{\"version\":1,\"wifi\":{\"ssid\":\"" GBK_ZHONGWEN "\"}}");
    T("老固件导出的原始 GBK ssid 照收（SSID 例外）", e == ESP_OK && !strcmp(g_config.wifi.ssid, GBK_ZHONGWEN), NULL);

    base_config();
    e = config_mgr_import_json("{\"version\":1,\"wifi\":{\"ssid\":\"whatever\",\"ssid_hex\":\"d6d0cec4\"}}");
    T("ssid_hex 优先于 ssid", e == ESP_OK && !strcmp(g_config.wifi.ssid, GBK_ZHONGWEN), NULL);

    base_config();
    char hex64[80] = "{\"version\":1,\"wifi\":{\"ssid_hex\":\"";
    for (int i = 0; i < 32; i++) strcat(hex64, "41");
    strcat(hex64, "\"}}");
    e = config_mgr_import_json(hex64);
    T("ssid_hex 32 字节（上限）照收", e == ESP_OK && strlen(g_config.wifi.ssid) == 32, NULL);

    printf("── 导入：非法 UTF-8 整份拒绝，配置不动 ──\n");
    expect_rejected("mqtt.username 含 \\xff",      "{\"version\":1,\"mqtt\":{\"username\":\"ha\xff\"}}");
    expect_rejected("ntp.timezone 含 GBK",         "{\"version\":1,\"ntp\":{\"timezone\":\"" GBK_ZHONGWEN "\"}}");
    expect_rejected("机密字段 wifi.password 也查", "{\"version\":1,\"wifi\":{\"password\":\"pw\xc0\x80pwpw\"}}");
    expect_rejected("坏值在前、好值在后：一样整份拒",
                    "{\"version\":1,\"mqtt\":{\"broker\":\"x\xff\"},\"ntp\":{\"server\":\"ok.example\"}}");
    expect_rejected("不认识的键里的坏值也拒（文件本身就不是合法 JSON）",
                    "{\"version\":1,\"future\":{\"note\":\"\xed\xa0\x80\"}}");
    expect_rejected("嵌套过深",
                    "{\"version\":1,\"a\":{\"b\":{\"c\":{\"d\":{\"e\":{\"f\":{\"g\":{\"h\":{\"i\":1}}}}}}}}}");
    expect_rejected("ssid_hex 空串",               "{\"version\":1,\"wifi\":{\"ssid_hex\":\"\"}}");
    expect_rejected("ssid_hex 奇数位",             "{\"version\":1,\"wifi\":{\"ssid_hex\":\"abc\"}}");
    expect_rejected("ssid_hex 非十六进制",         "{\"version\":1,\"wifi\":{\"ssid_hex\":\"zz\"}}");
    expect_rejected("ssid_hex 含 00（会截断 SSID）", "{\"version\":1,\"wifi\":{\"ssid_hex\":\"4100\"}}");
    expect_rejected("ssid_hex 不是字符串",         "{\"version\":1,\"wifi\":{\"ssid_hex\":123}}");
    char hex66[96] = "{\"version\":1,\"wifi\":{\"ssid_hex\":\"";
    for (int i = 0; i < 33; i++) strcat(hex66, "41");
    strcat(hex66, "\"}}");
    expect_rejected("ssid_hex 33 字节（超上限）",  hex66);

    printf("── 导入：超长值截在字符边界 ──\n");
    base_config();
    /* 前面垫一个 ASCII：上限 63 字节，纯汉字时 63 = 21×3 恰好落在边界上，按字节砍也测不出来 */
    char longname[256] = "{\"version\":1,\"mqtt\":{\"username\":\"a";
    for (int i = 0; i < 30; i++) strcat(longname, "网");      /* 1 + 90 字节 > 63 */
    strcat(longname, "\"}}");
    e = config_mgr_import_json(longname);
    size_t n = strlen(g_config.mqtt.username);
    T("超长中文用户名截在字符边界（a + 20 个字 = 61 字节，不切半个字）",
      e == ESP_OK && n == 61 && utf8_valid(g_config.mqtt.username), NULL);

    printf("\nResult: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
