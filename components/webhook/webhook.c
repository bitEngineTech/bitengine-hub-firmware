#include "webhook.h"
#include "config_mgr.h"
#include "led_ctrl.h"    /* NET 灯的"有流量"白闪 */
#include "watchdog.h"
#include "network_mgr.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_event.h"
#include "psram_task.h"

// WEBUI_EVENT base declared externally (avoid circular dep with webui component)
ESP_EVENT_DECLARE_BASE(WEBUI_EVENT);
/* Keep in sync with webui_event_id_t in webui.h (cannot include it:
   webui -> spectrum -> webhook would become a dependency cycle)
   ⚠️ 这几个数字必须和那个 enum 的顺序一致。webui/VERIFY.sh 里有跨侧断言盯着。 */
#define WEBUI_EVENT_ID_OTA_COMPLETE  2
#define WEBUI_EVENT_ID_OTA_VERIFIED  4
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include <net/if.h>
#include "esp_heap_caps.h"
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static int s_wd_slot_webhook = -1;

static const char *TAG = "webhook";

#define WEBHOOK_TIMEOUT_MS  5000
#define WEBHOOK_NET_TRIES   3      /* 网络切换类通知的发送次数（见 webhook_task） */
#define WEBHOOK_NET_RETRY_MS 3000
#define WEBHOOK_QUEUE_DEPTH 8

typedef struct {
    webhook_event_t event;
    uint32_t        timestamp;
    /* true = timestamp 是开机秒数而不是 epoch（SNTP 还没同步）。
       必须让接收端能分辨，否则一个 42 会被当成 1970-01-01 00:00:42。 */
    bool            uptime_based;
    char            data[128];
} webhook_item_t;

static QueueHandle_t s_queue = NULL;

static const char *event_name(webhook_event_t e)
{
    switch (e) {
    case WEBHOOK_EVENT_ZIGBEE_CRASH:    return "zigbee_crash";
    case WEBHOOK_EVENT_NETWORK_DOWN:    return "network_down";
    case WEBHOOK_EVENT_NETWORK_FAILOVER:return "network_failover";
    case WEBHOOK_EVENT_TEMP_HIGH:       return "temp_high";
    case WEBHOOK_EVENT_MEMORY_LOW:      return "memory_low";
    case WEBHOOK_EVENT_CONFIG_BACKUP:   return "config_backup";
    case WEBHOOK_EVENT_INTERFERENCE:    return "interference";
    case WEBHOOK_EVENT_OTA_AVAILABLE:   return "ota_available";
    case WEBHOOK_EVENT_OTA_COMPLETE:    return "ota_complete";
    case WEBHOOK_EVENT_NETWORK_RECOVERED:return "network_recovered";
    default:                            return "unknown";
    }
}

// ---- System event → webhook ----
static void system_event_handler(void *arg, esp_event_base_t base,
                                 int32_t id, void *data)
{
    const webhook_config_t *cfg = config_mgr_get_webhook();
    if (!cfg->enabled) return;

    /* 每类事件的开关判断已收进 webhook_post_event() 的 is_event_enabled()，
       这里只做"内部事件 → 对外事件类型"的映射。两处都判会变成两份真源，
       而当初正是靠"每个 if 各判一次"才漏掉了绕开本处理器的那两条路。 */
    if (base == WATCHDOG_EVENT) {
        /* 三种内部事件都映射成 zigbee_crash（对外名字不改，接收端在用）：data.reason 区分是哪一种，
           异常开机的还带上真原因 —— 看门狗因 Thread 停摆重启也会落到这里，不只是 Zigbee（T9.30 ④） */
        if (id == WATCHDOG_EVENT_MG24_RESET)
            webhook_post_event(WEBHOOK_EVENT_ZIGBEE_CRASH, "{\"reason\":\"mg24_reset\"}");
        else if (id == WATCHDOG_EVENT_TEMP_HIGH)
            webhook_post_event(WEBHOOK_EVENT_TEMP_HIGH, NULL);
        else if (id == WATCHDOG_EVENT_HEAP_LOW)
            webhook_post_event(WEBHOOK_EVENT_MEMORY_LOW, NULL);
        /* v2：剩下 6 个事件此前发出来无人接收（设计文档第十节 10.3）。
           复用最贴近的 webhook 事件类型，避免为每个内部事件都新增一种对外
           类型——用户关心的是"内存/温度/Zigbee/网关本身出事了"这四类。 */
        else if (id == WATCHDOG_EVENT_HEAP_CRITICAL)
            webhook_post_event(WEBHOOK_EVENT_MEMORY_LOW, NULL);
        else if (id == WATCHDOG_EVENT_HEAP_LEAK_SUSPECTED)
            webhook_post_event(WEBHOOK_EVENT_MEMORY_LOW, NULL);
        else if (id == WATCHDOG_EVENT_TEMP_CRITICAL)
            webhook_post_event(WEBHOOK_EVENT_TEMP_HIGH, NULL);
        else if (id == WATCHDOG_EVENT_ABNORMAL_BOOT) {
            char json[24 + WD_ABNORMAL_CAUSE_LEN];
            const wd_abnormal_boot_t *ev = data;
            /* cause 由 wd_boot_reason.c 只放行 [a-z0-9_:/.-]，可直接拼；%.*s 再兜一道长度 */
            snprintf(json, sizeof(json), "{\"reason\":\"%.*s\"}",
                     ev ? (int)strnlen(ev->cause, sizeof(ev->cause)) : 7, ev ? ev->cause : "unknown");
            webhook_post_event(WEBHOOK_EVENT_ZIGBEE_CRASH, json);
        }
        else if (id == WATCHDOG_EVENT_WIFI_RSSI_LOW)
            webhook_post_event(WEBHOOK_EVENT_NETWORK_DOWN, NULL);
        else if (id == WATCHDOG_EVENT_UART_ERROR_RATE_HIGH)
            webhook_post_event(WEBHOOK_EVENT_ZIGBEE_CRASH, "{\"reason\":\"uart_error_rate_high\"}");
    } else if (base == NETWORK_EVENT) {
        if (id == NETWORK_EVENT_OFFLINE)
            webhook_post_event(WEBHOOK_EVENT_NETWORK_DOWN, NULL);
        else if (id == NETWORK_EVENT_FAILOVER)
            webhook_post_event(WEBHOOK_EVENT_NETWORK_FAILOVER, NULL);
        else if (id == NETWORK_EVENT_RECOVERED)
            webhook_post_event(WEBHOOK_EVENT_NETWORK_RECOVERED, NULL);
    } else if (base == WEBUI_EVENT) {
        /* ⚠️ 订的是 OTA_VERIFIED（新固件第一次起来了）而**不是** OTA_COMPLETE
           （镜像写完、马上重启）。2026-09-11 之前订的是后者，而那之后 500ms 就
           esp_restart()，一次 webhook 要走 DNS + TCP + TLS 几秒起步 ——
           这条通知从来就没送出去过。T2.7 修。 */
        if (id == WEBUI_EVENT_ID_OTA_VERIFIED)
            webhook_post_event(WEBHOOK_EVENT_OTA_COMPLETE, NULL);
    }
}

// ---- HTTP POST worker ----
static void webhook_task(void *arg)
{
    webhook_item_t item;

    while (1) {

        wd_kick(s_wd_slot_webhook);
        if (xQueueReceive(s_queue, &item, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }

        const webhook_config_t *cfg = config_mgr_get_webhook();
        if (!cfg->enabled || strlen(cfg->url) == 0) {
            continue;  // discard silently
        }

        // Build JSON body
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);

        /* ── 配置备份走单独一条路（T3.7）──
         *
         * 此前发的是一句占位符：{"hostname":..,"note":"use /api/v1/config/export"}，
         * 因为队列项的 data 只有 128 字节，装不下完整配置。于是这个功能是个空壳 ——
         * 收到通知的人还得自己再去拉一次，那还要通知干什么。
         *
         * 做法是**发送时才生成**，而不是入队时：
         *   · 队列项不必变大（8 个槽都跟着涨是纯浪费）
         *   · 发送仍然走下面同一段 HTTP 代码 —— 一个超时、一条日志、一处 cleanup
         *   · 备份反映的是**发出那一刻**的配置，比入队那一刻更有意义
         * 生成在 webhook 任务里做：栈 12288 且在 PSRAM，放得下 1~2KB 的 JSON。
         *
         * 导出已不含任何机密（password_hash / wifi.password 等一律 ********，
         * 2026-08-22 起），所以可以直接发。⚠️ 也正因如此，**恢复备份后必须重填
         * 密码类字段** —— 这一点要写在文档里，否则用户会以为恢复完就能用。 */
        char *backup_json = NULL;
        if (item.event == WEBHOOK_EVENT_CONFIG_BACKUP) {
            backup_json = config_mgr_export_json();
            if (!backup_json) {
                ESP_LOGW(TAG, "配置备份：导出失败，本次跳过");
                continue;
            }
        }

        const char *data_str = backup_json ? backup_json :
                               (strlen(item.data) > 0 ? item.data : NULL);

        /* 备份的 JSON 实测 ~1.2KB，定长 body 放不下 —— 按需分配，走 PSRAM。 */
        size_t body_cap = 384 + (data_str ? strlen(data_str) : 0);
        char  *body = heap_caps_malloc(body_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!body) body = malloc(body_cap);
        if (!body) {
            ESP_LOGW(TAG, "webhook body 分配失败（%u 字节）", (unsigned)body_cap);
            free(backup_json);
            continue;
        }

        int n = snprintf(body, body_cap,
            "{\"event\":\"%s\",\"timestamp\":%"PRIu32","
            "%s"
            "\"device\":\"%02X%02X%02X%02X%02X%02X\""
            "%s%s%s}",
            event_name(item.event),
            item.timestamp,
            /* 只在退化时出现这个字段：正常情况下接收端不必关心它 */
            item.uptime_based ? "\"uptime_based\":true," : "",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
            data_str ? ",\"data\":" : "",
            data_str ? data_str : "",
            "");

        free(backup_json);          /* 已经拷进 body 了 */
        backup_json = NULL;

        if (n < 0 || n >= (int)body_cap) {
            ESP_LOGW(TAG, "Webhook body 截断（需 %d / 有 %u），跳过", n, (unsigned)body_cap);
            free(body);
            continue;
        }

        /* 网络切换类通知（network_failover / network_recovered）正好发生在上行切换的那一刻：第一次发常常撞上
           出口网卡还没拿到地址、DNS 刚被清（T9.37 服务身份的交接是一轮 DHCP，约 1–2 s；第二轮审计 #4）。
           发不出去就隔 WEBHOOK_NET_RETRY_MS 再试，最多 WEBHOOK_NET_TRIES 次，每次重新取出口网卡；其他通知照旧只发一次。 */
        bool net_switch = (item.event == WEBHOOK_EVENT_NETWORK_FAILOVER || item.event == WEBHOOK_EVENT_NETWORK_RECOVERED);
        int tries = net_switch ? WEBHOOK_NET_TRIES : 1;
        for (int attempt = 0; attempt < tries; attempt++) {
            if (attempt) {
                vTaskDelay(pdMS_TO_TICKS(WEBHOOK_NET_RETRY_MS));
                wd_kick(s_wd_slot_webhook);
            }
            // HTTP POST. crt_bundle_attach enables server verification for
            // https:// URLs — without it every TLS webhook fails the handshake.
            /* 绑定出口网卡（T3.8）。不指定时 lwip 对**本网段**目的地按 netif 链表顺序
               选出口，**不看** esp_netif_set_default_netif 设的默认口 —— 双上行且两口同
               网段时，会从 WiFi 地址发出，哪怕 active_interface=eth。按来源 IP 做 ACL 的
               用户放行了以太网地址却连不上，排查时还会发现网关自报以太网而日志里是另
               一个 IP。
               取不到就不填，退回原来的行为（由 lwip 选），不因此放弃发送。 */
            struct ifreq ifr = {0};
            char ifname[8] = {0};
            network_mgr_active_ifname(ifname, sizeof(ifname));
            if (ifname[0]) strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));

            esp_http_client_config_t http_cfg = {
                .url = cfg->url,
                .timeout_ms = WEBHOOK_TIMEOUT_MS,
                .method = HTTP_METHOD_POST,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .if_name = ifname[0] ? &ifr : NULL,
            };

            esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
            if (!client) {
                ESP_LOGW(TAG, "HTTP client init failed");
                break;
            }

            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_post_field(client, body, strlen(body));

            /* NET 灯：投递前置位。放在 perform 之前而不是成功之后 —— 灯表达的是
               "网口上有数据进出"，请求发出去了就算，对端回没回 404 是另一回事。 */
            led_ctrl_notify_network_activity();
            esp_err_t err = esp_http_client_perform(client);
            if (err == ESP_OK) {
                int status = esp_http_client_get_status_code(client);
                ESP_LOGI(TAG, "Webhook sent: %s (HTTP %d)", event_name(item.event), status);
            } else {
                ESP_LOGW(TAG, "Webhook failed: %s (%s)", event_name(item.event),
                         esp_err_to_name(err));
            }

            esp_http_client_cleanup(client);
            if (err == ESP_OK) break;
        }
        free(body);
    }
}

// ---- Public API ----

/* 每类事件的开关。放在 post 这一层是有意的：过滤原先散在 system_event_handler
 * 的每个 if 里，于是绕开那个处理器的两条路（spectrum 的干扰告警、scheduler 的
 * 定时备份）根本不过滤 —— 用户在 WebUI 关掉「干扰」开关，照样收得到 interference。
 * 设计文档里写的 is_event_enabled() 过滤点此前并不存在。
 *
 * CONFIG_BACKUP 没有专属开关，它由 scheduler 的 backup_interval（0=关）控制，
 * 所以这里放行 —— 不是漏了。 */
static bool is_event_enabled(const webhook_config_t *cfg, webhook_event_t e)
{
    switch (e) {
    case WEBHOOK_EVENT_ZIGBEE_CRASH:     return cfg->event_zigbee_crash;
    case WEBHOOK_EVENT_NETWORK_DOWN:
    case WEBHOOK_EVENT_NETWORK_FAILOVER:
    case WEBHOOK_EVENT_NETWORK_RECOVERED: return cfg->event_network_down;
    case WEBHOOK_EVENT_TEMP_HIGH:        return cfg->event_temp_high;
    case WEBHOOK_EVENT_MEMORY_LOW:       return cfg->event_memory_low;
    case WEBHOOK_EVENT_INTERFERENCE:     return cfg->event_interference;
    case WEBHOOK_EVENT_OTA_AVAILABLE:
    case WEBHOOK_EVENT_OTA_COMPLETE:     return cfg->event_ota;
    case WEBHOOK_EVENT_CONFIG_BACKUP:    return true;   /* 见上：由 backup_interval 管 */
    }
    /* 新增枚举值忘了在这里登记时，宁可不发也不要默认放行 —— 默认发等于
       "关了还在发"这个 bug 的下一次复发。编译器的 -Wswitch 会先抓到它。 */
    ESP_LOGW(TAG, "未登记的 webhook 事件类型 %d，按关处理", (int)e);
    return false;
}

void webhook_post_event(webhook_event_t event, const char *data_json)
{
    if (!s_queue) return;

    const webhook_config_t *cfg = config_mgr_get_webhook();
    if (!cfg->enabled || !is_event_enabled(cfg, event)) return;

    /* timestamp：SNTP 同步过就给 epoch 秒，否则退回开机秒数并在 body 里标注
        （T3.8）。此前一律是开机秒数 —— 接收端拿到 "timestamp":42 完全无从
        对时间线，而 webhook 的头号用途正是"什么时候出的事"。
        判据用年份而不是 sntp 的状态位：从 RTC 恢复、手动设时间都算数，
        而那些路径不一定经过 sntp 的状态机。 */
    time_t now = time(NULL);
    struct tm tm_now;
    bool wall_clock_ok = (gmtime_r(&now, &tm_now) != NULL &&
                          tm_now.tm_year + 1900 > 2020);

    webhook_item_t item = {
        .event = event,
        .timestamp = wall_clock_ok ? (uint32_t)now
                                   : (uint32_t)(esp_timer_get_time() / 1000000),
        .uptime_based = !wall_clock_ok,
    };
    if (data_json) {
        strlcpy(item.data, data_json, sizeof(item.data));
    } else {
        item.data[0] = '\0';
    }

    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Webhook queue full, event %s dropped", event_name(event));
    }
}

esp_err_t webhook_init(void)
{
    if (s_queue) return ESP_OK;  /* already initialized — don't leak
                                    queue/task/handler registrations */

    s_queue = xQueueCreate(WEBHOOK_QUEUE_DEPTH, sizeof(webhook_item_t));
    if (!s_queue) return ESP_ERR_NO_MEM;

    // Subscribe to system events
    esp_event_handler_instance_register(WATCHDOG_EVENT, ESP_EVENT_ANY_ID,
                                        system_event_handler, NULL, NULL);
    esp_event_handler_instance_register(NETWORK_EVENT, ESP_EVENT_ANY_ID,
                                        system_event_handler, NULL, NULL);

    // Subscribe to OTA events for webhook notification
    esp_event_handler_instance_register(WEBUI_EVENT, ESP_EVENT_ANY_ID,
                                        system_event_handler, NULL, NULL);

    static const wd_task_spec_t wd_spec = {
        .name = "webhook", .deadline_ms = 120000, .severity = WD_SEV_OBSERVE,
    };
    s_wd_slot_webhook = wd_register(&wd_spec);
    /* 12288：2026-09-05 之前是 3072，而 webhook 在那之前从没真正发出过一次
       （feature_gap_audit D3）。同日真机实测（12288 栈上读 /diag/watchdog 的 stack_left）：
         空闲                 ≈1028 B
         一次纯 HTTP POST 峰值  1912 B（3072 勉强够，余量 1.1KB）
         含 HTTPS 发送的一次开机峰值 3560 B（> 3072：老栈在 mbedtls 路径上必溢出）
       栈在 PSRAM，加大不占内部 SRAM；psram_task.h 四条规则本任务全部满足。
       ⚠️ 同日还发现 TLS 本身在 INTERNAL 分配模式下建不起来，见 sdkconfig.defaults
       的 CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC 注释 —— 栈够了不等于 HTTPS 能用。 */
    psram_task_create(webhook_task, "webhook", 12288, NULL, 8, NULL);

    const webhook_config_t *cfg = config_mgr_get_webhook();
    // Mask URL in log to avoid leaking embedded auth tokens
    ESP_LOGI(TAG, "Webhook initialized (enabled=%d, url=%s)",
             cfg->enabled, strlen(cfg->url) > 0 ? "(configured)" : "(empty)");
    return ESP_OK;
}
