/* 版本号比较 —— 刻意不依赖任何 ESP-IDF 头文件。
 *
 * 单独成文件的唯一理由：让 test_host/ 能**原样编译这个文件**去测，
 * 而不是抄一份副本过去测。抄副本的测试只能证明副本是对的。
 *
 * 这段代码是防降级那道闸的全部实现（ota_cloud_install() 用
 * cmp(latest, current) > 0 做判据）。真值表见 test_host/version_test.c。
 */
#include "version_cmp.h"

#include <string.h>

/* 解析 git describe 格式 "v<major>.<minor>.<patch>-<commits>-g<sha>[-dirty]"。
   缺失的段按 0；数字上限截断防溢出 —— 输入来自网络。

   ⚠️ 最容易写错的一处：'-' 后面**只有紧跟数字时**才是 commits 计数。
   "v0.5.0-g1aac8a0" 里 g 后面是十六进制 sha，无脑读数字会把 "1aac8a0"
   读成 1，于是它会莫名其妙地"比裸 tag 新"。 */
static void parse_ver(const char *s, int out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
    if (!s) return;
    while (*s == 'v' || *s == 'V' || *s == ' ') s++;

    for (int i = 0; i < 3; i++) {
        if (*s < '0' || *s > '9') return;
        long v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            if (v > 999999) { v = 999999; while (*s >= '0' && *s <= '9') s++; break; }
            s++;
        }
        out[i] = (int)v;
        if (*s != '.') break;
        s++;
    }

    if (*s == '-' && s[1] >= '0' && s[1] <= '9') {
        const char *p = s + 1;
        long v = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            if (v > 999999) { v = 999999; break; }
            p++;
        }
        out[3] = (int)v;
    }
}

int ota_cloud_version_cmp(const char *a, const char *b)
{
    int va[4], vb[4];
    parse_ver(a, va);
    parse_ver(b, vb);
    for (int i = 0; i < 4; i++) {
        if (va[i] != vb[i]) return va[i] > vb[i] ? 1 : -1;
    }
    return 0;
}

static bool nz(const char *s) { return s && s[0]; }

/* 指纹是十六进制，大小写不敏感；不用 strcasecmp —— 本文件不依赖 POSIX，主机测试用 -std=c11 编 */
static bool hex_equal(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = (*a >= 'A' && *a <= 'F') ? (char)(*a - 'A' + 'a') : *a;
        char y = (*b >= 'A' && *b <= 'F') ? (char)(*b - 'A' + 'a') : *b;
        if (x != y) return false;
    }
    return *a == *b;
}

bool ota_cloud_ncp_differs(const char *rec_sha, const char *cur_build, const char *cur_version,
                           const char *m_sha, const char *m_build, const char *m_version)
{
    if (nz(rec_sha)) return !hex_equal(rec_sha, m_sha ? m_sha : "");
    if (nz(cur_build) && nz(m_build)) return strcmp(cur_build, m_build) != 0;
    if (!nz(cur_version) || strcmp(cur_version, "unknown") == 0) return true;
    return strcmp(cur_version, m_version ? m_version : "") != 0;
}
