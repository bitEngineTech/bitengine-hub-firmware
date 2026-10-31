#include "bitengine_mqtt.h"
#include "config_mgr.h"
#include "led_ctrl.h"    /* NET 灯的"有流量"白闪 */
#include "watchdog.h"
#include "esp_timer.h"
#include "network_mgr.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdatomic.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include <net/if.h>
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "psram_task.h"

static int s_wd_slot_mqtt_pub = -1;

static const char *TAG = "bitengine_mqtt";

static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_connected = false;
static volatile bool s_restart_requested = false;
/* T9.21（2026-10-08）：收到 NETWORK_EVENT_ONLINE 时置位，mqtt_task 比较"现在的活动网口"与客户端绑定的网口，
   不一样才重建。ONLINE 不是告警事件（开机上线、或 WiFi 先上线后网线才到 —— T9.37 起这时也会发 ONLINE(ETH)），
   不补发任何 MQTT 事件消息，只管别让客户端一直绑在后来不是活动口的那张卡上。 */
static volatile bool s_iface_check_requested = false;
static char s_bound_if[8];          /* 客户端建立时绑的网口名（空 = 没绑）；只在 mqtt_task 里读写 */
/* T2.19：网络切换事件的"待发"位。FAILOVER/RECOVERED 发出的那一刻 MQTT 必然正在
   断线/重连（切换本身就把它的 socket 弄死了；RECOVERED 更是在拿到 IP 后 2.5s 就发、
   而重连要 6-8s），直接 publish 等于必丢 —— 2026-09-13 真板三次拔线一次都没收到
   network_recovered。所以事件回调只置位，MQTT_EVENT_CONNECTED 里补发，至少送达一次。
   写方是事件循环 task，读方是 esp-mqtt 的 task，用原子 fetch_or / exchange。 */
#define PENDING_NET_FAILOVER   (1u << 0)
#define PENDING_NET_RECOVERED  (1u << 1)
static atomic_uint_fast8_t s_pending_net_events;
static bool s_task_running = false;
static bool s_events_registered = false;
static char s_prefix[64];
/* Guards s_client lifetime against publishers on other tasks (BLE proxy,
   event loop) racing the stop/destroy path in mqtt_task. */
static SemaphoreHandle_t s_client_lock = NULL;

/* hostname comes from user config: restrict to MQTT-topic- and JSON-safe
   characters before embedding it in topics and discovery payloads */
static void sanitize_hostname(char *dst, const char *src, size_t dst_size)
{
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 1 < dst_size; i++) {
        char c = src[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_') {
            dst[j++] = c;
        }
    }
    if (j == 0) { strlcpy(dst, "bitengine", dst_size); return; }
    dst[j] = '\0';
}

// ---- HA Discovery ----

static void publish_ha_discovery(void)
{
    led_ctrl_notify_network_activity();   /* NET 灯：HA 自动发现（6 条 retained） */
    if (!s_client || !s_connected) return;

    const sys_config_t *sys = config_mgr_get_sys();
    const esp_app_desc_t *app = esp_app_get_description();

    char host[40];
    sanitize_hostname(host, sys->hostname, sizeof(host));

    char device[256];
    snprintf(device, sizeof(device),
        "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"bitEngine\","
        "\"mdl\":\"bitEngine-Hub-C5\",\"sw\":\"%s\"}",
        host, host, app->version);

    struct { const char *comp; const char *id; const char *name;
             const char *cls; const char *unit; const char *icon; } entities[] = {
        { "sensor", "temperature", "Temperature", "temperature", "°C", NULL },
        { "sensor", "free_memory", "Free Memory", NULL, "KB", "mdi:memory" },
#if !CONFIG_BITENGINE_NET_ETH_ONLY
        /* 纯以太网剖面不报（docs/15 设计 2.8 A-21）：没有 Wi-Fi，报出来就是一个永远 0 dBm 的实体 */
        { "sensor", "wifi_rssi", "WiFi RSSI", "signal_strength", "dBm", NULL },
#endif
        { "sensor", "uptime", "Uptime", "duration", "s", NULL },
        { "binary_sensor", "mg24_alive", "Zigbee Chip", "connectivity", NULL, NULL },
        { "sensor", "fw_version", "Firmware Version", NULL, NULL, "mdi:package-up" },
    };

    for (size_t i = 0; i < sizeof(entities) / sizeof(entities[0]); i++) {
        char topic[128], payload[512];
        snprintf(topic, sizeof(topic), "homeassistant/%s/%s/%s/config",
                 entities[i].comp, host, entities[i].id);

        int n = snprintf(payload, sizeof(payload),
            "{\"name\":\"%s\",\"stat_t\":\"%s/sensor/%s\","
            "\"uniq_id\":\"%s_%s\",%s",
            entities[i].name, s_prefix, entities[i].id,
            host, entities[i].id, device);

        if (entities[i].cls)
            n += snprintf(payload + n, sizeof(payload) - n,
                          ",\"dev_cla\":\"%s\"", entities[i].cls);
        if (entities[i].unit)
            n += snprintf(payload + n, sizeof(payload) - n,
                          ",\"unit_of_meas\":\"%s\"", entities[i].unit);
        if (entities[i].icon)
            n += snprintf(payload + n, sizeof(payload) - n,
                          ",\"ic\":\"%s\"", entities[i].icon);

        strlcat(payload, "}", sizeof(payload));
        esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);
    }
    ESP_LOGI(TAG, "HA discovery published (6 entities)");
}

// ---- Periodic status publish ----

static void publish_status(void)
{
    led_ctrl_notify_network_activity();   /* NET 灯：周期状态上报 */
    if (!s_client || !s_connected) return;

    const health_status_t *hs = watchdog_get_status();
    const esp_app_desc_t *app = esp_app_get_description();
    char topic[96], payload[64];

    snprintf(topic, sizeof(topic), "%s/sensor/temperature", s_prefix);
    snprintf(payload, sizeof(payload), "%.1f", hs ? hs->temperature : 0.0f);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 0);

    snprintf(topic, sizeof(topic), "%s/sensor/free_memory", s_prefix);
    snprintf(payload, sizeof(payload), "%"PRIu32, hs ? hs->free_heap / 1024 : 0);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 0);

#if !CONFIG_BITENGINE_NET_ETH_ONLY
    snprintf(topic, sizeof(topic), "%s/sensor/wifi_rssi", s_prefix);
    snprintf(payload, sizeof(payload), "%d", hs ? (int)hs->wifi_rssi : 0);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 0);
#endif

    snprintf(topic, sizeof(topic), "%s/sensor/uptime", s_prefix);
    snprintf(payload, sizeof(payload), "%"PRIu32, hs ? hs->uptime_seconds : 0);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 0);

    snprintf(topic, sizeof(topic), "%s/sensor/mg24_alive", s_prefix);
    snprintf(payload, sizeof(payload), "%s", (hs && hs->mg24_alive) ? "ON" : "OFF");
    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 0);

    snprintf(topic, sizeof(topic), "%s/sensor/fw_version", s_prefix);
    esp_mqtt_client_publish(s_client, topic, app->version, 0, 0, 1);
}

/* T2.19：在 esp-mqtt 自己的 task 里（CONNECTED 回调）补发切换事件。
   先取走再发：发之前先把位清零，发的过程中又来了新事件也不会丢。
   QoS 1：这两条是"至少送达一次"的语义，和 <prefix>/status 一个档次。
   顺序固定 failover → recovered：短暂拔线时两位可能同时挂着，按时序发。 */
static void flush_pending_net_events(void)
{
    uint_fast8_t bits = atomic_exchange(&s_pending_net_events, 0);
    if (!bits || !s_client) return;
    char topic[96];
    if (bits & PENDING_NET_FAILOVER) {
        led_ctrl_notify_network_activity();
        snprintf(topic, sizeof(topic), "%s/event/network_failover", s_prefix);
        esp_mqtt_client_publish(s_client, topic, "{\"event\":\"failover\"}", 0, 1, 0);
    }
    if (bits & PENDING_NET_RECOVERED) {
        led_ctrl_notify_network_activity();
        snprintf(topic, sizeof(topic), "%s/event/network_recovered", s_prefix);
        esp_mqtt_client_publish(s_client, topic, "{\"event\":\"recovered\"}", 0, 1, 0);
    }
}

// ---- MQTT event handler ----

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *data)
{
    /* 只认当前那个客户端的事件（2026-10-08 T9.21 上板看到）：重建时 mqtt_stop_client 先把 s_client 置空再
       stop / destroy，被拆的旧客户端可能恰好在这几秒里连上，它的 CONNECTED 晚到 —— 不挡的话 s_connected 被
       置成 true、还拿空句柄去发布（"Client was not initialized"），新客户端连上之前 mqtt_task 就开始发状态、全丢 */
    const esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
    if (!ev || ev->client != s_client) return;
    switch (event_id) {
    case MQTT_EVENT_CONNECTED: {
        s_connected = true;
        ESP_LOGI(TAG, "MQTT connected to broker");
        char topic[96];
        snprintf(topic, sizeof(topic), "%s/status", s_prefix);
        esp_mqtt_client_publish(s_client, topic, "online", 0, 1, 1);
        publish_ha_discovery();
        flush_pending_net_events();
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT error");
        break;
    default:
        break;
    }
}

// ---- System event handler ----

static void system_event_handler(void *arg, esp_event_base_t base,
                                 int32_t id, void *data)
{
    if (base == CONFIG_EVENT && id == CONFIG_EVENT_MQTT_CHANGED) {
        ESP_LOGI(TAG, "MQTT config changed, requesting restart");
        s_restart_requested = true;  // handled in mqtt_task, NOT here (avoid deadlock)
        return;
    }

    char topic[96];
    /* v2：把其余 8 个看门狗事件也发出去。之前只订阅 MG24_RESET，
       HEAP_CRITICAL / ABNORMAL_BOOT 这类最该让用户知道的反而无人接收。 */
    if (base == WATCHDOG_EVENT && id != WATCHDOG_EVENT_MG24_RESET) {
        static const char *names[] = {
            "mg24_reset", "heap_low", "heap_critical", "heap_leak",
            "temp_high", "temp_critical", "abnormal_boot",
            "wifi_rssi_low", "uart_error_rate_high",
        };
        if (id >= 0 && id < (int32_t)(sizeof(names)/sizeof(names[0]))) {
            char payload[96 + WD_ABNORMAL_CAUSE_LEN];
            const wd_abnormal_boot_t *ev = (id == WATCHDOG_EVENT_ABNORMAL_BOOT) ? data : NULL;
            /* 异常开机带上原因（T9.30 ④）："panic" / "task_wdt" / "watchdog:subsys/thread" …
               cause 只含 [a-z0-9_:/.-]，见 wd_boot_reason.c */
            snprintf(payload, sizeof(payload),
                     "{\"event\":\"%s\",\"uptime\":%u%s%.*s%s}", names[id],
                     (unsigned)(esp_timer_get_time() / 1000000),
                     ev ? ",\"reason\":\"" : "",
                     ev ? (int)strnlen(ev->cause, sizeof(ev->cause)) : 0, ev ? ev->cause : "",
                     ev ? "\"" : "");
            /* 主题前缀此前写死成 "bitengine"（T3.8）—— 用户改了 topic_prefix 之后，
               别的主题都跟着走，只有这一条还发到老前缀下，订阅端收不到。 */
            char wd_topic[96];
            snprintf(wd_topic, sizeof(wd_topic), "%s/event/watchdog", s_prefix);
            mqtt_client_publish(wd_topic, payload);
        }
        return;
    }

    if (base == WATCHDOG_EVENT && id == WATCHDOG_EVENT_MG24_RESET) {
        snprintf(topic, sizeof(topic), "%s/event/zigbee_restart", s_prefix);
        mqtt_client_publish(topic, "{\"event\":\"mg24_reset\"}");
    } else if (base == NETWORK_EVENT &&
               (id == NETWORK_EVENT_FAILOVER || id == NETWORK_EVENT_RECOVERED)) {
        /* FAILOVER / RECOVERED 的语义见 network_mgr.h（T2.16；T9.20 起"退过"含开机接管成立）。
           T2.19：客户端是绑在建立时的活动网口上的（T3.8 的 network.if_name），
           切换后它只会在那张已经没链路的口上重连、直到以太网回来 —— 2026-09-13 拔线
           3 分 40 秒，broker 全程没见过 WiFi 地址，超过 keepalive 后发了 LWT offline。
           所以两个方向都要按新的活动网口重建客户端：这里只置标志，真正的 stop/start
           在 mqtt_task 里做（事件循环里 stop/destroy 会自己等自己，见 rest_api.c 那条注释）。
           network_mgr 是先更新 s_active_if 再 post 的，重建时读到的就是新网口。
           事件本身进"待发"位，重连成功后由 flush_pending_net_events() 补发。 */
        atomic_fetch_or(&s_pending_net_events,
                        id == NETWORK_EVENT_FAILOVER ? PENDING_NET_FAILOVER
                                                     : PENDING_NET_RECOVERED);
        ESP_LOGI(TAG, "Network %s: rebinding MQTT client to the new active interface",
                 id == NETWORK_EVENT_FAILOVER ? "failover" : "recovered");
        s_restart_requested = true;
    } else if (base == NETWORK_EVENT && id == NETWORK_EVENT_ONLINE) {
        s_iface_check_requested = true;   /* 只置位：比较网口在 mqtt_task 里做（不在事件循环里调网络 API） */
    }
}

// ---- Internal: create/destroy MQTT client (NOT the task) ----

static esp_err_t mqtt_start_client(void)
{
    const mqtt_config_t *cfg = config_mgr_get_mqtt();
    if (!cfg->enabled) return ESP_OK;

    strlcpy(s_prefix, cfg->topic_prefix, sizeof(s_prefix));
    if (strlen(s_prefix) == 0) strlcpy(s_prefix, "bitengine", sizeof(s_prefix));

    char uri[280];
    snprintf(uri, sizeof(uri), "%s://%s:%d",
             cfg->tls_enabled ? "mqtts" : "mqtt",
             cfg->broker, cfg->port);

    char lwt_topic[96];
    snprintf(lwt_topic, sizeof(lwt_topic), "%s/status", s_prefix);

    /* 绑定出口网卡（T3.8）。不指定时 lwip 对**本网段**目的地按 netif 链表顺序选
       出口，**不看** esp_netif_set_default_netif 设的默认口 —— 双上行且两口同网段
       时会从 WiFi 地址发出，哪怕 active_interface=eth。broker 上按来源 IP 做 ACL
       的用户放行了以太网地址却连不上。
       broker 绝大多数在局域网，所以这里值得绑；云端 OTA 反而**不绑**（目的地在
       公网、跨网段本就按默认路由走，绑死还会挡掉"这个口没网另一个有网"的回退）。
       ⚠️ ifr 必须活到 esp_mqtt_client_init() 之后 —— 配置里存的是指针。
       取不到就不填，退回原来的行为。 */
    static struct ifreq s_ifr;      /* 静态：client 生命周期内都要有效 */
    memset(&s_ifr, 0, sizeof(s_ifr));
    char ifname[8] = {0};
    network_mgr_active_ifname(ifname, sizeof(ifname));
    if (ifname[0]) strlcpy(s_ifr.ifr_name, ifname, sizeof(s_ifr.ifr_name));
    strlcpy(s_bound_if, ifname, sizeof(s_bound_if));

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .network.if_name = ifname[0] ? &s_ifr : NULL,
        .credentials.username = cfg->username,
        .credentials.authentication.password = cfg->password,
        .session.last_will.topic = lwt_topic,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        /* 收发缓冲区故意 > CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL(1024)：esp-mqtt 用 malloc 分配它们，
           1024 正好落在"强制内部"那一档，两块共吃 2 KB 内部 SRAM；大于它 malloc 就去 PSRAM（T9.39）。
           只有 mqtt_task 与发布方访问，都不在 cache 关闭期间运行。别改回 1024 或更小。 */
        .buffer.size = 1536,
        .buffer.out_size = 1536,
    };
    if (cfg->tls_enabled) {
        /* Without a trust anchor every mqtts:// handshake fails — use the
           ESP x509 bundle for server verification. */
        mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "MQTT client init failed");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);

    ESP_LOGI(TAG, "MQTT client started: %s prefix=%s", uri, s_prefix);
    return ESP_OK;
}

static void mqtt_stop_client(void)
{
    /* Take the client lock so a publisher mid-call on another task can't
       use the handle after destroy (use-after-free). */
    if (s_client_lock) xSemaphoreTake(s_client_lock, portMAX_DELAY);
    if (s_client) {
        esp_mqtt_client_handle_t c = s_client;
        s_client = NULL;
        s_connected = false;
        esp_mqtt_client_stop(c);
        esp_mqtt_client_destroy(c);
    }
    if (s_client_lock) xSemaphoreGive(s_client_lock);
}

// ---- Generic publish (used by BLE Proxy and other modules) ----

void mqtt_client_publish(const char *topic, const char *payload)
{
    led_ctrl_notify_network_activity();   /* NET 灯：对外发布 API */
    if (!topic || !payload || !s_client_lock) return;
    xSemaphoreTake(s_client_lock, portMAX_DELAY);
    if (s_client && s_connected) {
        // Use enqueue (non-blocking) instead of publish (blocking).
        // Critical: BLE Proxy callback runs in NimBLE host task — blocking
        // would stall BLE event processing and could deadlock
        // nimble_port_stop().
        esp_mqtt_client_enqueue(s_client, topic, payload, 0, 0, 0, true);
    }
    xSemaphoreGive(s_client_lock);
}

// ---- Periodic publish task (created ONCE, never exits) ----

/* T9.38（2026-10-08）：开机时不立刻建客户端。esp-mqtt 的 mqtt_task 栈 6 KB 只能在内部内存（库里是普通 xTaskCreate），
   开机第 10 s 前后正赶上网络 / Thread 边界路由初始化，两者叠在一起，DMA 池同一时刻的真实最低只剩 2247 字节
   （诊断堆追踪器开机窗口实测；关着 MQTT 同一位置是 5859，硬停判据 6 要 ≥ 3072）。推迟到开机后这么多秒再建：
   开机的临时占用已经还回去，网线若插着也已经是活动口（顺带省掉一次 T9.21 的改绑）。 */
#define MQTT_BOOT_DEFER_S  30

static void mqtt_task(void *arg)
{
    /* 开机推迟（见 MQTT_BOOT_DEFER_S）。期间配置改了（s_restart_requested）就不等了，交给下面的重建路径 */
    for (;;) {
        wd_kick(s_wd_slot_mqtt_pub);
        int64_t left_s = MQTT_BOOT_DEFER_S - esp_timer_get_time() / 1000000;
        if (left_s <= 0 || s_restart_requested) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!s_restart_requested && config_mgr_get_mqtt()->enabled) {
        ESP_LOGI(TAG, "MQTT starting (deferred to %d s after boot)", MQTT_BOOT_DEFER_S);
        mqtt_start_client();
    }

    while (1) {
        wd_kick(s_wd_slot_mqtt_pub);
        if (s_iface_check_requested) {
            s_iface_check_requested = false;
            char now[8] = {0};
            network_mgr_active_ifname(now, sizeof(now));
            if (s_client && now[0] && strcmp(now, s_bound_if) != 0) {
                ESP_LOGI(TAG, "Active interface %s -> %s (online event): rebinding MQTT client",
                         s_bound_if[0] ? s_bound_if : "-", now);
                s_restart_requested = true;
            }
        }
        // Handle restart request (from config change event)
        if (s_restart_requested) {
            s_restart_requested = false;
            ESP_LOGI(TAG, "MQTT restarting (config change or interface switch)");
            mqtt_stop_client();
            vTaskDelay(pdMS_TO_TICKS(500));
            mqtt_start_client();
        }

        const mqtt_config_t *cfg = config_mgr_get_mqtt();
        if (!cfg->enabled) {
            if (s_client) mqtt_stop_client();
            /* 关着时切换过的网络事件不留到日后启用时补发 —— 那会是几小时前的旧闻 */
            atomic_store(&s_pending_net_events, 0);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        if (s_connected) {
            publish_status();
        }

        /* Sleep in 1s slices so a config-change restart takes effect within
           ~1s instead of waiting out the whole publish interval (which can
           be hours). */
        uint32_t interval = cfg->publish_interval > 0 ? cfg->publish_interval : 30;
        for (uint32_t s = 0; s < interval && !s_restart_requested && !s_iface_check_requested; s++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

// ---- Public API ----

esp_err_t mqtt_client_init(void)
{
    if (!s_client_lock) {
        s_client_lock = xSemaphoreCreateMutex();
        if (!s_client_lock) return ESP_ERR_NO_MEM;
    }

    // Register event handlers ONCE
    if (!s_events_registered) {
        esp_event_handler_instance_register(WATCHDOG_EVENT, ESP_EVENT_ANY_ID,
                                            system_event_handler, NULL, NULL);
        esp_event_handler_instance_register(NETWORK_EVENT, NETWORK_EVENT_FAILOVER,
                                            system_event_handler, NULL, NULL);
        /* T2.16：RECOVERED 是单独的事件 id，这里按 id 订阅，不加这一行 handler 永远
           收不到 —— 与 2026-09-13 ota_complete "先 post 后订阅"是同一类：有分支、没订阅者 */
        esp_event_handler_instance_register(NETWORK_EVENT, NETWORK_EVENT_RECOVERED,
                                            system_event_handler, NULL, NULL);
        esp_event_handler_instance_register(NETWORK_EVENT, NETWORK_EVENT_ONLINE,   /* T9.21 */
                                            system_event_handler, NULL, NULL);
        esp_event_handler_instance_register(CONFIG_EVENT, CONFIG_EVENT_MQTT_CHANGED,
                                            system_event_handler, NULL, NULL);
        s_events_registered = true;
    }

    // Create task ONCE
    if (!s_task_running) {
    static const wd_task_spec_t wd_spec = {
        .name = "mqtt_pub", .deadline_ms = 120000, .severity = WD_SEV_RECOVER,
    };
    s_wd_slot_mqtt_pub = wd_register(&wd_spec);
        wd_register_recovery("mqtt", mqtt_client_restart);
        psram_task_create(mqtt_task, "mqtt_pub", 4096, NULL, 8, NULL);
        s_task_running = true;
    }

    /* 客户端由 mqtt_task 在开机 MQTT_BOOT_DEFER_S 秒后建（T9.38），这里不建 */
    if (config_mgr_get_mqtt()->enabled) ESP_LOGI(TAG, "MQTT enabled: client starts %d s after boot", MQTT_BOOT_DEFER_S);
    else                                ESP_LOGI(TAG, "MQTT disabled");
    return ESP_OK;
}

esp_err_t mqtt_client_stop(void)
{
    mqtt_stop_client();
    return ESP_OK;
}

esp_err_t mqtt_client_restart(void)
{
    s_restart_requested = true;
    return ESP_OK;
}

bool mqtt_client_is_connected(void) { return s_connected; }
