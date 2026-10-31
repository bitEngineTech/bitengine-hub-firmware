#include "wifi_mgr.h"
#include "network_mgr.h"
#include "config_mgr.h"

#include <string.h>
#include <inttypes.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"     /* T2.21 保活：raw ICMP socket，见 keepalive_open() */
#include "lwip/inet_chksum.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/ip4.h"
#include <net/if.h>
#include "esp_netif.h"
#include "psram_task.h"

static const char *TAG = "wifi_mgr";

static esp_netif_t       *s_sta_netif       = NULL;
static esp_netif_t       *s_ap_netif        = NULL;
static esp_timer_handle_t s_reconnect_timer = NULL;
static esp_timer_handle_t s_exhausted_timer = NULL;
static int                s_reconnect_count = 0;
static volatile bool      s_sta_connected   = false;
static volatile bool      s_softap_active   = false;
/* 最近一次 STA 断连原因（wifi_err_reason_t）。配网时前端靠它区分
   "密码错误" 和 "找不到该网络" —— 只打日志的话用户什么也看不到。 */
static volatile int       s_last_disc_reason = 0;

#if CONFIG_BITENGINE_WIFI_KEEPALIVE_MS > 0
/* T2.21：WiFi 上行保活。某些 AP 会 park 空闲客户端，每 ~60s 有几秒不给它送下行单播
   （真板复现，见 docs/产品化TODO T2.21）；持续上行即可消除。这里对 WiFi 网关做一个
   无限低速 ping，钉在 STA 网口（用 netif index，不能靠源 IP —— 双网口同网段会误路由）。
   结果不关心，只要那一帧从 WiFi 发出去。网关地址变了就换目标，STA 掉线就停。
 *
 * ⚠️ 2026-10-05 起不再用 esp_ping，别改回去：esp_ping_new_session() 会另起一个
 * **内部** SRAM 栈的任务（ESP_TASK_PING_STACK，实测 2560 + TCB），WiFi 做主期间一直
 * 占着 —— 诊断追踪器（watchdog/wd_heaptrace.c）量出它是拔线 failover 后 DMA 池低谷
 * 的三块之一。现在由本任务（栈在 PSRAM）自己每轮发一个 ICMP echo。空口上的帧与
 * esp_ping 发的相同（ICMP echo → WiFi 网关、钉在 STA 口、8 字节载荷），T2.21 验过的
 * 效果不变。做法照抄 IDF lwip/apps/ping/ping_sock.c：raw socket + SO_BINDTODEVICE
 * （按 netif index 取名）+ inet_chksum。回包每轮非阻塞收干净（raw socket 的接收邮箱
 * 很小，不收会占着 pbuf），并按 id 计数 —— 停止时把"发了几个、网关回了几个"打进日志，
 * 这是保活真的在空口上生效的唯一证据。 */
#define KA_ICMP_ID     0x4B41      /* "KA"：只用来从收到的 ICMP 里认出自己的回包 */
#define KA_DATA_SIZE   8

static int      s_ka_sock     = -1;
static uint32_t s_ka_gw       = 0;     /* 网络字节序；0 = 没在保活 */
static uint16_t s_ka_seq      = 0;
static uint32_t s_ka_sent     = 0;
static uint32_t s_ka_replies  = 0;

static void keepalive_drain(void);

static void keepalive_stop(void)
{
    if (s_ka_sock >= 0) {
        /* 每轮是先收后发，最后一个 echo 的回包只能在这里收 —— 不收的话链路完全正常
           日志也是"发 N 回 N-1"，会被读成丢包。必须在 s_ka_gw 清零之前（drain 按它认回包）。 */
        keepalive_drain();
        close(s_ka_sock);
        s_ka_sock = -1;
        ESP_LOGI(TAG, "WiFi keepalive stopped (sent %" PRIu32 ", gateway replied %" PRIu32 ")",
                 s_ka_sent, s_ka_replies);
    }
    s_ka_gw = 0;
}

/* 建 raw ICMP socket 并钉在 STA 口。失败返回 false，下一轮再试。 */
static bool keepalive_open(void)
{
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) return false;
    struct ifreq ifr = { 0 };
    int idx = esp_netif_get_netif_impl_index(s_sta_netif);
    if (idx <= 0 || if_indextoname((unsigned)idx, ifr.ifr_name) == NULL ||
        setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) != 0) {
        close(sock);
        return false;
    }
    s_ka_sock = sock;
    s_ka_sent = 0;
    s_ka_replies = 0;
    return true;
}

/* 非阻塞把邮箱收干净，数一下自己的回包（上一轮发的那个）。 */
static void keepalive_drain(void)
{
    uint8_t buf[sizeof(struct ip_hdr) + sizeof(struct icmp_echo_hdr) + KA_DATA_SIZE + 40];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    int n;
    while ((n = recvfrom(s_ka_sock, buf, sizeof(buf), MSG_DONTWAIT,
                         (struct sockaddr *)&from, &fromlen)) > 0) {
        fromlen = sizeof(from);
        if (n < (int)sizeof(struct ip_hdr)) continue;
        const struct ip_hdr *iph = (const struct ip_hdr *)buf;
        int hl = IPH_HL_BYTES(iph);
        if (n < hl + (int)sizeof(struct icmp_echo_hdr)) continue;
        const struct icmp_echo_hdr *e = (const struct icmp_echo_hdr *)(buf + hl);
        if (ICMPH_TYPE(e) == ICMP_ER && e->id == lwip_htons(KA_ICMP_ID) &&
            from.sin_addr.s_addr == s_ka_gw) {
            s_ka_replies++;
        }
    }
}

static void keepalive_send(void)
{
    struct {
        struct icmp_echo_hdr h;
        uint8_t data[KA_DATA_SIZE];
    } __attribute__((packed)) pkt;
    memset(&pkt, 0, sizeof(pkt));
    ICMPH_TYPE_SET(&pkt.h, ICMP_ECHO);
    ICMPH_CODE_SET(&pkt.h, 0);
    pkt.h.id    = lwip_htons(KA_ICMP_ID);
    pkt.h.seqno = lwip_htons(++s_ka_seq);
    memset(pkt.data, 'K', sizeof(pkt.data));
    pkt.h.chksum = inet_chksum(&pkt, sizeof(pkt));

    struct sockaddr_in to = { .sin_family = AF_INET };
    to.sin_addr.s_addr = s_ka_gw;
    if (sendto(s_ka_sock, &pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to))
            == (ssize_t)sizeof(pkt)) {
        s_ka_sent++;
    }
    /* 发不出去不单独报：STA 掉线时下一轮 supervise 就会停掉保活；
       停的时候 sent 对不上轮数，日志里看得出来 */
}

static void wifi_keepalive_supervise(void)
{
    if (!s_sta_netif) return;
    /* 只在 WiFi 是活动上行时保活（failover 期间）。以太网做主时 WiFi 是空闲备用，
       被 AP park 也无所谓——真要 failover，第一批流量自己会把它唤醒，不值得常态白 ping。*/
    network_status_t ns;
    bool wifi_active = (network_mgr_get_status(&ns) == ESP_OK &&
                        ns.active_interface == NET_IF_WIFI);
    esp_netif_ip_info_t ip;
    bool up = wifi_active &&
              (esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK &&
               ip.ip.addr != 0 && ip.gw.addr != 0);
    if (!up) {
        keepalive_stop();
        return;
    }
    if (s_ka_sock < 0) {
        static bool s_open_failing = false;     /* 只报第一次和恢复，别每 5 s 刷一条 */
        if (!keepalive_open()) {
            if (!s_open_failing) {
                ESP_LOGW(TAG, "WiFi keepalive socket failed to open (sockets exhausted?) — "
                              "retrying every %dms", CONFIG_BITENGINE_WIFI_KEEPALIVE_MS);
            }
            s_open_failing = true;
            return;
        }
        if (s_open_failing) ESP_LOGI(TAG, "WiFi keepalive socket opened after earlier failures");
        s_open_failing = false;
    }
    if (s_ka_gw != ip.gw.addr) {                 /* 刚开始，或网关变了：换目标 */
        s_ka_gw = ip.gw.addr;
        ESP_LOGI(TAG, "WiFi keepalive -> " IPSTR " every %dms (T2.21)",
                 IP2STR(&ip.gw), CONFIG_BITENGINE_WIFI_KEEPALIVE_MS);
    }
    keepalive_drain();
    keepalive_send();
}

static void wifi_keepalive_task(void *arg)
{
    (void)arg;
    while (1) {
        /* 每隔 interval 醒一次：WiFi 做主时发一个保活 echo（supervise 里），
           否则只确认保活是停着的。2026-10-05 前发包靠 esp_ping 自己的内部栈任务。 */
        vTaskDelay(pdMS_TO_TICKS(CONFIG_BITENGINE_WIFI_KEEPALIVE_MS));
        wifi_keepalive_supervise();
    }
}
#endif /* CONFIG_BITENGINE_WIFI_KEEPALIVE_MS > 0 */

// ---- 5 GHz only (key decision #4) ----

/* Locking the band mode once is not enough (2026-10-10, RF bench with ANT2 off so no
   5 GHz AP was visible): a scan started from the web page covered 2.4 GHz too, and the
   esp_wifi_connect() running at the same time picked the 2.4 GHz twin of the same SSID
   ("connected with MyWiFi ... channel 11"). The SoftAP then had to follow the STA's channel,
   its ch36 config was refused, and it stayed up on 2.4 GHz anyway. Four defences:
     - every connect re-asserts the lock first, with the result checked (wifi_lock_5g)
     - scans skip the 2.4 GHz band via channel_bitmap (wifi_mgr_scan_5g)
     - an association that still lands on 2.4 GHz is dropped at once (wifi_mgr_reject_24g_link,
       called from net_events.c before the link is marked up); a SoftAP it dragged along is
       moved back to ch36 once the link is gone (wifi_mgr_softap_recheck)
     - a SoftAP whose config is refused is torn down, not left on the driver's channel
   check_band_lock.py pins all four. */
#define WIFI_2G_MAX_CHANNEL    14
#define WIFI_SCAN_BYPASS_BAND  1u           /* channel_bitmap bit0: skip the whole band */
#define WIFI_SCAN_5G_ALL       0x1FFFFFFEu  /* bits 1..28 = ch36..ch177 (wifi_5g_channel_bit_t) */

/* Set by wifi_mgr_reject_24g_link() while the SoftAP is up: in APSTA mode the AP follows the
   STA's home channel by CSA (IDF wifi-driver overview, "home channel"), so a rejected 2.4 GHz
   link has already pulled the AP there. Handled on the DISCONNECTED that follows. */
static volatile bool s_softap_recheck = false;

static esp_err_t wifi_lock_5g(const char *where)
{
    /* Read first. The driver refuses to change the band while the STA is scanning or connecting
       or the AP has a client ("cannot set band" in libnet80211) — which is exactly when SoftAP
       provisioning runs. Already 5G_ONLY is the normal case and needs no change at all. */
    wifi_band_mode_t cur = WIFI_BAND_MODE_AUTO;
    if (esp_wifi_get_band_mode(&cur) == ESP_OK && cur == WIFI_BAND_MODE_5G_ONLY) return ESP_OK;
    /* Nobody here sets anything else, so this means something moved it (10-10 never found what) */
    ESP_LOGW(TAG, "band mode is %d, not 5G_ONLY (%s) — locking again", (int)cur, where);
    esp_err_t err = esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "5GHz lock failed (%s): %s", where, esp_err_to_name(err));
    }
    return err;
}

bool wifi_mgr_reject_24g_link(uint8_t channel)
{
    if (channel == 0 || channel > WIFI_2G_MAX_CHANNEL) return false;
    wifi_band_mode_t bm = WIFI_BAND_MODE_AUTO;
    (void)esp_wifi_get_band_mode(&bm);
    ESP_LOGE(TAG, "STA associated on 2.4 GHz ch%u (band mode %d) — 5 GHz only (key decision #4), "
                  "dropping the link", channel, (int)bm);
    /* Disconnect before locking: the band can't be changed while associated. The DISCONNECTED
       event that follows drives the normal retry, and the next connect locks again anyway. */
    esp_wifi_disconnect();
    if (s_softap_active) s_softap_recheck = true;
    (void)wifi_lock_5g("2.4G link rejected");
    return true;
}

static void softap_build_config(wifi_config_t *ap_cfg, char *ssid, size_t ssid_size);

void wifi_mgr_softap_recheck(void)
{
    if (!s_softap_recheck) return;
    s_softap_recheck = false;
    if (!s_softap_active) return;

    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_err_t err = esp_wifi_get_channel(&primary, &second);
    if (err == ESP_OK && primary > WIFI_2G_MAX_CHANNEL) {
        /* 2026-10-21 on the board: the driver itself moved the AP back once the band was 5G_ONLY again */
        ESP_LOGI(TAG, "SoftAP already back on ch%u after the rejected 2.4 GHz STA link", primary);
        return;
    }

    wifi_config_t ap_cfg;
    char ssid[32];
    softap_build_config(&ap_cfg, ssid, sizeof(ssid));
    err = wifi_lock_5g("SoftAP recheck");
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        /* Key decision #5: the hotspot is 5 GHz or nothing. network_mgr learns it through
           wifi_mgr_softap_active() (network_mgr_start_softap re-checks it). */
        ESP_LOGE(TAG, "SoftAP stuck on 2.4 GHz ch%u after a rejected STA link (%s) — stopping it",
                 primary, esp_err_to_name(err));
        (void)wifi_mgr_stop_softap();
        return;
    }
    ESP_LOGW(TAG, "SoftAP moved back to ch36 after the rejected 2.4 GHz STA link (was ch%u)", primary);
}

// ---- Forward declarations ----
static void reconnect_timer_cb(void *arg);
static void exhausted_timer_cb(void *arg);

// ---- Init ----

esp_err_t wifi_mgr_init(void)
{
    s_sta_netif = esp_netif_create_default_wifi_sta();
    /* DHCP 主机名（option 12）。不设的话路由器客户端列表里显示 IDF 默认的
       "espressif" —— 家里有几个 ESP 设备就分不清哪个是网关，用户想做
       DHCP 保留都找不到目标。而 DHCP 保留正是"IP 变来变去"的正解。
       必须在 DHCP 启动前设，否则要等下一次续租才生效。 */
    esp_netif_set_hostname(s_sta_netif, config_mgr_get_sys()->hostname);
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "WiFi init failed");

    const esp_timer_create_args_t reconn_args = {
        .callback = reconnect_timer_cb,
        .name     = "wifi_reconnect",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&reconn_args, &s_reconnect_timer),
                        TAG, "Reconnect timer create failed");

    const esp_timer_create_args_t exhaust_args = {
        .callback = exhausted_timer_cb,
        .name     = "wifi_exhausted",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&exhaust_args, &s_exhausted_timer),
                        TAG, "Exhausted timer create failed");

#if CONFIG_BITENGINE_WIFI_KEEPALIVE_MS > 0
    /* 栈走 PSRAM：这 3072 在 net_init 区间内，而以太网主用时本任务每 5s 醒来
       只为 return（supervise 第一件事就是查 active_interface）—— 拿内部 SRAM
       养一个常态空转的任务是本清单里性价比最差的一笔。
       psram_task.h 四条前提 2026-09-15 逐条核过：
         1. 不碰 flash/NVS：调用链只有 network_mgr_get_status（grep nvs_ /
            esp_partition_ / config_mgr_ 三者零命中）、esp_netif_get_ip_info、
            lwip socket（2026-10-05 起保活自己发 ICMP，不再用 esp_ping），都不走 flash；
         2. 常驻不自删：while(1) 永不返回，不存在 vTaskDeleteWithCaps 漏栈；
         3. TCB 仍在内部内存（IDF 强制），本改动不碰它；
         4. 非延迟敏感：优先级 4，每 5s 一次状态检查。
       ⚠️ 将来若在 supervise 里加"把保活状态落 NVS"之类，必须改回 xTaskCreate。 */
    psram_task_create(wifi_keepalive_task, "wifi_keepalive", 3072, NULL, 4, NULL);
#endif

    ESP_LOGI(TAG, "WiFi driver initialized");
    return ESP_OK;
}

// ---- Credential access (via config_mgr) ----

bool wifi_mgr_has_credentials(void)
{
    return config_mgr_get_wifi()->has_credentials;
}

// ---- STA ----

esp_err_t wifi_mgr_start_sta(void)
{
    const wifi_config_data_t *wcfg = config_mgr_get_wifi();
    if (!wcfg->has_credentials) {
        ESP_LOGW(TAG, "No WiFi credentials available");
        return ESP_ERR_NOT_FOUND;
    }

    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, wcfg->ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, wcfg->password, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    if (s_softap_active) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA),
                            TAG, "Set APSTA mode failed");
    } else {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA),
                            TAG, "Set STA mode failed");
    }

    /* ⚠️ 新凭据 = 全新开始，必须清掉退避计数。
       不清的话：用户第一次把密码打错 → 7 次重连全失败 → count 停在 7；
       他改正密码再提交 → 这里立刻连一次（对的），但万一又打错，
       schedule_reconnect() 从 count=7 起步 = 直接判耗尽 = **下一次要等 5 分钟**。
       配网时用户正盯着页面等反馈，5 分钟等于功能不可用。 */
    wifi_mgr_reset_reconnect();

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg),
                        TAG, "Set STA config failed");
    /* PMF 默认**开着**（IDF 默认 PMF Optional：AP 支持就用）。
       2026-03-30 起这里曾无条件 esp_wifi_disable_pmf_config()，理由是"Thread 电台活动时
       SA Query 超时"——那次一并改了共存使能和 WIFI_PS_NONE，没有单变量隔离；代价是
       WPA3-only / PMF 强制的路由器完全连不上，而用户自家混合模式路由器一直在以 WPA2 无 PMF
       降级连接。2026-09-13 在本 PCB + v6.0.2 上开着 PMF（WPA3-SAE H2E, pmf:1）跑 20 分钟、
       叠加 53 次 6.6s 的 802.15.4 主动扫描，断线 0、SA Query 0（TODO T2.17）。
       留一个 Kconfig 逃生口：现场若真见到 reason=209（SA_QUERY_TIMEOUT）反复出现，打开它。 */
#if CONFIG_BITENGINE_WIFI_DISABLE_PMF
    esp_wifi_disable_pmf_config(WIFI_IF_STA);
    ESP_LOGW(TAG, "PMF disabled by CONFIG_BITENGINE_WIFI_DISABLE_PMF: WPA3-only APs will not connect");
#endif
    /* Clear stale AP/PMK cache — prevents auth failure after firmware update */
    esp_wifi_disconnect();
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "WiFi start failed");
    /* Keep WiFi always active — reduces beacon loss during 802.15.4 coexistence */
    esp_wifi_set_ps(WIFI_PS_NONE);
    /* A failed lock must not return early: nothing would be connecting, no DISCONNECTED
       would ever arrive, and the retry chain would never start. Same as a failed connect. */
    esp_err_t err = wifi_lock_5g("start STA");
    if (err == ESP_OK) err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STA connect not started: %s, will retry",
                 esp_err_to_name(err));
        wifi_mgr_schedule_reconnect();
    }

    return ESP_OK;
}

// ---- SoftAP ----

/* Key decision #5: SoftAP on 5 GHz ch36. Shared by start and by the post-reject recheck. */
static void softap_build_config(wifi_config_t *ap_cfg, char *ssid, size_t ssid_size)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(ssid, ssid_size, "bitEngine-Hub-%02X%02X", mac[4], mac[5]);

    *ap_cfg = (wifi_config_t){
        .ap = {
            .channel        = 36,
            .max_connection  = 4,
            .authmode        = WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)ap_cfg->ap.ssid, ssid, sizeof(ap_cfg->ap.ssid));
    ap_cfg->ap.ssid_len = strlen(ssid);
}

esp_err_t wifi_mgr_start_softap(void)
{
    /* Already up: nothing to do. Running the steps below again and failing would tear down a
       working hotspot (network_mgr's s_softap_on and this flag are not always in step). */
    if (s_softap_active) return ESP_OK;

    wifi_config_t ap_cfg;
    char ssid[32];
    softap_build_config(&ap_cfg, ssid, sizeof(ssid));

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    if (mode == WIFI_MODE_STA) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA),
                            TAG, "Set APSTA mode failed");
    } else {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP),
                            TAG, "Set AP mode failed");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "WiFi start failed");
        esp_wifi_set_ps(WIFI_PS_NONE);
    }

    /* Key decision #4/#5: 5GHz only, always — without the band lock the
       ch36 AP config is invalid and the driver could fall back to 2.4GHz.
       The STA path sets this too, but the pure-AP first-boot path must
       set it itself. */
    esp_err_t err = wifi_lock_5g("SoftAP");
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        /* The AP interface is already up by now; leaving it would run it on whatever
           channel the driver chose. 2026-10-10 the ch36 config was refused because the
           STA sat on 2.4 GHz ch11, and the AP stayed up on ch11 with a client on it. */
        ESP_LOGE(TAG, "SoftAP not started (%s) — back to STA mode", esp_err_to_name(err));
        esp_err_t m = esp_wifi_set_mode(WIFI_MODE_STA);
        if (m != ESP_OK) {
            ESP_LOGE(TAG, "back to STA mode failed too (%s) — AP interface may still be up", esp_err_to_name(m));
        }
        return err;
    }

    s_softap_active = true;
    ESP_LOGI(TAG, "SoftAP started: SSID=%s, IP=192.168.4.1", ssid);

    esp_event_post(NETWORK_EVENT, NETWORK_EVENT_SOFTAP_STARTED, NULL, 0, 0);
    return ESP_OK;
}

esp_err_t wifi_mgr_stop_softap(void)
{
    if (!s_softap_active) return ESP_OK;

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    /* 两个分支都只降级到 STA，**不要** esp_wifi_stop()。

       原来纯 AP 分支调 esp_wifi_stop() 会把整个 WiFi 驱动停掉，而不只是关掉 AP。
       出厂设备走的正是纯 AP 路径，插上网线拿到 IP 后状态机调到这里，WiFi 就此
       整体停止，此后：
         - wifi_mgr_scan_5g() 只切模式、从不 esp_wifi_start()，扫描恒返回
           ESP_ERR_WIFI_NOT_STARTED（实测日志坐实），WebUI 的扫描按钮点不动
         - 于是"插网线后用 WebUI 配 WiFi"这条配网路径也断了
       即出厂设备两条配网路径同时失效。 */
    if (mode == WIFI_MODE_APSTA || mode == WIFI_MODE_AP) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA),
                            TAG, "Switch to STA mode failed");
    }

    s_softap_active = false;
    ESP_LOGI(TAG, "SoftAP stopped");

    esp_event_post(NETWORK_EVENT, NETWORK_EVENT_SOFTAP_STOPPED, NULL, 0, 0);
    return ESP_OK;
}

// ---- Scan (5GHz only) ----

esp_err_t wifi_mgr_scan_5g(wifi_ap_record_t *results, uint16_t *count)
{
    if (!results || !count || *count == 0) return ESP_ERR_INVALID_ARG;

    // If in AP-only mode, temporarily switch to APSTA for scanning
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    bool was_ap_only = (mode == WIFI_MODE_AP);
    if (was_ap_only) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA),
                            TAG, "Switch to APSTA for scan failed");
    }

    /* scan_time 必须留空用驱动默认值：共存仲裁器激活时（我们这里是 802.15.4 /
       OpenThread 在跑，不是 BLE——ble_proxy_init() 是关的），驱动会拒绝自定义的
       active.min/max，扫描静默返回 0 个 AP，日志只有一行
       "Should use default active scan time parameter ... when Bluetooth is enabled"。
       曾写死 min=100/max=300，导致 ETH 做上行时（Thread BR 随之启动）整条
       "插网线用 WebUI 配 WiFi" 的路径拿不到任何 AP。见
       docs/test_cases_factory_provisioning.md 的 scan_time 附录。 */
    wifi_scan_config_t scan_cfg = {
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        /* .channel stays 0 so the bitmap is honoured (esp_wifi_types_generic.h).
           Skip 2.4 GHz entirely: a full-band scan let a concurrent connect pick the
           2.4 GHz twin of our SSID (see wifi_lock_5g). */
        .channel_bitmap = {
            .ghz_2_channels = WIFI_SCAN_BYPASS_BAND,
            .ghz_5_channels = WIFI_SCAN_5G_ALL,
        },
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        if (was_ap_only) esp_wifi_set_mode(WIFI_MODE_AP);
        return err;
    }

    err = esp_wifi_scan_get_ap_records(count, results);
    if (err != ESP_OK) {
        if (was_ap_only) esp_wifi_set_mode(WIFI_MODE_AP);
        return err;
    }

    // Filter: keep only 5GHz (channel > 30)
    uint16_t filtered = 0;
    for (uint16_t i = 0; i < *count; i++) {
        if (results[i].primary > 30) {
            if (filtered != i) {
                results[filtered] = results[i];
            }
            filtered++;
        }
    }
    *count = filtered;

    if (was_ap_only) {
        esp_wifi_set_mode(WIFI_MODE_AP);
    }

    ESP_LOGI(TAG, "Scan complete: %d 5GHz APs found", filtered);
    return ESP_OK;
}

// ---- Reconnect strategy (exponential backoff) ----

/* Full radio restart runs in its own short-lived task: esp_wifi_stop/start
   block for hundreds of ms, which must not stall the shared esp_timer task
   (S2 rule: no blocking calls in timer callbacks). */
static void wifi_radio_restart_task(void *arg)
{
    ESP_LOGW(TAG, "Full WiFi radio restart");
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_start();
    esp_err_t err = wifi_lock_5g("radio restart");
    if (err == ESP_OK) err = esp_wifi_connect();
    if (err != ESP_OK) {
        /* ⚠️ 必须重新排程。esp_wifi_connect() 返错时**不会产生
           WIFI_EVENT_STA_DISCONNECTED** —— 压根没发起过连接。而整条重试链
           全靠那个事件驱动（net_events.c 收到后转给状态机，状态机再调
           schedule_reconnect）。只打一条日志就返回 = 链条当场断死，设备
           永远停在 WIFI_RECONNECTING，连 SoftAP 兜底都轮不到。
           现场表现：路由器重启一次，WiFi 上联再也回不来（2026-08-21 定位）。
           返错是真实场景：驱动尚未就绪返 NOT_STARTED，或正在扫描时调用返
           ESP_ERR_WIFI_STATE（esp_wifi.h 的 attention 3）。 */
        ESP_LOGW(TAG, "esp_wifi_connect() 失败: %s —— 重新排程，否则重试链断死",
                 esp_err_to_name(err));
        if (s_reconnect_count >= WIFI_RECONNECT_MAX_RETRY) {
            wifi_mgr_schedule_background_retry();
        } else {
            wifi_mgr_schedule_reconnect();
        }
    }
    vTaskDelete(NULL);
}

static void reconnect_timer_cb(void *arg)
{
    /* Never yank the radio away while someone is provisioning through the
       SoftAP — a STA scan mid-session drops their connection. */
    if (s_softap_active) {
        wifi_sta_list_t sta_list;
        if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK && sta_list.num > 0) {
            ESP_LOGI(TAG, "SoftAP client present — postponing STA retry %dmin",
                     WIFI_PROVISIONING_RETRY_MS / 60000);
            esp_timer_start_once(s_reconnect_timer,
                                 (uint64_t)WIFI_PROVISIONING_RETRY_MS * 1000);
            return;
        }
    }

    ESP_LOGI(TAG, "Reconnect timer fired (attempt %d)", s_reconnect_count);

    /* On 3rd attempt, do a full WiFi radio restart to recover from
       stale PHY state after RTS soft-reset (flash + auto-reset). */
    if (s_reconnect_count == 3) {
        if (xTaskCreate(wifi_radio_restart_task, "wifi_restart", 3072,
                        NULL, 10, NULL) == pdPASS) {
            return;  /* the task connects when done */
        }
        ESP_LOGE(TAG, "radio restart task create failed, plain reconnect");
    }

    esp_err_t err = wifi_lock_5g("reconnect");
    if (err == ESP_OK) err = esp_wifi_connect();
    if (err != ESP_OK) {
        /* ⚠️ 必须重新排程。esp_wifi_connect() 返错时**不会产生
           WIFI_EVENT_STA_DISCONNECTED** —— 压根没发起过连接。而整条重试链
           全靠那个事件驱动（net_events.c 收到后转给状态机，状态机再调
           schedule_reconnect）。只打一条日志就返回 = 链条当场断死，设备
           永远停在 WIFI_RECONNECTING，连 SoftAP 兜底都轮不到。
           现场表现：路由器重启一次，WiFi 上联再也回不来（2026-08-21 定位）。
           返错是真实场景：驱动尚未就绪返 NOT_STARTED，或正在扫描时调用返
           ESP_ERR_WIFI_STATE（esp_wifi.h 的 attention 3）。5 GHz 锁没上成功同样不连、照样重排。 */
        ESP_LOGW(TAG, "esp_wifi_connect() 失败: %s —— 重新排程，否则重试链断死",
                 esp_err_to_name(err));
        if (s_reconnect_count >= WIFI_RECONNECT_MAX_RETRY) {
            wifi_mgr_schedule_background_retry();
        } else {
            wifi_mgr_schedule_reconnect();
        }
    }
}

/* esp_timer 任务：只投递，状态机在事件循环（sys_evt）里跑（T9.28）。
   以前这里直接调 network_state_machine —— 在优先级 22 的 esp_timer 任务里 portMAX_DELAY 等状态机的锁，
   状态机在这条路上还可能起 SoftAP（阻塞的 WiFi 驱动调用），等的那段全系统定时器都停。
   投不出去（事件队列满）就 50 ms 后再投；EXHAUSTED 只在 WIFI_RECONNECTING 里有动作，晚一点没有副作用。 */
static void exhausted_timer_cb(void *arg)
{
    (void)arg;
    if (network_mgr_defer_event(NET_EVT_WIFI_RECONNECT_EXHAUSTED) != ESP_OK) {
        (void)esp_timer_start_once(s_exhausted_timer, 50 * 1000);
    }
}

void wifi_mgr_schedule_reconnect(void)
{
    if (s_reconnect_count >= WIFI_RECONNECT_MAX_RETRY) {
        ESP_LOGW(TAG, "WiFi reconnect exhausted after %d attempts",
                 s_reconnect_count);
        /* Quiet the radio: without an explicit disconnect the driver keeps
           rescanning every ~2s forever ("Haven't to connect..."), starving
           the ch36 SoftAP beacon — phones never discover the hotspot.
           ⚠️ 即使不开 SoftAP 也要断：那个每 2s 的内部重扫会持续占用射频，
           而 802.15.4（Zigbee/Thread）跟它共存，代价是实打实的。 */
        esp_wifi_disconnect();
        /* 交给定时器回调去投递（不能在这里直接进状态机：本函数就是状态机里调的，会重入）。
           回调只投递、不拿锁（T9.28） */
        esp_timer_start_once(s_exhausted_timer, 1000); // 1ms deferred

        /* 没凭据 = 没有连接目标，重试毫无意义，直接不排定时器。
           状态机收到 EXHAUSTED 后会去开配网热点，等用户来填。
           （wifi_mgr_schedule_background_retry() 第一行就是同一个判断，
             2026-08-21 首版我在这里排了个 300s 去连一个不存在的 AP，
             与那边自相矛盾。） */
        if (!wifi_mgr_has_credentials()) return;

        /* 有凭据时的慢速间隔，判据是**配网热点在不在**：
           热点和 STA 共用一个射频，STA 一去扫描/连接，ch36 的信标就停发，
           手机搜不到热点、正在配网的人还会被踢掉。所以热点在就拉稀到 5 分钟。

             有凭据 + 热点在  → 用户正在配网（比如密码填错）→ 5 分钟
             有凭据 + 无热点  → 就是路由器不在，尽快重连     → 1 分钟

           ⚠️ 必须与 wifi_mgr_schedule_background_retry() **同口径**。
           以前这里写死 5 分钟，路由器重启后最坏要等 5 分钟才重连。 */
        uint64_t ms = s_softap_active ? WIFI_PROVISIONING_RETRY_MS
                                      : WIFI_BACKGROUND_RETRY_MS;
        esp_timer_start_once(s_reconnect_timer, ms * 1000);
        return;
    }

    uint32_t delay_ms = WIFI_RECONNECT_BASE_MS * (1 << s_reconnect_count);
    if (delay_ms > WIFI_RECONNECT_MAX_MS) delay_ms = WIFI_RECONNECT_MAX_MS;

    s_reconnect_count++;
    ESP_LOGI(TAG, "WiFi reconnect attempt %d in %"PRIu32"ms",
             s_reconnect_count, delay_ms);

    esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000);
}

void wifi_mgr_reset_reconnect(void)
{
    s_reconnect_count = 0;
    esp_timer_stop(s_reconnect_timer);
}

/* Re-arm the slow background retry. Used by the state machine when a
   background reconnect attempt fails in SOFTAP_ONLY / ETH_ONLINE — without
   this the 60s retry chain dies after its first failed attempt. */
void wifi_mgr_schedule_background_retry(void)
{
    if (!wifi_mgr_has_credentials()) return;
    if (esp_timer_is_active(s_reconnect_timer)) return;
    /* Stop the driver's internal rescan loop between attempts (see above —
       the radio must sit on ch36 so the provisioning AP stays visible). */
    esp_wifi_disconnect();
    uint64_t ms = s_softap_active ? WIFI_PROVISIONING_RETRY_MS
                                  : WIFI_BACKGROUND_RETRY_MS;
    esp_timer_start_once(s_reconnect_timer, ms * 1000);
}

// ---- Credential update (delegated to config_mgr by network_mgr_set_wifi) ----
// wifi_mgr_set_credentials removed — use config_mgr_set_wifi() instead.
// wifi_mgr only reads credentials via config_mgr_get_wifi().

// ---- Accessors ----

esp_netif_t *wifi_mgr_get_sta_netif(void)  { return s_sta_netif; }
esp_netif_t *wifi_mgr_get_ap_netif(void)   { return s_ap_netif; }

/* 事件驱动，不查驱动。原来每次都调 esp_wifi_sta_get_ap_info()，而调用方
   network_mgr_get_status() 被 ws_push_task 每 2s 调一次 —— STA 已配置但未连上时
   （凭据指向的 AP 不存在、正在重连）驱动每次都打一条
   `wifi: Haven't to connect to a suitable AP now!`，两次查询就是两条，
   2 秒一对刷满串口。而且这是个阻塞 ioctl，不该出现在周期性快照路径里。 */
void wifi_mgr_set_sta_connected(bool connected) { s_sta_connected = connected; }

bool wifi_mgr_is_connected(void) { return s_sta_connected; }

bool wifi_mgr_softap_active(void) { return s_softap_active; }

int  wifi_mgr_last_disconnect_reason(void) { return s_last_disc_reason; }

/* ---- 链路统计（见 network_mgr.h 的 wifi_link_stats_t）----
   单写者：系统事件任务（net_events.c 的两个 handler）。读者在 httpd 任务里，
   全是 32 位字，RISC-V 上对齐的 32 位读写是原子的，不加锁。 */
static volatile uint32_t s_disc_count       = 0;
static volatile uint32_t s_outages_done     = 0;
static volatile uint32_t s_down_since_s     = 0;   /* 0 = 不在断线中 */
static volatile uint32_t s_last_outage_s    = 0;
static volatile uint32_t s_longest_outage_s = 0;

static uint32_t uptime_s(void)
{
    uint32_t t = (uint32_t)(esp_timer_get_time() / 1000000);
    return t ? t : 1;               /* 0 留给"不在断线中" */
}

void wifi_mgr_note_link_lost(void)
{
    /* 连不上时每次重试失败都会再来一条 DISCONNECTED —— 那时 s_sta_connected 已是
       false，或这次断线已在计时，都不是新的一次。 */
    if (!s_sta_connected || s_down_since_s) return;
    s_disc_count++;
    s_down_since_s = uptime_s();
}

void wifi_mgr_note_link_restored(void)
{
    uint32_t since = s_down_since_s;
    if (!since) return;             /* 开机第一次拿 IP，不是"恢复" */
    uint32_t d = uptime_s() - since;
    s_last_outage_s = d;
    if (d > s_longest_outage_s) s_longest_outage_s = d;
    s_outages_done++;
    s_down_since_s = 0;
}

void wifi_mgr_link_stats(wifi_link_stats_t *out)
{
    if (!out) return;
    uint32_t since = s_down_since_s;
    out->disconnects      = s_disc_count;
    out->outages_done     = s_outages_done;
    out->down_s           = since ? uptime_s() - since : 0;
    out->last_outage_s    = s_last_outage_s;
    out->longest_outage_s = s_longest_outage_s;
}
void wifi_mgr_set_disconnect_reason(int r) { s_last_disc_reason = r; }
