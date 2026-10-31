/* 主机侧测试：外来字符串进 JSON 的三种拷贝 + UTF-8 判据（T9.24 / T9.32 / T9.33）——
 * 编的是**产品源文件本身**：../json_escape.c 的 webui_json_escape（转义）、json_sanitize_copy
 * （剥离），以及 components/utf8/utf8.c 的 utf8_repair_copy（只修，给 cJSON）、utf8_valid（判据，
 * 写配置的入口靠它拒收）。
 *
 * 两部分：
 *   1. 固定用例：转义 / 剥离、合法 UTF-8 的边界码点、各类非法序列、截断边界；
 *   2. 对照用例：三种拷贝与 utf8_valid 各两万条随机字节串（偏向各种首字节 / 续字节 / 控制字符）写进
 *      argv[1]，由 json_escape_check.py 拿 Python 的 bytes.decode('utf-8','replace')
 *      逐条对照 —— 那是一份独立实现，"一段非法字节换一个 U+FFFD"的切法以它为准。
 *
 * 为什么必须有：/api/v1/logs 的正文里可能有外部送进来的任意字节（不用登录就能把
 * \xff 塞进 Host 头，跨站守卫会原样写进 W 日志）。输出一旦不是合法 UTF-8，浏览器
 * 看不出来，读日志的脚本却会直接报错。 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "../../utf8/utf8.c"
#include "../json_escape.c"

static int pass = 0, fail = 0;

static void show(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p >= 0x20 && *p < 0x7f) putchar(*p);
        else printf("\\x%02x", *p);
    }
}

/* 三种拷贝统一成 (dst, size, src) → 写入长度 */
typedef size_t (*copy_fn)(char *, size_t, const char *);
static size_t do_sanitize(char *dst, size_t dst_size, const char *src)
{
    json_sanitize_copy(dst, src, dst_size);
    return dst_size ? strlen(dst) : 0;
}
static size_t do_repair(char *dst, size_t dst_size, const char *src)
{
    return utf8_repair_copy(dst, dst_size, src, NULL);
}

/* 输入 src、目标缓冲 dst_size，期望输出 want（以及返回值 = strlen(want)） */
static void check(copy_fn fn, const char *what, const char *src, size_t dst_size, const char *want)
{
    char dst[512];
    memset(dst, 0x5A, sizeof(dst));
    size_t got = fn(dst, dst_size, src);
    int ok = (dst_size == 0)
           ? (got == 0 && (unsigned char)dst[0] == 0x5A)            /* 一个字节都不许写 */
           : (got == strlen(want) && !strcmp(dst, want) && got < dst_size
              && (dst_size >= sizeof(dst) || (unsigned char)dst[dst_size] == 0x5A));  /* 没写出界 */
    if (ok) { printf("✅ %s\n", what); pass++; return; }
    printf("❌ %s —— 期望 \"", what); show(want);
    printf("\"(%zu)，实得 \"", strlen(want)); show(dst); printf("\"(%zu)\n", got);
    fail++;
}

#define E(...) check(webui_json_escape,   __VA_ARGS__)
#define S(...) check(do_sanitize,         __VA_ARGS__)
#define U(...) check(do_repair,           __VA_ARGS__)

static void V(const char *what, const char *src, int want)
{
    int got = utf8_valid(src) ? 1 : 0;
    if (got == want) { printf("✅ %s\n", what); pass++; return; }
    printf("❌ %s —— 期望 %s，实得 %s\n", what, want ? "合法" : "非法", got ? "合法" : "非法");
    fail++;
}

#define BIG 512

/* ---- 随机字节串（对照用）---- */
static uint32_t s_rng = 0x9E3779B9u;
static uint32_t rnd(void)            /* xorshift32，固定种子保证可复现 */
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}
static unsigned char rnd_in(unsigned lo, unsigned hi) { return (unsigned char)(lo + rnd() % (hi - lo + 1)); }

/* 把码点 cp 编成合法 UTF-8 追加到 b（调用方保证 cp 合法、空间够） */
static int put_cp(unsigned char *b, uint32_t cp)
{
    if (cp < 0x80)    { b[0] = (unsigned char)cp; return 1; }
    if (cp < 0x800)   { b[0] = 0xC0 | (cp >> 6);  b[1] = 0x80 | (cp & 0x3F); return 2; }
    if (cp < 0x10000) { b[0] = 0xE0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 0x3F);
                        b[2] = 0x80 | (cp & 0x3F); return 3; }
    b[0] = 0xF0 | (cp >> 18); b[1] = 0x80 | ((cp >> 12) & 0x3F);
    b[2] = 0x80 | ((cp >> 6) & 0x3F); b[3] = 0x80 | (cp & 0x3F); return 4;
}

static size_t gen(unsigned char *b, size_t cap)
{
    size_t n = 0, len = rnd() % 25;
    while (n < len && n + 4 < cap) {
        switch (rnd() % 12) {
        case 0:  b[n++] = rnd_in(0x20, 0x7E); break;                 /* 可打印 ASCII（含 " \） */
        case 1:  b[n++] = (rnd() & 1) ? '"' : '\\'; break;
        case 2:  b[n++] = rnd_in(0x01, 0x1F); break;                 /* 控制字符 */
        case 3:  b[n++] = rnd_in(0x80, 0xBF); break;                 /* 孤立续字节 */
        case 4:  b[n++] = rnd_in(0xC0, 0xC1); break;                 /* 过长编码首字节 */
        case 5:  b[n++] = rnd_in(0xC2, 0xF4); break;                 /* 合法首字节，后面随缘 */
        case 6:  b[n++] = rnd_in(0xF5, 0xFF); break;                 /* 永远非法 */
        case 7:  b[n++] = (unsigned char[]){0xE0, 0xED, 0xF0, 0xF4}[rnd() % 4]; break;  /* 第二字节范围特殊的四个 */
        case 8:  b[n++] = (unsigned char[]){0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF}[rnd() % 6]; break;  /* 第二字节范围的边 */
        default: {                                                    /* 一个合法字符 */
            uint32_t cp;
            do { cp = rnd() % 0x110000; } while (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF));
            n += put_cp(b + n, cp);
            break; }
        }
    }
    b[n] = 0;
    return n;
}

static void dump_hex(FILE *f, const unsigned char *b, size_t n)
{
    if (!n) { fputc('-', f); return; }
    for (size_t i = 0; i < n; i++) fprintf(f, "%02x", b[i]);
}

int main(int argc, char **argv)
{
    printf("── 转义 ──\n");
    E("纯 ASCII 原样",            "hello world", BIG, "hello world");
    E("空串",                     "",            BIG, "");
    E("NULL 当空串",              NULL,          BIG, "");
    E("引号与反斜杠",             "a\"b\\c",     BIG, "a\\\"b\\\\c");
    E("换行 回车 制表",           "\n\r\t",      BIG, "\\n\\r\\t");
    E("其余控制字符走 \\u00XX",   "\x01\x1f",    BIG, "\\u0001\\u001f");
    E("DEL 不必转义",             "\x7f",        BIG, "\x7f");

    printf("── 合法 UTF-8 原样拷贝（边界码点）──\n");
    E("中文",                     "网关日志",    BIG, "网关日志");
    E("U+0080 / U+07FF",          "\xC2\x80\xDF\xBF",          BIG, "\xC2\x80\xDF\xBF");
    E("U+0800 / U+D7FF",          "\xE0\xA0\x80\xED\x9F\xBF",  BIG, "\xE0\xA0\x80\xED\x9F\xBF");
    E("U+E000 / U+FFFF",          "\xEE\x80\x80\xEF\xBF\xBF",  BIG, "\xEE\x80\x80\xEF\xBF\xBF");
    E("U+10000 / U+10FFFF",       "\xF0\x90\x80\x80\xF4\x8F\xBF\xBF", BIG, "\xF0\x90\x80\x80\xF4\x8F\xBF\xBF");
    E("emoji",                    "\xF0\x9F\x98\x80", BIG, "\xF0\x9F\x98\x80");

    printf("── 非法字节换成 \\ufffd（一段一个）──\n");
    E("Host 头里的 \\xff",        "Host=\xff",   BIG, "Host=\\ufffd");
    E("孤立续字节",               "a\x80z",      BIG, "a\\ufffdz");
    E("C0 80 过长编码 = 两段",    "\xC0\x80",    BIG, "\\ufffd\\ufffd");
    E("E0 80 80 过长编码 = 三段", "\xE0\x80\x80", BIG, "\\ufffd\\ufffd\\ufffd");
    E("F0 80 80 80 过长编码 = 四段", "\xF0\x80\x80\x80", BIG, "\\ufffd\\ufffd\\ufffd\\ufffd");
    E("ED A0 80 代理区 = 三段",   "\xED\xA0\x80", BIG, "\\ufffd\\ufffd\\ufffd");
    E("F4 90 80 80 超 U+10FFFF",  "\xF4\x90\x80\x80", BIG, "\\ufffd\\ufffd\\ufffd\\ufffd");
    E("F5 永远非法",              "\xF5",        BIG, "\\ufffd");
    E("缺尾的三字节 + ASCII",     "\xE4\xB8" "A", BIG, "\\ufffdA");
    E("缺尾的三字节在串尾",       "x\xE4\xB8",   BIG, "x\\ufffd");
    E("缺尾的四字节 = 一段",      "\xF0\x9F\x98" "!", BIG, "\\ufffd!");
    E("缺尾后紧跟合法字符",       "\xE4\xB8\xE7\xBD\x91", BIG, "\\ufffd\xE7\xBD\x91");
    E("非法字节不吞后面的引号",   "\xC3\"",      BIG, "\\ufffd\\\"");

    printf("── 截断：绝不写半个序列 ──\n");
    E("dst_size=0 什么都不写",    "abc",         0, "");
    E("dst_size=1 只有 NUL",      "abc",         1, "");
    E("正好装下",                 "abc",         4, "abc");
    E("差一个字节",               "abc",         3, "ab");
    E("转义序列装不下就停",       "a\n",         3, "a");
    E("\\u00XX 装不下就停",       "a\x01",       6, "a");
    E("三字节字符装不下就停",     "a网",         4, "a");
    E("三字节字符正好装下",       "a网",         5, "a网");
    E("四字节字符装不下就停",     "\xF0\x9F\x98\x80", 4, "");
    E("\\ufffd 装不下就停",       "a\xff",       7, "a");
    E("\\ufffd 正好装下",         "a\xff",       8, "a\\ufffd");

    printf("── json_sanitize_copy：剥掉 \" \\ 控制字符，非法字节换原样 U+FFFD（T9.32）──\n");
    S("纯 ASCII 原样",            "bitengine",   BIG, "bitengine");
    S("引号 反斜杠 控制字符剥掉", "a\"b\\c\n\x01" "d", BIG, "abcd");
    S("NULL 当空串",              NULL,          BIG, "");
    S("中文 SSID 原样",           "我家WiFi",    BIG, "我家WiFi");
    S("GBK 编码的「中文」",       "\xD6\xD0\xCE\xC4", BIG, "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
    S("孤立 \\xff",              "ab\xff",     BIG, "ab\xEF\xBF\xBD");
    S("缺尾三字节 = 一个 U+FFFD", "\xE4\xB8" "x", BIG, "\xEF\xBF\xBD" "x");
    S("非法字节后的引号照样剥",   "\xC3\"",    BIG, "\xEF\xBF\xBD");
    S("dst_size=0 什么都不写",    "abc",         0, "");
    S("差一个字节",               "abc",         3, "ab");
    S("被剥的字符不占空间",       "\"\"ab",    3, "ab");
    S("中文装不下就停（不切半个）", "a网",       4, "a");
    S("U+FFFD 装不下就停",        "a\xff",      4, "a");
    S("U+FFFD 正好装下",          "a\xff",      5, "a\xEF\xBF\xBD");
    S("老接口 33 字节 SSID 缓冲：32 字节中文名不截断",
      "中文中文中文中文中文12", 33, "中文中文中文中文中文12");

    printf("── utf8_repair_copy：只修 UTF-8，引号与控制字符留给 cJSON 转义（T9.32）──\n");
    U("引号 反斜杠 控制字符原样", "a\"b\\c\n", BIG, "a\"b\\c\n");
    U("GBK 编码的「中文」",       "\xD6\xD0\xCE\xC4", BIG, "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
    U("合法中文原样",             "我家WiFi",    BIG, "我家WiFi");
    U("U+FFFD 装不下就停",        "a\xff",      4, "a");
    U("32 字节全非法：3 倍缓冲装得下", 
      "\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff"
      "\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff", 32 * 3 + 1,
      "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"
      "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"
      "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"
      "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");

    printf("── utf8_valid：写配置的入口靠它拒收（T9.33）──\n");
    V("空串合法",                 "",                        1);
    V("NULL 当空串",              NULL,                      1);
    V("ASCII 合法",               "mqtt.local",              1);
    V("中文合法",                 "我家网关",                1);
    V("U+10FFFF 合法",            "\xF4\x8F\xBF\xBF",       1);
    V("孤立 \\xff 非法",         "user\xff",               0);
    V("GBK「中文」非法",          "\xD6\xD0\xCE\xC4",       0);
    V("C0 80 过长编码非法",       "\xC0\x80",               0);
    V("ED A0 80 代理区非法",      "\xED\xA0\x80",           0);
    V("缺尾在串尾非法",           "ok\xE4\xB8",             0);
    V("U+FFFD 本身合法",          "\xEF\xBF\xBD",           1);

    /* ---- 对照用例写到文件，交给 Python ---- */
    if (argc > 1) {
        FILE *f = fopen(argv[1], "w");
        if (!f) { perror(argv[1]); return 2; }
        unsigned char in[128];
        char out[1024];
        static const struct { char mode; copy_fn fn; } modes[] = {
            { 'e', webui_json_escape }, { 's', do_sanitize }, { 'u', do_repair },
        };
        for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
            for (int i = 0; i < 20000; i++) {
                size_t n = gen(in, sizeof(in));
                /* 一半给足空间（必须与 Python 完全相等），一半随机截断 */
                size_t ds = (i & 1) ? n * 6 + 1 : 1 + rnd() % (n * 6 + 1);
                size_t w = modes[m].fn(out, ds, (const char *)in);
                fprintf(f, "%c %zu ", modes[m].mode, ds); dump_hex(f, in, n);
                fputc(' ', f); dump_hex(f, (const unsigned char *)out, w);
                fputc('\n', f);
            }
        }
        for (int i = 0; i < 20000; i++) {                 /* utf8_valid：输出 1 / 0 */
            size_t n = gen(in, sizeof(in));
            fprintf(f, "v 0 "); dump_hex(f, in, n);
            fprintf(f, " %d\n", utf8_valid((const char *)in) ? 1 : 0);
        }
        fclose(f);
    }

    printf("\nResult: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
