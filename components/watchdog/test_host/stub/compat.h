#pragma once
/* strlcpy 在旧 glibc 上不存在，用 -include 强制注入一份实现，与宿主无关。 */
#include <stddef.h>
#include <string.h>
static inline size_t wd_test_strlcpy(char *d, const char *s, size_t n)
{
    size_t sl = strlen(s);
    if (n) { size_t c = sl < n - 1 ? sl : n - 1; memcpy(d, s, c); d[c] = '\0'; }
    return sl;
}
#define strlcpy wd_test_strlcpy
