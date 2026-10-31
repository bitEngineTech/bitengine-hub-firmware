/* UTF-8 校验与修复。规则与谁在用见 include/utf8.h。
 * 只依赖 libc —— 主机侧测试直接编这个文件。 */
#include <string.h>

#include "utf8.h"

size_t utf8_scan(const unsigned char *p, size_t *consumed)
{
    unsigned char c = p[0];
    unsigned char lo = 0x80, hi = 0xBF;   /* 第二字节的取值范围 */
    size_t need;

    if (c < 0x80)                      { *consumed = 1; return 1; }
    else if (c >= 0xC2 && c <= 0xDF)   need = 2;
    else if (c == 0xE0)                { need = 3; lo = 0xA0; }
    else if (c >= 0xE1 && c <= 0xEC)   need = 3;
    else if (c == 0xED)                { need = 3; hi = 0x9F; }
    else if (c >= 0xEE && c <= 0xEF)   need = 3;
    else if (c == 0xF0)                { need = 4; lo = 0x90; }
    else if (c >= 0xF1 && c <= 0xF3)   need = 4;
    else if (c == 0xF4)                { need = 4; hi = 0x8F; }
    else                               { *consumed = 1; return 0; }  /* 80-C1、F5-FF 不能当首字节 */

    size_t i = 1;
    for (; i < need; i++) {
        if (p[i] < lo || p[i] > hi) break;
        lo = 0x80; hi = 0xBF;             /* 第三、四字节一律 80-BF */
    }
    *consumed = i;
    return (i == need) ? need : 0;
}

bool utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    while (*p) {
        size_t consumed;
        if (!utf8_scan(p, &consumed)) return false;
        p += consumed;
    }
    return true;
}

size_t utf8_repair_copy(char *dst, size_t dst_size, const char *src,
                        bool (*drop)(unsigned char c))
{
    if (!dst || dst_size == 0) return 0;

    static const char replacement[] = "\xEF\xBF\xBD";   /* U+FFFD */
    const unsigned char *p = (const unsigned char *)(src ? src : "");
    size_t w = 0;

    while (*p) {
        const char *out = (const char *)p;
        size_t out_len = 1, consumed = 1;

        if (*p >= 0x80) {
            out_len = utf8_scan(p, &consumed);
            if (!out_len) { out = replacement; out_len = 3; }
        } else if (drop && drop(*p)) {
            p++;
            continue;
        }

        if (w + out_len >= dst_size) break;       /* 宁可截断，不可写半个字符 */
        memcpy(dst + w, out, out_len);
        w += out_len;
        p += consumed;
    }
    dst[w] = '\0';
    return w;
}
