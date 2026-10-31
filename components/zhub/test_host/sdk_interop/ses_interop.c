/* zh_ezsp_ses + zh_ash_core（我们的主机侧，产品源文件原样编）对接 SDK 的真 NCP 侧 ASH（ash-ncp.c，经 libsdkncp.so）
 * 和测试里的 EZSP 应用层替身 —— docs/15 设计 8.4 收发规则、8.13；CHECKLIST Z57 Z58 Z59 C15。
 *
 * 替身像 em260 一样把应答**原地写回命令缓冲**（ncp_stub.c），所以噪声下会自然出现"主机自己的命令被当应答发回来"（回显）。
 * 它按命令里带的编号记"执行了几次"：非幂等命令用 sendUnicast（messageTag = 编号），幂等命令用 lookupEui64ByNodeId（nodeId = 编号），
 * 应答内容由编号推出，主机侧逐个核对。
 *   E1 上电握手：NCP 第一条收到的是旧式头 version，READY
 *   E2 干净信道 300 条混合命令：全部 OK、内容对、每条恰好执行一次、零重发零回显零超时
 *   E3 噪声信道 + 应用层随机不回（5%）：每条命令都有结论；**非幂等命令没有一条被执行两次**（Z59）；OK 的执行过、LINK_DOWN 的确定没执行
 *   E3d 极端噪声（每字节 2%）：链路反复复位，判据同 E3；NCP 替身把"同一块缓冲重复入队"（原地应答撞缓冲）当硬错误
 *   E3b 强制回显（确定性）：吞掉 NCP 的应答直到主机 ASH 重传命令，重传的命令覆盖 NCP 还没发完的应答 → NCP 把命令当应答发回来。
 *      幂等命令 → 会话重发、OK（执行 2 次）；非幂等命令 → UNKNOWN（执行恰好 1 次，Z59）
 *   E3c NCP 已确认收到命令、应答在路上丢了 2 s（只能等 NCP 侧 ASH 自己重传送到）：会话不先放弃（C15 的 NCP→主机方向）；
 *      放弃了就会发下一条，把新命令写进 NCP 还等着重传旧应答的那块缓冲（em260 原地应答）
 *   替身按长度严格收命令：长度不对回 invalidCommand（主机→NCP 方向同样有 CRC 结构性漏洞：丢了 0x00 尾字节的帧 CRC 仍对）
 *   E4 空闲保活：60 s 空闲发了 nop、NCP 都收到、不产生上层结果
 *   E5 应用层挂死但 ASH 还在确认（审计复现过的活性漏洞）：保活超时 → RESET_LINK(KEEPALIVE) → 复位 → NCP 重启 → 重新 READY，有时限
 *   E6 NCP 整个不响应：在途命令在时限内有结论，之后反复复位、不悬着
 *   E7 同步回调：应答带"还有回调待取"，会话发 callback 取，K 条按序、恰好一次
 *   E8 异步回调夹在命令中间：全部按序、恰好一次，命令照常
 * 用法：ses_interop <libsdkncp.so 的路径> [随机种子] */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zh_ash_core.h"
#include "zh_ezsp_core.h"
#include "zh_ezsp_ses.h"

static int s_fail, s_pass;
#define CHECK(cond, ...) do { if (cond) { s_pass++; } else { s_fail++; printf("❌ %s:%d ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- libsdkncp.so ---- */
static const char *s_path;
static void *s_so;
static void     (*ncp_boot)(uint8_t);
static void     (*ncp_set_time)(uint32_t);
static void     (*ncp_feed)(const uint8_t *, size_t);
static size_t   (*ncp_drain)(uint8_t *, size_t);
static int      (*ncp_poll)(void);
static size_t   (*ncp_take_cmd)(uint8_t *);
static void     (*ncp_send)(const uint8_t *, size_t, bool);
static void     (*ncp_reply)(const uint8_t *, size_t);
static void     (*ncp_set_inplace)(bool);
static uint32_t (*ncp_cmd_count)(void);

static void *sym(const char *n)
{
    void *p = dlsym(s_so, n);
    if (!p) { printf("❌ libsdkncp.so 里没有 %s\n", n); exit(1); }
    return p;
}
static void load(void)
{
    if (s_so) dlclose(s_so);
    s_so = dlopen(s_path, RTLD_NOW | RTLD_LOCAL);
    if (!s_so) { printf("❌ dlopen %s: %s\n", s_path, dlerror()); exit(1); }
    ncp_boot = sym("ncp_boot"); ncp_set_time = sym("ncp_set_time"); ncp_feed = sym("ncp_feed"); ncp_drain = sym("ncp_drain");
    ncp_poll = sym("ncp_poll"); ncp_take_cmd = sym("ncp_take_cmd"); ncp_send = sym("ncp_send"); ncp_reply = sym("ncp_reply");
    ncp_set_inplace = sym("ncp_set_inplace"); ncp_cmd_count = sym("ncp_cmd_count");
    if (ncp_cmd_count() != 0) { printf("❌ 重新加载后 NCP 的静态变量没清零\n"); exit(1); }
}

static uint32_t s_rng = 0x2468ACE1;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

/* ---------------------------------------------------------------- 主机侧：zh_ash + zh_ezsp_ses */

#define MAXK 1024
static zh_ash_t A;
static zh_ses_t S;
static uint32_t s_now;
static bool     s_need_reset;
static int      s_ready, s_mismatch, s_reset_link[8], s_ash_ev[8];

static zh_ses_result_t s_res[MAXK];
static bool     s_has_res[MAXK];
static int      s_res_dup, s_res_bad, s_res_short;   /* short：R_OK 但参数解不开（少一个字节、CRC 仍对）—— 契约里由上层发现（zh_ezsp_ses.h），不算错 */
static bool     s_short[MAXK];
static uint16_t s_cb_seen[256];
static int      s_ncb, s_cb_order_bad;

static bool     h_send(void *ctx, const uint8_t *f, size_t n) { (void)ctx; return zh_ash_send(&A, f, n); }
static unsigned h_unacked(void *ctx) { (void)ctx; return zh_ash_queued(&A); }

/* 应答内容由编号推出：sendUnicast → 状态 0 + apsSeq = 编号低字节；lookup → 状态 0 + EUI64 = 编号 × 每字节 */
static void h_result(void *ctx, uint32_t tag, zh_ses_result_t r, const zh_ezsp_frame_t *fr)
{
    (void)ctx;
    if (tag >= MAXK) { s_res_bad++; return; }
    if (s_has_res[tag]) s_res_dup++;
    s_has_res[tag] = true;
    s_res[tag] = r;
    if (r != ZH_SES_R_OK) return;
    uint32_t st = 1;
    if (fr->id == ZH_EZSP_ID_SEND_UNICAST) {
        uint8_t q = 0;
        const int d = zh_ezsp_dec_send(fr, ZH_EZSP_ID_SEND_UNICAST, &st, &q);
        if (d == ZH_EZSP_E_LENGTH) { s_res_short++; s_short[tag] = true; return; }
        if (d != ZH_EZSP_OK || st != 0 || q != (uint8_t)tag) s_res_bad++;
    } else if (fr->id == ZH_EZSP_ID_LOOKUP_EUI64_BY_NODE_ID) {
        uint8_t e[8];
        const int d = zh_ezsp_dec_lookup_eui64(fr, &st, e);
        if (d == ZH_EZSP_E_LENGTH) { s_res_short++; s_short[tag] = true; return; }
        if (d != ZH_EZSP_OK || st != 0) { s_res_bad++; return; }
        for (int i = 0; i < 8; i++) if (e[i] != (uint8_t)(tag * (i + 1))) { s_res_bad++; break; }
    } else {
        s_res_bad++;
    }
}
static void h_callback(void *ctx, const zh_ezsp_frame_t *fr)
{
    (void)ctx;
    uint32_t st = 0;
    if (zh_ezsp_dec_stack_status(fr, &st) != ZH_EZSP_OK) { s_cb_order_bad++; return; }
    if (s_ncb < 256) s_cb_seen[s_ncb] = (uint16_t)st;
    if (s_ncb > 0 && st != (uint32_t)s_cb_seen[s_ncb - 1] + 1) s_cb_order_bad++;
    s_ncb++;
}
static void h_event(void *ctx, zh_ses_event_t ev, uint32_t code)
{
    (void)ctx;
    if (ev == ZH_SES_EV_READY) s_ready++;
    else if (ev == ZH_SES_EV_VERSION_MISMATCH) s_mismatch++;
    else if (ev == ZH_SES_EV_RESET_LINK) {
        if (code < 8) s_reset_link[code]++;
        zh_ses_link_down(&S);          /* 上层的处置：会话收摊、复位 ASH（下一步做），连上后再 link_up */
        s_need_reset = true;
    }
}
static void a_frame(void *ctx, const uint8_t *d, size_t n) { (void)ctx; zh_ses_rx(&S, d, n, s_now); }
static void a_event(void *ctx, zh_ash_event_t ev, uint8_t code)
{
    (void)ctx; (void)code;
    s_ash_ev[ev]++;
    if (ev == ZH_ASH_EV_CONNECTED) zh_ses_link_up(&S, s_now);
    else { zh_ses_link_down(&S); s_need_reset = true; }
}

/* ---------------------------------------------------------------- NCP 侧：SDK ash-ncp + EZSP 应用层替身 */

static double   s_noise;            /* 每字节出错的概率（一半丢、一半翻位） */
static double   s_app_drop;         /* 应用层收到命令却不回的概率 */
static bool     s_app_hung;         /* 应用层挂死：什么都不回（ASH 照样确认）；NCP 重启后恢复 */
static bool     s_ncp_dead;         /* 整个 NCP 不响应（连 ASH 都不回） */
static bool     s_sync_cb;          /* 回调走同步：应答带 pending 位，等主机来取 */
static uint32_t s_exec_uni[MAXK], s_exec_lookup[MAXK];
static uint32_t s_ncp_nops, s_ncp_versions, s_ncp_first_legacy, s_ncp_cmds_session, s_ncp_malformed;
static int      s_slow_tag = -1;    /* E3c：NCP 执行这条 sendUnicast 之后，吞掉 NCP→主机的字节 2 s（ACK 已在同一步先发出） */
static uint32_t s_drop_from, s_drop_until, s_slow_ms = 2000;
static bool     s_echo_gate;        /* E3b：吞掉 NCP→主机的字节，直到主机 ASH 重传了命令（再多吞 30 ms，让重传的命令落进 NCP） */
static uint32_t s_echo_redata0, s_echo_t;
static uint16_t s_cbq[256];
static int      s_cbq_head, s_cbq_tail;
static int      s_ncp_boots;

static size_t channel(uint8_t *b, size_t n)
{
    if (s_noise <= 0) return n;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        const double r = (double)(rnd() % 1000000) / 1e6;
        if (r < s_noise / 2) continue;
        b[o] = b[i];
        if (r < s_noise) b[o] ^= (uint8_t)(1u << (rnd() % 8));
        o++;
    }
    return o;
}

static uint8_t rsp_fc(void) { return (uint8_t)(0x80 | ((s_sync_cb && s_cbq_head != s_cbq_tail) ? 0x04 : 0)); }

static void app_reply(const uint8_t *r, size_t n) { ncp_reply(r, n); }

static void ncp_app_on_cmd(void)
{
    uint8_t c[256], r[256];
    const size_t n = ncp_take_cmd(c);
    if (n < 3) return;
    const bool legacy = (c[2] & 0x03) != 0x01;
    const uint16_t id = legacy ? c[2] : (n >= 5 ? (uint16_t)(c[3] | (c[4] << 8)) : 0xFFFF);
    if (s_ncp_cmds_session++ == 0 && legacy && id == ZH_EZSP_ID_VERSION) s_ncp_first_legacy++;
    if (s_app_hung) return;
    /* 按长度严格收：主机→NCP 方向同样有 CRC 结构性漏洞，长度不对的命令不执行，回 invalidCommand（INVALID_VALUE） */
    bool good = true;
    if (legacy) good = id == ZH_EZSP_ID_VERSION && n == 4;
    else if (id == ZH_EZSP_ID_NOP || id == ZH_EZSP_ID_CALLBACK) good = n == 5;
    else if (id == ZH_EZSP_ID_LOOKUP_EUI64_BY_NODE_ID) good = n == 7;
    else if (id == ZH_EZSP_ID_SEND_UNICAST) good = n >= 22 && n == 22u + c[21];
    if (!good) {
        s_ncp_malformed++;
        if (legacy || n < 5) return;
        const uint8_t inv[] = { c[0], 0x80, 0x01, (uint8_t)ZH_EZSP_ID_INVALID_COMMAND, 0x00, 0x36 };
        app_reply(inv, sizeof(inv));
        return;
    }
    if (id == ZH_EZSP_ID_VERSION) s_ncp_versions++;
    if (id == ZH_EZSP_ID_NOP) s_ncp_nops++;
    /* 执行计数在"决定回不回"之前：不回 = 执行了但应答没出来（比如 NCP 处理到一半） */
    if (id == ZH_EZSP_ID_SEND_UNICAST) {
        const uint16_t tag = (uint16_t)(c[5 + 1 + 2 + 11] | (c[5 + 1 + 2 + 11 + 1] << 8));
        if (tag < MAXK) s_exec_uni[tag]++;
        if (tag == s_slow_tag) { s_drop_from = s_now + 1; s_drop_until = s_now + s_slow_ms; s_slow_tag = -1; }
    }
    if (id == ZH_EZSP_ID_LOOKUP_EUI64_BY_NODE_ID) {
        const uint16_t k = (uint16_t)(c[5] | (c[6] << 8));
        if (k < MAXK) s_exec_lookup[k]++;
        if (k == s_slow_tag) { s_drop_from = s_now + 1; s_drop_until = s_now + s_slow_ms; s_slow_tag = -1; }
    }
    if (id != ZH_EZSP_ID_VERSION && s_app_drop > 0 && (double)(rnd() % 1000000) / 1e6 < s_app_drop) return;
    size_t m = 0;
    if (legacy && id == ZH_EZSP_ID_VERSION) {
        r[0] = c[0]; r[1] = 0x80; r[2] = 0x00; r[3] = 0x12; r[4] = 0x02; r[5] = 0x34; r[6] = 0x12;
        m = 7;
    } else if (legacy) {
        return;
    } else {
        r[0] = c[0]; r[1] = rsp_fc(); r[2] = 0x01; r[3] = c[3]; r[4] = c[4];
        m = 5;
        switch (id) {
        case ZH_EZSP_ID_NOP:
            break;
        case ZH_EZSP_ID_SEND_UNICAST: {
            const uint16_t tag = (uint16_t)(c[5 + 1 + 2 + 11] | (c[5 + 1 + 2 + 11 + 1] << 8));
            memset(r + 5, 0, 4);
            r[9] = (uint8_t)tag;
            m = 10;
            break;
        }
        case ZH_EZSP_ID_LOOKUP_EUI64_BY_NODE_ID: {
            const uint16_t k = (uint16_t)(c[5] | (c[6] << 8));
            memset(r + 5, 0, 4);
            for (int i = 0; i < 8; i++) r[9 + i] = (uint8_t)(k * (i + 1));
            m = 17;
            break;
        }
        case ZH_EZSP_ID_CALLBACK:
            if (s_cbq_head != s_cbq_tail) {             /* 应答就是一条回调（fc 0x08）：stackStatusHandler(序号) */
                const uint16_t v = s_cbq[s_cbq_head++ % 256];
                r[1] = 0x88;
                r[3] = (uint8_t)ZH_EZSP_ID_STACK_STATUS_HANDLER; r[4] = 0;
                r[5] = (uint8_t)v; r[6] = (uint8_t)(v >> 8); r[7] = 0; r[8] = 0;
                if (s_cbq_head != s_cbq_tail) r[1] |= 0x04;
                m = 9;
            } else {
                r[1] = 0x88;
                r[3] = (uint8_t)ZH_EZSP_ID_NO_CALLBACKS; r[4] = 0;
            }
            break;
        default:
            memset(r + 5, 0, 4);                        /* 别的：一个 4 B 状态 */
            m = 9;
            break;
        }
    }
    app_reply(r, m);
}

/* 异步回调：stackStatusHandler(序号)，fc 0x90 */
static void ncp_async_cb(uint16_t v)
{
    const uint8_t f[] = { 0x00, 0x90, 0x01, (uint8_t)ZH_EZSP_ID_STACK_STATUS_HANDLER, 0x00, (uint8_t)v, (uint8_t)(v >> 8), 0, 0 };
    ncp_send(f, sizeof(f), false);
}

static void ncp_reboot(uint8_t reason)
{
    load();
    ncp_set_inplace(true);
    ncp_set_time(s_now);
    ncp_boot(reason);
    s_ncp_boots++;
    s_ncp_cmds_session = 0;
    s_app_hung = false;               /* 应用层挂死靠重启恢复 */
    s_cbq_head = s_cbq_tail = 0;
}

static void step(void)
{
    s_now++;
    if (s_need_reset) { s_need_reset = false; zh_ash_reset(&A); }
    zh_ses_tick(&S, s_now);
    static uint8_t buf[8192];
    size_t n = zh_ash_tx(&A, s_now, buf, sizeof(buf));
    n = channel(buf, n);
    if (s_ncp_dead) return;
    ncp_set_time(s_now);
    if (n) ncp_feed(buf, n);
    const int r = ncp_poll();
    if (r == 2) ncp_reboot(ZH_ASH_RESET_SOFTWARE);
    else if (r == 1) ncp_app_on_cmd();
    n = ncp_drain(buf, sizeof(buf));
    n = channel(buf, n);
    if (s_now >= s_drop_from && s_now < s_drop_until) n = 0;
    if (s_echo_gate) {
        if (s_echo_t == 0 && A.st.tx_redata != s_echo_redata0) s_echo_t = s_now;
        if (s_echo_t == 0 || s_now - s_echo_t < 30) n = 0;
        else s_echo_gate = false;
    }
    if (n) zh_ash_rx(&A, buf, n, s_now);
}

static bool run_until(bool (*cond)(void), uint32_t max_ms)
{
    for (uint32_t i = 0; i < max_ms; i++) {
        if (cond()) return true;
        step();
    }
    return cond();
}
static bool is_ready(void) { return zh_ses_ready(&S); }

static const zh_ses_cfg_t *s_ses_cfg;   /* 场景要换会话参数时设（NULL = 默认） */
static void fresh(void)
{
    s_now = 0;
    s_need_reset = false;
    s_ready = s_mismatch = s_ncb = s_cb_order_bad = s_res_dup = s_res_bad = s_res_short = 0;
    memset(s_short, 0, sizeof(s_short));
    memset(s_reset_link, 0, sizeof(s_reset_link));
    memset(s_ash_ev, 0, sizeof(s_ash_ev));
    memset(s_has_res, 0, sizeof(s_has_res));
    memset(s_exec_uni, 0, sizeof(s_exec_uni));
    memset(s_exec_lookup, 0, sizeof(s_exec_lookup));
    s_noise = s_app_drop = 0;
    s_app_hung = s_ncp_dead = s_sync_cb = false;
    s_ncp_nops = s_ncp_versions = s_ncp_first_legacy = s_ncp_malformed = 0;
    s_echo_gate = false;
    s_slow_tag = -1;
    s_drop_from = s_drop_until = 0;
    s_ncp_boots = 0;
    zh_ash_cfg_t ac;
    zh_ash_cfg_default(&ac);
    zh_ash_init(&A, &ac, a_frame, a_event, NULL);
    const zh_ses_ops_t ops = { h_send, h_unacked, h_result, h_callback, h_event, NULL };
    zh_ses_init(&S, s_ses_cfg, &ops);
    ncp_reboot(0x01);                 /* 上电（POWERON）：NCP 先报一次 RSTACK，主机 RST 后它再软重启 */
    zh_ash_reset(&A);
}

/* 发第 k 条：偶数非幂等（sendUnicast，messageTag = k），奇数幂等（lookupEui64ByNodeId(k)） */
static bool is_uni(int k) { return (k & 1) == 0; }
static int submit_k(int k)
{
    uint8_t b[ZH_EZSP_MAX];
    size_t n;
    if (is_uni(k)) {
        zh_ezsp_aps_t aps = { 0x0104, 0x0006, 1, 1, 0x0140, 0, (uint8_t)k };
        uint8_t msg[8] = { 0x01, (uint8_t)k, 0x02 };
        n = zh_ezsp_enc_send_unicast(b, sizeof(b), 0, 0, 0x1234, &aps, (uint16_t)k, 3, msg);
        return zh_ses_submit(&S, b, n, (uint32_t)k, zh_ses_idempotent(ZH_EZSP_ID_SEND_UNICAST), s_now);
    }
    n = zh_ezsp_enc_lookup_eui64(b, sizeof(b), 0, (uint16_t)k);
    return zh_ses_submit(&S, b, n, (uint32_t)k, zh_ses_idempotent(ZH_EZSP_ID_LOOKUP_EUI64_BY_NODE_ID), s_now);
}

/* 依次发 [k0, k1)，每条等到有结论（或链路断了等重新 READY 后再发下一条），返回用掉的时间 */
static uint32_t run_batch(int k0, int k1, uint32_t max_ms)
{
    const uint32_t t0 = s_now;
    int k = k0;
    while (k < k1 && s_now - t0 < max_ms) {
        if (zh_ses_can_submit(&S)) {
            if (submit_k(k) == ZH_SES_OK) {
                while (!s_has_res[k] && s_now - t0 < max_ms) step();
                k++;
                continue;
            }
        }
        step();
    }
    for (int i = 0; i < 200; i++) step();
    return s_now - t0;
}

/* ---------------------------------------------------------------- 场景 */

static void e1_e2(void)
{
    fresh();
    CHECK(run_until(is_ready, 20000) && s_ready == 1 && s_ncp_first_legacy == 1, "E1 上电：NCP 收到的第一条是旧式头 version，握手完成 READY");
    const zh_ses_stats_t st0 = S.st;
    const uint32_t ms = run_batch(0, 300, 120000);
    int ok = 0, exec_bad = 0;
    for (int k = 0; k < 300; k++) {
        ok += s_has_res[k] && s_res[k] == ZH_SES_R_OK;
        exec_bad += (is_uni(k) ? s_exec_uni[k] : s_exec_lookup[k]) != 1;
    }
    CHECK(ok == 300 && s_res_bad == 0 && s_res_dup == 0, "E2 干净信道 300 条：全部 OK、内容对、没有重复结论（OK %d，坏 %d，重复 %d）", ok, s_res_bad, s_res_dup);
    CHECK(exec_bad == 0, "E2 每条恰好执行一次（%d 条不是）", exec_bad);
    CHECK(S.st.retries == st0.retries && S.st.echoes == st0.echoes && S.st.timeouts == st0.timeouts && S.st.stale == st0.stale &&
          A.st.tx_redata == 0, "E2 零重发、零回显、零超时、零过期（ASH 零重传）");
    CHECK(ms < 300 * 40, "E2 平均每条 < 40 ms（实际 %u ms 共 300 条）", ms);
}

static void e3(void)
{
    fresh();
    run_until(is_ready, 20000);
    s_noise = 0.003;
    s_app_drop = 0.05;
    const int N = 600;
    run_batch(0, N, 1200000);
    s_noise = 0;
    s_app_drop = 0;
    int nores = 0, uni_twice = 0, okexec_bad = 0, unk = 0, tmo = 0, down = 0, ok = 0, inv = 0;
    for (int k = 0; k < N; k++) {
        if (!s_has_res[k]) { nores++; continue; }
        const uint32_t ex = is_uni(k) ? s_exec_uni[k] : s_exec_lookup[k];
        if (is_uni(k) && ex > 1) uni_twice++;
        switch (s_res[k]) {
        case ZH_SES_R_OK: ok++; if (is_uni(k) ? ex != 1 : ex < 1) okexec_bad++; break;
        case ZH_SES_R_UNKNOWN: unk++; break;                                        /* 可能执行了：0 或 1 次（非幂等）都对，两次由上面的判据管 */
        case ZH_SES_R_TIMEOUT: tmo++; break;
        case ZH_SES_R_LINK_DOWN: down++; if (ex != 0) okexec_bad++; break;          /* 从没交出去：确定没执行 */
        case ZH_SES_R_INVALID: inv++; if (ex != 0) okexec_bad++; break;
        default: break;
        }
    }
    CHECK(nores == 0 && s_res_dup == 0 && s_res_bad == 0, "E3 噪声 + 应用层 5%% 不回：%d 条每条都有结论、没有重复、OK 的内容都对（无结论 %d，重复 %d，坏 %d）",
          N, nores, s_res_dup, s_res_bad);
    CHECK(uni_twice == 0, "E3 Z59：非幂等命令（sendUnicast）没有一条被执行两次（%d 条两次以上）", uni_twice);
    CHECK(okexec_bad == 0, "E3 回 OK 的执行过（非幂等的恰好一次）、回 LINK_DOWN / INVALID 的没执行（%d 条不符）", okexec_bad);
    CHECK(S.st.timeouts > 0 && A.st.tx_redata > 0 && s_reset_link[ZH_SES_WHY_RSP_TIMEOUT] > 0,
          "E3 应答超时 → 复位链路、ASH 重传都真的走到了：%u / %d / %u（回显靠运气，另有 E3b 确定性地造）", S.st.timeouts,
          s_reset_link[ZH_SES_WHY_RSP_TIMEOUT], A.st.tx_redata);
    CHECK(ok > N * 8 / 10, "E3 大部分 OK（OK %d / UNKNOWN %d / TIMEOUT %d / LINK_DOWN %d / INVALID %d）", ok, unk, tmo, down, inv);
    printf("   E3：OK %d、UNKNOWN %d、TIMEOUT %d、LINK_DOWN %d、INVALID %d；回显 %u、重发 %u、应答超时 %u、stale %u、rejected %u、NCP 拒收畸形 %u、RESET_LINK %d\n",
           ok, unk, tmo, down, inv, S.st.echoes, S.st.retries, S.st.timeouts, S.st.stale, S.st.rejected, s_ncp_malformed,
           s_reset_link[1] + s_reset_link[2] + s_reset_link[3] + s_reset_link[4] + s_reset_link[5]);
}

/* E3e：应答超时故意调短（2 s，比 NCP 侧重传短得多）：因为超时即复位、复位清空 NCP 的发送队列，照样不撞缓冲（NCP 替身撞了会 abort）。
   若改回"超时后重发"，这里就会撞（复核审计实测 100/100 个种子）—— 端到端钉住"超时只能复位" */
static void e3e(void)
{
    zh_ses_cfg_t c;
    zh_ses_cfg_default(&c);
    c.rsp_timeout_ms = 2000;
    s_ses_cfg = &c;
    fresh();
    run_until(is_ready, 20000);
    s_noise = 0.003;
    s_app_drop = 0.08;                           /* 150 条里一次都不丢的概率约 4e-6：这个场景必须真的走到超时复位 */
    const int K0 = 300, N = 150;
    run_batch(K0, K0 + N, 900000);
    s_noise = 0;
    s_app_drop = 0;
    int nores = 0, twice = 0, bad = 0;
    for (int k = K0; k < K0 + N; k++) {
        if (!s_has_res[k]) { nores++; continue; }
        const uint32_t ex = is_uni(k) ? s_exec_uni[k] : s_exec_lookup[k];
        if (is_uni(k) && ex > 1) twice++;
        if (s_res[k] == ZH_SES_R_LINK_DOWN && ex != 0) bad++;
        if (s_res[k] == ZH_SES_R_OK && !s_short[k] && (is_uni(k) ? ex != 1 : ex < 1)) bad++;
    }
    CHECK(nores == 0 && twice == 0 && bad == 0 && s_reset_link[ZH_SES_WHY_RSP_TIMEOUT] > 0,
          "E3e 应答超时 2 s + 噪声 + 应用层不回：没撞缓冲（替身没 abort）、非幂等没执行两次、结论都对（超时复位 %d 次）", s_reset_link[ZH_SES_WHY_RSP_TIMEOUT]);
    /* 确定性的一条：幂等命令的应答在路上被吞 3 s（> 2 s 的应答超时，NCP 还在重传）。会话只能复位；要是重发，新命令就会写进 NCP 还等着重传
       旧应答的那块缓冲（替身当场 abort）—— 随机噪声很少碰到这种时序，所以单独造 */
    const int k = 498;                            /* 偶数是 sendUnicast；用 499（lookup，幂等）才会诱发重发 */
    s_slow_ms = 3000;
    s_slow_tag = k + 1;
    const int rl0 = s_reset_link[ZH_SES_WHY_RSP_TIMEOUT];
    while (!zh_ses_can_submit(&S)) step();
    CHECK(submit_k(k + 1) == ZH_SES_OK, "E3e 发第 %d 条（幂等）", k + 1);
    for (int i = 0; i < 20000 && !s_has_res[k + 1]; i++) step();
    for (int i = 0; i < 5000; i++) step();
    CHECK(s_has_res[k + 1] && s_res[k + 1] == ZH_SES_R_TIMEOUT && s_reset_link[ZH_SES_WHY_RSP_TIMEOUT] == rl0 + 1 && s_exec_lookup[k + 1] == 1,
          "E3e 应答被吞 3 s：会话超时即复位（不重发）、回 TIMEOUT、NCP 只执行了 1 次、没撞缓冲");
    s_slow_ms = 2000;
    s_ses_cfg = NULL;
}

/* E3d：极端噪声，链路反复复位 */
static void e3d(void)
{
    fresh();
    run_until(is_ready, 20000);
    s_noise = 0.02;
    const int K0 = 1000 - 600, N = 400;          /* 用 400..799 号（与别的场景不重叠） */
    run_batch(K0, K0 + N, 4000000);
    s_noise = 0;
    int nores = 0, twice = 0, bad = 0, ok = 0, unk = 0, tmo = 0, down = 0;
    for (int k = K0; k < K0 + N; k++) {
        if (!s_has_res[k]) { nores++; continue; }
        const uint32_t ex = is_uni(k) ? s_exec_uni[k] : s_exec_lookup[k];
        if (is_uni(k) && ex > 1) twice++;
        switch (s_res[k]) {
        case ZH_SES_R_OK: ok++; if (is_uni(k) ? ex != 1 : ex < 1) bad++; break;
        case ZH_SES_R_LINK_DOWN: down++; if (ex != 0) bad++; break;
        case ZH_SES_R_UNKNOWN: unk++; break;
        case ZH_SES_R_TIMEOUT: tmo++; break;
        default: break;
        }
    }
    const int resets = s_ash_ev[ZH_ASH_EV_TIMEOUTS] + s_ash_ev[ZH_ASH_EV_NCP_ERROR] + s_ash_ev[ZH_ASH_EV_NCP_RESET] +
                       s_reset_link[ZH_SES_WHY_RSP_TIMEOUT] + s_reset_link[ZH_SES_WHY_ACK_GUARD] + s_reset_link[ZH_SES_WHY_ECHOES];
    int short_unexec = 0;
    for (int k = K0; k < K0 + N; k++) if (s_short[k] && is_uni(k) && s_exec_uni[k] != 1) short_unexec++;
    CHECK(nores == 0 && s_res_dup == 0 && s_res_bad == 0, "E3d 极端噪声 %d 条：每条都有结论、没有重复、OK 的内容都对（无结论 %d）", N, nores);
    CHECK(twice == 0 && bad == 0, "E3d 非幂等命令没有被执行两次；OK 的执行过；LINK_DOWN 的确定没执行（两次 %d、不符 %d）", twice, bad);
    CHECK(A.st.tx_redata > 0 && short_unexec == 0, "E3d ASH 确实在纠错（重传 %u）；参数解不开的 OK 应答对应的非幂等命令都恰好执行过一次", A.st.tx_redata);
    printf("   E3d：OK %d（其中参数解不开 %d）、UNKNOWN %d、TIMEOUT %d、LINK_DOWN %d；链路复位 %d、回显 %u\n", ok, s_res_short, unk, tmo, down, resets, S.st.echoes);
}

/* E3c：应答在路上丢了 2 s，只能等 NCP 侧重传 */
static void e3c(void)
{
    fresh();
    run_until(is_ready, 20000);
    run_batch(800, 802, 5000);                 /* 先跑两条，让双方的 ACK 周期自适应到稳定 */
    const int k = 804;                          /* 非幂等：会话一旦先放弃就只能回 TIMEOUT */
    s_slow_tag = k;
    const uint32_t t0 = s_now, to0 = S.st.timeouts;
    while (!zh_ses_can_submit(&S)) step();
    CHECK(submit_k(k) == ZH_SES_OK, "E3c 发第 %d 条", k);
    while (!s_has_res[k] && s_now - t0 < 30000) step();
    CHECK(s_has_res[k] && s_res[k] == ZH_SES_R_OK && s_exec_uni[k] == 1 && S.st.timeouts == to0 && s_now - t0 >= 2000,
          "E3c 应答被吞 2 s：会话等到 NCP 侧重传送到、OK、恰好执行 1 次、没有判超时（用了 %u ms，结论 %d）", s_now - t0, s_has_res[k] ? (int)s_res[k] : -1);
    run_batch(806, 810, 20000);
    int ok = 0;
    for (int i = 806; i < 810; i++) ok += s_has_res[i] && s_res[i] == ZH_SES_R_OK;
    CHECK(ok == 4 && s_ash_ev[ZH_ASH_EV_NCP_ERROR] == 0, "E3c 之后的命令照常（%d / 4 OK），NCP 没进 ERROR", ok);
}

/* E3b：确定性地造一次回显 */
static void e3b(void)
{
    fresh();
    run_until(is_ready, 20000);
    const int ks[2] = { 701, 702 };   /* 701 幂等（lookup）、702 非幂等（sendUnicast） */
    for (int i = 0; i < 2; i++) {
        const int k = ks[i];
        while (!zh_ses_can_submit(&S)) step();
        const uint32_t echo0 = S.st.echoes;
        s_echo_redata0 = A.st.tx_redata;
        s_echo_t = 0;
        s_echo_gate = true;
        CHECK(submit_k(k) == ZH_SES_OK, "E3b 发第 %d 条", k);
        const uint32_t t0 = s_now;
        while (!s_has_res[k] && s_now - t0 < 60000) step();
        const uint32_t ex = is_uni(k) ? s_exec_uni[k] : s_exec_lookup[k];
        if (is_uni(k))
            CHECK(S.st.echoes > echo0 && s_has_res[k] && s_res[k] == ZH_SES_R_UNKNOWN && ex == 1,
                  "E3b 非幂等命令遇回显 → UNKNOWN、恰好执行 1 次（回显 +%u、结论 %d、执行 %u）", S.st.echoes - echo0, s_has_res[k] ? (int)s_res[k] : -1, ex);
        else
            CHECK(S.st.echoes > echo0 && s_has_res[k] && s_res[k] == ZH_SES_R_OK && ex == 2,
                  "E3b 幂等命令遇回显 → 会话重发、OK（回显 +%u、结论 %d、执行 %u）", S.st.echoes - echo0, s_has_res[k] ? (int)s_res[k] : -1, ex);
    }
}

static void e4_e5_e6(void)
{
    fresh();
    run_until(is_ready, 20000);
    const uint32_t n0 = s_ncp_nops;
    const uint32_t ka0 = S.st.keepalives;
    for (int i = 0; i < 60000; i++) step();
    CHECK(S.st.keepalives - ka0 >= 5 && s_ncp_nops - n0 == S.st.keepalives - ka0 && S.st.timeouts == 0 && s_reset_link[ZH_SES_WHY_KEEPALIVE] == 0,
          "E4 空闲 60 s：发了 %u 个保活 nop、NCP 都收到、没有超时", S.st.keepalives - ka0);

    /* E5：应用层挂死、ASH 照样确认 —— ASH 自己看不出来，只有保活能发现 */
    s_app_hung = true;
    const uint32_t t0 = s_now;
    const int boots0 = s_ncp_boots, ready0 = s_ready;
    bool rec = false;
    for (uint32_t i = 0; i < 60000 && !rec; i++) {
        step();
        rec = s_ready > ready0;
    }
    CHECK(s_reset_link[ZH_SES_WHY_KEEPALIVE] == 1 && s_ncp_boots > boots0 && rec,
          "E5 应用层挂死：保活超时 → RESET_LINK(KEEPALIVE) → 复位、NCP 重启 → 重新 READY");
    CHECK(s_now - t0 <= S.cfg.keepalive_ms + S.cfg.rsp_timeout_ms + 8000, "E5 从挂死到重新 READY ≤ 保活 + 应答超时 + 复位握手 8 s（实际 %u ms）", s_now - t0);
    CHECK(s_ash_ev[ZH_ASH_EV_TIMEOUTS] == 0, "E5 期间 ASH 没报过链路超时（证明是会话层自己发现的）");

    /* E6：NCP 整个不响应 */
    run_batch(900, 901, 2000);
    s_ncp_dead = true;
    CHECK(submit_k(902) == ZH_SES_OK, "E6 发一条后 NCP 死掉");
    for (int i = 0; i < 40000; i++) step();
    CHECK(s_has_res[902] && (s_res[902] == ZH_SES_R_UNKNOWN || s_res[902] == ZH_SES_R_TIMEOUT), "E6 在途命令在 40 s 内有结论、且是\"可能执行了\"（已交给 ASH，%d）", s_has_res[902] ? (int)s_res[902] : -1);
    CHECK(s_ash_ev[ZH_ASH_EV_RESET_FAILED] >= 2 && !zh_ses_ready(&S), "E6 之后反复复位（RESET_FAILED %d 次）、会话不 READY", s_ash_ev[ZH_ASH_EV_RESET_FAILED]);
    s_ncp_dead = false;
    ncp_reboot(0x01);
    CHECK(run_until(is_ready, 30000), "E6 NCP 回来后重新 READY");

    /* E6b：保活 nop 在途、上层一条排在后面时 NCP 死掉：排着的那条从没交出去，回 LINK_DOWN，而且 NCP 确实没执行过它 */
    const uint32_t ka = S.st.keepalives;
    while (S.st.keepalives == ka) step();
    s_ncp_dead = true;
    CHECK(S.cur.used && S.cur.internal && submit_k(904) == ZH_SES_OK && S.pend.used, "E6b nop 在途时 submit：排着");
    for (int i = 0; i < 40000 && !s_has_res[904]; i++) step();
    CHECK(s_has_res[904] && s_res[904] == ZH_SES_R_LINK_DOWN && s_exec_uni[904] == 0, "E6b 排着的那条回 LINK_DOWN、NCP 没执行过（%d）", s_has_res[904] ? (int)s_res[904] : -1);
    s_ncp_dead = false;
    ncp_reboot(0x01);
    CHECK(run_until(is_ready, 30000), "E6b NCP 回来后重新 READY");
}

static void e7_e8(void)
{
    fresh();
    run_until(is_ready, 20000);
    /* E7 同步回调：NCP 攒 20 条，应答带 pending 位 */
    s_sync_cb = true;
    for (int i = 0; i < 20; i++) s_cbq[s_cbq_tail++ % 256] = (uint16_t)(100 + i);
    run_batch(0, 1, 5000);           /* 一条命令的应答带出 pending 位 */
    for (int i = 0; i < 3000; i++) step();
    CHECK(s_ncb == 20 && s_cb_order_bad == 0 && s_cb_seen[0] == 100 && s_cbq_head == s_cbq_tail, "E7 同步回调 20 条：会话发 callback 取，按序、恰好一次（%d 条）", s_ncb);
    s_sync_cb = false;
    /* E8 异步回调夹在命令中间 */
    s_ncb = 0;
    int sent = 0;
    const uint32_t t0 = s_now;
    for (int k = 2; k < 102; k++) {
        while (!zh_ses_can_submit(&S)) step();
        submit_k(k);
        if (k % 3 == 0) ncp_async_cb((uint16_t)(500 + sent++));
        while (!s_has_res[k] && s_now - t0 < 60000) step();
    }
    for (int i = 0; i < 1000; i++) step();
    int ok = 0;
    for (int k = 2; k < 102; k++) ok += s_has_res[k] && s_res[k] == ZH_SES_R_OK;
    CHECK(s_ncb == sent && s_cb_order_bad == 0 && ok == 100, "E8 异步回调 %d 条夹在 100 条命令中：回调按序恰好一次（%d 条），命令全 OK（%d）", sent, s_ncb, ok);
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("用法：%s <libsdkncp.so>\n", argv[0]); return 2; }
    s_path = argv[1];
    if (argc >= 3) {                                                   /* 可选第二个参数换随机种子（换种子扫一遍用；xorshift 不能是 0） */
        s_rng = (uint32_t)strtoul(argv[2], NULL, 0);
        if (!s_rng) s_rng = 1;
    }
    e1_e2();
    e3();
    e3b();
    e3c();
    e3d();
    e3e();
    e4_e5_e6();
    e7_e8();
    printf("ses_interop：%d 通过，%d 失败\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
