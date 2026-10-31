#include "utf8_trunc.h"

#include <string.h>

/* 按 UTF-8 **字符边界**截断复制。
 *
 * 原来这里是 `strlcpy`，它按字节砍，会把一个多字节字符切成两半，回包就不再是
 * 合法 UTF-8。2026-09-15 真发生过：一条 164 字节的中文 notes 被砍在第 159 字节，
 * `/api/v1/ota/check` 的末尾留下一个孤立的 0xE6，严格的 JSON 解析器直接报
 * "invalid continuation byte"，浏览器则显示成一个替换字符。
 *
 * ⚠️ 现场设备取的是**云端**的 notes，所以这个 bug 由发布者写多长的说明触发，
 * 设备侧只能自保；发布侧的长度闸门在 tools/make_manifest.py（上限从
 * ota_cloud.h 的 notes[N] 读，不抄第二份）。
 *
 * ⚠️ 不用 strlcpy：那是 BSD 的，glibc 没有，而本文件要能在主机上编译测试。
 */
void utf8_safe_copy(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0) return;
    if (!src) { dst[0] = '\0'; return; }

    size_t n = strlen(src);
    if (n >= dst_sz) n = dst_sz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    if (n == 0) return;

    /* 退到最后一个字符的首字节 */
    size_t i = n - 1;
    while (i > 0 && ((unsigned char)dst[i] & 0xC0) == 0x80) i--;

    unsigned char c = (unsigned char)dst[i];
    size_t need = (c < 0x80)           ? 1
                : ((c & 0xE0) == 0xC0) ? 2
                : ((c & 0xF0) == 0xE0) ? 3
                : ((c & 0xF8) == 0xF0) ? 4
                : 1;                      /* 畸形输入：当单字节处理，别死循环 */

    /* 最后那个字符没凑齐就整个丢掉。没截断时 i + need == n，什么都不做。 */
    if (i + need > n) dst[i] = '\0';
}
