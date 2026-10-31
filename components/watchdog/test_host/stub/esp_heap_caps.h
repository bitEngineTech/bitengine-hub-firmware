#pragma once
#include <stdint.h>
#include <stddef.h>
#define MALLOC_CAP_EXEC      (1<<0)
#define MALLOC_CAP_32BIT     (1<<1)
#define MALLOC_CAP_8BIT      (1<<2)
#define MALLOC_CAP_DMA       (1<<3)
#define MALLOC_CAP_SPIRAM    (1<<10)
#define MALLOC_CAP_INTERNAL  (1<<11)
#define MALLOC_CAP_DEFAULT   (1<<12)
extern uint32_t g_fake_internal_free, g_fake_dma_free;
static inline size_t heap_caps_get_free_size(uint32_t c)
{ return (c & MALLOC_CAP_DMA) ? g_fake_dma_free : g_fake_internal_free; }
static inline size_t heap_caps_get_minimum_free_size(uint32_t c) { return heap_caps_get_free_size(c); }
static inline size_t heap_caps_get_largest_free_block(uint32_t c) { return heap_caps_get_free_size(c); }
static inline size_t heap_caps_get_total_size(uint32_t c) { (void)c; return 175084; }
typedef void (*esp_alloc_failed_hook_t)(size_t, uint32_t, const char *);
static inline int heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t cb) { (void)cb; return 0; }
