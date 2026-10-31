#include "wifi_probe.h"
#include "wifi_mgr.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "ping/ping_sock.h"
#include "lwip/inet.h"

static const char *TAG = "wifi_probe";

/* H1（ping_sock.h，IDF v6.0.2）：count=0 (ESP_PING_COUNT_INFINITE) 无限 ping；
   interface 是 lwIP netif index（esp_netif_get_netif_impl_index），钉在 STA 口；
   回调跑在 ping 任务里，**不能**在回调里 esp_ping_delete_session；
   on_ping_timeout 在 timeout_ms 到期时调，所以"发包时刻" = 回调时刻 - timeout_ms。 */
#define PROBE_INTERVAL_MS 1000
#define PROBE_TIMEOUT_MS  900

static esp_ping_handle_t   s_hdl = NULL;
static wifi_probe_status_t s_st;
static uint32_t            s_hole_start_ms;   /* 当前洞的估算发包时刻 */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static int8_t read_rssi(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

static void on_success(esp_ping_handle_t hdl, void *args)
{
    (void)hdl; (void)args;
    s_st.sent++; s_st.replied++;
    if (s_st.cur_miss_run == 0) return;

    /* 洞结束：从第一次没回音的发包时刻，到这次有回音的发包时刻 */
    uint32_t end_ms = now_ms();
    uint32_t dur = end_ms - s_hole_start_ms;
    uint32_t i = s_st.holes % WIFI_PROBE_HOLE_RING;
    s_st.hole_start_ms[i] = s_hole_start_ms;
    s_st.hole_ms[i] = dur;
    s_st.holes++;
    if (dur > s_st.max_hole_ms) s_st.max_hole_ms = dur;
    ESP_LOGW(TAG, "hole end: %" PRIu32 " ms, %" PRIu32 " misses, rssi now %d",
             dur, s_st.cur_miss_run, read_rssi());
    s_st.cur_miss_run = 0;
}

static void on_timeout(esp_ping_handle_t hdl, void *args)
{
    (void)hdl; (void)args;
    s_st.sent++; s_st.missed++;
    s_st.cur_miss_run++;
    if (s_st.cur_miss_run == 1) {
        s_hole_start_ms = now_ms() - PROBE_TIMEOUT_MS;
        s_st.rssi = read_rssi();
        uint16_t seq = 0;
        esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
        ESP_LOGW(TAG, "hole start: seq %u no reply, rssi %d", seq, s_st.rssi);
    }
}

esp_err_t wifi_probe_start(const char *target_ip, uint32_t interval_ms)
{
    if (interval_ms == 0) interval_ms = PROBE_INTERVAL_MS;
    if (interval_ms < 200 || interval_ms > 60000) return ESP_ERR_INVALID_ARG;
    if (!target_ip || !target_ip[0]) return ESP_ERR_INVALID_ARG;
    ip4_addr_t t;
    if (!ip4addr_aton(target_ip, &t)) return ESP_ERR_INVALID_ARG;
    esp_netif_t *sta = wifi_mgr_get_sta_netif();
    if (!sta) return ESP_ERR_INVALID_STATE;

    wifi_probe_stop();
    memset(&s_st, 0, sizeof(s_st));
    strlcpy(s_st.target, target_ip, sizeof(s_st.target));

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count       = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = interval_ms;
    cfg.timeout_ms  = PROBE_TIMEOUT_MS;
    cfg.data_size   = 32;
    cfg.interface   = esp_netif_get_netif_impl_index(sta);
    cfg.target_addr.type = IPADDR_TYPE_V4;
    cfg.target_addr.u_addr.ip4.addr = t.addr;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = on_success,
        .on_ping_timeout = on_timeout,
    };
    esp_err_t err = esp_ping_new_session(&cfg, &cbs, &s_hdl);
    if (err != ESP_OK) { s_hdl = NULL; return err; }
    err = esp_ping_start(s_hdl);
    if (err != ESP_OK) { esp_ping_delete_session(s_hdl); s_hdl = NULL; return err; }
    s_st.running = true;
    ESP_LOGI(TAG, "probe started: %s via STA netif every %" PRIu32 " ms", s_st.target, interval_ms);
    return ESP_OK;
}

esp_err_t wifi_probe_stop(void)
{
    if (!s_hdl) return ESP_OK;
    esp_ping_stop(s_hdl);
    esp_ping_delete_session(s_hdl);   /* 从 httpd 任务调，不在回调里 */
    s_hdl = NULL;
    s_st.running = false;
    ESP_LOGI(TAG, "probe stopped: sent %" PRIu32 " replied %" PRIu32 " missed %" PRIu32 " holes %" PRIu32,
             s_st.sent, s_st.replied, s_st.missed, s_st.holes);
    return ESP_OK;
}

void wifi_probe_get_status(wifi_probe_status_t *out)
{
    if (out) *out = s_st;   /* 诊断读数，读到新旧混合值可接受 */
}
