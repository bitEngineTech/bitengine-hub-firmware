#include "webui.h"
#include "json_escape.h"
#include "watchdog.h"
#include "network_mgr.h"
#include "config_mgr.h"
#include "ncp_version.h"
#include "ezsp_bridge.h"   /* ezsp_bridge_get_mode() —— coord_mode 的 usb 档 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>   /* atoi —— saveParams 的 pageId */
#include <inttypes.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "smlight";

// ---- GET /api2/info ----
static esp_err_t handle_api2_info(httpd_req_t *req)
{
    AUTH_REQUIRED_UNLESS_OPEN(req);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    const esp_app_desc_t *app = esp_app_get_description();
    const sys_config_t *sys = config_mgr_get_sys();

    char safe_host[40];
    json_sanitize_copy(safe_host, sys->hostname, sizeof(safe_host));

    /* 这个字段以前是写死的 "EmberZNet"——HA 的 SMLIGHT 集成会把它当作
       Zigbee 固件版本显示，等于一直在展示一个假值。现在报实读的版本；
       读不到（MG24 没应答）时退回原来的裸串，不要编一个数字出来。 */
    char zb_ver[56];
    char ncp[40];
    ncp_version_format(ncp, sizeof(ncp));
    if (ncp_version_get()->valid) {
        snprintf(zb_ver, sizeof(zb_ver), "EmberZNet %s", ncp);
    } else {
        strlcpy(zb_ver, "EmberZNet", sizeof(zb_ver));
    }

    char buf[320];
    /* model 是本机型号，不是"必须假装成 SLZB-06"的字段：Z2M / ZHA 靠
       mDNS 的 _slzb-06._tcp 服务类型和本接口的形状来识别，不解析 model。
       所以这里报自己的真实型号。 */
    snprintf(buf, sizeof(buf),
        "{\"model\":\"bitEngine-Hub-C5\","
        "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
        "\"fw_version\":\"%s\","
        "\"zb_version\":\"%s\","
        "\"zb_chip\":\"EFR32MG24\","
        "\"hostname\":\"%s\"}",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        app->version,
        zb_ver,
        safe_host);
    return send_json_response(req, buf);
}

// ---- GET /api2/state ----
static esp_err_t handle_api2_state(httpd_req_t *req)
{
    AUTH_REQUIRED_UNLESS_OPEN(req);

    const health_status_t *hs = watchdog_get_status();
    network_status_t ns;
    network_mgr_get_status(&ns);

    char ip_buf[16] = "0.0.0.0";
    if (ns.wifi_has_ip) {
        uint32_t a = ns.wifi_ip.addr;
        snprintf(ip_buf, sizeof(ip_buf), "%u.%u.%u.%u",
                 (uint8_t)(a), (uint8_t)(a>>8), (uint8_t)(a>>16), (uint8_t)(a>>24));
    } else if (ns.eth_has_ip) {
        uint32_t a = ns.eth_ip.addr;
        snprintf(ip_buf, sizeof(ip_buf), "%u.%u.%u.%u",
                 (uint8_t)(a), (uint8_t)(a>>8), (uint8_t)(a>>16), (uint8_t)(a>>24));
    }

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"uptime\":%"PRIu32","
        "\"ip\":\"%s\","
        "\"temperature\":%.1f,"
        "\"wifi_connected\":%s,"
        "\"eth_connected\":%s}",
        hs ? hs->uptime_seconds : 0,
        ip_buf,
        hs ? hs->temperature : 0.0f,
        ns.wifi_connected ? "true" : "false",
        ns.eth_link_up ? "true" : "false");
    return send_json_response(req, buf);
}

/* ---- POST /settings/saveParams — HA SMLIGHT 集成拨开关时打这里 ----
 *
 * 契约来自 pysmlight（web.py:370 set_toggle）：application/x-www-form-urlencoded，
 * 形如  pageId=8&disableLeds=on&ha=True 。判成功只看 **HTTP 200**（web.py:122
 * `return response.status == 200`），响应体内容不看。
 *
 * 只认 LED 那一页（Pages.API2_PAGE_SETTINGS_LED = 8，const.py:73）。其余 pageId
 * 一律回 200 但什么都不做 —— 回 404 会让 pysmlight 抛 SmlightConnectionError，
 * 把整个集成打成不可用，代价远大于"某个我们没实现的开关拨不动"。
 *
 * ⚠️ 光报状态不接写入是**更差**的：HA 里那两个开关看着能拨、拨完弹错误再跳回去，
 * 比一直 unknown 更让人困惑。要么两头都做，要么都别做。 */
static bool form_get(const char *body, const char *key, char *out, size_t outsz)
{
    size_t klen = strlen(key);
    const char *p = body;
    while ((p = strstr(p, key)) != NULL) {
        /* 必须是完整的 key：前面是开头或 '&'，后面紧跟 '=' */
        bool at_start = (p == body) || (p[-1] == '&');
        if (at_start && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *e = strchr(v, '&');
            size_t n = e ? (size_t)(e - v) : strlen(v);
            if (n >= outsz) n = outsz - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            return true;
        }
        p += klen;
    }
    return false;
}

static esp_err_t handle_settings_save(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    /* 表单很短（几十字节）。给足余量后仍超长 = 不是这个契约的请求，直接拒。 */
    char body[256];
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) return send_error(req, 400, "bad_body_len");
    int got = httpd_req_recv(req, body, len);
    if (got <= 0) return send_error(req, 400, "recv_failed");
    body[got] = '\0';

    char page[8] = "";
    if (!form_get(body, "pageId", page, sizeof(page))) return send_error(req, 400, "no_page_id");
    if (atoi(page) != 8) {
        /* 不是 LED 页：如实回 200 但不动任何东西，理由见函数头注释。 */
        ESP_LOGW(TAG, "saveParams pageId=%s 未实现，已忽略", page);
        return send_json_response(req, "{\"status\":\"ok\",\"note\":\"page_not_implemented\"}");
    }

    led_config_t led = *config_mgr_get_led();
    bool changed = false;
    char v[8];

    if (form_get(body, "nightMode", v, sizeof(v))) {
        led.night_mode_enabled = (strcmp(v, "on") == 0);
        changed = true;
    }
    if (form_get(body, "disableLeds", v, sizeof(v))) {
        /* SMLIGHT 的 disableLeds 是个布尔，而我们只有亮度。约定 0 = 关灯。
           关灯前把当时的亮度记在 RAM 里，开回来时还原 —— 否则用户自定义的亮度
           会被一次误触永久变成出厂值。重启后这份记忆丢失，那时按出厂默认 128
           还原（比还原成 0 好：至少灯是亮的，用户看得见东西）。 */
        static uint8_t s_brightness_before_off = 128;
        bool off = (strcmp(v, "on") == 0);   /* disableLeds=on 意为"禁用" */
        if (off) {
            if (led.brightness > 0) s_brightness_before_off = led.brightness;
            led.brightness = 0;
        } else {
            led.brightness = s_brightness_before_off ? s_brightness_before_off : 128;
        }
        changed = true;
    }

    if (changed) {
        esp_err_t err = config_mgr_set_led(&led);
        if (err != ESP_OK) return send_error(req, 500, "save_failed");
        ESP_LOGI(TAG, "saveParams: brightness=%u night_mode=%d",
                 led.brightness, (int)led.night_mode_enabled);
    }
    return send_json_response(req, "{\"status\":\"ok\"}");
}

// ---- POST /api2/reboot ----
static esp_err_t handle_api2_reboot(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    send_json_response(req, "{\"status\":\"ok\"}");
    ESP_LOGW(TAG, "Reboot requested via /api2/reboot");
    (void)config_mgr_flush(500);       /* 设置页"重启"：刚改的设置还在 2 s 防抖里 */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// ---- POST /api2/zb_restart ----
static esp_err_t handle_api2_zb_restart(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    esp_err_t err = watchdog_reset_mg24();
    if (err == ESP_ERR_NOT_ALLOWED)
        return send_error(req, 429, "cooldown_active");
    if (err != ESP_OK)
        return send_error(req, 500, "reset_failed");
    return send_json_response(req, "{\"status\":\"ok\"}");
}

// ---- GET /ha_info — HA SMLIGHT integration calls this ----
static esp_err_t handle_ha_info(httpd_req_t *req)
{
    AUTH_REQUIRED(req);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    const esp_app_desc_t *app = esp_app_get_description();
    const sys_config_t *sys = config_mgr_get_sys();

    network_status_t ns;
    network_mgr_get_status(&ns);

    /* device_ip 报**当前生效接口**的地址。以前只看 wifi_has_ip，走以太网时
       （PoE 是主部署形态）恒报 0.0.0.0。2026-08-20 订正。 */
    char ip_buf[16] = "0.0.0.0";
    uint32_t a = 0;
    if (ns.active_interface == NET_IF_ETH && ns.eth_has_ip)        a = ns.eth_ip.addr;
    else if (ns.wifi_has_ip)                                        a = ns.wifi_ip.addr;
    else if (ns.eth_has_ip)                                         a = ns.eth_ip.addr;
    if (a) {
        snprintf(ip_buf, sizeof(ip_buf), "%u.%u.%u.%u",
                 (uint8_t)(a), (uint8_t)(a>>8), (uint8_t)(a>>16), (uint8_t)(a>>24));
    }

    /* coord_mode 是**下标不是标志位**：HA 的 SMLIGHT 集成拿它去索引
       options=["eth","wifi","usb"]（sensor.py 的 SmInfoSensorEntity.native_value：
       `options[value]`）。以前写死 0，于是 sensor.bitengine_connection_mode
       永远显示 "eth" —— 实测走 WiFi 时它仍报 eth，而同一份回包里
       binary_sensor.bitengine_ethernet 是 off，自相矛盾。2026-08-20 订正。
       越界会让 HA 把实体置为 unknown（native_value 返回 None），
       所以没网时宁可报 wifi 也不要报越界值。 */
    int coord_mode;
    if (ezsp_bridge_get_mode() == BRIDGE_MODE_USB)      coord_mode = 2;  /* usb */
    else if (ns.active_interface == NET_IF_ETH)         coord_mode = 0;  /* eth */
    else                                                coord_mode = 1;  /* wifi（含 SoftAP）*/

    /* fw_channel：HA 只认 ["dev","release"] 两个值。
       ⚠️ 以前这里写着"不能直接上报 config 里的 sys.firmware_channel" ——
       那个字段 2026-09-11(T4.3) 已经整个删掉了：存得好好的却零消费者，而全仓
       唯一提到它的就是这句"别用它"的注释。
       这里报的是**关于这个二进制本身**、能验证的事实：带 -dirty = 工作区有未提交
       改动 = 手工构建，不是发布版。以前写死 "release"，连本地改出来的固件也自称
       release。2026-08-20 订正。 */
    const char *fw_channel = strstr(app->version, "-dirty") ? "dev" : "release";

    char safe_host[40];
    json_sanitize_copy(safe_host, sys->hostname, sizeof(safe_host));

    /* 同 /api2/info：以前写死 "EmberZNet 9.0.1"，那是个从未成立的版本号。 */
    char zb_ver[56];
    char ncp[40];
    ncp_version_format(ncp, sizeof(ncp));
    if (ncp_version_get()->valid) {
        snprintf(zb_ver, sizeof(zb_ver), "EmberZNet %s", ncp);
    } else {
        strlcpy(zb_ver, "EmberZNet", sizeof(zb_ver));
    }

    /* zb_channel 报用户声明值（实测值拿不到，见 ncp_version.h）。这里是对外
       兼容契约必须是整数，未声明时给 0。实测确认 HA 的 SMLIGHT 集成根本不读
       这个字段（整个 components/smlight/ 里 zb_channel 出现 0 次），所以给 0
       没有可观察影响。 */
    const zigbee_config_t *zbc = config_mgr_get_zigbee();
    int zb_channel = (zbc && zbc->channel_declared) ? zbc->channel : 0;

    char buf[560];
    snprintf(buf, sizeof(buf),
        "{\"Info\":{\"coord_mode\":%d,"
        "\"device_ip\":\"%s\","
        "\"fw_channel\":\"%s\","
        "\"hostname\":\"%s\","
        /* legacy_api **必须是 0**。这个数字直接决定 HA 走哪条路：
           coordinator.py:96 `self.legacy_api = info.legacy_api`
           :167 `if not self.legacy_api: sensors = await client.get_sensors()`
           报 2 的后果有两个，都实测确认过（2026-08-20）：
             ① HA **根本不去读 /ha_sensors**，于是温度/内存/开关等实体恒为
                unknown —— 我们明明在发 esp32_temp，它就是不取
             ② coordinator.py:97 会挂一条 error 级修复提示 unsupported_firmware，
                用户 HA 首页常驻一个红色告警
           0 = 现代 API（/ha_info + /ha_sensors），正是我们实现的那套。 */
        "\"legacy_api\":0,"
        "\"MAC\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
        "\"model\":\"SLZB-06\","
        "\"ram_total\":%lu,"
        "\"sw_version\":\"%s\","
        "\"zb_channel\":%d,"
        "\"zb_hw\":\"EFR32MG24\","
        "\"zb_version\":\"%s\","
        "\"zb_type\":0,"
        "\"zb_flash_size\":1024,"
        "\"zb_ram_size\":256}}",
        coord_mode,
        ip_buf,
        fw_channel,
        safe_host,
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        (unsigned long)(esp_get_free_heap_size()),
        app->version,
        zb_channel,
        zb_ver);
    return send_json_response(req, buf);
}

// ---- GET /ha_sensors — HA SMLIGHT integration calls this ----
static esp_err_t handle_ha_sensors(httpd_req_t *req)
{
    AUTH_REQUIRED(req);

    const health_status_t *hs = watchdog_get_status();
    network_status_t ns;
    network_mgr_get_status(&ns);
    const led_config_t *led = config_mgr_get_led();

    /* ⚠️ 刻意**不报** zb_temp：MG24 的温度全仓拿不到（要成为完整 EZSP 主机才读得
       到，违反决策 1）。字段缺失时 HA 的 sensor.zigbee_chip_temp 保持 unknown，
       那是如实的；编一个数字才是错的。 */
    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\"Sensors\":{\"esp32_temp\":%.1f,"
        "\"uptime\":%"PRIu32","
        "\"ram_usage\":%lu,"
        "\"ethernet\":%s,"
        "\"wifi_connected\":%s,"
        "\"wifi_status\":1,"
        "\"disable_leds\":%s,"
        "\"night_mode\":%s,"
        "\"socket_uptime\":%"PRIu32"}}",
        hs ? hs->temperature : 0.0f,
        hs ? hs->uptime_seconds : 0,
        (unsigned long)(esp_get_free_heap_size()),
        ns.eth_link_up ? "true" : "false",
        ns.wifi_connected ? "true" : "false",
        (led && led->brightness == 0) ? "true" : "false",
        (led && led->night_mode_enabled) ? "true" : "false",
        hs ? hs->uptime_seconds : 0);
    return send_json_response(req, buf);
}

// ---- GET /api2 — pysmlight calls /api2?action=0&page=N ----
static esp_err_t handle_api2(httpd_req_t *req)
{
    // pysmlight uses action=0 (get page) with page number, and probes this
    // endpoint to detect whether the device requires authentication: it must
    // return 401 when a password is set and the request is unauthenticated,
    // otherwise HA believes the device is open. Basic auth is accepted
    // (check_auth) so pysmlight can authenticate.
    AUTH_REQUIRED(req);

    char query[64] = "";
    httpd_req_get_url_query_str(req, query, sizeof(query));

    // For auth check (action=0, page=1), just return empty JSON object
    return send_json_response(req, "{}");
}

// ---- Registration ----
void register_smlight_handlers(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {  /* static：同 rest_api.c 的路由表，别放栈上 */
        { .uri = "/api2/info",       .method = HTTP_GET,  .handler = handle_api2_info },
        { .uri = "/api2/state",      .method = HTTP_GET,  .handler = handle_api2_state },
        { .uri = "/api2/reboot",     .method = HTTP_POST, .handler = handle_api2_reboot },
        { .uri = "/api2/zb_restart", .method = HTTP_POST, .handler = handle_api2_zb_restart },
        { .uri = "/api2",            .method = HTTP_GET,  .handler = handle_api2 },
        { .uri = "/ha_info",        .method = HTTP_GET,  .handler = handle_ha_info },
        { .uri = "/ha_sensors",     .method = HTTP_GET,  .handler = handle_ha_sensors },
        { .uri = "/settings/saveParams", .method = HTTP_POST, .handler = handle_settings_save },
    };
    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
        httpd_register_uri_handler(server, &uris[i]);
    ESP_LOGI(TAG, "SMLIGHT compat: %d endpoints registered", (int)(sizeof(uris) / sizeof(uris[0])));
}
