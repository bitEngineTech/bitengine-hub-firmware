/* muse_frame 主机测试：WebSocket 增量解析与控制流消息重组。编的是产品源文件 ../muse_frame.c。
 * 服务端帧用测试里独立写的编码器造（不复用 mws_encode_header），按任意切块方式喂进去，结果必须一样。 */
#include "muse_frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) { g_pass++; } else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* 独立的服务端帧编码：FIN + opcode，可选掩码，长度按 RFC 6455 最短形式 */
static size_t ref_frame(uint8_t *out, uint8_t b0, const uint8_t *p, size_t len, const uint8_t *mask)
{
    size_t o = 0;
    out[o++] = b0;
    uint8_t mbit = mask ? 0x80 : 0;
    if (len < 126) out[o++] = (uint8_t)(mbit | len);
    else if (len < 65536) { out[o++] = mbit | 126; out[o++] = (uint8_t)(len >> 8); out[o++] = (uint8_t)len; }
    else { out[o++] = mbit | 127; for (int i = 7; i >= 0; i--) out[o++] = (uint8_t)((uint64_t)len >> (8 * i)); }
    if (mask) { memcpy(out + o, mask, 4); o += 4; }
    for (size_t i = 0; i < len; i++) out[o + i] = (uint8_t)(p[i] ^ (mask ? mask[i & 3] : 0));
    return o + len;
}

/* 把 stream 按随机块大小喂完，收集所有事件 */
typedef struct { mws_ev_t ev; size_t len; uint8_t first, last; } got_t;
static int feed_all(const uint8_t *s, size_t n, size_t cap, got_t *out, int max, unsigned seed, int chunk_mode)
{
    static uint8_t buf[200000];
    mws_rx_t rx;
    mws_rx_init(&rx);
    srand(seed);
    size_t off = 0;
    int k = 0;
    int guard = 0;
    while (off < n && k < max && guard++ < 1000000) {
        size_t chunk = chunk_mode == 0 ? 1 : chunk_mode == 1 ? n - off : 1 + (size_t)(rand() % 300);
        if (chunk > n - off) chunk = n - off;
        size_t used = 0, fl = 0;
        mws_ev_t e = mws_feed(&rx, s + off, chunk, &used, buf, cap, &fl);
        if (used > chunk) { out[k++] = (got_t){ MWS_ERR_PROTOCOL, 99999, 0, 0 }; return k; }
        off += used;
        if (e != MWS_NEED_MORE) {
            out[k++] = (got_t){ e, fl, fl ? buf[0] : 0, fl ? buf[fl - 1] : 0 };
            if (e >= MWS_CLOSE) return k;
        } else if (used == 0 && chunk > 0) {
            out[k++] = (got_t){ MWS_ERR_PROTOCOL, 88888, 0, 0 };   /* 没进展：死循环 */
            return k;
        }
    }
    return k;
}

static void test_encode(void)
{
    uint8_t h[MWS_HDR_MAX], m[4] = { 1, 2, 3, 4 };
    CHECK(mws_encode_header(h, MWS_OP_BINARY, 0, m) == 6 && h[0] == 0x82 && h[1] == 0x80 && memcmp(h + 2, m, 4) == 0, "len 0");
    CHECK(mws_encode_header(h, MWS_OP_BINARY, 125, m) == 6 && h[1] == (0x80 | 125), "len 125");
    CHECK(mws_encode_header(h, MWS_OP_BINARY, 126, m) == 8 && h[1] == (0x80 | 126) && h[2] == 0 && h[3] == 126, "len 126");
    CHECK(mws_encode_header(h, MWS_OP_BINARY, 65535, m) == 8 && h[2] == 0xFF && h[3] == 0xFF, "len 65535");
    CHECK(mws_encode_header(h, MWS_OP_BINARY, 65536, m) == 14 && h[1] == (0x80 | 127) && h[7] == 1 && h[8] == 0 && h[9] == 0, "len 65536");
    CHECK(mws_encode_header(h, MWS_OP_PING, 0, m) == 6 && h[0] == 0x89, "ping");
    CHECK(mws_encode_header(h, MWS_OP_PONG, 5, m) == 6 && h[0] == 0x8A && h[1] == 0x85, "pong");
    CHECK(mws_encode_header(h, MWS_OP_TEXT, 1, m) == 0 && mws_encode_header(h, MWS_OP_CONT, 1, m) == 0 && mws_encode_header(h, 3, 1, m) == 0, "bad opcodes");
    CHECK(mws_encode_header(h, MWS_OP_PING, 126, m) == 0, "control > 125");
    CHECK(mws_encode_header(h, MWS_OP_BINARY, 1ull << 63, m) == 0, "len high bit");
    CHECK(mws_encode_header(NULL, MWS_OP_BINARY, 1, m) == 0 && mws_encode_header(h, MWS_OP_BINARY, 1, NULL) == 0, "NULLs");
    /* 客户端帧：编码 + 掩码，再用解析器解回来（解析器也收带掩码的帧） */
    static uint8_t payload[70000], frame[70100], out[70000];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(i * 7 + 3);
    size_t sizes[] = { 0, 1, 125, 126, 1000, 65535, 65536, 70000 };
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        size_t hl = mws_encode_header(frame, MWS_OP_BINARY, sizes[s], m);
        memcpy(frame + hl, payload, sizes[s]);
        mws_mask(frame + hl, sizes[s], m);
        mws_rx_t rx; mws_rx_init(&rx);
        size_t used, fl;
        mws_ev_t e = mws_feed(&rx, frame, hl + sizes[s], &used, out, sizeof(out), &fl);
        CHECK(e == MWS_BINARY && used == hl + sizes[s] && fl == sizes[s] && memcmp(out, payload, fl) == 0, "client round trip %zu", sizes[s]);
    }
}

static void test_parse_chunking(void)
{
    /* 三帧连在一起：binary(300 字节) + ping("hi") + binary(70000，带掩码)，外加一个空 binary 与 pong */
    static uint8_t s[80000], p[70000];
    for (size_t i = 0; i < sizeof(p); i++) p[i] = (uint8_t)(i ^ 0x5A);
    uint8_t mk[4] = { 9, 8, 7, 6 };
    size_t n = 0;
    n += ref_frame(s + n, 0x82, p, 300, NULL);
    n += ref_frame(s + n, 0x89, (const uint8_t *)"hi", 2, NULL);
    n += ref_frame(s + n, 0x82, p, 0, NULL);
    n += ref_frame(s + n, 0x82, p, 70000, mk);
    n += ref_frame(s + n, 0x8A, (const uint8_t *)"", 0, NULL);
    for (int mode = 0; mode < 3; mode++) {
        for (unsigned seed = 1; seed <= (mode == 2 ? 50u : 1u); seed++) {
            got_t g[8];
            int k = feed_all(s, n, 100000, g, 8, seed, mode);
            bool ok = k == 5 && g[0].ev == MWS_BINARY && g[0].len == 300 && g[0].first == p[0] && g[0].last == p[299]
                      && g[1].ev == MWS_PING && g[1].len == 2 && g[1].first == 'h' && g[1].last == 'i'
                      && g[2].ev == MWS_BINARY && g[2].len == 0
                      && g[3].ev == MWS_BINARY && g[3].len == 70000 && g[3].first == p[0] && g[3].last == p[69999]
                      && g[4].ev == MWS_PONG && g[4].len == 0;
            CHECK(ok, "chunk mode %d seed %u: k=%d", mode, seed, k);
        }
    }
}

static mws_ev_t one(const uint8_t *s, size_t n, size_t cap)
{
    static uint8_t buf[1000];
    mws_rx_t rx; mws_rx_init(&rx);
    size_t used, fl;
    mws_ev_t e = mws_feed(&rx, s, n, &used, buf, cap, &fl);
    /* 报错之后再喂：同一个错误、一个字节都不吃 */
    if (e >= MWS_CLOSE) {
        mws_ev_t e2 = mws_feed(&rx, s, n, &used, buf, cap, &fl);
        if (e2 != e || used != 0) return (mws_ev_t)99;
    }
    return e;
}

static void test_errors(void)
{
    uint8_t f[16];
    f[0] = 0xC2; f[1] = 0; CHECK(one(f, 2, 100) == MWS_ERR_PROTOCOL, "RSV1 set");
    f[0] = 0x02; f[1] = 0; CHECK(one(f, 2, 100) == MWS_ERR_PROTOCOL, "FIN clear (fragment)");
    f[0] = 0x81; f[1] = 0; CHECK(one(f, 2, 100) == MWS_ERR_PROTOCOL, "text frame");
    f[0] = 0x80; f[1] = 0; CHECK(one(f, 2, 100) == MWS_ERR_PROTOCOL, "continuation");
    f[0] = 0x83; f[1] = 0; CHECK(one(f, 2, 100) == MWS_ERR_PROTOCOL, "opcode 3");
    f[0] = 0x8B; f[1] = 0; CHECK(one(f, 2, 100) == MWS_ERR_PROTOCOL, "opcode 0xB");
    f[0] = 0x89; f[1] = 126; f[2] = 0; f[3] = 126; CHECK(one(f, 4, 1000) == MWS_ERR_PROTOCOL, "ping with extended length");
    f[0] = 0x82; f[1] = 127; f[2] = 0x80; memset(f + 3, 0, 7); CHECK(one(f, 10, 1000) == MWS_ERR_PROTOCOL, "64-bit length high bit");
    f[0] = 0x82; f[1] = 126; f[2] = 0x01; f[3] = 0x00; CHECK(one(f, 4, 255) == MWS_ERR_TOO_LARGE, "256 > cap 255");
    f[0] = 0x82; f[1] = 127; memset(f + 2, 0, 8); f[6] = 1; CHECK(one(f, 10, 1000) == MWS_ERR_TOO_LARGE, "4 GB frame");
    f[0] = 0x88; f[1] = 2; f[2] = 0x03; f[3] = 0xE8; CHECK(one(f, 4, 100) == MWS_CLOSE, "close");
    f[0] = 0x82; f[1] = 3; f[2] = 'a'; CHECK(one(f, 3, 100) == MWS_NEED_MORE, "partial payload needs more");
    size_t used;
    mws_rx_t rx; mws_rx_init(&rx);
    CHECK(mws_feed(NULL, f, 2, &used, (uint8_t *)f, 4, NULL) == MWS_ERR_PROTOCOL, "NULL rx");
    CHECK(mws_feed(&rx, NULL, 2, &used, (uint8_t *)f, 4, NULL) == MWS_ERR_PROTOCOL, "NULL in with n");
    CHECK(mws_feed(&rx, f, 0, &used, (uint8_t *)f, 4, NULL) == MWS_NEED_MORE && used == 0, "n = 0");
}

/* 随机字节：不越界（ASan）、吃掉的字节数 ≤ 输入、活着时有输入就有进展 */
static void test_fuzz(void)
{
    srand(4242);
    static uint8_t in[4096], buf[512];
    for (int round = 0; round < 20000; round++) {
        size_t n = (size_t)(rand() % sizeof(in));
        for (size_t i = 0; i < n; i++) in[i] = (uint8_t)rand();
        if (rand() & 1) { in[0] = (uint8_t)(0x80 | (rand() & 1 ? 0x2 : 0x9)); }   /* 让一部分流能过帧头 */
        size_t cap = (size_t)(rand() % sizeof(buf));
        mws_rx_t rx; mws_rx_init(&rx);
        size_t off = 0;
        int steps = 0;
        while (off < n && steps++ < 10000) {
            size_t chunk = 1 + (size_t)(rand() % 200);
            if (chunk > n - off) chunk = n - off;
            size_t used, fl;
            mws_ev_t e = mws_feed(&rx, in + off, chunk, &used, buf, cap, &fl);
            if (used > chunk || fl > cap) { CHECK(0, "round %d: used %zu chunk %zu fl %zu cap %zu", round, used, chunk, fl, cap); break; }
            off += used;
            if (e >= MWS_CLOSE) break;
            if (e == MWS_NEED_MORE && used == 0) { CHECK(0, "round %d: no progress", round); break; }
        }
        if (g_fail > 10) return;
    }
    g_pass++;
}

/* ---------------- 控制流 ---------------- */

static size_t put_msg(uint8_t *out, const char *s, size_t len)
{
    mctl_prefix(out, (uint32_t)len);
    memcpy(out + 4, s, len);
    return 4 + len;
}

static void test_ctl(void)
{
    uint8_t pf[4];
    mctl_prefix(pf, 0x01020304);
    CHECK(pf[0] == 4 && pf[1] == 3 && pf[2] == 2 && pf[3] == 1, "prefix little endian");
    static uint8_t s[20000];
    static char big[9000];
    memset(big, 'x', sizeof(big));
    size_t n = 0;
    n += put_msg(s + n, "{\"method\":\"link.invoke\",\"id\":\"1\"}", 34);
    n += put_msg(s + n, "", 0);                                      /* 空消息：吃掉不报 */
    n += put_msg(s + n, big, sizeof(big));                           /* 超过 cap 4096：整条跳过 */
    n += put_msg(s + n, "{\"type\":\"event\"}", 16);
    for (unsigned seed = 0; seed < 200; seed++) {
        srand(seed);
        mctl_rx_t rx; mctl_rx_init(&rx);
        static uint8_t buf[4096];
        size_t off = 0;
        int k = 0;
        mctl_ev_t evs[8]; size_t lens[8]; char firsts[8][40];
        while (off < n && k < 8) {
            size_t chunk = seed == 0 ? n : 1 + (size_t)(rand() % 700);
            if (chunk > n - off) chunk = n - off;
            size_t used, ml;
            mctl_ev_t e = mctl_feed(&rx, s + off, chunk, &used, buf, sizeof(buf), &ml);
            off += used;
            if (e != MCTL_NEED_MORE) {
                evs[k] = e; lens[k] = ml;
                snprintf(firsts[k], sizeof(firsts[k]), "%.*s", (int)(e == MCTL_MESSAGE && ml < 39 ? ml : 0), (const char *)buf);
                k++;
            } else if (used == 0) { break; }
        }
        bool ok = k == 3 && evs[0] == MCTL_MESSAGE && lens[0] == 34 && strncmp(firsts[0], "{\"method\"", 9) == 0
                  && evs[1] == MCTL_SKIPPED && lens[1] == sizeof(big)
                  && evs[2] == MCTL_MESSAGE && lens[2] == 16 && strcmp(firsts[2], "{\"type\":\"event\"}") == 0;
        CHECK(ok, "ctl seed %u: k=%d", seed, k);
    }
    size_t used, ml;
    CHECK(mctl_feed(NULL, s, 4, &used, NULL, 0, &ml) == MCTL_NEED_MORE, "NULL rx");
}

int main(void)
{
    test_encode();
    test_parse_chunking();
    test_errors();
    test_fuzz();
    test_ctl();
    printf("muse_frame: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
