/* zh_ash_core —— ASH 主机侧（见 include/zh_ash_core.h，那里有 H1 的出处）。纯 C，不依赖 ESP-IDF，不分配内存。 */
#include "zh_ash_core.h"

#include <string.h>

/* flags */
#define F_RST   0x0001u   /* 要发 RST */
#define F_CAN   0x0002u   /* RST 前要发 CAN */
#define F_ACK   0x0004u   /* 要发 ACK */
#define F_NAK   0x0008u   /* 要发 NAK */
#define F_REJ   0x0010u   /* 处在"已回过 NAK"的拒收状态：乱序帧不再重复 NAK（ash-host.c:764-770） */
#define F_RETX  0x0020u   /* 正在重传 */

#define MOD8(n) ((uint8_t)((n) & 7u))

/* lo..hi（模 8，闭区间）里有没有 n（ash-protocol.h:126-129 WITHIN_RANGE） */
static bool within_range(uint8_t lo, uint8_t n, uint8_t hi)
{
    return MOD8(n - lo) <= MOD8(hi - lo);
}

/* ---------------------------------------------------------------- 编解码工具 */

/* CRC-CCITT：多项式 0x1021、初值 0xFFFF、不反射（legacy_hal crc.c 的 halCommonCrc16 逐字节等价；这里按位算，与 SDK 的移位写法独立） */
uint16_t zh_ash_crc16(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; buf && i < len; i++) {
        crc ^= (uint16_t)((uint16_t)buf[i] << 8);
        for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

void zh_ash_randomize(uint8_t *buf, size_t len)
{
    uint8_t r = ZH_ASH_LFSR_SEED;
    for (size_t i = 0; buf && i < len; i++) {
        buf[i] ^= r;
        r = (r & 1) ? (uint8_t)((r >> 1) ^ ZH_ASH_LFSR_POLY) : (uint8_t)(r >> 1);
    }
}

static bool reserved(uint8_t b)
{
    return b == ZH_ASH_FLAG || b == ZH_ASH_ESC || b == ZH_ASH_XON || b == ZH_ASH_XOFF || b == ZH_ASH_SUB || b == ZH_ASH_CAN;
}

static size_t put_stuffed(uint8_t b, uint8_t *out, size_t n)
{
    if (reserved(b)) {
        out[n++] = ZH_ASH_ESC;
        out[n++] = (uint8_t)(b ^ ZH_ASH_FLIP);
    } else {
        out[n++] = b;
    }
    return n;
}

size_t zh_ash_encode(uint8_t ctrl, const uint8_t *data, size_t len, uint8_t *out, size_t cap)
{
    if (!out || len > ZH_ASH_DATA_MAX || (len && !data)) return 0;
    if (cap < 2 * (1 + len + 2) + 1) return 0;          /* 最坏每字节都转义 + FLAG：不够就不写半帧 */
    uint8_t raw[ZH_ASH_RAW_MAX];
    raw[0] = ctrl;
    if (len) memcpy(raw + 1, data, len);
    if (ZH_ASH_IS_DATA(ctrl)) zh_ash_randomize(raw + 1, len);   /* 只有 DATA 帧随机化；CRC 覆盖随机化之后的字节 */
    const uint16_t crc = zh_ash_crc16(raw, 1 + len);
    raw[1 + len] = (uint8_t)(crc >> 8);                /* 高字节先（ash-common.c:98-101） */
    raw[2 + len] = (uint8_t)(crc & 0xFF);
    size_t n = 0;
    for (size_t i = 0; i < 3 + len; i++) n = put_stuffed(raw[i], out, n);
    out[n++] = ZH_ASH_FLAG;
    return n;
}

/* ---------------------------------------------------------------- 定时器 */

static void timer_start(zh_ash_t *a, uint32_t now)
{
    a->ack_running = true;
    a->ack_start_ms = now;
}

static void timer_stop(zh_ash_t *a) { a->ack_running = false; }

static bool timer_expired(const zh_ash_t *a, uint32_t now)
{
    return a->ack_running && (uint32_t)(now - a->ack_start_ms) >= a->ack_period;
}

/* ash-common.c:233-257 ashAdjustAckPeriod：超时翻倍；收到确认按 (7×旧 + 4×往返)/8 平滑；夹在上下限；总是停表 */
static void timer_adjust(zh_ash_t *a, bool expired, uint32_t now)
{
    uint32_t period = a->ack_period;
    if (expired) {
        period = (uint32_t)a->ack_period * 2;
    } else if (a->ack_running) {
        const uint32_t rtt = (uint32_t)(now - a->ack_start_ms);
        period = (period * 7 + rtt * 4) / 8;
    }
    if (period > a->cfg.ack_max_ms) period = a->cfg.ack_max_ms;
    if (period < a->cfg.ack_min_ms) period = a->cfg.ack_min_ms;
    a->ack_period = (uint16_t)period;
    a->ack_running = false;
}

/* ---------------------------------------------------------------- 初始化 / 复位 */

void zh_ash_cfg_default(zh_ash_cfg_t *c)
{
    if (!c) return;
    c->ack_init_ms = 800;                              /* ash-host.c:86-89 */
    c->ack_min_ms = 400;
    c->ack_max_ms = 2400;
    c->rst_timeout_ms = 5000;
    c->max_timeouts = 6;                               /* ash-host.h:31 */
    c->tx_k = 1;                                       /* 设计 8.3：停等 */
}

static void sanitize_cfg(zh_ash_cfg_t *c)
{
    if (c->tx_k < 1) c->tx_k = 1;
    if (c->tx_k > ZH_ASH_TXQ - 1) c->tx_k = ZH_ASH_TXQ - 1;
    if (c->ack_min_ms < 1) c->ack_min_ms = 1;
    if (c->ack_max_ms < c->ack_min_ms) c->ack_max_ms = c->ack_min_ms;
    if (c->ack_init_ms < c->ack_min_ms) c->ack_init_ms = c->ack_min_ms;
    if (c->ack_init_ms > c->ack_max_ms) c->ack_init_ms = c->ack_max_ms;
    if (c->max_timeouts < 1) c->max_timeouts = 1;
    if (c->rst_timeout_ms < 1) c->rst_timeout_ms = 1;
}

void zh_ash_init(zh_ash_t *a, const zh_ash_cfg_t *cfg, zh_ash_frame_cb on_frame, zh_ash_event_cb on_event, void *ctx)
{
    if (!a) return;
    memset(a, 0, sizeof(*a));
    if (cfg) a->cfg = *cfg;
    else zh_ash_cfg_default(&a->cfg);
    sanitize_cfg(&a->cfg);
    a->on_frame = on_frame;
    a->on_event = on_event;
    a->ctx = ctx;
    a->ack_period = a->cfg.ack_init_ms;
    a->state = ZH_ASH_DOWN;
}

static void link_vars_clear(zh_ash_t *a)
{
    a->flags = 0;
    a->frm_tx = a->ack_rx = a->frm_retx = a->frm_rx = a->ack_tx = a->frm_head = 0;
    a->timeouts = 0;
    a->q_start = a->q_len = 0;
    timer_stop(a);
    a->rx_len = 0;
    a->rx_esc = a->rx_sub = false;
    a->rx_last_ctrl = 0;
}

void zh_ash_reset(zh_ash_t *a)
{
    if (!a) return;
    link_vars_clear(a);
    a->flags = F_RST | F_CAN;
    a->state = ZH_ASH_RESETTING;
    a->st.resets++;
}

void zh_ash_stop(zh_ash_t *a)
{
    if (!a) return;
    link_vars_clear(a);
    a->state = ZH_ASH_DOWN;
}

static void disconnect(zh_ash_t *a, zh_ash_event_t ev, uint8_t code)
{
    link_vars_clear(a);
    a->state = ZH_ASH_DOWN;
    if (a->on_event) a->on_event(a->ctx, ev, code);
}

/* ---------------------------------------------------------------- 发送队列 */

static zh_ash_qent_t *q_at(zh_ash_t *a, unsigned i) { return &a->q[(a->q_start + i) % ZH_ASH_TXQ]; }

bool zh_ash_send(zh_ash_t *a, const uint8_t *data, size_t len)
{
    if (!a || !data || len < ZH_ASH_DATA_MIN || len > ZH_ASH_DATA_MAX) return false;
    if (a->state != ZH_ASH_CONNECTED || a->q_len >= ZH_ASH_TXQ) return false;
    zh_ash_qent_t *e = q_at(a, a->q_len);
    e->len = (uint8_t)len;
    memcpy(e->data, data, len);
    a->q_len++;
    return true;
}

/* 对方确认到 ack_rx：把队头已确认的帧放掉（ash-host.c:781-791 ashScrubReTxQueue） */
static void q_scrub(zh_ash_t *a)
{
    while (a->frm_head != a->ack_rx && a->q_len > 0) {
        a->q_start = (uint8_t)((a->q_start + 1) % ZH_ASH_TXQ);
        a->q_len--;
        a->frm_head = MOD8(a->frm_head + 1);
    }
}

/* 没在重传、又有未确认的帧：从对方确认到的那帧开始重传，先发一个 CAN（ash-host.c:752-760） */
static void start_retransmission(zh_ash_t *a)
{
    if (!(a->flags & F_RETX) && a->ack_rx != a->frm_tx) {
        timer_stop(a);
        a->frm_retx = a->ack_rx;
        a->flags |= F_RETX | F_CAN;
    }
}

/* 上一个帧是 DATA、已连上、还没在拒收状态：回 NAK 并进拒收状态（ash-host.c:764-770） */
static void reject_frame(zh_ash_t *a)
{
    if (ZH_ASH_IS_DATA(a->rx_last_ctrl) && a->state == ZH_ASH_CONNECTED && !(a->flags & F_REJ)) a->flags |= F_REJ | F_NAK;
}

/* ---------------------------------------------------------------- 接收 */

typedef enum { T_INVALID, T_DATA, T_ACK, T_NAK, T_RSTACK, T_ERROR } ftype_t;

/* ash-host.c:882-912 ashFrameType：先看控制字节、再验长度（len 含控制字节、不含 CRC） */
static ftype_t frame_type(zh_ash_t *a, uint8_t c, size_t len)
{
    if (c == ZH_ASH_CTRL_RSTACK) {
        if (len == 3) return T_RSTACK;
    } else if (c == ZH_ASH_CTRL_ERROR) {
        if (len == 3) return T_ERROR;
    } else if (ZH_ASH_IS_DATA(c)) {
        if (len >= 1 + ZH_ASH_DATA_MIN) return T_DATA;
    } else if ((c & 0xE0) == ZH_ASH_CTRL_ACK) {
        if (len == 1) return T_ACK;
    } else if ((c & 0xE0) == ZH_ASH_CTRL_NAK) {
        if (len == 1) return T_NAK;
    } else {
        a->st.rx_bad_ctrl++;
        return T_INVALID;
    }
    a->st.rx_bad_len++;
    return T_INVALID;
}

static void process_frame(zh_ash_t *a, uint8_t *f, size_t len, uint32_t now)
{
    const uint8_t c = f[0];
    a->rx_last_ctrl = c;
    a->st.rx_frames++;
    ftype_t t = frame_type(a, c, len);

    if (a->state != ZH_ASH_CONNECTED) {
        /* 没连上时只认 RSTACK / ERROR（ash-host.c:322-358） */
        if (a->state != ZH_ASH_RESETTING) return;
        if (t == T_RSTACK) {
            if (f[1] != ZH_ASH_VERSION) {
                disconnect(a, ZH_ASH_EV_VERSION, f[1]);
                return;
            }
            if (f[2] != ZH_ASH_RESET_SOFTWARE) return;   /* 我们发的是 RST：不是软件复位的 RSTACK 是复位之前的旧货，接着等 */
            timer_stop(a);
            a->timeouts = 0;
            a->ack_period = a->cfg.ack_init_ms;
            a->state = ZH_ASH_CONNECTED;
            a->flags = F_ACK;                          /* 参考实现连上就回一个 ACK（ash-host.c:352） */
            if (a->on_event) a->on_event(a->ctx, ZH_ASH_EV_CONNECTED, f[2]);
        } else if (t == T_ERROR) {
            a->st.ncp_errors++;
            disconnect(a, ZH_ASH_EV_NCP_ERROR, f[2]);
        }
        return;
    }

    /* 连上了：DATA / ACK / NAK 先处理 ackNum（ash-host.c:360-385） */
    if (t == T_DATA || t == T_ACK || t == T_NAK) {
        const uint8_t ack = (uint8_t)(c & 0x07);
        if (t != T_DATA && (c & ZH_ASH_NFLAG)) a->st.rx_nflag++;
        if (!within_range(a->ack_rx, ack, a->frm_tx)) {
            a->st.rx_bad_ack++;
            t = T_INVALID;
        } else if (ack != a->ack_rx) {
            a->ack_rx = ack;
            a->timeouts = 0;
            if (a->flags & F_RETX) {
                timer_stop(a);
                if (ack != a->frm_retx) timer_start(a, now);
            } else {
                timer_adjust(a, false, now);           /* 把这次往返算进周期（并停表） */
                if (ack != a->frm_tx) timer_start(a, now);
                q_scrub(a);
            }
        }
    }

    switch (t) {
    case T_DATA: {
        const uint8_t frm = (uint8_t)((c >> 4) & 0x07);
        a->st.rx_data++;
        if (frm == a->frm_rx) {
            if (c & ZH_ASH_RFLAG) a->flags |= F_ACK;   /* 对方重传的：一定回 ACK */
            a->flags &= (uint16_t)~(F_REJ | F_NAK);
            a->frm_rx = MOD8(a->frm_rx + 1);
            zh_ash_randomize(f + 1, len - 1);
            if (a->on_frame) a->on_frame(a->ctx, f + 1, len - 1);
        } else if (c & ZH_ASH_RFLAG) {
            a->st.rx_dup++;                            /* 重复的重传帧：内容丢掉、再回一次 ACK */
            a->flags |= F_ACK;
        } else {
            if (!(a->flags & F_REJ)) a->st.rx_oos++;
            reject_frame(a);
        }
        break;
    }
    case T_ACK:
        a->st.rx_ack++;
        break;
    case T_NAK:
        a->st.rx_nak++;
        start_retransmission(a);
        break;
    case T_RSTACK:
        a->st.ncp_resets++;
        disconnect(a, ZH_ASH_EV_NCP_RESET, f[2]);
        break;
    case T_ERROR:
        a->st.ncp_errors++;
        disconnect(a, ZH_ASH_EV_NCP_ERROR, f[2]);
        break;
    case T_INVALID:
    default:
        reject_frame(a);
        break;
    }
}

static void rx_restart(zh_ash_t *a)
{
    a->rx_len = 0;
    a->rx_esc = false;
    a->rx_sub = false;
}

void zh_ash_rx(zh_ash_t *a, const uint8_t *buf, size_t n, uint32_t now_ms)
{
    if (!a || !buf) return;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = buf[i];
        if (b == ZH_ASH_WAKE && a->rx_len == 0 && !a->rx_esc && !a->rx_sub) continue;   /* 帧与帧之间的 WAKE；帧内的 0xFF 是数据 */
        switch (b) {
        case ZH_ASH_FLAG: {
            if (a->rx_len == 0 && !a->rx_sub) {       /* 两个 FLAG 之间没东西：不是帧尾（夹着的孤立 ESC 也丢掉） */
                a->rx_esc = false;
                break;
            }
            const uint16_t raw = a->rx_len;
            if (a->rx_sub) {
                a->st.rx_comm++;
                if (raw) a->rx_last_ctrl = a->rx_raw[0];
                reject_frame(a);
            } else if (raw > ZH_ASH_RAW_MAX) {
                a->st.rx_long++;
                a->rx_last_ctrl = a->rx_raw[0];
                reject_frame(a);
            } else if (raw < 3) {                      /* 控制字节 + CRC 都不够 */
                a->st.rx_short++;
                a->rx_last_ctrl = a->rx_raw[0];
                reject_frame(a);
            } else if (zh_ash_crc16(a->rx_raw, raw - 2u) != (uint16_t)((a->rx_raw[raw - 2] << 8) | a->rx_raw[raw - 1])) {
                a->st.rx_crc++;
                a->rx_last_ctrl = a->rx_raw[0];
                reject_frame(a);
            } else {
                process_frame(a, a->rx_raw, raw - 2u, now_ms);
            }
            rx_restart(a);
            break;
        }
        case ZH_ASH_ESC:
            a->rx_esc = true;
            break;
        case ZH_ASH_CAN:                               /* 取消当前帧，不算错（连上之后才计数，ash-host.c:272-277） */
            if (a->state == ZH_ASH_CONNECTED && a->rx_len) a->st.rx_cancelled++;
            rx_restart(a);
            break;
        case ZH_ASH_SUB:                               /* 这一帧里有 UART 错：到 FLAG 时整帧作废 */
            a->rx_sub = true;
            break;
        case ZH_ASH_XON:
        case ZH_ASH_XOFF:                              /* RTS/CTS 下的带外流控字节：数据里的 11 / 13 都转义过，裸的直接丢 */
            break;
        default:
            if (a->rx_esc) {
                b ^= ZH_ASH_FLIP;
                a->rx_esc = false;
            }
            if (a->rx_len < ZH_ASH_RAW_MAX) a->rx_raw[a->rx_len] = b;
            if (a->rx_len <= ZH_ASH_RAW_MAX) a->rx_len++;   /* 超长时停在 MAX+1：到 FLAG 判"太长" */
            break;
        }
    }
}

/* ---------------------------------------------------------------- 发送 */

static uint32_t emit(zh_ash_t *a, uint8_t ctrl, const uint8_t *data, size_t len, uint8_t *out, size_t cap, size_t *n)
{
    const size_t w = zh_ash_encode(ctrl, data, len, out + *n, cap - *n);
    if (w) {
        *n += w;
        a->st.tx_frames++;
    }
    return (uint32_t)w;
}

/* ash-host.c:443-594 ashSendExec 的顺序：RST > NAK / ACK > 重传 > （该回的 ACK）> 新的 DATA。只写整帧：写不下就留到下次 */
size_t zh_ash_tx(zh_ash_t *a, uint32_t now_ms, uint8_t *out, size_t cap)
{
    if (!a || !out) return 0;

    /* 定时器（ash-host.c:449-467） */
    if (timer_expired(a, now_ms)) {
        if (a->state == ZH_ASH_CONNECTED) {
            if (a->ack_rx != ((a->flags & F_RETX) ? a->frm_retx : a->frm_tx)) {
                a->st.ack_timeouts++;
                timer_adjust(a, true, now_ms);
                if (++a->timeouts >= a->cfg.max_timeouts) {
                    disconnect(a, ZH_ASH_EV_TIMEOUTS, a->timeouts);
                    return 0;
                }
                start_retransmission(a);
            } else {
                timer_stop(a);
            }
        } else if (a->state == ZH_ASH_RESETTING) {
            disconnect(a, ZH_ASH_EV_RESET_FAILED, 0);
            return 0;
        }
    }
    if (a->state == ZH_ASH_DOWN) return 0;

    size_t n = 0;
    for (;;) {
        if (cap - n < ZH_ASH_WIRE_MAX) break;          /* 剩下的放不下最坏的一帧：留到下次 */
        if (a->flags & F_CAN) {
            out[n++] = ZH_ASH_CAN;                     /* RST / 重传前的 CAN：清掉 NCP 解码器里半截的帧（1A C0 38 BC 7E） */
            a->flags &= (uint16_t)~F_CAN;
            continue;
        }
        /* 重传中：对方已经确认过的就不用再发了；全发完了退出重传（ash-host.c:486-496） */
        if (a->flags & F_RETX) {
            if (within_range(a->frm_retx, a->ack_rx, a->frm_tx)) a->frm_retx = a->ack_rx;
            if (a->frm_retx == a->frm_tx) {
                a->flags &= (uint16_t)~F_RETX;
                q_scrub(a);
            }
        }
        if (a->flags & F_RST) {
            a->flags &= (uint16_t)~(F_RST | F_NAK | F_ACK);
            emit(a, ZH_ASH_CTRL_RST, NULL, 0, out, cap, &n);
            a->st.tx_rst++;
            a->ack_period = a->cfg.rst_timeout_ms;     /* 等 RSTACK 用同一个表（ash-host.c:505 ashSetAndStartAckTimer(timeRst)） */
            timer_start(a, now_ms);
            continue;
        }
        if (a->state != ZH_ASH_CONNECTED) break;       /* 还在等 RSTACK：别的都不发 */
        if (a->flags & (F_NAK | F_ACK)) {
            const bool nak = (a->flags & F_NAK) != 0;
            a->flags &= (uint16_t)~(nak ? (F_NAK | F_ACK) : F_ACK);
            emit(a, (uint8_t)((nak ? ZH_ASH_CTRL_NAK : ZH_ASH_CTRL_ACK) | a->frm_rx), NULL, 0, out, cap, &n);
            if (nak) a->st.tx_nak++;
            else a->st.tx_ack++;
            a->ack_tx = a->frm_rx;
            continue;
        }
        if (a->flags & F_RETX) {
            const unsigned idx = MOD8(a->frm_retx - a->frm_head);
            if (idx >= a->q_len) {                     /* 不可能：未确认的帧都在队里。防御：退出重传 */
                a->flags &= (uint16_t)~F_RETX;
                continue;
            }
            const zh_ash_qent_t *e = q_at(a, idx);
            emit(a, (uint8_t)((a->frm_retx << 4) | ZH_ASH_RFLAG | a->frm_rx), e->data, e->len, out, cap, &n);
            a->st.tx_redata++;
            a->frm_retx = MOD8(a->frm_retx + 1);
            if (!a->ack_running) timer_start(a, now_ms);
            a->ack_tx = a->frm_rx;
            continue;
        }
        if (a->ack_tx != a->frm_rx) {                  /* 收到过 DATA 还没确认 */
            a->flags |= F_ACK;
            continue;
        }
        const unsigned unacked = MOD8(a->frm_tx - a->frm_head);
        if (unacked < a->q_len && within_range(a->ack_rx, a->frm_tx, MOD8(a->ack_rx + a->cfg.tx_k - 1))) {
            const zh_ash_qent_t *e = q_at(a, unacked);
            emit(a, (uint8_t)((a->frm_tx << 4) | a->frm_rx), e->data, e->len, out, cap, &n);
            a->st.tx_data++;
            a->frm_tx = MOD8(a->frm_tx + 1);
            if (!a->ack_running) timer_start(a, now_ms);
            a->ack_tx = a->frm_rx;
            continue;
        }
        break;                                         /* 没东西要发 */
    }
    return n;
}

bool zh_ash_tx_pending(const zh_ash_t *a)
{
    if (!a || a->state == ZH_ASH_DOWN) return false;
    if (a->flags & (F_RST | F_CAN)) return true;
    if (a->state != ZH_ASH_CONNECTED) return false;
    if ((a->flags & (F_ACK | F_NAK | F_RETX)) || a->ack_tx != a->frm_rx) return true;
    const unsigned unacked = MOD8(a->frm_tx - a->frm_head);
    return unacked < a->q_len && within_range(a->ack_rx, a->frm_tx, MOD8(a->ack_rx + a->cfg.tx_k - 1));
}

uint32_t zh_ash_next_timeout_ms(const zh_ash_t *a, uint32_t now_ms)
{
    if (!a) return UINT32_MAX;
    if (zh_ash_tx_pending(a)) return 0;              /* 有东西要写：别按定时器睡（第一轮审计：reset / send 之后会睡死） */
    if (!a->ack_running) return UINT32_MAX;
    const uint32_t el = (uint32_t)(now_ms - a->ack_start_ms);
    return el >= a->ack_period ? 0 : a->ack_period - el;
}
