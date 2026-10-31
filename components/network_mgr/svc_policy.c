/* components/network_mgr/svc_policy.c — T9.20 开机接管以太网服务 IP 的纯逻辑。
 * 说明见 svc_policy.h；主机侧测试 test_host/run.sh。不许引 ESP-IDF 头（VERIFY.sh 钉着）。 */
#include "svc_policy.h"

#include <string.h>

_Static_assert(sizeof(svc_rec_t) == 40, "svc_rec_t is persisted as an NVS blob: layout must not change without SVC_REC_VERSION++");

static bool mask_contiguous(uint32_t mask)
{
    /* 连续掩码取反后 +1 必是 2 的幂（含 0 → 全 1 的情形，下面另外排除 /0） */
    uint32_t inv = ~mask;
    return (inv & (inv + 1u)) == 0;
}

bool svc_ip_sane(uint32_t ip, uint32_t mask)
{
    if (ip == 0 || mask == 0) return false;
    if (!mask_contiguous(mask)) return false;
    if (mask == 0xFFFFFFFFu || mask == 0xFFFFFFFEu) return false;   /* /32 /31：没有"网段"可言 */
    uint8_t a = (uint8_t)(ip >> 24), b = (uint8_t)(ip >> 16);
    if (a == 0 || a == 127) return false;
    if (a >= 224) return false;                    /* 组播 224/4 与保留 240/4、广播 */
    if (a == 169 && b == 254) return false;        /* 链路本地 */
    if ((ip & ~mask) == 0) return false;           /* 网络号 */
    if ((ip & ~mask) == ~mask) return false;       /* 本网段广播 */
    return true;
}

bool svc_same_subnet(uint32_t svc_ip, uint32_t wifi_ip, uint32_t wifi_mask)
{
    if (wifi_mask == 0 || wifi_ip == 0) return false;
    return ((svc_ip ^ wifi_ip) & wifi_mask) == 0;
}

uint32_t svc_ssid_hash(const char *ssid)
{
    if (!ssid || !ssid[0]) return 0;
    uint32_t h = 2166136261u;                       /* FNV-1a 32 */
    for (const unsigned char *p = (const unsigned char *)ssid; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h ? h : 1u;                              /* 0 留给"没配 WiFi" */
}

bool svc_rec_valid(const svc_rec_t *r, size_t len)
{
    if (!r || len != sizeof(*r)) return false;
    if (r->ver != SVC_REC_VERSION) return false;
    if (r->flags & ~SVC_REC_F_INFINITE) return false;          /* 不认识的标志位 = 不是我们写的 */
    if (r->reserved != 0) return false;
    /* 写的时候没配 WiFi：svc_decide 对它恒判 NETWORK_CHANGED，存了只会在每次拔线开机时留一条
       "不接管 <旧地址>" —— 产线插网线测、不配 WiFi 的出货板就是这样（2026-10-06 审计 R2-18） */
    if (r->ssid_hash == 0) return false;
    if (!svc_ip_sane(r->ip, r->mask)) return false;
    if (r->gw != 0 && !svc_same_subnet(r->gw, r->ip, r->mask)) return false;
    if (r->saved_at < SVC_EPOCH_MIN) return false;              /* 只在墙钟可信时写，早于 2025 = 坏数据 */
    if (r->flags & SVC_REC_F_INFINITE) {
        if (r->expiry != 0) return false;
    } else {
        if (r->expiry <= r->saved_at) return false;             /* 写入时就已过期，不会这样写 */
        /* 写入时的剩余租期不会超过整条租约（留 60 s 给计时误差）；十年封顶挡住垃圾值
           （一年封顶会让少见的超长租约静默存不进去 —— 2026-10-06 审计 F15） */
        if (r->expiry - r->saved_at > (int64_t)r->lease_s + 60) return false;
        if (r->lease_s == 0 || r->lease_s > 10u * 366 * 24 * 3600) return false;
    }
    return true;
}

svc_verdict_t svc_decide(const svc_rec_t *rec, uint32_t wifi_ip, uint32_t wifi_mask,
                         uint32_t ssid_hash_now, bool clock_trusted, int64_t now, int64_t margin_s)
{
    if (!rec) return SVC_DENY_NO_RECORD;
    if (!svc_same_subnet(rec->ip, wifi_ip, wifi_mask)) return SVC_DENY_SUBNET;
    /* 按 WiFi 的掩码它若是网络号 / 广播号（有线、无线掩码不同时可能出现），etharp_query 会永远回
       ERR_ARG、探测永远出不了结论 —— 当成网段不符拒掉（2026-10-06 复核） */
    if (!svc_ip_sane(rec->ip, wifi_mask)) return SVC_DENY_SUBNET;
    if (rec->ip == wifi_ip) return SVC_DENY_SAME_AS_WIFI;
    /* 写记录时没配 WiFi（0）也算"变了"：那时的 WiFi 网络无从比对，宁可不接管 */
    if (rec->ssid_hash != ssid_hash_now || ssid_hash_now == 0) return SVC_DENY_NETWORK_CHANGED;
    if (rec->flags & SVC_REC_F_INFINITE) return SVC_OK;          /* 无限租期：不看时钟 */
    if (!clock_trusted) return SVC_WAIT_CLOCK;
    if (now < rec->saved_at) return SVC_DENY_CLOCK_BACKWARDS;
    if (margin_s < 0) margin_s = 0;
    if (now >= rec->expiry - margin_s) return SVC_DENY_EXPIRED;
    return SVC_OK;
}

const char *svc_verdict_name(svc_verdict_t v)
{
    switch (v) {
    case SVC_OK:                   return "ok";
    case SVC_WAIT_CLOCK:           return "wait_clock";
    case SVC_DENY_NO_RECORD:       return "no_record";
    case SVC_DENY_SUBNET:          return "subnet";
    case SVC_DENY_SAME_AS_WIFI:    return "same_as_wifi";
    case SVC_DENY_EXPIRED:         return "expired";
    case SVC_DENY_CLOCK_BACKWARDS: return "clock_backwards";
    case SVC_DENY_NETWORK_CHANGED: return "network_changed";
    case SVC_DENY_CONFLICT:        return "conflict";
    default:                       return "unknown";
    }
}

bool svc_persist_needed(const svc_rec_t *old, const svc_rec_t *neu,
                        int64_t now, int64_t min_interval_s)
{
    if (!neu) return false;
    if (!old) return true;
    if (old->ip != neu->ip || old->mask != neu->mask || old->gw != neu->gw ||
        old->flags != neu->flags || old->ssid_hash != neu->ssid_hash) {
        return true;
    }
    if (neu->flags & SVC_REC_F_INFINITE) return false;           /* 别的都没变，无可更新 */

    int64_t delta = neu->expiry - old->expiry;
    if (delta < -60) return true;                                /* 提前了：往安全方向改，不限流 */
    /* 续租把到期时刻推后约半个租期；平时的读数只有一两秒抖动。步长取租期的 1/4（至少 120 s），
       续租必过、抖动永不过 —— 短租约（几十分钟）也跟得上续租（2026-10-06 审计 F10） */
    int64_t step = (int64_t)neu->lease_s / 4;
    if (step < 120) step = 120;
    if (delta >= step && now - old->saved_at >= min_interval_s) return true;
    return false;
}

svc_gate_action_t svc_gate_step(const svc_gate_in_t *in)
{
    if (!in) return SVC_GATE_NOTHING;
    if (in->state == SVC_BT_NONE || in->state == SVC_BT_TAKEN || in->state == SVC_BT_DENIED) {
        return SVC_GATE_NOTHING;
    }
    if (in->eth_got_ip_this_boot) return SVC_GATE_RETIRE;
    if (in->eth_link_up || !in->wifi_has_own_ip) {
        return in->state == SVC_BT_PROBING ? SVC_GATE_ABORT : SVC_GATE_HOLD;
    }
    if (in->since_init_ms < in->grace_ms) {
        /* 探测只会在宽限期之后发起，PROBING 落到这里只可能是时钟回拨之类的异常：照样作废 */
        return in->state == SVC_BT_PROBING ? SVC_GATE_ABORT : SVC_GATE_ARM_GRACE;
    }
    if (in->state == SVC_BT_PROBING) return SVC_GATE_NOTHING;      /* 等探测结果 */
    return SVC_GATE_DECIDE;
}

svc_mdns_mode_t svc_mdns_decide(const svc_mdns_in_t *in)
{
    if (!in) return SVC_MDNS_DEFAULT;
    if (in->id_active) {
        if (in->id_known) return SVC_MDNS_FIXED;
        /* 首次安装还在定 S、或持有者还没拿到 A_S：先不给，理由同下面的开机等待 */
        return in->since_init_ms < in->hold_cap_ms ? SVC_MDNS_HOLD : SVC_MDNS_DEFAULT;
    }
    if (in->eth_has_ip || in->svc_on_wifi) return SVC_MDNS_FIXED;
    /* 插回网线、以太网还在等 DHCP：服务 IP 马上回到以太网上，别为这几秒换成 WiFi 地址再换回来 */
    if (in->svc_ip_valid && in->eth_dhcp_pending) return SVC_MDNS_FIXED;
    /* 开机：服务 IP 还没定下来（以太网链路在却还没地址 / 开机接管在判）就先不给 —— 别人第一次记下的地址就是以后
       连的地址。看链路而不是"DHCP 超时还没到"：插线开机实测 9.6 s 才拿到地址，离 10 s 的 DHCP 超时只差一点，
       慢一点的 DHCP（交换机 STP 收敛约 30 s）就会让 WiFi 地址漏出去（2026-10-07 审计）。
       有上限：插着没有 DHCP 的口、等对时（断电后又连不上 NTP）这类拖很久的，过了上限先给 WiFi 自己的地址，之后再换 */
    if (in->since_init_ms < in->hold_cap_ms &&
        ((in->eth_link_up && !in->eth_has_ip) || in->bt_pending)) return SVC_MDNS_HOLD;
    /* 只用 WiFi / 接管被拒 / 以太网 DHCP 超时 / 服务 IP 挂不过来（SSID 锁、网段变了）：宣告 WiFi 自己的地址 */
    return SVC_MDNS_DEFAULT;
}

/* ---- T9.37 服务身份（说明见 svc_policy.h；设计 docs/16）---- */
_Static_assert(sizeof(svc_id_rec_t) == 28, "svc_id_rec_t is persisted as an NVS blob: layout must not change without SVC_ID_REC_VERSION++");
_Static_assert(offsetof(svc_id_rec_t, mode_ts) == SVC_ID_REC_V2_LEN, "v3 must only append mode_ts after the v2 layout (svc_id_rec_load)");

bool svc_id_rec_valid(const svc_id_rec_t *r, size_t len)
{
    if (!r || len != sizeof(*r)) return false;
    if (r->ver != SVC_ID_REC_VERSION) return false;
    if (r->s_nic != SVC_NIC_ETH && r->s_nic != SVC_NIC_WIFI) return false;
    if (r->mode > SVC_IDMODE_MAC_ONLY) return false;
    if (r->flags & ~SVC_ID_F_AS_NATIVE) return false;            /* 不认识的标志位 = 不是我们写的 */
    if (r->net_ssid == 0) return false;                         /* 没配 WiFi 时定的 S 不存（产线） */
    if (r->mode != SVC_IDMODE_MAC_ONLY && r->mode_ts != 0) return false;   /* 计时只属于"只认 MAC" */
    if (r->ip == 0) return r->mask == 0 && r->gw == 0;          /* 还不知道 A_S：三个都得是 0 */
    if (!svc_ip_sane(r->ip, r->mask)) return false;
    if (r->gw != 0 && !svc_same_subnet(r->gw, r->ip, r->mask)) return false;
    return true;
}

bool svc_id_rec_load(const void *buf, size_t len, svc_id_rec_t *out)
{
    if (!buf || !out) return false;
    if (len == sizeof(*out)) {
        memcpy(out, buf, sizeof(*out));
    } else if (len == SVC_ID_REC_V2_LEN && ((const uint8_t *)buf)[0] == 2) {
        /* v2 与 v3 前 24 字节布局相同（v3 只在末尾加了 mode_ts） */
        memset(out, 0, sizeof(*out));
        memcpy(out, buf, SVC_ID_REC_V2_LEN);
        out->ver = SVC_ID_REC_VERSION;
    } else {
        return false;
    }
    return svc_id_rec_valid(out, sizeof(*out));
}

svc_maconly_t svc_id_mac_only_age(uint32_t mode_ts, bool clock_trusted, int64_t now_s, uint32_t period_s)
{
    if (!clock_trusted || now_s <= 0) return SVC_MACONLY_KEEP;
    if (mode_ts == 0 || now_s < (int64_t)mode_ts) return SVC_MACONLY_START;
    return (now_s - (int64_t)mode_ts >= (int64_t)period_s) ? SVC_MACONLY_RETRY : SVC_MACONLY_KEEP;
}

svc_degrade_t svc_id_degrade_mode(svc_nic_t s)
{
    return s == SVC_NIC_WIFI ? SVC_DEGRADE_PIN_WIFI : SVC_DEGRADE_LEGACY;
}

svc_cid_t svc_id_cid_for(svc_nic_t nic, svc_nic_t holder, svc_nic_t s, svc_id_mode_t mode)
{
    if (nic != SVC_NIC_ETH && nic != SVC_NIC_WIFI) return SVC_CID_SELF;
    if (s == SVC_NIC_NONE || holder == SVC_NIC_NONE || mode == SVC_IDMODE_MAC_ONLY) return SVC_CID_SELF;
    /* 持有者用 S、另一张卡用 other(S)：持有者就是 S 的本卡 → 都用自己的；否则两张卡都用对方的 */
    return holder == s ? SVC_CID_SELF : SVC_CID_PEER;
}

svc_nic_t svc_id_next_holder(const svc_id_holder_in_t *in)
{
    if (!in) return SVC_NIC_NONE;
    switch (in->cur) {
    case SVC_NIC_NONE:
        /* 开机待定：链路起来就推定网口（插线开机是主部署，别多做一轮交接）；宽限过了还没链路 → WiFi */
        if (in->eth_link_up) return SVC_NIC_ETH;
        if (in->since_init_ms >= in->grace_ms && in->wifi_usable) return SVC_NIC_WIFI;
        return SVC_NIC_NONE;
    case SVC_NIC_ETH:
        if (!in->wifi_usable) return SVC_NIC_ETH;                   /* 没有别人可让 */
        if (!in->eth_link_up) return SVC_NIC_WIFI;
        if (in->eth_dhcp_expired && !in->eth_has_ip) return SVC_NIC_WIFI;
        /* 开机推定的网口拿到的是别的网段（接在另一张网上）：服务地址还在 WiFi 那张网上，交回去 */
        if (in->eth_has_ip && !in->eth_ip_in_svc_net) return SVC_NIC_WIFI;
        return SVC_NIC_ETH;
    case SVC_NIC_WIFI:
        if (!in->eth_link_up) return SVC_NIC_WIFI;
        /* WiFi 当不了持有者（没凭据 / 静态）：网口一有链路就接，不用先证明（没有冲突的对象） */
        if (!in->wifi_usable) return SVC_NIC_ETH;
        /* 运行中插线不推定：网口先用 other(S) 拿到地址、且在 A_S 的网段里，证明有线这条路通、在同一张网上 */
        if (in->eth_has_ip && in->eth_ip_in_svc_net) return SVC_NIC_ETH;
        return SVC_NIC_WIFI;
    }
    return SVC_NIC_NONE;
}

svc_judge_t svc_id_judge(const svc_id_judge_in_t *in)
{
    if (!in || !in->is_holder || in->a == 0) return SVC_JUDGE_NONE;
    if (in->a_s == 0) return SVC_JUDGE_ADOPT;
    if (in->a == in->a_s) {
        /* 证实"认 client-id"要求 A_S 本来是 S 的本卡用自己身份拿到的：A_S 若是交换着拿到的，路由器只认 MAC 时
           这张卡再拿到它也只说明"它自己 MAC 的地址就是这个"（第二轮审计 #3） */
        return (in->swapped && in->mode == SVC_IDMODE_UNKNOWN && in->as_native) ? SVC_JUDGE_CID_CONFIRMED : SVC_JUDGE_OK;
    }
    if (!svc_same_subnet(in->a_s, in->a, in->mask)) {
        /* 另一张卡还在 A_S 的网段：不是整张网换了，是这张卡接在另一张网上 */
        return in->other_nic_in_svc_net ? SVC_JUDGE_OTHER_LAN : SVC_JUDGE_NET_CHANGED;
    }
    /* 同网段、换了地址 */
    if (!in->swapped) return SVC_JUDGE_ADOPT;                   /* 自己的身份：租约丢过、路由器重新分了址 */
    if (in->declined) return SVC_JUDGE_INCONCLUSIVE;            /* ARP 检查撞上了别人（冲突 / AP 代答）：不是身份的问题 */
    /* "只认 MAC"的特征是拿到的正是这张卡自己身份的地址（第一轮审计 D9：断电超过租期、A_S 被收回后交换着拿到一个
       新地址，以前也被判成只认 MAC，而且按 SSID 永久生效） */
    if (in->own_ip && in->a == in->own_ip) return SVC_JUDGE_MAC_ONLY;
    /* 交换着拿到一个既不是 A_S、也不是这张卡自己的地址：不采用（2026-10-07 写手册时改）。路由器上按 MAC 给 S 的本卡做了
       DHCP 保留时，A_S 被钉在那张卡上，另一张卡用 S 申请只会分到一个新地址 —— 以前这里判"认身份、A_S 换了"并写进 NVS，
       服务地址就悄悄变了，HA 里记的地址失效。A_S 真被收回（断电超过租期）的情形，S 的本卡下次用自己的身份拿到新地址时
       走上面"不交换"那条采用；在那之前本次开机退回旧行为 */
    return SVC_JUDGE_INCONCLUSIVE;
}

bool svc_id_wifi_can_hold(bool has_creds, uint32_t a_s, uint32_t wifi_ip, uint32_t wifi_mask,
                          uint32_t eth_ip, uint32_t eth_mask)
{
    if (!has_creds) return false;
    if (a_s == 0 || wifi_ip == 0) return true;
    if (svc_same_subnet(a_s, wifi_ip, wifi_mask)) return true;
    /* WiFi 不在 A_S 网段：网口还在 A_S 网段 → 两张网，WiFi 别接；网口也不在 → A_S 的网络已经不在了（换了网络） */
    return !(eth_ip && svc_same_subnet(a_s, eth_ip, eth_mask));
}

svc_nic_t svc_id_decide_s(svc_nic_t holder, svc_nic_t got)
{
    if (holder == SVC_NIC_NONE || got != holder) return SVC_NIC_NONE;
    return holder;
}

const char *svc_nic_name(svc_nic_t n)
{
    switch (n) {
    case SVC_NIC_ETH:  return "eth";
    case SVC_NIC_WIFI: return "wifi";
    default:           return "none";
    }
}

const char *svc_id_mode_name(svc_id_mode_t m)
{
    switch (m) {
    case SVC_IDMODE_CID:      return "client_id";
    case SVC_IDMODE_MAC_ONLY: return "mac_only";
    default:                  return "unknown";
    }
}
