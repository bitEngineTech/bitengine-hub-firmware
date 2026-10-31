/* zh_ezsp_ses 的真值表（docs/15 设计 8.4 收发规则、8.13；CHECKLIST Z08 Z17 Z47 Z57 Z58 Z59 C15）。编的是产品源文件 ../zh_ezsp_ses.c。
 * 下面的"ASH"是假的：send 记下交出去的帧，unacked 由测试控制（模拟 NCP 什么时候确认）；NCP 的应答是测试手搓的字节。
 * 与 SDK 真 NCP 状态机、真 zh_ash_core 一起跑的端到端在 sdk_interop/ses_interop.c。 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "zh_ash_core.h"
#include "zh_ezsp_ses.h"

static int s_fail, s_pass;
#define CHECK(cond, ...) do { if (cond) { s_pass++; } else { s_fail++; printf("❌ %s:%d ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------------------------------------------------------- 假 ASH 与回调记录 */

static uint8_t  s_sent[64][ZH_EZSP_MAX];
static size_t   s_sent_len[64];
static int      s_nsent;
static bool     s_send_ok = true;
static unsigned s_unacked;

static struct { uint32_t tag; zh_ses_result_t r; bool has_fr; uint16_t id; uint8_t seq; uint8_t reason; } s_res[32];
static int      s_nres;
static uint16_t s_cb_id[32];
static int      s_ncb;
static zh_ses_event_t s_ev[16];
static uint32_t s_ev_code[16];
static int      s_nev;

static zh_ses_t S;
/* 回调里做的事（重入测试） */
static enum { RE_NONE, RE_SUBMIT, RE_SUBMIT_ALWAYS, RE_LINK_DOWN, RE_DOWN_UP } s_on_result = RE_NONE, s_on_event = RE_NONE;
static zh_ses_state_t s_state_in_cb[32];     /* 每次结果回调时会话已经处在什么状态（回调里看到的应是新状态） */
static char     s_order[64];                 /* 公平性：依次发出的是回调（c）还是上层命令（u） */
static uint32_t s_now;

static bool f_send(void *ctx, const uint8_t *f, size_t n)
{
    (void)ctx;
    if (!s_send_ok) return false;
    if (s_nsent < 64) { memcpy(s_sent[s_nsent], f, n); s_sent_len[s_nsent] = n; }
    s_nsent++;
    s_unacked = 1;
    return true;
}
static unsigned f_unacked(void *ctx) { (void)ctx; return s_unacked; }
static void f_result(void *ctx, uint32_t tag, zh_ses_result_t r, const zh_ezsp_frame_t *fr)
{
    (void)ctx;
    if (s_nres < 32) {
        s_state_in_cb[s_nres] = S.state;
        s_res[s_nres].tag = tag; s_res[s_nres].r = r; s_res[s_nres].has_fr = fr != NULL;
        s_res[s_nres].id = fr ? fr->id : 0xFFFF; s_res[s_nres].seq = fr ? fr->seq : 0; s_res[s_nres].reason = fr ? fr->invalid_reason : 0;
    }
    s_nres++;
    if (s_on_result == RE_SUBMIT || s_on_result == RE_SUBMIT_ALWAYS) {
        uint8_t b[16];
        const size_t n = zh_ezsp_enc_get_node_id(b, sizeof(b), 0);
        const int before = s_nsent;
        CHECK(zh_ses_submit(&S, b, n, 99, true, s_now) == ZH_SES_OK, "on_result 里 submit 下一条应被接受");
        CHECK(s_nsent == before, "回调里 submit 的那条不在回调里发（等回调返回）");
        if (s_on_result == RE_SUBMIT) s_on_result = RE_NONE;
    } else if (s_on_result == RE_LINK_DOWN) {
        s_on_result = RE_NONE;
        zh_ses_link_down(&S);
    } else if (s_on_result == RE_DOWN_UP) {
        s_on_result = RE_NONE;
        zh_ses_link_down(&S);
        zh_ses_link_up(&S, s_now);
    }
}
static void f_callback(void *ctx, const zh_ezsp_frame_t *fr) { (void)ctx; if (s_ncb < 32) s_cb_id[s_ncb] = fr->id; s_ncb++; }
static void f_event(void *ctx, zh_ses_event_t ev, uint32_t code)
{
    (void)ctx;
    if (s_nev < 16) { s_ev[s_nev] = ev; s_ev_code[s_nev] = code; }
    s_nev++;
    if (s_on_event == RE_LINK_DOWN) {        /* 上层对 RESET_LINK 的正确处置：link_down，然后复位 ASH（这里只模拟），连上后才 link_up */
        s_on_event = RE_NONE;
        zh_ses_link_down(&S);
    }
}

static const zh_ses_ops_t k_ops = { f_send, f_unacked, f_result, f_callback, f_event, NULL };

/* 真值表用短一点、好数的参数（默认值另有用例核对）：应答 2 s、回显后重发 2 次 */
static zh_ses_cfg_t test_cfg(void)
{
    zh_ses_cfg_t c;
    zh_ses_cfg_default(&c);
    c.rsp_timeout_ms = 2000;
    c.max_retries = 2;
    return c;
}
static void reset_rec(void)
{
    s_nsent = s_nres = s_ncb = s_nev = 0;
    s_send_ok = true;
    s_unacked = 0;
    s_on_result = s_on_event = RE_NONE;
    s_now = 1000;
}
static void fresh(void)
{
    reset_rec();
    const zh_ses_cfg_t c = test_cfg();
    zh_ses_init(&S, &c, &k_ops);
}
static void at(uint32_t t) { s_now = t; zh_ses_tick(&S, t); }
static void ack(void) { s_unacked = 0; zh_ses_tick(&S, s_now); }

/* NCP 的应答：[seq][fc][0x01][id 小端] + 参数 */
static void ncp(uint8_t seq, uint8_t fc, uint16_t id, const uint8_t *par, size_t n)
{
    uint8_t f[ZH_EZSP_MAX];
    f[0] = seq; f[1] = fc; f[2] = 0x01; f[3] = (uint8_t)id; f[4] = (uint8_t)(id >> 8);
    if (n) memcpy(f + 5, par, n);
    zh_ses_rx(&S, f, 5 + n, s_now);
}
static uint8_t last_seq(void) { return s_sent[s_nsent - 1][0]; }
static uint16_t last_id(void) { const uint8_t *f = s_sent[s_nsent - 1]; return s_sent_len[s_nsent - 1] == 4 ? f[2] : (uint16_t)(f[3] | (f[4] << 8)); }
static const uint8_t k_nid[] = { 0x34, 0x12 };

/* 连上并握完手（旧式头 version → 协议 0x12） */
static void up(void)
{
    zh_ses_link_up(&S, s_now);
    const uint8_t v[] = { 0x00, 0x80, 0x00, 0x12, 0x02, 0x34, 0x12 };
    ack();
    zh_ses_rx(&S, v, sizeof(v), s_now);
}
static int submit_id(uint16_t id, uint32_t tag)
{
    uint8_t b[64];
    size_t n = 0;
    switch (id) {
    case ZH_EZSP_ID_GET_NODE_ID: n = zh_ezsp_enc_get_node_id(b, sizeof(b), 0x77); break;
    case ZH_EZSP_ID_NETWORK_STATE: n = zh_ezsp_enc_network_state(b, sizeof(b), 0x77); break;
    case ZH_EZSP_ID_LEAVE_NETWORK: n = zh_ezsp_enc_leave_network(b, sizeof(b), 0x77, 0); break;
    case ZH_EZSP_ID_PERMIT_JOINING: n = zh_ezsp_enc_permit_joining(b, sizeof(b), 0x77, 60); break;
    default: return -100;
    }
    return zh_ses_submit(&S, b, n, tag, zh_ses_idempotent(id), s_now);
}
static bool last_ev(zh_ses_event_t ev, uint32_t code) { return s_nev > 0 && s_ev[s_nev - 1] == ev && s_ev_code[s_nev - 1] == code; }

/* ---------------------------------------------------------------- 握手 */

static void t_handshake(void)
{
    fresh();
    CHECK(S.state == ZH_SES_DOWN && submit_id(ZH_EZSP_ID_GET_NODE_ID, 1) == ZH_SES_E_NOT_READY, "DOWN 时不收命令");
    zh_ses_link_up(&S, s_now);
    const uint8_t legacy[] = { 0x00, 0x00, 0x00, 0x12 };
    CHECK(s_nsent == 1 && s_sent_len[0] == 4 && !memcmp(s_sent[0], legacy, 4), "连上后首条是旧式头 version(0x12)：00 00 00 12");
    CHECK(S.state == ZH_SES_HANDSHAKE && submit_id(ZH_EZSP_ID_GET_NODE_ID, 1) == ZH_SES_E_NOT_READY, "握手中不收命令");
    ack();
    const uint8_t v[] = { 0x00, 0x80, 0x00, 0x12, 0x02, 0x34, 0x12 };
    zh_ses_rx(&S, v, sizeof(v), s_now);
    CHECK(S.state == ZH_SES_READY && s_nev == 1 && last_ev(ZH_SES_EV_READY, (2u << 16) | 0x1234), "旧式头的应答、协议 0x12 → READY，code = stackType<<16 | stackVersion");
    CHECK(s_nres == 0, "握手的结果不交上层");

    fresh();
    zh_ses_link_up(&S, s_now);
    ack();
    const uint8_t pv[] = { 0x12, 0x02, 0x00, 0x08 };
    ncp(0, 0x80, ZH_EZSP_ID_VERSION, pv, 4);
    CHECK(S.state == ZH_SES_READY, "扩展头的 version 应答也认");

    /* 协议号不对：停住 */
    fresh();
    zh_ses_link_up(&S, s_now);
    ack();
    const uint8_t old[] = { 0x00, 0x80, 0x00, 0x0D, 0x02, 0x00, 0x07 };
    zh_ses_rx(&S, old, sizeof(old), s_now);
    CHECK(S.state == ZH_SES_STOPPED && s_nev == 1 && last_ev(ZH_SES_EV_VERSION_MISMATCH, 0x0D), "协议 0x0D → MISMATCH、停住");
    at(s_now + 60000);
    CHECK(s_nsent == 1 && submit_id(ZH_EZSP_ID_GET_NODE_ID, 1) == ZH_SES_E_NOT_READY && zh_ses_next_timeout_ms(&S, s_now) == UINT32_MAX,
          "停住后什么都不发、不收命令（连保活都不发），没有定时器");

    /* 握手无应答：超时一次就复位链路，不重发 */
    fresh();
    zh_ses_link_up(&S, s_now);
    ack();
    at(s_now + 1999);
    CHECK(S.state == ZH_SES_HANDSHAKE, "握手应答等 1999 ms：还等");
    at(s_now + 1);
    CHECK(S.state == ZH_SES_STOPPED && s_nsent == 1 && last_ev(ZH_SES_EV_RESET_LINK, ZH_SES_WHY_HANDSHAKE), "握手超时 → RESET_LINK(HANDSHAKE)，不重发");
    /* 握手应答长度不对：当没收到 */
    fresh();
    zh_ses_link_up(&S, s_now);
    ack();
    const uint8_t shortv[] = { 0x00, 0x80, 0x00, 0x12, 0x02, 0x34 };
    zh_ses_rx(&S, shortv, sizeof(shortv), s_now);
    CHECK(S.state == ZH_SES_HANDSHAKE && S.st.rejected == 1, "握手应答少一个字节：不算 READY、记 rejected");
    /* 握手被 invalidCommand / 截断：停住报 HANDSHAKE（不能卡在 HANDSHAKE 里没有定时器） */
    fresh();
    zh_ses_link_up(&S, s_now);
    ack();
    const uint8_t r31[] = { 0x31 };
    ncp(0, 0x80, ZH_EZSP_ID_INVALID_COMMAND, r31, 1);
    CHECK(S.state == ZH_SES_STOPPED && last_ev(ZH_SES_EV_RESET_LINK, ZH_SES_WHY_HANDSHAKE), "握手的 version 被 invalidCommand → RESET_LINK(HANDSHAKE)");
    fresh();
    zh_ses_link_up(&S, s_now);
    ack();
    ncp(0, 0x82, ZH_EZSP_ID_VERSION, NULL, 0);
    CHECK(S.state == ZH_SES_STOPPED && last_ev(ZH_SES_EV_RESET_LINK, ZH_SES_WHY_HANDSHAKE), "握手的 version 被截断 → RESET_LINK(HANDSHAKE)");
    /* 握手被回显：version 内部当幂等，重发（还是旧式头、换 seq）；重发用完 → RESET_LINK(ECHOES) */
    fresh();
    zh_ses_link_up(&S, s_now);
    for (int i = 0; i < 3; i++) {
        ack();
        const uint8_t echo[] = { last_seq(), 0x00, 0x00, 0x12 };
        zh_ses_rx(&S, echo, 4, s_now);
        if (i == 0) CHECK(s_nsent == 2 && s_sent_len[1] == 4 && s_sent[1][0] == 1 && s_sent[1][2] == 0x00, "握手被回显：重发旧式头 version、seq 换成 1");
    }
    CHECK(S.state == ZH_SES_STOPPED && s_nsent == 3 && last_ev(ZH_SES_EV_RESET_LINK, ZH_SES_WHY_ECHOES), "握手连续回显、重发 2 次用完 → RESET_LINK(ECHOES)");
}

/* ---------------------------------------------------------------- submit */

static void t_submit_basic(void)
{
    fresh();
    up();
    uint8_t b[16];
    size_t n = zh_ezsp_enc_get_node_id(b, sizeof(b), 0);
    CHECK(zh_ses_submit(NULL, b, n, 1, true, s_now) == ZH_SES_E_ARG && zh_ses_submit(&S, NULL, n, 1, true, s_now) == ZH_SES_E_ARG &&
          zh_ses_submit(&S, b, 4, 1, true, s_now) == ZH_SES_E_ARG, "NULL / 短于扩展头 → E_ARG");
    uint8_t rsp[8] = { 0, 0x80, 0x01, 0x27, 0x00 };
    CHECK(zh_ses_submit(&S, rsp, 5, 1, true, s_now) == ZH_SES_E_ARG, "方向是应答的帧 → E_ARG");
    uint8_t lg5[5] = { 0, 0, 0, 0x12, 0 };
    CHECK(zh_ses_submit(&S, lg5, 5, 1, true, s_now) == ZH_SES_E_ARG, "旧式头 → E_ARG（只有会话自己发首条 version）");
    uint8_t cb[8];
    size_t cn = zh_ezsp_enc_callback(cb, sizeof(cb), 0);
    CHECK(zh_ses_submit(&S, cb, cn, 1, true, s_now) == ZH_SES_E_ARG, "callback 是会话自己的 → E_ARG（上层发了只会配不上、超时复位）");
    cb[3] = (uint8_t)ZH_EZSP_ID_NO_CALLBACKS;
    CHECK(zh_ses_submit(&S, cb, cn, 1, true, s_now) == ZH_SES_E_ARG, "noCallbacks 帧 ID → E_ARG");

    CHECK(zh_ses_submit(&S, b, n, 7, true, s_now) == ZH_SES_OK && s_nsent == 2 && last_seq() == 1 && last_id() == ZH_EZSP_ID_GET_NODE_ID,
          "submit 立即交给 ASH，seq 改写成 1（握手用了 0）");
    CHECK(!zh_ses_can_submit(&S) && submit_id(ZH_EZSP_ID_NETWORK_STATE, 8) == ZH_SES_E_BUSY, "上层同时只能一条：第二条 BUSY");
    ack();
    ncp(1, 0x80, ZH_EZSP_ID_GET_NODE_ID, k_nid, 2);
    CHECK(s_nres == 1 && s_res[0].tag == 7 && s_res[0].r == ZH_SES_R_OK && s_res[0].has_fr && s_res[0].id == ZH_EZSP_ID_GET_NODE_ID, "应答 → R_OK，带帧");
    CHECK(zh_ses_can_submit(&S), "了结后可以再 submit");
}

static void t_stale_and_mismatch(void)
{
    fresh();
    up();
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 1);
    ack();
    ncp(9, 0x80, ZH_EZSP_ID_GET_NODE_ID, k_nid, 2);
    CHECK(s_nres == 0 && S.st.stale == 1, "seq 不对 → stale，不交");
    ncp(1, 0x80, ZH_EZSP_ID_NETWORK_STATE, k_nid, 1);
    CHECK(s_nres == 0 && S.st.id_mismatch == 1, "seq 对、帧 ID 不对 → id_mismatch，不交");
    ncp(1, 0x80, ZH_EZSP_ID_GET_NODE_ID, k_nid, 2);
    CHECK(s_nres == 1 && s_res[0].r == ZH_SES_R_OK, "随后正确的应答照收");
    ncp(1, 0x80, ZH_EZSP_ID_GET_NODE_ID, k_nid, 2);
    CHECK(s_nres == 1 && S.st.stale == 2, "没有在途时来的应答 → stale");
    const uint8_t lg[] = { 0x05, 0x80, 0x00, 0x12, 0x02, 0x34, 0x12 };
    zh_ses_rx(&S, lg, sizeof(lg), s_now);
    CHECK(S.st.rejected == 1 && s_nres == 1 && s_ncb == 0, "握手之后的旧式头帧 → rejected");
    /* seq 恰好对上在途那条的旧式头帧（第 2 字节 0x27 的格式位是 3 → 旧式头、帧 ID 0x27 = getNodeId）：也拒 */
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 2);
    ack();
    const uint8_t lg2[] = { last_seq(), 0x80, 0x27, 0x34, 0x12 };
    zh_ses_rx(&S, lg2, sizeof(lg2), s_now);
    CHECK(S.st.rejected == 2 && s_nres == 1 && S.cur.used, "seq 对得上的旧式头帧也不当应答（旧式头只给握手）");
    ncp(last_seq(), 0x80, ZH_EZSP_ID_GET_NODE_ID, k_nid, 2);
    const uint8_t sh[] = { 0x05, 0x80 };
    zh_ses_rx(&S, sh, 2, s_now);
    const uint8_t rs[] = { 0x05, 0x80, 0x05, 0x27, 0x00 };
    zh_ses_rx(&S, rs, 5, s_now);
    CHECK(S.st.rejected == 4, "短帧、保留位 → rejected");
}

/* ---------------------------------------------------------------- 定时器 */

static void t_timers(void)
{
    /* C15：应答计时从 ASH 确认之后才起算；超时一次就复位链路、不重发（幂等的也不） */
    fresh();
    up();
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 1);
    const uint32_t t0 = s_now;
    at(t0 + 5000);
    CHECK(s_nres == 0 && s_nsent == 2 && S.st.timeouts == 0, "ASH 没确认时，过了 5 s 也不算应答超时（ASH 还在纠错）");
    s_unacked = 0;
    at(t0 + 5000);
    at(t0 + 6999);
    CHECK(S.st.timeouts == 0, "确认后 1999 ms：还没超时");
    at(t0 + 7000);
    CHECK(S.st.timeouts == 1 && s_nsent == 2 && S.st.retries == 0 && s_nres == 1 && s_res[0].r == ZH_SES_R_TIMEOUT && S.state == ZH_SES_STOPPED &&
          last_ev(ZH_SES_EV_RESET_LINK, ZH_SES_WHY_RSP_TIMEOUT), "确认后 2000 ms 超时：getNodeId（幂等）也不重发，回 TIMEOUT、RESET_LINK(RSP_TIMEOUT)");

    /* 兜底①：交给 ASH 之后 20 s 还没确认 */
    fresh();
    up();
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 5);
    const uint32_t t1 = s_now;
    at(t1 + 19999);
    CHECK(S.state == ZH_SES_READY && zh_ses_next_timeout_ms(&S, s_now) == 1, "19999 ms 未确认：还等，下一拍最多 1 ms 后");
    at(t1 + 20000);
    CHECK(S.state == ZH_SES_STOPPED && s_nres == 1 && s_res[0].tag == 5 && s_res[0].r == ZH_SES_R_TIMEOUT && last_ev(ZH_SES_EV_RESET_LINK, ZH_SES_WHY_ACK_GUARD),
          "20 s 未确认 → 在途的回 TIMEOUT、RESET_LINK(ACK_GUARD)、停住");

    /* 兜底②：ASH 一直不收 —— 这个等待点也有时限，而且 next_timeout 不是 0（调用方不空转） */
    fresh();
    up();
    s_send_ok = false;
    CHECK(submit_id(ZH_EZSP_ID_LEAVE_NETWORK, 6) == ZH_SES_OK && s_nsent == 1, "ASH 不收：submit 照样接受、先挂着");
    CHECK(zh_ses_next_timeout_ms(&S, s_now) == ZH_SES_POLL_MS, "交不出去时下一拍 %d ms 后（不是 0）", ZH_SES_POLL_MS);
    const uint32_t t2 = s_now;
    at(t2 + 19999);
    CHECK(S.state == ZH_SES_READY && s_nres == 0, "19999 ms 交不出去：还等");
    at(t2 + 20000);
    CHECK(S.state == ZH_SES_STOPPED && s_nres == 1 && s_res[0].r == ZH_SES_R_LINK_DOWN && last_ev(ZH_SES_EV_RESET_LINK, ZH_SES_WHY_ACK_GUARD),
          "20 s 交不出去 → RESET_LINK(ACK_GUARD)；从没交出去的回 LINK_DOWN（确定没执行）");

    /* 非幂等的超时：同样回 TIMEOUT 并复位 */
    fresh();
    up();
    submit_id(ZH_EZSP_ID_LEAVE_NETWORK, 3);
    ack();
    at(s_now + 2000);
    CHECK(s_nsent == 2 && s_nres == 1 && s_res[0].r == ZH_SES_R_TIMEOUT && S.state == ZH_SES_STOPPED, "leaveNetwork 超时：不重发（Z59），回 TIMEOUT，复位链路");
}

/* ---------------------------------------------------------------- 回显 */

static void t_echo(void)
{
    fresh();
    up();
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 1);
    const uint8_t seq = last_seq();
    ack();
    ncp(seq, 0x00, ZH_EZSP_ID_GET_NODE_ID, NULL, 0);
    CHECK(S.st.echoes == 1 && s_nres == 0 && s_nsent == 3 && last_seq() == (uint8_t)(seq + 1), "幂等命令的回显 → 立即重发、换 seq");
    ack();
    ncp(last_seq(), 0x80, ZH_EZSP_ID_GET_NODE_ID, k_nid, 2);
    CHECK(s_nres == 1 && s_res[0].r == ZH_SES_R_OK, "重发那次的应答 → OK");
    fresh();
    up();
    submit_id(ZH_EZSP_ID_LEAVE_NETWORK, 2);
    ack();
    ncp(last_seq(), 0x00, ZH_EZSP_ID_LEAVE_NETWORK, NULL, 0);
    CHECK(s_nres == 1 && s_res[0].r == ZH_SES_R_UNKNOWN && !s_res[0].has_fr && s_nsent == 2 && S.st.unknown == 1,
          "非幂等命令的回显 → R_UNKNOWN（执行了、结果未知），不重发（Z59）");
    fresh();
    up();
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 1);
    ack();
    ncp((uint8_t)(last_seq() + 5), 0x00, ZH_EZSP_ID_GET_NODE_ID, NULL, 0);
    ncp(last_seq(), 0x00, ZH_EZSP_ID_NETWORK_STATE, NULL, 0);
    CHECK(S.st.echoes == 0 && S.st.rejected == 2 && s_nsent == 2, "seq 或 ID 对不上的命令帧 → rejected，不重发");
    fresh();
    up();
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 1);
    for (int i = 0; i < 3; i++) { ack(); ncp(last_seq(), 0x00, ZH_EZSP_ID_GET_NODE_ID, NULL, 0); }
    CHECK(s_nres == 1 && s_res[0].r == ZH_SES_R_UNKNOWN && s_nsent == 4 && S.state == ZH_SES_READY, "幂等命令连续回显：重发 2 次后回 UNKNOWN，会话照常");
}

/* ---------------------------------------------------------------- invalidCommand / 截断 */

static void t_invalid_truncated(void)
{
    fresh();
    up();
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 1);
    ack();
    const uint8_t r31[] = { 0x31 };
    ncp(last_seq(), 0x80, ZH_EZSP_ID_INVALID_COMMAND, r31, 1);
    CHECK(s_nres == 1 && s_res[0].r == ZH_SES_R_INVALID && s_res[0].reason == 0x31 && S.state == ZH_SES_READY, "invalidCommand(0x31) → R_INVALID，带原因码");
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 2);
    ack();
    const uint8_t r30[] = { ZH_SES_INVALID_VERSION_NOT_SET };
    ncp(last_seq(), 0x80, ZH_EZSP_ID_INVALID_COMMAND, r30, 1);
    CHECK(s_nres == 2 && s_res[1].r == ZH_SES_R_INVALID && s_res[1].has_fr && s_res[1].reason == ZH_SES_INVALID_VERSION_NOT_SET && S.state == ZH_SES_STOPPED &&
          last_ev(ZH_SES_EV_RESET_LINK, ZH_SES_WHY_VERSION_NOT_SET), "原因 VERSION_NOT_SET → R_INVALID（带帧与原因码）+ RESET_LINK（NCP 不记得握过手）");
    fresh();
    up();
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 1);
    ack();
    ncp(last_seq(), 0x82, ZH_EZSP_ID_GET_NODE_ID, NULL, 0);
    CHECK(s_nres == 1 && s_res[0].r == ZH_SES_R_TRUNCATED && S.st.truncated == 1, "截断 → R_TRUNCATED");
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 2);
    ack();
    ncp(last_seq(), 0x81, ZH_EZSP_ID_GET_NODE_ID, k_nid, 2);
    CHECK(s_nres == 2 && s_res[1].r == ZH_SES_R_OK && S.st.overflow == 1, "溢出位：照收，记 overflow");
    /* 截断 / invalidCommand 的应答带 pending 位：照样去取回调（不等下一次保活） */
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 3);
    ack();
    ncp(last_seq(), 0x86, ZH_EZSP_ID_GET_NODE_ID, NULL, 0);
    CHECK(s_nres == 3 && s_res[2].r == ZH_SES_R_TRUNCATED && last_id() == ZH_EZSP_ID_CALLBACK, "截断的应答带 pending → 接着发 callback");
    ack();
    ncp(last_seq(), 0x88, ZH_EZSP_ID_NO_CALLBACKS, NULL, 0);
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 4);
    ack();
    ncp(last_seq(), 0x84, ZH_EZSP_ID_INVALID_COMMAND, r31, 1);
    CHECK(s_nres == 4 && s_res[3].r == ZH_SES_R_INVALID && last_id() == ZH_EZSP_ID_CALLBACK, "invalidCommand 带 pending → 接着发 callback");
}

/* ---------------------------------------------------------------- 回调 */

static void t_async(void)
{
    const uint8_t st4[] = { 0x90, 0, 0, 0 };
    fresh();
    up();
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 1);
    ack();
    ncp(0x55, 0x90, ZH_EZSP_ID_STACK_STATUS_HANDLER, st4, 4);
    CHECK(s_ncb == 1 && S.cur.used && s_nres == 0, "异步回调（fc 0x90）在途时照交，在途的不受影响");
    /* seq 恰好等于在途那条：仍是回调，不是应答 */
    ncp(last_seq(), 0x90, ZH_EZSP_ID_STACK_STATUS_HANDLER, st4, 4);
    CHECK(s_ncb == 2 && S.cur.used && s_nres == 0 && S.st.id_mismatch == 0, "seq 等于在途 seq 的异步回调：照交回调，不当应答");
    /* 截断的异步回调、seq 等于在途那条：不许把在途命令判成截断 */
    ncp(last_seq(), 0x92, ZH_EZSP_ID_STACK_STATUS_HANDLER, NULL, 0);
    CHECK(s_nres == 0 && S.cur.used && S.st.truncated == 1 && s_ncb == 2, "截断的异步回调：只计数丢掉，在途命令不受影响");
    ncp(last_seq(), 0x80, ZH_EZSP_ID_GET_NODE_ID, k_nid, 2);
    CHECK(s_nres == 1 && s_res[0].r == ZH_SES_R_OK, "随后在途的应答照收");
    const int n1 = s_nsent;
    at(s_now + 9000);
    ncp(0x56, 0x90, ZH_EZSP_ID_STACK_STATUS_HANDLER, st4, 4);
    at(s_now + 9000);
    CHECK(s_nsent == n1, "收到合法回调就重新计保活");
    /* 异步回调带 pending：空闲时去取 */
    ncp(0x57, 0x94, ZH_EZSP_ID_STACK_STATUS_HANDLER, st4, 4);
    CHECK(last_id() == ZH_EZSP_ID_CALLBACK, "异步回调带 pending → 发 callback");
}

static void t_callbacks(void)
{
    const uint8_t st4[] = { 0x90, 0, 0, 0 };
    fresh();
    up();
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 1);
    ack();
    ncp(last_seq(), 0x84, ZH_EZSP_ID_GET_NODE_ID, k_nid, 2);
    CHECK(s_nres == 1 && last_id() == ZH_EZSP_ID_CALLBACK, "应答带 pending 位 → 接着发 callback");
    ack();
    ncp(last_seq(), 0x8C, ZH_EZSP_ID_STACK_STATUS_HANDLER, st4, 4);
    CHECK(s_ncb == 1 && s_cb_id[0] == ZH_EZSP_ID_STACK_STATUS_HANDLER && last_id() == ZH_EZSP_ID_CALLBACK, "同步回调交 on_callback；它也带 pending → 再取");
    ack();
    ncp(last_seq(), 0x88, ZH_EZSP_ID_NO_CALLBACKS, NULL, 0);     /* 真 NCP 的 noCallbacks 很可能带 fc 0x08 */
    const int nn = s_nsent;
    at(s_now + 100);
    CHECK(s_nsent == nn && !S.cur.used && s_nres == 1 && s_ncb == 1 && S.st.callbacks == 1, "noCallbacks（fc 0x88）→ 结束、不当回调交上层、不再取");
    /* fc 0x80 的 noCallbacks 也一样 */
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 2);
    ack();
    ncp(last_seq(), 0x84, ZH_EZSP_ID_GET_NODE_ID, k_nid, 2);
    ack();
    ncp(last_seq(), 0x80, ZH_EZSP_ID_NO_CALLBACKS, NULL, 0);
    CHECK(!S.cur.used && s_ncb == 1, "noCallbacks（fc 0x80）→ 结束、不交上层");
    /* noCallbacks 带参数（长度不对）：当没收到 */
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 3);
    ack();
    ncp(last_seq(), 0x84, ZH_EZSP_ID_GET_NODE_ID, k_nid, 2);
    ack();
    const uint8_t junk[] = { 1 };
    ncp(last_seq(), 0x88, ZH_EZSP_ID_NO_CALLBACKS, junk, 1);
    CHECK(S.cur.used && S.st.rejected == 1, "noCallbacks 多一个字节：当没收到");
    ncp(last_seq(), 0x88, ZH_EZSP_ID_NO_CALLBACKS, NULL, 0);
    /* 过期帧上的 pending 位不作数 */
    ncp(0, 0x84, 0xFFFF, NULL, 0);
    CHECK(!S.cb_wanted, "过期帧上的 pending 位不作数");
    /* callback 的应答被回显吞掉：记 lost_callbacks、重发 */
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 4);
    ack();
    ncp(last_seq(), 0x84, ZH_EZSP_ID_GET_NODE_ID, k_nid, 2);
    ack();
    ncp(last_seq(), 0x00, ZH_EZSP_ID_CALLBACK, NULL, 0);
    CHECK(S.st.lost_callbacks == 1 && last_id() == ZH_EZSP_ID_CALLBACK && S.st.retries == 1, "callback 被回显：记丢了一条回调、重发");
    /* poll_callbacks=false */
    zh_ses_cfg_t c = test_cfg();
    c.poll_callbacks = false;
    reset_rec();
    zh_ses_init(&S, &c, &k_ops);
    up();
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 5);
    ack();
    ncp(last_seq(), 0x84, ZH_EZSP_ID_GET_NODE_ID, k_nid, 2);
    CHECK(last_id() == ZH_EZSP_ID_GET_NODE_ID, "poll_callbacks=false：pending 位不触发 callback");
}

/* 回调待取与上层命令轮流：谁也饿不死 */
static void t_fairness(void)
{
    const uint8_t st4[] = { 0x90, 0, 0, 0 };
    fresh();
    up();
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 1);
    ack();
    ncp(last_seq(), 0x84, ZH_EZSP_ID_GET_NODE_ID, k_nid, 2);
    CHECK(last_id() == ZH_EZSP_ID_CALLBACK, "A 的应答带 pending → callback");
    CHECK(submit_id(ZH_EZSP_ID_GET_NODE_ID, 2) == ZH_SES_OK && S.pend.used, "callback 在途时 submit B：排着");
    ack();
    ncp(last_seq(), 0x8C, ZH_EZSP_ID_STACK_STATUS_HANDLER, st4, 4);
    CHECK(last_id() == ZH_EZSP_ID_GET_NODE_ID && S.cur.tag == 2, "回调还带 pending、B 排着：这次轮到 B（回调刚轮过）");
    ack();
    ncp(last_seq(), 0x84, ZH_EZSP_ID_GET_NODE_ID, k_nid, 2);
    CHECK(s_nres == 2 && last_id() == ZH_EZSP_ID_CALLBACK, "B 了结（还带 pending）→ 回调再轮一次");
    /* 上层在结果回调里一条接一条 submit：回调也能轮到 */
    ack();
    ncp(last_seq(), 0x88, ZH_EZSP_ID_NO_CALLBACKS, NULL, 0);
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 3);
    s_on_result = RE_SUBMIT_ALWAYS;
    int k = 0;
    for (int i = 0; i < 8; i++) {
        ack();
        if (last_id() == ZH_EZSP_ID_CALLBACK) { s_order[k++] = 'c'; ncp(last_seq(), 0x8C, ZH_EZSP_ID_STACK_STATUS_HANDLER, st4, 4); }
        else { s_order[k++] = 'u'; ncp(last_seq(), 0x84, ZH_EZSP_ID_GET_NODE_ID, k_nid, 2); }
    }
    s_order[k] = 0;
    s_on_result = RE_NONE;
    CHECK(!strcmp(s_order, "ucucucuc"), "结果回调里不停 submit + 每个应答都带 pending：严格轮流（实为 %s）", s_order);
    /* poll_callbacks=false 时 pending 位不让 next_timeout 变 0（否则调用方空转） */
    zh_ses_cfg_t c = test_cfg();
    c.poll_callbacks = false;
    reset_rec();
    zh_ses_init(&S, &c, &k_ops);
    up();
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 9);
    ack();
    ncp(last_seq(), 0x84, ZH_EZSP_ID_GET_NODE_ID, k_nid, 2);
    CHECK(zh_ses_next_timeout_ms(&S, s_now) == 10000, "poll_callbacks=false：pending 位不让 next_timeout 变 0（到保活还有 10000 ms）");
}

static void t_keepalive(void)
{
    fresh();
    up();
    const int n0 = s_nsent;
    at(s_now + 9999);
    CHECK(s_nsent == n0, "空闲 9999 ms：不发保活");
    at(s_now + 1);
    CHECK(s_nsent == n0 + 1 && last_id() == ZH_EZSP_ID_NOP && S.st.keepalives == 1, "空闲 10 s → nop");
    CHECK(submit_id(ZH_EZSP_ID_GET_NODE_ID, 6) == ZH_SES_OK && s_nsent == n0 + 1, "内部 nop 在途时 submit：接受、先排着");
    ack();
    ncp(last_seq(), 0x80, ZH_EZSP_ID_NOP, NULL, 0);
    CHECK(s_nres == 0 && s_nsent == n0 + 2 && last_id() == ZH_EZSP_ID_GET_NODE_ID, "nop 的应答不交上层；排着的那条随即发出");
    fresh();
    up();
    at(s_now + 10000);
    ack();
    const uint8_t junk[] = { 1 };
    ncp(last_seq(), 0x80, ZH_EZSP_ID_NOP, junk, 1);
    CHECK(S.st.rejected == 1 && S.cur.used, "nop 应答多一个字节：当没收到，还在途");
    const int nk = s_nsent;
    at(s_now + 2000);
    CHECK(S.state == ZH_SES_STOPPED && last_ev(ZH_SES_EV_RESET_LINK, ZH_SES_WHY_KEEPALIVE) && s_nres == 0 && s_nsent == nk && S.st.retries == 0,
          "保活 nop 超时 → RESET_LINK(KEEPALIVE)，不重发、不产生上层结果");
    /* 保活失败时排着的上层命令回 LINK_DOWN（不悬着） */
    fresh();
    up();
    at(s_now + 10000);
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 7);
    ack();
    at(s_now + 2000);
    CHECK(S.state == ZH_SES_STOPPED && s_nres == 1 && s_res[0].tag == 7 && s_res[0].r == ZH_SES_R_LINK_DOWN, "停住时排着的上层命令回 LINK_DOWN");
}

/* ---------------------------------------------------------------- 重入、链路、清零、证据 */

static void t_reentrancy_and_link(void)
{
    fresh();
    up();
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 1);
    ack();
    s_on_result = RE_SUBMIT;
    ncp(last_seq(), 0x80, ZH_EZSP_ID_GET_NODE_ID, k_nid, 2);
    CHECK(s_nres == 1 && s_nsent == 3 && last_id() == ZH_EZSP_ID_GET_NODE_ID && S.cur.used && S.cur.tag == 99, "回调里 submit 的那条在回调返回后发出");
    ack();
    s_on_result = RE_LINK_DOWN;
    ncp(last_seq(), 0x80, ZH_EZSP_ID_GET_NODE_ID, k_nid, 2);
    CHECK(s_nres == 2 && S.state == ZH_SES_DOWN, "回调里 link_down：没有多余的结果");
    fresh();
    up();
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 3);
    ack();
    zh_ses_link_down(&S);
    CHECK(s_nres == 1 && s_state_in_cb[0] == ZH_SES_DOWN, "link_down 交结果时会话已经是 DOWN（回调里看到的是新状态）");

    /* link_down：交出去过的回 UNKNOWN（可能执行了），排着的回 LINK_DOWN */
    fresh();
    up();
    submit_id(ZH_EZSP_ID_LEAVE_NETWORK, 8);
    ack();
    zh_ses_link_down(&S);
    CHECK(s_nres == 1 && s_res[0].tag == 8 && s_res[0].r == ZH_SES_R_UNKNOWN && S.state == ZH_SES_DOWN, "link_down：已交给 ASH 的回 UNKNOWN（可能执行了，Z59）");
    zh_ses_link_down(&S);
    CHECK(s_nres == 1, "再 link_down 不重复");
    fresh();
    up();
    at(s_now + 10000);                       /* nop 在途 */
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 7);    /* 排着 */
    zh_ses_link_down(&S);
    CHECK(s_nres == 1 && s_res[0].tag == 7 && s_res[0].r == ZH_SES_R_LINK_DOWN, "link_down：排着（从没交出去）的回 LINK_DOWN（内部 nop 不交）");
    fresh();
    up();
    s_send_ok = false;
    submit_id(ZH_EZSP_ID_LEAVE_NETWORK, 9);
    zh_ses_link_down(&S);
    CHECK(s_nres == 1 && s_res[0].r == ZH_SES_R_LINK_DOWN, "link_down：在途但 ASH 一直没收下的回 LINK_DOWN");
    fresh();
    up();
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 8);
    zh_ses_link_up(&S, s_now);
    CHECK(s_nres == 1 && s_res[0].r == ZH_SES_R_UNKNOWN && S.state == ZH_SES_HANDSHAKE && last_seq() == 0 && s_sent_len[s_nsent - 1] == 4,
          "漏调 link_down 直接 link_up：交出去过的回 UNKNOWN，重新握手、seq 从 0");

    /* RESET_LINK 的正确处置：事件里 link_down → 复位 ASH → CONNECTED 之后（回调外）link_up */
    fresh();
    up();
    s_on_event = RE_LINK_DOWN;
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 9);
    ack();
    at(s_now + 2000);
    CHECK(s_nres == 1 && s_res[0].r == ZH_SES_R_TIMEOUT && S.state == ZH_SES_DOWN, "事件回调里 link_down：在途结果只交一次，会话 DOWN");
    zh_ses_link_up(&S, s_now);
    CHECK(S.state == ZH_SES_HANDSHAKE && s_sent_len[s_nsent - 1] == 4 && last_seq() == 0, "ASH 复位连上后 link_up：重新握手（旧式头、seq 0）");
    /* 回调里 link_up 被忽略：RESET_LINK 照报，version 不在没复位的链路上发 */
    fresh();
    up();
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 10);
    ack();
    s_on_result = RE_DOWN_UP;
    const int nev0 = s_nev, ns0 = s_nsent;
    at(s_now + 2000);
    CHECK(s_nres == 1 && s_res[0].r == ZH_SES_R_TIMEOUT && S.state == ZH_SES_DOWN && S.st.misuse == 1 && s_nsent == ns0 &&
          s_nev == nev0 + 1 && last_ev(ZH_SES_EV_RESET_LINK, ZH_SES_WHY_RSP_TIMEOUT),
          "结果回调里 link_down + link_up：link_up 被忽略（记 misuse）、不发 version、RESET_LINK 照报");

    /* ASH 收不下（队列满）：下一拍再交，seq 不跳 */
    fresh();
    up();
    s_send_ok = false;
    CHECK(submit_id(ZH_EZSP_ID_GET_NODE_ID, 1) == ZH_SES_OK && s_nsent == 1, "ASH 不收：先挂着");
    at(s_now + 50);
    s_send_ok = true;
    at(s_now + 50);
    CHECK(s_nsent == 2 && last_seq() == 1, "ASH 能收了就发，seq 没被吃掉（还是 1）");
    /* seq 回绕 */
    fresh();
    up();
    S.seq_next = 0xFF;
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 1);
    ack();
    ncp(0xFF, 0x80, ZH_EZSP_ID_GET_NODE_ID, k_nid, 2);
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 2);
    CHECK(s_nres == 1 && last_seq() == 0x00, "seq 0xFF 之后回到 0x00");
}

/* 会话的内存里找不到这 16 个字节 */
static bool key_in_session(const uint8_t key[16])
{
    const uint8_t *m = (const uint8_t *)&S;
    for (size_t i = 0; i + 16 <= sizeof(S); i++)
        if (!memcmp(m + i, key, 16)) return true;
    return false;
}

static void t_scrub_and_evidence(void)
{
    uint8_t key[16];
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)(0xA0 + i);
    zh_ezsp_init_sec_t sec;
    memset(&sec, 0, sizeof(sec));
    memcpy(sec.network_key, key, 16);
    uint8_t b[ZH_EZSP_MAX];
    const size_t n = zh_ezsp_enc_set_initial_security_state(b, sizeof(b), 0, &sec);
    /* ① 了结（OK） */
    fresh();
    up();
    CHECK(zh_ses_submit(&S, b, n, 1, false, s_now) == ZH_SES_OK && key_in_session(key), "（对照）在途时会话里有这份密钥");
    ack();
    const uint8_t st0[4] = { 0 };
    ncp(last_seq(), 0x80, ZH_EZSP_ID_SET_INITIAL_SECURITY_STATE, st0, 4);
    CHECK(s_nres == 1 && !key_in_session(key), "setInitialSecurityState 了结后，会话里找不到网络密钥（Z17）");
    /* ② 排着时 link_down */
    fresh();
    up();
    at(s_now + 10000);
    zh_ses_submit(&S, b, n, 2, false, s_now);
    zh_ses_link_down(&S);
    CHECK(!key_in_session(key), "排着时 link_down：密钥清掉");
    /* ②' 排着（内部 nop 在途）→ 轮到它上、了结：移走时排队那份也清掉 */
    fresh();
    up();
    at(s_now + 10000);
    zh_ses_submit(&S, b, n, 5, false, s_now);
    ack();
    ncp(last_seq(), 0x80, ZH_EZSP_ID_NOP, NULL, 0);
    ack();
    ncp(last_seq(), 0x80, ZH_EZSP_ID_SET_INITIAL_SECURITY_STATE, st0, 4);
    CHECK(s_nres == 1 && s_res[0].r == ZH_SES_R_OK && !key_in_session(key), "排在 nop 后面的那条上了又了结：在途与排队两份都清掉");
    /* ③ 超时停住 */
    fresh();
    up();
    zh_ses_submit(&S, b, n, 3, false, s_now);
    ack();
    at(s_now + 2000);
    CHECK(S.state == ZH_SES_STOPPED && !key_in_session(key), "超时停住：密钥清掉");
    /* ④ 回显 → UNKNOWN */
    fresh();
    up();
    zh_ses_submit(&S, b, n, 4, false, s_now);
    ack();
    ncp(last_seq(), 0x00, ZH_EZSP_ID_SET_INITIAL_SECURITY_STATE, NULL, 0);
    CHECK(s_nres == 1 && s_res[0].r == ZH_SES_R_UNKNOWN && !key_in_session(key), "回显了结：密钥清掉");

    /* 正向证据：只认配上的应答与合法回调；建链、超时了结都不算 */
    fresh();
    uint32_t t = 0;
    CHECK(!zh_ses_alive_evidence(&S, &t), "从没收到应答：没有证据");
    zh_ses_link_up(&S, s_now);
    CHECK(!zh_ses_alive_evidence(&S, &t), "只是建链：没有证据");
    ack();
    const uint8_t v[] = { 0x00, 0x80, 0x00, 0x12, 0x02, 0x34, 0x12 };
    s_now = 1500;
    zh_ses_rx(&S, v, sizeof(v), s_now);
    CHECK(zh_ses_alive_evidence(&S, &t) && t == 1500, "握手应答 → 证据时刻 1500");
    s_now = 1700;
    submit_id(ZH_EZSP_ID_LEAVE_NETWORK, 1);
    ack();
    ncp((uint8_t)(last_seq() + 3), 0x80, ZH_EZSP_ID_LEAVE_NETWORK, NULL, 0);
    ncp(last_seq(), 0x00, ZH_EZSP_ID_LEAVE_NETWORK, NULL, 0);     /* 回显 → UNKNOWN 了结（刷新 t_idle） */
    CHECK(s_nres == 1 && s_res[0].r == ZH_SES_R_UNKNOWN && S.t_idle == 1700 && zh_ses_alive_evidence(&S, &t) && t == 1500,
          "过期应答、回显都不算证据；UNKNOWN 了结刷新了 t_idle，证据时刻不动");
    s_now = 1800;
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 2);
    ack();
    at(s_now + 2000);
    CHECK(S.state == ZH_SES_STOPPED && zh_ses_alive_evidence(&S, &t) && t == 1500, "应答超时（停住）不算证据");
    fresh();
    up();
    s_now = 2500;
    const uint8_t st4[] = { 0x90, 0, 0, 0 };
    ncp(0x40, 0x90, ZH_EZSP_ID_STACK_STATUS_HANDLER, st4, 4);
    CHECK(zh_ses_alive_evidence(&S, &t) && t == 2500, "合法的异步回调算证据");
}

static void t_next_timeout_and_idem(void)
{
    fresh();
    CHECK(zh_ses_next_timeout_ms(&S, s_now) == UINT32_MAX, "DOWN：没有定时器");
    up();
    CHECK(zh_ses_next_timeout_ms(&S, s_now) == 10000, "READY 空闲：到保活还有 10000 ms");
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 1);
    CHECK(zh_ses_next_timeout_ms(&S, s_now) == ZH_SES_POLL_MS, "已交给 ASH、未确认：%d ms 后回来看 unacked", ZH_SES_POLL_MS);
    ack();
    CHECK(zh_ses_next_timeout_ms(&S, s_now) == 2000, "确认后：应答超时还剩 2000 ms");
    s_send_ok = false;
    ncp(last_seq(), 0x84, ZH_EZSP_ID_GET_NODE_ID, k_nid, 2);
    CHECK(zh_ses_next_timeout_ms(&S, s_now) == ZH_SES_POLL_MS, "callback 交不出去：%d ms 后再试（不空转）", ZH_SES_POLL_MS);

    const uint16_t yes[] = { ZH_EZSP_ID_VERSION, ZH_EZSP_ID_GET_VALUE, ZH_EZSP_ID_NOP, ZH_EZSP_ID_SET_CONFIGURATION_VALUE, ZH_EZSP_ID_SET_POLICY,
                             ZH_EZSP_ID_PERMIT_JOINING, ZH_EZSP_ID_SET_RADIO_POWER, ZH_EZSP_ID_GET_NETWORK_PARAMETERS, ZH_EZSP_ID_CLEAR_TRANSIENT_LINK_KEYS };
    const uint16_t no[] = { ZH_EZSP_ID_SEND_UNICAST, ZH_EZSP_ID_SEND_BROADCAST, ZH_EZSP_ID_FORM_NETWORK, ZH_EZSP_ID_LEAVE_NETWORK,
                            ZH_EZSP_ID_SET_INITIAL_SECURITY_STATE, ZH_EZSP_ID_ADD_ENDPOINT, ZH_EZSP_ID_NETWORK_INIT, ZH_EZSP_ID_START_SCAN,
                            ZH_EZSP_ID_CUSTOM_FRAME, ZH_EZSP_ID_SET_VALUE, ZH_EZSP_ID_CALLBACK, 0x0123 };
    int bad = 0;
    for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++) bad += !zh_ses_idempotent(yes[i]);
    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++) bad += zh_ses_idempotent(no[i]);
    CHECK(bad == 0, "幂等表：读取 / 设值类可重发；发报文、建网、离网、密钥、加端点、扫描、customFrame、setValue、callback、不认识的都不重发（%d 处不对）", bad);
}

static void t_defaults(void)
{
    zh_ses_cfg_t c;
    zh_ses_cfg_default(&c);
    zh_ash_cfg_t a;
    zh_ash_cfg_default(&a);
    CHECK(c.rsp_timeout_ms == 15000 && c.rsp_timeout_ms > 4 * 3200,
          "应答超时 15 s：长过 NCP 侧 ASH 单帧最坏 4 × 3200 ms（ash-ncp.h ASH_TIME_DATA_MAX、ASH_MAX_TIMEOUTS）");
    CHECK(c.ack_guard_ms > (uint32_t)a.max_timeouts * a.ack_max_ms, "兜底 %u ms 长过主机侧 zh_ash 最坏 %u × %u ms（从 zh_ash_cfg_default 推）",
          c.ack_guard_ms, (unsigned)a.max_timeouts, (unsigned)a.ack_max_ms);
    CHECK(c.keepalive_ms == 10000 && c.max_retries == 1 && c.poll_callbacks, "保活 10 s；回显后重发 1 次；取同步回调");
    /* 夹住越界的配置 */
    zh_ses_cfg_t bad = c;
    bad.max_retries = 200;
    bad.rsp_timeout_ms = 0;
    bad.ack_guard_ms = 0;
    reset_rec();
    zh_ses_init(&S, &bad, &k_ops);
    CHECK(S.cfg.max_retries == ZH_SES_MAX_RETRIES_CAP && S.cfg.rsp_timeout_ms == 15000 && S.cfg.ack_guard_ms == 20000, "越界的配置被夹住");
    /* 默认参数下：确认后 14999 ms 不超时、15000 ms 超时并复位（防止有人改回短超时） */
    reset_rec();
    zh_ses_init(&S, NULL, &k_ops);
    up();
    submit_id(ZH_EZSP_ID_GET_NODE_ID, 1);
    ack();
    const uint32_t t = s_now;
    at(t + 14999);
    CHECK(S.st.timeouts == 0, "默认参数：确认后 14999 ms 还在等");
    at(t + 15000);
    CHECK(S.st.timeouts == 1 && S.state == ZH_SES_STOPPED && s_nres == 1 && s_res[0].r == ZH_SES_R_TIMEOUT && last_ev(ZH_SES_EV_RESET_LINK, ZH_SES_WHY_RSP_TIMEOUT),
          "默认参数：15000 ms 超时 → TIMEOUT + RESET_LINK");
}

int main(void)
{
    t_handshake();
    t_submit_basic();
    t_stale_and_mismatch();
    t_timers();
    t_echo();
    t_invalid_truncated();
    t_async();
    t_callbacks();
    t_fairness();
    t_keepalive();
    t_reentrancy_and_link();
    t_scrub_and_evidence();
    t_next_timeout_and_idem();
    t_defaults();
    printf("zh_ses_test：%d 通过，%d 失败\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
