#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif_types.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------- Constants ----------------

/* 快速重连次数。2026-08-21 从 5 提到 7。
 *
 * 理由：**路由器重启通常要 30-90 秒**，而 5 次只覆盖 62 秒，往往还没等到 AP
 * 回来就判定耗尽、掉进慢速路径，用户体感是"WiFi 掉了要等好久"。
 * 7 次覆盖到 182 秒（3 分钟），让路由器重启这种最常见的场景**在快速阶段就接上**，
 * 压根走不到慢速重试。
 *
 * 退避序列（BASE 2s 翻倍，上限 MAX 60s）：
 *     2s   4s   8s   16s   32s   60s   60s
 *   累计 2   6   14   30    62   122   182 秒
 *
 * 比缩短慢速间隔更对症：慢速间隔要顾及 WiFi 扫描与 802.15.4（Zigbee/Thread）
 * 共用射频，调密了会干扰 Zigbee；而快速阶段本来就在扫，多试两次不额外扰动。
 *
 * ⚠️ 别把这个数调太大：延时算的是 BASE << count，count 到 22 时
 * uint32_t 就溢出了（虽然有 MAX_MS 夹住，溢出发生在夹之前）。 */
#define WIFI_RECONNECT_MAX_RETRY   7
#define WIFI_RECONNECT_BASE_MS     2000
#define WIFI_RECONNECT_MAX_MS      60000
#define WIFI_BACKGROUND_RETRY_MS   60000
/* Retry cadence while the provisioning SoftAP is up. STA scans and the ch36
   AP share one radio: scanning starves the AP beacon and phones can't see
   the hotspot. Keep the radio parked on ch36 most of the time. */
#define WIFI_PROVISIONING_RETRY_MS 300000

// ---------------- State & Interface Enums ----------------

typedef enum {
    NET_STATE_INIT,
    NET_STATE_SOFTAP_ONLY,
    NET_STATE_ETH_CONNECTING,
    NET_STATE_ETH_ONLINE,
    NET_STATE_WIFI_CONNECTING,
    NET_STATE_WIFI_ONLINE,
    NET_STATE_WIFI_RECONNECTING,
    NET_STATE_BOTH_ONLINE,
    /* 交互式配网失败的终点。与 NO_NETWORK 的区别：这里**刻意不再重试** ——
       用户正坐在配网页面前等答案，静默重试只会让他一直等不到结论。
       只有用户重新提交凭据才会离开这个状态。 */
    NET_STATE_WIFI_PROVISION_FAILED,
    NET_STATE_NO_NETWORK,
} net_state_t;

typedef enum {
    NET_IF_NONE   = 0,
    NET_IF_ETH    = 1,
    NET_IF_WIFI   = 2,
    NET_IF_SOFTAP = 3,
} net_interface_t;

// ---------------- Internal Events (state machine inputs) ----------------

typedef enum {
    NET_EVT_WIFI_CONNECTED,
    NET_EVT_WIFI_DISCONNECTED,
    NET_EVT_WIFI_GOT_IP,
    NET_EVT_WIFI_LOST_IP,
    NET_EVT_WIFI_RECONNECT_EXHAUSTED,
    NET_EVT_ETH_LINK_UP,
    NET_EVT_ETH_LINK_DOWN,
    NET_EVT_ETH_GOT_IP,
    NET_EVT_WIFI_CREDENTIALS_SET,
} net_event_t;

// ---------------- Public Events ----------------

ESP_EVENT_DECLARE_BASE(NETWORK_EVENT);

typedef enum {
    NETWORK_EVENT_ONLINE,
    NETWORK_EVENT_OFFLINE,
    /* 2026-09-13 起方向有语义（T2.16）：
       FAILOVER  = 主链路（以太网）掉了、退到 WiFi —— 这才是告警；
       RECOVERED = 之前退过、现在以太网回来接管。"退过"= 本次开机运行中真的从以太网退到过 WiFi
                   （发过 FAILOVER），或网线断着时开机、以太网服务 IP 真的被接管到了 WiFi 上
                   （T9.20，开机不补发 FAILOVER；只是以太网缺席而没接管成的开机不算）。
       开机时 WiFi 先拿到地址、以太网随后接管 **不发 FAILOVER / RECOVERED**：此前那条 FAILOVER
       每次开机必发一次，是假告警。这种情况（含运行中先 WiFi 后插网线）发一个 ONLINE(ETH)，
       让 Thread 边界路由和 MQTT 客户端跟到网线上（2026-10-07 T9.37 / 10-08 T9.21：跟随与告警解耦，
       跟随方一律也听 ONLINE、按"现在的活动网口"自己判要不要换）。data 都是 net_interface_t 新接口。 */
    NETWORK_EVENT_FAILOVER,
    NETWORK_EVENT_RECOVERED,
    NETWORK_EVENT_STATE_CHANGED,
    NETWORK_EVENT_WIFI_CREDENTIALS_SET,
    NETWORK_EVENT_SOFTAP_STARTED,
    NETWORK_EVENT_SOFTAP_STOPPED,
} network_event_id_t;

// ---------------- Status Struct ----------------

typedef struct {
    net_state_t     state;
    net_interface_t active_interface;
    // ETH
    bool            eth_link_up;
    bool            eth_has_ip;
    esp_ip4_addr_t  eth_ip;
    esp_ip4_addr_t  eth_gateway;
    esp_ip4_addr_t  eth_netmask;
    // WiFi STA
    bool            wifi_connected;
    bool            wifi_has_ip;
    esp_ip4_addr_t  wifi_ip;
    int8_t          wifi_rssi;
    char            wifi_ssid[33];
    uint8_t         wifi_channel;
    // SoftAP
    bool            softap_active;
    uint8_t         softap_clients;
    char            softap_ssid[33];
} network_status_t;

// ---------------- Public API ----------------

esp_err_t       network_mgr_init(void);
net_state_t     network_mgr_get_state(void);
// 活动接口从 network_mgr_get_status() 的 status.active_interface 读，不再有独立 getter。

/* 取当前活动接口的**底层网卡名**（lwip 的 if_name，如 "en1" / "st1"），
 * 写进 out（至少 8 字节）。没有活动接口时写空串并返回 ESP_ERR_INVALID_STATE。
 *
 * ── 为什么需要它（T3.8 第三条）──────────────────────────────────────
 *
 * 网关对外**主动发起**的连接（MQTT、webhook、云端更新）默认不指定出口，由 lwip
 * 选路。而双上行且两口**同网段**时，lwip 对目的地在本网段的包按 netif 链表顺序
 * 挑出口，**不看** esp_netif_set_default_netif() 设的默认口 —— 于是
 * active_interface=eth 时，对端看到的来源 IP 仍是 WiFi 那个。
 *
 * 后果不是功能坏掉（连接照样建得起来），而是：按来源 IP 做 ACL 的用户放行了
 * 以太网地址却连不上；排查时网关自报以太网在用而日志里是另一个 IP；WiFi 一断
 * 这些外发连接跟着断，尽管以太网好好的。
 *
 * 用法：把它填进 esp_http_client_config_t.if_name /
 *       esp_mqtt_client_config_t.network.if_name。
 * ⚠️ 只对**本网段**目的地有区别；跨网段本来就按默认路由走，填了也一样。
 * ⚠️ 拿到的名字只在当前这次连接有效 —— 故障切换后要重连才会换出口。 */
esp_err_t network_mgr_active_ifname(char *out, size_t out_len);
esp_err_t       network_mgr_get_status(network_status_t *out);
esp_err_t       network_mgr_set_wifi(const char *ssid, const char *password);
esp_err_t       network_mgr_scan_wifi(wifi_ap_record_t *results, uint16_t *count);
esp_err_t       network_mgr_set_eth_ip(const esp_netif_ip_info_t *ip_info);
/* 最近一次 WiFi STA 断连的 wifi_err_reason_t（0 = 从未断过）。
   配网页面靠它区分"密码错误"和"找不到该网络" —— 只打日志用户只能盲改。
   wifi_mgr.h 是 network_mgr 的私有头，webui 不该直接包，故在此转出。 */
int             network_mgr_wifi_last_reason(void);

/* WiFi STA 链路统计（本次开机以来，重启清零）。给"5GHz 回传断了多久、断了几次"
 * 一个可查的答案 —— 此前只有一条 ESP_LOGW，事后无从知道断线有多频繁、每次多长。
 *
 * ⚠️ 只数"连着 → 断开"的转变。连不上时驱动每次重试失败都会再发一条
 *    STA_DISCONNECTED，那是同一次断线的延续，不另计。
 * ⚠️ 断线时长从断开算到**重新拿到 IP**（不是重新关联）—— 上位机能重连的时点是有 IP。
 * ⚠️ 人为造成的断开（改 WiFi 设置、配网切换）也算一次；它统计的是链路事实，不分原因。
 * 非阻塞：只读几个 32 位字（单写者是系统事件任务），任何任务里都能调。 */
typedef struct {
    uint32_t disconnects;       /* 连着 → 断开 的次数 */
    uint32_t outages_done;      /* 已经恢复的断线次数；为 0 时下面两项无意义 */
    uint32_t down_s;            /* 当前这次断线已持续的秒数；在线时为 0 */
    uint32_t last_outage_s;     /* 上一次已恢复的断线持续了多久 */
    uint32_t longest_outage_s;  /* 本次开机以来最长的一次 */
} wifi_link_stats_t;
void            network_mgr_wifi_link_stats(wifi_link_stats_t *out);

/* 5GHz 的 DFS 信道（UNII-2A 52–64、UNII-2C 100–144）。路由器在这些信道上检测到
   雷达必须立刻换信道，且新信道要先静听 60 s（部分地区气象雷达信道 10 分钟）才能
   发射 —— 期间所有客户端掉线。网关只能跟随路由器，所以只做提示，不做规避。 */
static inline bool network_mgr_wifi_channel_is_dfs(uint8_t ch)
{
    return ch >= 52 && ch <= 144;
}
/* 交互式配网的进行状态，供 /api/v1/status 暴露给配网页面。 */
typedef enum { PROV_IDLE = 0, PROV_CONNECTING, PROV_OK, PROV_FAILED } prov_state_t;
prov_state_t    network_mgr_provision_state(void);
esp_err_t       network_mgr_start_softap(void);
esp_err_t       network_mgr_stop_softap(void);
/* 配网 AP 是否正在广播。非阻塞、只读一个标志——给需要避让射频的模块
   （thread_br）在事件回调里查，不要为此去调 network_mgr_get_status()，
   那个会顺带调用阻塞的 WiFi API。 */
bool            network_mgr_softap_active(void);
/* STA 是否已关联（不是"有 IP"）。事件驱动的标志位，不查驱动 —— 周期性代码
   要判断能不能问 WiFi 要 RSSI/SSID 时用它，别直接 esp_wifi_sta_get_ap_info()。 */
bool            network_mgr_sta_connected(void);

/* T9.20：SNTP 对上时后由 main.c 的同步回调调用（SNTP 的回调槽全系统只有一个，只在 main.c 注册）。
   运行在 lwIP 线程：本函数不阻塞、不拿锁。开机接管靠它知道"墙钟这次开机对过"。 */
void            network_mgr_notify_time_synced(void);
/* T9.20：main.c 遇到非干净复位（死机 / 看门狗 / 欠压…）、补那次 esp_restart 之前调用，可以在
   network_mgr_init 之前调（只写一个 RTC_NOINIT 字）。补的那次重启复位原因是 ESP_RST_SW，但墙钟
   带着 RC 慢时钟的漂移，开机接管不能把它当成"继承来的可信墙钟"。 */
void            network_mgr_note_unclean_reset(void);
/* T9.20：开机接管以太网服务 IP 的状态短名，给 /api/v1/status 的 svc_takeover 字段（字符串常量）。
   这是**本次开机**开机接管的结论，不是"此刻浮没浮在 WiFi 上"：
     off            编译时关（CONFIG_BITENGINE_WIFI_FLOAT_BOOT_TAKEOVER=n）
     none           没有候选（以太网静态 IP、从没存过快照、刚恢复出厂），或在等待 / 探测期间以太网先拿到了地址
     waiting / wait_clock / probing   还在判（wait_clock = 等对时）
     taken          接管成立；网线回来、地址收回以太网之后仍报 taken
     denied_<原因>  本次开机不接管（原因名同 svc_policy.h 的 svc_verdict_name），网线回来后仍保留
   条件与行为的正本是 Kconfig help（BITENGINE_WIFI_FLOAT_BOOT_TAKEOVER）。 */
const char     *network_mgr_svc_takeover_state(void);

/* T9.37 服务身份（/api/v1/status 的四个字段；设计 docs/16，行为正本是 Kconfig BITENGINE_SVC_IDENTITY 的 help）：
     svc_identity  服务身份 S 属于哪张卡："eth" / "wifi"；"undecided" = 首次安装还没定；"off" = 本次开机不用身份机制
                   （编译时关、没有以太网、任一张卡静态 IP）
     svc_holder    此刻谁持有服务地址："eth" / "wifi"；"pending" = 开机等网口链路；
                   "wifi_pinned" = 退化了（路由器只认 MAC / 本次开机判不了）且 S 是 WiFi：钉在 WiFi 上，插着网线流量也走 WiFi；
                   "off" = 身份机制没在管（S 是网口时退化、静态 IP、没有以太网）
     svc_id_mode   路由器按什么认租约："client_id"（交换着拿到过服务地址）/ "mac_only"（退回旧行为）/ "unknown"（还没交换过）/ "off"
   network_mgr_svc_ip：服务地址（网络序），HA / ZHA / Z2M 该连的那个；身份机制关着时是旧规则下的以太网服务 IP；0 = 还没有。
   httpd 无锁读，最坏晚一拍。 */
const char     *network_mgr_svc_identity(void);
const char     *network_mgr_svc_holder(void);
const char     *network_mgr_svc_id_mode(void);
uint32_t        network_mgr_svc_ip(void);
#if CONFIG_BITENGINE_DIAG_SVC_ID
/* 诊断（Kconfig 默认关）：把服务身份记录改成 s（"eth" / "wifi"），服务地址置为"还不知道"，交给 NVS 写，重启后生效。
   返回 ESP_ERR_INVALID_ARG（s 不认识）/ ESP_ERR_INVALID_STATE（本次开机身份机制没在管）。 */
esp_err_t       network_mgr_diag_svc_id_set(const char *s);
/* 诊断：两张卡的 DHCP 状态写成 JSON（{"eth":{…},"wifi":{…}}）——地址、lwIP 状态、租期、距上次 ACK、T1 / T2、
   身份表与实际发出的身份、续租 / 重绑请求数。长时间续租测试（测试 6e R5）用：续租成功 = used_s 在 T1 附近归零、地址不变、
   重绑数不涨。会同步等 lwIP 线程读一次（几毫秒）。返回写入的字节数；放不下 / 读不了返回 -1。 */
int             network_mgr_diag_dhcp_json(char *buf, size_t len);
#endif

// State machine entry (called by net_events.c)
void network_state_machine(net_event_t event);

/* 组件内部用（wifi_mgr 的 esp_timer 回调）：把状态机事件投给事件循环，由它去跑 network_state_machine。
   esp_timer 回调里不许直接调 network_state_machine —— 它要等状态机的锁（T9.28）。
   不阻塞；事件队列满时返回非 ESP_OK，调用方自己重排定时器再投。 */
esp_err_t network_mgr_defer_event(net_event_t event);

/* 看门狗 L3 恢复入口：重新触发状态机的重连/回退流程 */
esp_err_t network_mgr_recover(void);

#ifdef __cplusplus
}
#endif
