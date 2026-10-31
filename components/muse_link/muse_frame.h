#pragma once
/* muse_frame —— Muse 会话的两层分帧（docs/15_muse_direct_design_v1.md 4.6；M3 muse_session 用）。纯 C，不依赖 ESP-IDF：
 * test_host/ 原样编它，模糊测试。
 *
 * 1. WebSocket（RFC 6455，客户端）：上游 esp32/main/noise_control.cpp（e6fe517）:296-650 的 ws_send_binary / ws_recv_frame
 *    是逐字节阻塞读；这里换成增量解析器 —— 会话任务把 esp_tls 读到的字节随到随喂，一帧凑齐才交出来，读不满不阻塞。
 *    规则照上游：只收二进制帧（Noise 全是二进制）；ping 交给调用方回 pong（回显载荷）；pong 吃掉；close 即结束；
 *    控制帧载荷 ≤ 125（:509-516）；服务端帧带掩码也照解（:494-499）；不支持分片（上游注释：入口代理不分片）——
 *    分片和文本帧一律当协议错误，不静默拼接。
 * 2. 控制流（POST /link-control，stream 1）里的消息：每条是 4 字节小端长度 + JSON（上游 :1905-1975）。上游假定一个 body chunk
 *    里都是整条消息、跨 chunk 的直接丢；这里跨 chunk 重组，单条超过缓冲的整条跳过（报给调用方，它只记日志）。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {   /* muse_session.cpp 调它 */
#endif

/* ---------------- WebSocket ---------------- */

#define MWS_OP_CONT    0x0
#define MWS_OP_TEXT    0x1
#define MWS_OP_BINARY  0x2
#define MWS_OP_CLOSE   0x8
#define MWS_OP_PING    0x9
#define MWS_OP_PONG    0xA
#define MWS_HDR_MAX    14            /* 2 + 8 字节长度 + 4 字节掩码 */
#define MWS_CTRL_MAX   125

/* 客户端帧头（FIN 置位、带掩码）。返回头长度（6 / 8 / 14）；opcode 非法返回 0。 */
size_t mws_encode_header(uint8_t hdr[MWS_HDR_MAX], uint8_t opcode, uint64_t payload_len, const uint8_t mask[4]);
/* 原地掩码（同上游 ws_mask_inplace 的语义，逐字节写法；会话里用上游的字对齐版 ws_mask.h） */
void mws_mask(uint8_t *payload, size_t len, const uint8_t mask[4]);

typedef enum {
    MWS_NEED_MORE = 0,   /* 输入吃完了，帧还没凑齐 */
    MWS_BINARY,          /* 一个二进制帧凑齐：载荷在 buf[0 .. *frame_len) */
    MWS_PING,            /* ping：载荷（≤ 125）在 buf 里，调用方回 pong 回显它 */
    MWS_PONG,            /* pong：没有要做的，只说明对端活着 */
    MWS_CLOSE,           /* 对端关闭 */
    MWS_ERR_TOO_LARGE,   /* 载荷超过 buf 容量（会话必须断：Noise 的 nonce 会错位） */
    MWS_ERR_PROTOCOL,    /* 分片、文本帧、保留位、未知 opcode、控制帧过长、64 位长度最高位为 1 */
} mws_ev_t;

typedef struct {
    uint8_t  hdr[MWS_HDR_MAX];
    uint8_t  hdr_have, hdr_need;
    bool     in_payload;
    uint8_t  opcode;
    bool     masked;
    uint8_t  mask[4];
    uint64_t len;
    uint64_t got;
    uint8_t  dead_ev;        /* 报过错误或 close 之后记着那个事件（mws_ev_t），不再解析；0 = 还活着 */
} mws_rx_t;

void mws_rx_init(mws_rx_t *rx);
/* 喂 in[0..n)。返回事件；*consumed = 吃掉的字节数（可能小于 n：一帧凑齐就停，剩下的下次再喂）。
   报过 ERR / CLOSE 之后一律返回同一个错误、不再吃字节。 */
mws_ev_t mws_feed(mws_rx_t *rx, const uint8_t *in, size_t n, size_t *consumed, uint8_t *buf, size_t cap, size_t *frame_len);

/* ---------------- 控制流消息 ---------------- */

typedef enum {
    MCTL_NEED_MORE = 0,  /* 输入吃完了 */
    MCTL_MESSAGE,        /* 一条消息凑齐：buf[0 .. *msg_len)（不含长度前缀；不以 NUL 结尾） */
    MCTL_SKIPPED,        /* 一条超过缓冲的消息整条跳过了（*msg_len = 它声明的长度） */
} mctl_ev_t;

typedef struct {
    uint8_t  pfx[4];
    uint8_t  pfx_have;
    uint32_t len;
    uint32_t got;
    bool     skipping;
} mctl_rx_t;

void mctl_rx_init(mctl_rx_t *rx);
/* 同 mws_feed 的用法。长度为 0 的消息直接吃掉、不报（上游同样跳过）。 */
mctl_ev_t mctl_feed(mctl_rx_t *rx, const uint8_t *in, size_t n, size_t *consumed, uint8_t *buf, size_t cap, size_t *msg_len);
/* 出站消息的 4 字节小端长度前缀 */
void mctl_prefix(uint8_t out[4], uint32_t len);

#ifdef __cplusplus
}
#endif
