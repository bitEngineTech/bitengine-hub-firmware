/* zh_ash_core（我们的主机侧）对接 SDK 的真 NCP 侧 ASH（ash-ncp.c，经 libsdkncp.so）—— docs/15 设计 8.3、14.1 ②。
 * 自写真值表（zh_ash_test.c）只能证明"符合我们对协议的理解"；这里证明"和真 NCP 说得通"。
 *
 *   编解码：我们编的帧 SDK 的 ashDecodeByte 认、SDK 的 ashEncodeByte 编出来与我们逐字节相同；CRC、随机化与 SDK 一致。
 *   状态机（模拟时钟，1 ms 一步）。NCP 应用层像 em260 一样把应答**原地写回命令缓冲**（ncp_stub.c）；主机上层像 EZSP 层一样
 *   丢掉命令回显（方向不对的帧）、同一会话里超时重发（命令里带第几次发）。判据分两边：
 *     主机侧：同一会话里同一个 NCP 应答序号不交出两次（ASH 不重复交付）；应答内容对；每条命令最终有应答。
 *     NCP 侧：收到的命令与主机发的逐字节相同；同一次 NCP 会话里同一条（序号, 第几次发）不交出两次；首次交付按序。
 *     S1 上电：NCP 先报 POWERON 的 RSTACK，主机 RST → NCP 重启 → SOFTWARE 的 RSTACK → 连上
 *     S2 干净信道上 500 条命令（5–220 字节、偏向保留字节）：零重传零 NAK、零回显、零 EZSP 重发
 *     S3 NCP 主动发的回调（含命令在途时、连发 5 帧）：全部按序、恰好一次
 *     S4 有噪声的信道（翻位 / 丢字节）：每条命令最终有应答、内容对；两边都走到过纠错
 *     S5 NCP 自己重启（软件原因 → RSTACK → NCP_RESET）；S5w 看门狗复位（→ ERROR → NCP_ERROR）：都能复位后重新连上
 *     S6 NCP 不回 RSTACK：5000 ms 后 RESET_FAILED
 *     S7 窗口 3 的流水线（同时 4 条在途、每步只写得下一帧，逼出"重传中途收到部分确认"）+ 噪声：同样的判据，且窗口与队列确实撑满过
 * 用法：ncp_interop <libsdkncp.so 的路径> */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zh_ash_core.h"

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
static uint8_t  (*sdk_encode)(uint8_t, uint8_t, uint8_t *);
static uint8_t  (*sdk_decode)(uint8_t, uint8_t *, uint8_t *);
static uint8_t  (*sdk_randomize)(uint8_t, uint8_t *, uint8_t);
static uint16_t (*sdk_crc)(uint8_t, uint16_t);
static bool     *sdk_decode_in_progress;

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
    ncp_poll = sym("ncp_poll"); ncp_take_cmd = sym("ncp_take_cmd"); ncp_send = sym("ncp_send"); ncp_reply = sym("ncp_reply"); ncp_set_inplace = sym("ncp_set_inplace"); ncp_cmd_count = sym("ncp_cmd_count");
    sdk_encode = sym("ashEncodeByte"); sdk_decode = sym("ashDecodeByte"); sdk_randomize = sym("ashRandomizeArray"); sdk_crc = sym("halCommonCrc16");
    sdk_decode_in_progress = sym("ashDecodeInProgress");
    if (ncp_cmd_count() != 0) { printf("❌ 重新加载后 NCP 的静态变量没清零（dlclose 没卸载？）\n"); exit(1); }
}

/* ---- 随机数（确定的） ---- */
static uint32_t s_rng = 0x12345678;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }
static uint8_t  rbyte(void)
{
    static const uint8_t special[] = { 0x7E, 0x7D, 0x11, 0x13, 0x18, 0x1A, 0xFF, 0x00, 0x42, 0x21 };
    return (rnd() % 4 == 0) ? special[rnd() % sizeof(special)] : (uint8_t)rnd();
}

/* =========================================================== 编解码 */
static size_t sdk_encode_frame(const uint8_t *raw, size_t len, uint8_t *out)
{
    uint8_t off = 0;
    size_t n = 0;
    out[n++] = sdk_encode((uint8_t)len, raw[0], &off);
    while (off != 0xFF) out[n++] = sdk_encode(0, raw[off < len ? off : len - 1], &off);
    return n;
}

static void codec_tests(void)
{
    int bad_crc = 0, bad_rand = 0, bad_enc = 0, bad_dec = 0;
    for (int it = 0; it < 3000; it++) {
        uint8_t d[ZH_ASH_DATA_MAX];
        const size_t len = 3 + rnd() % (ZH_ASH_DATA_MAX - 2);
        for (size_t i = 0; i < len; i++) d[i] = rbyte();
        /* CRC：SDK 的 halCommonCrc16 逐字节 vs 我们的 */
        uint16_t c = 0xFFFF;
        for (size_t i = 0; i < len; i++) c = sdk_crc(d[i], c);
        if (c != zh_ash_crc16(d, len)) bad_crc++;
        /* 随机化 */
        uint8_t a[ZH_ASH_DATA_MAX], b[ZH_ASH_DATA_MAX];
        memcpy(a, d, len); memcpy(b, d, len);
        (void)sdk_randomize(0, a, (uint8_t)len);
        zh_ash_randomize(b, len);
        if (memcmp(a, b, len)) bad_rand++;
        /* 我们编 DATA 帧 vs SDK 编同一帧（控制 + 随机化后的数据） */
        const uint8_t ctrl = (uint8_t)(rnd() & 0x7F);
        uint8_t ours[ZH_ASH_WIRE_MAX], theirs[ZH_ASH_WIRE_MAX + 8], raw[ZH_ASH_RAW_MAX];
        const size_t on = zh_ash_encode(ctrl, d, len, ours, sizeof(ours));
        raw[0] = ctrl; memcpy(raw + 1, b, len);
        const size_t tn = sdk_encode_frame(raw, 1 + len, theirs);
        if (on != tn || memcmp(ours, theirs, on)) bad_enc++;
        /* SDK 解我们编的帧 */
        *sdk_decode_in_progress = false;
        uint8_t got[ZH_ASH_RAW_MAX + 4], outb = 0, outlen = 0;
        int st = -1;
        for (size_t i = 0; i < on; i++) {
            const uint8_t idx = outlen;
            st = sdk_decode(ours[i], &outb, &outlen);
            if (outlen != idx && idx < sizeof(got)) got[idx] = outb;
            if (st != 0x20 /* SL_ZIGBEE_EZSP_ASH_IN_PROGRESS */) break;
        }
        if (st != 0 /* SUCCESS */ || outlen != 1 + len || memcmp(got, raw, 1 + len)) bad_dec++;
    }
    CHECK(bad_crc == 0, "CRC 与 SDK halCommonCrc16 一致（不一致 %d / 3000）", bad_crc);
    CHECK(bad_rand == 0, "随机化与 SDK ashRandomizeArray 一致（%d）", bad_rand);
    CHECK(bad_enc == 0, "我们编的帧与 SDK ashEncodeByte 逐字节相同（%d）", bad_enc);
    CHECK(bad_dec == 0, "SDK ashDecodeByte 认我们编的帧、解出的字节对（%d）", bad_dec);

    /* 短帧（RST / ACK / NAK）也比一下 */
    const uint8_t shorts[] = { 0xC0, 0x80, 0x87, 0xA3 };
    int bad_sh = 0;
    for (size_t i = 0; i < sizeof(shorts); i++) {
        uint8_t o[16], t[16];
        const size_t on = zh_ash_encode(shorts[i], NULL, 0, o, sizeof(o));
        const size_t tn = sdk_encode_frame(&shorts[i], 1, t);
        if (on != tn || memcmp(o, t, on)) bad_sh++;
    }
    CHECK(bad_sh == 0, "短帧（RST、ACK、NAK）编码与 SDK 相同");
}

/* =========================================================== 状态机对接 */
static zh_ash_t A;
static uint32_t s_now;
static int      s_session;                       /* 主机第几次连上 */
static int      s_ncp_session;                   /* NCP 第几次启动 */
static bool     s_need_reset;
static int      s_ev_count[8];
static uint8_t  s_last_code[8];
static double   s_noise;                         /* 每字节出错的概率（一半丢、一半翻位） */
static bool     s_ncp_dead;
static size_t   s_cap = 8192;                    /* 每步 zh_ash_tx 的写缓冲（S7 限到一帧） */
static bool     s_inplace = true;                /* NCP 应用层原地应答（em260）；S7 用另开缓冲的理想化应用层 */
static uint32_t s_latency;                       /* 单向延迟（步 = ms）：S7 用，让确认晚到、窗口确定能撑满 */
static uint32_t s_tx_every = 1;                  /* 主机每几步写一次 UART：S7 取 4，重传（一步一帧）慢于往返，才会在重传中途收到部分确认 */

/* 延迟线：每步的字节块带上"该送到的时刻"排队 */
typedef struct { uint32_t at[512]; uint16_t len[512]; uint8_t data[1 << 18]; unsigned head, tail; size_t dh, dt; } dline_t;
static dline_t s_h2n, s_n2h;
static void dl_reset(dline_t *d) { d->head = d->tail = 0; d->dh = d->dt = 0; }
static void dl_push(dline_t *d, uint32_t at, const uint8_t *b, size_t n)
{
    if (!n) return;
    if (d->tail - d->head >= 512 || n > 0xFFFF) { printf("❌ 延迟线满\n"); exit(1); }
    d->at[d->tail % 512] = at; d->len[d->tail % 512] = (uint16_t)n;
    for (size_t i = 0; i < n; i++) d->data[(d->dt + i) % sizeof(d->data)] = b[i];
    d->dt += n; d->tail++;
}
static size_t dl_pop(dline_t *d, uint32_t now, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (d->head != d->tail && d->at[d->head % 512] <= now && n + d->len[d->head % 512] <= cap) {
        const size_t l = d->len[d->head % 512];
        for (size_t i = 0; i < l; i++) out[n + i] = d->data[(d->dh + i) % sizeof(d->data)];
        d->dh += l; n += l; d->head++;
    }
    return n;
}

#define MAXC 2048
#define EZSP_TIMEOUT_MS 4000                     /* 上层的应答超时（测试用；真 EZSP 层的值随 8.4 定） */
static uint8_t  s_cmd[MAXC][ZH_ASH_DATA_MAX];    /* 主机发的命令（最近一次发的那份） */
static size_t   s_cmd_len[MAXC];
static bool     s_answered[MAXC];
static int      s_bad_resp, s_echo, s_dup_serial, s_retries;
static uint8_t  s_seen_serial[1 << 16];          /* 主机侧：本会话里见过的 NCP 应答序号（存的是会话号 + 1） */
static int      s_ncp_bad, s_ncp_dup, s_ncp_order, s_ncp_last;
static uint8_t  s_ncp_seen[MAXC][32];            /* NCP 侧：(序号, 第几次发) 在哪次 NCP 会话里收到过（会话号 + 1） */
static uint16_t s_ncp_serial;
static int      s_cb_next, s_cb_bad, s_cb_got;
static unsigned s_max_unacked, s_max_q, s_retx_partial;

static void on_frame(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    if (n < 3) { s_bad_resp++; return; }
    const int seq = (d[1] << 8) | d[2];
    if (d[0] == 0x01) { s_echo++; return; }      /* 命令回显（em260 的应答缓冲被重传的命令覆盖）：方向不对，丢 */
    if (d[0] == 0x02) {                          /* NCP 的回调 */
        if (seq != s_cb_next) s_cb_bad++;
        s_cb_next = seq + 1;
        s_cb_got++;
        return;
    }
    if (d[0] != 0x81 || n < 5 || seq >= MAXC) { s_bad_resp++; return; }
    const uint16_t serial = (uint16_t)((d[3] << 8) | d[4]);
    if (s_seen_serial[serial] == (uint8_t)(s_session + 1)) s_dup_serial++;
    s_seen_serial[serial] = (uint8_t)(s_session + 1);
    bool ok = n == s_cmd_len[seq];
    for (size_t i = 5; ok && i < n; i++) ok = d[i] == (uint8_t)(s_cmd[seq][i] ^ 0x5A);
    if (!ok) { s_bad_resp++; return; }
    s_answered[seq] = true;
}
static void on_event(void *ctx, zh_ash_event_t ev, uint8_t code)
{
    (void)ctx;
    s_ev_count[ev]++;
    s_last_code[ev] = code;
    if (ev == ZH_ASH_EV_CONNECTED) s_session++;
    else s_need_reset = true;
}

static size_t channel(uint8_t *b, size_t n)
{
    if (s_noise <= 0) return n;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        const double r = (double)(rnd() % 1000000) / 1e6;
        if (r < s_noise / 2) continue;                          /* 丢 */
        b[o] = b[i];
        if (r < s_noise) b[o] ^= (uint8_t)(1u << (rnd() % 8));  /* 翻位 */
        o++;
    }
    return o;
}

/* NCP 应用层：命令 [01][seq][seq][第几次发][..] → 原地应答 [81][seq][seq][NCP 序号 2 B][payload ^ 5A]（同长度） */
static void ncp_app_on_cmd(void)
{
    uint8_t c[256];
    const size_t n = ncp_take_cmd(c);
    const int seq = n >= 5 ? (c[1] << 8) | c[2] : -1;
    if (n < 5 || c[0] != 0x01 || seq >= MAXC || c[3] >= 32) { s_ncp_bad++; return; }
    if (n != s_cmd_len[seq] || memcmp(c, s_cmd[seq], n)) { s_ncp_bad++; return; }   /* 与主机发的那份不同 */
    if (s_ncp_seen[seq][c[3]] == (uint8_t)(s_ncp_session + 1)) s_ncp_dup++;        /* 同一份 ASH 交了两次 */
    else if (c[3] == 0 && seq < s_ncp_last) s_ncp_order++;                          /* 首次交付乱序 */
    s_ncp_seen[seq][c[3]] = (uint8_t)(s_ncp_session + 1);
    if (seq > s_ncp_last) s_ncp_last = seq;
    uint8_t r[256];
    r[0] = 0x81; r[1] = c[1]; r[2] = c[2];
    r[3] = (uint8_t)(s_ncp_serial >> 8); r[4] = (uint8_t)s_ncp_serial;
    s_ncp_serial++;
    for (size_t i = 5; i < n; i++) r[i] = c[i] ^ 0x5A;
    if (s_inplace) ncp_reply(r, n);
    else ncp_send(r, n, false);
}

static void ncp_reboot(uint8_t reason)
{
    load();
    ncp_set_inplace(s_inplace);
    ncp_set_time(s_now);
    ncp_boot(reason);
    s_ncp_session++;
    s_ncp_last = -1;
}

static void step(void)
{
    s_now++;
    if (s_need_reset) { s_need_reset = false; zh_ash_reset(&A); }
    static uint8_t buf[8192];
    size_t n = (s_now % s_tx_every == 0) ? zh_ash_tx(&A, s_now, buf, s_cap) : 0;
    const unsigned u = (unsigned)((A.frm_tx - A.ack_rx) & 7);
    if (u > s_max_unacked) s_max_unacked = u;
    if (A.q_len > s_max_q) s_max_q = A.q_len;
    if ((A.flags & 0x0020) && A.ack_rx != A.frm_head) s_retx_partial++;   /* 重传中收到过部分确认（还没 scrub） */
    n = channel(buf, n);
    if (s_ncp_dead) return;
    ncp_set_time(s_now);
    dl_push(&s_h2n, s_now + s_latency, buf, n);
    n = dl_pop(&s_h2n, s_now, buf, sizeof(buf));
    if (n) ncp_feed(buf, n);
    const int r = ncp_poll();
    if (r == 2) ncp_reboot(ZH_ASH_RESET_SOFTWARE);   /* 收到 RST：NCP 重启 */
    else if (r == 1) ncp_app_on_cmd();
    n = ncp_drain(buf, sizeof(buf));
    n = channel(buf, n);
    dl_push(&s_n2h, s_now + s_latency, buf, n);
    n = dl_pop(&s_n2h, s_now, buf, sizeof(buf));
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
static bool connected(void) { return zh_ash_connected(&A); }

static void fresh(uint8_t boot_reason, uint8_t tx_k)
{
    ncp_reboot(boot_reason);
    zh_ash_cfg_t c;
    zh_ash_cfg_default(&c);
    c.tx_k = tx_k;
    zh_ash_init(&A, &c, on_frame, on_event, NULL);
    memset(s_ev_count, 0, sizeof(s_ev_count));
    memset(s_answered, 0, sizeof(s_answered));
    s_session = 0;
    s_need_reset = false;
    s_ncp_dead = false;
    s_bad_resp = s_echo = s_dup_serial = s_retries = 0;
    s_ncp_bad = s_ncp_dup = s_ncp_order = 0;
    s_cb_next = s_cb_bad = s_cb_got = 0;
    s_max_unacked = s_max_q = s_retx_partial = 0;
    dl_reset(&s_h2n);
    dl_reset(&s_n2h);
    zh_ash_reset(&A);
}

static void make_cmd(int seq, uint8_t attempt)
{
    if (attempt == 0) {
        const size_t len = 5 + rnd() % (ZH_ASH_DATA_MAX - 4);
        s_cmd[seq][0] = 0x01; s_cmd[seq][1] = (uint8_t)(seq >> 8); s_cmd[seq][2] = (uint8_t)seq;
        for (size_t i = 4; i < len; i++) s_cmd[seq][i] = rbyte();
        s_cmd_len[seq] = len;
    }
    s_cmd[seq][3] = attempt;
}

/* 发 count 条命令，同时最多 depth 条在途（交给 ASH、还没应答）。没应答的：链路重连后、或同一会话里超时，再发（第几次发 +1）。
   NCP 侧回调：每 cb_every 条命令发一次（每第三次连发 5 帧） */
static int run_commands(int count, int depth, int cb_every, uint32_t max_ms)
{
    int next = 0, cb_seq = 0;
    int sent_sess[MAXC];
    uint32_t sent_at[MAXC];
    uint8_t attempt[MAXC];
    memset(attempt, 0, sizeof(attempt));
    for (int i = 0; i < count; i++) sent_sess[i] = -1;
    const uint32_t t_end = s_now + max_ms;
    while (s_now < t_end) {
        int done = 0, inflight = 0;
        for (int i = 0; i < next; i++) { if (s_answered[i]) done++; else inflight++; }
        if (done == count) break;
        if (connected()) {
            /* 重发：链路换过会话、或本会话里超时 */
            for (int i = 0; i < next; i++) {
                if (s_answered[i] || zh_ash_queued(&A) >= ZH_ASH_TXQ) continue;
                const bool stale = sent_sess[i] != s_session || s_now - sent_at[i] >= EZSP_TIMEOUT_MS;
                if (stale && attempt[i] < 31) {
                    make_cmd(i, ++attempt[i]);
                    if (zh_ash_send(&A, s_cmd[i], s_cmd_len[i])) { sent_sess[i] = s_session; sent_at[i] = s_now; s_retries++; }
                }
            }
            while (next < count && inflight < depth && zh_ash_queued(&A) < ZH_ASH_TXQ) {
                make_cmd(next, 0);
                if (!zh_ash_send(&A, s_cmd[next], s_cmd_len[next])) break;
                sent_sess[next] = s_session; sent_at[next] = s_now;
                if (cb_every && next % cb_every == 0) {
                    const int burst = (next % (cb_every * 3) == 0) ? 5 : 1;
                    for (int k = 0; k < burst; k++) {
                        uint8_t cb[8] = { 0x02, (uint8_t)(cb_seq >> 8), (uint8_t)cb_seq, 0x7E, 0x7D, 0x11, 0x00, 0xFF };
                        cb_seq++;
                        ncp_send(cb, sizeof(cb), false);
                    }
                }
                next++;
                inflight++;
            }
        }
        step();
    }
    for (int t = 0; t < 3000; t++) step();        /* 把剩下的回调收完 */
    int answered = 0;
    for (int i = 0; i < count; i++) answered += s_answered[i];
    return answered;
}

static bool s5_reset_seen(void) { return s_ev_count[ZH_ASH_EV_NCP_RESET] > 0; }
static bool s5_error_seen(void) { return s_ev_count[ZH_ASH_EV_NCP_ERROR] > 0; }
static bool session2(void) { return s_session >= 2 && connected(); }

int main(int argc, char **argv)
{
    if (argc < 2) { printf("用法：ncp_interop <libsdkncp.so>\n"); return 2; }
    s_path = argv[1];
    load();

    codec_tests();

    /* S1 上电 */
    fresh(2 /* POWERON */, 1);
    CHECK(run_until(connected, 2000), "S1：连上");
    CHECK(s_ev_count[ZH_ASH_EV_CONNECTED] == 1 && s_last_code[ZH_ASH_EV_CONNECTED] == ZH_ASH_RESET_SOFTWARE,
          "S1：连上的 RSTACK 是软件复位（上电那个被忽略）");
    CHECK(s_ev_count[ZH_ASH_EV_NCP_RESET] == 0 && s_ev_count[ZH_ASH_EV_NCP_ERROR] == 0, "S1：没有误报 NCP 重启 / 错误");

    /* S2 干净信道 */
    int ok = run_commands(500, 1, 0, 600000);
    CHECK(ok == 500 && s_bad_resp == 0, "S2：500 条命令都有应答、内容对（%d，坏 %d）", ok, s_bad_resp);
    CHECK(s_dup_serial == 0 && s_ncp_dup == 0 && s_ncp_bad == 0 && s_ncp_order == 0, "S2：两边都不重复、不乱序、内容逐字节对");
    CHECK(s_echo == 0 && s_retries == 0, "S2：零回显、零 EZSP 重发（回显 %d、重发 %d）", s_echo, s_retries);
    CHECK(A.st.tx_redata == 0 && A.st.tx_nak == 0 && A.st.rx_nak == 0 && A.st.rx_crc == 0 && A.st.ack_timeouts == 0,
          "S2：零重传零 NAK 零 CRC 错（重传 %u、NAK %u/%u、CRC %u、超时 %u）",
          A.st.tx_redata, A.st.tx_nak, A.st.rx_nak, A.st.rx_crc, A.st.ack_timeouts);
    CHECK(s_ev_count[ZH_ASH_EV_NCP_RESET] + s_ev_count[ZH_ASH_EV_TIMEOUTS] + s_ev_count[ZH_ASH_EV_NCP_ERROR] == 0, "S2：链路一直没断");

    /* S3 NCP 回调 */
    fresh(2, 1);
    ok = run_commands(300, 1, 4, 600000);
    CHECK(ok == 300 && s_bad_resp == 0, "S3：有回调穿插时命令照样都有应答（%d）", ok);
    CHECK(s_cb_got > 100 && s_cb_bad == 0, "S3：回调 %d 帧全部按序、恰好一次（乱序 %d）", s_cb_got, s_cb_bad);
    CHECK(A.st.rx_oos == 0 && A.st.tx_nak == 0 && s_dup_serial == 0, "S3：NCP 连发 5 帧时主机不乱序、不 NAK");

    /* S4 噪声信道（窗口 1） */
    fresh(2, 1);
    s_noise = 1.0 / 1500;
    ok = run_commands(300, 1, 0, 3600000);
    s_noise = 0;
    CHECK(ok == 300 && s_bad_resp == 0, "S4：噪声下 300 条命令最终都有应答、内容对（%d，坏 %d）", ok, s_bad_resp);
    CHECK(s_dup_serial == 0 && s_ncp_dup == 0 && s_ncp_bad == 0, "S4：两边都不重复交付、NCP 收到的命令逐字节对（重复 %d/%d，坏 %d）",
          s_dup_serial, s_ncp_dup, s_ncp_bad);
    CHECK(A.st.tx_redata > 0 && A.st.rx_crc + A.st.rx_comm + A.st.rx_short + A.st.rx_long > 0 && A.st.tx_nak > 0,
          "S4：两个方向都走到过纠错（重传 %u、收到坏帧 %u、回 NAK %u）",
          A.st.tx_redata, A.st.rx_crc + A.st.rx_comm + A.st.rx_short + A.st.rx_long, A.st.tx_nak);

    /* S5 NCP 自己重启（软件原因：RSTACK） */
    fresh(2, 1);
    CHECK(run_until(connected, 2000), "S5：先连上");
    ncp_reboot(ZH_ASH_RESET_SOFTWARE);
    CHECK(run_until(s5_reset_seen, 100), "S5：主机报 NCP_RESET");
    CHECK(run_until(session2, 3000), "S5：主机复位后重新连上");
    /* S5w 看门狗复位：NCP 发 ERROR 不发 RSTACK（ash-ncp.c ashStart 的 default 分支） */
    fresh(2, 1);
    CHECK(run_until(connected, 2000), "S5w：先连上");
    ncp_reboot(3 /* EM2XX_RESET_WATCHDOG */);
    CHECK(run_until(s5_error_seen, 100) && s_last_code[ZH_ASH_EV_NCP_ERROR] == 3, "S5w：主机报 NCP_ERROR（code %u）", s_last_code[ZH_ASH_EV_NCP_ERROR]);
    CHECK(run_until(session2, 3000), "S5w：复位后重新连上");

    /* S6 NCP 不回 RSTACK */
    fresh(2, 1);
    s_ncp_dead = true;
    const uint32_t t0 = s_now;
    for (int i = 0; i < 6000 && s_ev_count[ZH_ASH_EV_RESET_FAILED] == 0; i++) step();
    CHECK(s_ev_count[ZH_ASH_EV_RESET_FAILED] == 1 && s_now - t0 >= 5000 && s_now - t0 <= 5002, "S6：%u ms 后 RESET_FAILED", s_now - t0);
    s_ncp_dead = false;

    /* S7 窗口 3 的流水线 + 噪声，每步只写得下一帧。em260 一次只能处理一条命令（单接收缓冲 + 原地应答：两条在途时同一块缓冲会
       同时挂在两条队列上），所以 zhub 用窗口 1；这里换成应答另开缓冲的理想化应用层（NCP 的 ASH 照旧是真的），只为验证 k>1 的帧号簿记 */
    s_inplace = false;
    s_latency = 5;
    s_tx_every = 4;
    fresh(2, 3);
    s_cap = ZH_ASH_WIRE_MAX;
    s_noise = 1.0 / 2000;
    ok = run_commands(300, 4, 0, 3600000);
    s_noise = 0;
    s_cap = 8192;
    s_inplace = true;
    s_latency = 0;
    s_tx_every = 1;
    CHECK(ok == 300 && s_bad_resp == 0, "S7：流水线 + 噪声，300 条最终都有应答、内容对（%d，坏 %d）", ok, s_bad_resp);
    CHECK(s_dup_serial == 0 && s_ncp_dup == 0 && s_ncp_bad == 0, "S7：两边都不重复交付、命令逐字节对（重复 %d/%d，坏 %d）",
          s_dup_serial, s_ncp_dup, s_ncp_bad);
    CHECK(s_max_unacked == 3 && s_max_q >= 3 && s_retx_partial > 0, "S7：窗口撑满过（%u）、队列 %u、重传中收到过部分确认（%u 步）",
          s_max_unacked, s_max_q, s_retx_partial);

    printf("ncp_interop: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
