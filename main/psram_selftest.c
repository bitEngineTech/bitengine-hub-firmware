#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"

#include "psram_selftest.h"

static const char *TAG = "psram_test";

#define PSRAM_CAPS        (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define MIN_CHUNK_BYTES   (64 * 1024)   /* stop when free fragments get smaller */
#define ALLOC_MARGIN      1024          /* headroom for allocator bookkeeping   */
#define CACHE_ALIGN       64            /* >= C5 data cache line size           */
#define MAX_CHUNKS        16
#define MAX_FAULT_LOGS    4

/* Address-derived pattern: each word stores its own address (pass 0) or its
   inverse (pass 1). Catches stuck data bits AND address-line aliasing, which
   uniform 0xAA/0x55 fills cannot. */
static size_t test_chunk(uint8_t *buf, size_t size, size_t *fault_logs)
{
    volatile uint32_t *w = (volatile uint32_t *)buf;
    size_t words = size / sizeof(uint32_t);
    size_t errors = 0;

    for (int pass = 0; pass < 2; pass++) {
        uint32_t inv = pass ? 0xFFFFFFFFu : 0u;

        for (size_t i = 0; i < words; i++) {
            w[i] = (uint32_t)(uintptr_t)&w[i] ^ inv;
        }

        /* Writeback + invalidate so the verify pass reads the PSRAM die,
           not the data cache. */
        esp_err_t err = esp_cache_msync(buf, size,
                ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "cache msync failed (%s) — chunk %p skipped",
                     esp_err_to_name(err), buf);
            return 1;
        }

        for (size_t i = 0; i < words; i++) {
            uint32_t expect = (uint32_t)(uintptr_t)&w[i] ^ inv;
            uint32_t got = w[i];
            if (got != expect) {
                if ((*fault_logs)++ < MAX_FAULT_LOGS) {
                    ESP_LOGE(TAG, "fault @%p: wrote 0x%08" PRIx32
                             " read 0x%08" PRIx32, (void *)&w[i], expect, got);
                }
                errors++;
            }
        }

        vTaskDelay(1);  /* let idle task breathe between passes */
    }
    return errors;
}

esp_err_t psram_selftest_run(void)
{
    int64_t t0 = esp_timer_get_time();
    uint8_t *chunks[MAX_CHUNKS];
    size_t sizes[MAX_CHUNKS];
    int n = 0;

    /* Claim the free PSRAM heap in largest-block-first chunks. Holding them
       all before testing guarantees each byte is covered exactly once (a
       free/realloc loop could hand back the same block repeatedly). */
    while (n < MAX_CHUNKS) {
        size_t largest = heap_caps_get_largest_free_block(PSRAM_CAPS);
        if (largest < MIN_CHUNK_BYTES + ALLOC_MARGIN) {
            break;
        }
        size_t size = (largest - ALLOC_MARGIN) & ~(size_t)(CACHE_ALIGN - 1);
        uint8_t *buf = heap_caps_aligned_alloc(CACHE_ALIGN, size, PSRAM_CAPS);
        if (!buf) {
            break;
        }
        chunks[n] = buf;
        sizes[n] = size;
        n++;
    }

    if (n == 0) {
        ESP_LOGW(TAG, "no free PSRAM region to test (PSRAM missing or heap full)");
        return ESP_ERR_NO_MEM;
    }

    /* 峰值快照：此刻 PSRAM 被全部占住，是整个启动过程中堆的最低点。
       /api/v1/status 的 min_free_heap 是 internal+PSRAM 的累加值，若不在这里
       分开打印，事后无法区分"PSRAM 被自检占满"（无害）和"内部 SRAM 见底"（危险）。 */
    ESP_LOGI(TAG, "peak: spiram free=%u internal free=%u (default free=%u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));

    size_t total = 0, errors = 0, fault_logs = 0;
    for (int i = 0; i < n; i++) {
        errors += test_chunk(chunks[i], sizes[i], &fault_logs);
        total += sizes[i];
    }

    for (int i = 0; i < n; i++) {
        heap_caps_free(chunks[i]);
    }

    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    if (errors) {
        ESP_LOGE(TAG, "FAILED: %u error(s) in %u KB (%d chunks, %lld ms)",
                 (unsigned)errors, (unsigned)(total / 1024), n, ms);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OK: %u KB in %d chunks verified, %lld ms",
             (unsigned)(total / 1024), n, ms);
    return ESP_OK;
}
