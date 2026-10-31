#include "network_mgr.h"
#include "watchdog.h"
#include "wifi_mgr.h"
#include "eth_mgr.h"
#include "config_mgr.h"

#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "psram_task.h"
#if CONFIG_BITENGINE_WIFI_FLOAT_ETH_IP
#include "esp_netif_net_stack.h"   /* esp_netif_get_netif_impl */
#include "lwip/tcpip.h"            /* tcpip_try_callback */
#include "lwip/etharp.h"           /* etharp_gratuitous / etharp_query / etharp_find_addr */
#include "lwip/def.h"              /* lwip_ntohl / lwip_htonl */
#include "svc_policy.h"            /* svc_same_subnet（运行中浮动也用） */
#endif
#if CONFIG_BITENGINE_MDNS_SVC_IP_ONLY
#include "mdns.h"                  /* mdns_netif_action：本机地址变了触发一次宣告（T9.34） */
#endif
#if CONFIG_BITENGINE_SVC_IDENTITY
#include "svc_cid.h"               /* 每张卡的 DHCP client-id（T9.37） */
#endif
#if CONFIG_BITENGINE_WIFI_FLOAT_BOOT_TAKEOVER
#include <time.h>
#include "esp_attr.h"              /* RTC_NOINIT_ATTR */
#include "esp_system.h"            /* esp_reset_reason */
#include "nvs.h"
#include "lwip/dhcp.h"             /* netif_dhcp_data / dhcp_supplied_address / DHCP_COARSE_TIMER_SECS */
#endif

static int s_wd_slot_net_status = -1;

static const char *TAG = "network_mgr";

ESP_EVENT_DEFINE_BASE(NETWORK_EVENT);

// Defined in net_events.c
extern esp_err_t net_events_register(void);

// ---- Forward declarations ----
static const char *state_name(net_state_t s);
static void post_online(net_interface_t iface);
static void post_offline(void);
static net_state_t no_network_fallback(void);
#if CONFIG_BITENGINE_MDNS_SVC_IP_ONLY
static void mdns_self_update(void);
#endif
#if CONFIG_BITENGINE_SVC_IDENTITY
/* T9.37 服务身份（实现在 T9.20 那段之后；设计 docs/16）。svc_id_on() 为真时服务地址由身份机制管（含"钉在 WiFi 上"），
   T2.23 的静态浮动与 T9.20 的开机接管都不动 —— 它们只在 S = 网口时退化、或静态 IP 时用。 */
static bool svc_id_on(void);
static void svc_id_update(void);
static void svc_id_on_got_ip(svc_nic_t nic);
static void svc_id_store(void);
static void svc_id_mac_only_stamp(void);
static void svc_id_on_time_synced(void);
#else
static inline bool svc_id_on(void) { return false; }
static inline void svc_id_on_time_synced(void) {}
#endif

// ---- Module state ----
static net_state_t       s_state     = NET_STATE_INIT;
static net_interface_t   s_active_if = NET_IF_NONE;
/* 本次开机是否真的从以太网退到过 WiFi（含 T9.20 开机接管成立）。只有退过，以太网回来才算
   RECOVERED；开机时"WiFi 先拿到地址、以太网随后接管"不是恢复，也不是 failover（T2.16）。
   事件语义的正本是 network_mgr.h 的 NETWORK_EVENT_RECOVERED。 */
static bool              s_failed_over = false;
static bool              s_softap_on = false;
/* 本次进入 WIFI_CONNECTING 是不是"用户提交凭据"引起的。
   WIFI_CONNECTING 有 6 个入口（开机/配网/以太网回退…），只有配网这一路
   是**用户正在等答案**，失败处理完全不同：快速试满就报错，不进后台重连。 */
static bool              s_provisioning = false;
static int               s_prov_attempts = 0;
static prov_state_t      s_prov_state = PROV_IDLE;
#define PROV_MAX_ATTEMPTS 3

/* 配网前的旧凭据快照。新凭据连不上时退回它，否则**远程填错一次密码 =
   设备永久失联**：旧凭据被覆盖、有凭据不开热点、失败又不重试，而 PoE 部署
   之外没有第二条路进得去。首次配网没有可回滚的对象，那时设备本来就在
   热点模式，用户能重来。 */
static char  s_prev_ssid[SSID_MAX_LEN];
static char  s_prev_pass[PASSWORD_MAX_LEN];
static bool  s_prev_valid = false;

/* T9.40：网口在线时配网失败、又没有可退的旧凭据（先插网线装的设备第一次配 WiFi 就填错）——
   语义同 WIFI_PROVISION_FAILED 的"不重试、等用户重填"，但主状态留在 ETH_ONLINE（网关是在线的）。
   下一次 CREDENTIALS_SET 清掉。 */
static bool  s_wifi_prov_hold = false;

/* T9.40：网口在线时配网失败、刚退回旧凭据 —— 接下来的断开按流程 B 的快速重连连回（同 WIFI_CONNECTING），
   不走 ETH_ONLINE 平时的后台重试：后者第一步就 esp_wifi_disconnect()，会打断回滚刚发起的连接，
   旧网络要白等 60 s（2026-10-08 真板）。快速重连耗尽（wifi_mgr 自己转慢速）、连上、重填凭据、网口断链时清。 */
static bool  s_wifi_fast_after_rollback = false;

static bool              s_wifi_has_ip = false;
static bool              s_eth_has_ip  = false;
#if CONFIG_BITENGINE_WIFI_FLOAT_ETH_IP
/* 浮动服务 IP（T2.23）：以太网链路在不在、以太网地址快照(=服务IP)、当前是否浮在 WiFi 口。*/
static bool                 s_eth_link_up  = false;
#if CONFIG_BITENGINE_MDNS_SVC_IP_ONLY
static int64_t              s_eth_link_up_us;         /* 以太网链路最近一次起来的时刻：本机 mDNS 地址的"等 DHCP"计时（T9.34） */
#endif
static esp_netif_ip_info_t  s_svc_ip;
static bool                 s_svc_ip_valid = false;
static volatile bool        s_svc_on_wifi  = false;   /* 也被 GARP 定时器回调读 */
static uint32_t             s_svc_subnet_warned;      /* 网段不符的 W 日志每个服务 IP 只打一次 */
/* WiFi 自己的寻址快照（没浮时记下）：地址、DNS、是不是静态。三处用：浮动前比网段；浮动时全局 DNS
   已被清空（刚收回、WiFi 的 DHCP 还没重新绑定）就还它的 DNS；收回 / 浮动失败时按它还原 —— 不重读配置：
   "已保存、重启才生效"的静态 IP 不能提前下发（2026-10-06 T9.20 复核）。
   刷新：WiFi 拿到自己的地址时（状态机 WIFI_GOT_IP 且没浮）和每次浮动前；作废：WiFi 断线 / 丢地址 / 换凭据。 */
typedef struct {
    bool                 valid;
    bool                 is_static;   /* dhcpc 是 STOPPED：WiFi 用的是静态地址 */
    esp_netif_ip_info_t  ip;
    esp_netif_dns_info_t dns_main, dns_bak;
} wifi_own_t;
static wifi_own_t           s_wifi_own;
/* 服务 IP 属于哪个 WiFi 网络：拍快照 / 开机接管时配置的 SSID 指纹（svc_ssid_hash；0 = 当时没配 WiFi，不比）。
   以太网缺席时用户换了 WiFi，新网络里这个地址可能属于别人 —— 只有当前 SSID 对得上才浮；重新提交同一个
   SSID、换新 WiFi 失败自动回滚到原凭据，都会自己浮回来（2026-10-06 T9.20 复核：一刀切作废会误伤这两种）。 */
static uint32_t             s_svc_ssid;
#endif

#if CONFIG_BITENGINE_WIFI_FLOAT_BOOT_TAKEOVER
/* ---- T9.20：网线断着时重启，开机把以太网服务 IP 接管到 WiFi（2026-10-06）----
 *
 * 行为说明的正本是 Kconfig help（BITENGINE_WIFI_FLOAT_BOOT_TAKEOVER），判据的正本是 svc_policy.c。
 * 这里只做三件事：以太网有 DHCP 租约时把"地址 + 到期墙钟"存进 NVS；开机时读回来当候选；
 * 条件满足（以太网缺席过了宽限、WiFi 有自己的地址、同网段、同一个 WiFi、租期没过）就 ARP 探测，
 * 没人占用才把候选交给运行中浮动（float_svc_update）去挂。
 *
 * 上下文（S2）：
 *   - 闸门只在 sys_evt 里推进、都持 s_sm_mutex：状态机每次跑完推一次；svc_work_cb（esp_timer 任务）
 *     被唤醒时**不拿锁**，投递 NET_DEFER_SVC_GATE，由 svc_gate_work 在事件循环里推（T9.28）。
 *     真正挂地址（float_svc_update，要同步等 lwIP 线程好几次 IPC）也在 svc_gate_work 里做。
 *     esp_timer 回调里不同步等 lwIP 线程、也不等状态机的锁（2026-10-05 审计与 T9.28 定下的先例）；
 *   - *_in_tcpip 跑在 lwIP 线程：只读 lwIP 内部结构、发 ARP、把结果写进 s_svc_mux 保护的小结构，
 *     再 svc_kick() 唤醒 svc_work_cb。**绝不拿 s_sm_mutex**：持锁的状态机正同步等 lwIP 线程处理
 *     esp_netif IPC（本构建没开 LWIP_TCPIP_CORE_LOCKING），两边互等就死锁；
 *   - NVS 只在 svc_persist_step（esp_timer 任务，内部栈）里写，且不持 s_sm_mutex。
 *     status_log_task 的栈在 PSRAM，写 flash 会关 cache → 栈不可用 → 崩，它的调用链不许碰 NVS。
 *
 * ⚠️ 不用 lwIP 的 acd_*：本构建（CONFIG_LWIP_DHCP_DOES_ARP_CHECK=y）编进去的是 ESP 给 DHCP 专用的
 *    替身 port/acd_dhcp_check.c —— acd_start 忽略地址参数、探测的永远是 DHCP 正在申请的那个地址，
 *    还会改 dhcp->tries；acd_stop/acd_tmr 根本没实现（链接失败）；在 DHCP 处于 CHECKING 时调到
 *    acd_remove 会让 DHCP 永远卡住（dhcp_timeout 没有 CHECKING 分支）。探测用 etharp_query +
 *    etharp_find_addr 自己做，见 probe_start_in_tcpip。 */
#define SVC_NVS_NS            "net_svc"   /* ≤15 字符；恢复出厂会擦（config_mgr.c 的 OWNED_NS） */
#define SVC_NVS_KEY           "rec"
#define SVC_BOOT_GRACE_MS     20000       /* 开机宽限：主部署插着网线重启时，WiFi 常比以太网先拿到地址，
                                             DM9051 协商 + DHCP 一般几秒；宽限内网线一出现就让路 */
#define SVC_PROBE_MS          3000        /* etharp_query 立刻发一次，etharp_tmr 每秒对 PENDING 条目补发
                                             （ARP_MAXPENDING=5 之后才释放），3 s 内约 3 次请求 */
#define SVC_PROBE_MIN_SENT_MS 2000        /* 到点时距"第一次请求真的发出去"不足这么久，结论不算数 */
#define SVC_PROBE_RETRY_US    (5LL * 1000 * 1000)     /* 没探成（WiFi 不在 / 请求没发出去）后隔多久再来 */
#define SVC_LEASE_POLL_US     (300LL * 1000 * 1000)   /* 以太网有租约时每 5 分钟读一次剩余租期 */
#define SVC_LEASE_MARGIN_S    300         /* 离到期不足 5 分钟也算过期：吸收绑定比 ACK 晚约 1 s、
                                             coarse tick 相位、软重启时 RTC 的漂移 */
#define SVC_CLOCK_TRUSTED     0x7C10C0DEu /* s_clock_trust 的"本次开机墙钟可信"魔数 */

static svc_rec_t            s_bt_rec;                 /* 开机时从 NVS 读到的候选（读完不再改） */
static svc_bt_state_t       s_bt_state   = SVC_BT_NONE;
static svc_verdict_t        s_bt_verdict = SVC_DENY_NO_RECORD;   /* 最近一次结论：/status、日志去重 */
static uint32_t             s_bt_gen;                 /* 探测代号：每次发起 / 作废都 +1，晚到的结果对不上就丢 */
static bool                 s_eth_ip_seen;            /* 以太网本次开机拿到过地址 → 实时快照接管，开机接管退场 */
static int64_t              s_init_us;
static esp_reset_reason_t   s_reset_reason;
static volatile bool        s_time_synced;            /* SNTP 本次开机对上过时（main.c 的回调通知） */
/* 墙钟的"出处"跨软复位传下去（RTC_NOINIT：软复位保留、上电清成随机值 → 魔数对不上 = 不可信）。
   为什么不能只看本次复位原因（2026-10-06 审计 R2-01）：main.c 遇到死机 / 看门狗这类非干净复位，
   会先补一次 esp_restart 再正常启动 —— 第二次启动的复位原因就成了 ESP_RST_SW，而墙钟带着上次上电
   以来 RC 慢时钟（CONFIG_RTC_CLK_SRC_INT_RC）的累计漂移（只有 esp_restart 的关机钩子会把 RTC 对齐，
   panic 不走它）。所以：main.c 在补那次重启之前调 network_mgr_note_unclean_reset() 清掉它；
   SNTP 对上时置可信；ESP_RST_SW 且上次可信才继承。 */
static RTC_NOINIT_ATTR uint32_t s_clock_trust;
static bool                 s_clock_inherited;        /* 本次开机从上次继承到了可信墙钟 */
static esp_timer_handle_t   s_svc_work_timer;         /* 一次性：消化探测结果 / 租约读数、推进闸门 */
static esp_timer_handle_t   s_svc_probe_timer;        /* 一次性：探测窗口到点 */
static esp_timer_handle_t   s_svc_lease_timer;        /* 周期：读以太网剩余租期 */
static ip4_addr_t           s_probe_ip;               /* 探测目标（网络序）；发起前写好，候选固定不变 */
static bool                 s_svc_reconcile_pending;  /* 接管成立、还没挂上地址（svc_gate_work 里置、同一处消费） */

/* lwIP 线程写、esp_timer 任务读：s_svc_mux 保护（单核上就是关中断的临界区，只做拷贝） */
static portMUX_TYPE s_svc_mux = portMUX_INITIALIZER_UNLOCKED;
typedef struct {
    uint32_t      gen;
    bool          sent;         /* probe_start_in_tcpip 真的把第一个 ARP 请求交给了驱动 */
    int64_t       sent_us;      /* 那一刻（esp_timer_get_time） */
    bool          done;
    bool          conclusive;   /* false = 到点时 WiFi 不在 / 请求没发出去 / 窗口不够长，没探成 */
    svc_verdict_t verdict;      /* SVC_OK 或 SVC_DENY_CONFLICT */
    uint8_t       mac[6];       /* 冲突时占用者的 MAC */
} svc_probe_res_t;
static svc_probe_res_t s_probe_res;
typedef struct {
    uint32_t seq;
    bool     ok;                /* 以太网此刻有 DHCP 租约（BOUND/RENEWING/REBINDING） */
    bool     infinite;
    uint32_t ip, mask, gw;      /* 主机序 */
    uint32_t lease_s;           /* 整条租约（秒） */
    uint32_t remaining_s;       /* 读数时刻的剩余（秒） */
    int64_t  mono_us;           /* 读数时刻（esp_timer_get_time） */
} svc_lease_rd_t;
static svc_lease_rd_t s_lease_rd;

static uint32_t  s_lease_consumed;    /* 以下四个只在 esp_timer 任务（svc_persist_step）里用 */
static svc_rec_t s_rec_cache;         /* 上次写进 NVS 的那条 */
static bool      s_rec_cache_ok;
static bool      s_rec_reject_logged; /* 读到的租约形态不认识（svc_rec_valid 拒）：每次开机只打一次 W */
#endif
static esp_timer_handle_t s_eth_dhcp_timer = NULL;
/* 状态机从事件循环（sys_evt）、httpd（改 WiFi）、wd_action（看门狗恢复）进来 —— 串行化。
   ⚠️ esp_timer 回调里**不许**拿它（T9.28，2026-10-07）：esp_timer 任务优先级 22，回调 portMAX_DELAY
   等锁时持锁的一方被优先级继承抬到 22，等的那段时间全系统的定时器都停。回调只投递 NET_DEFER_EVENT，
   活在 defer_handler（sys_evt）里做。check_timer_cbs.py 逐个回调钉着（VERIFY.sh）。 */
static SemaphoreHandle_t  s_sm_mutex = NULL;

/* 私有：esp_timer 回调把状态机的活交给事件循环。别处别订，也别从组件外投。 */
ESP_EVENT_DEFINE_BASE(NET_DEFER_EVENT);
enum {
    NET_DEFER_ETH_DHCP_TIMEOUT,     /* data: uint32_t 代号（见 s_eth_dhcp_gen） */
    NET_DEFER_SM_EVENT,             /* data: net_event_t，交给 network_state_machine */
    NET_DEFER_SVC_GATE,             /* 推进开机接管闸门（T9.20） */
    NET_DEFER_MDNS_RECHECK,         /* 本机 mDNS 地址到点复查（T9.34：等 DHCP / 开机等待到期） */
    NET_DEFER_SVC_ID,               /* 服务身份到点复查（T9.37：开机宽限 / 网口 DHCP 超时） */
};
/* 投递失败（默认事件队列 32 条满了）时，回调重排自己隔这么久再投 —— 活不能丢 */
#define NET_DEFER_RETRY_US   (50 * 1000)

#define ETH_DHCP_TIMEOUT_MS  10000

/* DHCP 超时的代号：每次武装 / 解除都 +1，回调把到点那一刻的代号带进事件。
   为什么要：回调投递之后、事件循环处理之前，状态机可能已经停掉又重新武装了定时器（掉线又插回）。
   不比代号的话，上一轮的超时会让新一轮 ETH_CONNECTING 刚开始就回退 WiFi。以前回调直接拿锁时
   也有这个窗口（等锁那段），改成投递后窗口更长，所以补上。只在持锁的状态机里改。 */
static volatile uint32_t s_eth_dhcp_gen;

/* 持 s_sm_mutex（init 时例外，那时还没有并发）：武装 DHCP 超时。先停再起 —— 定时器还在跑时
   esp_timer_start_once 会返错而不重新计时（以前四处直接 start_once，这个错误被静默吞掉）。 */
static void eth_dhcp_timer_arm(void)
{
    esp_timer_stop(s_eth_dhcp_timer);                    /* 没在跑会返错，忽略 */
    s_eth_dhcp_gen++;
    if (esp_timer_start_once(s_eth_dhcp_timer, (uint64_t)ETH_DHCP_TIMEOUT_MS * 1000) != ESP_OK) {
        ESP_LOGW(TAG, "ETH DHCP 超时定时器启动失败 —— 这一轮不会自动回退 WiFi");
    }
}

/* 持 s_sm_mutex：解除。代号 +1 让已经投出去、还没处理的超时作废。 */
static void eth_dhcp_timer_disarm(void)
{
    esp_timer_stop(s_eth_dhcp_timer);
    s_eth_dhcp_gen++;
}

/* esp_timer 任务：只投递（T9.28）。投不出去就重排自己 —— 这时它刚到点、没在跑，start 一定成功；
   若随后状态机先一步 arm / disarm，会先 stop 掉这次重排，代号也跟着变。 */
static void eth_dhcp_timeout_cb(void *arg)
{
    (void)arg;
    uint32_t gen = s_eth_dhcp_gen;
    if (esp_event_post(NET_DEFER_EVENT, NET_DEFER_ETH_DHCP_TIMEOUT, &gen, sizeof(gen), 0) != ESP_OK) {
        (void)esp_timer_start_once(s_eth_dhcp_timer, NET_DEFER_RETRY_US);
    }
}

/* 事件循环（sys_evt）：DHCP 超时真正的处理。 */
static void eth_dhcp_timeout_work(uint32_t gen)
{
    xSemaphoreTakeRecursive(s_sm_mutex, portMAX_DELAY);
    if (gen != s_eth_dhcp_gen) {
        /* 投出去之后定时器又被 arm / disarm 过：这次超时属于上一轮，作废 */
    } else if (s_state == NET_STATE_ETH_CONNECTING) {
        /* 日志放在判断里面：插着网线开机时 WiFi 常先拿到地址，状态从 ETH_CONNECTING 直接进
           WIFI_ONLINE（那条路不停这个定时器），以太网随后在约 9.6 s 拿到地址。10 s 到点时状态早已
           不是 ETH_CONNECTING、什么都不做 —— 以前却照样打一条 W "falling back"，每次开机告警环里
           都有一条假的回退（2026-10-06 测 T9.20 时发现） */
        ESP_LOGW(TAG, "ETH DHCP timeout (%dms), falling back", ETH_DHCP_TIMEOUT_MS);
        if (wifi_mgr_has_credentials()) {
            /* The boot path starts STA together with ETH, but the
               SOFTAP/NO_NETWORK -> ETH_LINK_UP path does not — start it
               here or WIFI_CONNECTING waits for events that never come. */
            wifi_mgr_start_sta();
            s_state = NET_STATE_WIFI_CONNECTING;
        } else {
            s_state = no_network_fallback();
        }
        /* 栈余量随手打出来：这条路以前在 esp_timer 任务（3584 B）上跑，T9.28 起在 sys_evt（2304 B） */
        ESP_LOGI(TAG, "State: ETH_CONNECTING -> %s（sys_evt 栈余 %u）", state_name(s_state),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        esp_event_post(NETWORK_EVENT, NETWORK_EVENT_STATE_CHANGED,
                       &s_state, sizeof(s_state), 0);
    }
    xSemaphoreGiveRecursive(s_sm_mutex);
}

// ---- Helpers ----

static const char *state_name(net_state_t s)
{
    switch (s) {
    case NET_STATE_INIT:              return "INIT";
    case NET_STATE_SOFTAP_ONLY:       return "SOFTAP_ONLY";
    case NET_STATE_ETH_CONNECTING:    return "ETH_CONNECTING";
    case NET_STATE_ETH_ONLINE:        return "ETH_ONLINE";
    case NET_STATE_WIFI_CONNECTING:   return "WIFI_CONNECTING";
    case NET_STATE_WIFI_PROVISION_FAILED: return "WIFI_PROVISION_FAILED";
    case NET_STATE_WIFI_ONLINE:       return "WIFI_ONLINE";
    case NET_STATE_WIFI_RECONNECTING: return "WIFI_RECONNECTING";
    case NET_STATE_BOTH_ONLINE:       return "BOTH_ONLINE";
    case NET_STATE_NO_NETWORK:        return "NO_NETWORK";
    default:                          return "UNKNOWN";
    }
}

static const char *event_name(net_event_t e)
{
    switch (e) {
    case NET_EVT_WIFI_CONNECTED:           return "WIFI_CONNECTED";
    case NET_EVT_WIFI_DISCONNECTED:        return "WIFI_DISCONNECTED";
    case NET_EVT_WIFI_GOT_IP:              return "WIFI_GOT_IP";
    case NET_EVT_WIFI_LOST_IP:             return "WIFI_LOST_IP";
    case NET_EVT_WIFI_RECONNECT_EXHAUSTED: return "WIFI_RECONNECT_EXHAUSTED";
    case NET_EVT_ETH_LINK_UP:              return "ETH_LINK_UP";
    case NET_EVT_ETH_LINK_DOWN:            return "ETH_LINK_DOWN";
    case NET_EVT_ETH_GOT_IP:              return "ETH_GOT_IP";
    case NET_EVT_WIFI_CREDENTIALS_SET:     return "WIFI_CREDENTIALS_SET";
    default:                               return "UNKNOWN_EVT";
    }
}

esp_err_t network_mgr_active_ifname(char *out, size_t out_len)
{
    if (!out || out_len == 0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';

    esp_netif_t *netif = NULL;
    switch (s_active_if) {
    case NET_IF_ETH:  netif = eth_mgr_get_netif();      break;
    case NET_IF_WIFI: netif = wifi_mgr_get_sta_netif(); break;
    default: return ESP_ERR_INVALID_STATE;
    }
    if (!netif) return ESP_ERR_INVALID_STATE;

    /* esp_netif_get_netif_impl_name() 要求缓冲至少 6 字节（lwip 的 if_name 是
       2 字符 + NUL，IDF 里按 NETIF_NAMESIZE=6 给）。小了直接拒，别截断。 */
    if (out_len < 6) return ESP_ERR_INVALID_SIZE;
    esp_err_t err = esp_netif_get_netif_impl_name(netif, out);
    if (err != ESP_OK) out[0] = '\0';
    return err;
}

static void set_default_route(net_interface_t iface)
{
    esp_netif_t *netif = NULL;

    switch (iface) {
    case NET_IF_ETH:  netif = eth_mgr_get_netif();     break;
    case NET_IF_WIFI: netif = wifi_mgr_get_sta_netif(); break;
    default: return;
    }

    if (!netif) return;

    esp_netif_set_default_netif(netif);
    net_interface_t old = s_active_if;
    s_active_if = iface;

    ESP_LOGI(TAG, "Default route -> %s",
             iface == NET_IF_ETH ? "ETH" : "WiFi");

    if (old != iface && old != NET_IF_NONE) {
        if (iface == NET_IF_WIFI) {
            /* ETH → WiFi：主链路掉了、退到备用 —— 这才是 failover */
            s_failed_over = true;
            esp_event_post(NETWORK_EVENT, NETWORK_EVENT_FAILOVER,
                           &iface, sizeof(iface), 0);
        } else if (s_failed_over) {
            /* WiFi → ETH 且此前真的退过：恢复 */
            s_failed_over = false;
            esp_event_post(NETWORK_EVENT, NETWORK_EVENT_RECOVERED,
                           &iface, sizeof(iface), 0);
        }
        /* WiFi → ETH 而没退过 = 开机时 WiFi 先拿到地址、以太网随后接管，或先用 WiFi 装、运行中插上网线：
           不是 FAILOVER / RECOVERED。2026-09-13 之前这里每次开机都发一条 FAILOVER，webhook/MQTT 端每次重启
           都收到一条"网络切换"假告警（T2.16，真发版验 T8.3 时三次重启三次都发）。
           但要发一个 ONLINE(ETH)（2026-10-07 T9.37 第二轮审计）：Thread 边界路由只跟着 FAILOVER / RECOVERED / ONLINE
           换 backbone，什么都不发的话"先用 WiFi 装、后插网线"的设备边界路由永远留在 WiFi 上 —— 服务地址搬到了网线上，
           Matter 的流量却还走 WiFi。ONLINE 不进 webhook（webhook 只转 OFFLINE / FAILOVER / RECOVERED），
           其余订阅方（main 清开机重试计数、指示灯置正常）对重复的 ONLINE 无副作用。 */
        else if (iface == NET_IF_ETH) {
            post_online(NET_IF_ETH);
        }
    }
}

/* WiFi 刚拿到 IP：决定它当主用还是备用（T2.22，2026-09-14）。
   以太网已在线时，以太网优先、WiFi 只作备份：默认路由**保持** ETH（幂等，不发
   FAILOVER/RECOVERED），状态进 BOTH_ONLINE，且**不** post_online —— 在线接口没变、
   以太网本就在线。否则任何 WiFi 抖动重连都会把主上行降级到 WiFi 且再不回退（真板复现：
   BOTH_ONLINE 时改一次 WiFi 配置就卡在 WIFI_ONLINE iface=WiFi，重启才回）。
   以太网不在线时 WiFi 就是主用，照常上线并广播 ONLINE。 */
static void wifi_got_ip_online(void)
{
    if (s_eth_has_ip) {
        set_default_route(NET_IF_ETH);       /* 已是 ETH 时是幂等 no-op */
        s_state = NET_STATE_BOTH_ONLINE;
    } else {
        set_default_route(NET_IF_WIFI);
        s_state = NET_STATE_WIFI_ONLINE;
        post_online(NET_IF_WIFI);
    }
}

static void post_online(net_interface_t iface)
{
    esp_event_post(NETWORK_EVENT, NETWORK_EVENT_ONLINE,
                   &iface, sizeof(iface), 0);
}

static void post_offline(void)
{
    esp_event_post(NETWORK_EVENT, NETWORK_EVENT_OFFLINE, NULL, 0, 0);
}

/* All interfaces lost. With no WiFi credentials the device would be
   unreachable forever — bring the provisioning SoftAP back instead. */
static net_state_t no_network_fallback(void)
{
    /* T9.40：调用方都是"WiFi 这一路放弃了"的出口；网口还有地址就不是无网络。以前不看网口，
       插着网线配 WiFi、新旧凭据都连不上时，重连耗尽就发 OFFLINE、进 NO_NETWORK（SYS 灯蓝闪、
       NET 灯红），WiFi 后来连上进 BOTH_ONLINE 又不补 ONLINE，灯一直不回来（2026-10-08 真板）。
       配网本身已改成网口在线时留在 ETH_ONLINE，这里是兜底。 */
    if (s_eth_has_ip) {
        ESP_LOGW(TAG, "WiFi 这一路放弃了，但网口仍在线 —— 留在 ETH_ONLINE，不宣布离线");
        set_default_route(NET_IF_ETH);   /* 已是 ETH 时是幂等 no-op */
        return NET_STATE_ETH_ONLINE;
    }
    post_offline();
    if (!wifi_mgr_has_credentials() && !s_softap_on) {
        if (wifi_mgr_start_softap() == ESP_OK) {
            s_softap_on = true;
            return NET_STATE_SOFTAP_ONLY;
        }
    }
    return s_softap_on ? NET_STATE_SOFTAP_ONLY : NET_STATE_NO_NETWORK;
}

/* 交互式配网中（s_provisioning）WiFi 断开一次。WIFI_CONNECTING 与"网口在线时的 ETH_ONLINE"
   共用（T9.40）—— 判据、回滚、报错只写一份，落点由调用方按自己的主状态定。 */
typedef enum {
    PROV_STEP_RETRY,        /* 还在试：已排 2s / 4s 快速重连 */
    PROV_STEP_ROLLED_BACK,  /* 收口失败，已退回旧凭据并发起连接；之后按普通重连走 */
    PROV_STEP_GAVE_UP,      /* 收口失败，没有可退的旧凭据：不再重试，等用户重填 */
} prov_step_t;

static prov_step_t prov_on_disconnect(void)
{
    /* 交互式配网：用户正盯着页面。快速试满 PROV_MAX_ATTEMPTS 次就
       收口报错，**不进流程 B** —— 静默重连 3 分钟只会让他干等。
       密码错(auth_failed)更是一次就够：再试一百次也是同样结果，
       早一秒告诉他早一秒能改。 */
    int reason = wifi_mgr_last_disconnect_reason();
    bool auth_bad = (reason == 15 || reason == 202 ||
                     reason == 204 || reason == 2);
    if (!auth_bad && s_prov_attempts < PROV_MAX_ATTEMPTS) {
        s_prov_attempts++;
        wifi_mgr_schedule_reconnect();   /* 2s / 4s，够快 */
        return PROV_STEP_RETRY;
    }
    ESP_LOGW(TAG, "配网失败：%s（第 %d 次，reason=%d）",
             auth_bad ? "认证不通过，多半是密码错" : "试满次数",
             s_prov_attempts, reason);
    s_provisioning = false;
    s_prov_state   = PROV_FAILED;   /* 页面据此报错 */
    wifi_mgr_reset_reconnect();     /* 别留着定时器偷偷再连 */
#if !CONFIG_BITENGINE_NET_ETH_ONLY  /* 纯以太网剖面不链接 Wi-Fi（设计 2.2）；那里也根本走不到配网 */
    esp_wifi_disconnect();          /* 停掉驱动的内部重扫 */
#endif

    if (!s_prev_valid) {
        /* 没有可回滚的对象（首次配网：热点模式，或先插网线装的设备第一次配 WiFi）。停下等用户重填。 */
        return PROV_STEP_GAVE_UP;
    }
    /* 有旧凭据可退 —— 退回去并重连。这样"误填一次密码"
       不再是致命操作：用户看到错误提示，设备还在原网络上，
       他可以直接重试。
       退回后按**普通重连**语义走（s_provisioning 已清），
       万一旧凭据也连不上就自然落进流程 B。 */
    ESP_LOGW(TAG, "配网失败，回滚到原凭据 SSID=%s", s_prev_ssid);
    config_mgr_set_wifi(s_prev_ssid, s_prev_pass);
    s_prev_valid = false;
    wifi_mgr_start_sta();
    return PROV_STEP_ROLLED_BACK;
}

// ---- State machine core ----


#if CONFIG_BITENGINE_WIFI_FLOAT_ETH_IP
/* T2.23：以太网掉线时，把以太网那个地址（服务 IP，也就是 ZHA/HA 填的那个）静态搬到
   WiFi 口并发 GARP，让外部按固定 IP 连进来的会话跟着活动链路走、不必改 HA 配置；
   以太网链路一回来先收回（WiFi 还原成它自己的 DHCP / 静态地址），避免两个口抢同一个地址。
   幂等：靠 s_svc_on_wifi 记录，重复调用无副作用。每次状态机跑完统一 reconcile。
   ⚠️ dhcpc_stop+set_ip_info 中途失败会把 WiFi 停在「DHCP 停 + 无地址」的死状态，而浮动
   期间以太网是断的、没有退路，所以失败一律退回 WiFi 自己的寻址（wifi_restore_own_addr）。
   DNS 随地址一起搬（set_ip_info 会清全局 DNS，见 float_svc_update），2026-10-06 之前这里
   写的是"DNS 不迁移、短暂不可用可接受"—— 实际是整段浮动期都没有 DNS。 */
/* ---- 浮动 IP 的 GARP 宣告（2026-10-05，docs/test_cases_stability.md 测试 6c）----
 *
 * 浮到 WiFi 之后局域网主机得知道 .34 换了 MAC，否则继续往以太网口的旧 MAC 发。原以为
 * lwIP 改地址时会自动发 gratuitous ARP —— **在本构建里不会**：
 *   CONFIG_LWIP_DHCP_DOES_ARP_CHECK=y → LWIP_DHCP_DOES_ACD_CHECK=1（lwipopts.h:319-321）
 *   → LWIP_ACD=1（opt.h:1049）→ netif_issue_reports() 的 `#if LWIP_ARP && !LWIP_ACD`
 *   （netif.c:922）整段不编译，地址宣告交给 ACD，而 ACD 只管 DHCP 拿到的地址。
 * esp_netif_set_ip_info() 设的是静态地址，所以**一条 GARP 都没有**；主机要等 ESP 每 60 s
 * 一次的周期 GARP（CONFIG_LWIP_GARP_TMR_INTERVAL）或自己的 ARP 过期 —— 实测 ZHA 拔线后
 * 14–41 s 才连回，快慢正好取决于离下一次周期 GARP 有多远。
 *
 * 照 RFC 5227 的 announce：立刻发一次，再每隔 2 s 补两次。etharp_* 只能在 lwIP 线程里调，
 * 用 tcpip_try_callback 投递（不阻塞调用方；队列满就丢这一次，后面还有补发和周期 GARP）。 */
#define FLOAT_GARP_REPEAT   2
#define FLOAT_GARP_GAP_US   (2000 * 1000)

static esp_timer_handle_t s_garp_timer;
static volatile int       s_garp_left;
static volatile int       s_garp_sent;
/* 往哪张卡宣告：T2.23 的浮动恒为 STA；T9.37 服务身份下是新持有者（网口或 WiFi）。
   s_garp_id_ip 非 0 = 身份机制的宣告：那张卡此刻还持有这个地址才发（lwIP 线程里比，交接走了就停）。 */
static esp_netif_t *volatile s_garp_nif;
static volatile uint32_t     s_garp_id_ip;

static void garp_in_tcpip(void *ctx)
{
    struct netif *lw = esp_netif_get_netif_impl((esp_netif_t *)ctx);
    if (lw && netif_is_up(lw) && netif_is_link_up(lw) && !ip4_addr_isany_val(*netif_ip4_addr(lw))) {
        uint32_t want = s_garp_id_ip;
        if (want && ip4_addr_get_u32(netif_ip4_addr(lw)) != want) return;   /* 服务地址已不在这张卡上 */
        etharp_gratuitous(lw);
    }
}

static void float_garp_send(void)
{
    esp_netif_t *nif = s_garp_nif;
    if (!nif) return;
    if (tcpip_try_callback(garp_in_tcpip, nif) != ERR_OK) {
        ESP_LOGW(TAG, "GARP 投递失败（lwIP 队列满），等下一次补发");
        return;
    }
    ESP_LOGI(TAG, "GARP 宣告服务IP 在%s口（第 %d 次）",
             nif == wifi_mgr_get_sta_netif() ? " WiFi " : "网", ++s_garp_sent);
}

static void garp_timer_cb(void *arg)
{
    if (!s_garp_id_ip) {                                 /* T2.23 浮动的宣告 */
        if (!s_svc_on_wifi) { s_garp_left = 0; return; } /* 已经收回以太网，不再宣告 */
    }
    float_garp_send();
    if (--s_garp_left > 0) esp_timer_start_once(s_garp_timer, FLOAT_GARP_GAP_US);
}

static void garp_begin(void)
{
    s_garp_sent = 0;
    float_garp_send();                                  /* 立刻一次 */
    if (!s_garp_timer) return;                          /* init 里建，建不成 init 就已失败 */
    esp_timer_stop(s_garp_timer);                       /* 没在跑会返回错误，忽略 */
    s_garp_left = FLOAT_GARP_REPEAT;
    if (esp_timer_start_once(s_garp_timer, FLOAT_GARP_GAP_US) != ESP_OK) {
        ESP_LOGW(TAG, "GARP 补发定时器启动失败 —— 只发了一次");
    }
}

/* T2.23：服务 IP 静态浮到 WiFi 口 */
static void float_garp_announce(void)
{
    s_garp_id_ip = 0;
    s_garp_nif   = wifi_mgr_get_sta_netif();
    garp_begin();
}

#if CONFIG_BITENGINE_SVC_IDENTITY
/* T9.37：新持有者用 DHCP 拿到了服务地址。DHCP 的 ARP 检查只探测不宣告（同上面 LWIP_ACD 那段），
   局域网主机还记着旧持有者的 MAC —— 照 T2.23 的节奏宣告三次。ip 是网络序。 */
static void svc_id_garp_announce(esp_netif_t *nif, uint32_t ip)
{
    if (!nif || !ip) return;
    s_garp_id_ip = ip;
    s_garp_nif   = nif;
    garp_begin();
}
#endif

/* set_ip_info 之后把 WiFi 原来的 DNS 还回去（见 float_svc_update）。全 0 的不还：
   esp_netif_set_dns_info 对 ANY 地址返回 INVALID_PARAMS（esp_netif_lwip.c 的 set_dns_info）。 */
static void float_dns_restore(esp_netif_t *sta, esp_netif_dns_info_t *dmain, esp_netif_dns_info_t *dbak)
{
    if (!ESP_IP_IS_ANY(dmain->ip)) esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, dmain);
    if (!ESP_IP_IS_ANY(dbak->ip))  esp_netif_set_dns_info(sta, ESP_NETIF_DNS_BACKUP, dbak);
}

/* 当前配置的 WiFi SSID 指纹（没配 WiFi = 0）。 */
static uint32_t wifi_cfg_ssid_hash(void)
{
    const wifi_config_data_t *w = config_mgr_get_wifi();
    return w->has_credentials ? svc_ssid_hash(w->ssid) : 0;
}

static bool svc_ssid_ok(void)
{
    return s_svc_ssid == 0 || wifi_cfg_ssid_hash() == s_svc_ssid;
}

/* 持 s_sm_mutex：记下 WiFi 此刻自己的寻址（见 s_wifi_own）。浮着、或 netif 上还没地址时不动。 */
static void wifi_own_refresh(esp_netif_t *sta)
{
    if (s_svc_on_wifi) return;                       /* 浮着时 netif 上是服务 IP，不是它自己的 */
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(sta, &info) != ESP_OK || info.ip.addr == 0) return;
    esp_netif_dhcp_status_t st = ESP_NETIF_DHCP_INIT;
    s_wifi_own.is_static = (esp_netif_dhcpc_get_status(sta, &st) == ESP_OK && st == ESP_NETIF_DHCP_STOPPED);
    s_wifi_own.ip = info;
    esp_netif_dns_info_t dm = {0}, db = {0};
    esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dm);
    esp_netif_get_dns_info(sta, ESP_NETIF_DNS_BACKUP, &db);
    if (!ESP_IP_IS_ANY(dm.ip)) s_wifi_own.dns_main = dm;     /* 一时为空就保留上一次的 */
    if (!ESP_IP_IS_ANY(db.ip)) s_wifi_own.dns_bak  = db;
    s_wifi_own.valid = true;
}

/* 收回（或浮动失败）时把 WiFi 还原成浮动前它自己的寻址（s_wifi_own）：原来是静态就重新下发那个静态地址
   和 DNS，否则启动 DHCP。2026-10-06 之前这里一律 dhcpc_start，静态 WiFi 插回网线后变成 DHCP 直到重启。
   静态还原失败照样退回 DHCP：别让 WiFi 停在没地址的死状态（理由同 apply_static_ip_config 那段）。 */
static void wifi_restore_own_addr(esp_netif_t *sta)
{
    if (s_wifi_own.valid && s_wifi_own.is_static) {
        esp_netif_dhcpc_stop(sta);       /* 静态时本就是 STOPPED：返回 ALREADY_STOPPED，无害 */
        if (esp_netif_set_ip_info(sta, &s_wifi_own.ip) == ESP_OK) {
            float_dns_restore(sta, &s_wifi_own.dns_main, &s_wifi_own.dns_bak);
            return;
        }
        ESP_LOGE(TAG, "WiFi 静态 IP 还原失败 —— 退回 DHCP，免得 WiFi 停在没地址的死状态");
    }
    esp_netif_dhcpc_start(sta);
}

static void float_svc_update(void)
{
    esp_netif_t *sta = wifi_mgr_get_sta_netif();
    if (!sta) return;
    /* T9.37 服务身份生效时服务地址由身份机制挪（持有者用 DHCP 拿），这里只管"路由器只认 MAC"/ 静态 IP 的旧行为 */
    bool want_float = (!svc_id_on() && !s_eth_link_up && s_wifi_has_ip && s_svc_ip_valid && svc_ssid_ok());

    if (want_float && !s_svc_on_wifi) {
        /* 只在服务 IP 落在 WiFi 自己的网段里时才浮（2026-10-06 随 T9.20 补）：有线、无线分属不同
           网段 / VLAN 时，把有线的地址挂到 WiFi 上只会让 WiFi 也一起断。比的是 WiFi 自己的地址：
           还没浮时 netif 上的就是；刚收回、WiFi 的 DHCP 还没重新绑定（地址为 0）时用快照里的，
           否则"插回又很快拔掉"要等 WiFi 重新拿到地址才浮，比 T2.23 原来的行为慢。 */
        wifi_own_refresh(sta);
        if (!s_wifi_own.valid) return;       /* 从没见过 WiFi 自己的地址：等下一次状态机 */
        const esp_netif_ip_info_t *own = &s_wifi_own.ip;
        if (!svc_same_subnet(lwip_ntohl(s_svc_ip.ip.addr), lwip_ntohl(own->ip.addr),
                             lwip_ntohl(own->netmask.addr))) {
            if (s_svc_subnet_warned != s_svc_ip.ip.addr) {
                s_svc_subnet_warned = s_svc_ip.ip.addr;
                ESP_LOGW(TAG, "服务IP " IPSTR " 不在 WiFi 的网段（" IPSTR "/" IPSTR "）里，不浮动",
                         IP2STR(&s_svc_ip.ip), IP2STR(&own->ip), IP2STR(&own->netmask));
            }
            return;
        }
        /* set_ip_info 会清掉**全局** DNS（esp_netif_lwip.c 的 set_ip_info：DHCP 客户端型 netif 调
           dns_clear_servers(true)；本配置 DNS 是全局的、没有后备 DNS）。不先存后还的话，浮动期间
           SNTP、云端检查更新、按主机名填的 MQTT / webhook 全部解析失败 —— 运行中浮动时是"短暂"，
           开机接管可能一浮就是整段断线期（2026-10-06 随 T9.20 补）。此刻全局 DNS 若已是空的（刚收回、
           两次 dhcpc_start 都清过），就用快照里 WiFi 上一次自己的 DNS。 */
        esp_netif_dns_info_t dns_main = {0}, dns_bak = {0};
        esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns_main);
        esp_netif_get_dns_info(sta, ESP_NETIF_DNS_BACKUP, &dns_bak);
        if (ESP_IP_IS_ANY(dns_main.ip)) { dns_main = s_wifi_own.dns_main; dns_bak = s_wifi_own.dns_bak; }
        esp_netif_dhcpc_stop(sta);
        if (esp_netif_set_ip_info(sta, &s_svc_ip) == ESP_OK) {
            float_dns_restore(sta, &dns_main, &dns_bak);
            s_svc_on_wifi = true;
            ESP_LOGW(TAG, "服务IP " IPSTR " 浮到 WiFi 口（failover，GARP 迁移，ZHA 免改配置）",
                     IP2STR(&s_svc_ip.ip));
            float_garp_announce();
            /* 服务 IP 真的挪到了 WiFi 上 = 本次开机真的退过。运行中拔线时 set_default_route 已置过；开机
               接管靠这里 —— 置在挂上地址这一刻，不在判定通过那一刻：两者之间网线可能插回、根本没浮，
               那网线回来就不该发 RECOVERED（2026-10-06 T9.20 复核） */
            s_failed_over = true;
        } else {
            wifi_restore_own_addr(sta);   /* 失败别停在死状态 */
            ESP_LOGE(TAG, "服务IP 浮到 WiFi 失败，WiFi 退回自己的地址");
        }
    } else if (!want_float && s_svc_on_wifi) {
        s_svc_on_wifi = false;
        wifi_restore_own_addr(sta);       /* WiFi 还原成自己的 DHCP / 静态地址 */
        ESP_LOGW(TAG, "服务IP 从 WiFi 口收回，WiFi 还原成自己的地址");
    }
}
#endif

#if CONFIG_BITENGINE_WIFI_FLOAT_BOOT_TAKEOVER
/* ---- T9.20 开机接管：实现（全貌见上面 s_bt_rec 那段注释）---- */

/* 主机序 IPv4 → "%u.%u.%u.%u" 的四个参数 */
#define HIP(x) (unsigned)((x) >> 24), (unsigned)(((x) >> 16) & 0xffu), \
               (unsigned)(((x) >> 8) & 0xffu), (unsigned)((x) & 0xffu)

/* 本构建 dhcp_timeout_t 是 u32（lwipopts.h 的 DHCP_TIMEOUT_SIZE_T）、DHCP_DEFINE_CUSTOM_TIMEOUTS=1，
   t0_timeout = ceil(租期 / DHCP_COARSE_TIMER_SECS)，不会被上游默认宏截成 0xffff。哪天退回 16 位，
   24 h 租约会被算成 18 h —— 编译期拦住，别等它在现场悄悄缩短。 */
_Static_assert(sizeof(((struct dhcp *)0)->t0_timeout) >= 4, "dhcp t0_timeout must be 32-bit (ESP DHCP_TIMEOUT_SIZE_T)");
_Static_assert(sizeof(((struct dhcp *)0)->lease_used) >= 4, "dhcp lease_used must be 32-bit (ESP DHCP_TIMEOUT_SIZE_T)");

/* 任意上下文：唤醒 svc_work_cb（delay_us 之后）。stop 失败（没在跑）忽略；start 失败只可能是
   别处刚把它启动了 —— 它照样会跑，跑的时候看到的是同一份状态。 */
static void svc_kick(uint64_t delay_us)
{
    if (!s_svc_work_timer) return;
    esp_timer_stop(s_svc_work_timer);
    (void)esp_timer_start_once(s_svc_work_timer, delay_us);
}

/* 墙钟可信吗：本次开机 SNTP 对上过；或者从上一次开机继承了可信墙钟（见 s_clock_trust），且年份像样。
   继承只在 esp_restart（ESP_RST_SW）时成立：它的关机钩子 esp_sync_timekeeping_timers 先把墙钟
   对齐进 RTC，boot_time 存在 LP_AON 寄存器里跨软复位接着走。断电从 1970 起算；死机 / 看门狗之后
   main.c 补的那次 esp_restart 前已经清了标记（network_mgr_note_unclean_reset）—— 这些都等 SNTP。 */
static bool svc_clock_trusted(void)
{
    if (s_time_synced) return true;
    return s_clock_inherited && (int64_t)time(NULL) >= SVC_EPOCH_MIN;
}

/* lwIP 线程：读以太网 DHCP 客户端的剩余租期。
   为什么在这里、为什么要周期读（H1，IDF v6.0.2 源码核过）：
   - struct dhcp 只由 lwIP 线程改，本配置没开 LWIP_TCPIP_CORE_LOCKING（LOCK_TCPIP_CORE 是空宏），
     只能在 lwIP 线程里读；
   - 拔线时 esp_eth 的 glue 先于我们的事件处理执行（esp_event 按 base 节点注册顺序分发，glue 的
     id 级处理挂在 ethernet_init 建的第一个节点上），它的 esp_netif_down → dhcp_stop 把租约字段
     清零 —— 到 ETH_LINK_DOWN 进状态机时已经读不到了，所以要在租约有效期间就记下；
   - 续租拿回同一地址不发 GOT_IP（esp_netif 判 ip 没变），续租只能靠周期读看见。
   lease_used 每个 coarse tick（本配置 1 s）+1、绑定（含续租）时清零，到 t0_timeout 即过期；
   tick 丢了不补，剩余只会被高估一两秒 —— 由 SVC_LEASE_MARGIN_S 吸收。
   无限租期：offered_t0_lease = 0xFFFFFFFF，dhcp_bind 不设 t0_timeout（保持 0）。 */
static void lease_read_in_tcpip(void *ctx)
{
    (void)ctx;
    svc_lease_rd_t rd = {0};
    esp_netif_t *eth = eth_mgr_get_netif();
    struct netif *lw = eth ? esp_netif_get_netif_impl(eth) : NULL;
    struct dhcp *d = lw ? netif_dhcp_data(lw) : NULL;
    if (d && dhcp_supplied_address(lw)) {                 /* BOUND / RENEWING / REBINDING */
        rd.ok          = true;
        rd.ip          = lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(lw)));
        rd.mask        = lwip_ntohl(ip4_addr_get_u32(netif_ip4_netmask(lw)));
        rd.gw          = lwip_ntohl(ip4_addr_get_u32(netif_ip4_gw(lw)));
        rd.infinite    = (d->offered_t0_lease == 0xFFFFFFFFUL);
        rd.lease_s     = d->t0_timeout * DHCP_COARSE_TIMER_SECS;
        rd.remaining_s = (!rd.infinite && d->t0_timeout > d->lease_used)
                         ? (d->t0_timeout - d->lease_used) * DHCP_COARSE_TIMER_SECS : 0;
        rd.mono_us     = esp_timer_get_time();
    }
    taskENTER_CRITICAL(&s_svc_mux);
    rd.seq = s_lease_rd.seq + 1;
    s_lease_rd = rd;
    taskEXIT_CRITICAL(&s_svc_mux);
    svc_kick(0);
}

/* 擦掉 NVS 里的快照（esp_timer 任务，svc_persist_step 用）。失败只打日志：下次照样会判。 */
static void svc_rec_erase(const char *why)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SVC_NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_key(h, SVC_NVS_KEY);
        if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) err = nvs_commit(h);
        nvs_close(h);
    }
    /* 擦成了才清缓存：两个擦除入口都以 s_rec_cache_ok 为前提，失败时保留它，下一次租约读数会再擦
       （2026-10-06 复核：以前一律清，擦失败后本次开机就不再试了） */
    if (err == ESP_OK) s_rec_cache_ok = false;
    ESP_LOGI(TAG, "服务IP 快照作废（%s）%s", why, err == ESP_OK ? "" : "—— 擦 NVS 失败，下次再试");
}

/* esp_timer 任务（svc_work_cb）：把新读到的租约锚定成墙钟，按 svc_persist_needed 限流写 NVS。
   不持 s_sm_mutex：写 flash 可能要擦页（十几到几十 ms），别让状态机陪着等。
   这里是 network_mgr 里唯一写 NVS 的地方（VERIFY.sh 钉着）。 */
static void svc_persist_step(void)
{
#if CONFIG_BITENGINE_SVC_IDENTITY
    svc_id_store();                                       /* T9.37 身份记录（有待写的才写；同一个任务、同一套约束） */
#endif
    svc_lease_rd_t rd;
    taskENTER_CRITICAL(&s_svc_mux);
    rd = s_lease_rd;
    taskEXIT_CRITICAL(&s_svc_mux);
    if (rd.seq == s_lease_consumed) return;
    s_lease_consumed = rd.seq;
    if (!rd.ok) return;                                   /* 以太网此刻没有 DHCP 租约 */
#if CONFIG_BITENGINE_SVC_IDENTITY
    /* 网口正用 WiFi 的身份（先用 WiFi 装、插着线）：它的租约是 WiFi 那份服务地址，不是"以太网服务 IP"——
       存进快照，退回旧行为时开机接管会把它当成以太网地址挂到 WiFi 上。无锁读一个字节：最坏晚一次读数 */
    if (svc_cid_get(SVC_NIC_ETH) != SVC_CID_SELF) return;
#endif
    if (!config_mgr_get_eth()->dhcp) return;              /* 静态以太网：正本是配置，不另存（7.6） */
    /* 以太网换了地址而墙钟还不可信（断电后没对上时）：新的锚不了，旧的也别留 —— 那是服务器可能已
       收回的地址（审计 F4）。擦除不需要墙钟；对上时后会补读一次、写新的。 */
    if (s_rec_cache_ok && (rd.ip != s_rec_cache.ip || rd.mask != s_rec_cache.mask || rd.gw != s_rec_cache.gw)
        && !svc_clock_trusted()) {
        svc_rec_erase("以太网换了地址");
        return;
    }
    if (!svc_clock_trusted()) return;                     /* 锚不了墙钟：对上时后会再读一次 */

    int64_t now = (int64_t)time(NULL);
    svc_rec_t neu = {0};
    neu.ver       = SVC_REC_VERSION;
    neu.flags     = rd.infinite ? SVC_REC_F_INFINITE : 0;
    neu.ssid_hash = wifi_cfg_ssid_hash();
    neu.ip        = rd.ip;
    neu.mask      = rd.mask;
    neu.gw        = rd.gw;
    neu.saved_at  = now;
    if (!rd.infinite) {
        int64_t rem = (int64_t)rd.remaining_s - (esp_timer_get_time() - rd.mono_us) / 1000000;
        if (rem <= 0) return;                             /* 读数陈旧到已过期：等下一次 */
        neu.lease_s = rd.lease_s;
        neu.expiry  = now + rem;
    }
    if (!svc_rec_valid(&neu, sizeof(neu))) {
        /* 没配 WiFi（ssid_hash=0 的快照永远过不了 svc_decide）或租约形态不认识：不存，旧的也作废，
           每次开机只说一次 —— 否则 svc_takeover 一直是 none 却查不到原因（审计 F7 / R2-18） */
        if (!s_rec_reject_logged) {
            s_rec_reject_logged = true;
            ESP_LOGW(TAG, "服务IP 快照不存（%s）：%u.%u.%u.%u/%u.%u.%u.%u 租期 %u s",
                     neu.ssid_hash ? "租约形态不认识" : "未配 WiFi",
                     HIP(neu.ip), HIP(neu.mask), (unsigned)neu.lease_s);
        }
        if (s_rec_cache_ok) svc_rec_erase("当前租约存不了");
        return;
    }
    /* 写盘间隔：租期的 1/4，夹在 5–30 分钟之间 —— 4 h 租约每次续租写一次（约 2 h 一次）；
       30 分钟的短租约也能跟上续租，否则 NVS 里的到期时刻老是落进余量、开机总判过期（审计 F10） */
    int64_t iv = (int64_t)neu.lease_s / 4;
    if (iv < 300) iv = 300;
    if (iv > 1800) iv = 1800;
    if (!svc_persist_needed(s_rec_cache_ok ? &s_rec_cache : NULL, &neu, now, iv)) return;

    nvs_handle_t h;
    esp_err_t err = nvs_open(SVC_NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, SVC_NVS_KEY, &neu, sizeof(neu));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "服务IP 快照写 NVS 失败：%s（下次读到租约再试）", esp_err_to_name(err));
        return;
    }
    s_rec_cache    = neu;
    s_rec_cache_ok = true;
    /* 栈余量随手打出来：esp_timer 任务 3584 B，nvs_set_blob 的栈峰值 IDF 没给数（H1，10-06 实测余 2672） */
    ESP_LOGI(TAG, "服务IP 快照已存：%u.%u.%u.%u，%s %" PRId64 " s（esp_timer 栈余 %u）",
             HIP(neu.ip), rd.infinite ? "无限租期" : "租期剩", rd.infinite ? (int64_t)0 : neu.expiry - now,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

/* network_mgr_init（main 任务，事件处理还没注册）：读回候选。读不了 / 不认识 = 没有候选。 */
static void svc_boot_load(void)
{
    if (svc_id_on()) {
        /* T9.37：网线断着时由 WiFi 用服务身份向路由器要服务地址 —— 租约有没有过期、地址有没有被占，路由器说了算，
           不用本地快照 + ARP 探测去猜。快照照常存（svc_persist_step），退回旧行为时还用得上 */
        ESP_LOGI(TAG, "开机接管：服务身份在管服务地址（T9.37），不用快照接管");
        return;
    }
    if (!config_mgr_get_eth()->dhcp) {
        ESP_LOGI(TAG, "开机接管：以太网是静态 IP，开机不接管（无租期可确认）");
        return;
    }
    nvs_handle_t h;
    if (nvs_open(SVC_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;   /* 从没写过，或刚恢复过出厂 */
    svc_rec_t r;
    size_t len = sizeof(r);
    esp_err_t err = nvs_get_blob(h, SVC_NVS_KEY, &r, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return;
    if (err != ESP_OK || !svc_rec_valid(&r, len)) {
        ESP_LOGW(TAG, "开机接管：NVS 里的服务IP 快照读不了或不认识（%s，%u 字节），忽略",
                 esp_err_to_name(err), (unsigned)len);
        return;
    }
    s_bt_rec       = r;
    s_rec_cache    = r;
    s_rec_cache_ok = true;
    s_bt_state     = SVC_BT_WAIT;
    s_bt_verdict   = SVC_OK;
    ESP_LOGI(TAG, "开机接管：候选 %u.%u.%u.%u（%s，墙钟%s）",
             HIP(r.ip), (r.flags & SVC_REC_F_INFINITE) ? "无限租期" : "有租期",
             s_clock_inherited ? "继承" : "待对时");
}

/* 持 s_sm_mutex：本次开机不再尝试，留一条 W 日志（进告警环，事后可查）。 */
static void svc_deny(svc_verdict_t v, const uint8_t *mac)
{
    s_bt_state   = SVC_BT_DENIED;
    s_bt_verdict = v;
    /* 原因写短：日志环按 140 字节截断（LOG_MSG_MAX）。各原因的含义见 svc_policy.h 的 svc_verdict_t */
    const char *why;
    switch (v) {
    case SVC_DENY_EXPIRED:         why = "租期已过"; break;
    case SVC_DENY_SUBNET:          why = "不在 WiFi 网段"; break;
    case SVC_DENY_NETWORK_CHANGED: why = "WiFi 换过网络"; break;
    case SVC_DENY_SAME_AS_WIFI:    why = "WiFi 已是此地址"; break;
    case SVC_DENY_CLOCK_BACKWARDS: why = "时钟倒退"; break;
    case SVC_DENY_CONFLICT:        why = "地址被占用"; break;
    default:                       why = svc_verdict_name(v); break;
    }
    if (mac) {
        ESP_LOGW(TAG, "开机接管：不接管 %u.%u.%u.%u（%s，%02x:%02x:%02x:%02x:%02x:%02x）",
                 HIP(s_bt_rec.ip), why, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        ESP_LOGW(TAG, "开机接管：不接管 %u.%u.%u.%u（%s）", HIP(s_bt_rec.ip), why);
    }
}

/* lwIP 线程：对候选地址发 ARP 请求。etharp_query(…, NULL) 为它建 PENDING 条目并立刻发请求
   （发送方 IP 是 WiFi 自己的地址），之后 etharp_tmr 每秒对 PENDING 条目补发，直到 ARP_MAXPENDING。
   地址的持有者回任何 ARP（应答、它自己的请求、GARP），etharp_input 都会把这个条目更新成 STABLE
   并记下 MAC —— 窗口到点时 probe_check_in_tcpip 看的就是它。返回 ERR_OK 才算"请求真的交给了驱动"
   （记进 sent；没发出去的探测到点时判"没探成"，不判"干净" —— 审计 F7：fail-open 会零探测接管）。
   盲区（接受）：此刻同样在对这个地址做 RFC 5227 探测（发送方 0.0.0.0）的主机不入表；不回 ARP 的
   主机看不见；探测窗口内 WiFi 掉线会清空该 netif 的 ARP 条目（到点时判"没探成"）。 */
static void probe_start_in_tcpip(void *ctx)
{
    uint32_t gen = (uint32_t)(uintptr_t)ctx;
    esp_netif_t *sta = wifi_mgr_get_sta_netif();
    struct netif *lw = sta ? esp_netif_get_netif_impl(sta) : NULL;
    bool sent = lw && netif_is_up(lw) && netif_is_link_up(lw) &&
                etharp_query(lw, &s_probe_ip, NULL) == ERR_OK;
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_svc_mux);
    if (s_probe_res.gen == gen) {                        /* 已作废的那一代不记 */
        s_probe_res.sent    = sent;
        s_probe_res.sent_us = now;
    }
    taskEXIT_CRITICAL(&s_svc_mux);
}

/* lwIP 线程：探测窗口到点，看候选地址有没有被别人认领。etharp_find_addr 只返回 STABLE 条目，
   且本配置 ETHARP_TABLE_MATCH_NETIF=1、按 netif 区分。 */
static void probe_check_in_tcpip(void *ctx)
{
    uint32_t gen = (uint32_t)(uintptr_t)ctx;
    bool same, sent;
    int64_t sent_us;
    taskENTER_CRITICAL(&s_svc_mux);
    same    = (s_probe_res.gen == gen);
    sent    = s_probe_res.sent;
    sent_us = s_probe_res.sent_us;
    taskEXIT_CRITICAL(&s_svc_mux);
    if (!same) return;                                    /* 作废过的探测：不看、不唤醒 */

    svc_probe_res_t res = { .gen = gen, .sent = sent, .sent_us = sent_us,
                            .done = true, .conclusive = false, .verdict = SVC_OK };
    esp_netif_t *sta = wifi_mgr_get_sta_netif();
    esp_netif_t *eth = eth_mgr_get_netif();
    struct netif *lw  = sta ? esp_netif_get_netif_impl(sta) : NULL;
    struct netif *elw = eth ? esp_netif_get_netif_impl(eth) : NULL;
    if (sent && esp_timer_get_time() - sent_us >= (int64_t)SVC_PROBE_MIN_SENT_MS * 1000 &&
        lw && netif_is_up(lw) && netif_is_link_up(lw)) {
        res.conclusive = true;
        struct eth_addr *ea = NULL;
        const ip4_addr_t *ipr = NULL;
        if (etharp_find_addr(lw, &s_probe_ip, &ea, &ipr) >= 0 && ea) {
            /* 应答方是我们自己不算冲突：AP 可能记着上次浮动时的 WiFi MAC 代答，或是以太网口的 MAC */
            bool ours = memcmp(ea->addr, lw->hwaddr, ETH_HWADDR_LEN) == 0 ||
                        (elw && memcmp(ea->addr, elw->hwaddr, ETH_HWADDR_LEN) == 0);
            if (!ours) {
                res.verdict = SVC_DENY_CONFLICT;
                memcpy(res.mac, ea->addr, sizeof(res.mac));
            }
        }
    }
    taskENTER_CRITICAL(&s_svc_mux);
    if (s_probe_res.gen == gen) s_probe_res = res;        /* 作废过的探测：代号对不上就丢 */
    taskEXIT_CRITICAL(&s_svc_mux);
    svc_kick(0);
}

/* esp_timer 任务：探测窗口到点 → 投到 lwIP 线程去看结果。 */
static void svc_probe_timer_cb(void *arg)
{
    (void)arg;
    uint32_t gen;
    taskENTER_CRITICAL(&s_svc_mux);
    gen = s_probe_res.gen;
    taskEXIT_CRITICAL(&s_svc_mux);
    if (tcpip_try_callback(probe_check_in_tcpip, (void *)(uintptr_t)gen) != ERR_OK) {
        /* lwIP 队列满：500 ms 后再收（窗口只会更长，结论不会因此变松） */
        (void)esp_timer_start_once(s_svc_probe_timer, 500 * 1000);
    }
}

/* 持 s_sm_mutex：作废在途探测（若有）。先停定时器再改代号：晚到的旧定时器拿不到新一代的代号。 */
static void svc_probe_cancel(void)
{
    esp_timer_stop(s_svc_probe_timer);
    s_bt_gen++;
}

/* 持 s_sm_mutex：发起一次探测。投递失败（lwIP 队列满）就留在 WAIT，1 s 后再来。 */
static void svc_probe_start(void)
{
    esp_timer_stop(s_svc_probe_timer);                    /* 先停：别让上一轮的定时器读到新代号 */
    uint32_t gen = ++s_bt_gen;
    s_probe_ip.addr = lwip_htonl(s_bt_rec.ip);
    taskENTER_CRITICAL(&s_svc_mux);
    s_probe_res = (svc_probe_res_t){ .gen = gen };
    taskEXIT_CRITICAL(&s_svc_mux);
    if (tcpip_try_callback(probe_start_in_tcpip, (void *)(uintptr_t)gen) != ERR_OK) {
        ESP_LOGW(TAG, "开机接管：ARP 探测投递失败（lwIP 队列满），1 s 后重试");
        svc_kick(1000 * 1000);
        return;
    }
    if (esp_timer_start_once(s_svc_probe_timer, (uint64_t)SVC_PROBE_MS * 1000) != ESP_OK) {
        ESP_LOGW(TAG, "开机接管：探测定时器启动失败，1 s 后重试");
        svc_kick(1000 * 1000);
        return;
    }
    s_bt_state = SVC_BT_PROBING;
    ESP_LOGI(TAG, "开机接管：ARP 探测 %u.%u.%u.%u（%d ms）", HIP(s_bt_rec.ip), SVC_PROBE_MS);
}

/* 持 s_sm_mutex：WiFi 此刻自己的地址（还没浮）。拿不到 / 还没地址 → false。 */
static bool svc_wifi_own_ip(esp_netif_ip_info_t *own)
{
    memset(own, 0, sizeof(*own));
    esp_netif_t *sta = wifi_mgr_get_sta_netif();
    return s_wifi_has_ip && !s_svc_on_wifi && sta &&
           esp_netif_get_ip_info(sta, own) == ESP_OK && own->ip.addr != 0;
}

/* 持 s_sm_mutex：除 ARP 外的全部条件（svc_decide），在发起探测前和探测干净后各判一次。 */
static svc_verdict_t svc_decide_now(const esp_netif_ip_info_t *own)
{
    return svc_decide(&s_bt_rec, lwip_ntohl(own->ip.addr), lwip_ntohl(own->netmask.addr),
                      wifi_cfg_ssid_hash(), svc_clock_trusted(), (int64_t)time(NULL), SVC_LEASE_MARGIN_S);
}

/* 持 s_sm_mutex：推进开机闸门。状态机每次跑完、svc_work_cb 每次被唤醒都调；幂等。 */
static void boot_takeover_eval(void)
{
    esp_netif_ip_info_t own;
    svc_gate_in_t in = {
        .state                = s_bt_state,
        /* 两个来源取或：s_eth_link_up 在 init 采样、之后靠事件维护，采样与注册事件处理之间若正好
           起链路会漏掉那个事件；eth_mgr 自己的标志在 glue 之前就注册了，不会漏（审计 F3） */
        .eth_link_up          = s_eth_link_up || eth_mgr_link_is_up(),
        .eth_got_ip_this_boot = s_eth_ip_seen,
        .wifi_has_own_ip      = svc_wifi_own_ip(&own),
        .since_init_ms        = (esp_timer_get_time() - s_init_us) / 1000,
        .grace_ms             = SVC_BOOT_GRACE_MS,
    };
    switch (svc_gate_step(&in)) {
    case SVC_GATE_NOTHING:
    case SVC_GATE_HOLD:
        return;
    case SVC_GATE_RETIRE:
        svc_probe_cancel();                               /* 若有在途探测，它的结果作废 */
        s_bt_state = SVC_BT_NONE;
        ESP_LOGI(TAG, "开机接管：以太网本次已拿到地址，开机接管退场");
        return;
    case SVC_GATE_ABORT:
        svc_probe_cancel();
        s_bt_state = SVC_BT_WAIT;
        ESP_LOGI(TAG, "开机接管：探测作废（网线插上了或 WiFi 掉了），回到等待");
        return;
    case SVC_GATE_ARM_GRACE:
        svc_kick((uint64_t)(SVC_BOOT_GRACE_MS - in.since_init_ms) * 1000);
        return;
    case SVC_GATE_DECIDE:
        break;
    }

    svc_verdict_t v = svc_decide_now(&own);
    if (v == SVC_OK) {
        s_bt_verdict = v;
        svc_probe_start();
        return;
    }
    if (v == SVC_WAIT_CLOCK) {
        if (s_bt_verdict != v) {
            /* 日志环按 140 字节截断：关键内容放前面、别写长（log_store 的 LOG_MSG_MAX） */
            ESP_LOGW(TAG, "开机接管：等对时（墙钟不可信，reset=%d），对上时后再判租期", (int)s_reset_reason);
        }
        s_bt_verdict = v;
        return;
    }
    svc_deny(v, NULL);
}

/* 持 s_sm_mutex：消化探测结果。干净且条件仍满足 → 把候选交给运行中浮动（由事件循环 reconcile）。
   返回 true = 调用方应当马上再推一次闸门。 */
static bool svc_probe_consume(void)
{
    if (s_bt_state != SVC_BT_PROBING) return false;
    svc_probe_res_t res;
    taskENTER_CRITICAL(&s_svc_mux);
    res = s_probe_res;
    taskEXIT_CRITICAL(&s_svc_mux);
    if (!res.done || res.gen != s_bt_gen) return false;   /* 还没到点 / 已作废 */
    if (!res.conclusive) {
        /* 到点时 WiFi 不在、请求没发出去、或窗口不够长：不判"干净"，隔一会儿从头再探 */
        s_bt_state = SVC_BT_WAIT;
        ESP_LOGI(TAG, "开机接管：这一轮没探成（%s），%d s 后重试",
                 res.sent ? "WiFi 不在或窗口不够" : "ARP 请求没发出去", (int)(SVC_PROBE_RETRY_US / 1000000));
        svc_kick(SVC_PROBE_RETRY_US);
        return false;
    }
    if (res.verdict == SVC_DENY_CONFLICT) {
        svc_deny(SVC_DENY_CONFLICT, res.mac);
        return false;
    }
    /* 探测干净。这 3 s 里租期 / WiFi 可能变了：除 ARP 外的条件再判一次（便宜，且只往安全方向变） */
    esp_netif_ip_info_t own;
    if (!svc_wifi_own_ip(&own)) {
        s_bt_state = SVC_BT_WAIT;
        return true;
    }
    svc_verdict_t v = svc_decide_now(&own);
    if (v == SVC_WAIT_CLOCK) { s_bt_state = SVC_BT_WAIT; s_bt_verdict = v; return false; }
    if (v != SVC_OK) { svc_deny(v, NULL); return false; }

    s_svc_ip.ip.addr      = lwip_htonl(s_bt_rec.ip);
    s_svc_ip.netmask.addr = lwip_htonl(s_bt_rec.mask);
    s_svc_ip.gw.addr      = lwip_htonl(s_bt_rec.gw);
    s_svc_ip_valid = true;                                /* 只有这里和 ETH_GOT_IP 的实时快照会置它 */
    s_svc_ssid     = s_bt_rec.ssid_hash;                  /* SSID 锁：svc_decide 已核过与当前配置一致 */
    /* s_failed_over 不在这里置：要等 float_svc_update 真把地址挂上（由事件循环 reconcile）。只是"以太网
       缺席"而没接管成（被拒、等对时、reconcile 前网线插回）的开机不算退过，网线回来不发 RECOVERED
       （T2.16 口径不变；2026-10-06 审计 R2-02 与复核）。 */
    s_bt_state     = SVC_BT_TAKEN;
    s_bt_verdict   = SVC_OK;
    s_svc_reconcile_pending = true;
    if (s_bt_rec.flags & SVC_REC_F_INFINITE) {
        ESP_LOGW(TAG, "开机接管：%u.%u.%u.%u 接管到 WiFi（无限租期，探测无人占用）", HIP(s_bt_rec.ip));
    } else {
        ESP_LOGW(TAG, "开机接管：%u.%u.%u.%u 接管到 WiFi（租期剩 %" PRId64 " s，探测无人占用）",
                 HIP(s_bt_rec.ip), s_bt_rec.expiry - (int64_t)time(NULL));
    }
    return false;
}

/* 事件循环（sys_evt，与状态机同一个任务）：推进闸门。由 svc_work_cb 投递 NET_DEFER_SVC_GATE 进来。
   eval → consume → (eval)：先让闸门看一眼（网线插上了就作废结果），消化结果后需要时再看一眼；
   接管成立就在这里真正挂地址 —— float_svc_update 要同步等 lwIP 线程好几次（dhcpc_stop /
   set_ip_info / DNS），只能在事件循环里做，不能在 esp_timer 回调里（2026-10-05 审计定下的先例）。
   以前闸门在 svc_work_cb 里拿锁推，挂地址再投一个 NET_SVC_EVENT 绕回来；T9.28 起整段都在这里。 */
static void svc_gate_work(void)
{
    xSemaphoreTakeRecursive(s_sm_mutex, portMAX_DELAY);
    boot_takeover_eval();
    if (svc_probe_consume()) boot_takeover_eval();
    if (s_svc_reconcile_pending) {
        s_svc_reconcile_pending = false;
        float_svc_update();
    }
#if CONFIG_BITENGINE_MDNS_SVC_IP_ONLY
    mdns_self_update();   /* 接管成立 / 被拒都会改变本机 mDNS 该宣告的地址 */
#endif
    xSemaphoreGiveRecursive(s_sm_mutex);
}

/* esp_timer 任务：一次性定时器，被 svc_kick 唤醒。先写盘（不持锁，栈在内部 RAM —— NVS 只能在这里写）；
   闸门还没到终态就投递给事件循环去推（T9.28：这里不拿状态机的锁）。终态之后不投
   （之后每 5 分钟的租约读数都会唤醒这里，别每次都去惊动状态机 —— 审计 F13）。 */
static void svc_work_cb(void *arg)
{
    (void)arg;
    svc_persist_step();
    svc_bt_state_t st = s_bt_state;                       /* 无锁读：终态之后闸门不会再动 */
    if (st != SVC_BT_NONE && st != SVC_BT_TAKEN && st != SVC_BT_DENIED) {
        if (esp_event_post(NET_DEFER_EVENT, NET_DEFER_SVC_GATE, NULL, 0, 0) != ESP_OK) {
            svc_kick(NET_DEFER_RETRY_US);                 /* 队列满：重排自己再投，写盘那步有 seq 去重 */
        }
    }
}

/* esp_timer 任务：周期读以太网剩余租期（续租不发事件，只能这样看见）。 */
static void svc_lease_timer_cb(void *arg)
{
    (void)arg;
    if (!s_eth_has_ip) return;                            /* 无锁读一个 bool：最坏晚一个周期 */
    if (tcpip_try_callback(lease_read_in_tcpip, NULL) != ERR_OK) {
        ESP_LOGD(TAG, "租约读数投递失败（lwIP 队列满），下个周期再读");
    }
}
#endif /* CONFIG_BITENGINE_WIFI_FLOAT_BOOT_TAKEOVER */

#if CONFIG_BITENGINE_SVC_IDENTITY
/* ---- T9.37：服务身份 —— 服务地址双向浮动（2026-10-07）----
 *
 * 设计 docs/16_service_identity_dual_float_design_v1.md；行为说明的正本是 Kconfig BITENGINE_SVC_IDENTITY 的 help；
 * 判据在 svc_policy.c 的 svc_id_*（test_host 测）；每张卡报哪个 client-id 在 svc_cid.c。这里做三件事：
 *   - 维护持有者（svc_id_update：状态机每次跑完、定时器到点时），变了就交接（svc_id_handover）；
 *   - 卡拿到地址时按设计 §7 判（svc_id_on_got_ip）：首次定 S、记 A_S、证实 / 否定路由器认 client-id；
 *   - 记录写 NVS 交给 svc_persist_step（esp_timer 任务、内部栈、不持状态机锁 —— 同 T9.20 的快照）。
 *
 * 上下文（S2）：svc_id_update / svc_id_handover / svc_id_on_got_ip 只在持 s_sm_mutex 的事件循环（sys_evt）里调
 * （init 里那次例外：还没有并发）。交接里的 dhcpc_stop / start 要同步等 lwIP 线程 —— 同 float_svc_update，
 * lwIP 线程从不拿 s_sm_mutex，不会死锁。s_id_timer 的回调只投递 NET_DEFER_SVC_ID（T9.28）。
 * A_S 在 s_id_rec 里是主机序；镜像到 s_svc_ip（网络序）给 T9.34 的 mDNS 与 /status 用。 */
#define SVC_ID_NVS_NS         "net_id"    /* 恢复出厂不擦（config_mgr 的 OWNED_NS 里没有它，10-07 用户拍板）；见 svc_policy.h */
#define SVC_ID_NVS_KEY        "id"        /* v2 记录在 SVC_NVS_NS（"net_svc"）的同名键下，开机读进来后挪到这里 */
#define SVC_ID_LINK_GRACE_MS  10000       /* 开机网口链路不在：等这么久再定 WiFi 当持有者（插线开机时链路几秒内就起来） */
#define SVC_ID_ETH_FAIL_MAX   2           /* 网口接过服务地址又 10 s 拿不到的次数；到了就不再从 WiFi 交过去，等链路断开重来 */
#define SVC_ID_SWAP_TIMEOUT_MS 20000      /* WiFi 交换着当持有者、已关联却一直拿不到地址：到点本次开机退回旧行为（两轮审计 ❌2：
                                             有的服务器对"别的 MAC 用这个 client-id"回 NAK 或不应答，WiFi 会永远没地址） */

static bool          s_id_enabled;        /* 本次开机身份机制可用：两张卡都在、都用 DHCP */
static svc_id_rec_t  s_id_rec;            /* s_nic == SVC_NIC_NONE（全 0）= S 还没定 */
static svc_nic_t     s_id_holder = SVC_NIC_NONE;
static int64_t       s_id_eth_wait_us;    /* 网口当持有者、链路在、还没地址：DHCP 超时从这一刻起算；0 = 不在等 */
static int           s_id_eth_fail;       /* 见 SVC_ID_ETH_FAIL_MAX；网口链路断开时清零 */
static bool          s_id_other_lan_warned;
static bool          s_id_as_live;        /* A_S 本次开机有卡真拿到过（不只是 NVS 里读来的）：退回旧行为时它才能留给静态浮动 */
static bool          s_id_off_boot;       /* 本次开机退化（交换后拿不到地址 / 判不了），不记进 NVS，下次开机再试 */
static volatile bool s_id_legacy_key;     /* 记录是从 v2 的老位置（net_svc/id）读来的：写进 net_id 之后把老键删掉 */
/* NVS 里那条记录（开机读到的、或之后写进去的；只有一个槽）。开机时它属于别的 WiFi 网络、或还没配 WiFi（恢复出厂后）
   就先按首次安装走；之后 WiFi 配回它那个网络时沿用它（svc_id_try_restore）—— 恢复出厂不擦身份记录就是为了这个 */
static svc_id_rec_t  s_id_nvs_rec;
static bool          s_id_nvs_valid;
static int64_t       s_id_swap_wait_us;   /* WiFi 交换着当持有者、已关联、还没地址：从这一刻起算；0 = 不在等 */
static uint32_t      s_id_own_ip[3];      /* 按 svc_nic_t 下标：这张卡本次开机用自己的身份拿到过的地址（主机序），判"只认 MAC"用 */
static bool          s_id_wifi_other_warned;
static esp_timer_handle_t s_id_timer;     /* 一次性：开机宽限 / 网口 DHCP 超时 / 交换超时到点 */
/* 待写 NVS 的那份：事件循环里放进来、esp_timer 任务（svc_persist_step）取走（s_svc_mux 保护） */
static svc_id_rec_t  s_id_store_req;
static bool          s_id_store_pending;

/* 退化了没有：路由器只认 MAC（记在案）或本次开机判不了。退化后怎么办看 svc_id_degrade_mode（S = WiFi 钉在 WiFi 上，
   其余走旧规则）—— 10-07 用户拍板。 */
static bool svc_id_degraded(void)
{
    return s_id_rec.mode == SVC_IDMODE_MAC_ONLY || s_id_off_boot;
}

/* 退化且 S = WiFi：WiFi 一直持有 A_S，两张卡都用自己的身份，网口只是第二个地址。身份机制仍"在管"（svc_id_on 为真）：
   旧规则那套（以太网地址静态浮到 WiFi、开机接管）这时会把 WiFi 上的 A_S 顶掉，正是要避免的 */
static bool svc_id_pinned(void)
{
    return s_id_enabled && svc_id_degraded() && svc_id_degrade_mode((svc_nic_t)s_id_rec.s_nic) == SVC_DEGRADE_PIN_WIFI;
}

static bool svc_id_on(void)
{
    return s_id_enabled && (!svc_id_degraded() || svc_id_pinned());
}

static esp_netif_t *svc_id_netif(svc_nic_t n)
{
    return n == SVC_NIC_ETH ? eth_mgr_get_netif() : n == SVC_NIC_WIFI ? wifi_mgr_get_sta_netif() : NULL;
}

/* 这张卡此刻的地址（0 = 没有）。esp_netif_get_ip_info 只拷 netif 上缓存的地址，不走 IPC。 */
static bool svc_id_ip(svc_nic_t n, esp_netif_ip_info_t *out)
{
    memset(out, 0, sizeof(*out));
    esp_netif_t *nif = svc_id_netif(n);
    return nif && esp_netif_get_ip_info(nif, out) == ESP_OK && out->ip.addr != 0;
}

/* 持 s_sm_mutex：当前记录交给 svc_persist_step 去写，并唤醒它。 */
static void svc_id_persist_req(void)
{
    /* 没配 WiFi 时定的 S 只留在内存（两轮审计 ❌1：产线插网线测、不配 WiFi —— 那时落盘，每台出货设备都带着"S = 网口"） */
    if (s_id_rec.net_ssid == 0) {
        ESP_LOGI(TAG, "服务身份：没配 WiFi，记录只留在内存（配上 WiFi 后按那张网重定）");
        return;
    }
    taskENTER_CRITICAL(&s_svc_mux);
    s_id_store_req     = s_id_rec;
    s_id_store_pending = true;
    taskEXIT_CRITICAL(&s_svc_mux);
    s_id_nvs_rec   = s_id_rec;                            /* 写盘失败会重试，按"终将写进去"算 */
    s_id_nvs_valid = true;
    svc_kick(0);
}

/* esp_timer 任务（svc_persist_step 调）：有待写的就写。失败放回去（若期间没有更新的请求），5 s 后再试。 */
static void svc_id_store(void)
{
    svc_id_rec_t r;
    bool pending;
    taskENTER_CRITICAL(&s_svc_mux);
    pending = s_id_store_pending;
    r = s_id_store_req;
    s_id_store_pending = false;
    taskEXIT_CRITICAL(&s_svc_mux);
    if (!pending) return;
    if (!svc_id_rec_valid(&r, sizeof(r))) {
        ESP_LOGW(TAG, "服务身份记录不存（字段不合理）：S=%u 地址 %u.%u.%u.%u", (unsigned)r.s_nic, HIP(r.ip));
        return;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(SVC_ID_NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, SVC_ID_NVS_KEY, &r, sizeof(r));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err == ESP_OK && s_id_legacy_key) {
        /* v2 的老位置随恢复出厂擦，留着它没用、还会让人以为那是正本 */
        if (nvs_open(SVC_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            esp_err_t e = nvs_erase_key(h, SVC_ID_NVS_KEY);
            if (e == ESP_OK) e = nvs_commit(h);
            nvs_close(h);
            if (e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND) s_id_legacy_key = false;
        }
    }
    if (err != ESP_OK) {
        taskENTER_CRITICAL(&s_svc_mux);
        if (!s_id_store_pending) { s_id_store_req = r; s_id_store_pending = true; }
        taskEXIT_CRITICAL(&s_svc_mux);
        ESP_LOGW(TAG, "服务身份记录写 NVS 失败：%s（5 s 后再试）", esp_err_to_name(err));
        svc_kick(SVC_PROBE_RETRY_US);
        return;
    }
    ESP_LOGI(TAG, "服务身份记录已存：S=%s 服务地址 %u.%u.%u.%u 路由器=%s%s",
             svc_nic_name((svc_nic_t)r.s_nic), HIP(r.ip), svc_id_mode_name((svc_id_mode_t)r.mode),
             (r.mode == SVC_IDMODE_MAC_ONLY && r.mode_ts) ? "（7 天后的下一次开机重试）" : "");
}

/* 持 s_sm_mutex：把两张卡的 client-id 换成 n_eth / n_wifi（设计 §5 的交接）。只动身份变了的卡：
     ① 先换哑身份 —— 下面 dhcpc_stop 底层是 dhcp_release_and_stop，已绑定时发 RELEASE，带哑身份对不上任何租约；
     ② 两张卡都停了再起 —— 旧持有者先放下服务地址，新持有者申请时 DHCP 的 ARP 检查（acd_dhcp_check.c）没人应答，
        不会 DECLINE；
     ③ 设新身份、起 DHCP。没在跑的卡（链路没起来 / 没关联，状态 INIT）只改表：IDF 在链路起来时自己起，
        用的就是那一刻表里的身份。
   被停掉的卡地址清零，协议栈逐个 RST 绑在那个地址上的连接，客户端重连。 */
static void svc_id_handover(svc_cid_t n_eth, svc_cid_t n_wifi)
{
    bool ce = svc_cid_get(SVC_NIC_ETH) != n_eth;
    bool cw = svc_cid_get(SVC_NIC_WIFI) != n_wifi;
    if (!ce && !cw) return;
    esp_netif_t *eth = eth_mgr_get_netif();
    esp_netif_t *sta = wifi_mgr_get_sta_netif();

    esp_netif_dhcp_status_t st;
    bool re = ce && eth && esp_netif_dhcpc_get_status(eth, &st) == ESP_OK && st == ESP_NETIF_DHCP_STARTED;
    bool rw = cw && sta && esp_netif_dhcpc_get_status(sta, &st) == ESP_OK && st == ESP_NETIF_DHCP_STARTED;
    /* 每张卡"设哑身份 → 停"成对做：一起设完再挨个停的话，第二张卡停之前它的 DHCP 定时器可能先发出一个带哑身份的
       续租（无害，路由器回 NAK，但没必要 —— 第一轮审计 B4） */
    esp_err_t e1 = ESP_OK, e2 = ESP_OK;
    if (ce) {
        svc_cid_set(SVC_NIC_ETH, SVC_CID_DUMMY);
        if (re) e1 = esp_netif_dhcpc_stop(eth);
    }
    if (cw) {
        svc_cid_set(SVC_NIC_WIFI, SVC_CID_DUMMY);
        if (rw) e2 = esp_netif_dhcpc_stop(sta);
    }

    svc_cid_set(SVC_NIC_ETH, n_eth);
    svc_cid_set(SVC_NIC_WIFI, n_wifi);
    if (ce) svc_cid_clear_declined(SVC_NIC_ETH);          /* 新的一轮申请：DECLINE 从这里重新算 */
    if (cw) svc_cid_clear_declined(SVC_NIC_WIFI);
    esp_err_t e3 = re ? esp_netif_dhcpc_start(eth) : ESP_OK;
    esp_err_t e4 = rw ? esp_netif_dhcpc_start(sta) : ESP_OK;
    if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK || e4 != ESP_OK) {
        ESP_LOGW(TAG, "服务身份交接：DHCP 停/起有失败（网口 %s/%s，WiFi %s/%s）", esp_err_to_name(e1),
                 esp_err_to_name(e3), esp_err_to_name(e2), esp_err_to_name(e4));
    }
    ESP_LOGI(TAG, "服务身份交接：网口报%s的身份%s，WiFi 报%s的身份%s（%s 栈余 %u）",
             n_eth == SVC_CID_PEER ? " WiFi " : "自己", re ? "、重新申请" : "",
             n_wifi == SVC_CID_PEER ? "网口" : "自己", rw ? "、重新申请" : "",
             pcTaskGetName(NULL), (unsigned)uxTaskGetStackHighWaterMark(NULL));   /* 平时在 sys_evt，开机那次在 main */
}

/* 持 s_sm_mutex：退回旧行为（服务地址 = 以太网地址，T2.23 静态浮动 / T9.20 开机接管）。
   persist = true：路由器只认 MAC，按 SSID 记下（下次开机也不再试）；false：只退本次开机（判不了 / 交换后拿不到地址）。
   两张卡回自然身份；之后 svc_id_on() 为假，float_svc_update 接手。镜像进 s_svc_ip 的 A_S：本次开机有卡真拿到过 → 留着，
   等同旧规则下 ETH_GOT_IP 的实时快照（网线断着时照旧浮过去）；只是从 NVS 读来、本次没人确认过 → 作废：旧规则只认
   "以太网此刻拿到的"或开机接管探测过的，不清的话静态浮动会不经 ARP 探测把它挂到 WiFi 上 */
static void svc_id_fallback(bool persist, const char *why)
{
    bool pin = svc_id_degrade_mode((svc_nic_t)s_id_rec.s_nic) == SVC_DEGRADE_PIN_WIFI;
    ESP_LOGW(TAG, "服务身份：%s —— %s%s", why, persist ? "记下（这个 WiFi 网络，7 天后的下一次开机重试）、" : "本次开机",
             pin ? "服务地址钉在 WiFi 上（插着网线流量也走 WiFi）" : "退回旧行为（服务地址 = 以太网地址）");
    if (persist) {
        s_id_rec.mode      = SVC_IDMODE_MAC_ONLY;
        s_id_rec.mode_ssid = wifi_cfg_ssid_hash();
        int64_t now = (int64_t)time(NULL);
        s_id_rec.mode_ts   = svc_clock_trusted() && now > 0 ? (uint32_t)now : 0;   /* 不可信：对上时后补记 */
        svc_id_persist_req();
    } else {
        s_id_off_boot = true;
    }
    s_id_eth_wait_us  = 0;
    s_id_swap_wait_us = 0;
    if (s_id_timer) esp_timer_stop(s_id_timer);
    svc_id_handover(SVC_CID_SELF, SVC_CID_SELF);
    if (pin) {
        /* WiFi 用自己的身份重拿（交接时它若正交换着，地址已清零）；A_S 照旧镜像进 s_svc_ip（svc_id_update 末尾） */
        s_id_holder = SVC_NIC_WIFI;
        return;
    }
    if (!s_id_as_live) s_svc_ip_valid = false;
    s_id_holder = SVC_NIC_NONE;
}

/* esp_timer 任务：只投递（T9.28）。投不出去就重排自己。 */
static void svc_id_timer_cb(void *arg)
{
    (void)arg;
    if (esp_event_post(NET_DEFER_EVENT, NET_DEFER_SVC_ID, NULL, 0, 0) != ESP_OK) {
        (void)esp_timer_start_once(s_id_timer, NET_DEFER_RETRY_US);
    }
}

/* 持 s_sm_mutex：按设计 §5 的表推进持有者；变了就交接。之后排好到点会变的条件（开机宽限、网口 DHCP 超时、交换超时）。幂等。 */
static void svc_id_update(void)
{
    if (!svc_id_on()) return;
    int64_t now = esp_timer_get_time();
    esp_netif_t *eth = eth_mgr_get_netif();
    esp_netif_t *sta = wifi_mgr_get_sta_netif();

    /* DHCP 停在 STOPPED（交接里 dhcpc_start 失败过）：身份机制只在两张卡都用 DHCP 时开，STOPPED 不正常 ——
       不补起的话，之后链路重连时 glue 会把它当静态 IP，这张卡本次开机再也不跑 DHCP（第一轮审计 B5） */
    esp_netif_t *nifs[2] = { eth, sta };
    for (int k = 0; k < 2; k++) {
        esp_netif_dhcp_status_t st;
        if (nifs[k] && esp_netif_dhcpc_get_status(nifs[k], &st) == ESP_OK && st == ESP_NETIF_DHCP_STOPPED) {
            ESP_LOGW(TAG, "服务身份：%s 的 DHCP 停着，补起", k == 0 ? "网口" : " WiFi ");
            (void)esp_netif_dhcpc_start(nifs[k]);
        }
    }

    if (svc_id_pinned()) {
        /* 钉在 WiFi 上：持有者恒为 WiFi，两张卡都用自己的身份（svc_id_fallback / svc_id_init 已经设好），不交接 */
        if (s_id_holder != SVC_NIC_WIFI) {
            s_id_holder = SVC_NIC_WIFI;
            svc_id_handover(SVC_CID_SELF, SVC_CID_SELF);
            svc_id_on_got_ip(SVC_NIC_WIFI);
        }
        if (s_id_rec.ip) {
            s_svc_ip.ip.addr      = lwip_htonl(s_id_rec.ip);
            s_svc_ip.netmask.addr = lwip_htonl(s_id_rec.mask);
            s_svc_ip.gw.addr      = lwip_htonl(s_id_rec.gw);
            s_svc_ip_valid        = true;
        }
        return;
    }

    /* 链路取两个来源（同开机接管闸门，审计 F3）：init 采样与注册事件处理之间链路正好起来，那个 LINK_UP 会漏掉 */
    bool link = s_eth_link_up || eth_mgr_link_is_up();
    if (!link) { s_id_eth_fail = 0; s_id_other_lan_warned = false; }   /* 链路断开：重新给网口机会 */

    esp_netif_ip_info_t ei, wi;
    bool eth_ip  = link && svc_id_ip(SVC_NIC_ETH, &ei);
    bool wifi_ip = svc_id_ip(SVC_NIC_WIFI, &wi);
    uint32_t as = s_id_rec.ip;
    bool eth_in_net = eth_ip && (!as || svc_same_subnet(as, lwip_ntohl(ei.ip.addr), lwip_ntohl(ei.netmask.addr)));
    svc_id_holder_in_t in = {
        .cur               = s_id_holder,
        .eth_link_up       = link,
        /* 接过来又拿不到地址满两次：不再把它当"已证明"，等链路断开重来（防 15 s 一次的来回交接） */
        .eth_has_ip        = eth_ip && (s_id_holder == SVC_NIC_ETH || s_id_eth_fail < SVC_ID_ETH_FAIL_MAX),
        .eth_ip_in_svc_net = !as || eth_in_net,
        .eth_dhcp_expired  = link && s_id_eth_wait_us && now - s_id_eth_wait_us >= (int64_t)ETH_DHCP_TIMEOUT_MS * 1000,
        /* 两张网（网口在 A_S 网段、WiFi 在别处）时 WiFi 不当持有者（第二轮审计 #1） */
        .wifi_usable       = svc_id_wifi_can_hold(wifi_mgr_has_credentials(), as,
                                                  wifi_ip ? lwip_ntohl(wi.ip.addr) : 0, wifi_ip ? lwip_ntohl(wi.netmask.addr) : 0,
                                                  eth_ip ? lwip_ntohl(ei.ip.addr) : 0, eth_ip ? lwip_ntohl(ei.netmask.addr) : 0),
        .since_init_ms     = (now - s_init_us) / 1000,
        .grace_ms          = SVC_ID_LINK_GRACE_MS,
    };
    if (s_id_holder == SVC_NIC_WIFI && eth_ip && as && !eth_in_net && !s_id_other_lan_warned) {
        s_id_other_lan_warned = true;
        ESP_LOGW(TAG, "服务身份：网口拿到 %u.%u.%u.%u，不在服务地址 %u.%u.%u.%u 的网段（接在另一张网上），服务地址留在 WiFi",
                 HIP(lwip_ntohl(ei.ip.addr)), HIP(as));
    }
    if (s_id_holder == SVC_NIC_ETH && wifi_ip && as && !in.wifi_usable && !s_id_wifi_other_warned) {
        s_id_wifi_other_warned = true;
        ESP_LOGW(TAG, "服务身份：WiFi 在 %u.%u.%u.%u 的网段、服务地址 %u.%u.%u.%u 在网口那张网 —— 两张网，WiFi 不接服务地址",
                 HIP(lwip_ntohl(wi.ip.addr)), HIP(as));
    }

    svc_nic_t nh = svc_id_next_holder(&in);
    if (nh != s_id_holder) {
        if (s_id_holder == SVC_NIC_ETH && nh == SVC_NIC_WIFI && link && in.eth_dhcp_expired) {
            s_id_eth_fail++;
            ESP_LOGW(TAG, "服务身份：网口 %d s 拿不到地址，服务地址交给 WiFi（第 %d 次）",
                     ETH_DHCP_TIMEOUT_MS / 1000, s_id_eth_fail);
        }
        ESP_LOGW(TAG, "服务身份：持有者 %s → %s（S=%s，服务地址 %u.%u.%u.%u）", svc_nic_name(s_id_holder),
                 svc_nic_name(nh), svc_nic_name((svc_nic_t)s_id_rec.s_nic), HIP(s_id_rec.ip));
        s_id_holder = nh;
        svc_nic_t sn = (svc_nic_t)s_id_rec.s_nic;
        svc_id_mode_t m = (svc_id_mode_t)s_id_rec.mode;
        svc_id_handover(svc_id_cid_for(SVC_NIC_ETH, nh, sn, m), svc_id_cid_for(SVC_NIC_WIFI, nh, sn, m));
        s_id_eth_wait_us  = 0;                            /* 交接后从头计（被交接的卡刚重启过 DHCP） */
        s_id_swap_wait_us = 0;
        eth_ip = link && svc_id_ip(SVC_NIC_ETH, &ei);
        /* 持有者变了却不用交接（如开机待定 → S 的本卡）：那张卡手上的地址早就拿到了、不会再来 GOT_IP，这里补判一次；
           交接过的卡地址已清零，补判直接返回（第一轮审计 ❌3） */
        if (nh != SVC_NIC_NONE) {
            svc_id_on_got_ip(nh);
            if (!svc_id_on()) return;                     /* 补判判出了退回旧行为 */
        }
    }
    if (s_id_holder == SVC_NIC_ETH && link && !eth_ip) {
        if (!s_id_eth_wait_us) s_id_eth_wait_us = now;
    } else {
        s_id_eth_wait_us = 0;
    }
    /* WiFi 交换着当持有者、已关联、还没地址：计时；到点本次开机退回 */
    bool wifi_swapped_waiting = s_id_holder == SVC_NIC_WIFI && svc_cid_get(SVC_NIC_WIFI) == SVC_CID_PEER &&
                                wifi_mgr_is_connected() && !svc_id_ip(SVC_NIC_WIFI, &wi);
    if (wifi_swapped_waiting) {
        if (!s_id_swap_wait_us) s_id_swap_wait_us = now;
        if (now - s_id_swap_wait_us >= (int64_t)SVC_ID_SWAP_TIMEOUT_MS * 1000) {
            svc_id_fallback(false, "WiFi 用网口的身份 20 s 拿不到地址（路由器不接受）");
            return;
        }
    } else {
        s_id_swap_wait_us = 0;
    }
    /* 首次安装：持有者是"后来才轮到"的（网口推定失败 / 宽限到点转 WiFi），它手上的地址早就拿到了、不会再来 GOT_IP
       —— 这里补判一次，否则 S 一直定不下来、mDNS 憋到上限 */
    if (s_id_rec.s_nic == SVC_NIC_NONE && s_id_holder != SVC_NIC_NONE) svc_id_on_got_ip(s_id_holder);

    /* T9.34 的 mDNS、旧代码、/status 都读 s_svc_ip：身份机制下它就是 A_S（不管此刻在哪张卡上） */
    if (s_id_rec.ip) {
        s_svc_ip.ip.addr      = lwip_htonl(s_id_rec.ip);
        s_svc_ip.netmask.addr = lwip_htonl(s_id_rec.mask);
        s_svc_ip.gw.addr      = lwip_htonl(s_id_rec.gw);
        s_svc_ip_valid        = true;
    }

    /* 只排**将来**的时刻：已经过了的条件这一轮已经判过了。排一个过去的时刻会立刻到点、再判出同样的结果、再排 ——
       忙循环（宽限到了却没有能当持有者的 WiFi、网口没地址又没别的卡可让，都会这样） */
    int64_t due = 0;
    int64_t grace_end = s_init_us + (int64_t)SVC_ID_LINK_GRACE_MS * 1000;
    if (s_id_holder == SVC_NIC_NONE && grace_end > now) due = grace_end;
    if (s_id_eth_wait_us) {
        int64_t d = s_id_eth_wait_us + (int64_t)ETH_DHCP_TIMEOUT_MS * 1000;
        if (d > now && (!due || d < due)) due = d;
    }
    if (s_id_swap_wait_us) {
        int64_t d = s_id_swap_wait_us + (int64_t)SVC_ID_SWAP_TIMEOUT_MS * 1000;
        if (d > now && (!due || d < due)) due = d;
    }
    if (due && s_id_timer) {
        esp_timer_stop(s_id_timer);
        (void)esp_timer_start_once(s_id_timer, (uint64_t)(due - now + 50 * 1000));   /* 多等 50 ms，到点时条件确实已翻过去 */
    }
}

/* 持 s_sm_mutex：WiFi 配的网络正是 NVS 里那条记录的网络（恢复出厂后重新配回同一个 WiFi；开机时还没配 WiFi）——
   沿用那条记录，别按首次安装重定（10-07 用户拍板：恢复出厂保留身份）。按沿用后的状态设好两张卡的身份。 */
static bool svc_id_try_restore(void)
{
    uint32_t h = wifi_cfg_ssid_hash();
    if (!s_id_nvs_valid || h == 0 || s_id_nvs_rec.net_ssid != h) return false;
    s_id_rec       = s_id_nvs_rec;
    s_id_as_live   = false;
    s_id_off_boot  = false;
    memset(s_id_own_ip, 0, sizeof(s_id_own_ip));
    ESP_LOGW(TAG, "服务身份：回到记录过的网络，沿用 S=%s 服务地址 %u.%u.%u.%u 路由器=%s",
             svc_nic_name((svc_nic_t)s_id_rec.s_nic), HIP(s_id_rec.ip), svc_id_mode_name((svc_id_mode_t)s_id_rec.mode));
    svc_nic_t sn = (svc_nic_t)s_id_rec.s_nic;
    svc_id_mode_t m = (svc_id_mode_t)s_id_rec.mode;
    if (svc_id_pinned()) {
        s_id_holder = SVC_NIC_WIFI;
        svc_id_handover(SVC_CID_SELF, SVC_CID_SELF);
    } else if (svc_id_on()) {
        svc_id_handover(svc_id_cid_for(SVC_NIC_ETH, s_id_holder, sn, m), svc_id_cid_for(SVC_NIC_WIFI, s_id_holder, sn, m));
    } else {
        /* 那条记录是"只认 MAC"、S = 网口：旧规则接手（服务地址 = 以太网地址，下次以太网拿到地址时快照） */
        svc_id_handover(SVC_CID_SELF, SVC_CID_SELF);
        s_id_holder    = SVC_NIC_NONE;
        s_svc_ip_valid = false;
    }
    return true;
}

/* 持 s_sm_mutex：某张卡拿到了地址（GOT_IP），按设计 §7 判。在状态机 switch 之前调；svc_id_update 在末尾再推持有者，
   持有者一变也会补判一次（那张卡手上的地址可能早就拿到了、不会再来 GOT_IP）。 */
static void svc_id_on_got_ip(svc_nic_t nic)
{
rejudge:                                                  /* 沿用了 NVS 里的记录之后按它再判一次（不递归：sys_evt 栈只剩约 1 KB） */
    if (!svc_id_on()) return;
    esp_netif_ip_info_t ip;
    if (!svc_id_ip(nic, &ip)) return;
    uint32_t a = lwip_ntohl(ip.ip.addr), mask = lwip_ntohl(ip.netmask.addr), gw = lwip_ntohl(ip.gw.addr);
    if (gw && !svc_same_subnet(gw, a, mask)) gw = 0;
    /* WiFi 在另一个 WiFi 网络上拿到了地址（用户换了 WiFi、且新网络真连上了）：换了网络，S 按首次安装重定 ——
       交换着的先回本卡身份，记录清空，往下按"S 未定"走。没配 WiFi 时（产线）只留在内存的 S 也在这里作废 */
    if (nic == SVC_NIC_WIFI && s_id_rec.s_nic != SVC_NIC_NONE && s_id_rec.net_ssid != wifi_cfg_ssid_hash()) {
        ESP_LOGW(TAG, "服务身份：WiFi 在另一个网络上拿到地址，身份按首次安装重定（原服务地址 %u.%u.%u.%u）", HIP(s_id_rec.ip));
        bool was_swapped = svc_cid_get(SVC_NIC_ETH) != SVC_CID_SELF || svc_cid_get(SVC_NIC_WIFI) != SVC_CID_SELF;
        memset(&s_id_rec, 0, sizeof(s_id_rec));
        s_id_as_live   = false;
        s_svc_ip_valid = false;
        s_id_off_boot  = false;                           /* 本次开机的退化是针对旧网络判的 */
        memset(s_id_own_ip, 0, sizeof(s_id_own_ip));
        if (svc_id_try_restore()) goto rejudge;          /* 这个网络 NVS 里有记录（恢复出厂后配回同一个 WiFi）。沿用后
                                                             net_ssid 对得上、S 已定，两个分支都不会再进来；交接过的卡地址已清零，重判直接返回 */
        if (was_swapped) {
            svc_id_handover(SVC_CID_SELF, SVC_CID_SELF);   /* WiFi 刚拿的地址是交换着拿的，丢掉重新申请 */
            return;
        }
    }
    /* 按这张卡拿到地址时**实际发出去**的身份判，不按表（svc_cid_sent 头注：表可能在 DHCP 已经发出去之后才改） */
    svc_cid_t sent = svc_cid_sent(nic);
    if (sent == SVC_CID_DUMMY) return;                    /* 交接中途的哑身份拿到的：不算数，交接完会重新申请 */
    bool swapped = (sent == SVC_CID_PEER);
    if (sent == SVC_CID_SELF) s_id_own_ip[nic] = a;       /* 判"只认 MAC"的对照 */

    if (s_id_rec.s_nic == SVC_NIC_NONE) {
        if (svc_id_try_restore()) goto rejudge;          /* 开机时记录不属于当时的网络，现在 WiFi 配回了它的网络 */
        svc_nic_t sn = svc_id_decide_s(s_id_holder, nic);
        if (sn == SVC_NIC_NONE) return;                   /* 不是持有者先拿到：等 */
        s_id_rec = (svc_id_rec_t){ .ver = SVC_ID_REC_VERSION, .s_nic = (uint8_t)sn, .mode = SVC_IDMODE_UNKNOWN,
                                   .flags = SVC_ID_F_AS_NATIVE,   /* S 没定时两张卡都用自己的身份 */
                                   .ip = a, .mask = mask, .gw = gw, .net_ssid = wifi_cfg_ssid_hash() };
        s_id_as_live = true;
        ESP_LOGW(TAG, "服务身份：首次定为%s的身份，服务地址 %u.%u.%u.%u（HA 等以后连的就是它）",
                 sn == SVC_NIC_ETH ? "网口" : " WiFi ", HIP(a));
        svc_id_persist_req();
        return;
    }

    svc_nic_t other = (nic == SVC_NIC_ETH) ? SVC_NIC_WIFI : SVC_NIC_ETH;
    esp_netif_ip_info_t oi;
    svc_id_judge_in_t jin = {
        .is_holder            = (nic == s_id_holder),
        .swapped              = swapped,
        .mode                 = (svc_id_mode_t)s_id_rec.mode,
        .a                    = a,
        .mask                 = mask,
        .a_s                  = s_id_rec.ip,
        .other_nic_in_svc_net = s_id_rec.ip && svc_id_ip(other, &oi) &&
                                svc_same_subnet(s_id_rec.ip, lwip_ntohl(oi.ip.addr), lwip_ntohl(oi.netmask.addr)),
        .as_native            = (s_id_rec.flags & SVC_ID_F_AS_NATIVE) != 0,
        .own_ip               = s_id_own_ip[nic],
        .declined             = svc_cid_declined(nic),
    };
    svc_judge_t jv = svc_id_judge(&jin);
    if (jv == SVC_JUDGE_OK || jv == SVC_JUDGE_CID_CONFIRMED || jv == SVC_JUDGE_ADOPT) s_id_as_live = true;
    switch (jv) {
    case SVC_JUDGE_NONE:
        return;
    case SVC_JUDGE_OK:
        /* S 的本卡用自己的身份拿回了 A_S（比如先用 WiFi 装、拔线后 WiFi 拿回服务地址）：补上"本卡亲自拿到"的标志 ——
           A_S 若是当初交换着采用的，之后再交换着拿到它才证实得了"认 client-id"（svc_id_judge 的 CID_CONFIRMED） */
        if (!swapped && nic == (svc_nic_t)s_id_rec.s_nic && !(s_id_rec.flags & SVC_ID_F_AS_NATIVE)) {
            s_id_rec.flags |= SVC_ID_F_AS_NATIVE;
            svc_id_persist_req();
        }
        break;
    case SVC_JUDGE_CID_CONFIRMED:
        s_id_rec.mode      = SVC_IDMODE_CID;
        s_id_rec.mode_ssid = wifi_cfg_ssid_hash();
        ESP_LOGI(TAG, "服务身份：路由器按 client-id 发址（%s 用对方的身份拿到 %u.%u.%u.%u）", svc_nic_name(nic), HIP(a));
        svc_id_persist_req();
        break;
    case SVC_JUDGE_ADOPT: {
        bool first = (s_id_rec.ip == 0);
        if (first) {
            ESP_LOGW(TAG, "服务身份：服务地址定为 %u.%u.%u.%u（%s拿到的第一个地址）", HIP(a), svc_nic_name(nic));
        } else {
            ESP_LOGW(TAG, "服务身份：服务地址 %u.%u.%u.%u → %u.%u.%u.%u（租约丢过、路由器重新分了址）", HIP(s_id_rec.ip), HIP(a));
        }
        s_id_rec.ip = a; s_id_rec.mask = mask; s_id_rec.gw = gw;
        s_id_rec.flags = (uint8_t)((s_id_rec.flags & ~SVC_ID_F_AS_NATIVE) | (swapped ? 0 : SVC_ID_F_AS_NATIVE));
        svc_id_persist_req();
        break;
    }
    case SVC_JUDGE_NET_CHANGED:
        /* 换了网络：S 属于"在这张网上第一次拿到地址"的那张卡（设计 §3）—— 重定为这张卡、回本卡身份。
           交换着拿到的地址不是这张卡自己的租约：A_S 置为未知，回本卡身份重新申请后再采用 */
        ESP_LOGW(TAG, "服务身份：%s 拿到 %u.%u.%u.%u，服务地址 %u.%u.%u.%u 的网段已不在任何一张卡上 —— 换了网络，身份重定为%s",
                 svc_nic_name(nic), HIP(a), HIP(s_id_rec.ip), nic == SVC_NIC_ETH ? "网口" : " WiFi ");
        s_id_rec = (svc_id_rec_t){ .ver = SVC_ID_REC_VERSION, .s_nic = (uint8_t)nic, .mode = SVC_IDMODE_UNKNOWN,
                                   .net_ssid = wifi_cfg_ssid_hash() };
        if (!swapped) {
            s_id_rec.ip = a; s_id_rec.mask = mask; s_id_rec.gw = gw;
            s_id_rec.flags = SVC_ID_F_AS_NATIVE;
            s_id_as_live = true;
        } else {
            s_id_as_live = false;
            s_svc_ip_valid = false;
        }
        svc_id_persist_req();
        if (swapped) {
            svc_nic_t sn = (svc_nic_t)s_id_rec.s_nic;
            svc_id_handover(svc_id_cid_for(SVC_NIC_ETH, s_id_holder, sn, SVC_IDMODE_UNKNOWN),
                            svc_id_cid_for(SVC_NIC_WIFI, s_id_holder, sn, SVC_IDMODE_UNKNOWN));
            return;
        }
        break;
    case SVC_JUDGE_MAC_ONLY:
        svc_id_fallback(true, "交换后拿到的正是这张卡自己身份的地址，路由器只认 MAC");
        return;
    case SVC_JUDGE_INCONCLUSIVE:
        svc_id_fallback(false, svc_cid_declined(nic) ? "交换着申请时撞上了地址冲突（DECLINE），判不了"
                                                     : "交换着拿到的不是服务地址（路由器上按 MAC 做了保留？），不改服务地址");
        return;
    case SVC_JUDGE_OTHER_LAN:
        if (!s_id_other_lan_warned) {
            s_id_other_lan_warned = true;
            ESP_LOGW(TAG, "服务身份：%s 拿到 %u.%u.%u.%u，不在服务地址 %u.%u.%u.%u 的网段（接在另一张网上），不让它持有",
                     svc_nic_name(nic), HIP(a), HIP(s_id_rec.ip));
        }
        return;                                           /* svc_id_update 按网段把持有者交回去 */
    }
    if (nic == s_id_holder) svc_id_garp_announce(svc_id_netif(nic), ip.ip.addr);
}

/* network_mgr_init（main 任务，事件处理还没注册）：决定本次开机用不用身份机制、读记录、定开机时的持有者与身份表。
   必须在 svc_boot_load 之前（它看 svc_id_on() 决定开机接管做不做），且在任何 DHCP 起来之前（第一个 DISCOVER 就要带对身份）。 */
static void svc_id_init(void)
{
    esp_netif_t *eth = eth_mgr_get_netif();
    esp_netif_t *sta = wifi_mgr_get_sta_netif();
    if (!eth || !sta) {
#if CONFIG_BITENGINE_NET_ETH_ONLY
        ESP_LOGI(TAG, "服务身份：纯以太网剖面只有一张网卡，不用（T9.37；docs/15 2.8 A-18）");
#else
        ESP_LOGI(TAG, "服务身份：没有以太网，不用（T9.37）");
#endif
        return;
    }
    if (!config_mgr_get_eth()->dhcp || config_mgr_get_wifi()->static_ip != 0) {
        ESP_LOGI(TAG, "服务身份：有静态 IP，服务地址按旧规则（T2.23 / T9.20）");
        return;
    }
    const esp_timer_create_args_t ta = { .callback = svc_id_timer_cb, .name = "svc_id" };
    if (esp_timer_create(&ta, &s_id_timer) != ESP_OK) {
        ESP_LOGE(TAG, "服务身份：定时器建不了，本次开机不用");
        return;
    }
    svc_cid_bind(eth, sta);

    svc_id_rec_t r;
    bool ok = false, migrated = false, from_snapshot = false;
    uint32_t ssid_now = wifi_cfg_ssid_hash();
    uint8_t buf[sizeof(svc_id_rec_t)];
    size_t len = sizeof(buf);
    nvs_handle_t h;
    /* 正本在 net_id（恢复出厂不擦）；没有就看 v2 的老位置 net_svc/id（读进来后写到 net_id、删掉老键）。
       命名空间不存在时 nvs_open（只读）也报 NOT_FOUND，与"没有这条"同样处理 */
    esp_err_t err = nvs_open(SVC_ID_NVS_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_blob(h, SVC_ID_NVS_KEY, buf, &len);
        nvs_close(h);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        len = sizeof(buf);
        err = nvs_open(SVC_NVS_NS, NVS_READONLY, &h);
        if (err == ESP_OK) {
            err = nvs_get_blob(h, SVC_ID_NVS_KEY, buf, &len);
            nvs_close(h);
        }
        if (err == ESP_OK) s_id_legacy_key = true;
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        /* 读出错（不是"没有这条"；含比 v3 还长的新格式 —— 降级刷了老固件）：当首次安装的话，之后写盘会把原来那条好记录
           覆盖掉（第一轮审计 D4）—— 本次不用 */
        ESP_LOGW(TAG, "服务身份：记录读不了（%s），本次开机按旧规则（T2.23 / T9.20）", esp_err_to_name(err));
        return;
    }
    if (err == ESP_OK) {
        ok = svc_id_rec_load(buf, len, &r);
        if (!ok) {
            ESP_LOGW(TAG, "服务身份：记录不认识（%u 字节，可能是旧版本），当首次安装", (unsigned)len);
        } else {
            s_id_nvs_rec   = r;
            s_id_nvs_valid = true;
            migrated = (len != sizeof(r)) || s_id_legacy_key;   /* v2 升上来的 / 在老位置的：写回 net_id */
        }
        if (ok && r.net_ssid != ssid_now) {
            /* S 属于"在这张网上第一次拿到地址"的那张卡（设计 §3）：换了 WiFi 网络（或恢复出厂后还没配 WiFi）就先按首次安装走；
               之后 WiFi 配回这条记录的网络时沿用它（svc_id_try_restore） */
            ESP_LOGI(TAG, "服务身份：记录属于别的 WiFi 网络（或还没配 WiFi），先按首次安装走");
            ok = migrated = false;
        }
    }
    if (!ok && ssid_now) {
        /* 迁移：老固件在同一个 WiFi 网络上插过网线、存过 T9.20 的以太网租约快照 → S = 网口，服务地址就是那个以太网地址
           （与升级前一致）。快照不是这个网络的不认 */
        svc_rec_t br;
        size_t bl = sizeof(br);
        if (nvs_open(SVC_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_blob(h, SVC_NVS_KEY, &br, &bl) == ESP_OK && svc_rec_valid(&br, bl) && br.ssid_hash == ssid_now) {
                r = (svc_id_rec_t){ .ver = SVC_ID_REC_VERSION, .s_nic = SVC_NIC_ETH, .mode = SVC_IDMODE_UNKNOWN,
                                    .flags = SVC_ID_F_AS_NATIVE, .ip = br.ip, .mask = br.mask, .gw = br.gw,
                                    .net_ssid = ssid_now };
                ok = migrated = from_snapshot = svc_id_rec_valid(&r, sizeof(r));
            }
            nvs_close(h);
        }
    }
    s_id_enabled = true;
    if (ok) {
        s_id_rec = r;
        if (s_id_rec.mode == SVC_IDMODE_MAC_ONLY && s_id_rec.mode_ssid != ssid_now) {
            ESP_LOGI(TAG, "服务身份：上次判过\"路由器只认 MAC\"，但换了 WiFi 网络 —— 重新试");
            s_id_rec.mode = SVC_IDMODE_UNKNOWN;
            s_id_rec.mode_ssid = 0;
            s_id_rec.mode_ts = 0;
            migrated = true;                              /* 借这个标志写回 */
        }
        if (s_id_rec.mode == SVC_IDMODE_MAC_ONLY) {
            /* "只认 MAC"记下满 7 天：这次开机重新试（用户删掉了路由器上的保留、换了路由器，都会在这里自己恢复）。
               只在开机做：运行中从旧规则切回身份机制要拆掉可能正挂着的静态浮动（svc_policy.h） */
            int64_t now = (int64_t)time(NULL);
            switch (svc_id_mac_only_age(s_id_rec.mode_ts, svc_clock_trusted(), now, SVC_ID_MAC_ONLY_RETRY_S)) {
            case SVC_MACONLY_RETRY:
                ESP_LOGW(TAG, "服务身份：\"路由器只认 MAC\"记下满 7 天，这次开机重新试");
                s_id_rec.mode = SVC_IDMODE_UNKNOWN;
                s_id_rec.mode_ssid = 0;
                s_id_rec.mode_ts = 0;
                migrated = true;
                break;
            case SVC_MACONLY_START:
                s_id_rec.mode_ts = (uint32_t)now;
                migrated = true;
                break;
            case SVC_MACONLY_KEEP:
                break;
            }
        }
        if (migrated) svc_id_persist_req();
    }
    if (svc_id_pinned()) {
        /* 路由器只认 MAC 记在案、S = WiFi：WiFi 一直持有，两张卡都用自己的身份（身份表默认值），不用定时器 */
        s_id_holder = SVC_NIC_WIFI;
        ESP_LOGI(TAG, "服务身份：路由器只认 MAC、S=wifi —— 服务地址 %u.%u.%u.%u 钉在 WiFi 上", HIP(s_id_rec.ip));
        return;
    }
    if (!svc_id_on()) {
        ESP_LOGI(TAG, "服务身份：路由器只认 MAC（%u.%u.%u.%u），服务地址按旧规则（T2.23 / T9.20）", HIP(s_id_rec.ip));
        return;                                           /* 身份表保持默认（两张卡都报自己的 MAC） */
    }
    s_id_holder = eth_mgr_link_is_up() ? SVC_NIC_ETH : SVC_NIC_NONE;
    svc_nic_t sn = (svc_nic_t)s_id_rec.s_nic;
    svc_id_mode_t m = (svc_id_mode_t)s_id_rec.mode;
    /* 走交接而不是直接改表：网口链路可能先于这里起来，IDF 已经用默认表（本卡身份）开始 DHCP 了（10-08 隔离网段实测）——
       那样的卡要"哑身份 → 停 → 换身份 → 重起"，第一份租约才是用对的身份拿的；还没起的卡只改表 */
    svc_id_handover(svc_id_cid_for(SVC_NIC_ETH, s_id_holder, sn, m), svc_id_cid_for(SVC_NIC_WIFI, s_id_holder, sn, m));
    if (s_id_holder == SVC_NIC_ETH) s_id_eth_wait_us = s_init_us;
    int64_t first = s_id_holder == SVC_NIC_ETH ? (int64_t)ETH_DHCP_TIMEOUT_MS : (int64_t)SVC_ID_LINK_GRACE_MS;
    (void)esp_timer_start_once(s_id_timer, (uint64_t)(first * 1000 + 50 * 1000));
    if (s_id_rec.s_nic == SVC_NIC_NONE) {
        ESP_LOGI(TAG, "服务身份：首次安装，开机持有者%s，第一个拿到地址的持有者定身份",
                 s_id_holder == SVC_NIC_ETH ? "推定网口" : "待定（等网口链路 10 s）");
    } else {
        ESP_LOGI(TAG, "服务身份：S=%s 服务地址 %u.%u.%u.%u 路由器=%s，开机持有者%s%s", svc_nic_name(sn), HIP(s_id_rec.ip),
                 svc_id_mode_name(m), s_id_holder == SVC_NIC_ETH ? "推定网口" : "待定（等网口链路 10 s）",
                 from_snapshot ? "（从开机接管快照迁移）" : "");
    }
}
/* 持 s_sm_mutex（事件循环，NET_DEFER_SVC_ID）："只认 MAC"记下时墙钟还不可信的，对上时后补记开始计时的时刻。
   只补记、不在运行中重试（重试在开机，见 svc_id_init）。 */
static void svc_id_mac_only_stamp(void)
{
    if (!s_id_enabled || s_id_rec.mode != SVC_IDMODE_MAC_ONLY || s_id_rec.mode_ts) return;
    int64_t now = (int64_t)time(NULL);
    if (svc_id_mac_only_age(0, svc_clock_trusted(), now, SVC_ID_MAC_ONLY_RETRY_S) != SVC_MACONLY_START) return;
    s_id_rec.mode_ts = (uint32_t)now;
    svc_id_persist_req();
}

/* lwIP 线程（SNTP 回调经 network_mgr_notify_time_synced）：只投递，补记在事件循环里做（T9.28）。投不出去不重试：
   之后任何一次 NET_DEFER_SVC_ID 都会补记，最晚下次开机。 */
static void svc_id_on_time_synced(void)
{
    if (!s_id_enabled) return;
    (void)esp_event_post(NET_DEFER_EVENT, NET_DEFER_SVC_ID, NULL, 0, 0);
}

/* 持 s_sm_mutex：用户提交了新的 WiFi 凭据（状态机 CREDENTIALS_SET 分支调）。只把"只认 MAC"按新网络作废；
   S 要不要重定，等 WiFi 真的在新网络上拿到地址时再判（svc_id_on_got_ip）—— 新凭据连不上会自动回滚到原凭据
   （不发 CREDENTIALS_SET），提交那一刻就清记录的话，一次失败的换 WiFi 就可能把 S 重定、服务地址跟着变（第一轮审计 E4）。 */
static void svc_id_on_creds_set(void)
{
    if (!s_id_enabled) return;
    if (s_id_rec.mode == SVC_IDMODE_MAC_ONLY && s_id_rec.mode_ssid != wifi_cfg_ssid_hash()) {
        ESP_LOGI(TAG, "服务身份：换了 WiFi 网络，\"路由器只认 MAC\"作废，重新试");
        s_id_rec.mode = SVC_IDMODE_UNKNOWN;
        s_id_rec.mode_ssid = 0;
        s_id_rec.mode_ts = 0;
    }
}

#if CONFIG_BITENGINE_DIAG_SVC_ID
esp_err_t network_mgr_diag_svc_id_set(const char *sname)
{
    if (!sname) return ESP_ERR_INVALID_ARG;
    svc_nic_t sn = !strcmp(sname, "eth") ? SVC_NIC_ETH : !strcmp(sname, "wifi") ? SVC_NIC_WIFI : SVC_NIC_NONE;
    if (sn == SVC_NIC_NONE) return ESP_ERR_INVALID_ARG;
    xSemaphoreTakeRecursive(s_sm_mutex, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_id_enabled) {
        /* 只改要写进 NVS 的那份，运行中的状态不动（换身份要整套重来，重启最干净）。
           服务地址 0 = 还不知道：下次开机持有者第一次拿到的地址就是它（svc_id_judge 的 ADOPT） */
        svc_id_rec_t r = { .ver = SVC_ID_REC_VERSION, .s_nic = (uint8_t)sn, .mode = SVC_IDMODE_UNKNOWN,
                           .net_ssid = wifi_cfg_ssid_hash() };
        taskENTER_CRITICAL(&s_svc_mux);
        s_id_store_req     = r;
        s_id_store_pending = true;
        taskEXIT_CRITICAL(&s_svc_mux);
        svc_kick(0);
        ESP_LOGW(TAG, "诊断：服务身份记录改成 S=%s、服务地址未知，重启后生效", svc_nic_name(sn));
        err = ESP_OK;
    }
    xSemaphoreGiveRecursive(s_sm_mutex);
    return err;
}
#endif

#if CONFIG_BITENGINE_DIAG_SVC_ID
/* 诊断：两张卡的 DHCP 状态（network_mgr.h）。struct dhcp 只由 lwIP 线程改，本配置没开 core locking，
   只能在 lwIP 线程里读（同 lease_read_in_tcpip）—— 用 esp_netif_tcpip_exec（公开 API）同步投过去读完再回来。 */
typedef struct {
    struct netif *lw[2];                  /* [0] 网口、[1] WiFi */
    struct {
        bool     ok;
        uint8_t  state;
        uint32_t ip, lease_s, used_s, t1_s, t2_s;
    } out[2];
} svc_dhcp_rd_t;

static esp_err_t svc_dhcp_read_in_tcpip(void *ctx)
{
    svc_dhcp_rd_t *r = (svc_dhcp_rd_t *)ctx;
    for (int k = 0; k < 2; k++) {
        struct netif *n = r->lw[k];
        struct dhcp *d = n ? netif_dhcp_data(n) : NULL;
        if (!d) continue;
        r->out[k].ok      = true;
        r->out[k].state   = d->state;
        r->out[k].ip      = ip4_addr_get_u32(netif_ip4_addr(n));
        r->out[k].lease_s = d->t0_timeout * DHCP_COARSE_TIMER_SECS;
        r->out[k].used_s  = d->lease_used * DHCP_COARSE_TIMER_SECS;
        r->out[k].t1_s    = d->t1_timeout * DHCP_COARSE_TIMER_SECS;
        r->out[k].t2_s    = d->t2_timeout * DHCP_COARSE_TIMER_SECS;
    }
    return ESP_OK;
}

int network_mgr_diag_dhcp_json(char *buf, size_t len)
{
    if (!buf || len == 0) return -1;
    esp_netif_t *nifs[2] = { eth_mgr_get_netif(), wifi_mgr_get_sta_netif() };
    svc_dhcp_rd_t r;
    memset(&r, 0, sizeof(r));
    for (int k = 0; k < 2; k++) r.lw[k] = nifs[k] ? esp_netif_get_netif_impl(nifs[k]) : NULL;
    if (esp_netif_tcpip_exec(svc_dhcp_read_in_tcpip, &r) != ESP_OK) return -1;

    static const char *const KIND[] = { "self", "peer", "dummy" };
    const svc_nic_t nic[2] = { SVC_NIC_ETH, SVC_NIC_WIFI };
    int n = snprintf(buf, len, "{");
    for (int k = 0; k < 2 && n >= 0 && (size_t)n < len; k++) {
        uint32_t ren = 0, reb = 0;
        svc_cid_counts(nic[k], &ren, &reb);
        svc_cid_t tab = svc_cid_get(nic[k]), sent = svc_cid_sent(nic[k]);
        uint32_t ip = lwip_ntohl(r.out[k].ip);
        n += snprintf(buf + n, len - (size_t)n,
                      "%s\"%s\":{\"ok\":%s,\"ip\":\"%u.%u.%u.%u\",\"state\":%u,\"lease_s\":%lu,\"used_s\":%lu,"
                      "\"t1_s\":%lu,\"t2_s\":%lu,\"table\":\"%s\",\"sent\":\"%s\",\"renew_reqs\":%lu,\"rebind_reqs\":%lu}",
                      k ? "," : "", k ? "wifi" : "eth", r.out[k].ok ? "true" : "false", HIP(ip), (unsigned)r.out[k].state,
                      (unsigned long)r.out[k].lease_s, (unsigned long)r.out[k].used_s,
                      (unsigned long)r.out[k].t1_s, (unsigned long)r.out[k].t2_s,
                      tab <= SVC_CID_DUMMY ? KIND[tab] : "?", sent <= SVC_CID_DUMMY ? KIND[sent] : "?",
                      (unsigned long)ren, (unsigned long)reb);
    }
    if (n >= 0 && (size_t)n < len) n += snprintf(buf + n, len - (size_t)n, "}");
    return (n < 0 || (size_t)n >= len) ? -1 : n;
}
#endif

#endif /* CONFIG_BITENGINE_SVC_IDENTITY */

void network_mgr_note_unclean_reset(void)
{
#if CONFIG_BITENGINE_WIFI_FLOAT_BOOT_TAKEOVER
    /* main.c 在非干净复位后补那次 esp_restart 之前调（network_mgr 还没初始化，只写一个 RTC 字）。
       补的那次重启复位原因是 ESP_RST_SW，但墙钟带着 RC 慢时钟的漂移，不能当成继承来的可信墙钟。 */
    s_clock_trust = 0;
#endif
}

void network_mgr_notify_time_synced(void)
{
#if CONFIG_BITENGINE_WIFI_FLOAT_BOOT_TAKEOVER
    s_clock_trust = SVC_CLOCK_TRUSTED;                    /* 每次对时都刷：之后 esp_restart 的下一次开机可继承 */
    if (s_time_synced) return;                            /* 闸门与补读只关心本次开机第一次 */
    s_time_synced = true;
    svc_id_on_time_synced();                              /* 只投递（lwIP 线程不拿锁） */
    /* 运行在 lwIP 线程（SNTP 回调）：不拿锁、不阻塞。补读一次租约（以太网开机时多半比 SNTP 先拿到
       地址，那时锚不了墙钟），读完它自己会 svc_kick；投递失败也要推一下闸门（可能正等对时）。 */
    if (tcpip_try_callback(lease_read_in_tcpip, NULL) != ERR_OK) svc_kick(0);
#endif
}

const char *network_mgr_svc_takeover_state(void)
{
#if CONFIG_BITENGINE_WIFI_FLOAT_BOOT_TAKEOVER
    svc_bt_state_t st = s_bt_state;                       /* httpd 无锁读两个枚举：最坏晚一拍 */
    svc_verdict_t  v  = s_bt_verdict;
    switch (st) {
    case SVC_BT_NONE:    return "none";
    case SVC_BT_WAIT:    return v == SVC_WAIT_CLOCK ? "wait_clock" : "waiting";
    case SVC_BT_PROBING: return "probing";
    case SVC_BT_TAKEN:   return "taken";
    case SVC_BT_DENIED:
        switch (v) {
        case SVC_DENY_NO_RECORD:       return "denied_no_record";
        case SVC_DENY_SUBNET:          return "denied_subnet";
        case SVC_DENY_SAME_AS_WIFI:    return "denied_same_as_wifi";
        case SVC_DENY_EXPIRED:         return "denied_expired";
        case SVC_DENY_CLOCK_BACKWARDS: return "denied_clock_backwards";
        case SVC_DENY_NETWORK_CHANGED: return "denied_network_changed";
        case SVC_DENY_CONFLICT:        return "denied_conflict";
        default:                       return "denied";
        }
    }
    return "none";
#else
    return "off";
#endif
}

const char *network_mgr_svc_identity(void)
{
#if CONFIG_BITENGINE_SVC_IDENTITY
    if (!s_id_enabled) return "off";
    return s_id_rec.s_nic == SVC_NIC_NONE ? "undecided" : svc_nic_name((svc_nic_t)s_id_rec.s_nic);
#else
    return "off";
#endif
}

const char *network_mgr_svc_holder(void)
{
#if CONFIG_BITENGINE_SVC_IDENTITY
    if (!svc_id_on()) return "off";
    if (svc_id_pinned()) return "wifi_pinned";
    return s_id_holder == SVC_NIC_NONE ? "pending" : svc_nic_name(s_id_holder);
#else
    return "off";
#endif
}

const char *network_mgr_svc_id_mode(void)
{
#if CONFIG_BITENGINE_SVC_IDENTITY
    if (!s_id_enabled) return "off";
    return svc_id_mode_name((svc_id_mode_t)s_id_rec.mode);
#else
    return "off";
#endif
}

uint32_t network_mgr_svc_ip(void)
{
#if CONFIG_BITENGINE_WIFI_FLOAT_ETH_IP
    return s_svc_ip_valid ? s_svc_ip.ip.addr : 0;
#else
    return 0;
#endif
}

void network_state_machine(net_event_t event)
{
    xSemaphoreTakeRecursive(s_sm_mutex, portMAX_DELAY);
    net_state_t old = s_state;

    /* CREDENTIALS_SET 在**任何**状态下语义都一样：用户提交了新凭据，
       开始一轮交互式配网。统一放在 switch 之前处理，不在 9 个状态里各写一份。
       ⚠️ 以前只有 SOFTAP_ONLY / NO_NETWORK 两个分支写了，于是"设备在线时
       用户改 WiFi 密码"这条最常见的路径**根本不走配网流程**，失败后直接
       掉进后台重连，用户永远等不到"密码错了"的提示。 */
    if (event == NET_EVT_WIFI_CREDENTIALS_SET) {
        s_provisioning  = true;   /* 用户正在等答案 */
        s_prov_attempts = 1;      /* set_wifi 里那次立刻连算第 1 次 */
        s_prov_state    = PROV_CONNECTING;
        s_wifi_has_ip   = false;
#if CONFIG_BITENGINE_WIFI_FLOAT_ETH_IP
        /* 换凭据可能换了网络：WiFi 自己的寻址快照作废（新网络重新拿到地址时再记）。服务 IP 浮不浮
           由 SSID 锁（s_svc_ssid）管：换到别的 SSID 不浮，同一个 SSID 重提或失败回滚到原凭据会浮回来。 */
        s_wifi_own.valid = false;
#endif
#if CONFIG_BITENGINE_SVC_IDENTITY
        svc_id_on_creds_set();   /* T9.37：换到另一个 WiFi 网络 = 换了网络，服务身份重定 */
#endif
        s_wifi_prov_hold           = false;   /* 用户重填了：上一轮"配网失败、不重试"作废 */
        s_wifi_fast_after_rollback = false;
        /* 先设标记再发起连接：反过来的话，连接若极快失败，DISCONNECTED
           可能在标记生效前就被处理，那次失败会被当成普通重连。 */
        wifi_mgr_start_sta();
        /* T9.40：网口在线时，配 WiFi 只是后台的一件事，网关本身是在线的 —— 主状态留在 ETH_ONLINE
           （BOTH_ONLINE 也回到 ETH_ONLINE：上面 set_wifi 已断开 WiFi），配网的成败由 ETH_ONLINE 收口。
           以前一律切到 WIFI_CONNECTING：配网期间 NET 灯黄闪"连接中"；新旧凭据都连不上时重连耗尽
           落进 NO_NETWORK、发 OFFLINE（2026-10-08 真板）。网口不在线时照旧走 WIFI_CONNECTING。 */
        s_state         = s_eth_has_ip ? NET_STATE_ETH_ONLINE : NET_STATE_WIFI_CONNECTING;
        if (s_state == old) {
            ESP_LOGI(TAG, "配网开始（主状态 %s 不变）", state_name(s_state));
        } else {
            ESP_LOGI(TAG, "State: %s -> %s (配网开始)", state_name(old), state_name(s_state));
            esp_event_post(NETWORK_EVENT, NETWORK_EVENT_STATE_CHANGED,
                           &s_state, sizeof(s_state), 0);
        }
        xSemaphoreGiveRecursive(s_sm_mutex);
        return;
    }

#if CONFIG_BITENGINE_WIFI_FLOAT_ETH_IP
    /* 浮动服务 IP：跟踪以太网链路，并在它拿到地址时把地址拍快照当服务 IP。
       事件本身不带 ip_info，直接读 netif。放在 switch 前，任何状态都覆盖到。 */
    if (event == NET_EVT_ETH_LINK_UP || event == NET_EVT_ETH_GOT_IP) {
#if CONFIG_BITENGINE_MDNS_SVC_IP_ONLY
        if (!s_eth_link_up) s_eth_link_up_us = esp_timer_get_time();   /* 等 DHCP 的计时起点（mdns_self_update） */
#endif
        s_eth_link_up = true;
    } else if (event == NET_EVT_ETH_LINK_DOWN) {
        s_eth_link_up = false;
    }
    if (event == NET_EVT_ETH_GOT_IP) {
        esp_netif_t *eth = eth_mgr_get_netif();
        esp_netif_ip_info_t info;
        /* T9.37 服务身份生效时，以太网的地址不一定是服务地址（可能是 other(S) 的）：服务地址由 svc_id_* 管 */
        if (!svc_id_on() && eth && esp_netif_get_ip_info(eth, &info) == ESP_OK && info.ip.addr) {
            s_svc_ip = info; s_svc_ip_valid = true;
            s_svc_ssid = wifi_cfg_ssid_hash();   /* 这个服务 IP 属于此刻配置的 WiFi 网络（SSID 锁） */
        }
#if CONFIG_BITENGINE_WIFI_FLOAT_BOOT_TAKEOVER
        /* 实时快照接管：开机接管退场（闸门见 s_eth_ip_seen）。顺手读一次租约存盘 —— 投递失败
           不要紧，周期读（SVC_LEASE_POLL_US）会补上。 */
        s_eth_ip_seen = true;
        (void)tcpip_try_callback(lease_read_in_tcpip, NULL);
#endif
    }
    /* WiFi 自己的寻址快照：拿到自己的地址就刷新（浮着时 netif 上是服务 IP，wifi_own_refresh 自己会跳过），
       断线 / 丢地址就作废 —— 别拿旧网络的网段去比（2026-10-06 T9.20 复核） */
    if (event == NET_EVT_WIFI_GOT_IP) {
        esp_netif_t *sta = wifi_mgr_get_sta_netif();
        if (sta) wifi_own_refresh(sta);
    } else if (event == NET_EVT_WIFI_DISCONNECTED || event == NET_EVT_WIFI_LOST_IP) {
        s_wifi_own.valid = false;
    }
#endif
#if CONFIG_BITENGINE_SVC_IDENTITY
    /* T9.37：拿到地址按设计 §7 判（定 S / 记 A_S / 证实或否定路由器认 client-id）；持有者在末尾 svc_id_update 推 */
    if (event == NET_EVT_ETH_GOT_IP)  svc_id_on_got_ip(SVC_NIC_ETH);
    if (event == NET_EVT_WIFI_GOT_IP) svc_id_on_got_ip(SVC_NIC_WIFI);
#endif

    /* 网口断链 = 网口没有可用地址，与当前在哪个状态无关。以前只有 ETH_ONLINE / BOTH_ONLINE 两处清，
       别的状态下断链 s_eth_has_ip 会卡在 true（mdns 那处为此另加了 && link，2026-10-07）；
       no_network_fallback() 的"网口仍在线"兜底靠它（T9.40）。各状态自己的分支照旧再清一次，无害。 */
    if (event == NET_EVT_ETH_LINK_DOWN) s_eth_has_ip = false;

    switch (s_state) {

    // ========== INIT ==========
    case NET_STATE_INIT:
        // INIT is handled in network_mgr_init(), not by events
        ESP_LOGW(TAG, "Unexpected event %s in state INIT", event_name(event));
        break;

    // ========== SOFTAP_ONLY ==========
    case NET_STATE_SOFTAP_ONLY:
        /* CREDENTIALS_SET 已在 switch 之前统一处理，此处不再有分支 */
        if (event == NET_EVT_ETH_LINK_UP) {
            s_state = NET_STATE_ETH_CONNECTING;
            eth_dhcp_timer_arm();
        } else if (event == NET_EVT_WIFI_GOT_IP) {
            // Background reconnect succeeded while in SoftAP
            s_wifi_has_ip = true;
            wifi_mgr_stop_softap();
            s_softap_on = false;
            wifi_got_ip_online();
        } else if (event == NET_EVT_ETH_GOT_IP) {
            s_eth_has_ip = true;
            /* Device reachable via ETH — don't leave an OPEN AP running */
            wifi_mgr_stop_softap();
            s_softap_on = false;
            set_default_route(NET_IF_ETH);
            s_state = NET_STATE_ETH_ONLINE;
            post_online(NET_IF_ETH);
        } else if (event == NET_EVT_WIFI_CONNECTED) {
            /* background reconnect associated — waiting for GOT_IP */
        } else if (event == NET_EVT_WIFI_DISCONNECTED) {
            /* background reconnect attempt failed — keep the retry chain
               alive, otherwise it dies after the first failure */
            wifi_mgr_schedule_background_retry();
        } else {
            ESP_LOGW(TAG, "Unexpected event %s in state %s",
                     event_name(event), state_name(s_state));
        }
        break;

    // ========== ETH_CONNECTING ==========
    case NET_STATE_ETH_CONNECTING:
        if (event == NET_EVT_ETH_GOT_IP) {
            eth_dhcp_timer_disarm();
            s_eth_has_ip = true;
            if (s_softap_on) {
                wifi_mgr_stop_softap();
                s_softap_on = false;
            }
            set_default_route(NET_IF_ETH);
            s_state = NET_STATE_ETH_ONLINE;
            post_online(NET_IF_ETH);
        } else if (event == NET_EVT_WIFI_GOT_IP) {
            // WiFi got IP first while waiting for ETH DHCP
            s_wifi_has_ip = true;
            wifi_got_ip_online();
        } else if (event == NET_EVT_ETH_LINK_DOWN) {
            eth_dhcp_timer_disarm();
            s_eth_has_ip = false;
            if (wifi_mgr_has_credentials()) {
                wifi_mgr_start_sta();
                s_state = NET_STATE_WIFI_CONNECTING;
            } else {
                s_state = no_network_fallback();
            }
        } else if (event == NET_EVT_WIFI_CONNECTED) {
            /* 开机插着网线时 STA 与以太网一起启动（network_mgr_init），WiFi 先关联上是正常的，
               等 GOT_IP。以前落到 else，每次开机打一条 "Unexpected event"（2026-10-06 测 T9.20 时发现，
               同 08-22 修 ETH_LINK_UP 那一类） */
        } else if (event == NET_EVT_WIFI_DISCONNECTED) {
            /* 网口 DHCP 还没回来时 WiFi 第一次就没连上（AP 没起来、密码错、被 5 GHz 防线拒掉）。
               以前落到 else 只打一条 "Unexpected event"，重试链就此断掉：进 ETH_ONLINE 之后没人再连 WiFi，
               服务身份在 WiFi 上时服务地址直到重启都不回来（2026-10-21 T9.44 诊断固件上板实测）。
               配网分支同 ETH_ONLINE；其余同不插网线开机的 WIFI_CONNECTING，走快速重连。 */
            s_wifi_has_ip = false;
            if (s_provisioning) {
                switch (prov_on_disconnect()) {
                case PROV_STEP_RETRY:       break;
                case PROV_STEP_ROLLED_BACK: s_wifi_fast_after_rollback = true; break;
                case PROV_STEP_GAVE_UP:     s_wifi_prov_hold = true; break;
                }
            } else if (!s_wifi_prov_hold) {
                wifi_mgr_schedule_reconnect();
            }
        } else if (event == NET_EVT_WIFI_RECONNECT_EXHAUSTED) {
            /* 上面的快速重连耗尽了还没等到网口地址：wifi_mgr 耗尽时已自己排好慢速重试，这里什么都不用变 */
        } else {
            ESP_LOGW(TAG, "Unexpected event %s in state %s",
                     event_name(event), state_name(s_state));
        }
        break;

    // ========== ETH_ONLINE ==========
    case NET_STATE_ETH_ONLINE:
        if (event == NET_EVT_WIFI_GOT_IP) {
            s_wifi_has_ip = true;
            if (s_provisioning) {
                /* 网口在线时的配网成功了（T9.40，同 WIFI_CONNECTING 的 GOT_IP） */
                s_provisioning   = false;
                s_prov_state     = PROV_OK;
                s_prev_valid     = false;   /* 新凭据已验证可用，快照作废 */
                wifi_mgr_reset_reconnect();
            } else if (s_wifi_prov_hold) {
                s_prov_state = PROV_OK;     /* 报过失败之后驱动自己连上了（同 WIFI_PROVISION_FAILED 的 GOT_IP） */
            }
            s_wifi_prov_hold           = false;
            s_wifi_fast_after_rollback = false;
            /* 网口在线时也可能开着热点：POST /network/softap 就是给"有线在网时用手机配网"的。
               WiFi 连上了就关掉（同 WIFI_CONNECTING / WIFI_RECONNECTING）—— 不关的话开放热点一直开着、
               Thread 边界路由也一直让着它（2026-10-08 审计） */
            if (s_softap_on) {
                wifi_mgr_stop_softap();
                s_softap_on = false;
            }
            // ETH stays as default route
            s_state = NET_STATE_BOTH_ONLINE;
        } else if (event == NET_EVT_WIFI_CONNECTED) {
            /* background WiFi reconnect associated — waiting for GOT_IP */
        } else if (event == NET_EVT_WIFI_DISCONNECTED) {
            if (s_provisioning) {
                /* 网口在线时的配网（T9.40）：判据与回滚同 WIFI_CONNECTING，主状态留在这里 */
                switch (prov_on_disconnect()) {
                case PROV_STEP_RETRY:       break;
                case PROV_STEP_ROLLED_BACK: s_wifi_fast_after_rollback = true; break;
                case PROV_STEP_GAVE_UP:     s_wifi_prov_hold = true; break;
                }
            } else if (s_wifi_fast_after_rollback) {
                wifi_mgr_schedule_reconnect();   /* 刚退回旧凭据：快速连回，耗尽后 wifi_mgr 自己转慢速 */
            } else if (!s_wifi_prov_hold) {
                wifi_mgr_schedule_background_retry();
            }
            /* s_wifi_prov_hold：配网失败、没有能用的凭据 —— 同 WIFI_PROVISION_FAILED，刻意不重试 */
        } else if (event == NET_EVT_WIFI_RECONNECT_EXHAUSTED) {
            /* 只会来自上面那条"退回旧凭据后快速重连"：旧网络也连不上。网口在线，什么都不用变 ——
               wifi_mgr 耗尽时已自己排好慢速重试；以后的断开按后台重试走 */
            s_wifi_fast_after_rollback = false;
        } else if (event == NET_EVT_WIFI_LOST_IP) {
            /* WiFi 断开约 40 s 后协议栈补报的"丢了地址"：正常事件（配网 / 换凭据都在本状态里进行，T9.40 起
               每次都会碰到；2026-10-08 真板）。本状态下 WiFi 本来就不算有地址。 */
            s_wifi_has_ip = false;
        } else if (event == NET_EVT_ETH_GOT_IP) {
            /* 服务身份交接（T9.37）重启了网口的 DHCP、重新拿到地址：正常事件，默认路由本来就是网口 */
            s_eth_has_ip = true;
        } else if (event == NET_EVT_ETH_LINK_DOWN) {
            s_eth_has_ip = false;
            /* 网口在线时配网失败后的"不重试"只在网口撑着时有意义；网口也没了，就按有凭据照常重连 ——
               上次"找不到 AP"可能只是一时的，这是唯一可能自己回来的路（T9.40）。
               配网还在进行中的（s_provisioning）由 WIFI_CONNECTING 接着收口。 */
            s_wifi_prov_hold           = false;
            s_wifi_fast_after_rollback = false;   /* WIFI_CONNECTING 自己快速重连 */
            if (s_wifi_has_ip) {
                set_default_route(NET_IF_WIFI);
                s_state = NET_STATE_WIFI_ONLINE;
            } else if (wifi_mgr_has_credentials()) {
                wifi_mgr_start_sta();
                s_state = NET_STATE_WIFI_CONNECTING;
            } else {
                s_state = no_network_fallback();
            }
        } else {
            ESP_LOGW(TAG, "Unexpected event %s in state %s",
                     event_name(event), state_name(s_state));
        }
        break;

    // ========== WIFI_CONNECTING ==========
    case NET_STATE_WIFI_CONNECTING:
        if (event == NET_EVT_WIFI_GOT_IP) {
            s_wifi_has_ip = true;
            if (s_provisioning) { s_provisioning = false; s_prov_state = PROV_OK; }
            s_prev_valid = false;   /* 新凭据已验证可用，快照作废 */
            wifi_mgr_reset_reconnect();
            if (s_softap_on) {
                wifi_mgr_stop_softap();
                s_softap_on = false;
            }
            wifi_got_ip_online();
        } else if (event == NET_EVT_WIFI_DISCONNECTED) {
            s_wifi_has_ip = false;
            if (s_provisioning) {
                switch (prov_on_disconnect()) {
                case PROV_STEP_RETRY:       break;
                case PROV_STEP_ROLLED_BACK: s_state = NET_STATE_WIFI_CONNECTING; break;
                case PROV_STEP_GAVE_UP:
                    if (s_softap_on) {
                        s_state = NET_STATE_WIFI_PROVISION_FAILED;   /* 经热点来配的：热点还在，等他重填 */
                    } else {
                        /* 经网口来配的、网口中途断了：没热点、没网口，"不重试"就是死路（老代码就这样，
                           2026-10-08 审计）—— 按有凭据照常重连，同 ETH_ONLINE 里 hold 遇到断链的处理 */
                        s_state = no_network_fallback();
                        if (s_state == NET_STATE_NO_NETWORK) wifi_mgr_schedule_background_retry();
                    }
                    break;
                }
            } else {
                wifi_mgr_schedule_reconnect();
                s_state = NET_STATE_WIFI_RECONNECTING;
            }
        } else if (event == NET_EVT_ETH_GOT_IP) {
            s_eth_has_ip = true;
            set_default_route(NET_IF_ETH);
            s_state = NET_STATE_ETH_ONLINE;
            post_online(NET_IF_ETH);
        } else if (event == NET_EVT_ETH_LINK_UP) {
            // ETH appeared while waiting for WiFi — will get ETH_GOT_IP later
        } else if (event == NET_EVT_WIFI_CONNECTED) {
            // Associated, waiting for GOT_IP — normal, not "unexpected"
        } else {
            ESP_LOGW(TAG, "Unexpected event %s in state %s",
                     event_name(event), state_name(s_state));
        }
        break;

    // ========== WIFI_ONLINE ==========
    case NET_STATE_WIFI_ONLINE:
        if (event == NET_EVT_ETH_GOT_IP) {
            s_eth_has_ip = true;
            set_default_route(NET_IF_ETH);
            s_state = NET_STATE_BOTH_ONLINE;
        } else if (event == NET_EVT_ETH_LINK_UP) {
            /* 网线插上了，但 WiFi 正在服务 —— 这里什么都不用做：DHCP 由 IDF 的
               esp_netif_action_connected() 自己起，拿到地址后 ETH_GOT_IP 会把
               状态推到 BOTH_ONLINE。同 WIFI_CONNECTING 那处的处理。
               ⚠️ 2026-08-22 补：此前没有这个分支，落到 else 打
               "Unexpected event ETH_LINK_UP in state WIFI_ONLINE" —— 两次拔插实测都命中。
               不是功能故障（DHCP 照跑），但它把一个**正常**事件报成异常，
               会掩盖真正异常的那些。 */
        } else if (event == NET_EVT_WIFI_DISCONNECTED) {
            s_wifi_has_ip = false;
            wifi_mgr_schedule_reconnect();
            s_state = NET_STATE_WIFI_RECONNECTING;
            /* WiFi was the only online interface — the gateway is offline
               now, subscribers (webhook network_down, mDNS) must know */
            post_offline();
        } else if (event == NET_EVT_WIFI_LOST_IP) {
            s_wifi_has_ip = false;
            // Stay in WIFI_ONLINE, wait for DISCONNECTED or new GOT_IP
        } else if (event == NET_EVT_WIFI_GOT_IP) {
            /* 两种来源，都是正常事件：① LOST_IP 之后 DHCP 又拿到了地址 —— 以前这里落到 else，
               s_wifi_has_ip 会一直卡在 false，浮动永远不再发生；② 浮动服务 IP 自己的 set_ip_info
               也会发 STA_GOT_IP（esp_netif_lwip.c 的 set_ip_info，STA 带 EVENT_IP_MODIFIED）——
               以前每次浮动都打一条 "Unexpected event"（2026-10-06 T9.20 审计）。 */
            s_wifi_has_ip = true;
        } else {
            ESP_LOGW(TAG, "Unexpected event %s in state %s",
                     event_name(event), state_name(s_state));
        }
        break;

    // ========== WIFI_RECONNECTING ==========
    case NET_STATE_WIFI_RECONNECTING:
        if (event == NET_EVT_WIFI_GOT_IP) {
            s_wifi_has_ip = true;
            wifi_mgr_reset_reconnect();
            if (s_softap_on) {
                wifi_mgr_stop_softap();
                s_softap_on = false;
            }
            wifi_got_ip_online();
        } else if (event == NET_EVT_WIFI_RECONNECT_EXHAUSTED) {
            /* ⚠️ **有凭据时绝不开 SoftAP**（2026-08-21 决策）。
               以前这里无条件开，于是路由器只要重启一次、重连没赶上，网关就
               自己支起一个 **WIFI_AUTH_OPEN 的开放热点**：用户没要求配网，
               只是路由器暂时不在，却凭空多出一个谁都能连的 AP。
               而且 SoftAP 与 Thread BR 互斥，代价不止是安全。
               no_network_fallback() 里已经有"无凭据才开"这条正确策略，
               直接复用，不要在这里再写一份。
               有凭据时落到 NO_NETWORK，靠后台慢速重试等路由器回来。 */
            s_state = no_network_fallback();
            // Background retry continues via wifi_mgr timer
        } else if (event == NET_EVT_ETH_GOT_IP) {
            s_eth_has_ip = true;
            set_default_route(NET_IF_ETH);
            s_state = NET_STATE_ETH_ONLINE;
            post_online(NET_IF_ETH);
        } else if (event == NET_EVT_ETH_LINK_UP) {
            /* 网线插上了，但 WiFi 正在服务 —— 这里什么都不用做：DHCP 由 IDF 的
               esp_netif_action_connected() 自己起，拿到地址后 ETH_GOT_IP 会把
               状态推到 ETH_ONLINE。同 WIFI_CONNECTING 那处的处理。
               ⚠️ 2026-08-22 补：此前没有这个分支，落到 else 打
               "Unexpected event ETH_LINK_UP in state WIFI_RECONNECTING" —— 两次拔插实测都命中。
               不是功能故障（DHCP 照跑），但它把一个**正常**事件报成异常，
               会掩盖真正异常的那些。 */
        } else if (event == NET_EVT_WIFI_DISCONNECTED) {
            // Already reconnecting, schedule next attempt
            wifi_mgr_schedule_reconnect();
        } else if (event == NET_EVT_WIFI_CONNECTED) {
            // Connected but no IP yet, wait for GOT_IP
        } else {
            ESP_LOGW(TAG, "Unexpected event %s in state %s",
                     event_name(event), state_name(s_state));
        }
        break;

    // ========== BOTH_ONLINE ==========
    case NET_STATE_BOTH_ONLINE:
        if (event == NET_EVT_ETH_LINK_DOWN) {
            s_eth_has_ip = false;
            set_default_route(NET_IF_WIFI);
            s_state = NET_STATE_WIFI_ONLINE;
        } else if (event == NET_EVT_WIFI_DISCONNECTED) {
            s_wifi_has_ip = false;
            wifi_mgr_schedule_reconnect();
            s_state = NET_STATE_ETH_ONLINE;
        } else if (event == NET_EVT_WIFI_LOST_IP) {
            s_wifi_has_ip = false;
        } else if (event == NET_EVT_WIFI_GOT_IP) {
            /* 收回服务 IP 后 WiFi 重新拿到自己的地址、或 LOST_IP 后恢复：正常事件（同 WIFI_ONLINE） */
            s_wifi_has_ip = true;
        } else if (event == NET_EVT_ETH_GOT_IP) {
            /* 服务身份交接（T9.37）重启了网口的 DHCP、重新拿到地址：正常事件 */
            s_eth_has_ip = true;
        } else {
            ESP_LOGW(TAG, "Unexpected event %s in state %s",
                     event_name(event), state_name(s_state));
        }
        break;

    // ========== NO_NETWORK ==========
    case NET_STATE_WIFI_PROVISION_FAILED:
        /* CREDENTIALS_SET 已在 switch 之前统一处理，此处不再有分支 */
        if (event == NET_EVT_ETH_LINK_UP) {
            s_state = NET_STATE_ETH_CONNECTING;
            eth_dhcp_timer_arm();
        } else if (event == NET_EVT_ETH_GOT_IP) {
            s_eth_has_ip = true;
            set_default_route(NET_IF_ETH);
            s_state = NET_STATE_ETH_ONLINE;
            post_online(NET_IF_ETH);
        } else if (event == NET_EVT_WIFI_DISCONNECTED) {
            /* ⚠️ 刻意什么都不做。这正是本状态存在的意义：**不重试**。
               别在这里"顺手"补 schedule_background_retry() —— 那会把
               交互式配网又变回后台静默重连。 */
        } else if (event == NET_EVT_WIFI_GOT_IP) {
            /* 极少数：报错之后驱动自己连上了。照常收编，别浪费。 */
            s_wifi_has_ip = true;
            s_prov_state  = PROV_OK;
            wifi_got_ip_online();
        } else {
            ESP_LOGW(TAG, "Unexpected event %s in state %s",
                     event_name(event), state_name(s_state));
        }
        break;

    case NET_STATE_NO_NETWORK:
        /* CREDENTIALS_SET 已在 switch 之前统一处理，此处不再有分支 */
        if (event == NET_EVT_ETH_LINK_UP) {
            s_state = NET_STATE_ETH_CONNECTING;
            eth_dhcp_timer_arm();
        } else if (event == NET_EVT_WIFI_GOT_IP) {
            s_wifi_has_ip = true;
            wifi_got_ip_online();
        } else if (event == NET_EVT_ETH_GOT_IP) {
            s_eth_has_ip = true;
            set_default_route(NET_IF_ETH);
            s_state = NET_STATE_ETH_ONLINE;
            post_online(NET_IF_ETH);
        } else if (event == NET_EVT_WIFI_CONNECTED) {
            /* 后台重连关联上了 —— 等 GOT_IP */
        } else if (event == NET_EVT_WIFI_DISCONNECTED) {
            /* ⚠️ 后台重连又失败了，必须把重试链续上，否则死在这里。
               与 SOFTAP_ONLY 那条同理 —— 本状态以前根本不处理这个事件，
               只打一句 "Unexpected event"，于是有凭据的设备落到 NO_NETWORK
               之后就再也不会重试了。 */
            wifi_mgr_schedule_background_retry();
        } else {
            ESP_LOGW(TAG, "Unexpected event %s in state %s",
                     event_name(event), state_name(s_state));
        }
        break;
    }

    // ---- Log + broadcast state change ----
    if (s_state != old) {
        ESP_LOGI(TAG, "State: %s -> %s", state_name(old), state_name(s_state));
        esp_event_post(NETWORK_EVENT, NETWORK_EVENT_STATE_CHANGED,
                       &s_state, sizeof(s_state), 0);
    }

    /* ⚠️ 不要在这里「地址变了就踢 EZSP 客户端」。db7cab8（2026-08-21）加过，
       2026-08-22 撤回 —— 判据不对，而且实测在自伤：

       1. 它跟踪的是「默认路由指向哪个 IP」，而一条 TCP 连接的存活取决于
          「这条连接自己的本地地址还在不在」。监听 socket 绑 0.0.0.0（设计决策 2），
          双上行时两个地址上的 :6638 都在服务，默认路由翻转与已建立连接的
          存活性**毫无关系**。
       2. 现场实证（2026-08-21 23:53 与 2026-08-22 00:33，各一次拔插网线）：
              [W] 对外地址变化 .43 -> .44 —— 强制断开 EZSP 客户端
              [W] 对外地址变化 .44 -> .43 —— 强制断开 EZSP 客户端
          一次拔插踢**两次**，而另一个地址全程在正常服务。两次都是纯自伤。
       3. 地址**真的**消失时协议栈已经做了，逐连接、更准更狠：
          netif_do_set_ipaddr 会逐 pcb 比对 local_ip 并 tcp_abort()（真 RST）。
          拔网线、WiFi 掉线、DHCP 续租换址三条路径都会走到。
       4. 这个能力 2026-08-21 之前根本不存在（用 git log -S 查那个已删的
          kick 函数名，只有 db7cab8 一条），产品在没有它的情况下过了 Phase 4 验收。

       地址变化只记录，不断连接。 */
    {
        network_status_t ns;
        network_mgr_get_status(&ns);
        ESP_LOGD(TAG, "addr snapshot: eth=" IPSTR " wifi=" IPSTR " active=%d",
                 IP2STR(&ns.eth_ip), IP2STR(&ns.wifi_ip), (int)ns.active_interface);
    }

#if CONFIG_BITENGINE_WIFI_FLOAT_BOOT_TAKEOVER
    boot_takeover_eval();   /* T9.20：网线插上 / WiFi 掉线要当场作废在途探测，别等定时器 */
#endif
#if CONFIG_BITENGINE_SVC_IDENTITY
    svc_id_update();      /* T9.37：服务身份的持有者（变了就交接）；身份生效时下面的静态浮动不动作 */
#endif
#if CONFIG_BITENGINE_WIFI_FLOAT_ETH_IP
    float_svc_update();   /* 状态settled后统一 reconcile 浮动服务 IP */
#endif
#if CONFIG_BITENGINE_MDNS_SVC_IP_ONLY
    mdns_self_update();   /* 浮动 / 以太网地址 / 开机接管都定了之后，再定本机 mDNS 宣告哪个地址 */
#endif
    xSemaphoreGiveRecursive(s_sm_mutex);
}

// ---- Periodic status log ----

/* 看门狗 L3 恢复入口：把 STA 断开事件喂给状态机，让它走完整的重连/回退流程
   （包括以太网优先与 SoftAP 兜底）。不直接调 esp_wifi_connect()——那会绕过
   状态机，和它自己的重试计数打架。 */
esp_err_t network_mgr_recover(void)
{
    ESP_LOGW(TAG, "网络恢复：重新触发状态机（看门狗 L3）");
    network_state_machine(NET_EVT_WIFI_DISCONNECTED);
    return ESP_OK;
}

static void status_log_task(void *arg)
{
    while (1) {
        wd_kick(s_wd_slot_net_status);
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "Net: state=%s iface=%s wifi_ip=%d eth_ip=%d softap=%d",
                 state_name(s_state),
                 s_active_if == NET_IF_ETH ? "ETH" :
                 s_active_if == NET_IF_WIFI ? "WiFi" : "NONE",
                 s_wifi_has_ip, s_eth_has_ip, s_softap_on);
    }
}

// ---- Public API ----

/* Apply persisted static IP configuration (config_mgr stores it, but
   nothing applied it at boot — static IP setups never worked). */
static void apply_static_ip_config(void)
{
    const eth_config_t *e = config_mgr_get_eth();
    if (!e->dhcp && e->static_ip != 0 && eth_mgr_get_netif()) {
        esp_netif_ip_info_t info = {
            .ip.addr      = e->static_ip,
            .gw.addr      = e->static_gateway,
            .netmask.addr = e->static_netmask ? e->static_netmask
                                              : esp_netif_htonl(0xFFFFFF00),
        };
        if (network_mgr_set_eth_ip(&info) == ESP_OK && e->static_dns) {
            esp_netif_dns_info_t dns = { .ip.u_addr.ip4.addr = e->static_dns,
                                         .ip.type = ESP_NETIF_DNS_MAIN };
            esp_netif_set_dns_info(eth_mgr_get_netif(), ESP_NETIF_DNS_MAIN, &dns);
        }
    }

    const wifi_config_data_t *w = config_mgr_get_wifi();
    esp_netif_t *sta = wifi_mgr_get_sta_netif();
    if (w->static_ip != 0 && sta) {
        /* ⚠️ 这里的 stop→set 配对必须成对成功，否则 netif 会停在
           「dhcpc_status=STOPPED + ip_info 已清零」—— 和 2026-08-21 撤回的
           release_dhcp_lease 完全同一形态：链路恢复时 esp_netif_action_connected
           落到 STOPPED 分支、ip 无效 → ESP_LOGE("invalid static ip") 且不启 DHCP，
           那条腿到整机重启为止拿不到 IP。发生在 WiFi 腿上时没有网线可退。

           **目前够不着**（2026-08-22 核对 IDF v6.0.2）：
           esp_netif_set_ip_info 只有两种失败 ——
             · esp_netif==NULL 或 ip_info==NULL（:2037）：两者此处都非空
             · dhcpc_status != STOPPED（:2046）：上一行 dhcpc_stop 无条件置
               STOPPED（esp_netif_lwip.c:1638），所以恒满足
           唯一窄缝是 dhcpc_stop 在 lwip_netif==NULL 且状态 STARTED 时提前返回
           不置 STOPPED（:1630），而那要求 netif 没建好 —— 本函数在
           network_mgr_init() 里跑，wifi_mgr 已初始化过。

           所以不做回滚重构（改一个够不着的失败路径收益为零，而这个文件每动
           一次都有失联风险）。但**必须留一条 W 级日志**：万一真走到这儿，
           你会得到一块没有 IP 的 WiFi 网卡，而原来这里是**完全静默**的。
           2026-08-21 那个缺陷的教训正是「有害的那条日志是 I 级，代价 23 秒后
           才以一条 E 出现」—— 失败路径不留痕，等于放弃诊断。 */
        esp_netif_dhcpc_stop(sta);
        esp_netif_ip_info_t info = {
            .ip.addr      = w->static_ip,
            .gw.addr      = w->static_gateway,
            .netmask.addr = w->static_netmask ? w->static_netmask
                                              : esp_netif_htonl(0xFFFFFF00),
        };

        esp_err_t sip = esp_netif_set_ip_info(sta, &info);
        if (sip == ESP_OK) {
            ESP_LOGI(TAG, "WiFi static IP applied: " IPSTR, IP2STR(&info.ip));
            if (w->static_dns) {
                esp_netif_dns_info_t dns = { .ip.u_addr.ip4.addr = w->static_dns,
                                             .ip.type = ESP_NETIF_DNS_MAIN };
                esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns);
            }
        } else {
            ESP_LOGE(TAG, "WiFi 静态 IP 设置失败(%s) —— DHCP 已停、IP 已清空，"
                          "这条腿在重启前拿不到 IP", esp_err_to_name(sip));
        }
    }
}

#if CONFIG_BITENGINE_MDNS_SVC_IP_ONLY
/* ---- T9.34：本机 mDNS 只宣告以太网服务 IP（路线 2，2026-10-07）----
 *
 * 行为说明的正本是 Kconfig BITENGINE_MDNS_SVC_IP_ONLY 的 help，判据是 svc_policy.c 的 svc_mdns_decide，
 * mdns 侧的改动是 tools/patch_mdns.py 第四组（SVCIP）：本机 A 记录的地址由下面的钩子给，两口同网段时固定
 * STA 应答。这里只做三件事：算结论（持 s_sm_mutex）、发布快照给钩子、结论变了触发一次宣告
 * （宣告里的 A 带缓存刷新位，别人缓存里的旧地址 1 s 内被清掉，不另做撤销）。
 *
 * 上下文（S2）：
 *   - mdns_self_update 只在持 s_sm_mutex 的事件循环（sys_evt）里调：状态机末尾、svc_gate_work 末尾、
 *     复查定时器投回来的 NET_DEFER_MDNS_RECHECK；network_mgr_init 里调一次（那时还没有并发）；
 *   - 钩子 bitengine_mdns_self_ipv4 跑在 mdns 任务里（优先级 1）：只在 s_mdns_mux 下拷两个字，
 *     不拿锁、不阻塞、不调会 IPC 的 esp_netif_get_handle_from_ifkey（STA / ETH 句柄在 init 里缓存）；
 *   - mdns_netif_action 入 mdns 的动作队列是 0 超时（mdns_service.c）；但口的缓存指针被清过（每次关口之后）时，
 *     它会用 esp_netif_get_handle_from_ifkey 同步等 lwIP 线程一下 —— 不会死锁（lwIP 线程从不拿 s_sm_mutex，
 *     状态机持锁时本来也做这类调用），只是会短暂等待。队列满时它静默丢掉、照样返回 ESP_OK，所以换地址后 2 s 补一次。 */
#define SVC_MDNS_HOLD_CAP_MS   60000      /* 开机最多憋 60 s（等对时那类）；插线开机实测约 9.6 s、拔线接管约 30 s */
_Static_assert(SVC_MDNS_DEFAULT == 0 && SVC_MDNS_FIXED == 1 && SVC_MDNS_HOLD == 2,
               "svc_mdns_mode_t must match BITENGINE_SELF_* in tools/patch_mdns.py (SVCIP)");

static portMUX_TYPE        s_mdns_mux  = portMUX_INITIALIZER_UNLOCKED;
static svc_mdns_mode_t     s_mdns_mode = SVC_MDNS_DEFAULT;   /* 钩子读；init 之前 = 上游行为 */
static uint32_t            s_mdns_addr;                      /* FIXED 时的服务 IP（网络序） */
static esp_netif_t        *s_mdns_sta;                       /* 钩子只认这两个口；SoftAP 照上游 */
static esp_netif_t        *s_mdns_eth;
static int64_t             s_mdns_init_us;
static esp_timer_handle_t  s_mdns_recheck_timer;
static int64_t             s_mdns_reannounce_us;             /* 非 0：到这个时刻再宣告一次（补被丢的那次） */
#define SVC_MDNS_REANNOUNCE_US (2000 * 1000)

/* mdns 任务：本机 A 记录用哪个地址（强定义；弱兜底在 mdns_send.c，见 patch_mdns.py SVCIP ①）。
   返回值即 BITENGINE_SELF_*：0 上游行为 / 1 只宣告 *out / 2 暂不给。 */
int bitengine_mdns_self_ipv4(esp_netif_t *netif, esp_ip4_addr_t *out);
int bitengine_mdns_self_ipv4(esp_netif_t *netif, esp_ip4_addr_t *out)
{
    if (!netif || (netif != s_mdns_sta && netif != s_mdns_eth)) return SVC_MDNS_DEFAULT;
    taskENTER_CRITICAL(&s_mdns_mux);
    svc_mdns_mode_t m = s_mdns_mode;
    uint32_t        a = s_mdns_addr;
    taskEXIT_CRITICAL(&s_mdns_mux);
    if (m == SVC_MDNS_FIXED) {
        if (!a) return SVC_MDNS_DEFAULT;
        /* 一个口只在服务 IP 落在它的网段里（或正挂在它身上）时才宣告服务 IP：两口分属不同网段时，另一边的
           A 记录给服务 IP 是不可达的（RFC 6762 §6.2），照上游给本口自己的地址（2026-10-07 审计）。
           T9.34 时只有 WiFi 口要这道闸门（网口宣告的永远是自己的地址）；T9.37 起网口也可能接在另一张网上、
           或开机时 NVS 里的服务地址还是旧网络的 —— 两口一视同仁（第二轮审计）。
           esp_netif_get_ip_info 只拷 netif 上缓存的地址，不走 IPC（上游 mdns 在本任务里也这么调） */
        {
            esp_netif_ip_info_t si;
            if (esp_netif_get_ip_info(netif, &si) != ESP_OK) return SVC_MDNS_DEFAULT;
            /* 地址为 0：插回网线、服务 IP 刚从 WiFi 收回、WiFi 还在重新拿 DHCP 那几秒 —— mdns 不知道（没有事件），
               本口照样应答；退回上游就会宣告 A=0.0.0.0（带刷新位），所以这里暂不给（2026-10-07 审计） */
            if (!si.ip.addr) return SVC_MDNS_HOLD;
            if (si.ip.addr != a &&
                !svc_same_subnet(lwip_ntohl(a), lwip_ntohl(si.ip.addr), lwip_ntohl(si.netmask.addr))) {
                return SVC_MDNS_DEFAULT;
            }
        }
        if (out) out->addr = a;
    }
    return (int)m;
}

/* esp_timer 任务：只投递（T9.28）。投不出去就重排自己。 */
static void mdns_recheck_cb(void *arg)
{
    (void)arg;
    if (esp_event_post(NET_DEFER_EVENT, NET_DEFER_MDNS_RECHECK, NULL, 0, 0) != ESP_OK) {
        (void)esp_timer_start_once(s_mdns_recheck_timer, NET_DEFER_RETRY_US);
    }
}

static const char *mdns_mode_name(svc_mdns_mode_t m)
{
    switch (m) {
    case SVC_MDNS_FIXED: return "只宣告服务 IP";
    case SVC_MDNS_HOLD:  return "暂不给（等服务 IP 定下来）";
    default:             return "WiFi 自己的地址";
    }
}

/* 持 s_sm_mutex：触发 STA / ETH 两口各一次宣告；没就绪的口 mdns 自己忽略。mdns 还没起来时返回 INVALID_STATE：
   它起来时会按当时的结论宣告，不用补 */
static void mdns_self_announce(void)
{
    if (s_mdns_sta) (void)mdns_netif_action(s_mdns_sta, MDNS_EVENT_ANNOUNCE_IP4 | MDNS_EVENT_ANNOUNCE_IP6);
    if (s_mdns_eth) (void)mdns_netif_action(s_mdns_eth, MDNS_EVENT_ANNOUNCE_IP4 | MDNS_EVENT_ANNOUNCE_IP6);
}

/* 持 s_sm_mutex：算结论、发布、变了就宣告（2 s 后补一次）；有到点会变的条件就排一次复查。幂等。 */
static void mdns_self_update(void)
{
    int64_t now = esp_timer_get_time();
    /* 链路取两个来源（同开机接管闸门，审计 F3）：init 里采样之后、注册事件处理之前链路正好起来，那个 LINK_UP 会漏掉 */
    bool link = s_eth_link_up || eth_mgr_link_is_up();
    int64_t link_us = s_eth_link_up_us ? s_eth_link_up_us : s_mdns_init_us;
    svc_mdns_in_t in = {
        /* 以太网"有地址"同时要求链路在。2026-10-07 审计时 s_eth_has_ip 在不处理 LINK_DOWN 的状态里会卡在 true；
           T9.40 起断链在状态机 switch 之前统一清，这里的 && link 留作兜底（链路事件与本函数之间的窗口） */
        .eth_has_ip       = s_eth_has_ip && link,
        .svc_on_wifi      = s_svc_on_wifi,
        .svc_ip_valid     = s_svc_ip_valid,
        .eth_link_up      = link,
        .eth_dhcp_pending = link && !s_eth_has_ip && now - link_us < (int64_t)ETH_DHCP_TIMEOUT_MS * 1000,
#if CONFIG_BITENGINE_WIFI_FLOAT_BOOT_TAKEOVER
        .bt_pending       = (s_bt_state == SVC_BT_WAIT || s_bt_state == SVC_BT_PROBING),
#endif
        .since_init_ms    = (now - s_mdns_init_us) / 1000,
        .hold_cap_ms      = SVC_MDNS_HOLD_CAP_MS,
#if CONFIG_BITENGINE_SVC_IDENTITY
        /* T9.37：身份机制生效时只看这两个 —— 服务地址就是 A_S（svc_id_update 已镜像进 s_svc_ip） */
        .id_active        = svc_id_on(),
        .id_known         = s_id_rec.s_nic != SVC_NIC_NONE && s_id_rec.ip != 0,
#endif
    };
    svc_mdns_mode_t m = svc_mdns_decide(&in);
    uint32_t a = (m == SVC_MDNS_FIXED && s_svc_ip_valid) ? s_svc_ip.ip.addr : 0;
    if (m == SVC_MDNS_FIXED && !a) m = SVC_MDNS_DEFAULT;   /* 防御：判 FIXED 却没有服务 IP —— 不宣告 0.0.0.0 */

    bool changed;
    taskENTER_CRITICAL(&s_mdns_mux);
    changed = (m != s_mdns_mode || a != s_mdns_addr);
    s_mdns_mode = m;
    s_mdns_addr = a;
    taskEXIT_CRITICAL(&s_mdns_mux);

    if (changed) {
        ESP_LOGI(TAG, "mDNS 本机地址：%s%s" IPSTR, mdns_mode_name(m), m == SVC_MDNS_FIXED ? " " : "",
                 IP2STR((esp_ip4_addr_t *)&(esp_ip4_addr_t){ .addr = a }));
        /* HOLD 不宣告：宣告里没有本机地址，也没意义 */
        if (m != SVC_MDNS_HOLD) {
            mdns_self_announce();
            /* mdns 的动作队列满时 mdns_netif_action 会静默丢掉、照样返回 ESP_OK（mdns_netif.c 的
               post_custom_action），而"只用 WiFi → 插上网线"这种转换没有别的路径补宣告：2 s 后再宣告一次 */
            s_mdns_reannounce_us = now + SVC_MDNS_REANNOUNCE_US;
        } else {
            s_mdns_reannounce_us = 0;
        }
    } else if (s_mdns_reannounce_us && now >= s_mdns_reannounce_us) {
        s_mdns_reannounce_us = 0;
        if (m != SVC_MDNS_HOLD) mdns_self_announce();
    }

    /* 到点会变的两个条件：等 DHCP 到超时、开机等待到上限。没有事件会在那一刻来推，得自己排复查 */
    int64_t due = s_mdns_reannounce_us;
    if (in.eth_dhcp_pending) {
        int64_t d = link_us + (int64_t)ETH_DHCP_TIMEOUT_MS * 1000;
        if (!due || d < due) due = d;
    }
    if (m == SVC_MDNS_HOLD) {
        int64_t cap = s_mdns_init_us + (int64_t)SVC_MDNS_HOLD_CAP_MS * 1000;
        if (!due || cap < due) due = cap;
    }
    if (due && s_mdns_recheck_timer) {
        int64_t wait = due - now + 50 * 1000;             /* 多等 50 ms，到点时条件确实已翻过去 */
        if (wait < 0) wait = 0;
        esp_timer_stop(s_mdns_recheck_timer);
        (void)esp_timer_start_once(s_mdns_recheck_timer, (uint64_t)wait);
    }
}
#endif /* CONFIG_BITENGINE_MDNS_SVC_IP_ONLY */

/* 事件循环（sys_evt）：esp_timer 回调投进来的活（T9.28）。与 net_events.c 的处理函数同一个任务，
   所以这里拿 s_sm_mutex 与以前状态机的其余入口完全一样。 */
static void defer_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    switch (id) {
    case NET_DEFER_ETH_DHCP_TIMEOUT:
        if (data) eth_dhcp_timeout_work(*(const uint32_t *)data);
        break;
    case NET_DEFER_SM_EVENT:
        if (data) {
            net_event_t ev = *(const net_event_t *)data;
            network_state_machine(ev);
            if (ev == NET_EVT_WIFI_RECONNECT_EXHAUSTED) {
                /* 栈余量随手打出来：这条路以前在 esp_timer 任务（3584 B）上跑，T9.28 起在 sys_evt（2304 B） */
                ESP_LOGI(TAG, "WiFi 重连耗尽已处理（sys_evt 栈余 %u）", (unsigned)uxTaskGetStackHighWaterMark(NULL));
            }
        }
        break;
#if CONFIG_BITENGINE_WIFI_FLOAT_BOOT_TAKEOVER
    case NET_DEFER_SVC_GATE:
        svc_gate_work();
        break;
#endif
#if CONFIG_BITENGINE_SVC_IDENTITY
    case NET_DEFER_SVC_ID:
        xSemaphoreTakeRecursive(s_sm_mutex, portMAX_DELAY);
        svc_id_mac_only_stamp();      /* 对上时后补记"只认 MAC"开始计时的时刻（svc_id_on 为假时也要做） */
        svc_id_update();
        float_svc_update();           /* 身份机制刚退回旧行为（只认 MAC）时，静态浮动在这里接手 */
#if CONFIG_BITENGINE_MDNS_SVC_IP_ONLY
        mdns_self_update();
#endif
        xSemaphoreGiveRecursive(s_sm_mutex);
        break;
#endif
#if CONFIG_BITENGINE_MDNS_SVC_IP_ONLY
    case NET_DEFER_MDNS_RECHECK:
        xSemaphoreTakeRecursive(s_sm_mutex, portMAX_DELAY);
        mdns_self_update();
        xSemaphoreGiveRecursive(s_sm_mutex);
        break;
#endif
    default:
        ESP_LOGW(TAG, "未知的 NET_DEFER_EVENT id=%d", (int)id);
        break;
    }
}

esp_err_t network_mgr_defer_event(net_event_t event)
{
    return esp_event_post(NET_DEFER_EVENT, NET_DEFER_SM_EVENT, &event, sizeof(event), 0);
}

esp_err_t network_mgr_init(void)
{
    ESP_LOGI(TAG, "Initializing network manager");

    s_sm_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_sm_mutex) return ESP_ERR_NO_MEM;

    // 1. Create event loop (may already exist)
    esp_event_loop_create_default();

    // 2. Init netif
    esp_netif_init();

    // 3. Init WiFi
    esp_err_t err = wifi_mgr_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
        return err;
    }

    // 4. Init ETH (may be no-op on DevKit)
    err = eth_mgr_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ETH init failed (degraded mode): %s",
                 esp_err_to_name(err));
        // Continue without ETH — degraded mode
    }

    // 4b. Apply persisted static IP configuration before connecting
    apply_static_ip_config();

    // 5. esp_timer 回调投递状态机活的处理函数（T9.28）—— 必须在任何定时器能到点之前注册
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(NET_DEFER_EVENT, ESP_EVENT_ANY_ID,
                                                            defer_handler, NULL, NULL),
                        TAG, "defer handler register failed");

    // 5a. Create ETH DHCP timeout timer
    const esp_timer_create_args_t dhcp_timer_args = {
        .callback = eth_dhcp_timeout_cb,
        .name     = "eth_dhcp_timeout",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&dhcp_timer_args, &s_eth_dhcp_timer),
                        TAG, "ETH DHCP timer create failed");

#if CONFIG_BITENGINE_WIFI_FLOAT_ETH_IP
    // 5b. 浮动 IP 的 GARP 补发定时器（见 float_garp_announce）
    const esp_timer_create_args_t garp_timer_args = {
        .callback = garp_timer_cb,
        .name     = "float_garp",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&garp_timer_args, &s_garp_timer),
                        TAG, "GARP timer create failed");
    /* 开机就插着、链路已经起来的情况：别让 s_eth_link_up 的初值 false 骗过浮动 / 开机接管的判定 */
    s_eth_link_up = eth_mgr_link_is_up();
#endif

#if CONFIG_BITENGINE_WIFI_FLOAT_BOOT_TAKEOVER
    // 5c. T9.20 开机接管：三个定时器 + 读回 NVS 里的候选（必须在注册事件处理之前）
    s_init_us      = esp_timer_get_time();
    s_reset_reason = esp_reset_reason();
    /* 墙钟出处：只有 esp_restart 且上次开机墙钟可信才继承（见 s_clock_trust）；其余一律清掉，
       等本次对时。上电时 RTC_NOINIT 是随机值，魔数对不上也落到"不可信"。 */
    s_clock_inherited = (s_reset_reason == ESP_RST_SW && s_clock_trust == SVC_CLOCK_TRUSTED);
    if (!s_clock_inherited) s_clock_trust = 0;
    const esp_timer_create_args_t svc_work_args  = { .callback = svc_work_cb,        .name = "svc_work"  };
    const esp_timer_create_args_t svc_probe_args = { .callback = svc_probe_timer_cb, .name = "svc_probe" };
    const esp_timer_create_args_t svc_lease_args = { .callback = svc_lease_timer_cb, .name = "svc_lease" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&svc_work_args, &s_svc_work_timer), TAG, "svc work timer create failed");
    ESP_RETURN_ON_ERROR(esp_timer_create(&svc_probe_args, &s_svc_probe_timer), TAG, "svc probe timer create failed");
    ESP_RETURN_ON_ERROR(esp_timer_create(&svc_lease_args, &s_svc_lease_timer), TAG, "svc lease timer create failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_svc_lease_timer, SVC_LEASE_POLL_US),
                        TAG, "svc lease timer start failed");
#if CONFIG_BITENGINE_SVC_IDENTITY
    svc_id_init();          /* T9.37：先于 svc_boot_load（它看 svc_id_on()）、先于任何 DHCP 起来（第一个 DISCOVER 就要带对身份） */
#endif
    svc_boot_load();
#endif

#if CONFIG_BITENGINE_MDNS_SVC_IP_ONLY
    // 5d. T9.34 本机 mDNS 地址：缓存两口句柄（钩子只比指针）、复查定时器、开机时刻
    s_mdns_init_us = esp_timer_get_time();
    s_mdns_sta     = wifi_mgr_get_sta_netif();
    s_mdns_eth     = eth_mgr_get_netif();                 /* 以太网初始化失败（降级）时为 NULL：钩子照上游 */
    if (s_eth_link_up) s_eth_link_up_us = s_mdns_init_us;  /* 开机就插着：等 DHCP 从现在算 */
    const esp_timer_create_args_t mdns_recheck_args = { .callback = mdns_recheck_cb, .name = "mdns_recheck" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&mdns_recheck_args, &s_mdns_recheck_timer),
                        TAG, "mdns recheck timer create failed");
#endif

    // 6. Register event handlers
    ESP_RETURN_ON_ERROR(net_events_register(), TAG, "Event registration failed");

    // 7. Determine initial state
    /* Event handlers are already registered (step 6), and wifi_mgr_start_softap() / start_sta()
       block for a few hundred ms. Hold the state-machine lock (recursive) so an Ethernet event
       arriving meanwhile waits for the real starting state instead of being dropped in INIT. */
    xSemaphoreTakeRecursive(s_sm_mutex, portMAX_DELAY);
    if (wifi_mgr_has_credentials()) {
        if (eth_mgr_link_is_up()) {
            s_state = NET_STATE_ETH_CONNECTING;
            eth_dhcp_timer_arm();
            wifi_mgr_start_sta();
            ESP_LOGI(TAG, "State: INIT -> ETH_CONNECTING (+ WiFi STA)");
        } else {
            s_state = NET_STATE_WIFI_CONNECTING;
            wifi_mgr_start_sta();
            ESP_LOGI(TAG, "State: INIT -> WIFI_CONNECTING");
        }
    } else if (wifi_mgr_start_softap() == ESP_OK) {
        s_state = NET_STATE_SOFTAP_ONLY;
        s_softap_on = true;
        ESP_LOGI(TAG, "State: INIT -> SOFTAP_ONLY (no WiFi credentials)");
    } else {
        /* The hotspot was refused (wifi_mgr has already put the mode back to STA). Say so
           instead of claiming SOFTAP_ONLY with no AP on air; NO_NETWORK still goes online
           when an Ethernet cable brings an address. */
        s_state = NET_STATE_NO_NETWORK;
#if CONFIG_BITENGINE_NET_ETH_ONLY
        /* 纯以太网剖面每次开机都走到这里（wifi_none.c：没凭据、热点起不来），这是常态不是故障。
           E 级会进日志告警环，每次开机一条"SoftAP failed"会把排障的人带偏（审计 2026-10-21）。 */
        ESP_LOGI(TAG, "State: INIT -> NO_NETWORK (Ethernet only, waiting for the cable / DHCP)");
#else
        ESP_LOGE(TAG, "State: INIT -> NO_NETWORK (no WiFi credentials, SoftAP failed)");
#endif
    }
    xSemaphoreGiveRecursive(s_sm_mutex);

#if CONFIG_BITENGINE_MDNS_SVC_IP_ONLY
    /* mdns 还没起来（main.c 里它在 network_mgr_init 之后），这里只是把开机时的结论先发布给钩子：
       插着网线 / 有开机接管候选 → 先不给地址；否则照旧。事件处理已注册，状态机可能已经在跑，所以拿锁 */
    xSemaphoreTakeRecursive(s_sm_mutex, portMAX_DELAY);
    mdns_self_update();
    xSemaphoreGiveRecursive(s_sm_mutex);
#endif

    esp_event_post(NETWORK_EVENT, NETWORK_EVENT_STATE_CHANGED,
                   &s_state, sizeof(s_state), 0);

    // 7. Start periodic status log
    static const wd_task_spec_t wd_spec = {
        .name = "net_status", .deadline_ms = 180000, .severity = WD_SEV_OBSERVE,
    };
    s_wd_slot_net_status = wd_register(&wd_spec);
    wd_register_recovery("network", network_mgr_recover);
    /* 2560（原 2048）：实测 stack_left 只剩 848，是全表最紧的一个。
       栈在 PSRAM，加大不占内部 SRAM —— 安全优先。 */
    psram_task_create(status_log_task, "net_status", 2560, NULL, 5, NULL);

    return ESP_OK;
}

net_state_t network_mgr_get_state(void)
{
    return s_state;
}

esp_err_t network_mgr_get_status(network_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    out->state = s_state;
    out->active_interface = s_active_if;

    // ETH
    out->eth_link_up = eth_mgr_link_is_up();
    out->eth_has_ip  = s_eth_has_ip;
    esp_netif_t *eth_nif = eth_mgr_get_netif();
    if (eth_nif && s_eth_has_ip) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(eth_nif, &info) == ESP_OK) {
            out->eth_ip      = info.ip;
            out->eth_gateway = info.gw;
            out->eth_netmask = info.netmask;
        }
    }

    // WiFi STA
    out->wifi_connected = wifi_mgr_is_connected();
    out->wifi_has_ip    = s_wifi_has_ip;
    esp_netif_t *sta_nif = wifi_mgr_get_sta_netif();
    if (sta_nif && s_wifi_has_ip) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(sta_nif, &info) == ESP_OK) {
            out->wifi_ip = info.ip;
        }
    }

    out->softap_active = wifi_mgr_softap_active();
#if !CONFIG_BITENGINE_NET_ETH_ONLY
    /* 纯以太网剖面（设计 2.2）：wifi_none.c 让 wifi_connected / softap_active 恒为 false，
       这几个驱动查询也就不编译 —— 编进来就会把 Wi-Fi 库链接进来 */

    /* 只在真的连上时才问驱动要 RSSI/SSID。没连上时这个 ioctl 除了打一条
       `Haven't to connect to a suitable AP now!` 什么也拿不到，而本函数被
       ws_push_task 每 2s 调一次。 */
    if (out->wifi_connected) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out->wifi_rssi    = ap.rssi;
            out->wifi_channel = ap.primary;
            strlcpy(out->wifi_ssid, (char *)ap.ssid, sizeof(out->wifi_ssid));
        }
    }

    if (out->softap_active) {
        wifi_config_t ap_cfg;
        if (esp_wifi_get_config(WIFI_IF_AP, &ap_cfg) == ESP_OK) {
            strlcpy(out->softap_ssid, (char *)ap_cfg.ap.ssid,
                    sizeof(out->softap_ssid));
        }
        wifi_sta_list_t sta_list;
        if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
            out->softap_clients = sta_list.num;
        }
    }
#endif

    return ESP_OK;
}

esp_err_t network_mgr_set_wifi(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;
#if CONFIG_BITENGINE_NET_ETH_ONLY
    /* 纯以太网剖面没有 Wi-Fi（设计 2.2）：不存凭据、不进状态机。/wifi/config 在这个剖面里也不注册 */
    (void)password;
    return ESP_ERR_NOT_SUPPORTED;
#else

    /* 先把当前凭据拍个快照，再覆盖 —— 新的连不上时要退回去。
       只有本来就有凭据才值得存：首次配网没有可回滚的对象。 */
    const wifi_config_data_t *cur = config_mgr_get_wifi();
    s_prev_valid = cur->has_credentials;
    if (s_prev_valid) {
        strlcpy(s_prev_ssid, cur->ssid, sizeof(s_prev_ssid));
        strlcpy(s_prev_pass, cur->password, sizeof(s_prev_pass));
    }

    // Save via config_mgr (handles NVS + debounce + event)
    esp_err_t err = config_mgr_set_wifi(ssid, password);
    if (err != ESP_OK) return err;

    /* 对外广播「凭据已更新」。无 payload——订阅方要 SSID 自己去问 config_mgr，
       别让凭据在事件循环里传来传去。发在这里而不是下面的状态机分支里：
       那个分支只在 SOFTAP_ONLY / NO_NETWORK 下触发，而"凭据被改了"这件事
       在任何状态下都成立（比如 WIFI_ONLINE 时换了个 AP）。 */
    esp_event_post(NETWORK_EVENT, NETWORK_EVENT_WIFI_CREDENTIALS_SET, NULL, 0, 0);

    /* 断开旧连接。真正的重连由状态机的 CREDENTIALS_SET 分支发起 ——
       那里会先设好 s_provisioning 再连。 */
    wifi_mgr_reset_reconnect();
    esp_wifi_disconnect();

    /* 任何状态都要进状态机：配网这件事在 WIFI_ONLINE 下同样成立
       （用户换了个 AP），而且只有走状态机才会标记 s_provisioning。
       以前只在 SOFTAP_ONLY / NO_NETWORK 下触发，是这块最隐蔽的一个洞。 */
    network_state_machine(NET_EVT_WIFI_CREDENTIALS_SET);

    return ESP_OK;
#endif /* CONFIG_BITENGINE_NET_ETH_ONLY */
}

esp_err_t network_mgr_scan_wifi(wifi_ap_record_t *results, uint16_t *count)
{
    return wifi_mgr_scan_5g(results, count);
}

esp_err_t network_mgr_set_eth_ip(const esp_netif_ip_info_t *ip_info)
{
    esp_netif_t *eth_nif = eth_mgr_get_netif();
    if (!eth_nif) return ESP_ERR_NOT_SUPPORTED;

    if (ip_info) {
        // Static IP
        esp_netif_dhcpc_stop(eth_nif);
        ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(eth_nif, ip_info),
                            TAG, "Set ETH static IP failed");
        ESP_LOGI(TAG, "ETH static IP set: " IPSTR, IP2STR(&ip_info->ip));
    } else {
        // Back to DHCP
        ESP_RETURN_ON_ERROR(esp_netif_dhcpc_start(eth_nif),
                            TAG, "Start ETH DHCP failed");
        ESP_LOGI(TAG, "ETH switched to DHCP");
    }

    return ESP_OK;
}

prov_state_t network_mgr_provision_state(void) { return s_prov_state; }

int network_mgr_wifi_last_reason(void)
{
    return wifi_mgr_last_disconnect_reason();
}

void network_mgr_wifi_link_stats(wifi_link_stats_t *out)
{
    if (!out) return;
    wifi_mgr_link_stats(out);
}

/* REST (httpd task) side. Take the state-machine lock: its branches read s_softap_on and stop
   the AP, and the flag must not change under them. */
esp_err_t network_mgr_start_softap(void)
{
    if (!s_sm_mutex) return ESP_ERR_INVALID_STATE;   /* before network_mgr_init() */
    xSemaphoreTakeRecursive(s_sm_mutex, portMAX_DELAY);
    /* Ask wifi_mgr too: wifi_mgr_softap_recheck() may have stopped a hotspot it could not move
       back to 5 GHz, and a stale s_softap_on must not turn the next start into a no-op. */
    esp_err_t err = ESP_OK;
    if (!s_softap_on || !wifi_mgr_softap_active()) {
        /* Only after it really started: a refused hotspot left marked on would make every
           later start a no-op and every "stop the AP" branch act on nothing. */
        err = wifi_mgr_start_softap();
        if (err == ESP_OK) s_softap_on = true;
    }
    xSemaphoreGiveRecursive(s_sm_mutex);
    return err;
}

esp_err_t network_mgr_stop_softap(void)
{
    if (!s_sm_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(s_sm_mutex, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (s_softap_on) {
        s_softap_on = false;
        err = wifi_mgr_stop_softap();
    }
    xSemaphoreGiveRecursive(s_sm_mutex);
    return err;
}

/* 问 wifi_mgr 而不是回 s_softap_on：状态机里几处直接调 wifi_mgr_start/stop_softap()，
   两个标志不是永远一致，AP 到底在不在广播只有 wifi_mgr 说了算。 */
bool network_mgr_softap_active(void)
{
    return wifi_mgr_softap_active();
}

bool network_mgr_sta_connected(void)
{
    return wifi_mgr_is_connected();
}
