/* wifi_none.c —— 纯以太网剖面里 wifi_mgr.h / wifi_probe.h 的"没有 Wi-Fi"实现（docs/15 设计 2.2）。
 *
 * 只在 CONFIG_BITENGINE_NET_ETH_ONLY 打开时代替 wifi_mgr.c / wifi_probe.c（network_mgr 的 CMakeLists.txt 选）。
 * 这个开关只在直连型里可见；HA 型永远编真的 wifi_mgr.c。
 *
 * 语义一律是"这台设备没有 Wi-Fi"：没有凭据、连不上、热点起不来、netif 为 NULL、统计全 0。于是 network_mgr 的
 * 状态机只走以太网那几步：INIT → 热点起不来 → NO_NETWORK → 插着网线 → ETH_CONNECTING → ETH_ONLINE；
 * 网线断了 → 没有凭据 → no_network_fallback() → 热点起不来 → NO_NETWORK。failover、IP 漂移（T2.23）、
 * 服务身份浮动（T9.37，sta netif 为 NULL 时 svc_id_init 直接不用）都因此不会发生。
 * 2026-10-21 G0-3 上板跑过同样语义的原型（spike/g0-3-eth-only）：以太网、Thread、桥接回归全过。
 *
 * ⚠️ 这里不许引用任何 esp_wifi_* 函数或 WIFI_EVENT —— 引用一个就会把整套 Wi-Fi 库链接进来，纯以太网剖面
 *    省下的 58 KB 内部内存就没了（tools/check_profiles.py 按库字节数查，network_mgr/VERIFY.sh 也钉着）。
 *    只用头文件里的类型（wifi_ap_record_t 之类）没关系，类型不产生链接。
 */
#include "sdkconfig.h"

#include <string.h>

#include "esp_log.h"
#include "wifi_mgr.h"
#include "wifi_probe.h"

static const char *TAG = "wifi_none";

esp_err_t wifi_mgr_init(void)
{
    ESP_LOGI(TAG, "Ethernet-only profile: no Wi-Fi in this build (docs/15 design 2.2)");
    return ESP_OK;
}

/* 配置里可能还留着从 HA 型迁移过来的 Wi-Fi 凭据（主 nvs 原样保留，设计 8.12）。这里一律说"没有"，
   状态机就不会去连；凭据本身不动，换回 HA 型时还在。 */
bool         wifi_mgr_has_credentials(void)              { return false; }
esp_err_t    wifi_mgr_start_sta(void)                    { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t    wifi_mgr_start_softap(void)                 { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t    wifi_mgr_stop_softap(void)                  { return ESP_OK; }   /* 没开着 = 已经停了 */
void         wifi_mgr_schedule_reconnect(void)           { }
void         wifi_mgr_reset_reconnect(void)              { }
void         wifi_mgr_schedule_background_retry(void)    { }
int          wifi_mgr_last_disconnect_reason(void)       { return 0; }
void         wifi_mgr_set_disconnect_reason(int reason)  { (void)reason; }
esp_netif_t *wifi_mgr_get_sta_netif(void)                { return NULL; }
esp_netif_t *wifi_mgr_get_ap_netif(void)                 { return NULL; }
bool         wifi_mgr_is_connected(void)                 { return false; }
void         wifi_mgr_set_sta_connected(bool connected)  { (void)connected; }
bool         wifi_mgr_reject_24g_link(uint8_t channel)   { (void)channel; return false; }
void         wifi_mgr_softap_recheck(void)               { }
void         wifi_mgr_note_link_lost(void)               { }
void         wifi_mgr_note_link_restored(void)           { }
bool         wifi_mgr_softap_active(void)                { return false; }

esp_err_t wifi_mgr_scan_5g(wifi_ap_record_t *results, uint16_t *count)
{
    (void)results;
    if (count) *count = 0;
    return ESP_ERR_NOT_SUPPORTED;
}

void wifi_mgr_link_stats(wifi_link_stats_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
}

esp_err_t wifi_probe_start(const char *target_ip, uint32_t interval_ms)
{
    (void)target_ip;
    (void)interval_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wifi_probe_stop(void) { return ESP_OK; }

void wifi_probe_get_status(wifi_probe_status_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
}
