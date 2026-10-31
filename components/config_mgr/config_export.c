#include "config_mgr.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_event.h"
#include "utf8.h"
#include <string.h>

static const char *TAG = "config_export";

/* ---- T9.33：非 UTF-8 的 SSID 在备份里另存成十六进制 ----
 * 配置里的字符串都必须是合法 UTF-8，唯独 WiFi SSID 例外（AP 定的任意字节，GBK 编码的
 * 中文名就不是 UTF-8；2026-10-07 定）。导出时它若不合法，"ssid" 放修过的可读版本（坏字节
 * 换成 U+FFFD，给人看），"ssid_hex" 放原始字节 —— 备份文件始终是合法 JSON，导入时又能
 * 逐字节还原。合法的 SSID 不带 ssid_hex，备份格式与以前完全一样。 */
static void hex_encode(char *dst, size_t dst_size, const char *src)
{
    static const char hex[] = "0123456789abcdef";
    size_t w = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && w + 2 < dst_size; p++) {
        dst[w++] = hex[*p >> 4];
        dst[w++] = hex[*p & 0x0F];
    }
    dst[w] = '\0';
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ssid_hex → 原始字节。必须是 1..(cap-1) 字节、偶数位十六进制、不含 00（会把 SSID 截断）。 */
static bool ssid_hex_decode(const char *hex, char *dst, size_t cap)
{
    size_t n = strlen(hex);
    if (n == 0 || (n % 2) != 0 || n / 2 >= cap) return false;
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hex_nibble(hex[2 * i]), lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) return false;
        dst[i] = (char)((hi << 4) | lo);
    }
    dst[n / 2] = '\0';
    return true;
}

/* 整份导入里的字符串值都必须是合法 UTF-8，exempt（wifi.ssid）除外。返回第一个坏节点。
   配置文档只有两三层；超过 8 层的一律当坏文件，免得一份恶意的深层嵌套把栈递归穿。 */
static const cJSON *first_invalid_string(const cJSON *node, const cJSON *exempt, int depth)
{
    if (depth > 8) return node;
    if (node == exempt) return NULL;
    if (cJSON_IsString(node) && !utf8_valid(node->valuestring)) return node;
    for (const cJSON *ch = node->child; ch; ch = ch->next) {
        const cJSON *bad = first_invalid_string(ch, exempt, depth + 1);
        if (bad) return bad;
    }
    return NULL;
}

// ---- Export ----

char *config_mgr_export_json(void)
{
    const gateway_config_t *c = config_mgr_get_all();
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "version", c->version);

    // Helper: abort on allocation failure
#define ADD_OBJ(parent, name) ({ \
    cJSON *_o = cJSON_AddObjectToObject(parent, name); \
    if (!_o) { cJSON_Delete(root); return NULL; } \
    _o; })

/* ⚠️ 导出**不含任何机密**。2026-08-22 加。
 *
 * 之前 wifi.password / mqtt.password 是**明文**、password_hash 是无盐 SHA256、
 * webhook.url 常带 token（Slack/Discord 那类）。而 auth.c:176 是
 * 「没设密码 → 全部放行」—— 也就是说一台还没被浏览器打开过的出厂网关上，
 * 局域网任何人一条 curl 就能把用户家的 WiFi 密码拿走。那泄露的正是我们
 * 声称的那条信任边界本身，比 Thread NetworkKey 值钱得多。
 *
 * 现在这四个字段导出为占位符。空值仍导出为空串 —— 「本来就没设」和
 * 「设了但被隐去」必须分得开，否则导入端无法判断该不该清空。
 *
 * ⚠️ 代价（必须在 WebUI 和文档里说明）：备份不再能一键还原这四项，
 * 导入后要重填 WiFi 密码、MQTT 密码，并重新设置设备密码。
 * 这是有意的取舍 —— 备份文件会被邮件传、被丢在网盘里。 */
#define CFG_REDACTED  "********"

/* 导出机密字段：非空 → 占位符；空 → 空串（保留"本来就没设"这个信息） */
#define ADD_SECRET(obj, key, val) \
    cJSON_AddStringToObject(obj, key, (val)[0] ? CFG_REDACTED : "")

    // sys
    cJSON *sys = ADD_OBJ(root, "system");
    cJSON_AddStringToObject(sys, "hostname", c->sys.hostname);
    /* 不做 ADD_SECRET —— extra_host 不是秘密，恰恰相反：它是一张跨站守卫的
       永久通行证，导出时**必须看得见**，否则一份被做过手脚的备份可以把
       攻击者的域名悄悄带进设备。 */
    cJSON_AddStringToObject(sys, "extra_host", c->sys.extra_host);
    cJSON_AddNumberToObject(sys, "bridge_mode", c->sys.bridge_mode);
    cJSON_AddStringToObject(sys, "radio_region", c->sys.radio_region);
    ADD_SECRET(sys, "password_hash", c->sys.password_hash);

    // wifi
    cJSON *wifi = ADD_OBJ(root, "wifi");
    if (utf8_valid(c->wifi.ssid)) {
        cJSON_AddStringToObject(wifi, "ssid", c->wifi.ssid);
    } else {
        /* 见文件头 T9.33：可读版本 + 原始字节，导入时 ssid_hex 优先 */
        char shown[sizeof(c->wifi.ssid) * 3];          /* 每个坏字节最多变成 3 字节 */
        char hex[sizeof(c->wifi.ssid) * 2 + 1];
        utf8_repair_copy(shown, sizeof(shown), c->wifi.ssid, NULL);
        hex_encode(hex, sizeof(hex), c->wifi.ssid);
        cJSON_AddStringToObject(wifi, "ssid", shown);
        cJSON_AddStringToObject(wifi, "ssid_hex", hex);
    }
    ADD_SECRET(wifi, "password", c->wifi.password);
    cJSON_AddNumberToObject(wifi, "static_ip", c->wifi.static_ip);
    cJSON_AddNumberToObject(wifi, "static_gateway", c->wifi.static_gateway);
    cJSON_AddNumberToObject(wifi, "static_netmask", c->wifi.static_netmask);
    cJSON_AddNumberToObject(wifi, "static_dns", c->wifi.static_dns);

    // ethernet
    cJSON *eth = ADD_OBJ(root, "ethernet");
    cJSON_AddBoolToObject(eth, "dhcp", c->eth.dhcp);
    cJSON_AddNumberToObject(eth, "static_ip", c->eth.static_ip);
    cJSON_AddNumberToObject(eth, "static_gateway", c->eth.static_gateway);
    cJSON_AddNumberToObject(eth, "static_netmask", c->eth.static_netmask);
    cJSON_AddNumberToObject(eth, "static_dns", c->eth.static_dns);

    // zigbee
    cJSON *zb = ADD_OBJ(root, "zigbee");
    /* channel 是用户声明值，必须连同 channel_declared 一起导出 —— 只导 channel
       的话，恢复备份时会把"未声明"复活成"声明了某个值"，等于凭空造一个假声明。
       pan_id 是实测到的自家协调器 PAN，恢复它才能继续做过期检测。 */
    cJSON_AddNumberToObject(zb, "channel", c->zigbee.channel);
    cJSON_AddBoolToObject(zb, "channel_declared", c->zigbee.channel_declared);
    cJSON_AddNumberToObject(zb, "pan_id", c->zigbee.pan_id);
    cJSON_AddBoolToObject(zb, "pan_known", c->zigbee.pan_known);

    // thread
    cJSON *thr = ADD_OBJ(root, "thread");
    cJSON_AddBoolToObject(thr, "enabled", c->thread.enabled);
    cJSON_AddNumberToObject(thr, "channel", c->thread.channel);
    cJSON_AddBoolToObject(thr, "rest_auth", c->thread.rest_auth);
    /* token 是**凭据**，脱敏 —— 与 extra_host 相反（那个是权限授予，必须看得见）。
       导出的备份会被贴进工单、聊天记录、云盘，明文凭据不能进去。 */
    ADD_SECRET(thr, "otbr_token", c->thread.otbr_token);

    // mqtt
    cJSON *mqtt = ADD_OBJ(root, "mqtt");
    cJSON_AddBoolToObject(mqtt, "enabled", c->mqtt.enabled);
    cJSON_AddStringToObject(mqtt, "broker", c->mqtt.broker);
    cJSON_AddNumberToObject(mqtt, "port", c->mqtt.port);
    cJSON_AddStringToObject(mqtt, "username", c->mqtt.username);
    ADD_SECRET(mqtt, "password", c->mqtt.password);
    cJSON_AddStringToObject(mqtt, "topic_prefix", c->mqtt.topic_prefix);
    cJSON_AddNumberToObject(mqtt, "publish_interval", c->mqtt.publish_interval);
    cJSON_AddBoolToObject(mqtt, "tls_enabled", c->mqtt.tls_enabled);

    // webhook
    cJSON *wh = ADD_OBJ(root, "webhook");
    cJSON_AddBoolToObject(wh, "enabled", c->webhook.enabled);
    ADD_SECRET(wh, "url", c->webhook.url);
    cJSON_AddNumberToObject(wh, "backup_interval", c->webhook.backup_interval);
    cJSON_AddBoolToObject(wh, "event_zigbee_crash", c->webhook.event_zigbee_crash);
    cJSON_AddBoolToObject(wh, "event_network_down", c->webhook.event_network_down);
    cJSON_AddBoolToObject(wh, "event_interference", c->webhook.event_interference);
    cJSON_AddBoolToObject(wh, "event_ota", c->webhook.event_ota);
    cJSON_AddBoolToObject(wh, "event_temp_high", c->webhook.event_temp_high);
    cJSON_AddBoolToObject(wh, "event_memory_low", c->webhook.event_memory_low);

    // led
    cJSON *led = ADD_OBJ(root, "led");
    cJSON_AddNumberToObject(led, "brightness", c->led.brightness);
    cJSON_AddBoolToObject(led, "night_mode_enabled", c->led.night_mode_enabled);
    cJSON_AddNumberToObject(led, "night_start_hour", c->led.night_start_hour);
    cJSON_AddNumberToObject(led, "night_start_min", c->led.night_start_min);
    cJSON_AddNumberToObject(led, "night_end_hour", c->led.night_end_hour);
    cJSON_AddNumberToObject(led, "night_end_min", c->led.night_end_min);
    cJSON_AddNumberToObject(led, "color_booting", c->led.color_booting);
    cJSON_AddNumberToObject(led, "color_normal", c->led.color_normal);
    cJSON_AddNumberToObject(led, "color_zigbee", c->led.color_zigbee);
    cJSON_AddNumberToObject(led, "color_error", c->led.color_error);
    cJSON_AddNumberToObject(led, "color_ota", c->led.color_ota);

    // whitelist
    cJSON *wl = ADD_OBJ(root, "tcp_whitelist");
    cJSON_AddBoolToObject(wl, "enabled", c->whitelist.enabled);
    cJSON *ips = cJSON_AddArrayToObject(wl, "allowed_ips");
    for (int i = 0; i < c->whitelist.count && i < MAX_WHITELIST_IPS; i++) {
        cJSON_AddItemToArray(ips, cJSON_CreateNumber(c->whitelist.allowed_ips[i]));
    }

    // ntp
    cJSON *ntp = ADD_OBJ(root, "ntp");
    cJSON_AddStringToObject(ntp, "server", c->ntp.server);
    cJSON_AddStringToObject(ntp, "timezone", c->ntp.timezone);
    cJSON_AddNumberToObject(ntp, "sync_interval", c->ntp.sync_interval);
    cJSON_AddBoolToObject(ntp, "tz_declared", c->ntp.tz_declared);

#undef ADD_OBJ

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Config exported (%d bytes)", json_str ? (int)strlen(json_str) : 0);
    return json_str;
}

// ---- Import (partial: only updates fields present in JSON) ----

// Helper macros for partial import
/* 超长值照旧截断（导出的值本来都放得下，超长只可能是手改的文件），但必须截在**字符边界**：
   strlcpy 按字节砍，会把一个中文字符切成两半，存进 NVS 的就又是非法 UTF-8（T9.33）。
   值在进来之前已经整体验过 UTF-8，所以 utf8_repair_copy 在这里只负责截断，不会改字。 */
#define IMPORT_STR(obj, key, dst, maxlen) do { \
    cJSON *_v = cJSON_GetObjectItem(obj, key); \
    if (_v && cJSON_IsString(_v)) utf8_repair_copy(dst, maxlen, _v->valuestring, NULL); \
} while (0)

/* 原样字节拷贝 —— 只给 WiFi SSID 用（它不要求是 UTF-8，见文件头 T9.33）。 */
#define IMPORT_RAW_STR(obj, key, dst, maxlen) do { \
    cJSON *_v = cJSON_GetObjectItem(obj, key); \
    if (_v && cJSON_IsString(_v)) strlcpy(dst, _v->valuestring, maxlen); \
} while (0)

/* 机密字段专用：遇到占位符**跳过**，保留设备上现有的值。
   ⚠️ 少了这一条，用户还原备份时会把 WiFi 密码真的写成 "********"，
   设备当场连不上网、只能按键恢复出厂。这是本次改动最危险的失效路径。 */
#define IMPORT_SECRET_STR(obj, key, dst, maxlen) do { \
    cJSON *_v = cJSON_GetObjectItem(obj, key); \
    if (_v && cJSON_IsString(_v) && strcmp(_v->valuestring, CFG_REDACTED) != 0) \
        utf8_repair_copy(dst, maxlen, _v->valuestring, NULL); \
} while (0)

#define IMPORT_U8(obj, key, dst) do { \
    cJSON *_v = cJSON_GetObjectItem(obj, key); \
    if (_v && cJSON_IsNumber(_v)) dst = (uint8_t)_v->valueint; \
} while (0)

#define IMPORT_I8(obj, key, dst) do { \
    cJSON *_v = cJSON_GetObjectItem(obj, key); \
    if (_v && cJSON_IsNumber(_v)) dst = (int8_t)_v->valueint; \
} while (0)

#define IMPORT_U16(obj, key, dst) do { \
    cJSON *_v = cJSON_GetObjectItem(obj, key); \
    if (_v && cJSON_IsNumber(_v)) dst = (uint16_t)_v->valueint; \
} while (0)

#define IMPORT_U32(obj, key, dst) do { \
    cJSON *_v = cJSON_GetObjectItem(obj, key); \
    if (_v && cJSON_IsNumber(_v)) dst = (uint32_t)_v->valuedouble; \
} while (0)

#define IMPORT_BOOL(obj, key, dst) do { \
    cJSON *_v = cJSON_GetObjectItem(obj, key); \
    if (_v && cJSON_IsBool(_v)) dst = cJSON_IsTrue(_v); \
} while (0)

esp_err_t config_mgr_import_json(const char *json)
{
    if (!json) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "JSON parse failed");
        return ESP_ERR_INVALID_ARG;
    }

    // Version check
    cJSON *ver = cJSON_GetObjectItem(root, "version");
    if (!ver || !cJSON_IsNumber(ver)) {
        ESP_LOGW(TAG, "JSON missing version field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (ver->valueint > CONFIG_VERSION_CURRENT) {
        ESP_LOGW(TAG, "JSON version %d > current %d, incompatible",
                 ver->valueint, CONFIG_VERSION_CURRENT);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    // Note: we import by accessing g_config directly with extern — declared in config_mgr.c
    extern gateway_config_t g_config;
    extern SemaphoreHandle_t s_mutex;

    /* T9.33：先整份验，再动配置 —— 中途发现坏值时已经写了一半，回不去。
       整份拒绝而不是像 extra_host 那样"清掉那一项继续"：那一类是语法合法、语义不许的值；
       含非法 UTF-8 的文件本身就不是合法 JSON（RFC 8259），我们自己导出的备份不会这样，
       只可能是被改过或损坏了。WiFi SSID 例外，且 ssid_hex 优先于 ssid。 */
    const cJSON *wifi_j  = cJSON_GetObjectItem(root, "wifi");
    const cJSON *ssid_j  = wifi_j ? cJSON_GetObjectItem(wifi_j, "ssid") : NULL;
    const cJSON *bad     = first_invalid_string(root, ssid_j, 0);
    if (bad) {
        ESP_LOGW(TAG, "导入拒绝：字段 %s 不是合法 UTF-8（或嵌套过深）", bad->string ? bad->string : "?");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    char ssid_raw[sizeof(g_config.wifi.ssid)];
    bool have_ssid_hex = false;
    const cJSON *ssid_hex_j = wifi_j ? cJSON_GetObjectItem(wifi_j, "ssid_hex") : NULL;
    if (ssid_hex_j) {
        if (!cJSON_IsString(ssid_hex_j) ||
            !ssid_hex_decode(ssid_hex_j->valuestring, ssid_raw, sizeof(ssid_raw))) {
            ESP_LOGW(TAG, "导入拒绝：wifi.ssid_hex 格式非法");
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        have_ssid_hex = true;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    // sys
    cJSON *sys = cJSON_GetObjectItem(root, "system");
    if (sys) {
        IMPORT_STR(sys, "hostname", g_config.sys.hostname, sizeof(g_config.sys.hostname));
        IMPORT_STR(sys, "extra_host", g_config.sys.extra_host, sizeof(g_config.sys.extra_host));
        IMPORT_U8(sys, "bridge_mode", g_config.sys.bridge_mode);
        IMPORT_STR(sys, "radio_region", g_config.sys.radio_region, sizeof(g_config.sys.radio_region));
        IMPORT_SECRET_STR(sys, "password_hash", g_config.sys.password_hash, sizeof(g_config.sys.password_hash));
        /* 2026-09-11(T4.3)：firmware_channel 已删，旧备份里的该键按未知键忽略 */
    }

    // wifi
    cJSON *wifi = cJSON_GetObjectItem(root, "wifi");
    if (wifi) {
        IMPORT_RAW_STR(wifi, "ssid", g_config.wifi.ssid, sizeof(g_config.wifi.ssid));
        if (have_ssid_hex) strlcpy(g_config.wifi.ssid, ssid_raw, sizeof(g_config.wifi.ssid));
        IMPORT_SECRET_STR(wifi, "password", g_config.wifi.password, sizeof(g_config.wifi.password));
        IMPORT_U32(wifi, "static_ip", g_config.wifi.static_ip);
        IMPORT_U32(wifi, "static_gateway", g_config.wifi.static_gateway);
        IMPORT_U32(wifi, "static_netmask", g_config.wifi.static_netmask);
        IMPORT_U32(wifi, "static_dns", g_config.wifi.static_dns);
        g_config.wifi.has_credentials = (strlen(g_config.wifi.ssid) > 0);
    }

    // ethernet
    cJSON *eth = cJSON_GetObjectItem(root, "ethernet");
    if (eth) {
        IMPORT_BOOL(eth, "dhcp", g_config.eth.dhcp);
        IMPORT_U32(eth, "static_ip", g_config.eth.static_ip);
        IMPORT_U32(eth, "static_gateway", g_config.eth.static_gateway);
        IMPORT_U32(eth, "static_netmask", g_config.eth.static_netmask);
        IMPORT_U32(eth, "static_dns", g_config.eth.static_dns);
    }

    // zigbee
    cJSON *zb = cJSON_GetObjectItem(root, "zigbee");
    if (zb) {
        IMPORT_U8(zb, "channel", g_config.zigbee.channel);
        IMPORT_BOOL(zb, "channel_declared", g_config.zigbee.channel_declared);
        IMPORT_U16(zb, "pan_id", g_config.zigbee.pan_id);
        IMPORT_BOOL(zb, "pan_known", g_config.zigbee.pan_known);
        /* 2026-09-10 前的备份里还有 "tx_power"/"turbo_mode"：按未知键忽略 */
    }

    // thread
    cJSON *thr = cJSON_GetObjectItem(root, "thread");
    if (thr) {
        IMPORT_BOOL(thr, "enabled", g_config.thread.enabled);
        IMPORT_U8(thr, "channel", g_config.thread.channel);
        IMPORT_BOOL(thr, "rest_auth", g_config.thread.rest_auth);
        /* 占位符 ******** 会被跳过 —— 从自己的备份恢复时保留设备上现有的 token，
           不会被打成星号。同 password_hash 的处理。 */
        IMPORT_SECRET_STR(thr, "otbr_token", g_config.thread.otbr_token,
                          sizeof(g_config.thread.otbr_token));
    }

    // mqtt
    cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt");
    if (mqtt) {
        IMPORT_BOOL(mqtt, "enabled", g_config.mqtt.enabled);
        IMPORT_STR(mqtt, "broker", g_config.mqtt.broker, sizeof(g_config.mqtt.broker));
        IMPORT_U16(mqtt, "port", g_config.mqtt.port);
        IMPORT_STR(mqtt, "username", g_config.mqtt.username, sizeof(g_config.mqtt.username));
        IMPORT_SECRET_STR(mqtt, "password", g_config.mqtt.password, sizeof(g_config.mqtt.password));
        IMPORT_STR(mqtt, "topic_prefix", g_config.mqtt.topic_prefix, sizeof(g_config.mqtt.topic_prefix));
        IMPORT_U16(mqtt, "publish_interval", g_config.mqtt.publish_interval);
        IMPORT_BOOL(mqtt, "tls_enabled", g_config.mqtt.tls_enabled);
    }

    // webhook
    cJSON *wh = cJSON_GetObjectItem(root, "webhook");
    if (wh) {
        IMPORT_BOOL(wh, "enabled", g_config.webhook.enabled);
        IMPORT_SECRET_STR(wh, "url", g_config.webhook.url, sizeof(g_config.webhook.url));
        IMPORT_U32(wh, "backup_interval", g_config.webhook.backup_interval);
        IMPORT_BOOL(wh, "event_zigbee_crash", g_config.webhook.event_zigbee_crash);
        IMPORT_BOOL(wh, "event_network_down", g_config.webhook.event_network_down);
        IMPORT_BOOL(wh, "event_interference", g_config.webhook.event_interference);
        IMPORT_BOOL(wh, "event_ota", g_config.webhook.event_ota);
        IMPORT_BOOL(wh, "event_temp_high", g_config.webhook.event_temp_high);
        IMPORT_BOOL(wh, "event_memory_low", g_config.webhook.event_memory_low);
    }

    // led
    cJSON *led = cJSON_GetObjectItem(root, "led");
    if (led) {
        IMPORT_U8(led, "brightness", g_config.led.brightness);
        IMPORT_BOOL(led, "night_mode_enabled", g_config.led.night_mode_enabled);
        IMPORT_U8(led, "night_start_hour", g_config.led.night_start_hour);
        IMPORT_U8(led, "night_start_min", g_config.led.night_start_min);
        IMPORT_U8(led, "night_end_hour", g_config.led.night_end_hour);
        IMPORT_U8(led, "night_end_min", g_config.led.night_end_min);
        IMPORT_U32(led, "color_booting", g_config.led.color_booting);
        IMPORT_U32(led, "color_normal", g_config.led.color_normal);
        IMPORT_U32(led, "color_zigbee", g_config.led.color_zigbee);
        IMPORT_U32(led, "color_error", g_config.led.color_error);
        IMPORT_U32(led, "color_ota", g_config.led.color_ota);
    }

    // whitelist
    cJSON *wl_obj = cJSON_GetObjectItem(root, "tcp_whitelist");
    if (wl_obj) {
        IMPORT_BOOL(wl_obj, "enabled", g_config.whitelist.enabled);
        cJSON *ips = cJSON_GetObjectItem(wl_obj, "allowed_ips");
        if (ips && cJSON_IsArray(ips)) {
            g_config.whitelist.count = 0;
            cJSON *ip;
            cJSON_ArrayForEach(ip, ips) {
                if (g_config.whitelist.count >= MAX_WHITELIST_IPS) break;
                if (cJSON_IsNumber(ip)) {
                    g_config.whitelist.allowed_ips[g_config.whitelist.count++] =
                        (uint32_t)ip->valuedouble;
                }
            }
        }
    }

    // ntp
    cJSON *ntp = cJSON_GetObjectItem(root, "ntp");
    if (ntp) {
        IMPORT_STR(ntp, "server", g_config.ntp.server, sizeof(g_config.ntp.server));
        IMPORT_STR(ntp, "timezone", g_config.ntp.timezone, sizeof(g_config.ntp.timezone));
        IMPORT_U32(ntp, "sync_interval", g_config.ntp.sync_interval);
        /* 老备份没有此字段 → 保持 false，不把默认 UTC 当成用户声明过 */
        IMPORT_BOOL(ntp, "tz_declared", g_config.ntp.tz_declared);
    }

    /* Sanitize: imported values bypass the setters' range checks, so an
       edited/corrupt backup could persist values the rest of the firmware
       assumes are valid (S1: imported JSON is untrusted input). */
    /* 老备份（本版之前）只有 channel、没有 channel_declared。那时的 channel
       是个从未被用户确认过的默认值，恢复时绝不能当成"用户声明过"。 */
    if (g_config.zigbee.channel_declared &&
        (g_config.zigbee.channel < 11 || g_config.zigbee.channel > 26)) {
        g_config.zigbee.channel_declared = false;
        g_config.zigbee.pan_known = false;
    }
    /* 地区只收菜单里的码（radio_region.h）：手改的备份塞进 "DE"，Thread 会当 UNKNOWN
       而 NCP 当 CE，两颗射频口径不一致，所以整条丢掉当没设过 */
    radio_region_normalize(g_config.sys.radio_region);
    if (!radio_region_code_valid(g_config.sys.radio_region)) g_config.sys.radio_region[0] = '\0';
    /* rest_auth=true 但没有密码 = 假保护（check_auth 在无密码时恒真），
       UI 还会显示"已开启"。HTTP 端点拦了这一条，但配置导入绕得过去，
       所以在这里兜一道。 */
    if (g_config.sys.password_hash[0] == '\0')
        g_config.thread.rest_auth = false;

    /* token 的格式校验只长在 config_mgr_set_otbr_token() 里，而 IMPORT_SECRET_STR
       是直接写结构体的 —— 同 extra_host 那条，两条写入路径规则不一致等于 API 拒掉
       的值可以从备份绕进来。畸形 token 的后果不是放行而是**永远匹配不上**：
       HA 侧连不上，用户看到的是"我恢复了备份 Thread 就废了"，而 UI 上 token
       明明显示着 —— 无从排查。所以宁可清掉并告警。
       注意：token 不像 rest_auth 那样依赖密码，它自己就是凭据，不做"没密码就关掉"。 */
    if (g_config.thread.otbr_token[0]) {
        size_t n = strlen(g_config.thread.otbr_token);
        bool ok = (n == 32);
        for (size_t i = 0; ok && i < n; i++) {
            char c = g_config.thread.otbr_token[i];
            ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }
        if (!ok) {
            ESP_LOGW(TAG, "导入的 otbr_token 格式非法，已丢弃");
            g_config.thread.otbr_token[0] = '\0';
        }
    }

    /* 越界兜底必须与出厂默认同一个值：夹到 15 会把 Thread 压在 ZHA 的默认信道上，
       正是 THREAD_CHANNEL_DEFAULT 选 25 要避开的事（2026-09-11 修，T2.10）。 */
    if (g_config.thread.channel < 11 || g_config.thread.channel > 26)
        g_config.thread.channel = THREAD_CHANNEL_DEFAULT;
    if (g_config.sys.bridge_mode > 2) g_config.sys.bridge_mode = 0;
    g_config.led.night_start_hour %= 24;
    g_config.led.night_end_hour   %= 24;
    g_config.led.night_start_min  %= 60;
    g_config.led.night_end_min    %= 60;
    if (g_config.mqtt.enabled && g_config.mqtt.port == 0)
        g_config.mqtt.port = 1883;

    xSemaphoreGive(s_mutex);
    cJSON_Delete(root);

    // Immediate persist (explicit user action)
    config_mgr_save_now();

    esp_event_post(CONFIG_EVENT, CONFIG_EVENT_IMPORTED, NULL, 0, 0);

    /* 导入不只是把值写进 NVS，还必须让它**生效**。
     *
     * 2026-08-13 实测：恢复一份含时区的备份后，NVS 里是对的，而运行时的 TZ
     * 仍是导入前的值 —— 因为没有任何人处理 CONFIG_EVENT_IMPORTED，而 NTP 的
     * 应用逻辑挂在 CONFIG_EVENT_NTP_CHANGED 上。MQTT 同理（换了 broker 也不
     * 会重连）。这与当初"NTP 事件发了没人接"是同一类错误：**存了 ≠ 生效**。
     *
     * 补发各子系统自己的 CHANGED 事件，现有消费者直接复用，将来新增的消费者
     * 也自动获得导入支持，不用每个都去订阅 IMPORTED。
     *
     * ⚠️ 故意**不发** WIFI/ETH_CHANGED：那些会重配网络接口，而调用方正通过这条
     * 网络等 HTTP 响应，重配会把响应打断，用户看到的是"导入失败"。
     * 网络类配置仍然按原样需要重启生效。
     *
     * ⚠️ 2026-09-11(T3.2) 订正：HOSTNAME_CHANGED **原本也在这份排除名单里**，
     * 理由写的是"会重配网络接口"—— 那个理由对它不成立。它唯一的订阅者是
     * mdns_svc，做的事只有 mdns_hostname_set()，**不碰 netif**，HTTP 响应
     * 照常送达。不发的后果反而是：恢复一份改过主机名的备份，NVS 里是新名字，
     * 而 mDNS 还在广播旧名字，直到下次重启 —— 又一个"存了 ≠ 生效"。
     *
     * LED / webhook / 白名单不在此列，因为它们每次使用时都重新读配置，本来
     * 就立刻生效。 */
    esp_event_post(CONFIG_EVENT, CONFIG_EVENT_NTP_CHANGED,  NULL, 0, 0);
    esp_event_post(CONFIG_EVENT, CONFIG_EVENT_MQTT_CHANGED, NULL, 0, 0);
    esp_event_post(CONFIG_EVENT, CONFIG_EVENT_HOSTNAME_CHANGED, NULL, 0, 0);

    /* extra_host 的字符校验只长在 config_mgr_set_extra_host() 里，而 IMPORT_STR
       是直接写结构体的 —— 两条写入路径规则不一致，等于 API 拒掉的值可以从一份
       备份里绕进来。这里补一次同样的校验。
       为什么是"清空 + 打警告"而不是"整份导入失败"：非法值的后果只是这条放行
       永不命中（守卫 fail-safe，拒绝而不是放行），为它作废整份配置恢复不划算；
       但如果**静默**留着它，用户看到的是"我恢复了备份，却还是被 421 挡着"，
       而 /net/access-host 里明明显示着那个值 —— 无从排查。 */
    if (g_config.sys.extra_host[0]) {
        char probe[sizeof(g_config.sys.extra_host)];
        strlcpy(probe, g_config.sys.extra_host, sizeof(probe));
        g_config.sys.extra_host[0] = '\0';          /* 先清，校验过再放回 */
        if (config_mgr_set_extra_host(probe) != ESP_OK) {
            ESP_LOGW(TAG, "导入的 extra_host \"%s\" 不合法，已丢弃", probe);
        }
    }

    ESP_LOGI(TAG, "Config imported from JSON");
    return ESP_OK;
}
