#pragma once

/*
 * Reads the MG24 NCP's firmware version over the EZSP UART.
 *
 * Why this exists: until now nothing on the C5 could say which NCP build a
 * shipped unit is running — `/api/v1/status` reported the C5 firmware only,
 * and the MG24 image was identifiable solely by the sha256 of the .gbl that
 * happened to be flashed. Field support had no way to answer "which Zigbee
 * firmware is on this box".
 *
 * What it is NOT: an EZSP stack. It speaks just enough ASH to complete three
 * transactions (version, getValue, getNetworkParameters) and then gets out of
 * the way. The bridge stays a pure byte-stream pipe — see docs/02, decision 1.
 *
 * ⚠️ Exclusive UART required. The query sends an ASH RST, which resets the
 * NCP and tears down whatever ASH session exists. Callers must guarantee no
 * TCP client is attached; the function double-checks and refuses otherwise.
 * Safe call sites are (a) ezsp_bridge_init() before the TCP server starts and
 * (b) inside an MG24 OTA session, where the bridge is already claimed.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* XNCP 0x7001/0x7000 的应答快照（docs/14 第 5.2 节）。只在独占 UART 时读：开机、MG24 升级后、
   用户"应用地区"时。之后 ZHA 再声明别的国家，C5 看不见（决策 1：有客户端时不碰 UART），
   所以带 read_uptime_s 让界面写明"截至第 N 秒"。 */
typedef struct {
    bool     xncp;          /* NCP 认得我们的私有命令；false = 旧固件或别家 XNCP */
    uint8_t  cls;           /* 生效等级：0 NONE 1 UNKNOWN 2 CE 3 FCC */
    uint8_t  host_cls;      /* ZHA 声明的等级 */
    uint8_t  gw_cls;        /* 网关声明的等级（应等于 C5 发下去的） */
    int8_t   cap_dbm;       /* 生效上限 */
    int8_t   requested_dbm; /* 协议栈存的请求值，不是射频实测 */
    char     host_cc[3];    /* 没声明则 "" */
    char     gw_cc[3];
    uint16_t clamped_count; /* 硬钳触发次数，自 NCP 上电 */
    uint32_t read_uptime_s; /* 读到它时的网关 uptime；0 = 从没读到过 */
} ncp_txp_state_t;

typedef struct {
    bool     valid;           /* the `version` exchange completed */
    uint8_t  ezsp_protocol;   /* EZSP protocol version the NCP speaks (0x12 = 18) */
    uint8_t  stack_type;      /* 0x02 = mesh */
    uint16_t stack_version;   /* major<<12 | minor<<8 | patch<<4 | special */

    /* From getValue(VERSION_INFO) — needs EZSP >= 8 (extended frame format).
       `build` is the only field that distinguishes two builds of the same
       EmberZNet release, so it is the useful one for support. */
    bool     detail_valid;
    uint16_t build;
    uint8_t  major, minor, patch, special, type;

    /* NCP 的表容量（getConfigurationValue，开机独占 UART 时读一次）。
       为什么要它：Zigbee 能接多少设备的约束**全在 MG24 侧** —— C5 是纯字节流
       透传，设备数对它是 0 字节开销也是 0 可见性。这些表有多大此前本仓库零记录。
       读到的是 NCP 上电值。10-06 实测上位机多数表只能调小，ZHA 握手后一项不变。例外：Z2M 每次握手
       都把直连子设备写回 stack_config.json 的值（默认 32），接 Z2M 时它不是运行值
       （mg24/README.md「关于设备数」）。 */
    struct {
        bool     valid;
        uint16_t packet_buffer_heap;
        uint16_t neighbor_table;
        uint16_t aps_unicast_msgs;
        uint16_t address_table;
        uint16_t route_table;
        uint16_t max_end_device_children;
        uint16_t source_route_table;
        uint16_t key_table;
    } cfg;
    ncp_txp_state_t txp;      /* 地区 / 功率上限快照（sync_region） */
    /* XNCP 0x0003 GET_BUILD_STRING："bitEngine-xncp/1.0 <git describe> <日期> nvm3c=<N>"（mg24/build_ncp.sh 写）。
       EmberZNet 版本串所有 build 都一样，这个才逐版不同 —— 没有指纹时云端拿它比（T9.19 ⑦）。
       只收可打印 ASCII；读不到 / 旧 NCP / 别家 XNCP 为 ""。 */
    char     build_string[96];
    /* true = 这次开机没有读 NCP，沿用的是上一次真读到的值（ncp_cache.c，T9.31 ②）：
       C5 单独重启时 MG24 多半还在带网，ASH RST 会把它的协议栈打掉。txp 里的 clamped_count、
       requested_dbm 是那时的快照，read_uptime_s 置 0。 */
    bool     from_cache;
} ncp_version_t;

/*
 * 为什么这里没有"读 Zigbee 实际信道"的接口 —— 别再尝试加回来。
 *
 * 读 getNetworkParameters 之前必须先 ASH RST 拿到会话，而复位会把 NCP 的
 * Zigbee 协议栈打回未初始化状态。要让它重新起来，主机必须先 write_config
 * （一整套 setConfigurationValue，给协议栈分配地址表/密钥表/缓冲区，且这张表
 * 随 EZSP 版本变化），然后才能 networkInit。少了这一步，networkInit 与
 * getNetworkParameters **恒回 NOT_JOINED，与 NVM3 里有没有网络无关**。
 * bellows 自己的注释说得很直白（bellows/zigbee/application.py:190）：
 *   "Writing config is required here because network info can't be loaded"
 *
 * 2026-08-12 上板实证：我们读到 NOT_JOINED 的同一时刻，ZHA 的设备正在 ch20
 * 上正常收发。据此对外宣称"NCP 没有网络"是错的，而且错得很有说服力。
 *
 * 把 write_config 实现出来等于在网关里再造一个 EZSP 主机 —— 正是 docs/02
 * 决策 1 明确不做的事（桥是纯字节流透传）。所以 Zigbee 信道改由用户声明，
 * 见 config_mgr.h 的 zigbee_config_t。
 */

/**
 * Run the query and cache the result. Caller must own the UART.
 *
 * ESP_ERR_INVALID_STATE — a TCP client is attached, so nothing was sent and
 *   the previous reading is kept: it still describes the running NCP.
 * ESP_ERR_TIMEOUT / other — the exchange was attempted and failed (NCP dead,
 *   wrong baud, flow control mismatch). The cache is invalidated: a version
 *   string that outlives the firmware it describes is worse than none.
 */
/* 一次独占会话：ASH RST → version → getValue → getConfigurationValue → **下发地区**（XNCP 0x7001）。
   gw_region：网关声明的国家码（config sys.radio_region），"" 或 NULL = 清除网关声明。
   每次都发（没设就发空），让 NCP 的网关声明永远镜像 C5 的配置（docs/14 第 7.5 节）。
   ⚠️ 只能在没有 TCP 客户端时调，会软复位 MG24。 */
esp_err_t ncp_version_query(const char *gw_region);

/** Last successful query, or a zeroed struct with .valid == false. */
const ncp_version_t *ncp_version_get(void);

/* 每次 ncp_version_query() 真跑过会话后调用一次（有客户端被拒的那种不算）：读到了 v->valid=true，
   读失败 v->valid=false（落盘那边据此作废旧缓存）。ncp_cache.c 的 ncp_cache_save 就是它。
   用回调而不是直接调 NVS：ncp_version.c 要在主机侧测试里编，不能依赖 nvs。 */
void ncp_version_set_persist_hook(void (*hook)(const ncp_version_t *v));

/* 沿用一份缓存的读数，不碰 UART（开机时 C5 单独重启、MG24 多半还在带网的情形）。
   结果 from_cache=true、txp.read_uptime_s=0。 */
void ncp_version_adopt_cached(const ncp_version_t *v);

/**
 * Human-readable version into @p buf, e.g. "8.2.2.0 build 532" or, when only
 * the `version` exchange succeeded, "8.2.2.0". Writes "unknown" when nothing
 * was ever read. Always NUL-terminates.
 */
void ncp_version_format(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
