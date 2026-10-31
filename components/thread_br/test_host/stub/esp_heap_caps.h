#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#define MALLOC_CAP_DMA       (1<<3)
#define MALLOC_CAP_SPIRAM    (1<<10)
#define MALLOC_CAP_INTERNAL  (1<<11)
#define MALLOC_CAP_DEFAULT   (1<<12)
extern int g_fake_spiram_fail;   /* 置 1 模拟 PSRAM 取不到，验回退路径 */
static inline void *heap_caps_calloc(size_t n, size_t sz, uint32_t caps)
{
    if ((caps & MALLOC_CAP_SPIRAM) && g_fake_spiram_fail) return NULL;
    return calloc(n, sz);
}
