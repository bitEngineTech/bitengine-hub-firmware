/* zh_ezsp_core 对 SDK 主机侧 EZSP 实现的预言机测试（docs/15 设计 8.4；run.sh 的第 ④ 段）。
 *
 * SDK 的 protocol/zigbee/app/util/ezsp/ezsp.c（含 command-functions.h）、ezsp-frame-utilities.c、ezsp-callbacks.c 原样编进来，
 * 串口层换成这里的替身：serialSendCommand 截下 SDK 编出的命令帧，serialResponseReceived 把测试拼好的回包放进同一块缓冲（与真串口层一样原地）。
 *  ① 编码：同样的参数，SDK 的 sl_zigbee_ezsp_xxx() 编出的字节与 zh_ezsp_enc_xxx() 逐字节相同（含 seq、帧控制、首条 version 的旧式头、220 字节上限）；
 *  ② 回包：同一串随机回包字节，SDK 取出的每个输出与 zh_ezsp_dec_xxx() 取出的逐字段相同；
 *  ③ 回调：同一串回调帧经 sl_zigbee_ezsp_tick() 派发到 SDK 的回调，参数与 zh_ezsp_dec_xxx() 逐字段相同；
 *  ④ 头的分类：fc × fc 高字节 × 帧 ID × 原因码 全排列，SDK 收不收与 zh_ezsp_parse() 一致——除了头文件里写明的"我们更严"：
 *     "SDK 收、我们拒"的格子与"SDK 把下一帧当应答"的格子各自单独断言，而且只在预言的那些格子里出现。
 *  每一段结束都核对 SDK 的 error_handler 没被叫过、SDK 也没有多等一帧（有意构造错误的两段自己逐格核对）。
 * SDK 源码是 Silicon Labs MSLA：只在 run.sh 里从本机路径编，不进仓库（UPSTREAM.md）。 */
#include PLATFORM_HEADER
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stack/include/sl_zigbee_types.h"
#include "app/util/ezsp/ezsp-protocol.h"
#include "app/util/ezsp/ezsp.h"
#include "app/util/ezsp/serial-interface.h"
#include "app/util/ezsp/ezsp-frame-utilities.h"

#include "zh_ezsp_core.h"

static int s_fail, s_pass;
#define CHECK(cond, ...) do { if (cond) { s_pass++; } else { s_fail++; printf("❌ %s:%d ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

#define ITER 200

static uint32_t s_rng = 0x9E3779B9u;
static uint32_t r32(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}
static uint8_t r8(void) { return (uint8_t)r32(); }
static uint16_t r16(void) { return (uint16_t)r32(); }
static void rbytes(uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = r8(); }

/* ---------------------------------------------------------------- 串口层替身 */

static uint8_t s_buf[256];                 /* SDK 的 ezspFrameContents：命令与回包共用（与 serial-interface-uart.c 一样原地） */
static uint8_t s_len;
uint8_t *ezspFrameContents = s_buf;
uint8_t *ezspFrameLengthLocation = &s_len;

static uint8_t s_cmd[256];                 /* 截下的命令帧 */
static size_t  s_cmd_n;
static int     s_nsent;
static uint8_t s_rsp[256];                 /* 排队的回包 / 回调帧 */
static size_t  s_rsp_n;
static bool    s_awaiting;                 /* SDK 发了命令、测试排的回包还没被取走 */
static bool    s_in_cmd;                   /* SDK 还在 sendCommand 里（取走回包之后它若再来要，就是它没把那帧当应答） */
static int     s_sentinels;                /* 为把 SDK 从等待里放出来补发的哨兵应答（nop 的正常应答）次数 */
static int     s_err[8], s_nerr;           /* sl_zigbee_ezsp_error_handler 收到的 */

sl_zigbee_ezsp_status_t serialSendCommand(void)
{
    memcpy(s_cmd, s_buf, s_len);
    s_cmd_n = s_len;
    s_nsent++;
    s_awaiting = true;
    s_in_cmd = true;
    return SL_ZIGBEE_EZSP_SUCCESS;
}

sl_zigbee_ezsp_status_t serialResponseReceived(void)
{
    if (s_rsp_n) {
        memset(s_buf, 0xA5, sizeof(s_buf));       /* 不让上一帧的残留碰巧凑对 */
        memcpy(s_buf, s_rsp, s_rsp_n);
        s_len = (uint8_t)s_rsp_n;
        s_rsp_n = 0;
        s_awaiting = false;
        return SL_ZIGBEE_EZSP_SUCCESS;
    }
    if (s_awaiting) {                             /* sendCommand 会死等：测试漏排了回包，直接失败退出 */
        printf("❌ SDK 在等回包，测试没排\n");
        exit(2);
    }
    if (s_in_cmd) {                               /* SDK 取走了回包却还在等（它把那帧当成"还没收到"）：补一帧 nop 的正常应答放它出来 */
        static const uint8_t k_sentinel[] = { 0x00, 0x80, 0x01, 0x05, 0x00 };
        memset(s_buf, 0xA5, sizeof(s_buf));
        memcpy(s_buf, k_sentinel, sizeof(k_sentinel));
        s_len = sizeof(k_sentinel);
        s_in_cmd = false;
        s_sentinels++;
        return SL_ZIGBEE_EZSP_SUCCESS;
    }
    return SL_ZIGBEE_EZSP_NO_RX_DATA;
}

uint8_t serialPendingResponseCount(void) { return 0; }
void simulatedTimePasses(void) {}
void sl_zigbee_ezsp_error_handler(sl_zigbee_ezsp_status_t status)
{
    if (s_nerr < 8) s_err[s_nerr] = status;
    s_nerr++;
}

/* ---------------------------------------------------------------- 回调替身（截参数） */

static struct {
    int      hit;
    uint16_t id;
    uint32_t status;
    uint8_t  u8a, u8b, u8c, u8d;
    int8_t   i8;
    uint16_t u16a, u16b;
    uint8_t  eui[8];
    sl_zigbee_aps_frame_t aps;
    sl_zigbee_rx_packet_info_t ri;
    uint8_t  len;
    uint8_t  data[256];
} s_cb;

static void cb_hit(uint16_t id) { s_cb.hit++; s_cb.id = id; }

void sl_zigbee_ezsp_stack_status_handler(sl_status_t status) { cb_hit(ZH_EZSP_ID_STACK_STATUS_HANDLER); s_cb.status = status; }
void sl_zigbee_ezsp_energy_scan_result_handler(uint8_t channel, int8_t maxRssiValue)
{
    cb_hit(ZH_EZSP_ID_ENERGY_SCAN_RESULT_HANDLER);
    s_cb.u8a = channel;
    s_cb.i8 = maxRssiValue;
}
void sl_zigbee_ezsp_scan_complete_handler(uint8_t channel, sl_status_t status)
{
    cb_hit(ZH_EZSP_ID_SCAN_COMPLETE_HANDLER);
    s_cb.u8a = channel;
    s_cb.status = status;
}
void sl_zigbee_ezsp_network_found_handler(sl_zigbee_zigbee_network_t *networkFound, uint8_t lastHopLqi, int8_t lastHopRssi)
{
    (void)networkFound; (void)lastHopLqi; (void)lastHopRssi;
    cb_hit(0xFFFF);
}
void sl_zigbee_ezsp_unused_pan_id_found_handler(sl_802154_pan_id_t panId, uint8_t channel)
{
    (void)panId; (void)channel;
    cb_hit(0xFFFF);
}
void sl_zigbee_ezsp_child_join_handler(uint8_t index, bool joining, sl_802154_short_addr_t childId, sl_802154_long_addr_t childEui64,
                                       sl_zigbee_node_type_t childType)
{
    cb_hit(ZH_EZSP_ID_CHILD_JOIN_HANDLER);
    s_cb.u8a = index;
    s_cb.u8b = joining ? 1 : 0;
    s_cb.u16a = childId;
    memcpy(s_cb.eui, childEui64, 8);
    s_cb.u8c = childType;
}
void sl_zigbee_ezsp_trust_center_post_join_handler(sl_802154_short_addr_t newNodeId, sl_802154_long_addr_t newNodeEui64,
                                                   sl_zigbee_device_update_t status, sl_zigbee_join_decision_t policyDecision,
                                                   sl_802154_short_addr_t parentOfNewNodeId)
{
    cb_hit(ZH_EZSP_ID_TRUST_CENTER_POST_JOIN_HANDLER);
    s_cb.u16a = newNodeId;
    memcpy(s_cb.eui, newNodeEui64, 8);
    s_cb.u8a = status;
    s_cb.u8b = policyDecision;
    s_cb.u16b = parentOfNewNodeId;
}
void sl_zigbee_message_sent_handler(sl_status_t status, sl_zigbee_outgoing_message_type_t type, uint16_t indexOrDestination,
                                    sl_zigbee_aps_frame_t *apsFrame, uint16_t messageTag, uint8_t messageLength, uint8_t *message)
{
    cb_hit(ZH_EZSP_ID_MESSAGE_SENT_HANDLER);
    s_cb.status = status;
    s_cb.u8a = type;
    s_cb.u16a = indexOrDestination;
    s_cb.aps = *apsFrame;
    s_cb.u16b = messageTag;
    s_cb.len = messageLength;
    memcpy(s_cb.data, message, messageLength);
}
void sl_zigbee_incoming_message_handler(sl_zigbee_incoming_message_type_t type, sl_zigbee_aps_frame_t *apsFrame,
                                        sl_zigbee_rx_packet_info_t *packetInfo, uint8_t messageLength, uint8_t *message)
{
    cb_hit(ZH_EZSP_ID_INCOMING_MESSAGE_HANDLER);
    s_cb.u8a = type;
    s_cb.aps = *apsFrame;
    s_cb.ri = *packetInfo;
    s_cb.len = messageLength;
    memcpy(s_cb.data, message, messageLength);
    s_cb.u8b = getSourceRouteOverhead(messageLength);   /* 同 SDK 主机框架（af-host.c:810-814）：分发函数之外再取一个字节 */
}
void sl_zigbee_ezsp_incoming_route_record_handler(sl_802154_short_addr_t source, sl_802154_long_addr_t sourceEui, uint8_t lastHopLqi,
                                                  int8_t lastHopRssi, uint8_t relayCount, uint8_t *relayList)
{
    cb_hit(ZH_EZSP_ID_INCOMING_ROUTE_RECORD_HANDLER);
    s_cb.u16a = source;
    memcpy(s_cb.eui, sourceEui, 8);
    s_cb.u8a = lastHopLqi;
    s_cb.i8 = lastHopRssi;
    s_cb.len = relayCount;
    memcpy(s_cb.data, relayList, (size_t)relayCount * 2);
}
void sl_zigbee_ezsp_id_conflict_handler(sl_802154_short_addr_t id) { cb_hit(ZH_EZSP_ID_ID_CONFLICT_HANDLER); s_cb.u16a = id; }
void sl_zigbee_ezsp_no_callbacks(void) { cb_hit(ZH_EZSP_ID_NO_CALLBACKS); }   /* ezsp-callbacks.c 的默认实现用 EZSP_APPLICATION_HAS_NO_CALLBACKS 让出来 */
void sl_zigbee_ezsp_custom_frame_handler(uint8_t payloadLength, uint8_t *payload)
{
    cb_hit(ZH_EZSP_ID_CUSTOM_FRAME_HANDLER);
    s_cb.len = payloadLength;
    memcpy(s_cb.data, payload, payloadLength);
}

/* ---------------------------------------------------------------- 夹具 */

static uint8_t s_seq;                       /* SDK 下一条命令的 seq（ezsp.c 的 ezspSequence，每条命令 +1，超长被拒的也算） */
static uint8_t s_keep[256];                 /* 回包副本：SDK 解完以后我们解同一串字节 */
static size_t  s_keep_n;

/* 排一帧回包：[seq][fc][0x01][id 小端] + 参数 */
static void queue(uint8_t fc, uint16_t id, const uint8_t *par, size_t n)
{
    s_rsp[0] = s_seq;
    s_rsp[1] = fc;
    s_rsp[2] = 0x01;
    s_rsp[3] = (uint8_t)id;
    s_rsp[4] = (uint8_t)(id >> 8);
    if (n) memcpy(s_rsp + 5, par, n);
    s_rsp_n = 5 + n;
    memcpy(s_keep, s_rsp, s_rsp_n);
    s_keep_n = s_rsp_n;
}
static void queue_rsp(uint16_t id, const uint8_t *par, size_t n) { queue(0x80, id, par, n); }

/* SDK 刚发的命令与我们编的是否逐字节相同；然后 seq 前进 */
static bool same_cmd(const uint8_t *ours, size_t n)
{
    const bool ok = n != 0 && n == s_cmd_n && !memcmp(ours, s_cmd, n) && s_cmd[0] == s_seq;
    if (!ok) {
        printf("   SDK(%zu):", s_cmd_n);
        for (size_t i = 0; i < s_cmd_n; i++) printf(" %02X", s_cmd[i]);
        printf("\n   我们(%zu):", n);
        for (size_t i = 0; i < n; i++) printf(" %02X", ours[i]);
        printf("\n");
    }
    s_seq++;
    return ok;
}

static bool parse_keep(zh_ezsp_frame_t *fr) { return zh_ezsp_parse(s_keep, s_keep_n, fr) == ZH_EZSP_OK; }

static void rand_aps(sl_zigbee_aps_frame_t *a, zh_ezsp_aps_t *z)
{
    z->profile_id = a->profileId = r16();
    z->cluster_id = a->clusterId = r16();
    z->src_ep = a->sourceEndpoint = r8();
    z->dst_ep = a->destinationEndpoint = r8();
    z->options = a->options = r16();
    z->group_id = a->groupId = r16();
    z->sequence = a->sequence = r8();
}
static bool aps_eq(const sl_zigbee_aps_frame_t *a, const zh_ezsp_aps_t *z)
{
    return a->profileId == z->profile_id && a->clusterId == z->cluster_id && a->sourceEndpoint == z->src_ep && a->destinationEndpoint == z->dst_ep &&
           a->options == z->options && a->groupId == z->group_id && a->sequence == z->sequence;
}

/* ---------------------------------------------------------------- ① + ② 命令与回包 */

static void t_version(void)
{
    /* 必须是 SDK 收到的第一条命令：只有它走旧式头 */
    uint8_t ours[256], par[4];
    rbytes(par, 4);
    s_rsp[0] = s_seq; s_rsp[1] = 0x80; s_rsp[2] = 0x00;    /* 旧式头的回包（第 2 字节是帧 ID 0x00，格式位 0） */
    memcpy(s_rsp + 3, par, 4);
    s_rsp_n = 7;
    memcpy(s_keep, s_rsp, 7);
    s_keep_n = 7;
    uint8_t st = 0;
    uint16_t sv = 0;
    const uint8_t pv = sl_zigbee_ezsp_version(ZH_EZSP_PROTOCOL_VERSION, &st, &sv);
    CHECK(same_cmd(ours, zh_ezsp_enc_version_legacy(ours, sizeof(ours), s_seq, ZH_EZSP_PROTOCOL_VERSION)), "首条 version：旧式头逐字节相同");
    zh_ezsp_frame_t fr;
    uint8_t zp = 0, zt = 0;
    uint16_t zv = 0;
    CHECK(zh_ezsp_parse(s_keep, s_keep_n, &fr) == ZH_EZSP_OK && fr.legacy && zh_ezsp_dec_version(&fr, &zp, &zt, &zv) == ZH_EZSP_OK &&
          zp == pv && zt == st && zv == sv, "旧式头的 version 回包：协议 / 栈类型 / 栈版本与 SDK 相同");
    int bad = 0;
    for (int i = 0; i < ITER; i++) {
        const uint8_t want = r8();
        rbytes(par, 4);
        queue_rsp(ZH_EZSP_ID_VERSION, par, 4);
        const uint8_t p = sl_zigbee_ezsp_version(want, &st, &sv);
        if (!same_cmd(ours, zh_ezsp_enc_version(ours, sizeof(ours), s_seq, want))) bad++;
        if (!parse_keep(&fr) || zh_ezsp_dec_version(&fr, &zp, &zt, &zv) != ZH_EZSP_OK || zp != p || zt != st || zv != sv) bad++;
    }
    CHECK(bad == 0, "其后的 version × %d：扩展头编码、回包解码与 SDK 相同（%d 处不同）", ITER, bad);
}

/* 只回一个 4 B 状态的命令：编码比对 + 状态比对 */
static int status_case(uint16_t id, sl_status_t sdk_status, const uint8_t *ours, size_t on)
{
    int bad = !same_cmd(ours, on);
    zh_ezsp_frame_t fr;
    uint32_t zs = 0;
    if (!parse_keep(&fr) || zh_ezsp_dec_status(&fr, id, &zs) != ZH_EZSP_OK || zs != sdk_status) bad++;
    return bad;
}
static void q_status(uint16_t id)
{
    uint8_t par[4];
    rbytes(par, 4);
    queue_rsp(id, par, 4);
}

static void t_config_policy_value(void)
{
    uint8_t ours[256];
    int bad = 0;
    for (int i = 0; i < ITER; i++) {   /* get_config */
        const uint8_t cid = r8();
        uint8_t par[6];
        rbytes(par, 6);
        queue_rsp(ZH_EZSP_ID_GET_CONFIGURATION_VALUE, par, 6);
        uint16_t v = 0;
        const sl_status_t s = sl_zigbee_ezsp_get_configuration_value(cid, &v);
        if (!same_cmd(ours, zh_ezsp_enc_get_config(ours, sizeof(ours), s_seq, cid))) bad++;
        zh_ezsp_frame_t fr;
        uint32_t zs;
        uint16_t zv;
        if (!parse_keep(&fr) || zh_ezsp_dec_get_config(&fr, &zs, &zv) != ZH_EZSP_OK || zs != s || zv != v) bad++;
    }
    CHECK(bad == 0, "getConfigurationValue × %d（%d 处不同）", ITER, bad);

    bad = 0;
    for (int i = 0; i < ITER; i++) {   /* set_config */
        const uint8_t cid = r8();
        const uint16_t v = r16();
        q_status(ZH_EZSP_ID_SET_CONFIGURATION_VALUE);
        const sl_status_t s = sl_zigbee_ezsp_set_configuration_value(cid, v);
        bad += status_case(ZH_EZSP_ID_SET_CONFIGURATION_VALUE, s, ours, zh_ezsp_enc_set_config(ours, sizeof(ours), s_seq, cid, v));
    }
    CHECK(bad == 0, "setConfigurationValue × %d（%d 处不同）", ITER, bad);

    bad = 0;
    for (int i = 0; i < ITER; i++) {   /* set_policy */
        const uint8_t p = r8(), d = r8();
        q_status(ZH_EZSP_ID_SET_POLICY);
        const sl_status_t s = sl_zigbee_ezsp_set_policy(p, d);
        bad += status_case(ZH_EZSP_ID_SET_POLICY, s, ours, zh_ezsp_enc_set_policy(ours, sizeof(ours), s_seq, p, d));
    }
    CHECK(bad == 0, "setPolicy × %d（%d 处不同）", ITER, bad);

    bad = 0;
    for (int i = 0; i < ITER; i++) {   /* get_value：回包里的长度 0..210 */
        const uint8_t vid = r8();
        uint8_t par[220];
        const uint8_t L = (uint8_t)(i == 0 ? 0 : r8() % 211);
        rbytes(par, 4);
        par[4] = L;
        rbytes(par + 5, L);
        queue_rsp(ZH_EZSP_ID_GET_VALUE, par, 5u + L);
        uint8_t vl = 255, val[255];
        const sl_status_t s = sl_zigbee_ezsp_get_value(vid, &vl, val);
        if (!same_cmd(ours, zh_ezsp_enc_get_value(ours, sizeof(ours), s_seq, vid))) bad++;
        zh_ezsp_frame_t fr;
        uint32_t zs;
        uint8_t zl;
        const uint8_t *zvp;
        if (!parse_keep(&fr) || zh_ezsp_dec_get_value(&fr, &zs, &zl, &zvp) != ZH_EZSP_OK || zs != s || zl != vl || memcmp(zvp, val, vl)) bad++;
    }
    CHECK(bad == 0, "getValue × %d（变长回包；%d 处不同）", ITER, bad);

    bad = 0;
    for (int i = 0; i < ITER; i++) {   /* set_value：长度 0..213（再长就超 220） */
        const uint8_t vid = r8();
        const uint8_t L = (uint8_t)(i == 0 ? 0 : (i == 1 ? 213 : r8() % 214));
        uint8_t v[255];
        rbytes(v, L);
        q_status(ZH_EZSP_ID_SET_VALUE);
        const sl_status_t s = sl_zigbee_ezsp_set_value(vid, L, v);
        bad += status_case(ZH_EZSP_ID_SET_VALUE, s, ours, zh_ezsp_enc_set_value(ours, sizeof(ours), s_seq, vid, L, v));
    }
    CHECK(bad == 0, "setValue × %d（含 213 B = 恰好 220；%d 处不同）", ITER, bad);
}

static void t_too_long(void)
{
    /* SDK：sendCommand 发现 > 220 就报 COMMAND_TOO_LONG、不发；我们：编码器回 0。两边在同一个长度上翻转 */
    uint8_t ours[300], v[255];
    rbytes(v, sizeof(v));
    int bad = 0;
    for (int L = 205; L <= 230; L++) {
        q_status(ZH_EZSP_ID_SET_VALUE);
        s_nerr = 0;
        const int sent0 = s_nsent;
        (void)sl_zigbee_ezsp_set_value(1, (uint8_t)L, v);
        const bool sdk_long = s_nsent == sent0 && s_nerr == 1 && s_err[0] == SL_ZIGBEE_EZSP_ERROR_COMMAND_TOO_LONG;
        const size_t on = zh_ezsp_enc_set_value(ours, sizeof(ours), s_seq, 1, (uint8_t)L, v);
        if (sdk_long) {
            if (on != 0) bad++;
            s_rsp_n = 0;
            s_seq++;                                        /* 超长被拒的也吃掉一个 seq（ezsp.c:619-620 在长度检查之前） */
        } else {
            if (on == 0 || !same_cmd(ours, on) || s_nerr != 0) bad++;
        }
        if ((L <= 213) == sdk_long) bad++;                  /* 5 + 1 + 1 + 213 = 220 是最后一个能发的 */
    }
    s_nerr = 0;
    CHECK(bad == 0, "setValue 205–230 B：SDK 报 COMMAND_TOO_LONG 的长度恰好是我们回 0 的长度（214 起；其余长度 SDK 不报错；%d 处不同）", bad);
    /* 超长被拒以后 seq 仍与 SDK 同步 */
    q_status(ZH_EZSP_ID_PERMIT_JOINING);
    const sl_status_t s = sl_zigbee_ezsp_permit_joining(60);
    CHECK(status_case(ZH_EZSP_ID_PERMIT_JOINING, s, ours, zh_ezsp_enc_permit_joining(ours, sizeof(ours), s_seq, 60)) == 0 && s_nerr == 0,
          "超长被拒以后 seq 仍对得上");
}

static void t_endpoint_custom_misc(void)
{
    uint8_t ours[256];
    int bad = 0;
    for (int i = 0; i < ITER; i++) {   /* add_endpoint：簇表各 0..20 */
        uint16_t in[20], out[20];
        const uint8_t ni = r8() % 21, no = r8() % 21;
        for (int k = 0; k < 20; k++) { in[k] = r16(); out[k] = r16(); }
        const uint8_t ep = r8(), dv = r8();
        const uint16_t pid = r16(), did = r16();
        q_status(ZH_EZSP_ID_ADD_ENDPOINT);
        const sl_status_t s = sl_zigbee_ezsp_add_endpoint(ep, pid, did, dv, ni, no, in, out);
        bad += status_case(ZH_EZSP_ID_ADD_ENDPOINT, s, ours, zh_ezsp_enc_add_endpoint(ours, sizeof(ours), s_seq, ep, pid, did, dv, ni, no, in, out));
    }
    CHECK(bad == 0, "addEndpoint × %d（%d 处不同）", ITER, bad);

    bad = 0;
    for (int i = 0; i < ITER; i++) {   /* custom_frame：发 0..200、回 0..200 */
        uint8_t pl[255], par[220];
        const uint8_t L = (uint8_t)(r8() % 201), R = (uint8_t)(r8() % 201);
        rbytes(pl, L);
        rbytes(par, 4);
        par[4] = R;
        rbytes(par + 5, R);
        queue_rsp(ZH_EZSP_ID_CUSTOM_FRAME, par, 5u + R);
        uint8_t rl = 255, rep[255];
        const sl_status_t s = sl_zigbee_ezsp_custom_frame(L, pl, &rl, rep);
        if (!same_cmd(ours, zh_ezsp_enc_custom_frame(ours, sizeof(ours), s_seq, L, pl))) bad++;
        zh_ezsp_frame_t fr;
        uint32_t zs;
        uint8_t zl;
        const uint8_t *zr;
        if (!parse_keep(&fr) || zh_ezsp_dec_custom_frame(&fr, &zs, &zl, &zr) != ZH_EZSP_OK || zs != s || zl != rl || memcmp(zr, rep, rl)) bad++;
    }
    CHECK(bad == 0, "customFrame × %d（双向变长；%d 处不同）", ITER, bad);

    bad = 0;
    for (int i = 0; i < ITER; i++) {   /* nop / get_eui64 / get_node_id / network_state / clear_transient_link_keys */
        zh_ezsp_frame_t fr;
        queue_rsp(ZH_EZSP_ID_NOP, NULL, 0);
        sl_zigbee_ezsp_nop();
        if (!same_cmd(ours, zh_ezsp_enc_nop(ours, sizeof(ours), s_seq))) bad++;
        if (!parse_keep(&fr) || zh_ezsp_dec_empty(&fr, ZH_EZSP_ID_NOP) != ZH_EZSP_OK) bad++;

        uint8_t e[8], ze[8];
        rbytes(e, 8);
        queue_rsp(ZH_EZSP_ID_GET_EUI64, e, 8);
        uint8_t se[8];
        sl_zigbee_ezsp_get_eui64(se);
        if (!same_cmd(ours, zh_ezsp_enc_get_eui64(ours, sizeof(ours), s_seq))) bad++;
        if (!parse_keep(&fr) || zh_ezsp_dec_get_eui64(&fr, ze) != ZH_EZSP_OK || memcmp(ze, se, 8)) bad++;

        uint8_t par[2];
        rbytes(par, 2);
        queue_rsp(ZH_EZSP_ID_GET_NODE_ID, par, 2);
        const uint16_t nid = sl_zigbee_ezsp_get_node_id();
        uint16_t zn;
        if (!same_cmd(ours, zh_ezsp_enc_get_node_id(ours, sizeof(ours), s_seq))) bad++;
        if (!parse_keep(&fr) || zh_ezsp_dec_get_node_id(&fr, &zn) != ZH_EZSP_OK || zn != nid) bad++;

        par[0] = r8();
        queue_rsp(ZH_EZSP_ID_NETWORK_STATE, par, 1);
        const uint8_t ns = sl_zigbee_ezsp_network_state();
        uint8_t zns;
        if (!same_cmd(ours, zh_ezsp_enc_network_state(ours, sizeof(ours), s_seq))) bad++;
        if (!parse_keep(&fr) || zh_ezsp_dec_network_state(&fr, &zns) != ZH_EZSP_OK || zns != ns) bad++;

        queue_rsp(ZH_EZSP_ID_CLEAR_TRANSIENT_LINK_KEYS, NULL, 0);
        sl_zigbee_ezsp_clear_transient_link_keys();
        if (!same_cmd(ours, zh_ezsp_enc_clear_transient_link_keys(ours, sizeof(ours), s_seq))) bad++;
        if (!parse_keep(&fr) || zh_ezsp_dec_empty(&fr, ZH_EZSP_ID_CLEAR_TRANSIENT_LINK_KEYS) != ZH_EZSP_OK) bad++;
    }
    CHECK(bad == 0, "nop / getEui64 / getNodeId / networkState / clearTransientLinkKeys × %d（%d 处不同）", ITER, bad);
}

static void t_network(void)
{
    uint8_t ours[256];
    int bad = 0;
    for (int i = 0; i < ITER; i++) {
        /* networkInit */
        sl_zigbee_network_init_struct_t ni = { .bitmask = r16() };
        q_status(ZH_EZSP_ID_NETWORK_INIT);
        sl_status_t s = sl_zigbee_ezsp_network_init(&ni);
        bad += status_case(ZH_EZSP_ID_NETWORK_INIT, s, ours, zh_ezsp_enc_network_init(ours, sizeof(ours), s_seq, ni.bitmask));
        /* startScan */
        const uint8_t st = r8(), du = r8();
        const uint32_t cm = r32();
        q_status(ZH_EZSP_ID_START_SCAN);
        s = sl_zigbee_ezsp_start_scan(st, cm, du);
        bad += status_case(ZH_EZSP_ID_START_SCAN, s, ours, zh_ezsp_enc_start_scan(ours, sizeof(ours), s_seq, st, cm, du));
        /* formNetwork */
        sl_zigbee_network_parameters_t np;
        zh_ezsp_net_params_t zp;
        rbytes(np.extendedPanId, 8);
        memcpy(zp.ext_pan_id, np.extendedPanId, 8);
        zp.pan_id = np.panId = r16();
        zp.tx_power = np.radioTxPower = (int8_t)r8();
        zp.channel = np.radioChannel = r8();
        zp.join_method = np.joinMethod = r8();
        zp.nwk_manager_id = np.nwkManagerId = r16();
        zp.nwk_update_id = np.nwkUpdateId = r8();
        zp.channels = np.channels = r32();
        q_status(ZH_EZSP_ID_FORM_NETWORK);
        s = sl_zigbee_ezsp_form_network(&np);
        bad += status_case(ZH_EZSP_ID_FORM_NETWORK, s, ours, zh_ezsp_enc_form_network(ours, sizeof(ours), s_seq, &zp));
        /* leaveNetwork / permitJoining / setRadioPower */
        const uint8_t lo = r8(), pj = (uint8_t)(r8() % 255);         /* 255 = 永久开放，我们拒编（另有真值表） */
        const int8_t pw = (int8_t)r8();
        q_status(ZH_EZSP_ID_LEAVE_NETWORK);
        s = sl_zigbee_ezsp_leave_network(lo);
        bad += status_case(ZH_EZSP_ID_LEAVE_NETWORK, s, ours, zh_ezsp_enc_leave_network(ours, sizeof(ours), s_seq, lo));
        q_status(ZH_EZSP_ID_PERMIT_JOINING);
        s = sl_zigbee_ezsp_permit_joining(pj);
        bad += status_case(ZH_EZSP_ID_PERMIT_JOINING, s, ours, zh_ezsp_enc_permit_joining(ours, sizeof(ours), s_seq, pj));
        q_status(ZH_EZSP_ID_SET_RADIO_POWER);
        s = sl_zigbee_ezsp_set_radio_power(pw);
        bad += status_case(ZH_EZSP_ID_SET_RADIO_POWER, s, ours, zh_ezsp_enc_set_radio_power(ours, sizeof(ours), s_seq, pw));
        /* setConcentrator */
        const bool on = r8() & 1;
        const uint16_t ct = r16(), mn = r16(), mx = r16();
        const uint8_t re = r8(), df = r8(), mh = r8();
        q_status(ZH_EZSP_ID_SET_CONCENTRATOR);
        s = sl_zigbee_ezsp_set_concentrator(on, ct, mn, mx, re, df, mh);
        bad += status_case(ZH_EZSP_ID_SET_CONCENTRATOR, s, ours, zh_ezsp_enc_set_concentrator(ours, sizeof(ours), s_seq, on, ct, mn, mx, re, df, mh));
        /* setSourceRouteDiscoveryMode：回包是 4 B 的剩余毫秒数，不是状态 */
        const uint8_t md = r8();
        uint8_t rem[4];
        rbytes(rem, 4);
        queue_rsp(ZH_EZSP_ID_SET_SOURCE_ROUTE_DISCOVERY_MODE, rem, 4);
        const uint32_t sr = sl_zigbee_ezsp_set_source_route_discovery_mode(md);
        if (!same_cmd(ours, zh_ezsp_enc_set_source_route_discovery_mode(ours, sizeof(ours), s_seq, md))) bad++;
        {
            zh_ezsp_frame_t fr;
            uint32_t zr = 0;
            if (!parse_keep(&fr) || zh_ezsp_dec_source_route_discovery_mode(&fr, &zr) != ZH_EZSP_OK || zr != sr) bad++;
        }
        /* getNetworkParameters：随机 25 B 回包 */
        uint8_t par[25];
        rbytes(par, 25);
        queue_rsp(ZH_EZSP_ID_GET_NETWORK_PARAMETERS, par, 25);
        sl_zigbee_node_type_t nt;
        sl_zigbee_network_parameters_t gp;
        s = sl_zigbee_ezsp_get_network_parameters(&nt, &gp);
        if (!same_cmd(ours, zh_ezsp_enc_get_network_parameters(ours, sizeof(ours), s_seq))) bad++;
        zh_ezsp_frame_t fr;
        uint32_t zs;
        uint8_t znt;
        zh_ezsp_net_params_t zg;
        if (!parse_keep(&fr) || zh_ezsp_dec_get_network_parameters(&fr, &zs, &znt, &zg) != ZH_EZSP_OK || zs != s || znt != nt ||
            memcmp(zg.ext_pan_id, gp.extendedPanId, 8) || zg.pan_id != gp.panId || zg.tx_power != gp.radioTxPower || zg.channel != gp.radioChannel ||
            zg.join_method != gp.joinMethod || zg.nwk_manager_id != gp.nwkManagerId || zg.nwk_update_id != gp.nwkUpdateId || zg.channels != gp.channels)
            bad++;
    }
    CHECK(bad == 0, "networkInit / startScan / formNetwork / leaveNetwork / permitJoining / setRadioPower / setConcentrator / setSourceRouteDiscoveryMode / getNetworkParameters × %d"
          "（%d 处不同）", ITER, bad);
}

static void t_messaging(void)
{
    uint8_t ours[256];
    int bad = 0;
    for (int i = 0; i < ITER; i++) {
        sl_zigbee_aps_frame_t a;
        zh_ezsp_aps_t za;
        rand_aps(&a, &za);
        const uint8_t type = r8();
        const uint16_t dst = r16(), tag = r16();
        const uint8_t L = (uint8_t)(i == 0 ? 0 : r8() % 199);         /* 5+1+2+11+2+1+L ≤ 220 */
        uint8_t msg[255];
        rbytes(msg, L);
        uint8_t par[5];
        rbytes(par, 5);
        queue_rsp(ZH_EZSP_ID_SEND_UNICAST, par, 5);
        uint8_t sq = 0;
        sl_status_t s = sl_zigbee_ezsp_send_unicast(type, dst, &a, tag, L, msg, &sq);
        if (!same_cmd(ours, zh_ezsp_enc_send_unicast(ours, sizeof(ours), s_seq, type, dst, &za, tag, L, msg))) bad++;
        zh_ezsp_frame_t fr;
        uint32_t zs;
        uint8_t zq;
        if (!parse_keep(&fr) || zh_ezsp_dec_send(&fr, ZH_EZSP_ID_SEND_UNICAST, &zs, &zq) != ZH_EZSP_OK || zs != s || zq != sq) bad++;

        const uint16_t alias = r16(), bdst = r16();
        const uint8_t nseq = r8(), radius = r8();
        const uint8_t BL = (uint8_t)(r8() % 196);                     /* 5+2+2+1+11+1+2+1+L ≤ 220 */
        rand_aps(&a, &za);
        rbytes(par, 5);
        queue_rsp(ZH_EZSP_ID_SEND_BROADCAST, par, 5);
        s = sl_zigbee_ezsp_send_broadcast(alias, bdst, nseq, &a, radius, tag, BL, msg, &sq);
        if (!same_cmd(ours, zh_ezsp_enc_send_broadcast(ours, sizeof(ours), s_seq, alias, bdst, nseq, &za, radius, tag, BL, msg))) bad++;
        if (!parse_keep(&fr) || zh_ezsp_dec_send(&fr, ZH_EZSP_ID_SEND_BROADCAST, &zs, &zq) != ZH_EZSP_OK || zs != s || zq != sq) bad++;

        /* lookup：两个方向 */
        const uint16_t nid = r16();
        uint8_t lp[12], se[8], ze[8];
        rbytes(lp, 12);
        queue_rsp(ZH_EZSP_ID_LOOKUP_EUI64_BY_NODE_ID, lp, 12);
        s = sl_zigbee_ezsp_lookup_eui64_by_node_id(nid, se);
        if (!same_cmd(ours, zh_ezsp_enc_lookup_eui64(ours, sizeof(ours), s_seq, nid))) bad++;
        if (!parse_keep(&fr) || zh_ezsp_dec_lookup_eui64(&fr, &zs, ze) != ZH_EZSP_OK || zs != s || memcmp(ze, se, 8)) bad++;

        uint8_t eui[8];
        rbytes(eui, 8);
        rbytes(lp, 6);
        queue_rsp(ZH_EZSP_ID_LOOKUP_NODE_ID_BY_EUI64, lp, 6);
        uint16_t sn = 0, zn = 0;
        s = sl_zigbee_ezsp_lookup_node_id_by_eui64(eui, &sn);
        if (!same_cmd(ours, zh_ezsp_enc_lookup_node_id(ours, sizeof(ours), s_seq, eui))) bad++;
        if (!parse_keep(&fr) || zh_ezsp_dec_lookup_node_id(&fr, &zs, &zn) != ZH_EZSP_OK || zs != s || zn != sn) bad++;

        /* 入网窗口的临时链路密钥（2026-10-23）：[EUI64 8][密钥 16] → u32 status */
        uint8_t te[8], tk[16], tp[4];
        rbytes(te, 8);
        rbytes(tk, 16);
        rbytes(tp, 4);
        queue_rsp(ZH_EZSP_ID_SEC_MAN_IMPORT_TRANSIENT_KEY, tp, 4);
        sl_zigbee_sec_man_key_t skey;
        memcpy(skey.key, tk, 16);
        s = sl_zigbee_ezsp_sec_man_import_transient_key(te, &skey);
        if (!same_cmd(ours, zh_ezsp_enc_import_transient_key(ours, sizeof(ours), s_seq, te, tk))) bad++;
        if (!parse_keep(&fr) || zh_ezsp_dec_status(&fr, ZH_EZSP_ID_SEC_MAN_IMPORT_TRANSIENT_KEY, &zs) != ZH_EZSP_OK || zs != s) bad++;

        /* 诊断（2026-10-23）：readCounters（42 个 u16、没有长度前缀）/ getChildData（status + 子设备一项） */
        uint8_t cp[2 * ZH_EZSP_COUNTER_N];
        rbytes(cp, sizeof(cp));
        queue_rsp(ZH_EZSP_ID_READ_COUNTERS, cp, sizeof(cp));
        uint16_t sv[ZH_EZSP_COUNTER_N], zv[ZH_EZSP_COUNTER_N];
        sl_zigbee_ezsp_read_counters(sv);
        if (!same_cmd(ours, zh_ezsp_enc_read_counters(ours, sizeof(ours), s_seq))) bad++;
        if (!parse_keep(&fr) || zh_ezsp_dec_read_counters(&fr, zv) != ZH_EZSP_OK || memcmp(sv, zv, sizeof(sv))) bad++;
        const uint8_t ci = (uint8_t)r16();
        uint8_t chp[4 + 8 + 1 + 2 + 1 + 1 + 1 + 4];
        rbytes(chp, sizeof(chp));
        queue_rsp(ZH_EZSP_ID_GET_CHILD_DATA, chp, sizeof(chp));
        sl_zigbee_child_data_t scd;
        zh_ezsp_child_t zcd;
        s = sl_zigbee_ezsp_get_child_data(ci, &scd);
        if (!same_cmd(ours, zh_ezsp_enc_get_child_data(ours, sizeof(ours), s_seq, ci))) bad++;
        if (!parse_keep(&fr) || zh_ezsp_dec_child_data(&fr, &zs, &zcd) != ZH_EZSP_OK || zs != s || memcmp(zcd.eui64, scd.eui64, 8) ||
            zcd.type != (uint8_t)scd.type || zcd.id != scd.id || zcd.phy != scd.phy || zcd.power != scd.power || zcd.timeout != scd.timeout ||
            zcd.remaining != scd.remainingTimeout) bad++;
    }
    CHECK(bad == 0, "sendUnicast / sendBroadcast / lookupEui64ByNodeId / lookupNodeIdByEui64 / importTransientKey / readCounters / getChildData × %d（%d 处不同）", ITER, bad);
}

static void t_security(void)
{
    uint8_t ours[256];
    int bad = 0;
    for (int i = 0; i < ITER; i++) {
        sl_zigbee_initial_security_state_t st;
        zh_ezsp_init_sec_t zs;
        zs.bitmask = st.bitmask = r16();
        rbytes(st.preconfiguredKey.contents, 16);
        rbytes(st.networkKey.contents, 16);
        rbytes(st.preconfiguredTrustCenterEui64, 8);
        memcpy(zs.preconfigured_key, st.preconfiguredKey.contents, 16);
        memcpy(zs.network_key, st.networkKey.contents, 16);
        memcpy(zs.tc_eui64, st.preconfiguredTrustCenterEui64, 8);
        zs.network_key_seq = st.networkKeySequenceNumber = r8();
        q_status(ZH_EZSP_ID_SET_INITIAL_SECURITY_STATE);
        sl_status_t s = sl_zigbee_ezsp_set_initial_security_state(&st);
        bad += status_case(ZH_EZSP_ID_SET_INITIAL_SECURITY_STATE, s, ours, zh_ezsp_enc_set_initial_security_state(ours, sizeof(ours), s_seq, &zs));

        uint8_t par[14];
        rbytes(par, 14);
        queue_rsp(ZH_EZSP_ID_GET_CURRENT_SECURITY_STATE, par, 14);
        sl_zigbee_current_security_state_t cs;
        s = sl_zigbee_ezsp_get_current_security_state(&cs);
        if (!same_cmd(ours, zh_ezsp_enc_get_current_security_state(ours, sizeof(ours), s_seq))) bad++;
        zh_ezsp_frame_t fr;
        uint32_t zst;
        uint16_t zb;
        uint8_t ze[8];
        if (!parse_keep(&fr) || zh_ezsp_dec_get_current_security_state(&fr, &zst, &zb, ze) != ZH_EZSP_OK || zst != s || zb != cs.bitmask ||
            memcmp(ze, cs.trustCenterLongAddress, 8))
            bad++;
    }
    CHECK(bad == 0, "setInitialSecurityState / getCurrentSecurityState × %d（%d 处不同）", ITER, bad);
}

/* ---------------------------------------------------------------- ③ 回调 */

/* 排一帧回调（fc 0x90：方向 = NCP、异步回调）并让 SDK 派发；返回我们解析好的帧 */
static bool fire(uint16_t id, const uint8_t *par, size_t n, zh_ezsp_frame_t *fr)
{
    memset(&s_cb, 0, sizeof(s_cb));
    s_in_cmd = false;                               /* tick 不在命令里：队列空时 SDK 拿到 NO_RX_DATA 就停 */
    queue(0x90, id, par, n);
    sl_zigbee_ezsp_tick();
    return s_cb.hit == 1 && s_cb.id == id && zh_ezsp_parse(s_keep, s_keep_n, fr) == ZH_EZSP_OK && zh_ezsp_is_callback(fr);
}

static void t_callbacks(void)
{
    int bad = 0, n = 0;
    for (int i = 0; i < ITER; i++) {
        uint8_t par[220];
        zh_ezsp_frame_t fr;

        rbytes(par, 4);
        uint32_t s32;
        if (!fire(ZH_EZSP_ID_STACK_STATUS_HANDLER, par, 4, &fr) || zh_ezsp_dec_stack_status(&fr, &s32) != ZH_EZSP_OK || s32 != s_cb.status) bad++;

        rbytes(par, 2);
        uint8_t ch;
        int8_t rs;
        if (!fire(ZH_EZSP_ID_ENERGY_SCAN_RESULT_HANDLER, par, 2, &fr) || zh_ezsp_dec_energy_scan_result(&fr, &ch, &rs) != ZH_EZSP_OK ||
            ch != s_cb.u8a || rs != s_cb.i8)
            bad++;

        rbytes(par, 5);
        if (!fire(ZH_EZSP_ID_SCAN_COMPLETE_HANDLER, par, 5, &fr) || zh_ezsp_dec_scan_complete(&fr, &ch, &s32) != ZH_EZSP_OK || ch != s_cb.u8a ||
            s32 != s_cb.status)
            bad++;

        rbytes(par, 13);
        par[1] = (uint8_t)(i % 3);                                    /* joining：0 / 1 / 2（非 0 都算真） */
        uint8_t idx, ct, eui[8];
        bool jn;
        uint16_t cid;
        if (!fire(ZH_EZSP_ID_CHILD_JOIN_HANDLER, par, 13, &fr) || zh_ezsp_dec_child_join(&fr, &idx, &jn, &cid, eui, &ct) != ZH_EZSP_OK ||
            idx != s_cb.u8a || (jn ? 1 : 0) != s_cb.u8b || cid != s_cb.u16a || memcmp(eui, s_cb.eui, 8) || ct != s_cb.u8c)
            bad++;

        rbytes(par, 14);
        uint16_t nid, pid;
        uint8_t st, dec;
        if (!fire(ZH_EZSP_ID_TRUST_CENTER_POST_JOIN_HANDLER, par, 14, &fr) || zh_ezsp_dec_tc_post_join(&fr, &nid, eui, &st, &dec, &pid) != ZH_EZSP_OK ||
            nid != s_cb.u16a || memcmp(eui, s_cb.eui, 8) || st != s_cb.u8a || dec != s_cb.u8b || pid != s_cb.u16b)
            bad++;

        uint8_t L = (uint8_t)(i == 0 ? 0 : r8() % 195);              /* 5 + 20 + 1 + L ≤ 220 */
        rbytes(par, 20);
        par[20] = L;
        rbytes(par + 21, L);
        uint8_t type, ml;
        uint16_t dst, tag;
        zh_ezsp_aps_t za;
        const uint8_t *m;
        if (!fire(ZH_EZSP_ID_MESSAGE_SENT_HANDLER, par, 21u + L, &fr) ||
            zh_ezsp_dec_message_sent(&fr, &s32, &type, &dst, &za, &tag, &ml, &m) != ZH_EZSP_OK || s32 != s_cb.status || type != s_cb.u8a ||
            dst != s_cb.u16a || !aps_eq(&s_cb.aps, &za) || tag != s_cb.u16b || ml != s_cb.len || memcmp(m, s_cb.data, ml))
            bad++;

        L = (uint8_t)(i == 0 ? 0 : r8() % 184);                       /* 5 + 30 + 1 + L + 1 ≤ 220 */
        rbytes(par, 30);
        par[30] = L;
        rbytes(par + 31, L + 1u);                                     /* 末尾一个字节是源路由开销（真 NCP 每条都带，见 zh_ezsp_core.h） */
        zh_ezsp_rx_info_t ri;
        uint8_t ov = 0;
        if (!fire(ZH_EZSP_ID_INCOMING_MESSAGE_HANDLER, par, 31u + L + 1u, &fr) ||
            zh_ezsp_dec_incoming_message(&fr, &type, &za, &ri, &ml, &m, &ov) != ZH_EZSP_OK || ov != s_cb.u8b || type != s_cb.u8a || !aps_eq(&s_cb.aps, &za) ||
            ri.sender_short != s_cb.ri.sender_short_id || memcmp(ri.sender_long, s_cb.ri.sender_long_id, 8) || ri.binding_index != s_cb.ri.binding_index ||
            ri.address_index != s_cb.ri.address_index || ri.last_hop_lqi != s_cb.ri.last_hop_lqi || ri.last_hop_rssi != s_cb.ri.last_hop_rssi ||
            ri.last_hop_timestamp != s_cb.ri.last_hop_timestamp || ml != s_cb.len || memcmp(m, s_cb.data, ml))
            bad++;

        const uint8_t rc = (uint8_t)(i == 0 ? 0 : r8() % 101);       /* 5 + 13 + 2×rc ≤ 220 */
        rbytes(par, 12);
        par[12] = rc;
        rbytes(par + 13, (size_t)rc * 2);
        uint16_t src;
        uint8_t lqi, cnt;
        const uint8_t *rl;
        if (!fire(ZH_EZSP_ID_INCOMING_ROUTE_RECORD_HANDLER, par, 13u + rc * 2u, &fr) ||
            zh_ezsp_dec_incoming_route_record(&fr, &src, eui, &lqi, &rs, &cnt, &rl) != ZH_EZSP_OK || src != s_cb.u16a || memcmp(eui, s_cb.eui, 8) ||
            lqi != s_cb.u8a || rs != s_cb.i8 || cnt != s_cb.len || memcmp(rl, s_cb.data, (size_t)cnt * 2))
            bad++;

        rbytes(par, 2);
        uint16_t idc;
        if (!fire(ZH_EZSP_ID_ID_CONFLICT_HANDLER, par, 2, &fr) || zh_ezsp_dec_id_conflict(&fr, &idc) != ZH_EZSP_OK || idc != s_cb.u16a) bad++;

        L = (uint8_t)(i == 0 ? 0 : r8() % 215);                       /* 5 + 1 + L ≤ 220 */
        par[0] = L;
        rbytes(par + 1, L);
        uint8_t pl;
        const uint8_t *pp;
        if (!fire(ZH_EZSP_ID_CUSTOM_FRAME_HANDLER, par, 1u + L, &fr) || zh_ezsp_dec_custom_frame_handler(&fr, &pl, &pp) != ZH_EZSP_OK ||
            pl != s_cb.len || memcmp(pp, s_cb.data, pl))
            bad++;
        n += 10;
    }
    CHECK(bad == 0, "10 种回调 × %d 经 sl_zigbee_ezsp_tick 派发：参数与 SDK 逐字段相同（%d/%d 处不同）", ITER, bad, n);
}

/* callback 命令（NCP 用同步回调时取回调）：应答是 noCallbacks，或者那条回调本身（fc 带 0x08） */
static void t_callback_cmd(void)
{
    uint8_t ours[256];
    int bad = 0;
    for (int i = 0; i < ITER; i++) {
        zh_ezsp_frame_t fr;
        memset(&s_cb, 0, sizeof(s_cb));
        queue(0x88, ZH_EZSP_ID_NO_CALLBACKS, NULL, 0);
        sl_zigbee_ezsp_callback();
        if (!same_cmd(ours, zh_ezsp_enc_callback(ours, sizeof(ours), s_seq))) bad++;
        if (s_cb.hit != 1 || s_cb.id != ZH_EZSP_ID_NO_CALLBACKS || !parse_keep(&fr) || zh_ezsp_dec_empty(&fr, ZH_EZSP_ID_NO_CALLBACKS) != ZH_EZSP_OK) bad++;

        uint8_t par[4];
        rbytes(par, 4);
        memset(&s_cb, 0, sizeof(s_cb));
        queue(0x88, ZH_EZSP_ID_STACK_STATUS_HANDLER, par, 4);
        sl_zigbee_ezsp_callback();
        if (!same_cmd(ours, zh_ezsp_enc_callback(ours, sizeof(ours), s_seq))) bad++;
        uint32_t st;
        if (s_cb.hit != 1 || s_cb.id != ZH_EZSP_ID_STACK_STATUS_HANDLER || !parse_keep(&fr) || !zh_ezsp_is_callback(&fr) ||
            zh_ezsp_dec_stack_status(&fr, &st) != ZH_EZSP_OK || st != s_cb.status)
            bad++;
    }
    CHECK(bad == 0, "callback × %d：编码相同；应答 noCallbacks / 同步回调两边都派发到同一处（%d 处不同）", ITER, bad);
}

/* ---------------------------------------------------------------- ④ 头的分类 */

typedef enum { C_OK, C_RESERVED, C_INVALID, C_DIRECTION, C_TRUNCATED } cls_t;

static cls_t from_ours(int e)
{
    switch (e) {
    case ZH_EZSP_OK: return C_OK;
    case ZH_EZSP_E_RESERVED: return C_RESERVED;
    case ZH_EZSP_E_INVALID_CMD: return C_INVALID;
    case ZH_EZSP_E_DIRECTION: return C_DIRECTION;
    case ZH_EZSP_E_TRUNCATED: return C_TRUNCATED;
    default: return (cls_t)-1;
    }
}

static void t_classify(void)
{
    /* 扩展头的 fc 高字节：格式 1 + 各保留位 / 填充 / 加密；旧式头：第 2 字节是帧 ID（格式位 ≠ 1 的才是旧式） */
    static const uint8_t k_fch[] = { 0x01, 0x05, 0x09, 0x11, 0x21, 0x3D, 0x41, 0x81, 0xC1 };
    static const uint8_t k_legacy_id[] = { 0x00, 0x58, 0x02, 0x03 };
    /* 0 = SUCCESS、0x31 INVALID_FRAME_ID（正常的原因码）、0x34 = OVERFLOW 的值、0x27 = NO_RX_DATA 的值、0x19 = SPI_WAITING_FOR_RESPONSE 的值 */
    static const uint8_t k_reason[] = { 0x00, 0x31, 0x34, 0x27, 0x19 };
    int cases = 0, same = 0, dev_pred = 0, dev_seen = 0, unexpected = 0, code_bad = 0, reason_bad = 0;
    int wait_pred = 0, wait_seen = 0, wait_unexpected = 0;
    for (int legacy = 0; legacy < 2; legacy++) {
        const int nb = legacy ? (int)sizeof(k_legacy_id) : (int)sizeof(k_fch) * 2;
        for (int b = 0; b < nb; b++) {
            for (int fc = 0; fc < 256; fc++) {
                for (int ri = 0; ri < (int)sizeof(k_reason); ri++) {
                    uint8_t f[8];
                    size_t n;
                    f[0] = s_seq;
                    f[1] = (uint8_t)fc;
                    if (legacy) {
                        f[2] = k_legacy_id[b];
                        f[3] = k_reason[ri];
                        n = 4;
                    } else {
                        f[2] = k_fch[b / 2];
                        const uint16_t id = (b & 1) ? ZH_EZSP_ID_INVALID_COMMAND : ZH_EZSP_ID_NOP;
                        f[3] = (uint8_t)id;
                        f[4] = (uint8_t)(id >> 8);
                        f[5] = k_reason[ri];
                        n = 6;
                    }
                    /* SDK：拿 nop 当载体，看 error_handler 收到什么（OVERFLOW 收到了也算"收"——ezsp.c:610） */
                    memcpy(s_rsp, f, n);
                    s_rsp_n = n;
                    s_nerr = 0;
                    const int sent0 = s_sentinels;
                    sl_zigbee_ezsp_nop();
                    s_seq++;
                    const bool sdk_waited = s_sentinels != sent0;      /* SDK 没把这帧当应答，又要了一帧（哨兵） */
                    const int serr = s_nerr ? s_err[0] : SL_ZIGBEE_EZSP_SUCCESS;
                    const bool sdk_accept = s_nerr == 0 || serr == SL_ZIGBEE_EZSP_ERROR_OVERFLOW;
                    /* 我们 */
                    zh_ezsp_frame_t fr;
                    const int e = zh_ezsp_parse(f, n, &fr);
                    const cls_t oc = from_ours(e);
                    const bool our_accept = oc == C_OK;
                    /* 这一格有哪些错（与先后无关） */
                    const uint16_t id = legacy ? f[2] : (uint16_t)(f[3] | (f[4] << 8));
                    const bool reserved = !legacy && (f[2] & ZH_EZSP_FCH_RESERVED_MASK);
                    const bool inv = id == ZH_EZSP_ID_INVALID_COMMAND, dir = !(fc & 0x80), trunc = (fc & 0x02) != 0, ovf = (fc & 0x01) != 0;
                    /* 预言的差异：照 ezsp.c:569-585 的"后写的覆盖先写的"算出 SDK 的最终状态，凡是有错却落到 SUCCESS / OVERFLOW 的格子，
                       SDK 收、我们拒（溢出洗白错误；invalidCommand 的原因码恰好是 0 或 0x34） */
                    int sim = SL_ZIGBEE_EZSP_SUCCESS;
                    if (inv) sim = k_reason[ri];
                    if (dir) sim = SL_ZIGBEE_EZSP_ERROR_WRONG_DIRECTION;
                    if (trunc) sim = SL_ZIGBEE_EZSP_ERROR_TRUNCATED;
                    if (ovf) sim = SL_ZIGBEE_EZSP_ERROR_OVERFLOW;
                    const bool any_err = inv || dir || trunc;
                    const bool predicted = !reserved && any_err && (sim == SL_ZIGBEE_EZSP_SUCCESS || sim == SL_ZIGBEE_EZSP_ERROR_OVERFLOW);
                    /* 最终状态恰好是 NO_RX_DATA：sendCommand 的等待循环（ezsp.c:647-651）把它当"还没收到"，接着等下一帧 */
                    const bool wait = !reserved && sim == SL_ZIGBEE_EZSP_NO_RX_DATA;
                    if (wait) wait_pred++;
                    if (sdk_waited && wait) wait_seen++;
                    if (sdk_waited != wait) wait_unexpected++;
                    cases++;
                    if (predicted) {
                        dev_pred++;
                        if (sdk_accept && !our_accept) dev_seen++;
                        else unexpected++;
                        continue;
                    }
                    if (sdk_accept != our_accept) {
                        unexpected++;
                        if (unexpected < 5) printf("   格子 legacy=%d f=%02X %02X %02X n=%zu：SDK %s(%d) / 我们 %d\n", legacy, f[1], f[2], f[3], n,
                                                   sdk_accept ? "收" : "拒", serr, e);
                        continue;
                    }
                    same++;
                    /* 只有一种错时，错误种类也要对得上 */
                    if (!our_accept && (int)reserved + (int)inv + (int)dir + (int)trunc == 1) {
                        cls_t sc = serr == SL_ZIGBEE_EZSP_ERROR_UNSUPPORTED_CONTROL ? C_RESERVED
                                 : serr == SL_ZIGBEE_EZSP_ERROR_WRONG_DIRECTION   ? C_DIRECTION
                                 : serr == SL_ZIGBEE_EZSP_ERROR_TRUNCATED         ? C_TRUNCATED
                                                                                  : C_INVALID;
                        if (sc != oc) code_bad++;
                        if (sc == C_INVALID && fr.invalid_reason != serr) reason_bad++;
                    }
                }
            }
        }
    }
    s_nerr = 0;
    CHECK(unexpected == 0, "头的分类 %d 格：除预言的差异外，SDK 收 ⇔ 我们收（%d 格对不上）", cases, unexpected);
    CHECK(dev_pred > 0 && dev_seen == dev_pred, "预言的 %d 格差异全部是\"SDK 收、我们拒\"（溢出不洗白错误；原因码 0 / 0x34 不当成功）", dev_pred);
    CHECK(code_bad == 0 && reason_bad == 0, "只有一种错的格子：错误种类与 invalidCommand 原因码都与 SDK 相同（种类 %d / 原因码 %d 处不同）", code_bad, reason_bad);
    CHECK(same + dev_pred == cases, "每一格都被归类（%d + %d = %d）", same, dev_pred, cases);
    CHECK(wait_pred > 0 && wait_seen == wait_pred && wait_unexpected == 0,
          "原因码 0x27（= NO_RX_DATA）的 %d 格 SDK 都把下一帧当应答、别的格子都没有（%d 格对不上）；我们这些格子都拒", wait_pred, wait_unexpected);
    s_sentinels = 0;
}

/* 一段测试跑完：SDK 的 error_handler 一次没被叫过、也没有多等一帧（有意构造错误的段落不走这里） */
static void clean(const char *name, void (*fn)(void))
{
    s_nerr = 0;
    s_sentinels = 0;
    fn();
    CHECK(s_nerr == 0 && s_sentinels == 0, "%s：SDK 的 error_handler 一次没叫、也没多等一帧（%d / %d）", name, s_nerr, s_sentinels);
}

int main(void)
{
    clean("version", t_version);                /* 必须第一个：SDK 只有第一条 version 用旧式头 */
    clean("config / policy / value", t_config_policy_value);
    t_too_long();                               /* 有意超长：自己逐条核对 error_handler */
    clean("endpoint / customFrame / 杂项", t_endpoint_custom_misc);
    clean("网络", t_network);
    clean("收发", t_messaging);
    clean("安全", t_security);
    clean("回调", t_callbacks);
    clean("callback 命令", t_callback_cmd);
    t_classify();                               /* 有意构造坏头：自己逐格核对 */
    printf("ezsp_oracle：%d 通过，%d 失败\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
