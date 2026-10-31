#pragma once
/* zh_ezsp_core —— EZSP v18（协议号 0x12）帧的编码与解析，纯逻辑（docs/15_muse_direct_design_v1.md 8.4、附录 D）。不依赖 ESP-IDF：test_host/ 原样编它。
 *
 * 语义正本是本机 SDK（Simplicity SDK 2025.6.3）的主机实现：protocol/zigbee/app/util/ezsp/ezsp.c（头部与回包校验）、command-functions.h（每条命令的
 * append / fetch 顺序、回调 callbackDispatch）、ezsp-frame-utilities.c/.h（结构体序列化）、ezsp-protocol.h（帧控制位）、ezsp-enum.h（帧 ID）。
 * 主机测试把那几份 SDK 源码编起来当预言机：同样的参数编出的字节逐字节相同，同样的回包 / 回调字节解析出的字段逐个相同（test_host/sdk_interop/ezsp_oracle.c）。
 *
 * H1（写代码前答清楚；出处都是上面几份）：
 * 1. 字段含义：
 *    - 扩展头 5 B：[seq][fc 低字节][fc 高字节][帧 ID 低][帧 ID 高]，参数从第 5 字节起（ezsp-protocol.h:39-54）。
 *      fc 低字节：0x80 方向（1 = NCP → 主机）、0x60 网络号、0x10 异步回调、0x08 同步回调、0x04 还有回调待取、0x02 截断、0x01 溢出；
 *      主机发命令时只放网络号与睡眠模式（低 2 位，我们恒 0 = idle）（ezsp-protocol.h:75-131、ezsp.c:621-626）。
 *      fc 高字节：低 2 位是帧格式版本（必须 0x01）、0x3C 保留（置了任何一位都拒）、0x40 填充、0x80 加密（ezsp-protocol.h:138-154、ezsp.c:552-561）。
 *    - 旧式头 3 B：[seq][fc][帧 ID]，只给**第一条** version 用（老 NCP 只认这个；ezsp.c:525-528、628-633）；NCP 的回包看 fc 高字节的格式位判断是哪种头。
 *    - 状态码：v18 的命令回包把状态扩成 4 B 的 sl_status_t（command-functions.h 里一律 fetchInt32u）；回调里 trustCenterPostJoin 的 status 仍是 1 B。
 *    - 多字节整数一律小端（ezsp-frame-utilities.c:56-79）；有符号的 int8 按原字节传（appendInt8 = appendInt8u）。
 * 2. 必须实现的回调 / 释放：本模块不分配内存；解析结果里的指针都指向调用方给的帧（帧活多久指针就有效多久）。
 * 3. 配对关系：同时只有一条命令在途，按 seq 配回包（8.4）；回调帧（fc 0x18 任一位）可能夹在中间，按帧 ID 派发。
 * 4. 字节格式：每条命令 / 回包 / 回调的字段顺序见各函数旁的注释（command-functions.h 的行号）。
 *
 * ⚠️ 下面三件是 ASH 审计复现过的、本层的硬要求（设计 8.4）：
 *  - 方向位：真 NCP（em260）重传时会把**主机自己的命令**当应答发回来（同 seq、同帧 ID）。zh_ezsp_parse() 对方向不对的帧回 ZH_EZSP_E_DIRECTION，上层丢掉、等超时重发；
 *  - 长度：CRC-CCITT 不做终异或，丢了一个 0x00 尾字节的帧 CRC 仍可能对。**每个解码器都要求参数恰好读完**，多一个少一个都回 ZH_EZSP_E_LENGTH；
 *  - 截断（fc 0x02）的帧拒收（ZH_EZSP_E_TRUNCATED）；溢出（0x01）只是 NCP 缓冲紧张的提示，照常处理（ezsp.c:606-613）。
 *
 * 有意比 SDK 严的地方（都是 SDK 把坏帧当好帧收下，或者被它带偏）：
 *  - ezsp.c:569-585 是"后写的覆盖先写的"：溢出位会把前面判出的截断 / 方向 / invalidCommand 一起盖成"成功"——我们先判先拒，溢出不洗白任何错误；
 *  - invalidCommand 的原因码恰好是 0（SUCCESS）或 0x34（= OVERFLOW 的状态值）时 SDK 当成功；恰好是 0x27（= NO_RX_DATA）时 SDK 当"还没收到"、
 *    把下一帧当成应答（sendCommand 的等待循环）——我们一律拒；
 *  - 短帧：第 2 字节说是扩展头却不到 5 B、invalidCommand 没带原因码，SDK 照读，读到的是共用缓冲里**刚发出去的命令**的字节
 *    （串口层原地收发、不清缓冲），于是像一条帧 ID 正确匹配的应答——我们回 ZH_EZSP_E_SHORT / 原因码记 0 并拒。
 * 预言机测试把前两类当"已知差异"逐格断言，其余分类与 SDK 逐格一致；短帧那类要靠缓冲残留，预言机的串口替身不留残留，只在真值表里测。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZH_EZSP_PROTOCOL_VERSION   0x12   /* ezsp-protocol.h:33 EZSP_PROTOCOL_VERSION */
#define ZH_EZSP_MAX                220    /* ezsp-protocol.h:36 EZSP_MAX_FRAME_LENGTH（= ASH 数据域上限） */
#define ZH_EZSP_EXT_HDR            5      /* ezsp-protocol.h:54 EZSP_EXTENDED_PARAMETERS_INDEX */
#define ZH_EZSP_LEGACY_HDR         3      /* ezsp-protocol.h:45 EZSP_PARAMETERS_INDEX */

/* 帧控制（ezsp-protocol.h:75-154） */
#define ZH_EZSP_FC_RESPONSE        0x80
#define ZH_EZSP_FC_NETIDX_MASK     0x60
#define ZH_EZSP_FC_NETIDX_SHIFT    5
#define ZH_EZSP_FC_ASYNC_CB        0x10
#define ZH_EZSP_FC_SYNC_CB         0x08
#define ZH_EZSP_FC_PENDING_CB      0x04
#define ZH_EZSP_FC_TRUNCATED       0x02
#define ZH_EZSP_FC_OVERFLOW        0x01
#define ZH_EZSP_FCH_FORMAT_MASK    0x03
#define ZH_EZSP_FCH_FORMAT_V1      0x01
#define ZH_EZSP_FCH_RESERVED_MASK  0x3C

/* 帧 ID（附录 D；行号是 ezsp-enum.h 的） */
#define ZH_EZSP_ID_VERSION                      0x0000  /* ezsp-enum.h:757 */
#define ZH_EZSP_ID_GET_CONFIGURATION_VALUE      0x0052  /* ezsp-enum.h:758 */
#define ZH_EZSP_ID_SET_CONFIGURATION_VALUE      0x0053  /* ezsp-enum.h:759 */
#define ZH_EZSP_ID_ADD_ENDPOINT                 0x0002  /* ezsp-enum.h:762 */
#define ZH_EZSP_ID_SET_POLICY                   0x0055  /* ezsp-enum.h:763 */
#define ZH_EZSP_ID_GET_VALUE                    0x00AA  /* ezsp-enum.h:766 */
#define ZH_EZSP_ID_SET_VALUE                    0x00AB  /* ezsp-enum.h:768 */
#define ZH_EZSP_ID_NOP                          0x0005  /* ezsp-enum.h:777 */
#define ZH_EZSP_ID_CALLBACK                     0x0006  /* ezsp-enum.h:780 */
#define ZH_EZSP_ID_NO_CALLBACKS                 0x0007  /* ezsp-enum.h:781 */
#define ZH_EZSP_ID_INVALID_COMMAND              0x0058  /* ezsp-enum.h:779 */
#define ZH_EZSP_ID_CUSTOM_FRAME                 0x0047  /* ezsp-enum.h:799 */
#define ZH_EZSP_ID_CUSTOM_FRAME_HANDLER         0x0054  /* ezsp-enum.h:800 */
#define ZH_EZSP_ID_GET_EUI64                    0x0026  /* ezsp-enum.h:801 */
#define ZH_EZSP_ID_GET_NODE_ID                  0x0027  /* ezsp-enum.h:802 */
#define ZH_EZSP_ID_NETWORK_INIT                 0x0017  /* ezsp-enum.h:817 */
#define ZH_EZSP_ID_NETWORK_STATE                0x0018  /* ezsp-enum.h:818 */
#define ZH_EZSP_ID_STACK_STATUS_HANDLER         0x0019  /* ezsp-enum.h:819 */
#define ZH_EZSP_ID_START_SCAN                   0x001A  /* ezsp-enum.h:820 */
#define ZH_EZSP_ID_ENERGY_SCAN_RESULT_HANDLER   0x0048  /* ezsp-enum.h:821 */
#define ZH_EZSP_ID_SCAN_COMPLETE_HANDLER        0x001C  /* ezsp-enum.h:823 */
#define ZH_EZSP_ID_FORM_NETWORK                 0x001E  /* ezsp-enum.h:827 */
#define ZH_EZSP_ID_LEAVE_NETWORK                0x0020  /* ezsp-enum.h:830 */
#define ZH_EZSP_ID_PERMIT_JOINING               0x0022  /* ezsp-enum.h:832 */
#define ZH_EZSP_ID_CHILD_JOIN_HANDLER           0x0023  /* ezsp-enum.h:833 */
#define ZH_EZSP_ID_GET_NETWORK_PARAMETERS       0x0028  /* ezsp-enum.h:835 */
#define ZH_EZSP_ID_SET_RADIO_POWER              0x0099  /* ezsp-enum.h:866 */
#define ZH_EZSP_ID_SET_CONCENTRATOR             0x0010  /* ezsp-enum.h:870 */
#define ZH_EZSP_ID_SEND_UNICAST                 0x0034  /* ezsp-enum.h:913 */
#define ZH_EZSP_ID_SEND_BROADCAST               0x0036  /* ezsp-enum.h:914 */
#define ZH_EZSP_ID_MESSAGE_SENT_HANDLER         0x003F  /* ezsp-enum.h:918 */
#define ZH_EZSP_ID_INCOMING_MESSAGE_HANDLER     0x0045  /* ezsp-enum.h:928 */
#define ZH_EZSP_ID_SET_SOURCE_ROUTE_DISCOVERY_MODE 0x005A /* ezsp-enum.h:929 */
#define ZH_EZSP_ID_INCOMING_ROUTE_RECORD_HANDLER 0x0059 /* ezsp-enum.h:933 */
#define ZH_EZSP_ID_LOOKUP_NODE_ID_BY_EUI64      0x0060  /* ezsp-enum.h:942 */
#define ZH_EZSP_ID_LOOKUP_EUI64_BY_NODE_ID      0x0061  /* ezsp-enum.h:943 */
#define ZH_EZSP_ID_ID_CONFLICT_HANDLER          0x007C  /* ezsp-enum.h:946 */
#define ZH_EZSP_ID_SET_INITIAL_SECURITY_STATE   0x0068  /* ezsp-enum.h:961 */
#define ZH_EZSP_ID_GET_CURRENT_SECURITY_STATE   0x0069  /* ezsp-enum.h:962 */
#define ZH_EZSP_ID_CLEAR_TRANSIENT_LINK_KEYS    0x006B  /* ezsp-enum.h:973 */
#define ZH_EZSP_ID_TRUST_CENTER_POST_JOIN_HANDLER 0x0024 /* ezsp-enum.h:987 */
#define ZH_EZSP_ID_GET_CHILD_DATA               0x004A  /* ezsp-enum.h:850 */
#define ZH_EZSP_ID_SEC_MAN_IMPORT_TRANSIENT_KEY 0x0111  /* ezsp-enum.h:980 */
#define ZH_EZSP_ID_READ_COUNTERS                0x00F1  /* ezsp-enum.h:793 */
#define ZH_EZSP_COUNTER_N                       42      /* sl_zigbee_types.h:1709 SL_ZIGBEE_COUNTER_TYPE_COUNT：readCounters 回这么多个 u16（command-functions.h:618-627） */

typedef enum {
    ZH_EZSP_OK = 0,
    ZH_EZSP_E_SHORT = -1,       /* 头都不够（含：第 2 字节说是扩展头，却不到 5 字节） */
    ZH_EZSP_E_RESERVED = -2,    /* 扩展头的 fc 高字节保留位（0x3C）置了（SDK 回 UNSUPPORTED_CONTROL） */
    ZH_EZSP_E_DIRECTION = -3,   /* 方向位是"命令"：主机自己的命令被回显（em260 重传覆盖应答缓冲） */
    ZH_EZSP_E_TRUNCATED = -4,   /* NCP 说它把这帧截断了 */
    ZH_EZSP_E_INVALID_CMD = -5, /* NCP 回 invalidCommand（帧 ID 0x0058），原因码在 invalid_reason */
    ZH_EZSP_E_LENGTH = -6,      /* 参数没恰好读完：多了少了都拒 */
    ZH_EZSP_E_ID = -7,          /* 解码器拿到的不是它认识的帧 ID */
    ZH_EZSP_E_ARG = -8,         /* 参数不合法：NULL、通用解码器给了表外的帧 ID（编码器参数不合法 / 放不下则回长度 0） */
} zh_ezsp_err_t;

/* ---- 写 ---- */
typedef struct {
    uint8_t *p;
    size_t   cap, n;
    bool     ovf;
} zh_ezsp_w_t;

/* 扩展头（网络号 0–3；越界整帧作废，w_end 回 0） */
void   zh_ezsp_w_begin(zh_ezsp_w_t *w, uint8_t *buf, size_t cap, uint8_t seq, uint16_t frame_id, uint8_t net_idx);
void   zh_ezsp_w_u8(zh_ezsp_w_t *w, uint8_t v);
void   zh_ezsp_w_u16(zh_ezsp_w_t *w, uint16_t v);
void   zh_ezsp_w_u32(zh_ezsp_w_t *w, uint32_t v);
void   zh_ezsp_w_bytes(zh_ezsp_w_t *w, const uint8_t *b, size_t n);
/* 帧长度；放不下（> cap 或 > ZH_EZSP_MAX）返回 0 */
size_t zh_ezsp_w_end(zh_ezsp_w_t *w);

/* ---- 读 ---- */
typedef struct {
    const uint8_t *p;
    size_t         n, off;
    bool           err;
} zh_ezsp_r_t;

uint8_t        zh_ezsp_r_u8(zh_ezsp_r_t *r);
uint16_t       zh_ezsp_r_u16(zh_ezsp_r_t *r);
uint32_t       zh_ezsp_r_u32(zh_ezsp_r_t *r);
void           zh_ezsp_r_bytes(zh_ezsp_r_t *r, uint8_t *out, size_t n);
const uint8_t *zh_ezsp_r_ptr(zh_ezsp_r_t *r, size_t n);   /* 不拷贝；越界返回 NULL 并置 err */

/* ---- 解析头 ---- */
typedef struct {
    uint8_t     seq;
    uint8_t     fc, fc_hi;
    uint16_t    id;
    bool        legacy;
    uint8_t     invalid_reason;   /* ZH_EZSP_E_INVALID_CMD 时的原因码 */
    zh_ezsp_r_t params;
} zh_ezsp_frame_t;

/* NCP 发来的一帧（ASH 交出的明文）。对照 ezsp.c responseReceived()：扩展头看 fc 高字节的格式位；保留位 / 方向 / 截断 / invalidCommand 都回错误，
   按 保留位 → invalidCommand → 方向 → 截断 的顺序先判先拒（与 SDK 的差异见文件头）。头解析出来之后才判出的错（保留位 / invalidCommand / 方向 / 截断），out 里的 seq / fc / id 仍已填好（上层按 seq 记账用）。 */
int  zh_ezsp_parse(const uint8_t *f, size_t n, zh_ezsp_frame_t *out);
static inline bool zh_ezsp_is_callback(const zh_ezsp_frame_t *fr) { return (fr->fc & (ZH_EZSP_FC_ASYNC_CB | ZH_EZSP_FC_SYNC_CB)) != 0; }
static inline bool zh_ezsp_pending_cb(const zh_ezsp_frame_t *fr) { return (fr->fc & ZH_EZSP_FC_PENDING_CB) != 0; }
static inline bool zh_ezsp_overflow(const zh_ezsp_frame_t *fr) { return (fr->fc & ZH_EZSP_FC_OVERFLOW) != 0; }

/* ---- 结构体（ezsp-frame-utilities.c/.h） ---- */
typedef struct {                 /* sl_zigbee_aps_frame_t，11 B */
    uint16_t profile_id, cluster_id;
    uint8_t  src_ep, dst_ep;
    uint16_t options, group_id;
    uint8_t  sequence;
} zh_ezsp_aps_t;

typedef struct {                 /* sl_zigbee_network_parameters_t，20 B */
    uint8_t  ext_pan_id[8];
    uint16_t pan_id;
    int8_t   tx_power;
    uint8_t  channel, join_method;
    uint16_t nwk_manager_id;
    uint8_t  nwk_update_id;
    uint32_t channels;
} zh_ezsp_net_params_t;

typedef struct {                 /* sl_zigbee_initial_security_state_t，43 B */
    uint16_t bitmask;
    uint8_t  preconfigured_key[16];
    uint8_t  network_key[16];
    uint8_t  network_key_seq;
    uint8_t  tc_eui64[8];
} zh_ezsp_init_sec_t;

typedef struct {                 /* sl_zigbee_rx_packet_info_t，18 B */
    uint16_t sender_short;
    uint8_t  sender_long[8];
    uint8_t  binding_index, address_index, last_hop_lqi;
    int8_t   last_hop_rssi;
    uint32_t last_hop_timestamp;
} zh_ezsp_rx_info_t;

/* ---- 命令编码（返回帧长度，0 = 参数不合法 / 放不下）。字段顺序见 command-functions.h 同名函数 ---- */
size_t zh_ezsp_enc_version_legacy(uint8_t *buf, size_t cap, uint8_t seq, uint8_t desired_protocol);
size_t zh_ezsp_enc_version(uint8_t *buf, size_t cap, uint8_t seq, uint8_t desired_protocol);
size_t zh_ezsp_enc_get_config(uint8_t *buf, size_t cap, uint8_t seq, uint8_t config_id);
size_t zh_ezsp_enc_set_config(uint8_t *buf, size_t cap, uint8_t seq, uint8_t config_id, uint16_t value);
size_t zh_ezsp_enc_add_endpoint(uint8_t *buf, size_t cap, uint8_t seq, uint8_t endpoint, uint16_t profile_id, uint16_t device_id, uint8_t device_version,
                                uint8_t in_count, uint8_t out_count, const uint16_t *in_clusters, const uint16_t *out_clusters);
size_t zh_ezsp_enc_set_policy(uint8_t *buf, size_t cap, uint8_t seq, uint8_t policy_id, uint8_t decision_id);
size_t zh_ezsp_enc_get_value(uint8_t *buf, size_t cap, uint8_t seq, uint8_t value_id);
size_t zh_ezsp_enc_set_value(uint8_t *buf, size_t cap, uint8_t seq, uint8_t value_id, uint8_t len, const uint8_t *value);
size_t zh_ezsp_enc_nop(uint8_t *buf, size_t cap, uint8_t seq);
/* 取一条待取的回调（NCP 用同步回调时；应答是那条回调帧，fc 带 0x08，或者 noCallbacks）（command-functions.h sl_zigbee_ezsp_callback） */
size_t zh_ezsp_enc_callback(uint8_t *buf, size_t cap, uint8_t seq);
size_t zh_ezsp_enc_custom_frame(uint8_t *buf, size_t cap, uint8_t seq, uint8_t len, const uint8_t *payload);
size_t zh_ezsp_enc_get_eui64(uint8_t *buf, size_t cap, uint8_t seq);
size_t zh_ezsp_enc_get_node_id(uint8_t *buf, size_t cap, uint8_t seq);
size_t zh_ezsp_enc_network_init(uint8_t *buf, size_t cap, uint8_t seq, uint16_t bitmask);
size_t zh_ezsp_enc_network_state(uint8_t *buf, size_t cap, uint8_t seq);
size_t zh_ezsp_enc_start_scan(uint8_t *buf, size_t cap, uint8_t seq, uint8_t scan_type, uint32_t channel_mask, uint8_t duration);
size_t zh_ezsp_enc_form_network(uint8_t *buf, size_t cap, uint8_t seq, const zh_ezsp_net_params_t *p);
size_t zh_ezsp_enc_leave_network(uint8_t *buf, size_t cap, uint8_t seq, uint8_t options);
/* duration 0xFF 在 Zigbee 里是"永久开放"（SDK stack/include/network-formation.h:121-125）：拒编、回 0。入网窗口 ≤ 254 s（设计 8.6、14.2） */
size_t zh_ezsp_enc_permit_joining(uint8_t *buf, size_t cap, uint8_t seq, uint8_t duration);
size_t zh_ezsp_enc_get_network_parameters(uint8_t *buf, size_t cap, uint8_t seq);
size_t zh_ezsp_enc_set_radio_power(uint8_t *buf, size_t cap, uint8_t seq, int8_t power);
size_t zh_ezsp_enc_set_concentrator(uint8_t *buf, size_t cap, uint8_t seq, bool on, uint16_t type, uint16_t min_time, uint16_t max_time,
                                    uint8_t route_error_threshold, uint8_t delivery_failure_threshold, uint8_t max_hops);
size_t zh_ezsp_enc_set_source_route_discovery_mode(uint8_t *buf, size_t cap, uint8_t seq, uint8_t mode);   /* mode：0 关 / 1 开 / 2 立即发一次再按周期 */
size_t zh_ezsp_enc_send_unicast(uint8_t *buf, size_t cap, uint8_t seq, uint8_t type, uint16_t index_or_dest, const zh_ezsp_aps_t *aps,
                                uint16_t tag, uint8_t len, const uint8_t *msg);
size_t zh_ezsp_enc_send_broadcast(uint8_t *buf, size_t cap, uint8_t seq, uint16_t alias, uint16_t dest, uint8_t nwk_seq, const zh_ezsp_aps_t *aps,
                                  uint8_t radius, uint16_t tag, uint8_t len, const uint8_t *msg);
size_t zh_ezsp_enc_lookup_eui64(uint8_t *buf, size_t cap, uint8_t seq, uint16_t node_id);
size_t zh_ezsp_enc_lookup_node_id(uint8_t *buf, size_t cap, uint8_t seq, const uint8_t eui64[8]);
size_t zh_ezsp_enc_set_initial_security_state(uint8_t *buf, size_t cap, uint8_t seq, const zh_ezsp_init_sec_t *s);
size_t zh_ezsp_enc_get_current_security_state(uint8_t *buf, size_t cap, uint8_t seq);
size_t zh_ezsp_enc_clear_transient_link_keys(uint8_t *buf, size_t cap, uint8_t seq);
/* 临时链路密钥（入网窗口用）：[EUI64 8][密钥 16]，回 u32 status（command-functions.h:3032-3046，append_sl_zigbee_sec_man_key_t 是裸 16 字节） */
size_t zh_ezsp_enc_import_transient_key(uint8_t *buf, size_t cap, uint8_t seq, const uint8_t eui64[8], const uint8_t key[16]);

/* ---- 回包解码（fr 是 zh_ezsp_parse 的结果；帧 ID 不对回 ZH_EZSP_E_ID，参数没恰好读完回 ZH_EZSP_E_LENGTH）。
 * 解码器自己再查一遍头：方向是命令（回显）、截断、保留位的帧照样拒（上层漏看 parse 的返回值时，nop 保活不会把回显当应答）；
 * 旧式头只有 dec_version 认（首条 version 的应答；"只认首条"是会话状态，在 zh_ezsp_ses）。
 * 输出指针都可以给 NULL（不要的字段）；返回非 0 时输出里的内容不可用（可能写了一半），调用方只看返回值。 ---- */
int zh_ezsp_dec_version(const zh_ezsp_frame_t *fr, uint8_t *protocol, uint8_t *stack_type, uint16_t *stack_version);
int zh_ezsp_dec_get_config(const zh_ezsp_frame_t *fr, uint32_t *status, uint16_t *value);
/* 只有一个 4 B 状态的回包：set_config / add_endpoint / set_policy / set_value / network_init / start_scan / form_network / leave_network /
   permit_joining / set_radio_power / set_concentrator / set_initial_security_state；id 是期望的帧 ID，不在这张表里回 ZH_EZSP_E_ARG */
int zh_ezsp_dec_status(const zh_ezsp_frame_t *fr, uint16_t id, uint32_t *status);
/* 没有参数的回包：nop / clear_transient_link_keys / noCallbacks（callback 命令在没有待取回调时的应答）；别的 ID 回 ZH_EZSP_E_ARG */
int zh_ezsp_dec_empty(const zh_ezsp_frame_t *fr, uint16_t id);
int zh_ezsp_dec_get_value(const zh_ezsp_frame_t *fr, uint32_t *status, uint8_t *len, const uint8_t **value);
int zh_ezsp_dec_custom_frame(const zh_ezsp_frame_t *fr, uint32_t *status, uint8_t *len, const uint8_t **reply);
int zh_ezsp_dec_get_eui64(const zh_ezsp_frame_t *fr, uint8_t eui64[8]);
int zh_ezsp_dec_get_node_id(const zh_ezsp_frame_t *fr, uint16_t *node_id);
int zh_ezsp_dec_network_state(const zh_ezsp_frame_t *fr, uint8_t *state);
/* setSourceRouteDiscoveryMode 的回包不是状态，是 4 B 的"距下一次 MTORR 广播的毫秒数"（关掉时 0xFFFFFFFF；command-prototypes.h:1700-1706、
   command-functions.h:2384-2397） */
int zh_ezsp_dec_source_route_discovery_mode(const zh_ezsp_frame_t *fr, uint32_t *remaining_ms);
int zh_ezsp_dec_get_network_parameters(const zh_ezsp_frame_t *fr, uint32_t *status, uint8_t *node_type, zh_ezsp_net_params_t *p);
int zh_ezsp_dec_send(const zh_ezsp_frame_t *fr, uint16_t id, uint32_t *status, uint8_t *aps_seq);   /* send_unicast / send_broadcast */
int zh_ezsp_dec_lookup_eui64(const zh_ezsp_frame_t *fr, uint32_t *status, uint8_t eui64[8]);
int zh_ezsp_dec_lookup_node_id(const zh_ezsp_frame_t *fr, uint32_t *status, uint16_t *node_id);
int zh_ezsp_dec_get_current_security_state(const zh_ezsp_frame_t *fr, uint32_t *status, uint16_t *bitmask, uint8_t tc_eui64[8]);

/* ---- 回调解码（command-functions.h callbackDispatch） ---- */
int zh_ezsp_dec_stack_status(const zh_ezsp_frame_t *fr, uint32_t *status);
int zh_ezsp_dec_energy_scan_result(const zh_ezsp_frame_t *fr, uint8_t *channel, int8_t *max_rssi);
int zh_ezsp_dec_scan_complete(const zh_ezsp_frame_t *fr, uint8_t *channel, uint32_t *status);
int zh_ezsp_dec_child_join(const zh_ezsp_frame_t *fr, uint8_t *index, bool *joining, uint16_t *child_id, uint8_t child_eui64[8], uint8_t *child_type);
int zh_ezsp_dec_tc_post_join(const zh_ezsp_frame_t *fr, uint16_t *node_id, uint8_t eui64[8], uint8_t *status, uint8_t *policy_decision,
                             uint16_t *parent_id);
int zh_ezsp_dec_message_sent(const zh_ezsp_frame_t *fr, uint32_t *status, uint8_t *type, uint16_t *index_or_dest, zh_ezsp_aps_t *aps,
                             uint16_t *tag, uint8_t *len, const uint8_t **msg);
/* incomingMessageHandler：生成的分发函数（command-functions.h:4602-4615）读到消息为止，但真 NCP 在消息后面**还跟一个字节**——
   源路由开销（"NCP 主动送来的"，SDK 主机在回调里另外取：af-host.c:810-814 → ezsp.c:752-757 getSourceRouteOverhead；0xFF =
   SL_ZIGBEE_SOURCE_ROUTE_OVERHEAD_UNKNOWN，sl_zigbee_types.h:3064）。所以这里的布局是 类型 / APS / 接收信息 / 长度 / 消息 / 开销，
   照样要求恰好读完。2026-10-22 第五步首次上板：只按分发函数的布局解，真设备的上报（人体传感器的 IAS Zone）条条多一个字节、全被拒
   （预言机只比了分发函数，看不见这个字节）。 */
int zh_ezsp_dec_incoming_message(const zh_ezsp_frame_t *fr, uint8_t *type, zh_ezsp_aps_t *aps, zh_ezsp_rx_info_t *info, uint8_t *len,
                                 const uint8_t **msg, uint8_t *src_route_overhead);
int zh_ezsp_dec_incoming_route_record(const zh_ezsp_frame_t *fr, uint16_t *source, uint8_t source_eui[8], uint8_t *lqi, int8_t *rssi,
                                      uint8_t *relay_count, const uint8_t **relays);   /* relays：relay_count 个小端 u16 */
int zh_ezsp_dec_id_conflict(const zh_ezsp_frame_t *fr, uint16_t *id);
int zh_ezsp_dec_custom_frame_handler(const zh_ezsp_frame_t *fr, uint8_t *len, const uint8_t **payload);

/* 诊断（NCP 侧计数器与子设备表，只读）。子设备一项的线上格式见 ezsp-frame-utilities.c:258-267 */
typedef struct {
    uint8_t  eui64[8];
    uint8_t  type;                /* sl_zigbee_node_type_t（2 = 终端、3 = 休眠终端…） */
    uint16_t id;                  /* 短地址 */
    uint8_t  phy, power, timeout; /* timeout：终端超时档（2^n 分钟，0 = 10 s） */
    uint32_t remaining;           /* 离超时还剩（NCP 的单位：秒） */
} zh_ezsp_child_t;
size_t zh_ezsp_enc_read_counters(uint8_t *buf, size_t cap, uint8_t seq);
size_t zh_ezsp_enc_get_child_data(uint8_t *buf, size_t cap, uint8_t seq, uint8_t index);
int zh_ezsp_dec_read_counters(const zh_ezsp_frame_t *fr, uint16_t v[ZH_EZSP_COUNTER_N]);
int zh_ezsp_dec_child_data(const zh_ezsp_frame_t *fr, uint32_t *status, zh_ezsp_child_t *c);   /* status != 0（NOT_JOINED 等）时 c 照样解、内容无意义 */
