#include "network_mgr.h"
#include "wifi_mgr.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#ifdef CONFIG_BITENGINE_HAS_ETHERNET
#include "esp_eth.h"
#include "eth_mgr.h"       /* eth_mgr_get_netif() — 组件私有头 */
#endif

static const char *TAG = "net_events";

// ---- WiFi events ----
#if !CONFIG_BITENGINE_NET_ETH_ONLY
/* 纯以太网剖面（docs/15 设计 2.2）不编这一段：不注册 WIFI_EVENT —— 这个符号定义在 Wi-Fi 库里，引用它就会把库链接进来 */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *data)
{
    switch (event_id) {
    case WIFI_EVENT_STA_CONNECTED: {
        const wifi_event_sta_connected_t *evt = (const wifi_event_sta_connected_t *)data;
        /* 5 GHz only (key decision #4), whatever path got us here. A rejected link is never
           marked up: the DISCONNECTED that follows counts as a failed retry and backs off. */
        if (evt && wifi_mgr_reject_24g_link(evt->channel)) break;
        ESP_LOGI(TAG, "WiFi STA connected to AP (ch%u)", evt ? evt->channel : 0);
        wifi_mgr_set_sta_connected(true);
        /* Create IPv6 link-local address — required for Thread Border Routing */
        esp_netif_create_ip6_linklocal(wifi_mgr_get_sta_netif());
        wifi_mgr_reset_reconnect();
        network_state_machine(NET_EVT_WIFI_CONNECTED);
        break;
    }

    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *evt = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "WiFi STA disconnected, reason=%d", evt->reason);
        /* 存下来给 REST 用 —— 配网页面靠它告诉用户"密码错了"还是"没找到这个网络"。
           光打日志用户看不到，只能盲改。 */
        wifi_mgr_set_disconnect_reason(evt->reason);
        /* 必须在 set_sta_connected(false) 之前：它靠"刚才还连着"区分新断线和重试失败 */
        wifi_mgr_note_link_lost();
        wifi_mgr_set_sta_connected(false);
        wifi_mgr_softap_recheck();   /* SoftAP dragged to 2.4 GHz by a rejected link → back to ch36 */
        network_state_machine(NET_EVT_WIFI_DISCONNECTED);
        break;
    }

    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *evt = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "SoftAP: client connected (aid=%d)", evt->aid);
        break;
    }

    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *evt = (wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "SoftAP: client disconnected (aid=%d)", evt->aid);
        break;
    }

    default:
        break;
    }
}

#endif /* !CONFIG_BITENGINE_NET_ETH_ONLY */

// ---- IP events ----
static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *data)
{
    switch (event_id) {
    case IP_EVENT_STA_GOT_IP: {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        /* A link refused by wifi_mgr_reject_24g_link() is never marked connected. With a
           static IP, esp_netif_action_connected() posts GOT_IP straight from STA_CONNECTED
           without waiting for DHCP (esp_netif_handlers.c, IDF v6.0.2), so without this the
           2.4 GHz link would be declared online before its DISCONNECTED arrives. */
        if (!wifi_mgr_is_connected()) {
            ESP_LOGW(TAG, "WiFi STA got IP " IPSTR " on a link that was not accepted — ignored",
                     IP2STR(&evt->ip_info.ip));
            break;
        }
        ESP_LOGI(TAG, "WiFi STA got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        wifi_mgr_note_link_restored();
        network_state_machine(NET_EVT_WIFI_GOT_IP);
        break;
    }

    case IP_EVENT_STA_LOST_IP:
        ESP_LOGW(TAG, "WiFi STA lost IP");
        network_state_machine(NET_EVT_WIFI_LOST_IP);
        break;

#ifdef CONFIG_BITENGINE_HAS_ETHERNET
    case IP_EVENT_ETH_GOT_IP: {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "ETH got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        /* 建 IPv6 链路本地地址 —— Thread 边界路由的前置条件。
           OpenThread 的 RoutingManager 要以 LL 地址为源往 ff02::2 发路由请求(RS)，
           没有 LL 就一条 RS 都发不出去 → 收不到 RA → RoutePublisher 停在 none →
           fc00::/7 不发布 → 有线部署下 Thread 设备没有经网关出去的 ULA 默认路由。
           NAT64 /96 不依赖 RA 照常发布，所以从"边界路由初始化成功"看不出问题，
           很容易误以为是好的。

           时机很关键：必须在这里（GOT_IP），不能在 ETHERNET_EVENT_CONNECTED。
           在那里建，API 返回 ESP_OK 但地址随后被以太网 glue 的
           esp_netif_action_connected() 抹掉，读回永远 ESP_FAIL。实测：
           在 link-up 建 → Sent RS 0 次 / Received RA 0 次 / fc00::/7 从不发布；
           改到这里 → Sent RS 3 次 / Received RA 10 次 / fc00::/7 进入 Added。

           H1：esp_netif_create_ip6_linklocal() 不检查 NULL——IDF v6.0.2 的
           esp_netif_lwip.c:2385 直接解引用 esp_netif->lwip_netif，派发宏也不拦
           （旁边的 get_ip6_linklocal 反而检查了）。eth_mgr_get_netif() 在以太网
           初始化失败时返回 NULL，所以必须自己挡一道。 */
        /* ⚠️ 已经可用（preferred）就别再建（2026-10-22）：lwIP 的 netif_create_ip6_linklocal_address()
           无条件把地址打回 TENTATIVE 重做 DAD，这 1–2 s 里从它发出的包全失败（OT 的 RS、组播探测 ——
           开机偶发的 "Failed to send RS 2/3" 就是它），网关也不应答对它的 NS，HA → Thread 的下一跳
           正是这个地址。一次开机会来两次 GOT_IP：T9.37 服务身份交接时网口用自己的身份重新申请 DHCP，
           地址不变、链路没断。链路真断过时以太网 glue 会把它抹掉（见上），那时读回失败，照建。 */
        {
            esp_netif_t *eth_nif = eth_mgr_get_netif();
            esp_ip6_addr_t ll_now;
            if (!eth_nif) {
                ESP_LOGW(TAG, "ETH netif missing — skipping IPv6 link-local setup "
                              "(Thread border routing will lack a default route)");
            } else if (esp_netif_get_ip6_linklocal(eth_nif, &ll_now) == ESP_OK) {
                ESP_LOGI(TAG, "ETH IPv6 link-local already preferred (" IPV6STR "), not recreating", IPV62STR(ll_now));
            } else {
                esp_err_t e6 = esp_netif_create_ip6_linklocal(eth_nif);
                if (e6 != ESP_OK) {
                    ESP_LOGW(TAG, "ETH IPv6 link-local not created: %s "
                                  "(Thread border routing will lack a default route)",
                             esp_err_to_name(e6));
                } else {
                    /* 刚建出来还在 DAD（tentative），此刻读回拿不到属正常，
                       所以这里不告警。约 1s 后转 preferred，OT 随即开始发 RS。 */
                    esp_ip6_addr_t ll;
                    if (esp_netif_get_ip6_linklocal(eth_nif, &ll) == ESP_OK) {
                        ESP_LOGI(TAG, "ETH IPv6 link-local: " IPV6STR, IPV62STR(ll));
                    } else {
                        ESP_LOGI(TAG, "ETH IPv6 link-local created (DAD pending)");
                    }
                }
            }
        }
        network_state_machine(NET_EVT_ETH_GOT_IP);
        break;
    }
#endif

    default:
        break;
    }
}

#ifdef CONFIG_BITENGINE_HAS_ETHERNET
// ---- Ethernet events ----
static void eth_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *data)
{
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "ETH link up");
        /* 注意：IPv6 链路本地地址**不能**在这里建。
           IDF 官方 examples 的 eth_connect.c 确实是在 ETHERNET_EVENT_CONNECTED
           调 esp_netif_create_ip6_linklocal() 的，但那依赖它的 handler 排在
           以太网 glue 之后。我们的 handler 排在前面，此刻 glue 的
           esp_netif_action_connected() 还没跑完，建出来的地址随后被抹掉——
           API 返回 ESP_OK，读回却永远是 ESP_FAIL。实测走过这个坑，见
           IP_EVENT_ETH_GOT_IP 分支。 */
        network_state_machine(NET_EVT_ETH_LINK_UP);
        break;

    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "ETH link down");
        network_state_machine(NET_EVT_ETH_LINK_DOWN);
        break;

    default:
        break;
    }
}
#endif

esp_err_t net_events_register(void)
{
    esp_err_t err;

#if !CONFIG_BITENGINE_NET_ETH_ONLY
    err = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) return err;
#endif

    err = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL, NULL);
    if (err != ESP_OK) return err;

    err = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_LOST_IP, ip_event_handler, NULL, NULL);
    if (err != ESP_OK) return err;

#ifdef CONFIG_BITENGINE_HAS_ETHERNET
    err = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_ETH_GOT_IP, ip_event_handler, NULL, NULL);
    if (err != ESP_OK) return err;

    err = esp_event_handler_instance_register(
        ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL, NULL);
    if (err != ESP_OK) return err;
#endif

    ESP_LOGI(TAG, "Network event handlers registered");
    return ESP_OK;
}
