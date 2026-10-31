/* zh_ash_core 的真值表（docs/15 设计 8.3）。编的是产品源文件 ../zh_ash_core.c。
 * 这里的"NCP"是测试里手搓的帧；与真 NCP 状态机（SDK ash-ncp.c）的互通在 ncp_interop.c。 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "zh_ash_core.h"

static int s_fail, s_pass;
#define CHECK(cond, ...) do { if (cond) { s_pass++; } else { s_fail++; printf("❌ %s:%d ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- 回调记录 ---- */
static uint8_t  s_rx[16][ZH_ASH_DATA_MAX];
static size_t   s_rx_len[16];
static int      s_nrx;
static int      s_ev[16], s_ev_code[16], s_nev;
static bool     s_reset_in_cb;                     /* on_event 里直接 zh_ash_reset（上层收到断开就复位） */
static bool     s_send_in_cb;                      /* on_frame 里直接 zh_ash_send（收到回调就回一条命令） */
static zh_ash_t A;

static void on_frame(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    if (s_nrx < 16) { memcpy(s_rx[s_nrx], d, n); s_rx_len[s_nrx] = n; }
    s_nrx++;
    if (s_send_in_cb) { const uint8_t c[4] = { 0x09, 0x00, 0x05, 0x00 }; (void)zh_ash_send(&A, c, 4); }
}
static void on_event(void *ctx, zh_ash_event_t ev, uint8_t code)
{
    (void)ctx;
    if (s_nev < 16) { s_ev[s_nev] = ev; s_ev_code[s_nev] = code; }
    s_nev++;
    if (s_reset_in_cb && ev != ZH_ASH_EV_CONNECTED) zh_ash_reset(&A);
}
static void rec_clear(void) { s_nrx = s_nev = 0; }

/* ---- 线上字节的拆帧（测试自己的、独立于被测解码器）：把 tx 输出拆成一帧帧的原始字节（去转义，含 CRC） ---- */
typedef struct { uint8_t b[ZH_ASH_RAW_MAX + 4]; size_t n; } rawf_t;
static int split(const uint8_t *w, size_t n, rawf_t *out, int max, int *cans)
{
    int k = 0;
    size_t cur = 0;
    int esc = 0;
    if (cans) *cans = 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t b = w[i];
        if (b == ZH_ASH_CAN) { if (cans) (*cans)++; cur = 0; continue; }
        if (b == ZH_ASH_FLAG) { if (cur && k < max) { out[k].n = cur; k++; } cur = 0; continue; }
        if (b == ZH_ASH_ESC) { esc = 1; continue; }
        if (k < max && cur < sizeof(out[k].b)) out[k].b[cur++] = esc ? (uint8_t)(b ^ 0x20) : b;
        esc = 0;
    }
    return k;
}

/* 独立的 CRC（按 CCITT 定义逐位、反过来写的版本：从低到高处理多项式的另一种实现，免得测试复用被测函数） */
static uint16_t crc_ref(const uint8_t *p, size_t n)
{
    uint16_t c = 0xFFFF;
    while (n--) {
        uint8_t x = (uint8_t)((c >> 8) ^ *p++);
        x ^= (uint8_t)(x >> 4);
        c = (uint16_t)((c << 8) ^ ((uint16_t)x << 12) ^ ((uint16_t)x << 5) ^ (uint16_t)x);
    }
    return c;
}

/* 测试里的"NCP"发帧：raw = 控制 + 数据（DATA 的数据这里做随机化），按规矩编码 */
static size_t ncp_frame(uint8_t ctrl, const uint8_t *data, size_t len, uint8_t *out)
{
    uint8_t raw[ZH_ASH_RAW_MAX];
    raw[0] = ctrl;
    if (len) memcpy(raw + 1, data, len);
    if ((ctrl & 0x80) == 0) {
        uint8_t r = 0x42;
        for (size_t i = 0; i < len; i++) { raw[1 + i] ^= r; r = (r & 1) ? (uint8_t)((r >> 1) ^ 0xB8) : (uint8_t)(r >> 1); }
    }
    const uint16_t c = crc_ref(raw, 1 + len);
    raw[1 + len] = (uint8_t)(c >> 8);
    raw[2 + len] = (uint8_t)c;
    size_t n = 0;
    for (size_t i = 0; i < 3 + len; i++) {
        const uint8_t b = raw[i];
        if (b == 0x7E || b == 0x7D || b == 0x11 || b == 0x13 || b == 0x18 || b == 0x1A) { out[n++] = 0x7D; out[n++] = (uint8_t)(b ^ 0x20); }
        else out[n++] = b;
    }
    out[n++] = 0x7E;
    return n;
}

static uint8_t  W[4096];

/* 跑一次 tx，拆出帧 */
static int tx_frames(uint32_t now, rawf_t *f, int max, int *cans)
{
    const size_t n = zh_ash_tx(&A, now, W, sizeof(W));
    return split(W, n, f, max, cans);
}

static void feed(uint8_t ctrl, const uint8_t *d, size_t len, uint32_t now)
{
    uint8_t w[600];
    const size_t n = ncp_frame(ctrl, d, len, w);
    zh_ash_rx(&A, w, n, now);
}

/* 复位并连上（返回连上后 tx 吐出的那个 ACK 的控制字节） */
static uint8_t connect(uint32_t now)
{
    zh_ash_init(&A, NULL, on_frame, on_event, NULL);
    zh_ash_reset(&A);
    rawf_t f[4];
    (void)tx_frames(now, f, 4, NULL);
    const uint8_t rstack[2] = { 2, ZH_ASH_RESET_SOFTWARE };
    feed(ZH_ASH_CTRL_RSTACK, rstack, 2, now);
    int k = tx_frames(now, f, 4, NULL);
    return k == 1 ? f[0].b[0] : 0;
}

int main(void)
{
    /* ---- CRC / 随机化 / 编码金标 ---- */
    CHECK(zh_ash_crc16((const uint8_t *)"123456789", 9) == 0x29B1, "CRC-CCITT(FFFF) 的标准校验值 0x29B1（%04X）", zh_ash_crc16((const uint8_t *)"123456789", 9));
    uint8_t z[6] = {0};
    zh_ash_randomize(z, 6);
    CHECK(z[0] == 0x42 && z[1] == 0x21 && z[2] == 0xA8 && z[3] == 0x54 && z[4] == 0x2A && z[5] == 0x15, "LFSR 序列 42 21 A8 54 2A 15");
    uint8_t w[32];
    size_t n = zh_ash_encode(ZH_ASH_CTRL_RST, NULL, 0, w, sizeof(w));
    CHECK(n == 4 && w[0] == 0xC0 && w[1] == 0x38 && w[2] == 0xBC && w[3] == 0x7E, "RST 编成 C0 38 BC 7E（常见的 1A C0 38 BC 7E 去掉前导 CAN）");
    CHECK(zh_ash_encode(0x00, (const uint8_t *)"ab", 2, w, 8) == 0, "缓冲不够写最坏的一帧：一个字节都不写");
    CHECK(zh_ash_encode(0x00, NULL, 3, w, sizeof(w)) == 0, "有长度没数据：拒");

    /* 每个保留字节都转义、0xFF 不转义 */
    {
        uint8_t raw[] = { 0x7E, 0x7D, 0x11, 0x13, 0x18, 0x1A, 0xFF, 0x20 };
        uint8_t big[64];
        /* 用 ACK 控制字节（不随机化）不行：ACK 没数据。拿 DATA，先反随机化，编出来数据域就是 raw 本身 */
        uint8_t pre[sizeof(raw)];
        memcpy(pre, raw, sizeof(raw));
        zh_ash_randomize(pre, sizeof(pre));
        n = zh_ash_encode(0x00, pre, sizeof(pre), big, sizeof(big));
        size_t esc = 0, ff = 0;
        for (size_t i = 0; i + 1 < n; i++) { if (big[i] == 0x7D) esc++; if (big[i] == 0xFF) ff++; }
        CHECK(esc >= 6 && ff >= 1, "6 个保留字节转义（%zu）、0xFF 原样（%zu）", esc, ff);
    }

    /* ---- 复位与连接 ---- */
    rec_clear();
    zh_ash_init(&A, NULL, on_frame, on_event, NULL);
    CHECK(!zh_ash_send(&A, (const uint8_t *)"abc", 3), "没连上：拒收");
    zh_ash_reset(&A);
    rawf_t f[8];
    int cans = 0;
    int k = tx_frames(0, f, 8, &cans);
    CHECK(k == 1 && cans == 1 && f[0].n == 3 && f[0].b[0] == 0xC0, "复位：先 CAN 再 RST");
    CHECK(tx_frames(10, f, 8, NULL) == 0, "等 RSTACK 期间什么都不发");
    uint8_t rs_bad[2] = { 2, 2 };                     /* 上电复位的 RSTACK：复位之前的旧货 */
    feed(ZH_ASH_CTRL_RSTACK, rs_bad, 2, 20);
    CHECK(!zh_ash_connected(&A) && s_nev == 0, "复位原因不是 SOFTWARE：不算连上、接着等");
    uint8_t rs[2] = { 2, ZH_ASH_RESET_SOFTWARE };
    feed(ZH_ASH_CTRL_RSTACK, rs, 2, 30);
    CHECK(zh_ash_connected(&A) && s_nev == 1 && s_ev[0] == ZH_ASH_EV_CONNECTED, "合格的 RSTACK：连上");
    k = tx_frames(30, f, 8, NULL);
    CHECK(k == 1 && f[0].b[0] == 0x80, "连上后回一个 ACK（ackNum 0）");

    rec_clear();
    zh_ash_init(&A, NULL, on_frame, on_event, NULL);
    zh_ash_reset(&A);
    (void)tx_frames(0, f, 8, NULL);
    uint8_t rs_v[2] = { 3, ZH_ASH_RESET_SOFTWARE };
    feed(ZH_ASH_CTRL_RSTACK, rs_v, 2, 5);
    CHECK(s_nev == 1 && s_ev[0] == ZH_ASH_EV_VERSION && s_ev_code[0] == 3 && !zh_ash_connected(&A), "ASH 版本不是 2：报 VERSION、不连");

    rec_clear();
    zh_ash_init(&A, NULL, on_frame, on_event, NULL);
    zh_ash_reset(&A);
    (void)tx_frames(0, f, 8, NULL);
    (void)tx_frames(4999, f, 8, NULL);
    CHECK(s_nev == 0, "5000 ms 之内不判复位失败");
    (void)tx_frames(5000, f, 8, NULL);
    CHECK(s_nev == 1 && s_ev[0] == ZH_ASH_EV_RESET_FAILED && A.state == ZH_ASH_DOWN, "5000 ms 没等到 RSTACK：RESET_FAILED");

    rec_clear();
    zh_ash_init(&A, NULL, on_frame, on_event, NULL);
    zh_ash_reset(&A);
    (void)tx_frames(0, f, 8, NULL);
    uint8_t er[2] = { 2, 0x51 };
    feed(ZH_ASH_CTRL_ERROR, er, 2, 1);
    CHECK(s_nev == 1 && s_ev[0] == ZH_ASH_EV_NCP_ERROR && s_ev_code[0] == 0x51, "等 RSTACK 时来 ERROR：报 NCP_ERROR 带错误码");

    /* ---- 发数据、确认、自适应定时器 ---- */
    rec_clear();
    CHECK(connect(0) == 0x80, "连上");
    uint8_t cmd[5] = { 0x01, 0x00, 0x01, 0x00, 0x00 };
    CHECK(zh_ash_send(&A, cmd, 5), "排一帧");
    CHECK(!zh_ash_send(&A, cmd, 2) && !zh_ash_send(&A, cmd, 221), "长度 < 3 或 > 220：拒");
    k = tx_frames(100, f, 8, NULL);
    CHECK(k == 1 && f[0].b[0] == 0x00 && f[0].n == 1 + 5 + 2, "DATA 帧：frm 0、ack 0");
    uint8_t got[5];
    memcpy(got, f[0].b + 1, 5);
    zh_ash_randomize(got, 5);
    CHECK(memcmp(got, cmd, 5) == 0 && crc_ref(f[0].b, 6) == (uint16_t)((f[0].b[6] << 8) | f[0].b[7]), "数据域是随机化过的明文、CRC 对");
    CHECK(zh_ash_send(&A, cmd, 5), "再排一帧");
    CHECK(tx_frames(110, f, 8, NULL) == 0, "窗口 1：上一帧没确认，第二帧不发");
    feed(0x81, NULL, 0, 300);                          /* ACK，ackNum 1，往返 200 ms */
    CHECK(A.ack_period == (800 * 7 + 200 * 4) / 8, "往返 200 ms：周期 (7×800 + 4×200)/8 = %u（%u）", (800 * 7 + 200 * 4) / 8, A.ack_period);
    k = tx_frames(300, f, 8, NULL);
    CHECK(k == 1 && f[0].b[0] == 0x10, "确认后第二帧发出：frm 1、ack 0");
    feed(0x82, NULL, 0, 320);
    CHECK(zh_ash_queued(&A) == 0, "两帧都确认了：队列空");

    /* ---- 收数据：按序交出、回 ACK；0xFF 与 XON / WAKE ---- */
    rec_clear();
    uint8_t rsp[6] = { 0x01, 0x80, 0x01, 0x00, 0x00, 0xFF };
    {
        uint8_t ww[64];
        uint8_t pre[6];
        memcpy(pre, rsp, 6);
        size_t m = 0;
        ww[m++] = 0xFF;                                /* 帧前的 WAKE：丢掉 */
        ww[m++] = 0x11;                                /* 裸 XON：丢掉 */
        m += ncp_frame(0x02, pre, 6, ww + m);          /* NCP 的 DATA frm 0、ack 2 */
        zh_ash_rx(&A, ww, m, 400);
    }
    CHECK(s_nrx == 1 && s_rx_len[0] == 6 && memcmp(s_rx[0], rsp, 6) == 0, "按序的 DATA 去随机化后交出（末字节 0xFF 不丢）");
    k = tx_frames(400, f, 8, NULL);
    CHECK(k == 1 && f[0].b[0] == 0x81, "回 ACK（ackNum 1）");
    feed(0x02, rsp, 6, 410);                           /* 同一帧不带 rFlag 又来：乱序 */
    k = tx_frames(410, f, 8, NULL);
    CHECK(s_nrx == 1 && k == 1 && f[0].b[0] == 0xA1 && A.st.rx_oos == 1, "乱序（frm 0 又来、没 rFlag）：不交出、回 NAK");
    feed(0x22, rsp, 6, 411);                           /* frm 2：还是乱序 */
    CHECK(tx_frames(411, f, 8, NULL) == 0, "拒收状态里再乱序：不重复 NAK");
    feed(0x0A, rsp, 6, 420);                           /* frm 0 带 rFlag：重复的重传 */
    k = tx_frames(420, f, 8, NULL);
    CHECK(s_nrx == 1 && k == 1 && f[0].b[0] == 0x81 && A.st.rx_dup == 1, "重复的重传帧：内容丢掉、回 ACK");
    feed(0x1A, rsp, 6, 430);                           /* frm 1 带 rFlag：正是期望的那帧 */
    k = tx_frames(430, f, 8, NULL);
    CHECK(s_nrx == 2 && k == 1 && f[0].b[0] == 0x82 && !(A.flags & 0x0010), "期望的帧（带 rFlag）：交出、回 ACK、出拒收状态");

    /* ---- 坏帧：CRC / SUB / 太长 / 太短 / CAN ---- */
    rec_clear();
    {
        uint8_t ww[600];
        size_t m = ncp_frame(0x22, rsp, 6, ww);
        ww[2] ^= 0x01;                                 /* 弄坏一个数据字节 */
        zh_ash_rx(&A, ww, m, 500);
    }
    k = tx_frames(500, f, 8, NULL);
    CHECK(A.st.rx_crc == 1 && s_nrx == 0 && k == 1 && f[0].b[0] == 0xA2, "CRC 错的 DATA：丢、回 NAK");
    feed(0x22, rsp, 6, 510);                           /* 正常的 frm 2 来了 */
    (void)tx_frames(510, f, 8, NULL);
    CHECK(s_nrx == 1, "之后按序的帧照收");
    {
        uint8_t ww[600];
        size_t m = ncp_frame(0x32, rsp, 6, ww);
        uint8_t w2[620];
        memcpy(w2, ww, 3);
        w2[3] = 0x18;                                  /* 中间插个 SUB */
        memcpy(w2 + 4, ww + 3, m - 3);
        zh_ash_rx(&A, w2, m + 1, 520);
    }
    k = tx_frames(520, f, 8, NULL);
    CHECK(A.st.rx_comm == 1 && s_nrx == 1 && k == 1 && f[0].b[0] == 0xA3, "带 SUB 的帧：作废、回 NAK");
    {
        uint8_t ww[600];
        size_t m = ncp_frame(0x32, rsp, 6, ww);
        uint8_t w2[700];
        memcpy(w2, ww, 4);
        w2[4] = 0x1A;                                  /* CAN：取消半截帧 */
        memcpy(w2 + 5, ww, m);                         /* 然后完整地重发 */
        zh_ash_rx(&A, w2, m + 5, 530);
    }
    CHECK(s_nrx == 2 && A.st.rx_cancelled == 1, "CAN 取消半截帧（不算错），后面完整的帧照收");
    {
        uint8_t ww[1200];
        size_t m = 0;
        uint8_t junk[300];
        memset(junk, 0x55, sizeof(junk));
        for (size_t i = 0; i < sizeof(junk); i++) ww[m++] = junk[i];
        ww[m++] = 0x7E;
        zh_ash_rx(&A, ww, m, 540);
        uint8_t sh[3] = { 0x42, 0x01, 0x7E };          /* 2 个字节就 FLAG：太短 */
        zh_ash_rx(&A, sh, 3, 541);
    }
    CHECK(A.st.rx_long == 1 && A.st.rx_short == 1, "太长（%u）、太短（%u）都计数", A.st.rx_long, A.st.rx_short);
    (void)tx_frames(545, f, 8, NULL);

    /* ---- 重传：NAK 触发、超时触发、超时到上限 ---- */
    rec_clear();
    CHECK(connect(0) == 0x80, "重连");
    CHECK(zh_ash_send(&A, cmd, 5), "排一帧");
    k = tx_frames(1000, f, 8, NULL);
    CHECK(k == 1 && f[0].b[0] == 0x00, "发出 frm 0");
    feed(0xA0, NULL, 0, 1100);                         /* NCP 回 NAK（ackNum 0） */
    int cn0 = 0;
    k = tx_frames(1100, f, 8, &cn0);
    CHECK(k == 1 && f[0].b[0] == (0x00 | 0x08) && cn0 == 1, "NAK：先 CAN、再重传 frm 0、带 rFlag（%02X，CAN %d）", k ? f[0].b[0] : 0, cn0);
    k = tx_frames(1100 + 799, f, 8, NULL);
    CHECK(k == 0, "周期内不超时");
    k = tx_frames(1100 + 800, f, 8, NULL);
    CHECK(k == 1 && f[0].b[0] == 0x08 && A.st.ack_timeouts == 1 && A.ack_period == 1600, "超时：重传、周期翻倍到 1600");
    uint32_t t = 1900;
    for (int i = 0; i < 4; i++) { t += A.ack_period; (void)tx_frames(t, f, 8, NULL); }
    CHECK(A.st.ack_timeouts == 5 && s_nev == 1 && A.ack_period == 2400, "再超时 4 次（共 5）：还没断、周期夹在 2400（%u）", A.ack_period);
    t += A.ack_period;
    (void)tx_frames(t, f, 8, NULL);
    CHECK(s_nev == 2 && s_ev[1] == ZH_ASH_EV_TIMEOUTS && A.state == ZH_ASH_DOWN, "第 6 次超时：判链路断（TIMEOUTS）");
    CHECK(!zh_ash_send(&A, cmd, 5), "断了之后拒收");

    rec_clear();
    CHECK(connect(0) == 0x80, "重连");
    CHECK(zh_ash_send(&A, cmd, 5), "排一帧");
    (void)tx_frames(10, f, 8, NULL);
    t = 10 + 800;
    (void)tx_frames(t, f, 8, NULL);                    /* 一次超时、重传 */
    feed(0x81, NULL, 0, t + 50);                       /* 重传那帧被确认 */
    k = tx_frames(t + 60, f, 8, NULL);
    CHECK(A.timeouts == 0 && zh_ash_queued(&A) == 0 && !(A.flags & 0x0020) && k == 0, "重传被确认：超时计数清零、出重传、队列空");

    /* ---- 窗口外的 ackNum、连上后的 RSTACK / ERROR ---- */
    rec_clear();
    CHECK(connect(0) == 0x80, "重连");
    feed(0x05, rsp, 6, 10);                            /* DATA frm 0、ack 5：窗口外（frm_tx 0） */
    k = tx_frames(10, f, 8, NULL);
    CHECK(A.st.rx_bad_ack == 1 && s_nrx == 0 && k == 1 && f[0].b[0] == 0xA0, "ackNum 在窗口外：当坏帧、回 NAK");
    feed(ZH_ASH_CTRL_RSTACK, rs, 2, 20);
    CHECK(s_nev == 2 && s_ev[1] == ZH_ASH_EV_NCP_RESET && s_ev_code[1] == ZH_ASH_RESET_SOFTWARE && A.state == ZH_ASH_DOWN,
          "连上后又来 RSTACK：NCP 自己重启了（NCP_RESET）");
    rec_clear();
    CHECK(connect(0) == 0x80, "重连");
    feed(ZH_ASH_CTRL_ERROR, er, 2, 20);
    CHECK(s_nev == 2 && s_ev[1] == ZH_ASH_EV_NCP_ERROR, "连上后来 ERROR：NCP_ERROR");

    /* ---- 回调里重入：on_event 里直接 reset（rx 中途、tx 超时路径）、on_frame 里直接 send，同一缓冲后面还跟着帧 ---- */
    rec_clear();
    CHECK(connect(0) == 0x80, "重连");
    s_reset_in_cb = true;
    {
        uint8_t ww[64];
        size_t m = ncp_frame(ZH_ASH_CTRL_RSTACK, rs, 2, ww);
        m += ncp_frame(0x00, rsp, 6, ww + m);          /* 复位后紧跟一个 DATA：复位中只认 RSTACK / ERROR，应当不交出 */
        zh_ash_rx(&A, ww, m, 20);
    }
    int cn = 0;
    k = tx_frames(30, f, 8, &cn);
    CHECK(s_ev[1] == ZH_ASH_EV_NCP_RESET && A.state == ZH_ASH_RESETTING && s_nrx == 0 && k == 1 && cn == 1 && f[0].b[0] == 0xC0,
          "on_event 里复位：剩下的字节按复位中处理（DATA 不交出），随后 CAN + RST");
    feed(ZH_ASH_CTRL_RSTACK, rs, 2, 40);
    (void)tx_frames(40, f, 8, NULL);
    CHECK(zh_ash_send(&A, cmd, 5), "排一帧");
    (void)tx_frames(50, f, 8, NULL);
    t = 50;
    for (int i = 0; i < 6 && A.state == ZH_ASH_CONNECTED; i++) { t += A.ack_period; (void)tx_frames(t, f, 8, NULL); }
    CHECK(A.state == ZH_ASH_RESETTING && zh_ash_tx_pending(&A), "tx 超时路径里 on_event 复位：状态是复位中、有待发的 RST");
    s_reset_in_cb = false;
    rec_clear();
    CHECK(connect(0) == 0x80, "重连");
    s_send_in_cb = true;
    {
        uint8_t ww[128];
        size_t m = ncp_frame(0x00, rsp, 6, ww);        /* NCP 的 frm 0 */
        m += ncp_frame(0x10, rsp, 6, ww + m);          /* 紧跟 frm 1 */
        zh_ash_rx(&A, ww, m, 60);
    }
    k = tx_frames(60, f, 8, NULL);
    CHECK(s_nrx == 2 && zh_ash_queued(&A) == 2 && k == 2 && f[0].b[0] == 0x82 && f[1].b[0] == 0x02,
          "on_frame 里 send：两帧都交出、两条命令都排上；先回 ACK 再发第一条（窗口 1）");
    s_send_in_cb = false;

    /* ---- tx_pending / next_timeout：有待发的不许按定时器睡 ---- */
    zh_ash_init(&A, NULL, on_frame, on_event, NULL);
    zh_ash_reset(&A);
    CHECK(zh_ash_tx_pending(&A) && zh_ash_next_timeout_ms(&A, 0) == 0, "复位后有 RST 待发：next_timeout 是 0（不是 UINT32_MAX）");
    (void)tx_frames(0, f, 8, NULL);
    CHECK(!zh_ash_tx_pending(&A) && zh_ash_next_timeout_ms(&A, 100) == 4900, "RST 发出后：按 RSTACK 定时器睡（还剩 4900）");
    feed(ZH_ASH_CTRL_RSTACK, rs, 2, 100);
    CHECK(zh_ash_tx_pending(&A), "连上后要回 ACK：待发");
    (void)tx_frames(100, f, 8, NULL);
    CHECK(!zh_ash_tx_pending(&A) && zh_ash_next_timeout_ms(&A, 100) == UINT32_MAX, "什么都不欠：UINT32_MAX");
    CHECK(zh_ash_send(&A, cmd, 5) && zh_ash_next_timeout_ms(&A, 100) == 0, "send 之后：next_timeout 0");

    /* ---- 第一轮审计补的五行（R1–R5）：每行钉住一个能过旧测试的变异 ---- */
    zh_ash_init(&A, NULL, NULL, NULL, NULL);
    zh_ash_reset(&A);
    n = zh_ash_tx(&A, 0, W, sizeof(W));
    CHECK(n == 5 && W[0] == 0x1A && W[1] == 0xC0 && W[2] == 0x38 && W[3] == 0xBC && W[4] == 0x7E, "R1：复位写出的正是 1A C0 38 BC 7E（CAN 在 RST 之前）");
    {
        zh_ash_cfg_t kc;
        zh_ash_cfg_default(&kc);
        const uint8_t d5[5] = { 1, 2, 3, 4, 5 };
        kc.tx_k = 1;
        zh_ash_init(&A, &kc, on_frame, NULL, NULL);
        zh_ash_reset(&A); (void)tx_frames(0, f, 8, NULL); feed(ZH_ASH_CTRL_RSTACK, rs, 2, 0); (void)tx_frames(0, f, 8, NULL);
        CHECK(zh_ash_send(&A, d5, 5), "R2 排一帧");
        feed(0x81, NULL, 0, 1);                        /* ACK(1)：可那帧还没发出去 */
        CHECK(A.st.rx_bad_ack == 1 && zh_ash_queued(&A) == 1, "R2：确认一个还没发的帧 = 窗口外，排着的帧不许被当成已确认放掉");
        CHECK(tx_frames(2, f, 8, NULL) == 1 && f[0].b[0] == 0x00, "R2：那帧照样发出");
        rec_clear();
        zh_ash_init(&A, &kc, on_frame, NULL, NULL);
        zh_ash_reset(&A); (void)tx_frames(0, f, 8, NULL); feed(ZH_ASH_CTRL_RSTACK, rs, 2, 0); (void)tx_frames(0, f, 8, NULL);
        feed(0x00, d5, 2, 1);                          /* 只有 2 个数据字节的 DATA */
        k = tx_frames(1, f, 8, NULL);
        CHECK(s_nrx == 0 && A.st.rx_bad_len == 1 && k == 1 && f[0].b[0] == 0xA0, "R3：2 字节的 DATA 长度非法：不交出、回 NAK");
        kc.tx_k = 2;
        zh_ash_init(&A, &kc, on_frame, NULL, NULL);
        zh_ash_reset(&A); (void)tx_frames(0, f, 8, NULL); feed(ZH_ASH_CTRL_RSTACK, rs, 2, 0); (void)tx_frames(0, f, 8, NULL);
        zh_ash_send(&A, d5, 5); zh_ash_send(&A, d5, 5);
        CHECK(tx_frames(0, f, 8, NULL) == 2 && A.frm_tx == 2, "R4：窗口 2，两帧在途");
        feed(0x81, NULL, 0, 100);                      /* 只确认了第一帧 */
        CHECK(zh_ash_next_timeout_ms(&A, 100) != UINT32_MAX, "R4：部分确认后定时器还得为第二帧跑着");
        (void)tx_frames(100 + 2400, f, 8, NULL);
        CHECK(A.st.tx_redata == 1, "R4：第二帧到期重传（%u）", A.st.tx_redata);
        zh_ash_init(&A, &kc, on_frame, NULL, NULL);
        zh_ash_reset(&A); (void)tx_frames(0, f, 8, NULL); feed(ZH_ASH_CTRL_RSTACK, rs, 2, 0); (void)tx_frames(0, f, 8, NULL);
        zh_ash_send(&A, d5, 5); zh_ash_send(&A, d5, 5);
        (void)tx_frames(0, f, 8, NULL);
        (void)zh_ash_tx(&A, 800, W, ZH_ASH_WIRE_MAX + 1);   /* 超时：每次只写得下一帧 */
        (void)zh_ash_tx(&A, 801, W, ZH_ASH_WIRE_MAX);
        CHECK(A.st.tx_redata == 2 && (A.flags & 0x0020), "R5：两帧都重传了、还在重传状态（%u，flags %#x）", A.st.tx_redata, A.flags);
        feed(0x81, NULL, 0, 850);                      /* 重传中只确认了第一帧 */
        (void)tx_frames(851, f, 8, NULL);
        CHECK(zh_ash_next_timeout_ms(&A, 851) != UINT32_MAX, "R5：重传结束后第二帧还没确认，定时器得跑着（不然只能等 NAK 救）");

        /* R6：重传中两帧都被确认了 —— 重传结束那一刻要把已确认的帧放掉，否则队列被已确认的帧占满、再也排不进 */
        zh_ash_init(&A, &kc, on_frame, NULL, NULL);
        zh_ash_reset(&A); (void)tx_frames(0, f, 8, NULL); feed(ZH_ASH_CTRL_RSTACK, rs, 2, 0); (void)tx_frames(0, f, 8, NULL);
        zh_ash_send(&A, d5, 5); zh_ash_send(&A, d5, 5);
        (void)tx_frames(0, f, 8, NULL);
        (void)zh_ash_tx(&A, 800, W, ZH_ASH_WIRE_MAX + 1);   /* 超时：这次只写得下 CAN + 第一帧的重传 */
        CHECK((A.flags & 0x0020) && A.frm_retx == 1, "R6：重传进行中（frm_retx %u）", A.frm_retx);
        feed(0x82, NULL, 0, 810);                      /* 重传中途：两帧都确认了 */
        (void)tx_frames(811, f, 8, NULL);
        CHECK(!(A.flags & 0x0020) && zh_ash_queued(&A) == 0, "R6：重传中途全确认、重传随即结束：队列清空（%u）", zh_ash_queued(&A));

        /* R7：开始重传后、还没发出重传帧时对方确认了第一帧 —— 只重传第二帧，别把已确认的再发一遍 */
        zh_ash_init(&A, &kc, on_frame, NULL, NULL);
        zh_ash_reset(&A); (void)tx_frames(0, f, 8, NULL); feed(ZH_ASH_CTRL_RSTACK, rs, 2, 0); (void)tx_frames(0, f, 8, NULL);
        zh_ash_send(&A, d5, 5); zh_ash_send(&A, d5, 5);
        (void)tx_frames(0, f, 8, NULL);
        feed(0xA0, NULL, 0, 10);                       /* NAK(0)：开始重传 */
        feed(0x81, NULL, 0, 11);                       /* 紧接着确认了第一帧 */
        k = tx_frames(12, f, 8, NULL);
        CHECK(k == 1 && ((f[0].b[0] >> 4) & 7) == 1 && (f[0].b[0] & 0x08), "R7：只重传 frm 1（%02X），已确认的 frm 0 不再发", k ? f[0].b[0] : 0);
    }

    /* R8：复位时清掉解码器里的半截帧 —— 否则紧跟来的 RSTACK（前面没有 CAN 时）会接在垃圾后面、CRC 不对被丢 */
    zh_ash_init(&A, NULL, on_frame, on_event, NULL);
    {
        const uint8_t junk[4] = { 0x00, 0x42, 0x21, 0xA8 };   /* 半截 DATA，没有 FLAG */
        zh_ash_rx(&A, junk, 4, 0);
    }
    zh_ash_reset(&A);
    (void)tx_frames(0, f, 8, NULL);
    rec_clear();
    feed(ZH_ASH_CTRL_RSTACK, rs, 2, 5);                /* 不带前导 CAN */
    CHECK(zh_ash_connected(&A), "R8：复位清了解码器：不带 CAN 的 RSTACK 也认");

    /* R9：带数据的 ACK（长度不对，CRC 却对）不当 ACK：不推进确认、不放队头 */
    CHECK(connect(0) == 0x80, "重连");
    CHECK(zh_ash_send(&A, cmd, 5), "排一帧");
    (void)tx_frames(10, f, 8, NULL);
    {
        const uint8_t two[2] = { 0x11, 0x22 };
        feed(0x81, two, 2, 20);
    }
    CHECK(A.st.rx_bad_len == 1 && zh_ash_queued(&A) == 1 && A.ack_rx == 0, "R9：带数据的 ACK 长度非法：确认不推进（队列 %u、ack_rx %u）",
          zh_ash_queued(&A), A.ack_rx);

    /* ---- t_stop：zh_ash_stop 回 DOWN、不报事件、不清统计、什么都不发（编排层退避期间用） ---- */
    {
        CHECK(connect(0) == 0x80, "t_stop：连上");
        CHECK(zh_ash_send(&A, cmd, 5), "t_stop：排一帧");
        (void)tx_frames(10, f, 8, NULL);
        const uint32_t resets0 = A.st.resets, tx0 = A.st.tx_data;
        const uint8_t half[3] = { 0x7E, 0x00, 0x42 };   /* 先吞一个 FLAG，再留半截 DATA 在解码器里 */
        zh_ash_rx(&A, half, 3, 15);
        rec_clear();
        zh_ash_stop(&A);
        CHECK(A.state == ZH_ASH_DOWN && s_nev == 0, "t_stop：回 DOWN、不报事件（%d 个）", s_nev);
        CHECK(A.st.resets == resets0 && A.st.tx_data == tx0, "t_stop：统计保留");
        CHECK(zh_ash_queued(&A) == 0 && A.rx_len == 0 && !A.ack_running, "t_stop：队列、半截帧、定时器都清掉");
        CHECK(!zh_ash_send(&A, cmd, 5), "t_stop：停下后拒收");
        CHECK(tx_frames(100000, f, 8, NULL) == 0 && s_nev == 0, "t_stop：过了多久都什么都不发、也不报超时");
        CHECK(zh_ash_next_timeout_ms(&A, 100000) == UINT32_MAX && !zh_ash_tx_pending(&A), "t_stop：没有定时器、没有待发");
        const uint8_t rst_ok[2] = { 2, ZH_ASH_RESET_SOFTWARE };
        feed(ZH_ASH_CTRL_RSTACK, rst_ok, 2, 100001);
        CHECK(!zh_ash_connected(&A) && s_nev == 0, "t_stop：停着时来的 RSTACK 不算连上");
        zh_ash_reset(&A);
        k = tx_frames(100002, f, 8, &cans);
        CHECK(k == 1 && f[0].b[0] == 0xC0, "t_stop：之后 zh_ash_reset 照常发 RST");
        zh_ash_stop(NULL);                               /* NULL 不崩 */
    }

    /* ---- 配置清理 ---- */
    zh_ash_cfg_t c = { .ack_init_ms = 10, .ack_min_ms = 0, .ack_max_ms = 5, .rst_timeout_ms = 0, .max_timeouts = 0, .tx_k = 9 };
    zh_ash_init(&A, &c, NULL, NULL, NULL);
    CHECK(A.cfg.tx_k == ZH_ASH_TXQ - 1 && A.cfg.ack_min_ms >= 1 && A.cfg.ack_max_ms >= A.cfg.ack_min_ms && A.cfg.ack_init_ms <= A.cfg.ack_max_ms
          && A.cfg.max_timeouts >= 1, "荒唐的配置被夹到合法范围");
    CHECK(zh_ash_next_timeout_ms(&A, 0) == UINT32_MAX, "没有在跑的定时器：UINT32_MAX");

    printf("zh_ash: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
