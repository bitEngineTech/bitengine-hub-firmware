#pragma once
/* hub_ti_core —— Thread 网络层清单（thread_inventory）的纯逻辑（docs/15_muse_direct_design_v1.md 第七节）。纯 C，不依赖 ESP-IDF：
 * test_host/ 原样编它。ESP 那一层（thread_inventory.c）在 OT 锁里把 SRP 主机、子节点、路由器、EID 缓存原样拷进 ti_raw_t，
 * 放锁之后交给这里：关联（7.2）、Matter 识别（7.3）、设备 id、两次快照之间的增减（7.4）、列表项 JSON（附录 A）。
 *
 * 只说网络层的事（7.5）：在不在网、链路质量、挂在谁下面、属于哪些 fabric、是否等待添加。不说名字（`_matterc` 的 DN 例外）、
 * 类型、状态。来自设备的字符串（主机名、DN）输出前一律过 hub_sanitize_device_str（6.8）。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hub_json.h"

#define TI_MAX_HOSTS      252      /* = SRP 代播上限（7.5） */
#define TI_MAX_CHILDREN   64       /* = CONFIG_OPENTHREAD_MLE_MAX_CHILDREN */
#define TI_MAX_ROUTERS    63       /* router id 0–62 */
#define TI_MAX_CACHE      128      /* EID→RLOC 缓存拷多少条 */
#define TI_HOST_ADDRS     4
#define TI_CHILD_ADDRS    4
#define TI_FABRICS        6        /* Matter 要求设备至少支持 5 个 fabric（M5 判"是不是本网关的 fabric"别正好丢了自己那个） */
#define TI_HOSTNAME_MAX   64       /* SRP 主机名的第一段（DNS 标签 ≤ 63） */
#define TI_DN_MAX         33       /* _matterc TXT 的 DN（≤ 32 字节） */
#define TI_ID_MAX         24       /* "th-" + 16 hex，或 "th-h" + 15 hex */
#define TI_RLOC_NONE      0xFFFEu

typedef struct { uint8_t b[16]; } ti_ip6_t;

typedef struct {
    char     name[TI_HOSTNAME_MAX];           /* 主机名第一段（原样，未净化） */
    ti_ip6_t addrs[TI_HOST_ADDRS];
    uint8_t  n_addrs;
    uint32_t lease_remaining_s;
    struct { char cfid[17]; char node[17]; } fabrics[TI_FABRICS];   /* _matter._tcp 实例名拆出来的（已校验、统一大写） */
    uint8_t  n_fabrics;
    bool     commissionable;                  /* 有 _matterc._udp：等待被添加 */
    bool     has_vp;                          /* VID / PID：只解析不输出（说出了设备类型，超出 7.5）；留给 M5 matter_ctrl */
    uint16_t vid, pid;
    char     dn[TI_DN_MAX];                   /* _matterc 的 DN（原样，未净化） */
} ti_host_t;

typedef struct {
    uint8_t  ext[8];
    uint16_t rloc16;
    int8_t   rssi_avg, rssi_last;             /* 127 = 没有样本（OT_RADIO_RSSI_INVALID） */
    bool     rx_on_idle, ftd;
    uint32_t age_s, timeout_s, connected_s;
    ti_ip6_t addrs[TI_CHILD_ADDRS];
    uint8_t  n_addrs;
} ti_child_t;

typedef struct {
    uint8_t  ext[8];
    uint16_t rloc16;
    bool     is_neighbor;                     /* 和本 BR 直接相邻（邻居表里有它） */
    int8_t   rssi_avg;                        /* 只有邻居才有；127 = 没有 */
    uint8_t  age_s;
} ti_router_t;

typedef struct { ti_ip6_t addr; uint16_t rloc16; } ti_cache_t;

typedef struct {
    ti_host_t   hosts[TI_MAX_HOSTS];
    uint16_t    n_hosts;
    ti_child_t  children[TI_MAX_CHILDREN];
    uint16_t    n_children;
    ti_router_t routers[TI_MAX_ROUTERS];
    uint8_t     n_routers;
    ti_cache_t  cache[TI_MAX_CACHE];
    uint16_t    n_cache;
    uint16_t    my_rloc16;
    int8_t      noise_floor;                  /* link_margin 的参照（OT 的算法，正本在 hub_ti_core.c 的 link_margin） */
    bool        hosts_truncated;              /* SRP 主机超过 TI_MAX_HOSTS，后面的没拷 */
} ti_raw_t;

typedef enum {
    TI_LINK_CHILD = 0,       /* 本 BR 的子节点 */
    TI_LINK_VIA_ROUTER,      /* 挂在别的路由器下（EID 缓存查到 RLOC 子节点号 ≠ 0；或查到的是路由器 RLOC、但主机名表明它不是那台路由器 ——
                                OT 嗅探记的是上一跳，子节点的缓存会指向父路由器）：父路由器知道、链路质量不知道 */
    TI_LINK_ROUTER,          /* 它自己是路由器 */
    TI_LINK_UNKNOWN,         /* 关联不上（只知道它在 SRP 里注册着） */
} ti_link_t;

typedef struct {
    char      id[TI_ID_MAX];
    int16_t   host;          /* raw.hosts 的下标；-1 = 没有 SRP 注册 */
    int16_t   child;         /* raw.children 的下标；-1 */
    int16_t   router;        /* raw.routers 的下标；-1 */
    ti_link_t link;
    uint16_t  rloc16;        /* 知道就填，否则 TI_RLOC_NONE（缓存只给了父路由器的 RLOC 时也是 NONE） */
    uint16_t  parent;        /* 子节点：本 BR；挂在别的路由器下：缓存里那个 RLOC 的路由器部分（& 0xFC00）；否则 TI_RLOC_NONE */
} ti_entry_t;

typedef struct {
    ti_entry_t  e[TI_MAX_HOSTS + TI_MAX_CHILDREN + TI_MAX_ROUTERS];
    uint16_t    n;
    const char *sticky[TI_MAX_HOSTS];  /* ti_associate 的草稿：每个主机上一轮粘连的 id（指向 prev），只在关联时有效 */
} ti_list_t;

/* 上一次快照里的 id（增减判定、id 粘连用）：主机名 → th-<ext> 的对应记住，这一轮关联不上时沿用，别让 id 来回变 */
typedef struct {
    char     id[TI_ID_MAX];
    char     host[TI_HOSTNAME_MAX];  /* 空串 = 不是 SRP 主机 */
    uint8_t  miss;                   /* 连续几轮没看到 */
    uint64_t last_seen_ms;           /* 上一次看到它的那一轮 */
    bool     used;
} ti_prev_entry_t;

typedef struct {
    ti_prev_entry_t p[TI_MAX_HOSTS + TI_MAX_CHILDREN + TI_MAX_ROUTERS];
    bool            have_baseline;   /* 第一轮只建基线，不报增减 */
    uint64_t        baseline_ms;     /* 建基线的时刻：静默期 TI_QUIET_MS 里新出现的不报 */
} ti_prev_t;

#define TI_LEFT_AFTER   2            /* 连续这么多轮没看到、并且 */
#define TI_QUIET_MS     300000u      /* 建基线后 5 分钟里新出现的只并入基线、不报"新来"（SRP 主机陆续重新注册） */
#define TI_LEFT_MIN_MS  110000u      /* 距上次看到满这么久才算"离开"（60 s 一轮约 2 分钟；agent 调用时的现刷也算一轮，光数轮次会缩到 15 s） */

/* ---- 解析（7.3） ---- */
/* "_matter._tcp" 的实例名 "<16hex 压缩 fabric ID>-<16hex 节点号>"：合格返回 true，cfid / node 写成大写 16 hex */
bool ti_parse_matter_instance(const char *label, char cfid[17], char node[17]);
/* "_matterc._udp" 的 TXT（DNS-SD 线格式）：取 VP（"vid+pid" 十进制）、DN（≤ 32 字节，原样）。D / DT / CM 不用。 */
void ti_parse_matterc_txt(const uint8_t *txt, uint16_t len, ti_host_t *h);
/* 服务名（"_matter._tcp.default.service.arpa."）的前两段是否是 type / proto */
bool ti_service_is(const char *full_name, const char *type, const char *proto);
/* 主机全名 → 第一段（out ≥ TI_HOSTNAME_MAX）；不合格返回 false */
bool ti_host_label(const char *full_name, char *out, size_t cap);

/* ---- 关联（7.2） ---- */
/* raw → 清单。prev 可为 NULL（不沿用旧 id）。 */
void ti_associate(const ti_raw_t *raw, const ti_prev_t *prev, ti_list_t *out);
/* 设备 id：有扩展地址 "th-<16hex>"；只有主机名时 "th-h" + 15 hex（主机名前 15 个字符都是十六进制就取它们，小写；
   否则取主机名 FNV-1a 64 位哈希的 15 个 hex —— 稳定、不带设备给的原文） */
void ti_id_from_ext(const uint8_t ext[8], char out[TI_ID_MAX]);
void ti_id_from_host(const char *host, char out[TI_ID_MAX]);

/* ---- 增减（7.4） ---- */
/* 用这一轮的清单更新 prev，把新出现的 / 离开的 id 交给回调（第一轮只建基线）。离开 = 连续 TI_LEFT_AFTER 轮、且满 TI_LEFT_MIN_MS
   没看到。同一个 SRP 主机名这一轮换了 id（关联上 / 关联不上切换）：原地改名，不报离开也不报新来（第六步审计：否则推假离开）。 */
typedef void (*ti_change_fn)(void *ctx, const char *id, bool joined);
void ti_diff(ti_prev_t *prev, const ti_raw_t *raw, const ti_list_t *cur, uint64_t now_ms, ti_change_fn fn, void *ctx);

/* ---- 输出（附录 A 列表项） ---- */
const char *ti_role_str(const ti_raw_t *raw, const ti_entry_t *e);
/* 一项：full = hub.device.get 的详情（多 rloc16 / 最近 RSSI / 连接时长 / 超时 / 主机名 / 租约） */
void ti_write_entry(hj_t *w, const ti_raw_t *raw, const ti_entry_t *e, bool full);
/* "thread" 子对象（ti_write_entry 与 mt- 设备共用；M7 第四步） */
void ti_write_thread_obj(hj_t *w, const ti_raw_t *raw, const ti_entry_t *e, bool full);
/* 这一条的 SRP 主机有没有 cfid（16 位十六进制压缩 fabric ID）的 _matter._tcp 实例；有就把节点号（16 位十六进制）写进 node_out。
   本网关 fabric 上的设备按 mt- 呈现，th- 的列表里跳过它（7.3） */
bool ti_entry_on_fabric(const ti_raw_t *raw, const ti_entry_t *e, const char *cfid, char node_out[17]);
/* 找 cfid 上节点号是 node 的那一条（没有 NULL） */
const ti_entry_t *ti_find_fabric_node(const ti_raw_t *raw, const ti_list_t *l, const char *cfid, uint64_t node);
/* 按 id 找（不区分大小写：agent 可能把 hex 改成大写） */
const ti_entry_t *ti_find(const ti_list_t *l, const char *id);
