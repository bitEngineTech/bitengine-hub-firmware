/* hub_json 主机测试（docs/15 设计 6.9、14.1：转义、UTF-8 透传、截断后仍是合法 JSON）。编的是产品源文件 ../hub_json.c。
 *
 * 带一个严格的 JSON 校验器（RFC 8259 语法 + 字符串必须是合法 UTF-8、不许裸控制字符），把每个缓冲大小 1..N 都跑一遍：
 * 输出要么是 -1，要么是合法 JSON；分页场景里每一项要么完整、要么不在。 */
#include "hub_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) { g_pass++; } else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------- 独立的 UTF-8 判定 ----------------
 * 不复用产品的 hub_utf8_seq（审计：复用就不独立，产品里去掉超长编码检查，校验器跟着放过，测不出来）。
 * 写法也不同：先按位解出码点，再按码点算最短编码长度去比。 */
static size_t ref_utf8(const unsigned char *s, size_t n, size_t i)
{
    unsigned c = s[i];
    size_t len;
    uint32_t cp;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
    else return 0;
    if (i + len > n) return 0;
    for (size_t k = 1; k < len; k++) {
        if ((s[i + k] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (s[i + k] & 0x3F);
    }
    size_t minlen = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    if (minlen != len || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return len;
}

/* ---------------- 严格 JSON 校验 ---------------- */
typedef struct { const char *s; size_t n, i; int depth; } jv_t;
static void jv_ws(jv_t *v) { while (v->i < v->n && (v->s[v->i] == ' ' || v->s[v->i] == '\t' || v->s[v->i] == '\n' || v->s[v->i] == '\r')) v->i++; }
static bool jv_value(jv_t *v);
static bool jv_string(jv_t *v)
{
    if (v->i >= v->n || v->s[v->i] != '"') return false;
    v->i++;
    while (v->i < v->n) {
        unsigned char c = (unsigned char)v->s[v->i];
        if (c == '"') { v->i++; return true; }
        if (c < 0x20) return false;
        if (c == '\\') {
            v->i++;
            if (v->i >= v->n) return false;
            char e = v->s[v->i];
            if (e == 'u') {
                for (int k = 1; k <= 4; k++) {
                    if (v->i + k >= v->n) return false;
                    char h = v->s[v->i + k];
                    if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F'))) return false;
                }
                v->i += 5;
            } else if (strchr("\"\\/bfnrt", e) && e) {
                v->i++;
            } else {
                return false;
            }
        } else if (c < 0x80) {
            v->i++;
        } else {
            size_t k = ref_utf8((const unsigned char *)v->s, v->n, v->i);
            if (k == 0) return false;
            v->i += k;
        }
    }
    return false;
}
static bool jv_number(jv_t *v)
{
    size_t st = v->i;
    if (v->i < v->n && v->s[v->i] == '-') v->i++;
    if (v->i >= v->n) return false;
    if (v->s[v->i] == '0') v->i++;
    else if (v->s[v->i] >= '1' && v->s[v->i] <= '9') { while (v->i < v->n && v->s[v->i] >= '0' && v->s[v->i] <= '9') v->i++; }
    else return false;
    return v->i > st;
}
static bool jv_lit(jv_t *v, const char *lit) { size_t l = strlen(lit); if (v->n - v->i < l || memcmp(v->s + v->i, lit, l)) return false; v->i += l; return true; }
static bool jv_value(jv_t *v)
{
    if (++v->depth > 64) return false;
    jv_ws(v);
    if (v->i >= v->n) return false;
    char c = v->s[v->i];
    bool ok;
    if (c == '{') {
        v->i++; jv_ws(v);
        if (v->i < v->n && v->s[v->i] == '}') { v->i++; ok = true; }
        else {
            ok = false;
            for (;;) {
                jv_ws(v);
                if (!jv_string(v)) break;
                jv_ws(v);
                if (v->i >= v->n || v->s[v->i] != ':') break;
                v->i++;
                if (!jv_value(v)) break;
                jv_ws(v);
                if (v->i < v->n && v->s[v->i] == ',') { v->i++; continue; }
                if (v->i < v->n && v->s[v->i] == '}') { v->i++; ok = true; }
                break;
            }
        }
    } else if (c == '[') {
        v->i++; jv_ws(v);
        if (v->i < v->n && v->s[v->i] == ']') { v->i++; ok = true; }
        else {
            ok = false;
            for (;;) {
                if (!jv_value(v)) break;
                jv_ws(v);
                if (v->i < v->n && v->s[v->i] == ',') { v->i++; continue; }
                if (v->i < v->n && v->s[v->i] == ']') { v->i++; ok = true; }
                break;
            }
        }
    } else if (c == '"') ok = jv_string(v);
    else if (c == 't') ok = jv_lit(v, "true");
    else if (c == 'f') ok = jv_lit(v, "false");
    else if (c == 'n') ok = jv_lit(v, "null");
    else ok = jv_number(v);
    v->depth--;
    return ok;
}
static bool json_valid(const char *s, size_t n)
{
    jv_t v = { s, n, 0, 0 };
    if (!jv_value(&v)) return false;
    jv_ws(&v);
    return v.i == n;
}

/* 自测校验器本身：它若放过坏 JSON，下面所有"合法"断言都是空的 */
static void test_validator(void)
{
    const char *good[] = { "{}", "[]", "{\"a\":1}", "[1,\"x\",true,false,null,{\"b\":[]}]", "\"\\u00e9\\n\"", "\"\xe4\xb8\xad\"", "-12",
                           "\"\xe0\xa0\x80\"", "\"\xf0\x90\x80\x80\"", "\"\xf4\x8f\xbf\xbf\"", "\"\xc2\x80\"" };   /* 各长度的最小 / 最大合法值 */
    const char *bad[] = { "{", "{\"a\":}", "{\"a\":1,}", "[1,]", "{\"a\" 1}", "\"\x01\"", "\"\xc0\xaf\"", "\"\xed\xa0\x80\"",
                          "{\"a\":1}}", "", "[1 2]", "\"abc", "{1:2}", "\"\\x\"",
                          "\"\xe0\x80\x80\"", "\"\xe0\x9f\xbf\"", "\"\xf0\x80\x80\x80\"", "\"\xf0\x8f\xbf\xbf\"",   /* 三、四字节超长编码 */
                          "\"\xf4\x90\x80\x80\"", "\"\xf8\x88\x80\x80\x80\"", "\"\xc1\xbf\"" };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) CHECK(json_valid(good[i], strlen(good[i])), "validator rejects good %s", good[i]);
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) CHECK(!json_valid(bad[i], strlen(bad[i])), "validator accepts bad #%zu", i);
}

/* ---------------- 基本形状 ---------------- */
static void test_basic(void)
{
    char b[256];
    hj_t w;
    hj_init(&w, b, sizeof(b));
    hj_obj_begin(&w);
    hj_kstr(&w, "s", "x");
    hj_kint(&w, "i", -42);
    hj_kuint(&w, "u", 18446744073709551615ull);
    hj_kbool(&w, "t", true);
    hj_kbool(&w, "f", false);
    hj_key(&w, "n"); hj_null(&w);
    hj_key(&w, "a"); hj_arr_begin(&w); hj_int(&w, 1); hj_str(&w, "two"); hj_obj_begin(&w); hj_obj_end(&w); hj_arr_begin(&w); hj_arr_end(&w); hj_arr_end(&w);
    hj_key(&w, "z"); hj_str(&w, NULL);
    hj_obj_end(&w);
    int n = hj_finish(&w);
    const char *want = "{\"s\":\"x\",\"i\":-42,\"u\":18446744073709551615,\"t\":true,\"f\":false,\"n\":null,\"a\":[1,\"two\",{},[]],\"z\":null}";
    CHECK(n == (int)strlen(want) && strcmp(b, want) == 0, "basic: got %s", b);
    CHECK(!w.truncated, "basic: not truncated");

    hj_init(&w, b, sizeof(b));
    hj_int(&w, INT64_MIN);
    n = hj_finish(&w);
    CHECK(n > 0 && strcmp(b, "-9223372036854775808") == 0, "top-level scalar: %s", b);
}

static void test_escape_and_utf8(void)
{
    char b[512];
    hj_t w;
    hj_init(&w, b, sizeof(b));
    hj_arr_begin(&w);
    hj_str(&w, "q\"b\\s/\b\f\n\r\t\x01\x1f\x7f");
    hj_str(&w, "\xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x98\x80 caf\xc3\xa9");       /* 合法 UTF-8 原样透传 */
    hj_str(&w, "a\xc0\xaf" "b\xed\xa0\x80" "c\xf4\x90\x80\x80" "d\x80" "e\xe4\xb8");  /* 超长、代理、超范围、孤立续字节、截半 */
    hj_str(&w, "f\xe0\x80\x80" "g\xf0\x8f\xbf\xbf" "h");                              /* 三、四字节超长编码 */
    hj_strn(&w, "x\0y", 3);                                                    /* 内嵌 NUL */
    hj_arr_end(&w);
    int n = hj_finish(&w);
    CHECK(n > 0 && json_valid(b, (size_t)n), "escape: invalid JSON %s", b);
    CHECK(strstr(b, "\"q\\\"b\\\\s/\\b\\f\\n\\r\\t\\u0001\\u001f\x7f\"") != NULL, "escape: control/quote forms: %s", b);
    CHECK(strstr(b, "\"\xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x98\x80 caf\xc3\xa9\"") != NULL, "utf8 passthrough");
    /* 每个坏字节一个 '?'：C0 AF → ??；ED A0 80 → ???；F4 90 80 80 → ????；80 → ?；E4 B8（截半）→ ?? */
    CHECK(strstr(b, "\"a??b???c????d?e??\"") != NULL, "bad utf8 replaced byte-wise: %s", b);
    CHECK(strstr(b, "\"f???g????h\"") != NULL, "3/4-byte overlong replaced byte-wise: %s", b);
    CHECK(strstr(b, "\"x\\u0000y\"") != NULL, "embedded NUL escaped");

    hj_init(&w, b, sizeof(b));
    hj_obj_begin(&w);
    hj_kstr(&w, "k\"ey\n", "v");                                               /* 键也转义 */
    hj_obj_end(&w);
    n = hj_finish(&w);
    CHECK(n > 0 && strcmp(b, "{\"k\\\"ey\\n\":\"v\"}") == 0, "key escaped: %s", b);
}

static void test_misuse(void)
{
    char b[64];
    hj_t w;
    /* 对象里不写键就写值 */
    hj_init(&w, b, sizeof(b)); hj_obj_begin(&w); hj_int(&w, 1); CHECK(hj_finish(&w) == -1, "value without key");
    /* 数组里写键 */
    hj_init(&w, b, sizeof(b)); hj_arr_begin(&w); hj_key(&w, "k"); CHECK(hj_finish(&w) == -1, "key in array");
    /* 连写两个键 */
    hj_init(&w, b, sizeof(b)); hj_obj_begin(&w); hj_key(&w, "a"); hj_key(&w, "b"); CHECK(hj_finish(&w) == -1, "two keys");
    /* 键之后直接收尾 */
    hj_init(&w, b, sizeof(b)); hj_obj_begin(&w); hj_key(&w, "a"); CHECK(hj_finish(&w) == -1, "key then finish");
    /* 括号不配对 */
    hj_init(&w, b, sizeof(b)); hj_obj_begin(&w); hj_arr_end(&w); CHECK(hj_finish(&w) == -1, "mismatched close");
    hj_init(&w, b, sizeof(b)); hj_obj_end(&w); CHECK(hj_finish(&w) == -1, "close at top");
    /* 顶层两个值 */
    hj_init(&w, b, sizeof(b)); hj_int(&w, 1); hj_int(&w, 2); CHECK(hj_finish(&w) == -1, "two top-level values");
    hj_init(&w, b, sizeof(b)); hj_obj_begin(&w); hj_obj_end(&w); hj_arr_begin(&w); CHECK(hj_finish(&w) == -1, "container after top value");
    /* 嵌套超深 */
    hj_init(&w, b, sizeof(b));
    for (int i = 0; i <= HJ_MAX_DEPTH; i++) hj_arr_begin(&w);
    CHECK(hj_finish(&w) == -1, "depth overflow");
    /* 键与值之间 mark */
    hj_init(&w, b, sizeof(b)); hj_obj_begin(&w); hj_key(&w, "a"); (void)hj_mark(&w); hj_int(&w, 1); CHECK(hj_finish(&w) == -1, "mark after key");
    /* NULL 参数不崩 */
    hj_init(NULL, b, sizeof(b)); hj_obj_begin(NULL); hj_kstr(NULL, "a", "b"); hj_int(NULL, 1); (void)hj_mark(NULL); hj_rollback(NULL, (hj_mark_t){0});
    CHECK(hj_finish(NULL) == -1, "finish NULL");
    hj_init(&w, NULL, 10); hj_obj_begin(&w); CHECK(hj_finish(&w) == -1, "NULL buffer");
    hj_init(&w, b, 0); CHECK(hj_finish(&w) == -1, "zero cap");
    /* 什么都没写 */
    hj_init(&w, b, sizeof(b)); CHECK(hj_finish(&w) == -1, "nothing written");
}

/* 键和值是一体的：值放不下连键一起撤 */
static void test_key_value_atomic(void)
{
    char b[64];
    for (size_t cap = 1; cap <= sizeof(b); cap++) {
        hj_t w;
        hj_init(&w, b, cap);
        hj_obj_begin(&w);
        hj_kstr(&w, "first", "1");
        hj_kstr(&w, "second", "a-rather-long-value-here");
        hj_obj_end(&w);
        int n = hj_finish(&w);
        if (n < 0) { CHECK(cap < 3, "cap %zu: -1 only when nothing fits", cap); continue; }
        CHECK(json_valid(b, (size_t)n), "cap %zu: invalid %s", cap, b);
        CHECK(strstr(b, "\"second\":") == NULL || strstr(b, "a-rather-long-value-here") != NULL, "cap %zu: dangling key %s", cap, b);
        CHECK((size_t)n < cap, "cap %zu: len %d overruns", cap, n);
    }
}

/* 分页：每个缓冲大小下，输出合法、条目完整、truncated 时带 next_cursor */
static int write_list(char *b, size_t cap, int items, int *written, bool *trunc)
{
    hj_t w;
    hj_init(&w, b, cap);
    hj_set_reserve(&w, 40);                                       /* ,"truncated":true,"next_cursor":"NNN" 最多 37 */
    hj_obj_begin(&w);
    hj_kbool(&w, "ok", true);
    hj_key(&w, "devices");
    hj_arr_begin(&w);
    *written = 0;
    *trunc = false;
    for (int i = 0; i < items; i++) {
        hj_mark_t m = hj_mark(&w);
        hj_obj_begin(&w);
        char id[24];
        snprintf(id, sizeof(id), "zb-%016x", (unsigned)i * 2654435761u);
        hj_kstr(&w, "id", id);
        hj_kstr(&w, "name", (i % 3) ? "Kitchen \"leak\" sensor" : "\xe5\x8e\xa8\xe6\x88\xbf");
        hj_key(&w, "capabilities"); hj_arr_begin(&w); hj_str(&w, "contact"); hj_str(&w, "battery"); hj_arr_end(&w);
        hj_kint(&w, "last_seen_s", i * 7);
        hj_obj_end(&w);
        if (w.truncated) {
            hj_rollback(&w, m);
            *trunc = true;
            break;
        }
        (*written)++;
    }
    hj_arr_end(&w);
    hj_set_reserve(&w, 0);
    if (*trunc) {
        char cur[8];
        snprintf(cur, sizeof(cur), "%d", *written);
        hj_kbool(&w, "truncated", true);
        hj_kstr(&w, "next_cursor", cur);
    }
    hj_obj_end(&w);
    return hj_finish(&w);
}

static void test_pagination_all_caps(void)
{
    static char b[4096];
    int full_written = 0; bool full_trunc = true;
    int full = write_list(b, sizeof(b), 12, &full_written, &full_trunc);
    CHECK(full > 0 && full_written == 12 && !full_trunc && json_valid(b, (size_t)full), "full list");
    int seen_partial = 0;
    for (size_t cap = 1; cap <= (size_t)full + 1; cap++) {
        int written; bool trunc;
        int n = write_list(b, cap, 12, &written, &trunc);
        if (n < 0) continue;
        CHECK(json_valid(b, (size_t)n), "cap %zu: invalid JSON: %s", cap, b);
        CHECK((size_t)n < cap, "cap %zu overrun", cap);
        /* 条目完整：数到的 "id" 个数等于 written，每个 id 后面都有 last_seen_s */
        int ids = 0, ls = 0;
        for (const char *p = b; (p = strstr(p, "\"id\":")); p++) ids++;
        for (const char *p = b; (p = strstr(p, "\"last_seen_s\":")); p++) ls++;
        CHECK(ids == written && ls == written, "cap %zu: partial item (ids %d, last_seen %d, written %d)", cap, ids, ls, written);
        if (trunc) {
            seen_partial++;
            char want[32];
            snprintf(want, sizeof(want), "\"next_cursor\":\"%d\"", written);
            CHECK(strstr(b, "\"truncated\":true") && strstr(b, want), "cap %zu: truncated without cursor: %s", cap, b);
        } else {
            CHECK(written == 12, "cap %zu: not truncated but only %d items", cap, written);
        }
    }
    CHECK(seen_partial > 100, "pagination exercised (%d partial caps)", seen_partial);
}

/* 不 rollback 直接 finish：仍然合法（半截容器被闭合） */
static void test_truncated_finish_all_caps(void)
{
    char b[160];
    for (size_t cap = 1; cap <= sizeof(b); cap++) {
        hj_t w;
        hj_init(&w, b, cap);
        hj_obj_begin(&w);
        hj_key(&w, "a"); hj_arr_begin(&w);
        for (int i = 0; i < 6; i++) { hj_obj_begin(&w); hj_kstr(&w, "k", "v\"\xe4\xb8\xad"); hj_key(&w, "n"); hj_arr_begin(&w); hj_int(&w, i); hj_arr_end(&w); hj_obj_end(&w); }
        hj_arr_end(&w);
        hj_kstr(&w, "tail", "end");
        hj_obj_end(&w);
        int n = hj_finish(&w);
        if (n < 0) { CHECK(cap < 3, "cap %zu: unexpected -1", cap); continue; }
        CHECK(json_valid(b, (size_t)n), "cap %zu: invalid %s", cap, b);
        CHECK((size_t)n < cap, "cap %zu overrun", cap);
    }
}

/* rollback 的守卫 */
static void test_rollback_guards(void)
{
    char b[128];
    hj_t w;
    /* mark 之后把 mark 那一层关掉又开新的：mark 作废 */
    hj_init(&w, b, sizeof(b));
    hj_arr_begin(&w);
    hj_arr_begin(&w);
    hj_mark_t m = hj_mark(&w);       /* 深度 2 */
    hj_arr_end(&w);                  /* 回到 1，低于 mark 那层 */
    hj_obj_begin(&w);                /* 栈[1] 被改写成 '{' */
    hj_rollback(&w, m);
    CHECK(hj_finish(&w) == -1, "rollback after popping below the mark");
    /* 正常 rollback 之后可以接着写 */
    hj_init(&w, b, sizeof(b));
    hj_arr_begin(&w);
    hj_int(&w, 1);
    m = hj_mark(&w);
    hj_obj_begin(&w); hj_kint(&w, "x", 2);
    hj_rollback(&w, m);
    hj_int(&w, 3);
    hj_arr_end(&w);
    int n = hj_finish(&w);
    CHECK(n > 0 && strcmp(b, "[1,3]") == 0, "rollback then continue: %s", b);
    /* rollback 清掉 truncated */
    hj_init(&w, b, 12);
    hj_arr_begin(&w);
    m = hj_mark(&w);
    hj_str(&w, "this does not fit");
    CHECK(w.truncated, "truncated set");
    hj_rollback(&w, m);
    CHECK(!w.truncated, "rollback clears truncated");
    hj_str(&w, "ok");
    n = hj_finish(&w);
    CHECK(n > 0 && strcmp(b, "[\"ok\"]") == 0, "after rollback: %s", b);
}

/* 审计 ⚠️：reserve 大到 SIZE_MAX 级时 fits() 的加法回绕会越界写（ASan 盯着） */
static void test_reserve_overflow(void)
{
    char *b = malloc(8);
    hj_t w;
    hj_init(&w, b, 8);
    hj_set_reserve(&w, SIZE_MAX - 1);
    hj_arr_begin(&w);
    hj_int(&w, 123456);
    int n = hj_finish(&w);
    CHECK(n < 8, "huge reserve: no overrun (%d)", n);
    hj_init(&w, b, 8);
    hj_set_reserve(&w, SIZE_MAX);
    hj_obj_begin(&w);
    CHECK(w.truncated && hj_finish(&w) == -1, "SIZE_MAX reserve blocks everything");
    free(b);
}

/* 审计 ⚠️：只有最近一次 mark 能用；截断之后不许 mark */
static void test_stale_mark_and_mark_after_truncation(void)
{
    char b[128];
    hj_t w;
    hj_init(&w, b, sizeof(b));
    hj_arr_begin(&w);
    hj_obj_begin(&w);
    hj_mark_t a = hj_mark(&w);
    hj_kint(&w, "x", 1);
    hj_obj_end(&w);
    hj_arr_begin(&w);
    (void)hj_mark(&w);                 /* 新 mark 把 a 作废 */
    hj_rollback(&w, a);
    hj_int(&w, 5);
    CHECK(hj_finish(&w) == -1, "rollback to a stale mark is an error (would give [{5]] )");
    hj_init(&w, b, 24);
    hj_obj_begin(&w);
    hj_kstr(&w, "status", "this value does not fit at all");
    CHECK(w.truncated, "truncated");
    (void)hj_mark(&w);
    CHECK(hj_finish(&w) == -1, "mark after truncation is an error (rollback would hide the dropped field)");
}

/* 同一个写入器里两段分页列表：第二段的 mark / rollback 照样好用（审计：hj_mark 不重置作废标志时测不出来） */
static void test_two_paginated_lists(void)
{
    static char b[2048];
    int checked = 0;
    for (size_t cap = 40; cap <= sizeof(b); cap++) {
        hj_t w;
        hj_init(&w, b, cap);
        hj_set_reserve(&w, 24);
        hj_obj_begin(&w);
        int written[2] = { 0, 0 };
        bool trunc = false;
        for (int list = 0; list < 2 && !trunc; list++) {
            hj_mark_t lm = hj_mark(&w);            /* 键和空数组也可能放不下：整段撤掉（截断后不许再 mark，所以先 mark 再写） */
            hj_key(&w, list ? "b" : "a");
            hj_arr_begin(&w);
            if (w.truncated) { hj_rollback(&w, lm); trunc = true; break; }
            for (int i = 0; i < 8; i++) {
                hj_mark_t m = hj_mark(&w);
                hj_obj_begin(&w); hj_kint(&w, "i", i); hj_kstr(&w, "s", "abcdefghij"); hj_obj_end(&w);
                if (w.truncated) { hj_rollback(&w, m); trunc = true; break; }
                written[list]++;
            }
            hj_arr_end(&w);
        }
        hj_set_reserve(&w, 0);
        if (trunc) hj_kbool(&w, "truncated", true);
        hj_obj_end(&w);
        int n = hj_finish(&w);
        if (n < 0) { CHECK(cap < 60, "cap %zu: two lists -> -1 (written %d/%d)", cap, written[0], written[1]); continue; }
        CHECK(json_valid(b, (size_t)n), "cap %zu: invalid %s", cap, b);
        int items = 0;
        for (const char *p = b; (p = strstr(p, "\"s\":\"abcdefghij\"")); p++) items++;
        CHECK(items == written[0] + written[1], "cap %zu: partial item", cap);
        if (written[0] == 8 && written[1] > 0 && written[1] < 8) checked++;
    }
    CHECK(checked > 50, "second list pagination exercised (%d caps)", checked);
}

/* 预留：普通写入用不了，放出来之后能用 */
static void test_reserve(void)
{
    char b[32];
    hj_t w;
    hj_init(&w, b, sizeof(b));
    hj_set_reserve(&w, 20);
    hj_arr_begin(&w);
    hj_str(&w, "0123456789");        /* 12 字节 + [ + ] + NUL + 20 = 35 > 32 */
    CHECK(w.truncated, "reserve blocks ordinary writes");
    hj_t w2;
    hj_init(&w2, b, sizeof(b));
    hj_set_reserve(&w2, 20);
    hj_arr_begin(&w2);
    hj_set_reserve(&w2, 0);
    hj_str(&w2, "0123456789");
    hj_arr_end(&w2);
    CHECK(hj_finish(&w2) > 0 && strcmp(b, "[\"0123456789\"]") == 0, "released reserve usable: %s", b);
}

/* 随机操作序列：任何组合下都不越界、输出合法或 -1（ASan 盯越界） */
static void test_random_sequences(void)
{
    srand(12345);
    static const char *strs[] = { "", "a", "\"\\", "\xe4\xb8\xad", "\xff\xfe", "long string with spaces and \x01 control" };
    for (int round = 0; round < 20000; round++) {
        size_t cap = 1 + (size_t)(rand() % 200);
        char *b = malloc(cap);
        hj_t w;
        hj_init(&w, b, cap);
        if (rand() % 4 == 0) hj_set_reserve(&w, (size_t)(rand() % 40));
        hj_obj_begin(&w);
        int ops = rand() % 40;
        hj_mark_t m = {0};
        bool marked = false;
        for (int k = 0; k < ops && !w.error; k++) {
            bool in_obj = w.depth > 0 && w.stack[w.depth - 1] == '{';
            int r = rand() % 10;
            if (in_obj && !w.after_key) {
                if (r < 7) hj_key(&w, strs[rand() % 6]);
                else if (w.depth > 1) hj_obj_end(&w);
            } else {
                if (r == 0) hj_obj_begin(&w);
                else if (r == 1) hj_arr_begin(&w);
                else if (r == 2 && !in_obj && w.depth > 1) hj_arr_end(&w);
                else if (r == 3) hj_int(&w, rand() - RAND_MAX / 2);
                else if (r == 4) hj_bool(&w, rand() & 1);
                else if (r == 5 && !in_obj && !marked && !w.truncated) { m = hj_mark(&w); marked = true; }
                else if (r == 6 && marked && !in_obj) { hj_rollback(&w, m); marked = false; }
                else hj_str(&w, strs[rand() % 6]);
            }
        }
        int n = hj_finish(&w);
        if (n >= 0) {
            CHECK(json_valid(b, (size_t)n), "random round %d cap %zu: invalid %s", round, cap, b);
            CHECK((size_t)n < cap, "random overrun");
        }
        free(b);
        if (g_fail > 20) return;
    }
}

/* 定点数（M5 第一步的读数）：小数位照写、负数、零、整数位为 0 的负数、decimals 0、越界 */
static void test_fixed(void)
{
    char b[256];
    hj_t w;
    hj_init(&w, b, sizeof(b));
    hj_obj_begin(&w);
    hj_kfixed(&w, "t", -512, 2);
    hj_kfixed(&w, "h", 4567, 2);
    hj_kfixed(&w, "p", 10130, 1);
    hj_kfixed(&w, "z", 0, 2);
    hj_kfixed(&w, "n", -5, 2);
    hj_kfixed(&w, "i", 42, 0);
    hj_kfixed(&w, "m", 1000000, 6);
    hj_obj_end(&w);
    int n = hj_finish(&w);
    const char *want = "{\"t\":-5.12,\"h\":45.67,\"p\":1013.0,\"z\":0.00,\"n\":-0.05,\"i\":42,\"m\":1.000000}";
    CHECK(n == (int)strlen(want) && strcmp(b, want) == 0, "fixed: got %s", b);
    hj_init(&w, b, sizeof(b));
    hj_fixed(&w, 1, 7);
    CHECK(hj_finish(&w) < 0, "fixed: decimals > 6 is an error");
    hj_init(&w, b, sizeof(b));
    hj_fixed(&w, INT64_MIN, 2);
    CHECK(hj_finish(&w) < 0, "fixed: INT64_MIN with decimals is an error");
    hj_init(&w, b, sizeof(b));
    hj_fixed(&w, INT64_MAX, 3);
    n = hj_finish(&w);
    CHECK(n > 0 && strcmp(b, "9223372036854775.807") == 0, "fixed: INT64_MAX %s", b);
}

/* hj_raw（M7 第三步：作业的结果体原样放进 link.result 的 payload）：当一个值用，逗号与键照常；放不下整个不写；空串置 error */
static void test_raw(void)
{
    char b[96];
    hj_t w;
    hj_init(&w, b, sizeof(b));
    hj_obj_begin(&w);
    hj_kstr(&w, "a", "x");
    hj_key(&w, "r");
    hj_raw(&w, "{\"id\":\"mt-0000000000000002\"}");
    hj_kint(&w, "n", 1);
    hj_obj_end(&w);
    int n = hj_finish(&w);
    CHECK(n > 0 && strcmp(b, "{\"a\":\"x\",\"r\":{\"id\":\"mt-0000000000000002\"},\"n\":1}") == 0, "raw: %s", b);
    char s[24];
    hj_init(&w, s, sizeof(s));
    hj_obj_begin(&w);
    hj_key(&w, "r");
    hj_raw(&w, "{\"long\":\"0123456789abcdef\"}");
    n = hj_finish(&w);
    CHECK(n > 0 && strcmp(s, "{}") == 0 && w.truncated, "raw 放不下：连键一起撤、truncated（%s）", s);
    hj_init(&w, b, sizeof(b));
    hj_obj_begin(&w);
    hj_key(&w, "r");
    hj_raw(&w, "");
    CHECK(hj_finish(&w) < 0, "raw 空串：error");
}

int main(void)
{
    test_raw();
    test_fixed();
    test_validator();
    test_basic();
    test_escape_and_utf8();
    test_misuse();
    test_key_value_atomic();
    test_pagination_all_caps();
    test_truncated_finish_all_caps();
    test_rollback_guards();
    test_reserve();
    test_reserve_overflow();
    test_stale_mark_and_mark_after_truncation();
    test_two_paginated_lists();
    test_random_sequences();
    printf("hub_json: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
