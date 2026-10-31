#include "config_mgr.h"
#include "utf8.h"

#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "mbedtls/md.h"

static const char *TAG = "config_mgr";

ESP_EVENT_DEFINE_BASE(CONFIG_EVENT);

// ---- Global state ----
// Not static: config_export.c accesses g_config and s_mutex via extern
gateway_config_t          g_config;
SemaphoreHandle_t         s_mutex;
static TimerHandle_t      s_debounce_timer;
static bool               s_dirty = false;
static volatile bool      s_factory_resetting = false;

// ---- Forward declarations ----
static void debounce_callback(TimerHandle_t timer);
static esp_err_t config_save_to_nvs(void);
static esp_err_t config_load_from_nvs(void);
static void sha256_hex(const char *input, char *output);

// ---- NVS helpers ----

static void nvs_read_str(nvs_handle_t h, const char *key, char *buf, size_t max)
{
    size_t len = max;
    if (nvs_get_str(h, key, buf, &len) != ESP_OK) {
        // keep default (already set)
    }
}

static void nvs_read_u8(nvs_handle_t h, const char *key, uint8_t *val)
{
    nvs_get_u8(h, key, val);
}

static void nvs_read_u16(nvs_handle_t h, const char *key, uint16_t *val)
{
    nvs_get_u16(h, key, val);
}

static void nvs_read_u32(nvs_handle_t h, const char *key, uint32_t *val)
{
    nvs_get_u32(h, key, val);
}

static void nvs_read_bool(nvs_handle_t h, const char *key, bool *val)
{
    uint8_t v;
    if (nvs_get_u8(h, key, &v) == ESP_OK) {
        *val = (v != 0);
    }
}

// ---- NVS Load ----

static esp_err_t config_load_from_nvs(void)
{
    nvs_handle_t h;

    // -- sys --
    if (nvs_open("sys", NVS_READONLY, &h) == ESP_OK) {
        nvs_read_str(h, "hostname", g_config.sys.hostname, sizeof(g_config.sys.hostname));
        nvs_read_str(h, "extra_host", g_config.sys.extra_host, sizeof(g_config.sys.extra_host));
        nvs_read_u8(h, "bridge_mode", &g_config.sys.bridge_mode);
        nvs_read_str(h, "radio_region", g_config.sys.radio_region, sizeof(g_config.sys.radio_region));
        /* NVS 里的值绕过了 setter；坏了就当没设过，别把一个不在菜单里的码发给 NCP */
        radio_region_normalize(g_config.sys.radio_region);
        if (!radio_region_code_valid(g_config.sys.radio_region)) g_config.sys.radio_region[0] = '\0';
        nvs_read_str(h, "pwd_hash", g_config.sys.password_hash, sizeof(g_config.sys.password_hash));

        uint8_t ver = 0;
        if (nvs_get_u8(h, "cfg_version", &ver) == ESP_OK) {
            g_config.version = ver;
        }
        nvs_close(h);
    } else {
        ESP_LOGI(TAG, "First boot — no sys namespace in NVS");
    }

    // -- wifi (with key migration: "pass" → "password") --
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_read_str(h, "ssid", g_config.wifi.ssid, sizeof(g_config.wifi.ssid));

        // Try new key first, then old key
        size_t plen = sizeof(g_config.wifi.password);
        if (nvs_get_str(h, "password", g_config.wifi.password, &plen) != ESP_OK) {
            plen = sizeof(g_config.wifi.password);
            if (nvs_get_str(h, "pass", g_config.wifi.password, &plen) == ESP_OK) {
                // Migrate: write to new key, erase old
                nvs_set_str(h, "password", g_config.wifi.password);
                nvs_erase_key(h, "pass");
                nvs_commit(h);
                ESP_LOGI(TAG, "Migrated wifi key 'pass' -> 'password'");
            }
        }

        nvs_read_u32(h, "static_ip", &g_config.wifi.static_ip);
        nvs_read_u32(h, "static_gw", &g_config.wifi.static_gateway);
        nvs_read_u32(h, "static_mask", &g_config.wifi.static_netmask);
        nvs_read_u32(h, "static_dns", &g_config.wifi.static_dns);
        nvs_close(h);

        g_config.wifi.has_credentials = (strlen(g_config.wifi.ssid) > 0);
    }

#if CONFIG_BITENGINE_WIFI_KCONFIG_FALLBACK
    /* Dev-only convenience: adopt menuconfig credentials when NVS is empty.
       MUST stay off for production (default n): a factory-fresh device must
       report no credentials so it enters SoftAP provisioning immediately
       instead of chasing a baked-in SSID forever. */
    if (!g_config.wifi.has_credentials) {
        const char *kc_ssid = CONFIG_BITENGINE_WIFI_SSID;
        if (strlen(kc_ssid) > 0 && strcmp(kc_ssid, "MyNetwork") != 0) {
            strlcpy(g_config.wifi.ssid, kc_ssid, sizeof(g_config.wifi.ssid));
            strlcpy(g_config.wifi.password, CONFIG_BITENGINE_WIFI_PASSWORD,
                    sizeof(g_config.wifi.password));
            g_config.wifi.has_credentials = true;
            ESP_LOGI(TAG, "WiFi credentials from Kconfig: SSID=\"%s\"", kc_ssid);
        }
    }
#endif

    // -- eth --
    if (nvs_open("eth", NVS_READONLY, &h) == ESP_OK) {
        nvs_read_bool(h, "dhcp", &g_config.eth.dhcp);
        nvs_read_u32(h, "static_ip", &g_config.eth.static_ip);
        nvs_read_u32(h, "static_gw", &g_config.eth.static_gateway);
        nvs_read_u32(h, "static_mask", &g_config.eth.static_netmask);
        nvs_read_u32(h, "static_dns", &g_config.eth.static_dns);
        nvs_close(h);
    }

    // -- zigbee --
    if (nvs_open("zigbee", NVS_READONLY, &h) == ESP_OK) {
        nvs_read_u8(h, "channel", &g_config.zigbee.channel);
        nvs_read_bool(h, "ch_declared", &g_config.zigbee.channel_declared);
        nvs_read_u16(h, "pan_id", &g_config.zigbee.pan_id);
        nvs_read_bool(h, "pan_known", &g_config.zigbee.pan_known);
        nvs_read_bool(h, "host_seen", &g_config.zigbee.host_ever_seen);
        /* 旧固件写过 "tx_power"/"turbo" 两个键，不读、不删；留在 NVS 里无害 */
        nvs_close(h);
    }

    // -- thread --
    if (nvs_open("thread", NVS_READONLY, &h) == ESP_OK) {
        nvs_read_bool(h, "enabled", &g_config.thread.enabled);
        nvs_read_u8(h, "channel", &g_config.thread.channel);
        nvs_read_bool(h, "rest_auth", &g_config.thread.rest_auth);
        nvs_read_str(h, "otbr_token", g_config.thread.otbr_token, sizeof(g_config.thread.otbr_token));
        nvs_close(h);
    }

    // -- mqtt --
    if (nvs_open("mqtt", NVS_READONLY, &h) == ESP_OK) {
        nvs_read_bool(h, "enabled", &g_config.mqtt.enabled);
        nvs_read_str(h, "broker", g_config.mqtt.broker, sizeof(g_config.mqtt.broker));
        nvs_read_u16(h, "port", &g_config.mqtt.port);
        nvs_read_str(h, "username", g_config.mqtt.username, sizeof(g_config.mqtt.username));
        nvs_read_str(h, "password", g_config.mqtt.password, sizeof(g_config.mqtt.password));
        nvs_read_str(h, "topic_pfx", g_config.mqtt.topic_prefix, sizeof(g_config.mqtt.topic_prefix));
        nvs_read_u16(h, "pub_intv", &g_config.mqtt.publish_interval);
        nvs_read_bool(h, "tls", &g_config.mqtt.tls_enabled);
        nvs_close(h);
    }

    // -- webhook --
    if (nvs_open("webhook", NVS_READONLY, &h) == ESP_OK) {
        nvs_read_bool(h, "enabled", &g_config.webhook.enabled);
        nvs_read_str(h, "url", g_config.webhook.url, sizeof(g_config.webhook.url));
        nvs_read_u32(h, "backup_intv", &g_config.webhook.backup_interval);
        uint8_t mask = 0x3F;  // default all enabled
        nvs_read_u8(h, "evt_mask", &mask);
        g_config.webhook.event_zigbee_crash = (mask >> 0) & 1;
        g_config.webhook.event_network_down = (mask >> 1) & 1;
        g_config.webhook.event_interference = (mask >> 2) & 1;
        g_config.webhook.event_ota          = (mask >> 3) & 1;
        g_config.webhook.event_temp_high    = (mask >> 4) & 1;
        g_config.webhook.event_memory_low   = (mask >> 5) & 1;
        nvs_close(h);
    }

    // -- led --
    if (nvs_open("led", NVS_READONLY, &h) == ESP_OK) {
        nvs_read_u8(h, "brightness", &g_config.led.brightness);
        nvs_read_bool(h, "night_en", &g_config.led.night_mode_enabled);
        uint16_t packed;
        if (nvs_get_u16(h, "night_start", &packed) == ESP_OK) {
            g_config.led.night_start_hour = packed / 100;
            g_config.led.night_start_min  = packed % 100;
        }
        if (nvs_get_u16(h, "night_end", &packed) == ESP_OK) {
            g_config.led.night_end_hour = packed / 100;
            g_config.led.night_end_min  = packed % 100;
        }
        nvs_read_u32(h, "clr_boot", &g_config.led.color_booting);
        nvs_read_u32(h, "clr_normal", &g_config.led.color_normal);
        nvs_read_u32(h, "clr_zigbee", &g_config.led.color_zigbee);
        nvs_read_u32(h, "clr_error", &g_config.led.color_error);
        nvs_read_u32(h, "clr_ota", &g_config.led.color_ota);
        nvs_close(h);
    }

    // -- whitelist --
    if (nvs_open("whitelist", NVS_READONLY, &h) == ESP_OK) {
        nvs_read_bool(h, "enabled", &g_config.whitelist.enabled);
        nvs_read_u8(h, "count", &g_config.whitelist.count);
        /* Clamp: a corrupted count would make the TCP accept loop read
           past allowed_ips[] */
        if (g_config.whitelist.count > MAX_WHITELIST_IPS) {
            ESP_LOGW(TAG, "whitelist count %d > max %d, clamping",
                     g_config.whitelist.count, MAX_WHITELIST_IPS);
            g_config.whitelist.count = MAX_WHITELIST_IPS;
        }
        size_t blob_len = sizeof(g_config.whitelist.allowed_ips);
        nvs_get_blob(h, "ips", g_config.whitelist.allowed_ips, &blob_len);
        nvs_close(h);
    }

    // -- ntp --
    if (nvs_open("ntp", NVS_READONLY, &h) == ESP_OK) {
        nvs_read_str(h, "server", g_config.ntp.server, sizeof(g_config.ntp.server));
        nvs_read_str(h, "timezone", g_config.ntp.timezone, sizeof(g_config.ntp.timezone));
        nvs_read_u32(h, "sync_intv", &g_config.ntp.sync_interval);
        /* 老配置没有这个 key，读不到就保持 false = 未声明 */
        nvs_read_bool(h, "tz_decl", &g_config.ntp.tz_declared);
        nvs_close(h);

        /* 迁移：tz_decl 是后加的字段，比它更早写入的配置一律读到 false。
           但时区如果不是出厂默认，那它只可能来自用户导入的配置 —— 那就是
           声明过。不做这个推断的话，已经配好时区的用户会被无谓提示一次。
           反过来仍然保守：值就是默认 UTC 时绝不当成已声明。 */
        if (!g_config.ntp.tz_declared &&
            strcmp(g_config.ntp.timezone, NTP_TIMEZONE_DEFAULT) != 0) {
            g_config.ntp.tz_declared = true;
        }
    }

    return ESP_OK;
}

// ---- NVS Save ----

static esp_err_t config_save_to_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err;
    esp_err_t first_err = ESP_OK;

#define NVS_WRITE_NS(ns, body) do { \
    err = nvs_open(ns, NVS_READWRITE, &h); \
    if (err == ESP_OK) { body; err = nvs_commit(h); nvs_close(h); } \
    if (err != ESP_OK) { \
        ESP_LOGW(TAG, "NVS write '%s' failed: %s", ns, esp_err_to_name(err)); \
        if (first_err == ESP_OK) first_err = err; \
    } \
} while (0)

    NVS_WRITE_NS("sys", {
        nvs_set_str(h, "hostname", g_config.sys.hostname);
        nvs_set_str(h, "extra_host", g_config.sys.extra_host);
        nvs_set_u8(h, "bridge_mode", g_config.sys.bridge_mode);
        nvs_set_str(h, "radio_region", g_config.sys.radio_region);
        nvs_set_str(h, "pwd_hash", g_config.sys.password_hash);
        nvs_set_u8(h, "cfg_version", g_config.version);
    });

    NVS_WRITE_NS("wifi", {
        nvs_set_str(h, "ssid", g_config.wifi.ssid);
        nvs_set_str(h, "password", g_config.wifi.password);
        nvs_set_u32(h, "static_ip", g_config.wifi.static_ip);
        nvs_set_u32(h, "static_gw", g_config.wifi.static_gateway);
        nvs_set_u32(h, "static_mask", g_config.wifi.static_netmask);
        nvs_set_u32(h, "static_dns", g_config.wifi.static_dns);
    });

    NVS_WRITE_NS("eth", {
        nvs_set_u8(h, "dhcp", g_config.eth.dhcp ? 1 : 0);
        nvs_set_u32(h, "static_ip", g_config.eth.static_ip);
        nvs_set_u32(h, "static_gw", g_config.eth.static_gateway);
        nvs_set_u32(h, "static_mask", g_config.eth.static_netmask);
        nvs_set_u32(h, "static_dns", g_config.eth.static_dns);
    });

    NVS_WRITE_NS("zigbee", {
        nvs_set_u8(h, "channel", g_config.zigbee.channel);
        nvs_set_u8(h, "ch_declared", g_config.zigbee.channel_declared ? 1 : 0);
        nvs_set_u16(h, "pan_id", g_config.zigbee.pan_id);
        nvs_set_u8(h, "pan_known", g_config.zigbee.pan_known ? 1 : 0);
        nvs_set_u8(h, "host_seen", g_config.zigbee.host_ever_seen ? 1 : 0);
    });

    NVS_WRITE_NS("thread", {
        nvs_set_u8(h, "enabled", g_config.thread.enabled ? 1 : 0);
        nvs_set_u8(h, "channel", g_config.thread.channel);
        nvs_set_u8(h, "rest_auth", g_config.thread.rest_auth ? 1 : 0);
        nvs_set_str(h, "otbr_token", g_config.thread.otbr_token);
    });

    NVS_WRITE_NS("mqtt", {
        nvs_set_u8(h, "enabled", g_config.mqtt.enabled ? 1 : 0);
        nvs_set_str(h, "broker", g_config.mqtt.broker);
        nvs_set_u16(h, "port", g_config.mqtt.port);
        nvs_set_str(h, "username", g_config.mqtt.username);
        nvs_set_str(h, "password", g_config.mqtt.password);
        nvs_set_str(h, "topic_pfx", g_config.mqtt.topic_prefix);
        nvs_set_u16(h, "pub_intv", g_config.mqtt.publish_interval);
        nvs_set_u8(h, "tls", g_config.mqtt.tls_enabled ? 1 : 0);
    });

    NVS_WRITE_NS("webhook", {
        nvs_set_u8(h, "enabled", g_config.webhook.enabled ? 1 : 0);
        nvs_set_str(h, "url", g_config.webhook.url);
        nvs_set_u32(h, "backup_intv", g_config.webhook.backup_interval);
        uint8_t mask = (g_config.webhook.event_zigbee_crash << 0) |
                       (g_config.webhook.event_network_down << 1) |
                       (g_config.webhook.event_interference << 2) |
                       (g_config.webhook.event_ota          << 3) |
                       (g_config.webhook.event_temp_high    << 4) |
                       (g_config.webhook.event_memory_low   << 5);
        nvs_set_u8(h, "evt_mask", mask);
    });

    NVS_WRITE_NS("led", {
        nvs_set_u8(h, "brightness", g_config.led.brightness);
        nvs_set_u8(h, "night_en", g_config.led.night_mode_enabled ? 1 : 0);
        nvs_set_u16(h, "night_start", g_config.led.night_start_hour * 100 + g_config.led.night_start_min);
        nvs_set_u16(h, "night_end", g_config.led.night_end_hour * 100 + g_config.led.night_end_min);
        nvs_set_u32(h, "clr_boot", g_config.led.color_booting);
        nvs_set_u32(h, "clr_normal", g_config.led.color_normal);
        nvs_set_u32(h, "clr_zigbee", g_config.led.color_zigbee);
        nvs_set_u32(h, "clr_error", g_config.led.color_error);
        nvs_set_u32(h, "clr_ota", g_config.led.color_ota);
    });

    NVS_WRITE_NS("whitelist", {
        nvs_set_u8(h, "enabled", g_config.whitelist.enabled ? 1 : 0);
        nvs_set_u8(h, "count", g_config.whitelist.count);
        nvs_set_blob(h, "ips", g_config.whitelist.allowed_ips,
                     g_config.whitelist.count * sizeof(uint32_t));
    });

    NVS_WRITE_NS("ntp", {
        nvs_set_str(h, "server", g_config.ntp.server);
        nvs_set_str(h, "timezone", g_config.ntp.timezone);
        nvs_set_u32(h, "sync_intv", g_config.ntp.sync_interval);
        nvs_set_u8(h, "tz_decl", g_config.ntp.tz_declared ? 1 : 0);
    });

#undef NVS_WRITE_NS

    if (first_err != ESP_OK) {
        ESP_LOGE(TAG, "Config save incomplete: %s", esp_err_to_name(first_err));
        return first_err;
    }
    ESP_LOGI(TAG, "Config saved to NVS");
    return ESP_OK;
}

/* NVS 落盘失败计数。两个 post 点共用。
   volatile：写在 config task / httpd task，读在 httpd task，只是两个字，
   不值得为它上锁 —— 读到旧值最多让 /status 晚一拍报出来。 */
static volatile uint32_t s_nvs_fail_count  = 0;
static volatile uint32_t s_nvs_fail_uptime = 0;

static void nvs_health_record(void)
{
    s_nvs_fail_count++;
    s_nvs_fail_uptime = (uint32_t)(esp_timer_get_time() / 1000000);
}

// ---- Debounce callback ----

static void debounce_callback(TimerHandle_t timer)
{
    if (s_factory_resetting) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_dirty) {
        esp_err_t err = config_save_to_nvs();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(err));
            /* 计数在锁内做（两次整数写，廉价）；真正的反应交给事件处理器在锁外做
               —— 这一处是持着 s_mutex 的，不能在这里做 LED / 日志格式化之类的事。
               ESP_LOGE 本身已经进了 log_store 的告警环（vprintf 钩子），不必另写。 */
            nvs_health_record();
            esp_event_post(CONFIG_EVENT, CONFIG_EVENT_NVS_ERROR, NULL, 0, 0);
        }
        s_dirty = false;
    }
    xSemaphoreGive(s_mutex);
}

static void mark_dirty_and_schedule(void)
{
    if (s_factory_resetting) return;  /* nothing may be written back anymore */
    s_dirty = true;
    if (xTimerReset(s_debounce_timer, 0) != pdPASS) {
        /* Timer queue full — the daemon is wedged. s_dirty stays set, the
           next successful schedule or save_now will persist the change. */
        ESP_LOGW(TAG, "debounce timer reset failed — save deferred");
    }
}

// ---- SHA256 password hashing ----

static void sha256_hex(const char *input, char *output)
{
    uint8_t hash[32];
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
               (const uint8_t *)input, strlen(input), hash);
    for (int i = 0; i < 32; i++) {
        sprintf(output + i * 2, "%02x", hash[i]);
    }
    output[64] = '\0';
}

// ============================================================
// Public API: Init
// ============================================================

esp_err_t config_mgr_init(void)
{
    ESP_LOGI(TAG, "Initializing config manager");

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    s_debounce_timer = xTimerCreate("cfg_debounce", pdMS_TO_TICKS(2000),
                                     pdFALSE, NULL, debounce_callback);
    if (!s_debounce_timer) return ESP_ERR_NO_MEM;

    // Load defaults first, then overwrite from NVS
    config_set_defaults(&g_config);

    // Detect first boot: try to open "sys" namespace
    nvs_handle_t sys_check;
    bool first_boot = (nvs_open("sys", NVS_READONLY, &sys_check) != ESP_OK);
    if (!first_boot) nvs_close(sys_check);

    config_load_from_nvs();

    if (first_boot) {
        ESP_LOGI(TAG, "First boot — writing defaults to NVS");
        config_save_to_nvs();
    } else if (g_config.version < CONFIG_VERSION_CURRENT) {
        config_run_migrations(g_config.version);
        g_config.version = CONFIG_VERSION_CURRENT;
        config_save_to_nvs();
        ESP_LOGI(TAG, "Config migrated to version %d", CONFIG_VERSION_CURRENT);
    }

    ESP_LOGI(TAG, "Config loaded: hostname=\"%s\" wifi=%s zigbee_ch=%d",
             g_config.sys.hostname,
             g_config.wifi.has_credentials ? g_config.wifi.ssid : "(none)",
             g_config.zigbee.channel);
    return ESP_OK;
}

// ============================================================
// Public API: Read (zero latency, no lock)
// ============================================================

const gateway_config_t       *config_mgr_get_all(void)       { return &g_config; }
const wifi_config_data_t     *config_mgr_get_wifi(void)      { return &g_config.wifi; }
const eth_config_t           *config_mgr_get_eth(void)       { return &g_config.eth; }
const zigbee_config_t        *config_mgr_get_zigbee(void)    { return &g_config.zigbee; }
const thread_config_t        *config_mgr_get_thread(void)    { return &g_config.thread; }
const mqtt_config_t          *config_mgr_get_mqtt(void)      { return &g_config.mqtt; }
const webhook_config_t       *config_mgr_get_webhook(void)   { return &g_config.webhook; }
const led_config_t           *config_mgr_get_led(void)       { return &g_config.led; }
const ntp_config_t           *config_mgr_get_ntp(void)       { return &g_config.ntp; }
const sys_config_t           *config_mgr_get_sys(void)       { return &g_config.sys; }
const tcp_whitelist_config_t *config_mgr_get_whitelist(void) { return &g_config.whitelist; }

// ============================================================
// Public API: Write (lock + debounce + event)
// ============================================================

/* T9.33：写进配置的字符串必须是合法 UTF-8（WiFi SSID 除外，见 set_wifi）。
   坏字节一旦进 NVS，/config/export 与 webhook 自动备份吐出来的就不是合法 JSON。
   REST 入口已经先拦了（回 400），这里是最后一道 —— 也防调用方传进没结尾的数组：
   先确认 cap 以内有 NUL，再交给 utf8_valid。 */
static bool str_field_ok(const char *s, size_t cap)
{
    return s && memchr(s, '\0', cap) != NULL && utf8_valid(s);
}

esp_err_t config_mgr_set_wifi(const char *ssid, const char *password)
{
    /* SSID 不查 UTF-8（2026-10-07 定）：它是 AP 定的任意字节，GBK 编码的中文名就不是
       UTF-8，拒了就连不上那台 AP；导出时另存成 ssid_hex（config_export.c）。
       密码要查：WPA 口令按规范只是可打印 ASCII。 */
    if (password && !utf8_valid(password)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy(g_config.wifi.ssid, ssid ? ssid : "", sizeof(g_config.wifi.ssid));
    strlcpy(g_config.wifi.password, password ? password : "", sizeof(g_config.wifi.password));
    g_config.wifi.has_credentials = (strlen(g_config.wifi.ssid) > 0);
    mark_dirty_and_schedule();
    xSemaphoreGive(s_mutex);

    esp_event_post(CONFIG_EVENT, CONFIG_EVENT_WIFI_CHANGED, NULL, 0, 0);
    return ESP_OK;
}

/* dns 单独一个参数：esp_netif_ip_info_t 里没有 DNS 字段，而 network_mgr 的
   apply_static_ip_config() 确实会下发 wifi.static_dns（network_mgr.c:462）。
   老签名只写三个字段，DNS 永远是 0 —— 静态 IP 配好了却没有 DNS，表现为
   "能 ping 通 IP、域名全不通"。此函数此前零调用者，改签名无成本。 */
esp_err_t config_mgr_set_wifi_static_ip(const esp_netif_ip_info_t *ip, uint32_t dns)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (ip) {
        g_config.wifi.static_ip      = ip->ip.addr;
        g_config.wifi.static_gateway = ip->gw.addr;
        g_config.wifi.static_netmask = ip->netmask.addr;
        g_config.wifi.static_dns     = dns;
    } else {
        g_config.wifi.static_ip = g_config.wifi.static_gateway =
        g_config.wifi.static_netmask = g_config.wifi.static_dns = 0;
    }
    mark_dirty_and_schedule();
    xSemaphoreGive(s_mutex);

    esp_event_post(CONFIG_EVENT, CONFIG_EVENT_WIFI_CHANGED, NULL, 0, 0);
    return ESP_OK;
}

esp_err_t config_mgr_set_eth(const eth_config_t *eth)
{
    if (!eth) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    g_config.eth = *eth;
    mark_dirty_and_schedule();
    xSemaphoreGive(s_mutex);

    esp_event_post(CONFIG_EVENT, CONFIG_EVENT_ETH_CHANGED, NULL, 0, 0);
    return ESP_OK;
}

esp_err_t config_mgr_set_zigbee(const zigbee_config_t *zb)
{
    if (!zb) return ESP_ERR_INVALID_ARG;
    /* 未声明时不校验信道值——它本来就没有意义，强行要求 11-26 会逼调用方
       编一个假值出来，那正是要避免的。 */
    if (zb->channel_declared && (zb->channel < 11 || zb->channel > 26))
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    g_config.zigbee = *zb;
    mark_dirty_and_schedule();
    xSemaphoreGive(s_mutex);

    esp_event_post(CONFIG_EVENT, CONFIG_EVENT_ZIGBEE_CHANGED, NULL, 0, 0);
    return ESP_OK;
}

esp_err_t config_mgr_set_thread(const thread_config_t *thread)
{
    if (!thread) return ESP_ERR_INVALID_ARG;
    if (thread->channel < 11 || thread->channel > 26) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    g_config.thread = *thread;
    mark_dirty_and_schedule();
    xSemaphoreGive(s_mutex);

    esp_event_post(CONFIG_EVENT, CONFIG_EVENT_THREAD_CHANGED, NULL, 0, 0);
    return ESP_OK;
}

esp_err_t config_mgr_set_mqtt(const mqtt_config_t *mqtt)
{
    if (!mqtt) return ESP_ERR_INVALID_ARG;
    if (mqtt->enabled && (mqtt->broker[0] == '\0' || mqtt->port == 0)) {
        return ESP_ERR_INVALID_ARG;  /* enabled MQTT needs a reachable broker */
    }
    if (!str_field_ok(mqtt->broker, sizeof(mqtt->broker)) ||
        !str_field_ok(mqtt->username, sizeof(mqtt->username)) ||
        !str_field_ok(mqtt->password, sizeof(mqtt->password)) ||
        !str_field_ok(mqtt->topic_prefix, sizeof(mqtt->topic_prefix))) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    g_config.mqtt = *mqtt;
    mark_dirty_and_schedule();
    xSemaphoreGive(s_mutex);

    esp_event_post(CONFIG_EVENT, CONFIG_EVENT_MQTT_CHANGED, NULL, 0, 0);
    return ESP_OK;
}

esp_err_t config_mgr_set_webhook(const webhook_config_t *wh)
{
    if (!wh) return ESP_ERR_INVALID_ARG;
    if (!str_field_ok(wh->url, sizeof(wh->url))) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    g_config.webhook = *wh;
    mark_dirty_and_schedule();
    xSemaphoreGive(s_mutex);

    esp_event_post(CONFIG_EVENT, CONFIG_EVENT_WEBHOOK_CHANGED, NULL, 0, 0);
    return ESP_OK;
}

esp_err_t config_mgr_set_led(const led_config_t *led)
{
    if (!led) return ESP_ERR_INVALID_ARG;
    if (led->night_start_hour > 23 || led->night_end_hour > 23) return ESP_ERR_INVALID_ARG;
    if (led->night_start_min > 59 || led->night_end_min > 59) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    g_config.led = *led;
    mark_dirty_and_schedule();
    xSemaphoreGive(s_mutex);

    esp_event_post(CONFIG_EVENT, CONFIG_EVENT_LED_CHANGED, NULL, 0, 0);
    return ESP_OK;
}

esp_err_t config_mgr_set_ntp(const ntp_config_t *ntp)
{
    if (!ntp) return ESP_ERR_INVALID_ARG;
    if (!str_field_ok(ntp->server, sizeof(ntp->server)) ||
        !str_field_ok(ntp->timezone, sizeof(ntp->timezone))) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    g_config.ntp = *ntp;
    mark_dirty_and_schedule();
    xSemaphoreGive(s_mutex);

    esp_event_post(CONFIG_EVENT, CONFIG_EVENT_NTP_CHANGED, NULL, 0, 0);
    return ESP_OK;
}

/* extra_host 的校验。规则比 RFC1123 更严，因为这个字符串会直接参与安全判定：
     · 只收 [A-Za-z0-9.-]，长度 0..63（空串 = 关闭这条放行）
     · 不能以 '-' 或 '.' 起止，不能出现 ".."
   ⚠️ 不能只拒空串。含空格、':'、或大小写以外任何字符的值，会让 Host 比对
      **永远不匹配**，而用户看到的现象是"我明明填了却还是 421"——
      一个静默且无从排查的故障。拒在写入时，用户当场就知道填错了。
   ⚠️ 端口不能填：守卫比对时已经把 Host 的 ":port" 切掉了，这里收了 ':'
      只会造出一个永不命中的值。 */
/* OTBR 专用 token。规则见 config_mgr.h 的字段注释。
   ⚠️ 只收 32 位**小写**十六进制或空串。放宽成"任意字符串"会立刻踩回那两个坑：
      非 ASCII 让 aiohttp 抛 UnicodeEncodeError、`/` 让 yarl 抛 ValueError，
      两者都发生在 HA 侧，设备上看不到任何请求 —— 用户只会看到"连不上"。
   ⚠️ 立即落盘：这是安全设置，且用户多半正拿着它去配 HA。 */
esp_err_t config_mgr_set_otbr_token(const char *t)
{
    if (!t) return ESP_ERR_INVALID_ARG;
    size_t n = strlen(t);
    if (n != 0 && n != 32) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < n; i++) {
        char c = t[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy(g_config.thread.otbr_token, t, sizeof(g_config.thread.otbr_token));
    xSemaphoreGive(s_mutex);
    /* 不打 token 本身。它是凭据，日志会被 /api/v1/logs 读走。 */
    ESP_LOGW(TAG, "OTBR token %s", n ? "已更新" : "已清除");
    return config_mgr_save_now();
}

esp_err_t config_mgr_set_extra_host(const char *h)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    size_t n = strlen(h);
    if (n >= sizeof(g_config.sys.extra_host)) return ESP_ERR_INVALID_ARG;
    if (n > 0) {
        if (h[0] == '-' || h[0] == '.' || h[n - 1] == '-' || h[n - 1] == '.')
            return ESP_ERR_INVALID_ARG;
        for (size_t i = 0; i < n; i++) {
            char c = h[i];
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '.';
            if (!ok) return ESP_ERR_INVALID_ARG;
            if (c == '.' && h[i + 1] == '.') return ESP_ERR_INVALID_ARG;
        }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy(g_config.sys.extra_host, h, sizeof(g_config.sys.extra_host));
    xSemaphoreGive(s_mutex);

    /* 立即落盘而不是走 2 秒防抖：这是安全设置，且用户填它多半正是因为**当前
       正被守卫挡在门外**。若在防抖窗口内断电，回来还是进不去，而界面已经
       回了 200 说"已保存"。 */
    ESP_LOGW(TAG, "跨站守卫额外放行 Host: \"%s\"", h[0] ? h : "(已清空)");
    return config_mgr_save_now();
}

/* 主机名校验（2026-09-11 T3.2 补）。
 *
 * ⚠️ 此前这里**只查了非空** —— 而这个串会直接进 mDNS 广播和 JSON 回包。
 *    带空格或点的名字会让 mDNS 记录变成畸形，带引号的会破坏 /status 的 JSON。
 *    校验放在 setter 里而不是 REST 处理器里，是为了让**配置导入**那条路径
 *    也受同一套规则管（extra_host 就吃过"API 拒掉的值可以从备份里绕进来"的亏）。
 *
 * 规则按 RFC 1123 的单个标签：字母数字和连字符，不能以连字符开头或结尾。
 * **不允许点**：这是主机名（bitengine），不是 FQDN；mDNS 会自己补 .local。
 */
static bool hostname_valid(const char *h)
{
    if (!h) return false;
    size_t n = strlen(h);
    if (n == 0 || n >= HOSTNAME_MAX_LEN) return false;
    if (n > 63) return false;                      /* RFC 1123 单标签上限 */
    if (h[0] == '-' || h[n - 1] == '-') return false;
    for (size_t i = 0; i < n; i++) {
        char c = h[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
    }
    return true;
}

esp_err_t config_mgr_set_hostname(const char *hostname)
{
    if (!hostname_valid(hostname)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy(g_config.sys.hostname, hostname, sizeof(g_config.sys.hostname));
    mark_dirty_and_schedule();
    xSemaphoreGive(s_mutex);

    esp_event_post(CONFIG_EVENT, CONFIG_EVENT_HOSTNAME_CHANGED, NULL, 0, 0);
    return ESP_OK;
}

esp_err_t config_mgr_set_radio_region(const char *cc)
{
    /* 先写失败路径：NULL、长度不是 0/2、不在菜单里，一律拒 */
    if (!cc) return ESP_ERR_INVALID_ARG;
    size_t n = strlen(cc);
    if (n != 0 && n != RADIO_REGION_CODE_LEN) return ESP_ERR_INVALID_ARG;
    char norm[RADIO_REGION_CODE_LEN + 1];
    strlcpy(norm, cc, sizeof(norm));
    radio_region_normalize(norm);
    if (!radio_region_code_valid(norm)) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = strcmp(g_config.sys.radio_region, norm) != 0;
    strlcpy(g_config.sys.radio_region, norm, sizeof(g_config.sys.radio_region));
    if (changed) mark_dirty_and_schedule();
    xSemaphoreGive(s_mutex);

    /* 值没变也发事件：用户点"保存并应用"就是要它重新下发一次 */
    esp_event_post(CONFIG_EVENT, CONFIG_EVENT_RADIO_REGION_CHANGED, NULL, 0, 0);
    return ESP_OK;
}

esp_err_t config_mgr_set_bridge_mode(uint8_t mode)
{
    if (mode > 2) return ESP_ERR_INVALID_ARG;  /* LAN=0, USB=1, DISABLED=2 */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    g_config.sys.bridge_mode = mode;
    mark_dirty_and_schedule();
    xSemaphoreGive(s_mutex);

    esp_event_post(CONFIG_EVENT, CONFIG_EVENT_BRIDGE_MODE_CHANGED, NULL, 0, 0);
    return ESP_OK;
}


esp_err_t config_mgr_set_password(const char *new_password)
{
    if (!new_password || strlen(new_password) < 4) return ESP_ERR_INVALID_ARG;
    if (!utf8_valid(new_password)) return ESP_ERR_INVALID_ARG;   /* T9.33 */

    char hash[PWD_HASH_LEN];
    sha256_hex(new_password, hash);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy(g_config.sys.password_hash, hash, sizeof(g_config.sys.password_hash));
    xSemaphoreGive(s_mutex);

    // Password: save immediately, no debounce
    config_mgr_save_now();

    ESP_LOGI(TAG, "Password updated (immediate persist)");
    return ESP_OK;
}

esp_err_t config_mgr_set_whitelist(const tcp_whitelist_config_t *wl)
{
    if (!wl) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    g_config.whitelist = *wl;
    if (g_config.whitelist.count > MAX_WHITELIST_IPS)
        g_config.whitelist.count = MAX_WHITELIST_IPS;
    mark_dirty_and_schedule();
    xSemaphoreGive(s_mutex);

    esp_event_post(CONFIG_EVENT, CONFIG_EVENT_WHITELIST_CHANGED, NULL, 0, 0);
    return ESP_OK;
}

// ============================================================
// Public API: Persist / Reset
// ============================================================

esp_err_t config_mgr_save_now(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    xTimerStop(s_debounce_timer, 0);
    esp_err_t err = config_save_to_nvs();
    s_dirty = false;
    xSemaphoreGive(s_mutex);

    if (err != ESP_OK) {
        nvs_health_record();
        esp_event_post(CONFIG_EVENT, CONFIG_EVENT_NVS_ERROR, NULL, 0, 0);
    }
    return err;
}

esp_err_t config_mgr_flush(uint32_t timeout_ms)
{
    if (s_factory_resetting || !s_dirty || !s_mutex) return ESP_OK;   /* 无锁先看一眼：没改动就别抢锁 */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "重启前落盘：配置锁 %u ms 没拿到，放弃（这次的改动可能丢）", (unsigned)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ESP_OK;
    if (s_dirty && !s_factory_resetting) {
        xTimerStop(s_debounce_timer, 0);
        err = config_save_to_nvs();
        if (err == ESP_OK) {
            s_dirty = false;
            ESP_LOGI(TAG, "重启前把防抖中的配置写进了 NVS");
        } else {
            nvs_health_record();
            ESP_LOGE(TAG, "重启前落盘失败：%s", esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s_mutex);
    return err;
}

void config_mgr_get_nvs_health(config_nvs_health_t *out)
{
    if (!out) return;
    out->count       = s_nvs_fail_count;
    out->last_uptime = s_nvs_fail_uptime;
}

/* 恢复出厂**只擦我们自己拥有的命名空间**，白名单式列举；没列到的一律不动。
 *
 * ⚠️ 这里原本是反过来的——列一份"要保留的"，其余全擦。2026-08-15 那次事故就是
 * 这么来的：OpenThread 的网络密钥材料存在 IDF 的 PSA 密钥库里，也就是 nvs 分区
 * 的 "psa_its" 命名空间（mbedtls/port/psa_crypto_storage/esp_psa_its.c），不在
 * 保留名单里，被擦掉了。结果是数据集、ChildInfo、had_child 全都完好，ExtPANID /
 * 网络名 / 信道逐项对得上，唯独密钥没了 —— 子设备拿真密钥来敲门，网关一律回
 * "Failed to process UDP - Security"，整个 Thread 网络永久失联，而所有检查项
 * 都是绿的。
 *
 * 教训不是"再补一个名字"，而是**极性反了**：擦除的默认值必须是"不擦"。我们数得
 * 清自己写了哪些命名空间，数不清 IDF 和各组件在同一个分区里还放了什么。列举我们
 * 自己的，漏掉的后果是"某项配置没被恢复出厂"（可发现、可修）；反过来漏掉的后果
 * 是"平台状态被销毁"（不可逆，且伪装成成功）。
 *
 * 新增一个存配置的命名空间时，必须同时加到这里，否则它不会被恢复出厂清掉。
 * components/config_mgr/VERIFY.sh 里有一条 check 会比对 config_save_to_nvs()
 * 里出现的命名空间与本表，漏了会红。 */
static const char *const OWNED_NS[] = {
    /* config_mgr 自己的 10 个 —— 与 config_save_to_nvs() 里的 NVS_WRITE_NS 一一对应 */
    "sys", "wifi", "eth", "zigbee", "thread", "mqtt", "webhook",
    "led", "whitelist", "ntp",
    /* WiFi 驱动自己存的 AP 记录。不擦它的话，恢复出厂后设备会自动连回原来的
       AP —— 用户以为清干净了，其实还在原来的网里 */
    "nvs.net80211",
    /* 看门狗取证与安全模式标志。恢复出厂就该从干净的记录开始 */
    "wd",
    /* network_mgr 的以太网服务 IP 快照（T9.20 开机接管用）。不擦的话，恢复出厂后换到别的网络，
       开机还会拿旧网络的地址去判 —— 虽有 SSID / 网段 / 租期三道拦着，没理由留着。
       ⚠️ T9.37 的服务身份记录**不在这里**、也**刻意不擦**：它在 network_mgr 的 "net_id"（10-07 用户拍板）。擦了的话，
       先用 WiFi 装、后插网线的用户恢复出厂后设备地址会被定成网口的，HA 里记的地址失效；记录按 WiFi 网络认，
       配回同一个 WiFi 时沿用。VERIFY.sh 钉着 net_id 不许进这张表 */
    "net_svc",
};

/* WIPE_ALL 才擦的：Thread 网络的三件套。缺一不可——
     openthread —— 数据集、ChildInfo、NetworkInfo
     psa_its    —— 网络密钥的**实际材料**（数据集里只有引用）
     thread_br  —— had_child 标记，自动改信道的闸门
   ⚠️ psa_its 里可能还有 Matter/TLS 的密钥，所以它只在 WIPE_ALL 里被擦。 */
static const char *const THREAD_NS[] = {
    "openthread", "psa_its", "thread_br",
};


/* 按名字逐个擦，不遍历分区。遍历+反向名单是上一版的做法，正是它擦掉了 psa_its。
   现在连"分区里还有什么"都不需要知道：没列举的一律不碰。 */
static esp_err_t erase_one_ns(const char *name, int *erased)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(name, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;   /* 本就不存在，不算错 */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open '%s' failed: %s", name, esp_err_to_name(err));
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase '%s' failed: %s", name, esp_err_to_name(err));
        return err;
    }
    (*erased)++;
    return ESP_OK;
}

static esp_err_t erase_user_namespaces(factory_reset_scope_t scope)
{
    int erased = 0;
    esp_err_t first_err = ESP_OK;

    for (size_t i = 0; i < sizeof(OWNED_NS) / sizeof(OWNED_NS[0]); i++) {
        esp_err_t e = erase_one_ns(OWNED_NS[i], &erased);
        if (e != ESP_OK && first_err == ESP_OK) first_err = e;
    }
    if (scope == FACTORY_RESET_WIPE_ALL) {
        for (size_t i = 0; i < sizeof(THREAD_NS) / sizeof(THREAD_NS[0]); i++) {
            esp_err_t e = erase_one_ns(THREAD_NS[i], &erased);
            if (e != ESP_OK && first_err == ESP_OK) first_err = e;
        }
    }

    ESP_LOGW(TAG, "Erased %d namespace(s), scope=%s", erased,
             scope == FACTORY_RESET_KEEP_THREAD ? "keep-thread" : "wipe-all");
    return first_err;
}

#if CONFIG_BITENGINE_PROFILE_DIRECT
/* 弱定义：hub_core 没链接进来时用（正常的直连型构建里 hub_store.c 的强定义覆盖它）。见 config_mgr.h。 */
__attribute__((weak)) esp_err_t config_mgr_hubdb_factory_reset(bool wipe_all)
{
    (void)wipe_all;
    ESP_LOGE(TAG, "hubdb 的恢复出厂钩子没链接进来 —— Muse 凭据没清");
    return ESP_ERR_NOT_SUPPORTED;
}
#endif

esp_err_t config_mgr_factory_reset(factory_reset_scope_t scope)
{
    ESP_LOGW(TAG, "Factory reset initiated (scope=%s)",
             scope == FACTORY_RESET_KEEP_THREAD ? "keep-thread" : "wipe-all");

    /* Cancel any pending debounce write FIRST: if it fired between the NVS
       erase and the restart it would write the in-memory config straight
       back, silently undoing the reset. */
    s_factory_resetting = true;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    xTimerStop(s_debounce_timer, portMAX_DELAY);
    s_dirty = false;
    xSemaphoreGive(s_mutex);

    esp_event_post(CONFIG_EVENT, CONFIG_EVENT_FACTORY_RESET, NULL, 0, 0);

    esp_err_t err = erase_user_namespaces(scope);
#if CONFIG_BITENGINE_PROFILE_DIRECT
    /* 直连型：hubdb 里的 Muse 设置与凭据（两种都清）、设备元数据（全清时清）—— 设计第十二节 */
    esp_err_t herr = config_mgr_hubdb_factory_reset(scope == FACTORY_RESET_WIPE_ALL);
    if (err == ESP_OK) err = herr;
#endif
    if (err != ESP_OK) {
        /* 有命名空间擦不掉就如实报，**不再退回整分区擦**。那条兜底看着稳妥，实际
           是把"某项配置没清干净"（可发现、可重试）换成"平台状态被销毁"（不可逆，
           且伪装成成功）——2026-08-15 丢掉 Thread 网络密钥就是这个量级的代价。
           NVS 真的损坏到擦不动，是 wd_recovery 的职责，不该由恢复出厂顺手代劳。 */
        ESP_LOGE(TAG, "Factory reset incomplete: %s — some settings may survive",
                 esp_err_to_name(err));
    }

    /* 这是本函数唯一一次真实执行，也是调用方任务栈上最深的一条路径——把余量打
       出来，栈大小才是量出来的而不是猜的。按键任务栈很小（内部 RAM 紧张），这
       个数字接近 0 就说明它不够。 */
    ESP_LOGW(TAG, "Stack headroom on reset path: %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    /* Still restart even if the erase failed: callers (WebUI/button) have
       already told the user the device is resetting — a reboot with old
       config is better than silently continuing in a half-announced state. */
    ESP_LOGW(TAG, "Restarting in 2s...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;  // unreachable
}

// ============================================================
// Public API: Password
// ============================================================

bool config_mgr_verify_password(const char *password)
{
    if (g_config.sys.password_hash[0] == '\0') return true;  // no password set
    if (!password) return false;

    char hash[PWD_HASH_LEN];
    sha256_hex(password, hash);
    // Constant-time comparison to prevent timing attacks
    volatile int diff = 0;
    for (int i = 0; i < 64; i++) {
        diff |= hash[i] ^ g_config.sys.password_hash[i];
    }
    return diff == 0;
}

bool config_mgr_has_password(void)
{
    return (g_config.sys.password_hash[0] != '\0');
}

/* config_mgr_clear_password() 已删除（2026-08-16）。
 * 按键双击清密码于 2026-08-14 撤销（物理接触即拿到整机管理权且完全无痕），
 * 此后它零调用者，却是唯一能清空 password_hash 且**不**触发恢复出厂的入口 ——
 * 留在公共头文件上等于把那条被否决的攻击面摆着，重新接上不会有人察觉。
 * 忘记密码的唯一出路是恢复出厂，见 docs/00「撤销密码：知道密码时可以，忘了就不行」。 */
