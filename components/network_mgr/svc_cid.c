/* components/network_mgr/svc_cid.c — 每张卡的 DHCP client-id + lwIP 钩子（T9.37 服务身份）
 *
 * 为什么要自己附 option 61（设计 docs/16 §3 / §11）：路由器按 client-id 认租约（10-07 实测本机路由器：陌生 MAC 带网关
 * WiFi 的 client-id 拿到的就是 WiFi 的地址）。IDF 默认给每张卡报 01:<本卡 MAC>，服务地址就只能由"拿到它的那张卡"续租。
 * 让持有者报服务身份 S，服务地址就能由任一张卡长期持有、正常续租，不用换 MAC。
 *
 * H1（IDF v6.0.2 源码核过）：
 *   - 钩子由 dhcp.c 在每个出站报文组好标准选项之后、加结束符之前调用（DISCOVER / REQUEST / DECLINE / RELEASE / INFORM），
 *     参数 state 是 dhcp->state、msg_type 是报文类型；选项直接写进 msg->options[*len ..]，写多少就把 *len 加多少，
 *     容量 DHCP_OPTIONS_LEN（本配置 69）—— 与 IDF 默认的 dhcp_append_extra_opts 同一写法；
 *   - IDF 默认在 BOUND 态（RELEASE 就在这个态发）不附 option 61。这里**所有报文都附**：交接时停 DHCP 发出的 RELEASE
 *     要带哑身份才对不上租约（esp_netif_dhcpc_stop → dhcp_release_and_stop 会发 RELEASE）；
 *   - 格式 01 + 6 字节 MAC（类型 1 = 以太网硬件地址），与 IDF 默认逐字节相同：本卡身份下现有租约不变。
 *
 * 上下文（S2）：钩子在 lwIP 线程（tcpip）里跑，只在 s_cid_mux 临界区里拷几个字节，不拿锁、不阻塞、不打日志。
 * 表由 network_mgr.c 在事件循环里改（svc_cid_set）。 */
#include "sdkconfig.h"

/* 钩子头文件在 IDF 默认的 option 61 关着时就接管（见 lwip_hooks/bitengine_lwip_hooks.h）：这里跟它同一个条件，
   服务身份关着时表不绑定，钩子一律报本卡 MAC */
#if CONFIG_LWIP_DHCP_DISABLE_CLIENT_ID
#include "svc_cid.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_netif_net_stack.h"   /* esp_netif_get_netif_impl */
#include "lwip/netif.h"
#include "lwip/prot/dhcp.h"        /* struct dhcp_msg, DHCP_OPTION_CLIENT_ID */
#include "lwip_default_hooks.h"    /* dhcp_append_extra_opts（IDF 默认那份，保留它附的其余选项） */
#include "lwip_hooks/bitengine_lwip_hooks.h"   /* 钩子的声明：让编译器核对下面的定义与 lwIP 侧调用的签名一致 */
#include "esp_mac.h"                   /* esp_read_mac：WiFi STA 的 MAC 在 WiFi 启动前就要用 */

#define CID_LEN 7                  /* 类型 1 字节 + 6 字节 */

static portMUX_TYPE   s_cid_mux = portMUX_INITIALIZER_UNLOCKED;
static struct netif  *s_eth_lw;    /* bind 之后不再变；钩子在临界区里读 */
static struct netif  *s_sta_lw;
static svc_cid_t      s_kind[3] = { SVC_CID_SELF, SVC_CID_SELF, SVC_CID_SELF };   /* 按 svc_nic_t 下标 */
static volatile bool  s_declined[3];   /* 按 svc_nic_t 下标；钩子置、network_mgr 清 */
static volatile uint8_t s_sent[3];     /* 按 svc_nic_t 下标：最近一个 REQUEST 实际带的 svc_cid_t（钩子写、network_mgr 读） */
/* 两张卡的 MAC（按 svc_nic_t 下标，s_cid_mux 保护）。不能在发报文时才去读对方 lwIP netif 的 hwaddr：开机时对方可能还没启动、
   hwaddr_len 是 0（10-08 隔离网段实测：网口链路先起来，WiFi 还没启动，"报 WiFi 的身份"悄悄退成了报本卡的） */
static uint8_t        s_mac[3][6];
static bool           s_mac_ok[3];
static volatile uint32_t s_renew_reqs[3];   /* 按 svc_nic_t 下标；只有 lwIP 线程写 */
static volatile uint32_t s_rebind_reqs[3];

/* 交接时停 DHCP 用的身份：类型 0（非硬件标识）+ 6 个字母，不会与任何真实客户端的 client-id 相同，
   带它的 RELEASE 对不上任何租约，路由器忽略（RFC 2131 §4.4.6：按 client-id + ciaddr 认租约） */
static const uint8_t DUMMY_CID[CID_LEN] = { 0x00, 'B', 'E', 'H', 'O', 'F', 'F' };

static bool mac_nonzero(const uint8_t m[6])
{
    return (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]) != 0;
}

void svc_cid_bind(esp_netif_t *eth, esp_netif_t *sta)
{
    struct netif *e = eth ? esp_netif_get_netif_impl(eth) : NULL;
    struct netif *s = sta ? esp_netif_get_netif_impl(sta) : NULL;
    /* WiFi STA：IDF 默认就用 eFuse 派生的 ESP_MAC_WIFI_STA（/status 的 mac 字段同源）。网口：DM9051 自身无 MAC，
       ethernet_init 派生后写进网卡，只能问 netif 要（webui/VERIFY.sh "以太网 MAC 从网卡实读"）—— 开机时网口已经启动；
       万一还是 0，等网口自己发第一个 DHCP 报文时由钩子补上 */
    uint8_t me[6] = {0}, ms[6] = {0};
    bool oke = eth && esp_netif_get_mac(eth, me) == ESP_OK && mac_nonzero(me);
    bool oks = esp_read_mac(ms, ESP_MAC_WIFI_STA) == ESP_OK && mac_nonzero(ms);
    taskENTER_CRITICAL(&s_cid_mux);
    s_eth_lw = e;
    s_sta_lw = s;
    if (oke) { memcpy(s_mac[SVC_NIC_ETH], me, 6);  s_mac_ok[SVC_NIC_ETH]  = true; }
    if (oks) { memcpy(s_mac[SVC_NIC_WIFI], ms, 6); s_mac_ok[SVC_NIC_WIFI] = true; }
    taskEXIT_CRITICAL(&s_cid_mux);
}

void svc_cid_set(svc_nic_t nic, svc_cid_t kind)
{
    if (nic != SVC_NIC_ETH && nic != SVC_NIC_WIFI) return;
    if (kind != SVC_CID_SELF && kind != SVC_CID_PEER && kind != SVC_CID_DUMMY) return;
    taskENTER_CRITICAL(&s_cid_mux);
    s_kind[nic] = kind;
    taskEXIT_CRITICAL(&s_cid_mux);
}

bool svc_cid_declined(svc_nic_t nic)
{
    return (nic == SVC_NIC_ETH || nic == SVC_NIC_WIFI) && s_declined[nic];
}

void svc_cid_clear_declined(svc_nic_t nic)
{
    if (nic == SVC_NIC_ETH || nic == SVC_NIC_WIFI) s_declined[nic] = false;
}

svc_cid_t svc_cid_sent(svc_nic_t nic)
{
    if (nic != SVC_NIC_ETH && nic != SVC_NIC_WIFI) return SVC_CID_SELF;
    return (svc_cid_t)s_sent[nic];                    /* 一个字节，读写天然原子 */
}

void svc_cid_counts(svc_nic_t nic, uint32_t *renew_reqs, uint32_t *rebind_reqs)
{
    bool ok = (nic == SVC_NIC_ETH || nic == SVC_NIC_WIFI);
    if (renew_reqs)  *renew_reqs  = ok ? s_renew_reqs[nic]  : 0;
    if (rebind_reqs) *rebind_reqs = ok ? s_rebind_reqs[nic] : 0;
}

svc_cid_t svc_cid_get(svc_nic_t nic)
{
    if (nic != SVC_NIC_ETH && nic != SVC_NIC_WIFI) return SVC_CID_SELF;
    taskENTER_CRITICAL(&s_cid_mux);
    svc_cid_t k = s_kind[nic];
    taskEXIT_CRITICAL(&s_cid_mux);
    return k;
}

void bitengine_dhcp_append_options(struct netif *netif, struct dhcp *dhcp, u8_t state,
                                   struct dhcp_msg *msg, u8_t msg_type, u16_t *len)
{
    (void)dhcp;
    if (!netif || !msg || !len) return;
    dhcp_append_extra_opts(netif, state, msg, len);   /* IDF 默认的其余选项；它那份 option 61 已由 Kconfig 关掉 */

    svc_cid_t kind = SVC_CID_SELF;
    svc_nic_t me = SVC_NIC_NONE;
    uint8_t peer_mac[6];
    bool peer_ok = false;
    taskENTER_CRITICAL(&s_cid_mux);
    if (netif == s_eth_lw && s_eth_lw)      me = SVC_NIC_ETH;
    else if (netif == s_sta_lw && s_sta_lw) me = SVC_NIC_WIFI;
    if (me != SVC_NIC_NONE) {
        svc_nic_t other = (me == SVC_NIC_ETH) ? SVC_NIC_WIFI : SVC_NIC_ETH;
        kind = s_kind[me];
        if (netif->hwaddr_len == 6) { memcpy(s_mac[me], netif->hwaddr, 6); s_mac_ok[me] = true; }   /* 顺手刷新自己那份 */
        peer_ok = s_mac_ok[other];
        if (peer_ok) memcpy(peer_mac, s_mac[other], 6);
    }
    taskEXIT_CRITICAL(&s_cid_mux);
    if (msg_type == DHCP_DECLINE && me != SVC_NIC_NONE) s_declined[me] = true;

    uint8_t cid[CID_LEN];
    svc_cid_t used = kind;                            /* 实际带出去的身份（对方 MAC 不知道时退成本卡） */
    if (kind == SVC_CID_DUMMY) {
        memcpy(cid, DUMMY_CID, sizeof(cid));
    } else {
        cid[0] = 0x01;                                /* 硬件类型 1 = 以太网 */
        if (kind == SVC_CID_PEER && peer_ok) {
            memcpy(&cid[1], peer_mac, 6);
        } else {
            /* 对方 MAC 还不知道（以太网初始化失败）：退回本卡 —— 记成 SELF，判定才不会把它当成交换着拿的 */
            if (netif->hwaddr_len != 6) return;       /* 不是以太网类的口：不附（IDF 默认在这里也只认 MAC） */
            memcpy(&cid[1], netif->hwaddr, 6);
            used = SVC_CID_SELF;
        }
    }
    if (msg_type == DHCP_REQUEST && me != SVC_NIC_NONE) {
        s_sent[me] = (uint8_t)used;
        if (state == DHCP_STATE_RENEWING)  s_renew_reqs[me]++;
        if (state == DHCP_STATE_REBINDING) s_rebind_reqs[me]++;
    }
    /* 放不下就不附（与 IDF 默认一样宁缺勿溢）。+1 给 dhcp_option_trailer 随后写的结束符（第一轮审计 C4） */
    if (*len + 2u + CID_LEN + 1u > DHCP_OPTIONS_LEN) return;
    uint8_t *o = msg->options + *len;
    *o++ = DHCP_OPTION_CLIENT_ID;
    *o++ = CID_LEN;
    memcpy(o, cid, CID_LEN);
    *len = (u16_t)(*len + 2u + CID_LEN);
}
#endif /* CONFIG_LWIP_DHCP_DISABLE_CLIENT_ID */
