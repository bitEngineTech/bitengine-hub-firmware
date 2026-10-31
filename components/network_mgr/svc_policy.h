/* components/network_mgr/svc_policy.h — T9.20 开机接管以太网服务 IP 的纯逻辑
 *
 * 不依赖 ESP-IDF（主机侧可编、可测：test_host/run.sh）。network_mgr.c 负责取数、
 * 存 NVS、发 ARP 探测；这里只做判断，所以判据的正本在这一个文件里。
 * 行为说明的正本是 main/Kconfig.projbuild 的 BITENGINE_WIFI_FLOAT_BOOT_TAKEOVER help。
 *
 * 地址一律**主机序** uint32（network_mgr.c 在边界上用 lwip_ntohl 换）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- NVS 记录（命名空间 "net_svc"，键 "rec"，blob）----
 * 只存 **DHCP** 拿到的以太网地址：静态以太网 IP 的正本是 config_mgr 的 eth 配置，
 * 而且没有租期可确认，开机不接管（见 Kconfig help）。
 * 版本号不认识 / 长度不对 / 字段不合理 → 整条当不存在（不接管），下一次以太网拿到
 * 租约时覆盖。 */
#define SVC_REC_VERSION     1
#define SVC_REC_F_INFINITE  0x01u   /* 无限租期（DHCP option 51 = 0xFFFFFFFF） */

typedef struct {
    uint8_t  ver;
    uint8_t  flags;
    uint16_t reserved;    /* 写 0 */
    uint32_t ssid_hash;   /* 写入时配置的 WiFi SSID（svc_ssid_hash）；0 = 当时没配 WiFi */
    uint32_t ip;
    uint32_t mask;
    uint32_t gw;
    uint32_t lease_s;     /* 这次租约的总长（秒），只用于写盘限流 */
    int64_t  expiry;      /* 租期到期的墙钟秒；INFINITE 时为 0 */
    int64_t  saved_at;    /* 写入时的墙钟秒 */
} svc_rec_t;

/* 2025-01-01T00:00:00Z。早于它的墙钟一律当"没对过时"（RTC 从 1970 起算）。 */
#define SVC_EPOCH_MIN       1735689600LL

/* 地址本身合理：非 0、非 127/8、非 169.254/16、非组播/保留、掩码连续且不是 /0 /31 /32、
   不是本网段的网络号或广播号。 */
bool svc_ip_sane(uint32_t ip, uint32_t mask);

/* 服务 IP 落在 WiFi 自己的网段里（按 WiFi 的掩码）。WiFi 掩码为 0 = 不知道 → false。 */
bool svc_same_subnet(uint32_t svc_ip, uint32_t wifi_ip, uint32_t wifi_mask);

/* WiFi SSID 的指纹（FNV-1a 32）。NULL / 空串 = 0；非空串保证非 0。
   用来判断"开机时连的 WiFi 还是写记录时那个网络"：搬到另一个同网段的网络、重新配了 WiFi，
   网段比对看不出来，SSID 变了看得出来。不用网关 MAC —— 经 MAC-NAT 中继器连 WiFi 时，
   两条腿看到的网关 MAC 可以不同，会把正常接管误拒。 */
uint32_t svc_ssid_hash(const char *ssid);

/* blob 是否可用（len = 实际读到的字节数）。读回时用，写之前也用：没配 WiFi（ssid_hash=0）、
   地址不像样、租约形态不认识的记录一律不存不读。 */
bool svc_rec_valid(const svc_rec_t *r, size_t len);

/* ---- 开机接管的判定（ARP 探测之前的全部条件）---- */
typedef enum {
    SVC_OK = 0,              /* 除 ARP 探测外都满足 —— 去探测 */
    SVC_WAIT_CLOCK,          /* 墙钟不可信（断电 / 异常复位后还没对上时）—— 等对时 */
    SVC_DENY_NO_RECORD,      /* 没有可用记录（从没用过以太网 DHCP、恢复过出厂、记录坏了） */
    SVC_DENY_SUBNET,         /* 不在 WiFi 当前所在的网段：换了网络，或两口不在同一 L2 */
    SVC_DENY_SAME_AS_WIFI,   /* WiFi 自己拿到的就是这个地址，无事可做 */
    SVC_DENY_EXPIRED,        /* 租期已过（或只剩余量以内）：路由器可能已把它分给别人 */
    SVC_DENY_CLOCK_BACKWARDS,/* 现在早于记录的写入时刻：两次的时钟有一次不对，宁可不接管 */
    SVC_DENY_NETWORK_CHANGED,/* WiFi SSID 和写记录时不一样（换了网络 / 重新配过 WiFi） */
    SVC_DENY_CONFLICT,       /* ARP 探测：有别的设备在用这个地址（由探测结果给出，不由 svc_decide） */
} svc_verdict_t;

/* rec 为 NULL 表示没有可用记录。ssid_hash_now = 当前配置的 WiFi SSID 的 svc_ssid_hash。
   now 只在 clock_trusted 时有意义。margin_s：离到期不足这么多秒也算过期（吸收计时误差，
   见 network_mgr.c 的 SVC_LEASE_MARGIN_S）。 */
svc_verdict_t svc_decide(const svc_rec_t *rec, uint32_t wifi_ip, uint32_t wifi_mask,
                         uint32_t ssid_hash_now, bool clock_trusted, int64_t now, int64_t margin_s);

/* 稳定的短名（日志、/api/v1/status 的 svc_takeover 字段都用它）。 */
const char *svc_verdict_name(svc_verdict_t v);

/* ---- 写盘限流（设计决策 7：别磨 NVS）----
 * old 为 NULL = 还没写过。地址/掩码/网关/SSID 指纹/无限标志变了 → 立刻写；
 * 到期时刻**提前**超过 60 s（租期被缩短或时钟被往回校）→ 立刻写（安全方向，不限流）；
 * 到期时刻**推后**（续租）→ 推后至少 max(120, lease_s/4) 秒、且距上次写入满 min_interval_s 才写
 * （network_mgr.c 传 lease_s/4，夹在 300–1800 s）。
 * 所以 NVS 里的到期时刻只会比真实的早（保守），每次续租最多写一次。 */
bool svc_persist_needed(const svc_rec_t *old, const svc_rec_t *neu,
                        int64_t now, int64_t min_interval_s);

/* ---- 开机闸门的推进 ----
 * 纯函数，只看当前的事实，告诉调用方下一步做什么；状态由 network_mgr.c 持有。 */
typedef enum {
    SVC_BT_NONE = 0,   /* 没有候选，或已退场（以太网本次开机拿到过地址） */
    SVC_BT_WAIT,       /* 有候选，等条件 */
    SVC_BT_PROBING,    /* ARP 探测在途 */
    SVC_BT_TAKEN,      /* 已接管（之后归运行中浮动的逻辑管） */
    SVC_BT_DENIED,     /* 已拒绝（本次开机不再尝试） */
} svc_bt_state_t;

typedef struct {
    svc_bt_state_t state;
    bool    eth_link_up;          /* 以太网链路此刻在 */
    bool    eth_got_ip_this_boot; /* 以太网本次开机拿到过地址 → 实时快照接管，开机接管退场 */
    bool    wifi_has_own_ip;      /* WiFi 有自己的地址（有 IP 且不是浮过来的服务 IP） */
    int64_t since_init_ms;        /* network_mgr_init 以来的毫秒数 */
    int64_t grace_ms;             /* 开机宽限：给慢的以太网协商留时间 */
} svc_gate_in_t;

typedef enum {
    SVC_GATE_NOTHING = 0,  /* 终态 / 探测在途：什么都不做 */
    SVC_GATE_RETIRE,       /* 以太网本次拿到过地址：进 NONE（若在探测，探测作废） */
    SVC_GATE_ABORT,        /* 在探测但前提没了：探测作废，回 WAIT */
    SVC_GATE_HOLD,         /* 前提不满足：继续等 */
    SVC_GATE_ARM_GRACE,    /* 宽限期没过：grace_ms - since_init_ms 之后再看 */
    SVC_GATE_DECIDE,       /* 前提都满足：跑 svc_decide */
} svc_gate_action_t;

svc_gate_action_t svc_gate_step(const svc_gate_in_t *in);

/* ---- 本机 mDNS 宣告哪个地址（T9.34 路线 2；mdns 侧见 tools/patch_mdns.py SVCIP）----
 * 两个口都在线时，上游 mdns 由地址大的口应答、A 记录两个地址都带（实抓 [.36, .34]）—— HA / ZHA / Z2M
 * 添加网关时记下的是 WiFi 地址，一拔网线（服务 IP 浮到 WiFi、顶掉 WiFi 自己的地址）就连不上。
 * 这里决定本机 A 记录给什么；行为说明的正本是 Kconfig BITENGINE_MDNS_SVC_IP_ONLY 的 help。
 * 纯函数，只看当前事实；状态由 network_mgr.c 持有。数值与补丁里的 BITENGINE_SELF_* 一一对应（network_mgr.c 有编译期断言）。 */
typedef enum {
    SVC_MDNS_DEFAULT = 0,  /* 上游行为：本口自己的地址 —— 这次开机没有以太网服务 IP 可宣告 */
    SVC_MDNS_FIXED   = 1,  /* 只宣告服务 IP（以太网的地址；浮动时它在 WiFi 口上） */
    SVC_MDNS_HOLD    = 2,  /* 本机 A / AAAA 暂不给：开机还没定下服务 IP，先给 WiFi 地址会被 HA 记住 */
} svc_mdns_mode_t;

typedef struct {
    bool    eth_has_ip;        /* 以太网此刻有地址（就是服务 IP） */
    bool    svc_on_wifi;       /* 服务 IP 正浮在 WiFi 上 */
    bool    svc_ip_valid;      /* 有服务 IP 快照（本次开机以太网拿到过地址，或开机接管成立） */
    bool    eth_link_up;       /* 以太网链路此刻在 */
    bool    eth_dhcp_pending;  /* 以太网链路在、还没拿到地址、DHCP 超时还没到 */
    bool    bt_pending;        /* 开机接管还没出结论（等待 / 探测中） */
    int64_t since_init_ms;     /* network_mgr_init 以来的毫秒数 */
    int64_t hold_cap_ms;       /* 开机最多憋这么久，过了先给 WiFi 自己的地址 */
    /* T9.37 服务身份生效时（路由器不是"只认 MAC"）上面那些以太网 / 浮动输入不看，只看这两个：
       服务地址就是 A_S，不管此刻在哪张卡上（交接空档里也照样宣告它，别让 WiFi 自己的地址漏出去） */
    bool    id_active;         /* 服务身份机制在管服务地址 */
    bool    id_known;          /* S 已定且知道 A_S */
} svc_mdns_in_t;

svc_mdns_mode_t svc_mdns_decide(const svc_mdns_in_t *in);

/* ---- T9.37 服务身份：服务地址双向浮动（2026-10-07）----
 * 设计 docs/16_service_identity_dual_float_design_v1.md；行为说明的正本是 Kconfig BITENGINE_SVC_IDENTITY 的 help。
 * 服务地址不再是"以太网的地址"，而是某个 DHCP 身份（client-id）S 的地址 A_S：持有者 H 用 S 做 DHCP，另一张卡用
 * other(S)。路由器按 client-id 认租约时（10-07 实测本机路由器如此），A_S 能由任一张卡长期持有并正常续租。
 * 这里只做判断；交接（先哑身份停、再设表起）在 network_mgr.c，client-id 钩子在 svc_cid.c。 */
typedef enum {
    SVC_NIC_NONE = 0,      /* 持有者：开机待定 / 没有；S：还没定 */
    SVC_NIC_ETH  = 1,
    SVC_NIC_WIFI = 2,
} svc_nic_t;

typedef enum {
    SVC_CID_SELF  = 0,     /* 本卡的自然身份 01:<本卡 MAC>（IDF 默认就是这个，现有租约都按它记） */
    SVC_CID_PEER  = 1,     /* 另一张卡的自然身份（交换） */
    SVC_CID_DUMMY = 2,     /* 交接时停 DHCP 用：RELEASE 带它，对不上任何租约，路由器忽略 */
} svc_cid_t;

typedef enum {
    SVC_IDMODE_UNKNOWN  = 0,   /* 还没在这个网络上交换着拿过地址 */
    SVC_IDMODE_CID      = 1,   /* 交换着拿到过 A_S：路由器按 client-id 认 */
    SVC_IDMODE_MAC_ONLY = 2,   /* 交换着拿到的正是这张卡自己的地址：路由器只认 MAC → 退化（svc_id_degrade_mode） */
} svc_id_mode_t;

/* NVS 记录（命名空间 "net_id"，键 "id"，blob）。版本 / 长度 / 字段不认识 → 当不存在。
 * **恢复出厂不擦**（10-07 用户拍板）：设备地址不是用户设置，擦了的话先用 WiFi 装、后插网线的用户恢复出厂后会被定成
 * 网口的地址，HA 里记的地址失效。记录按 WiFi 网络（net_ssid）认，换了网络照样按首次安装重定。
 * 只在配过 WiFi 时写（net_ssid 非 0）：产线插网线测、不配 WiFi，那时定下的 S 不算数（2026-10-07 两轮审计 ❌1：
 * 否则每台出货设备都带着"S = 网口"，用户先用 WiFi 装时整个机制被静默关掉或定错）。
 * v2（10-07）：加 net_ssid、flags，存在 "net_svc"（随恢复出厂擦）；v1 当不存在。
 * v3（10-07 晚）：加 mode_ts，挪到 "net_id"；v2 读得进来（svc_id_rec_load 升级，mode_ts = 0）。 */
#define SVC_ID_REC_VERSION  3
#define SVC_ID_REC_V2_LEN   24u
#define SVC_ID_F_AS_NATIVE  0x01u   /* A_S 是 S 的本卡用自己的身份拿到的（"认 client-id"只在这时证实得了） */
typedef struct {
    uint8_t  ver;
    uint8_t  s_nic;        /* SVC_NIC_ETH / SVC_NIC_WIFI */
    uint8_t  mode;         /* svc_id_mode_t */
    uint8_t  flags;        /* SVC_ID_F_* */
    uint32_t ip;           /* A_S（主机序）；0 = 还不知道（那时 mask / gw 也是 0） */
    uint32_t mask;
    uint32_t gw;
    uint32_t mode_ssid;    /* 学到 mode 时的 WiFi SSID 指纹（svc_ssid_hash）；换了 WiFi 网络，mode 作废重试 */
    uint32_t net_ssid;     /* 定 S 时配置的 WiFi SSID 指纹：S 属于这个网络；换了网络当首次安装重定 */
    uint32_t mode_ts;      /* mode = MAC_ONLY 时：开始计时的墙钟（秒，可信墙钟才记）；0 = 还没开始计。其余 mode 恒 0 */
} svc_id_rec_t;

bool svc_id_rec_valid(const svc_id_rec_t *r, size_t len);

/* 从 NVS 读出的字节解析记录：v3（sizeof）原样、v2（SVC_ID_REC_V2_LEN 字节、ver 2）升成 v3（mode_ts = 0）。
   解析完再按 svc_id_rec_valid 校验；不认识 / 不合理 → false（out 内容未定义）。 */
bool svc_id_rec_load(const void *buf, size_t len, svc_id_rec_t *out);

/* "路由器只认 MAC"记下后多久重试一次（10-07 用户拍板 7 天）。只在开机时重试：运行中从旧规则切回身份机制
   要拆掉可能正挂着的静态浮动，不值当；到期后的下一次开机就会再试。 */
#define SVC_ID_MAC_ONLY_RETRY_S  (7u * 24u * 3600u)
typedef enum {
    SVC_MACONLY_KEEP  = 0,     /* 不动：墙钟不可信，或还没到期 */
    SVC_MACONLY_START = 1,     /* 记下开始计时的时刻（mode_ts = now）：还没记过，或墙钟比记的还早（回拨过） */
    SVC_MACONLY_RETRY = 2,     /* 到期：mode 作废、重新试 */
} svc_maconly_t;
svc_maconly_t svc_id_mac_only_age(uint32_t mode_ts, bool clock_trusted, int64_t now_s, uint32_t period_s);

/* 退化（路由器只认 MAC 记在案 / 本次开机判不了）时服务地址怎么办（10-07 用户拍板）：
   S = WiFi → 钉在 WiFi 上（两张卡都用自己的身份，WiFi 一直持有 A_S —— 插着线流量也走 WiFi，但 HA 记的地址不断）；
   其余 → 旧规则（服务地址 = 以太网地址，T2.23 静态浮动 + T9.20 开机接管 —— S 是网口时这正是对的）。 */
typedef enum {
    SVC_DEGRADE_LEGACY   = 0,
    SVC_DEGRADE_PIN_WIFI = 1,
} svc_degrade_t;
svc_degrade_t svc_id_degrade_mode(svc_nic_t s);

/* 每张卡该报哪个身份。s = 服务身份所属的卡（NONE = 还没定）；holder = 当前持有者（NONE = 开机待定）。
   S 没定、持有者待定、或路由器只认 MAC 时两张卡都用自然身份；持有者不是 S 的本卡时两张卡互换。 */
svc_cid_t svc_id_cid_for(svc_nic_t nic, svc_nic_t holder, svc_nic_t s, svc_id_mode_t mode);

/* 持有者怎么变（设计 §5 的表）。纯函数：只看当前事实，告诉调用方新的持有者；交接由调用方做。 */
typedef struct {
    svc_nic_t cur;               /* 当前持有者；SVC_NIC_NONE = 开机待定 */
    bool    eth_link_up;
    bool    eth_has_ip;          /* 网口此刻有地址（用什么身份拿的都算） */
    bool    eth_ip_in_svc_net;   /* 网口的地址在 A_S 的网段里（A_S 还不知道时为 true） */
    bool    eth_dhcp_expired;    /* 网口链路在、从链路起来 / 交接起已超过 DHCP 超时还没地址 */
    bool    wifi_usable;         /* WiFi 有凭据、不是静态地址（能当持有者） */
    int64_t since_init_ms;
    int64_t grace_ms;            /* 开机时网口链路不在：等它这么久再定 WiFi */
} svc_id_holder_in_t;

svc_nic_t svc_id_next_holder(const svc_id_holder_in_t *in);

/* 持有者拿到地址 a 之后怎么判（设计 §7 的表）。非持有者拿到地址不归这里判（返回 NONE）。 */
typedef enum {
    SVC_JUDGE_NONE = 0,        /* 没事 */
    SVC_JUDGE_OK,              /* a == A_S */
    SVC_JUDGE_CID_CONFIRMED,   /* a == A_S，且是交换着拿到的、mode 还是 UNKNOWN：记"认 client-id" */
    SVC_JUDGE_ADOPT,           /* A_S = a：第一次 / 租约丢过、路由器给 S 的本卡（用自己的身份）重新分了址。交换着拿到的从不采用 */
    SVC_JUDGE_MAC_ONLY,        /* 交换着拿到的正是这张卡自己身份的地址：路由器只认 MAC → 记下、退回旧行为 */
    SVC_JUDGE_OTHER_LAN,       /* 网口拿到别的网段、而 WiFi 在 A_S 的网段：网口接在另一张网上，别当持有者 */
    SVC_JUDGE_NET_CHANGED,     /* 拿到别的网段、另一张卡也不在 A_S 网段：换了网络 → S 重定为这张卡、回本卡身份 */
    SVC_JUDGE_INCONCLUSIVE,    /* 交换着拿到别的地址，又不是这张卡自己的（或这一轮发过 DECLINE）：不改服务地址，
                                  本次开机退回旧行为，不记 —— 可能是按 MAC 做的 DHCP 保留，也可能 A_S 被收回了 */
} svc_judge_t;

typedef struct {
    bool          is_holder;
    bool          swapped;              /* 这张卡正用另一张卡的身份 */
    svc_id_mode_t mode;
    uint32_t      a, mask;              /* 拿到的地址与掩码（主机序） */
    uint32_t      a_s;                  /* A_S；0 = 还不知道 */
    bool          other_nic_in_svc_net; /* 另一张卡此刻有 A_S 网段里的地址 */
    bool          as_native;            /* A_S 是 S 的本卡用自己的身份拿到的（SVC_ID_F_AS_NATIVE） */
    uint32_t      own_ip;               /* 这张卡本次开机用自己的身份拿到过的地址；0 = 没拿到过 */
    bool          declined;             /* 这一轮申请里发过 DHCPDECLINE（ARP 检查撞上了别人） */
} svc_id_judge_in_t;

svc_judge_t svc_id_judge(const svc_id_judge_in_t *in);

/* WiFi 能不能当持有者（第二轮审计 #1：网口接 IoT VLAN、WiFi 在主网这类"两张网"拓扑）：
   有凭据，且 A_S 还不知道 / WiFi 还没地址 / WiFi 的地址在 A_S 网段 / A_S 的网段已经不在任何一张卡上（换了网络）。
   网口在 A_S 网段而 WiFi 在别的网段时不行 —— 交换过去只会在主网上拿一个陌生地址，HA 记的 WiFi 地址反而丢了。
   地址、掩码都是主机序，0 = 没有。 */
bool svc_id_wifi_can_hold(bool has_creds, uint32_t a_s, uint32_t wifi_ip, uint32_t wifi_mask,
                          uint32_t eth_ip, uint32_t eth_mask);

/* S 还没定时（首次安装）：第一个拿到地址的持有者定 S。got = 拿到地址的卡。返回 NONE = 还不定。 */
svc_nic_t svc_id_decide_s(svc_nic_t holder, svc_nic_t got);

/* 稳定短名（日志、/api/v1/status 用） */
const char *svc_nic_name(svc_nic_t n);
const char *svc_id_mode_name(svc_id_mode_t m);
