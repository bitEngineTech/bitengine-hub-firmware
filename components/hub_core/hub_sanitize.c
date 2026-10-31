/* hub_sanitize —— 字符串卫生（见 include/hub_sanitize.h）。纯 C，不依赖 ESP-IDF。 */
#include "hub_sanitize.h"
#include "hub_utf8.h"

#include <stdint.h>
#include <string.h>

typedef enum { CP_KEEP, CP_DROP, CP_SPACE, CP_NEWLINE } cp_class_t;

/* Unicode 一般类别 Cf（格式字符，Unicode 15.1 的全部码位）：软连字符、阿拉伯数字前缀、双向控制（含 U+061C ALM）、
   零宽字符、词连接符、不可见运算符、BOM、行间注释、标签字符（U+E0000–E007F，对大模型的 ASCII smuggling 就用它）…
   它们在网页上看不见、模型却照样读到（2026-10-22 第二轮审计 A4：只去掉一部分不够） */
static bool is_cf(uint32_t cp)
{
    return cp == 0x00ADu || (cp >= 0x0600u && cp <= 0x0605u) || cp == 0x061Cu || cp == 0x06DDu || cp == 0x070Fu
        || (cp >= 0x0890u && cp <= 0x0891u) || cp == 0x08E2u || cp == 0x180Eu
        || (cp >= 0x200Bu && cp <= 0x200Fu) || (cp >= 0x202Au && cp <= 0x202Eu) || (cp >= 0x2060u && cp <= 0x2064u)
        || (cp >= 0x2066u && cp <= 0x206Fu) || cp == 0xFEFFu || (cp >= 0xFFF9u && cp <= 0xFFFBu)
        || cp == 0x110BDu || cp == 0x110CDu || (cp >= 0x13430u && cp <= 0x1343Fu) || (cp >= 0x1BCA0u && cp <= 0x1BCA3u)
        || (cp >= 0x1D173u && cp <= 0x1D17Au) || cp == 0xE0001u || (cp >= 0xE0020u && cp <= 0xE007Fu);
}

/* Zs（空格类）除 U+0020 外的码位：NBSP、全角空格等，当普通空格压缩 */
static bool is_zs(uint32_t cp)
{
    return cp == 0x00A0u || cp == 0x1680u || (cp >= 0x2000u && cp <= 0x200Au) || cp == 0x202Fu || cp == 0x205Fu
        || cp == 0x3000u;
}

/* keep_joiners：用户起的名字里保留 ZWNJ / ZWJ（U+200C / U+200D）—— 去掉会把 emoji 组合（家庭、旗帜）拆开、
   把波斯文写坏（审计 C2）；设备字符串里照样去掉 */
static cp_class_t classify(uint32_t cp, bool keep_joiners)
{
    if (cp == ' ' || cp == '\t' || is_zs(cp)) return CP_SPACE;
    if (cp == '\n' || cp == '\r' || cp == '\v' || cp == '\f' || cp == 0x2028u || cp == 0x2029u) return CP_NEWLINE;
    if (cp < 0x20u || cp == 0x7Fu) return CP_DROP;                         /* C0 其余、DEL */
    if (cp >= 0x80u && cp <= 0x9Fu) return CP_DROP;                        /* C1（含 U+0085） */
    if (keep_joiners && (cp == 0x200Cu || cp == 0x200Du)) return CP_KEEP;
    if (is_cf(cp)) return CP_DROP;
    return CP_KEEP;
}

/* 共用的净化：最多写 limit 字节进 out 并补 NUL（调用方保证 out 至少 limit + 1 字节）。
   *saw_newline：见过换行类字符（设备字符串把它当空格，用户名据此拒绝）；*over：还有要保留的字符没放下。 */
static size_t sanitize(const unsigned char *in, size_t n, char *out, size_t limit, bool keep_joiners, bool *saw_newline, bool *over)
{
    size_t o = 0;
    bool pending_space = false;                /* 空白先记着，后面真有字符才写：首尾空白自然去掉，连续空白压成一个 */
    *saw_newline = false;
    *over = false;
    for (size_t i = 0; i < n;) {
        uint32_t cp = 0;
        const size_t k = hub_utf8_seq(in, n, i, &cp);
        const unsigned char *src = in + i;
        size_t sl = k;
        cp_class_t c;
        if (k == 0) {                          /* 坏字节 → '?' */
            static const unsigned char q = '?';
            src = &q;
            sl = 1;
            i += 1;
            c = CP_KEEP;
        } else {
            i += k;
            c = classify(cp, keep_joiners);
        }
        if (c == CP_NEWLINE) {
            *saw_newline = true;
            c = CP_SPACE;
        }
        if (c == CP_DROP) continue;
        if (c == CP_SPACE) {
            if (o > 0) pending_space = true;
            continue;
        }
        const size_t need = sl + (pending_space ? 1u : 0u);
        if (need > limit - o) {                /* 放不下：在码点边界截断（结尾的空格也就不写了） */
            *over = true;
            break;
        }
        if (pending_space) {
            out[o++] = ' ';
            pending_space = false;
        }
        memcpy(out + o, src, sl);
        o += sl;
    }
    out[o] = '\0';
    return o;
}

size_t hub_sanitize_device_str(const char *in, size_t in_len, char *out, size_t out_cap, size_t max)
{
    if (!out || out_cap == 0) return 0;
    if (!in) {
        out[0] = '\0';
        return 0;
    }
    const size_t limit = max < out_cap - 1 ? max : out_cap - 1;
    bool nl, over;
    return sanitize((const unsigned char *)in, in_len, out, limit, false, &nl, &over);
}

hub_name_verdict_t hub_sanitize_user_name(const char *in, char *out, size_t out_cap, size_t max)
{
    if (!in || !out || out_cap == 0 || out_cap <= max) {
        if (out && out_cap) out[0] = '\0';
        return HUB_NAME_BAD_ARG;
    }
    bool nl, over;
    (void)sanitize((const unsigned char *)in, strlen(in), out, max, true, &nl, &over);
    if (nl) {
        out[0] = '\0';
        return HUB_NAME_NEWLINE;
    }
    if (over) {
        out[0] = '\0';
        return HUB_NAME_TOO_LONG;
    }
    return HUB_NAME_OK;
}

const char *hub_name_verdict_str(hub_name_verdict_t v)
{
    switch (v) {
    case HUB_NAME_OK:       return "ok";
    case HUB_NAME_BAD_ARG:  return "bad_arg";
    case HUB_NAME_NEWLINE:  return "newline";
    case HUB_NAME_TOO_LONG: return "too_long";
    }
    return "unknown";
}
