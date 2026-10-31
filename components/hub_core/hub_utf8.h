#pragma once
/* hub_utf8 —— hub_json 与 hub_sanitize 共用的 UTF-8 判定（组件内部头文件，不进 include/）。纯 C，不依赖 ESP-IDF。 */
#include <stddef.h>
#include <stdint.h>

/* s[i] 起的一个合法 UTF-8 序列的字节数（1–4）；不合法返回 0。
   不合法 = 孤立的续字节、C0 / C1 / F5–FF 开头（超长编码与超过 U+10FFFF 的引导字节）、续字节不够或不对、
   三字节的超长编码与代理区（U+D800–DFFF）、四字节的超长编码与超过 U+10FFFF。*cp 可为 NULL。 */
static inline size_t hub_utf8_seq(const unsigned char *s, size_t n, size_t i, uint32_t *cp)
{
    if (i >= n) return 0;
    const unsigned char c = s[i];
    size_t len;
    uint32_t v;
    if (c < 0x80) {
        if (cp) *cp = c;
        return 1;
    } else if (c >= 0xC2 && c <= 0xDF) {
        len = 2; v = c & 0x1Fu;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3; v = c & 0x0Fu;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4; v = c & 0x07u;
    } else {
        return 0;
    }
    if (len > n - i) return 0;
    for (size_t k = 1; k < len; k++) {
        if ((s[i + k] & 0xC0u) != 0x80u) return 0;
        v = (v << 6) | (s[i + k] & 0x3Fu);
    }
    if (len == 3 && (v < 0x800u || (v >= 0xD800u && v <= 0xDFFFu))) return 0;
    if (len == 4 && (v < 0x10000u || v > 0x10FFFFu)) return 0;
    if (cp) *cp = v;
    return len;
}
