/* muse_frame —— Muse 会话的 WebSocket 与控制流分帧（见 muse_frame.h）。纯 C，不依赖 ESP-IDF。 */
#include "muse_frame.h"

#include <string.h>

/* ---------------- WebSocket ---------------- */

size_t mws_encode_header(uint8_t hdr[MWS_HDR_MAX], uint8_t opcode, uint64_t payload_len, const uint8_t mask[4])
{
    if (!hdr || !mask) return 0;
    if (opcode != MWS_OP_BINARY && opcode != MWS_OP_CLOSE && opcode != MWS_OP_PING && opcode != MWS_OP_PONG) return 0;
    if (opcode >= MWS_OP_CLOSE && payload_len > MWS_CTRL_MAX) return 0;
    if (payload_len >> 63) return 0;
    size_t n = 0;
    hdr[n++] = (uint8_t)(0x80u | opcode);                       /* FIN */
    if (payload_len < 126) {
        hdr[n++] = (uint8_t)(0x80u | payload_len);              /* MASK */
    } else if (payload_len <= 0xFFFFu) {
        hdr[n++] = 0x80u | 126u;
        hdr[n++] = (uint8_t)(payload_len >> 8);
        hdr[n++] = (uint8_t)payload_len;
    } else {
        hdr[n++] = 0x80u | 127u;
        for (int i = 7; i >= 0; i--) hdr[n++] = (uint8_t)(payload_len >> (8 * i));
    }
    memcpy(hdr + n, mask, 4);
    return n + 4;
}

void mws_mask(uint8_t *payload, size_t len, const uint8_t mask[4])
{
    if (!payload || !mask) return;
    for (size_t i = 0; i < len; i++) payload[i] ^= mask[i & 3];
}

void mws_rx_init(mws_rx_t *rx)
{
    if (!rx) return;
    memset(rx, 0, sizeof(*rx));
    rx->hdr_need = 2;
}

static mws_ev_t die(mws_rx_t *rx, mws_ev_t ev)
{
    rx->dead_ev = (uint8_t)ev;
    return ev;
}

static void next_frame(mws_rx_t *rx)
{
    rx->hdr_have = 0;
    rx->hdr_need = 2;
    rx->in_payload = false;
    rx->got = 0;
    rx->len = 0;
}

/* 帧头凑齐了：解析长度与掩码。返回 MWS_NEED_MORE 表示进入载荷阶段，其余是错误。 */
static mws_ev_t parse_header(mws_rx_t *rx, size_t cap)
{
    const uint8_t l7 = rx->hdr[1] & 0x7Fu;
    size_t o = 2;
    uint64_t len;
    if (l7 < 126) {
        len = l7;
    } else if (l7 == 126) {
        len = ((uint64_t)rx->hdr[2] << 8) | rx->hdr[3];
        o = 4;
    } else {
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | rx->hdr[2 + i];
        o = 10;
        if (len >> 63) return die(rx, MWS_ERR_PROTOCOL);       /* RFC 6455 5.2：最高位必须为 0 */
    }
    if (rx->masked) memcpy(rx->mask, rx->hdr + o, 4);
    if (len > cap) return die(rx, MWS_ERR_TOO_LARGE);
    rx->len = len;
    rx->got = 0;
    rx->in_payload = true;
    return MWS_NEED_MORE;
}

mws_ev_t mws_feed(mws_rx_t *rx, const uint8_t *in, size_t n, size_t *consumed, uint8_t *buf, size_t cap, size_t *frame_len)
{
    if (consumed) *consumed = 0;
    if (frame_len) *frame_len = 0;
    if (!rx || !consumed || (n && !in) || (cap && !buf)) return MWS_ERR_PROTOCOL;
    if (rx->dead_ev) return (mws_ev_t)rx->dead_ev;
    size_t i = 0;
    while (i < n || (rx->in_payload && rx->got == rx->len)) {
        if (!rx->in_payload) {
            rx->hdr[rx->hdr_have++] = in[i++];
            if (rx->hdr_have == 2) {
                const uint8_t b0 = rx->hdr[0], b1 = rx->hdr[1];
                const uint8_t op = b0 & 0x0Fu;
                if ((b0 & 0x70u) != 0) return *consumed = i, die(rx, MWS_ERR_PROTOCOL);       /* 保留位 */
                if (!(b0 & 0x80u)) return *consumed = i, die(rx, MWS_ERR_PROTOCOL);           /* 分片：不支持 */
                if (op != MWS_OP_BINARY && op != MWS_OP_CLOSE && op != MWS_OP_PING && op != MWS_OP_PONG) {
                    return *consumed = i, die(rx, MWS_ERR_PROTOCOL);                         /* 文本、续帧、未知 */
                }
                if (op >= MWS_OP_CLOSE && (b1 & 0x7Fu) > MWS_CTRL_MAX) return *consumed = i, die(rx, MWS_ERR_PROTOCOL);
                rx->opcode = op;
                rx->masked = (b1 & 0x80u) != 0;
                const uint8_t l7 = b1 & 0x7Fu;
                rx->hdr_need = (uint8_t)(2 + (l7 == 126 ? 2 : l7 == 127 ? 8 : 0) + (rx->masked ? 4 : 0));
            }
            if (rx->hdr_have < 2 || rx->hdr_have < rx->hdr_need) continue;
            const mws_ev_t e = parse_header(rx, cap);
            if (e != MWS_NEED_MORE) return *consumed = i, e;
            if (rx->len > 0) continue;                                       /* 有载荷：接着读 */
        } else if (rx->got < rx->len) {
            size_t take = n - i;
            if ((uint64_t)take > rx->len - rx->got) take = (size_t)(rx->len - rx->got);
            memcpy(buf + rx->got, in + i, take);
            if (rx->masked) {
                for (size_t k = 0; k < take; k++) buf[rx->got + k] ^= rx->mask[(rx->got + k) & 3];
            }
            rx->got += take;
            i += take;
            if (rx->got < rx->len) continue;
        }
        /* 一帧凑齐 */
        const uint8_t op = rx->opcode;
        const size_t len = (size_t)rx->len;
        next_frame(rx);
        *consumed = i;
        if (frame_len) *frame_len = len;
        switch (op) {
        case MWS_OP_BINARY: return MWS_BINARY;
        case MWS_OP_PING:   return MWS_PING;
        case MWS_OP_PONG:   return MWS_PONG;
        default:            return die(rx, MWS_CLOSE);
        }
    }
    *consumed = i;
    return MWS_NEED_MORE;
}

/* ---------------- 控制流消息 ---------------- */

void mctl_rx_init(mctl_rx_t *rx)
{
    if (rx) memset(rx, 0, sizeof(*rx));
}

mctl_ev_t mctl_feed(mctl_rx_t *rx, const uint8_t *in, size_t n, size_t *consumed, uint8_t *buf, size_t cap, size_t *msg_len)
{
    if (consumed) *consumed = 0;
    if (msg_len) *msg_len = 0;
    if (!rx || !consumed || (n && !in) || (cap && !buf)) return MCTL_NEED_MORE;
    size_t i = 0;
    while (i < n) {
        if (rx->pfx_have < 4) {
            rx->pfx[rx->pfx_have++] = in[i++];
            if (rx->pfx_have == 4) {
                rx->len = (uint32_t)rx->pfx[0] | ((uint32_t)rx->pfx[1] << 8) | ((uint32_t)rx->pfx[2] << 16)
                          | ((uint32_t)rx->pfx[3] << 24);
                rx->got = 0;
                rx->skipping = rx->len > cap;
                if (rx->len == 0) rx->pfx_have = 0;                          /* 空消息：吃掉，不报（同上游） */
            }
            continue;
        }
        size_t take = n - i;
        if (take > rx->len - rx->got) take = rx->len - rx->got;
        if (!rx->skipping) memcpy(buf + rx->got, in + i, take);
        rx->got += (uint32_t)take;
        i += take;
        if (rx->got == rx->len) {
            const mctl_ev_t ev = rx->skipping ? MCTL_SKIPPED : MCTL_MESSAGE;
            if (msg_len) *msg_len = rx->len;
            rx->pfx_have = 0;
            rx->skipping = false;
            *consumed = i;
            return ev;
        }
    }
    *consumed = i;
    return MCTL_NEED_MORE;
}

void mctl_prefix(uint8_t out[4], uint32_t len)
{
    if (!out) return;
    out[0] = (uint8_t)len;
    out[1] = (uint8_t)(len >> 8);
    out[2] = (uint8_t)(len >> 16);
    out[3] = (uint8_t)(len >> 24);
}
