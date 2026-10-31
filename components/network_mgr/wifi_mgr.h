#pragma once

#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi_types.h"
#include "network_mgr.h"   /* wifi_link_stats_t */
#include <stdbool.h>
#include <stdint.h>

esp_err_t   wifi_mgr_init(void);
bool        wifi_mgr_has_credentials(void);
esp_err_t   wifi_mgr_start_sta(void);
esp_err_t   wifi_mgr_start_softap(void);
esp_err_t   wifi_mgr_stop_softap(void);
esp_err_t   wifi_mgr_scan_5g(wifi_ap_record_t *results, uint16_t *count);
void        wifi_mgr_schedule_reconnect(void);
void        wifi_mgr_reset_reconnect(void);
void        wifi_mgr_schedule_background_retry(void);
/* 最近一次 STA 断连的 wifi_err_reason_t。0 = 从未断过。 */
int         wifi_mgr_last_disconnect_reason(void);
void        wifi_mgr_set_disconnect_reason(int reason);
esp_netif_t *wifi_mgr_get_sta_netif(void);
esp_netif_t *wifi_mgr_get_ap_netif(void);
bool        wifi_mgr_is_connected(void);
/* 由 net_events.c 在 STA_CONNECTED / STA_DISCONNECTED 事件里维护 */
void        wifi_mgr_set_sta_connected(bool connected);
/* 由 net_events.c 在 STA_CONNECTED 里、标记已连上**之前**调：落在 2.4 GHz（ch1–14）就断开并返回 true。
   WiFi 只许 5 GHz（关键设计决策 4）；见 wifi_mgr.c 的 wifi_lock_5g 头注。 */
bool        wifi_mgr_reject_24g_link(uint8_t channel);
/* 由 net_events.c 在 STA_DISCONNECTED 里、set_sta_connected(false) 之后调：刚才被拒的 2.4 GHz 关联若把热点拖了过去
   （APSTA 下热点跟随 STA 信道），把它拉回 ch36；拉不回就关掉（决策 5）。没有被拒过时什么都不做。 */
void        wifi_mgr_softap_recheck(void);
/* 链路统计，由 net_events.c 维护：
   note_link_lost     在 STA_DISCONNECTED 里、set_sta_connected(false) **之前**调
   note_link_restored 在 IP_EVENT_STA_GOT_IP 里调 */
void        wifi_mgr_note_link_lost(void);
void        wifi_mgr_note_link_restored(void);
void        wifi_mgr_link_stats(wifi_link_stats_t *out);
bool        wifi_mgr_softap_active(void);
