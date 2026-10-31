/* zh_ezsp_ses —— EZSP 会话层（见 include/zh_ezsp_ses.h：管什么、H1、线程契约、为什么这样定）。纯 C，不依赖 ESP-IDF，不分配内存。 */
#include "zh_ezsp_ses.h"

#include <string.h>

void zh_ses_cfg_default(zh_ses_cfg_t *c)
{
    if (!c) return;
    /* 应答超时要盖住的是 **NCP 侧** ASH 重传应答的耗时，不是 NCP 的处理时间：应答帧被噪声打坏时，NCP 按自己的 ACK 定时器重传，
       周期超时就翻倍、上限 3200 ms，第 4 次超时才进 ERROR（ash-common.c:238-239、ash-ncp.c:483-490、ash-ncp.h:237-242）→ 单帧最坏约
       4 × 3200 = 12.8 s。这还不是严格上界：NCP 的发送窗口里排着别的帧（回调）时，部分确认会把它的超时计数清零（ash-ncp.c:404-406），
       机制上没有严格上界（复核审计在 200 个种子里实测最大 9.6 s = 单帧第 4 次发送的时刻；另一轮审计在极端噪声 + 回调排队下报告过 14.7 s）。
       会话若先放弃又发下一条，NCP 会把新命令写进那块还等着重传旧应答的静态缓冲（em260 原地应答；
       2 s 时 ses_interop 的种子扫描实测把 NCP 的发送队列弄乱）。所以两件事一起做：应答超时取 15 s（健康但嘈杂的链路几乎碰不到），
       而且**超时一次就复位链路**（on_timeout：ASH RST 让 NCP 重启、清空它的发送队列 —— 唯一不依赖上界的做法），不重发。
       主机侧 ASH 的最坏耗时（zh_ash_cfg_default() 的 max_timeouts × ack_max_ms）由"确认之后才计时"避开；ack_guard 是它之上的兜底。
       max_retries 只管回显后的重发。为什么回显后重发、以及所有"收到 NCP 一帧之后发下一条"都不撞缓冲：主机 ASH 已收下 NCP 那一帧，
       之后主机发的任何 DATA 帧都带着对它的确认，而 NCP 收齐一帧后先处理 ackNum（把那块缓冲移出重传队列）再交数据（ash-ncp.c:395-418 先于 432-450）。
       撞缓冲只会发生在"NCP 那一帧还没被主机收下"时——也就是应答超时之后，所以超时只能复位。 */
    c->rsp_timeout_ms = 15000;
    c->ack_guard_ms = 20000;
    c->keepalive_ms = 10000;
    c->max_retries = 1;
    c->poll_callbacks = true;
}

static void sanitize(zh_ses_cfg_t *c)
{
    zh_ses_cfg_t d;
    zh_ses_cfg_default(&d);
    if (!c->rsp_timeout_ms) c->rsp_timeout_ms = d.rsp_timeout_ms;
    if (!c->ack_guard_ms) c->ack_guard_ms = d.ack_guard_ms;
    if (c->max_retries > ZH_SES_MAX_RETRIES_CAP) c->max_retries = ZH_SES_MAX_RETRIES_CAP;   /* 防"回显就重发"无限循环 */
}

void zh_ses_init(zh_ses_t *s, const zh_ses_cfg_t *cfg, const zh_ses_ops_t *ops)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    if (cfg) s->cfg = *cfg;
    else zh_ses_cfg_default(&s->cfg);
    sanitize(&s->cfg);
    if (ops) s->ops = *ops;
    s->state = ZH_SES_DOWN;
}

bool zh_ses_idempotent(uint16_t id)
{
    switch (id) {
    /* 读取类 */
    case ZH_EZSP_ID_VERSION:
    case ZH_EZSP_ID_GET_CONFIGURATION_VALUE:
    case ZH_EZSP_ID_GET_VALUE:
    case ZH_EZSP_ID_NOP:
    case ZH_EZSP_ID_GET_EUI64:
    case ZH_EZSP_ID_GET_NODE_ID:
    case ZH_EZSP_ID_NETWORK_STATE:
    case ZH_EZSP_ID_GET_NETWORK_PARAMETERS:
    case ZH_EZSP_ID_LOOKUP_EUI64_BY_NODE_ID:
    case ZH_EZSP_ID_LOOKUP_NODE_ID_BY_EUI64:
    case ZH_EZSP_ID_GET_CURRENT_SECURITY_STATE:
    /* "设成某个值"：做两次与做一次结果相同（setConfigurationValue 在网络起来后两次都回 INVALID_CALL，command-prototypes.h:49-52） */
    case ZH_EZSP_ID_SET_CONFIGURATION_VALUE:
    case ZH_EZSP_ID_SET_POLICY:
    case ZH_EZSP_ID_SET_RADIO_POWER:
    case ZH_EZSP_ID_SET_CONCENTRATOR:
    case ZH_EZSP_ID_PERMIT_JOINING:          /* 再发一次只是把窗口重新计时 */
    case ZH_EZSP_ID_CLEAR_TRANSIENT_LINK_KEYS:
    case ZH_EZSP_ID_SEC_MAN_IMPORT_TRANSIENT_KEY:   /* 同一把（通配 EUI64 + 公开密钥）再导一次只是重新计时 */
        return true;
    default:
        /* addEndpoint / networkInit / startScan / formNetwork / leaveNetwork / sendUnicast / sendBroadcast / setInitialSecurityState /
           customFrame（XNCP 的语义由它的子命令定）/ setValue（有的 value 是动作，比如清计数）/ callback（会话自己用，取走一条回调）/
           不认识的：一律不自动重发 */
        return false;
    }
}

/* ---------------------------------------------------------------- 内部 */

static bool active(const zh_ses_t *s) { return s->state == ZH_SES_HANDSHAKE || s->state == ZH_SES_READY; }

/* 命令了结后整块清零：命令帧里可能有网络密钥（setInitialSecurityState，设计第十三节 / Z17），不留在会话里 */
static void scrub(zh_ses_cmd_t *c) { memset(c, 0, sizeof(*c)); }

/* 了结时只带走这几个字段（不把整块命令连同密钥拷到栈上） */
typedef struct { bool used, internal; uint8_t tries; uint32_t tag; } meta_t;
static meta_t take(zh_ses_cmd_t *c)
{
    const meta_t m = { c->used, c->internal, c->tries, c->tag };
    scrub(c);
    return m;
}

static void set_internal(zh_ses_cmd_t *c, uint16_t id, size_t len, bool idempotent, uint32_t now)
{
    c->used = true;
    c->internal = true;
    c->idempotent = idempotent;
    c->handed = c->acked = false;
    c->tries = 0;
    c->id = id;
    c->tag = 0;
    c->len = (uint8_t)len;
    c->t_queued = now;
}

/* 把在途命令交给 ASH。每次发送（含重发）用新 seq：迟到的旧应答就按 seq 当过期丢掉，不会被当成新这次的应答 */
static void send_cur(zh_ses_t *s, uint32_t now)
{
    zh_ses_cmd_t *c = &s->cur;
    const uint8_t keep = c->buf[0];
    c->buf[0] = s->seq_next++;
    if (s->ops.send && s->ops.send(s->ops.ctx, c->buf, c->len)) {
        c->seq = c->buf[0];
        c->handed = true;
        c->acked = false;
        c->t_handed = now;
        c->tries++;
    } else {
        c->buf[0] = keep;      /* ASH 没收（没连上 / 队列满）：下一拍再试；久收不下由 ack_guard 兜（tick） */
        s->seq_next--;
    }
}

/* 该发的发出去。正在调上层回调时不发（回调返回后由外层再 pump）。
   NCP 说还有回调待取、上层又有一条排着时轮流来：谁也饿不死（同步回调模式下回调攒在 NCP 里会溢出；上层命令也不能一直等） */
static void pump(zh_ses_t *s, uint32_t now)
{
    if (s->depth || !active(s)) return;
    if (!s->cur.used) {
        const bool cb = s->state == ZH_SES_READY && s->cb_wanted && s->cfg.poll_callbacks;
        if (cb && (!s->pend.used || !s->cb_turn)) {
            const size_t n = zh_ezsp_enc_callback(s->cur.buf, sizeof(s->cur.buf), 0);
            if (n) set_internal(&s->cur, ZH_EZSP_ID_CALLBACK, n, true, now);   /* 会话自己当幂等：回显后重发拿下一条（被吞的那条记 lost） */
            s->cb_wanted = false;
            s->cb_turn = true;
        } else if (s->pend.used) {
            s->cur = s->pend;
            scrub(&s->pend);
            s->cb_turn = false;
        } else if (s->state == ZH_SES_READY && s->cfg.keepalive_ms && now - s->t_idle >= s->cfg.keepalive_ms) {
            const size_t n = zh_ezsp_enc_nop(s->cur.buf, sizeof(s->cur.buf), 0);
            if (n) {
                set_internal(&s->cur, ZH_EZSP_ID_NOP, n, true, now);
                s->st.keepalives++;
            }
        }
    }
    if (s->cur.used && !s->cur.handed) send_cur(s, now);
}

static void deliver(zh_ses_t *s, uint32_t tag, zh_ses_result_t r, const zh_ezsp_frame_t *fr)
{
    if (r == ZH_SES_R_UNKNOWN) s->st.unknown++;
    if (!s->ops.on_result) return;
    s->depth++;
    s->ops.on_result(s->ops.ctx, tag, r, fr);
    s->depth--;
}

static void emit(zh_ses_t *s, zh_ses_event_t ev, uint32_t code)
{
    if (!s->ops.on_event) return;
    s->depth++;
    s->ops.on_event(s->ops.ctx, ev, code);
    s->depth--;
}

/* 链路断 / 停住时一条上层命令的结论：从没交给 ASH 的才是"确定没执行"（LINK_DOWN），交出去过的执行没执行都不知道（UNKNOWN，Z59） */
static zh_ses_result_t cut_result(const meta_t *m) { return m->tries ? ZH_SES_R_UNKNOWN : ZH_SES_R_LINK_DOWN; }

/* NCP 应用层还活着的正向证据（04 的"正向证据"，Z47）：只在收到与在途命令配上的应答、或一条合法回调时记 */
static void alive(zh_ses_t *s, uint32_t now)
{
    s->have_alive = true;
    s->t_alive = now;
    s->st.responses++;
}

/* 在途命令了结：先把会话收拾好（回调里可以 submit / link_down），再交结果，最后发下一条 */
static void finish(zh_ses_t *s, zh_ses_result_t r, const zh_ezsp_frame_t *fr, uint32_t now)
{
    const meta_t m = take(&s->cur);
    s->t_idle = now;
    if (!m.internal) deliver(s, m.tag, r, fr);
    pump(s, now);
}

/* 会话判断链路不可用：停住；在途的上层命令给 cur_r（从没交出去的给 LINK_DOWN），排队的给 LINK_DOWN；最后**总是**报 RESET_LINK ——
   哪怕上层在结果回调里已经 link_down 了（它在 RESET_LINK 里再 link_down 一次是幂等的）：复位 ASH 是唯一能清空 NCP 发送队列的办法，
   这条事件不能被吞（回调里 link_up 会被忽略，见 zh_ses_link_up） */
static void stop(zh_ses_t *s, zh_ses_why_t why, zh_ses_result_t cur_r, const zh_ezsp_frame_t *fr)
{
    const meta_t c = take(&s->cur), p = take(&s->pend);
    s->cb_wanted = false;
    s->state = ZH_SES_STOPPED;
    if (c.used && !c.internal) deliver(s, c.tag, c.tries ? cur_r : ZH_SES_R_LINK_DOWN, c.tries ? fr : NULL);
    if (p.used) deliver(s, p.tag, ZH_SES_R_LINK_DOWN, NULL);
    emit(s, ZH_SES_EV_RESET_LINK, why);
}

static zh_ses_why_t why_internal(const zh_ses_t *s)
{
    if (s->cur.id == ZH_EZSP_ID_VERSION) return ZH_SES_WHY_HANDSHAKE;
    if (s->cur.id == ZH_EZSP_ID_NOP) return ZH_SES_WHY_KEEPALIVE;
    return ZH_SES_WHY_RSP_TIMEOUT;
}

static void retry(zh_ses_t *s, uint32_t now)
{
    s->st.retries++;
    s->cur.handed = false;
    s->cur.acked = false;
    s->cur.t_queued = now;
}

/* 回显：NCP 执行过这条命令，应答被重传的命令覆盖了。幂等的重发，非幂等的回 UNKNOWN（Z59） */
static void on_echo(zh_ses_t *s, uint32_t now)
{
    s->st.echoes++;
    if (s->cur.id == ZH_EZSP_ID_CALLBACK && s->cur.internal) s->st.lost_callbacks++;   /* 被吞的应答就是那条回调 */
    if (s->cur.idempotent && s->cur.tries <= s->cfg.max_retries) {
        retry(s, now);
        pump(s, now);
    } else if (s->cur.internal) {
        stop(s, ZH_SES_WHY_ECHOES, ZH_SES_R_LINK_DOWN, NULL);   /* 内部命令重发用完了还是回显：这条链路有问题 */
    } else {
        finish(s, ZH_SES_R_UNKNOWN, NULL, now);
    }
}

/* NCP 确认收到之后 rsp_timeout_ms 没应答：复位链路，不重发（理由见 zh_ses_cfg_default：此时 NCP 可能还有应答在重传，再发就撞缓冲）。
   在途的上层命令回 TIMEOUT（执行没执行不知道）。 */
static void on_timeout(zh_ses_t *s)
{
    s->st.timeouts++;
    stop(s, s->cur.internal ? why_internal(s) : ZH_SES_WHY_RSP_TIMEOUT, ZH_SES_R_TIMEOUT, NULL);
}

static void deliver_callback(zh_ses_t *s, const zh_ezsp_frame_t *fr)
{
    s->st.callbacks++;
    if (!s->ops.on_callback) return;
    s->depth++;
    s->ops.on_callback(s->ops.ctx, fr);
    s->depth--;
}

/* 首条 version 的应答 */
static void on_version(zh_ses_t *s, const zh_ezsp_frame_t *fr, uint32_t now)
{
    uint8_t proto = 0, stype = 0;
    uint16_t sver = 0;
    if (zh_ezsp_dec_version(fr, &proto, &stype, &sver) != ZH_EZSP_OK) {
        s->st.rejected++;          /* 长度不对的应答：当没收到，等超时（超时即复位） */
        return;
    }
    alive(s, now);
    scrub(&s->cur);
    s->t_idle = now;
    if (proto == ZH_EZSP_PROTOCOL_VERSION) {
        s->state = ZH_SES_READY;
        emit(s, ZH_SES_EV_READY, ((uint32_t)stype << 16) | sver);
        pump(s, now);
    } else {
        s->state = ZH_SES_STOPPED;
        const meta_t p = take(&s->pend);     /* 握手中上层不能 submit，这里按理为空；防御 */
        if (p.used) deliver(s, p.tag, ZH_SES_R_LINK_DOWN, NULL);
        emit(s, ZH_SES_EV_VERSION_MISMATCH, proto);
    }
}

/* ---------------------------------------------------------------- 对外 */

void zh_ses_link_down(zh_ses_t *s)
{
    if (!s) return;
    const meta_t c = take(&s->cur), p = take(&s->pend);
    s->cb_wanted = false;
    s->state = ZH_SES_DOWN;
    if (c.used && !c.internal) deliver(s, c.tag, cut_result(&c), NULL);
    if (p.used) deliver(s, p.tag, ZH_SES_R_LINK_DOWN, NULL);
}

void zh_ses_link_up(zh_ses_t *s, uint32_t now)
{
    if (!s) return;
    if (s->depth) {            /* 回调里不许 link_up：那时 ASH 还没复位（复位要等 RSTACK 往返），在没复位的链路上握手会撞 NCP 的缓冲 */
        s->st.misuse++;
        return;
    }
    if (s->cur.used || s->pend.used || s->state != ZH_SES_DOWN) zh_ses_link_down(s);   /* 上层漏调 link_down 也不留悬着的命令 */
    s->state = ZH_SES_HANDSHAKE;
    s->seq_next = 0;
    s->cb_turn = false;
    s->t_idle = now;
    /* 首条 version 用旧式头（ezsp.c:525-528、628-633）；只有这一条在途时才接受旧式头的应答 */
    const size_t n = zh_ezsp_enc_version_legacy(s->cur.buf, sizeof(s->cur.buf), 0, ZH_EZSP_PROTOCOL_VERSION);
    set_internal(&s->cur, ZH_EZSP_ID_VERSION, n, true, now);
    pump(s, now);
}

bool zh_ses_can_submit(const zh_ses_t *s)
{
    return s && s->state == ZH_SES_READY && !s->pend.used && !(s->cur.used && !s->cur.internal);
}

int zh_ses_submit(zh_ses_t *s, const uint8_t *frame, size_t len, uint32_t tag, bool idempotent, uint32_t now)
{
    if (!s || !frame || len < ZH_EZSP_EXT_HDR || len > ZH_EZSP_MAX) return ZH_SES_E_ARG;
    /* 只收扩展头的命令帧（zh_ezsp_enc_* 编出来的）：方向位是"命令"、格式版本 1、保留位全 0 */
    if ((frame[1] & ZH_EZSP_FC_RESPONSE) || frame[2] != ZH_EZSP_FCH_FORMAT_V1) return ZH_SES_E_ARG;
    const uint16_t id = (uint16_t)(frame[3] | (frame[4] << 8));
    /* callback 是会话自己的（应答是别的帧 ID 的回调，上层发了只会配不上、超时复位） */
    if (id == ZH_EZSP_ID_CALLBACK || id == ZH_EZSP_ID_NO_CALLBACKS) return ZH_SES_E_ARG;
    if (s->state != ZH_SES_READY) return ZH_SES_E_NOT_READY;
    if (!zh_ses_can_submit(s)) return ZH_SES_E_BUSY;
    /* 内部命令在途就排在它后面；NCP 说还有回调待取、而回调这一轮还没轮到时也先排着（否则上层在结果回调里一条接一条地 submit，
       callback 轮询永远轮不到） */
    const bool cb_first = s->cb_wanted && s->cfg.poll_callbacks && !s->cb_turn;
    zh_ses_cmd_t *c = (s->cur.used || cb_first) ? &s->pend : &s->cur;
    if (c == &s->cur) s->cb_turn = false;     /* 上层这一条直接上了：下一轮回调先 */
    memcpy(c->buf, frame, len);
    c->len = (uint8_t)len;
    c->used = true;
    c->internal = false;
    c->idempotent = idempotent;
    c->handed = c->acked = false;
    c->tries = 0;
    c->id = id;
    c->tag = tag;
    c->t_queued = now;
    pump(s, now);
    return ZH_SES_OK;
}

void zh_ses_rx(zh_ses_t *s, const uint8_t *f, size_t n, uint32_t now)
{
    if (!s || !f || !active(s)) return;
    s->st.rx_frames++;
    zh_ezsp_frame_t fr;
    const int e = zh_ezsp_parse(f, n, &fr);
    if (e == ZH_EZSP_E_SHORT || e == ZH_EZSP_E_ARG || e == ZH_EZSP_E_RESERVED) {
        s->st.rejected++;
        return;
    }
    if (zh_ezsp_overflow(&fr)) s->st.overflow++;
    zh_ses_cmd_t *c = &s->cur;
    const bool mine = c->used && c->handed && fr.seq == c->seq;
    if (e == ZH_EZSP_E_DIRECTION) {
        if (mine && fr.id == c->id) on_echo(s, now);
        else s->st.rejected++;
        return;
    }
    /* 异步回调（UART 版 NCP 主动推送，fc 0x10）先认出来：它不是谁的应答，seq 碰巧等于在途那条也一样（SDK 主机也先看这一位，
       serial-interface-uart.c:129-133）。截断的回调只计数丢掉，不许把在途命令判成截断 */
    if (fr.fc & ZH_EZSP_FC_ASYNC_CB) {
        if (fr.legacy || e != ZH_EZSP_OK) {
            if (e == ZH_EZSP_E_TRUNCATED) s->st.truncated++;
            else s->st.rejected++;
            return;
        }
        s->t_idle = now;
        alive(s, now);
        if (zh_ezsp_pending_cb(&fr)) s->cb_wanted = true;
        deliver_callback(s, &fr);
        pump(s, now);
        return;
    }
    /* 旧式头只给握手那条 version 的应答（Z08） */
    if (fr.legacy && !(mine && c->internal && c->id == ZH_EZSP_ID_VERSION && s->state == ZH_SES_HANDSHAKE)) {
        s->st.rejected++;
        return;
    }
    if (!mine) {
        s->st.stale++;             /* 迟到的旧应答（重发换了 seq）/ 没有在途时来的应答 */
        return;
    }
    if (e == ZH_EZSP_E_INVALID_CMD) {
        s->st.invalid++;
        alive(s, now);
        s->cb_wanted = zh_ezsp_pending_cb(&fr);
        if (fr.invalid_reason == ZH_SES_INVALID_VERSION_NOT_SET) {
            stop(s, ZH_SES_WHY_VERSION_NOT_SET, ZH_SES_R_INVALID, &fr);   /* NCP 不记得握过手：只能整条链路重来 */
        } else if (c->internal && c->id == ZH_EZSP_ID_VERSION) {
            stop(s, ZH_SES_WHY_HANDSHAKE, ZH_SES_R_LINK_DOWN, NULL);
        } else {
            finish(s, ZH_SES_R_INVALID, &fr, now);                       /* 内部 nop / callback 的 invalid：了结、不交上层 */
        }
        return;
    }
    if (e == ZH_EZSP_E_TRUNCATED) {
        s->st.truncated++;
        alive(s, now);
        s->cb_wanted = zh_ezsp_pending_cb(&fr);
        if (c->internal && c->id == ZH_EZSP_ID_VERSION) stop(s, ZH_SES_WHY_HANDSHAKE, ZH_SES_R_LINK_DOWN, NULL);
        else finish(s, ZH_SES_R_TRUNCATED, &fr, now);
        return;
    }
    /* e == ZH_EZSP_OK：一条应答 */
    if (c->internal && c->id == ZH_EZSP_ID_CALLBACK) {
        if (fr.id == ZH_EZSP_ID_NO_CALLBACKS) {     /* 先认 noCallbacks：它也带 fc 0x08（"这是 callback 命令的应答"），但不是回调 */
            if (zh_ezsp_dec_empty(&fr, ZH_EZSP_ID_NO_CALLBACKS) != ZH_EZSP_OK) {
                s->st.rejected++;
                return;
            }
            alive(s, now);
            s->cb_wanted = zh_ezsp_pending_cb(&fr);
            finish(s, ZH_SES_R_OK, &fr, now);
            return;
        }
        if (fr.fc & ZH_EZSP_FC_SYNC_CB) {           /* 应答就是一条回调 */
            alive(s, now);
            s->cb_wanted = zh_ezsp_pending_cb(&fr);
            scrub(c);
            s->t_idle = now;
            deliver_callback(s, &fr);
            pump(s, now);
            return;
        }
        s->st.id_mismatch++;
        return;
    }
    if (fr.id != c->id) {
        s->st.id_mismatch++;      /* seq 对、帧 ID 不对：不当应答，等超时 */
        return;
    }
    if (c->internal && c->id == ZH_EZSP_ID_VERSION) {
        on_version(s, &fr, now);
        return;
    }
    if (c->internal && c->id == ZH_EZSP_ID_NOP && zh_ezsp_dec_empty(&fr, ZH_EZSP_ID_NOP) != ZH_EZSP_OK) {
        s->st.rejected++;         /* 保活 nop 的应答长度不对：当没收到 */
        return;
    }
    alive(s, now);
    s->cb_wanted = zh_ezsp_pending_cb(&fr);
    finish(s, ZH_SES_R_OK, &fr, now);
}

void zh_ses_tick(zh_ses_t *s, uint32_t now)
{
    if (!s || !active(s)) return;
    zh_ses_cmd_t *c = &s->cur;
    if (c->used) {
        if (!c->handed) {
            if (now - c->t_queued >= s->cfg.ack_guard_ms) {   /* ASH 一直不收（不该发生：CONNECTED 之后才 link_up）：兜底 */
                stop(s, ZH_SES_WHY_ACK_GUARD, ZH_SES_R_TIMEOUT, NULL);
                return;
            }
        } else if (!c->acked) {
            if (!s->ops.unacked || s->ops.unacked(s->ops.ctx) == 0) {
                c->acked = true;
                c->t_acked = now;
            } else if (now - c->t_handed >= s->cfg.ack_guard_ms) {
                stop(s, ZH_SES_WHY_ACK_GUARD, ZH_SES_R_TIMEOUT, NULL);
                return;
            }
        }
        if (c->acked && now - c->t_acked >= s->cfg.rsp_timeout_ms) {
            on_timeout(s);
            return;
        }
    }
    pump(s, now);
}

static uint32_t poll_until(uint32_t since, uint32_t limit, uint32_t now)
{
    const uint32_t el = now - since;
    if (el >= limit) return 0;
    const uint32_t left = limit - el;
    return left < ZH_SES_POLL_MS ? left : ZH_SES_POLL_MS;
}

uint32_t zh_ses_next_timeout_ms(const zh_ses_t *s, uint32_t now)
{
    if (!s || !active(s)) return UINT32_MAX;
    const zh_ses_cmd_t *c = &s->cur;
    /* 没交出去（ASH 不收）/ 没被确认：都没有事件可等，只能短间隔回来看；同时不晚于兜底 */
    if (c->used && !c->handed) return poll_until(c->t_queued, s->cfg.ack_guard_ms, now);
    if (c->used && !c->acked) return poll_until(c->t_handed, s->cfg.ack_guard_ms, now);
    if (c->used) {
        const uint32_t el = now - c->t_acked;
        return el >= s->cfg.rsp_timeout_ms ? 0 : s->cfg.rsp_timeout_ms - el;
    }
    if (s->state != ZH_SES_READY) return UINT32_MAX;
    if (s->pend.used || (s->cb_wanted && s->cfg.poll_callbacks)) return 0;
    if (!s->cfg.keepalive_ms) return UINT32_MAX;
    const uint32_t el = now - s->t_idle;
    return el >= s->cfg.keepalive_ms ? 0 : s->cfg.keepalive_ms - el;
}

bool zh_ses_alive_evidence(const zh_ses_t *s, uint32_t *t_ms)
{
    if (!s || !s->have_alive) return false;
    if (t_ms) *t_ms = s->t_alive;
    return true;
}
