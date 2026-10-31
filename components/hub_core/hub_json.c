/* hub_json —— 流式 JSON 写入器（见 include/hub_json.h）。纯 C，不依赖 ESP-IDF。
 *
 * 不变量：任何时候 len + depth + 1 ≤ cap（还开着的每个容器留 1 字节闭合，再留 1 字节 NUL）。
 * 每次写入都按 len + n + 写入后的深度 + reserve + 1 ≤ cap 检查，闭合括号写进预留的那 1 字节，所以 hj_*_end 与
 * hj_finish 永远放得下；reserve 只挡普通写入，不参与这个物理不变量。 */
#include "hub_json.h"
#include "hub_utf8.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

void hj_init(hj_t *w, char *buf, size_t cap)
{
    if (!w) return;
    memset(w, 0, sizeof(*w));
    w->buf = buf;
    w->cap = cap;
    if (!buf || cap == 0) {
        w->error = true;
        return;
    }
    buf[0] = '\0';
}

void hj_set_reserve(hj_t *w, size_t n)
{
    if (w) w->reserve = n > w->cap ? w->cap : n;       /* 钳位：fits() 里的加法就不会回绕（2026-10-22 审计：SIZE_MAX 级 reserve 越界写） */
}

static bool usable(const hj_t *w)
{
    return w && !w->error && !w->truncated;
}

/* 再写 n 字节、写完深度为 depth_after 时还守得住预留吗（不溢出地算） */
static bool fits(const hj_t *w, size_t n, size_t depth_after)
{
    if (w->reserve >= w->cap || depth_after >= w->cap) return false;
    const size_t fixed = depth_after + w->reserve + 1;     /* 三项都 < cap，cap 是真实缓冲大小，加起来不会回绕 */
    if (fixed > w->cap || n > w->cap - fixed) return false;
    return w->len <= w->cap - fixed - n;
}

/* 写值之前：位置对不对、要不要逗号。用错置 error 返回 false。 */
static bool value_slot(hj_t *w, size_t *comma)
{
    *comma = 0;
    if (w->depth == 0) {
        if (w->top_done) {
            w->error = true;
            return false;
        }
        return true;
    }
    if (w->stack[w->depth - 1] == '{') {
        if (!w->after_key) {
            w->error = true;
            return false;
        }
        return true;                         /* 逗号随键写过了 */
    }
    *comma = w->need_comma ? 1 : 0;
    return true;
}

/* 值放不下：对象里连同刚写的键一起撤掉，再置 truncated */
static void value_overflow(hj_t *w)
{
    if (w->depth > 0 && w->stack[w->depth - 1] == '{' && w->after_key) {
        w->len = w->key_start;
        w->need_comma = w->key_had_comma;
        w->after_key = false;
        w->buf[w->len] = '\0';
    }
    w->truncated = true;
}

static void value_done(hj_t *w)
{
    if (w->depth == 0) {
        w->top_done = true;
    } else {
        w->need_comma = true;
        w->after_key = false;
    }
}

/* ---------------- 字符串 ---------------- */

/* 转义后的长度（含两个引号）；溢出返回 SIZE_MAX */
static size_t esc_len(const unsigned char *s, size_t n)
{
    if (n > (SIZE_MAX - 2) / 6) return SIZE_MAX;
    size_t out = 2;
    for (size_t i = 0; i < n;) {
        const unsigned char c = s[i];
        if (c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t') {
            out += 2; i++;
        } else if (c < 0x20) {
            out += 6; i++;
        } else if (c < 0x80) {
            out += 1; i++;
        } else {
            const size_t k = hub_utf8_seq(s, n, i, NULL);
            if (k == 0) {
                out += 1; i++;              /* 坏字节 → '?' */
            } else {
                out += k; i += k;
            }
        }
    }
    return out;
}

/* 调用前已确认放得下 esc_len 个字节 */
static void esc_write(hj_t *w, const unsigned char *s, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    char *b = w->buf;
    size_t o = w->len;
    b[o++] = '"';
    for (size_t i = 0; i < n;) {
        const unsigned char c = s[i];
        switch (c) {
        case '"':  b[o++] = '\\'; b[o++] = '"';  i++; continue;
        case '\\': b[o++] = '\\'; b[o++] = '\\'; i++; continue;
        case '\b': b[o++] = '\\'; b[o++] = 'b';  i++; continue;
        case '\f': b[o++] = '\\'; b[o++] = 'f';  i++; continue;
        case '\n': b[o++] = '\\'; b[o++] = 'n';  i++; continue;
        case '\r': b[o++] = '\\'; b[o++] = 'r';  i++; continue;
        case '\t': b[o++] = '\\'; b[o++] = 't';  i++; continue;
        default: break;
        }
        if (c < 0x20) {
            b[o++] = '\\'; b[o++] = 'u'; b[o++] = '0'; b[o++] = '0';
            b[o++] = hex[c >> 4]; b[o++] = hex[c & 0x0F];
            i++;
        } else if (c < 0x80) {
            b[o++] = (char)c;
            i++;
        } else {
            const size_t k = hub_utf8_seq(s, n, i, NULL);
            if (k == 0) {
                b[o++] = '?';
                i++;
            } else {
                memcpy(b + o, s + i, k);
                o += k; i += k;
            }
        }
    }
    b[o++] = '"';
    b[o] = '\0';
    w->len = o;
}

/* ---------------- 容器 ---------------- */

static void open_c(hj_t *w, char c)
{
    if (!usable(w)) return;
    if (w->depth >= HJ_MAX_DEPTH) {
        w->error = true;
        return;
    }
    size_t comma;
    if (!value_slot(w, &comma)) return;
    if (!fits(w, comma + 1, (size_t)w->depth + 1)) {
        value_overflow(w);
        return;
    }
    if (comma) w->buf[w->len++] = ',';
    w->buf[w->len++] = c;
    w->buf[w->len] = '\0';
    w->stack[w->depth++] = c;
    w->need_comma = false;
    w->after_key = false;
}

static void close_c(hj_t *w, char c)
{
    if (!usable(w)) return;
    if (w->depth == 0 || w->stack[w->depth - 1] != c || w->after_key) {
        w->error = true;
        return;
    }
    w->buf[w->len++] = (c == '{') ? '}' : ']';   /* 写进为它预留的那 1 字节 */
    w->buf[w->len] = '\0';
    w->depth--;
    if (w->depth < w->mark_floor) w->mark_broken = true;
    value_done(w);
}

void hj_obj_begin(hj_t *w) { open_c(w, '{'); }
void hj_obj_end(hj_t *w)   { close_c(w, '{'); }
void hj_arr_begin(hj_t *w) { open_c(w, '['); }
void hj_arr_end(hj_t *w)   { close_c(w, '['); }

void hj_key(hj_t *w, const char *k)
{
    if (!usable(w)) return;
    if (!k || w->depth == 0 || w->stack[w->depth - 1] != '{' || w->after_key) {
        w->error = true;
        return;
    }
    const size_t kl = strlen(k);
    const size_t comma = w->need_comma ? 1 : 0;
    const size_t el = esc_len((const unsigned char *)k, kl);
    if (el == SIZE_MAX || el > SIZE_MAX - 2 || !fits(w, comma + el + 1, w->depth)) {
        w->truncated = true;
        return;
    }
    w->key_start = w->len;
    w->key_had_comma = w->need_comma;
    if (comma) w->buf[w->len++] = ',';
    esc_write(w, (const unsigned char *)k, kl);
    w->buf[w->len++] = ':';
    w->buf[w->len] = '\0';
    w->after_key = true;
}

/* ---------------- 值 ---------------- */

static void put_token(hj_t *w, const char *tok, size_t n)
{
    if (!usable(w)) return;
    size_t comma;
    if (!value_slot(w, &comma)) return;
    if (!fits(w, comma + n, w->depth)) {
        value_overflow(w);
        return;
    }
    if (comma) w->buf[w->len++] = ',';
    memcpy(w->buf + w->len, tok, n);
    w->len += n;
    w->buf[w->len] = '\0';
    value_done(w);
}

void hj_strn(hj_t *w, const char *s, size_t n)
{
    if (!usable(w)) return;
    if (!s) {
        put_token(w, "null", 4);
        return;
    }
    size_t comma;
    if (!value_slot(w, &comma)) return;
    const size_t el = esc_len((const unsigned char *)s, n);
    if (el == SIZE_MAX || !fits(w, comma + el, w->depth)) {
        value_overflow(w);
        return;
    }
    if (comma) w->buf[w->len++] = ',';
    esc_write(w, (const unsigned char *)s, n);
    value_done(w);
}

void hj_str(hj_t *w, const char *s)
{
    hj_strn(w, s, s ? strlen(s) : 0);
}

void hj_raw(hj_t *w, const char *json)
{
    if (!json || !*json) {
        if (w) w->error = true;
        return;
    }
    put_token(w, json, strlen(json));
}

void hj_int(hj_t *w, int64_t v)
{
    char t[24];
    const int n = snprintf(t, sizeof(t), "%" PRId64, v);
    if (n > 0 && (size_t)n < sizeof(t)) put_token(w, t, (size_t)n);
    else if (w) w->error = true;
}

void hj_uint(hj_t *w, uint64_t v)
{
    char t[24];
    const int n = snprintf(t, sizeof(t), "%" PRIu64, v);
    if (n > 0 && (size_t)n < sizeof(t)) put_token(w, t, (size_t)n);
    else if (w) w->error = true;
}

void hj_bool(hj_t *w, bool v) { put_token(w, v ? "true" : "false", v ? 4 : 5); }
void hj_null(hj_t *w)         { put_token(w, "null", 4); }

void hj_kstr(hj_t *w, const char *k, const char *v)  { hj_key(w, k); hj_str(w, v); }
void hj_fixed(hj_t *w, int64_t v, unsigned decimals)
{
    if (decimals > 6) {
        if (w) w->error = true;
        return;
    }
    int64_t div = 1;
    for (unsigned k = 0; k < decimals; k++) div *= 10;
    if (decimals == 0 || v == INT64_MIN) {         /* INT64_MIN 取不了绝对值：读数到不了这里，当错 */
        if (v == INT64_MIN && decimals) {
            if (w) w->error = true;
            return;
        }
        hj_int(w, v);
        return;
    }
    const uint64_t a = (uint64_t)(v < 0 ? -v : v);
    char t[32];
    const int n = snprintf(t, sizeof(t), "%s%" PRIu64 ".%0*" PRIu64, v < 0 ? "-" : "", a / (uint64_t)div, (int)decimals, a % (uint64_t)div);
    if (n > 0 && (size_t)n < sizeof(t)) put_token(w, t, (size_t)n);
    else if (w) w->error = true;
}

void hj_kint(hj_t *w, const char *k, int64_t v)      { hj_key(w, k); hj_int(w, v); }
void hj_kfixed(hj_t *w, const char *k, int64_t v, unsigned decimals) { hj_key(w, k); hj_fixed(w, v, decimals); }
void hj_kuint(hj_t *w, const char *k, uint64_t v)    { hj_key(w, k); hj_uint(w, v); }
void hj_kbool(hj_t *w, const char *k, bool v)        { hj_key(w, k); hj_bool(w, v); }

/* ---------------- 分页 ---------------- */

hj_mark_t hj_mark(hj_t *w)
{
    hj_mark_t m = {0};
    if (!w) return m;
    if (w->after_key || w->truncated) w->error = true;   /* 键和值之间不能切；截断后 mark 会让 rollback 把截断信息清掉 */
    m.len = w->len;
    m.depth = w->depth;
    m.need_comma = w->need_comma;
    m.top_done = w->top_done;
    w->mark_floor = w->depth;
    w->mark_broken = false;
    if (++w->mark_gen == 0) w->mark_gen = 1;             /* 0 留给"没 mark 过"（hj_mark(NULL) 返回的零值） */
    m.gen = w->mark_gen;
    return m;
}

void hj_rollback(hj_t *w, hj_mark_t m)
{
    if (!w || w->error) return;
    if (m.gen != w->mark_gen || w->mark_broken || m.depth != w->mark_floor || m.depth > w->depth || m.len > w->len) {
        w->error = true;
        return;
    }
    w->len = m.len;
    w->depth = m.depth;
    w->need_comma = m.need_comma;
    w->top_done = m.top_done;
    w->after_key = false;
    w->truncated = false;
    w->buf[w->len] = '\0';
}

int hj_finish(hj_t *w)
{
    if (!w || !w->buf || w->cap == 0) return -1;
    if (w->error || w->after_key) {          /* after_key：写了键没写值就收尾（截断时键已随值撤掉，不会走到这） */
        w->error = true;
        w->buf[0] = '\0';
        return -1;
    }
    while (w->depth > 0) {
        w->buf[w->len++] = (w->stack[w->depth - 1] == '{') ? '}' : ']';
        w->depth--;
    }
    w->buf[w->len] = '\0';
    w->top_done = true;
    if (w->len == 0 || w->len > (size_t)INT32_MAX) {   /* 一个字节都没写进去（缓冲太小）也不是合法 JSON */
        w->error = true;
        return -1;
    }
    return (int)w->len;
}
