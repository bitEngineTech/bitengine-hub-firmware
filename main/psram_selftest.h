#pragma once

#include "esp_err.h"

/**
 * Run a synchronous PSRAM self-test over all free PSRAM heap regions.
 *
 * Replaces the boot-time CONFIG_SPIRAM_MEMTEST, which was disabled because it
 * runs before app code and delays the first RS706 WDI feed past the 1.6s
 * hardware watchdog timeout. Call this only AFTER watchdog_init() so that
 * wdt_feed_task (priority 22) keeps the RS706 fed while the test runs.
 *
 * Must run before network_mgr_init(): the test temporarily claims all free
 * PSRAM, and concurrent WiFi/LWIP allocations would fail.
 *
 * @return ESP_OK          all tested bytes verified
 *         ESP_FAIL        one or more mismatches (details in error log)
 *         ESP_ERR_NO_MEM  no PSRAM region could be allocated for testing
 */
esp_err_t psram_selftest_run(void);
