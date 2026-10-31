/* 主机侧桩：wd_recovery.c 在重启前调 config_mgr_flush()（2026-10-08，防抖中的配置先落盘）。
   这里的测试不关心配置，恒返回 ESP_OK。 */
#pragma once
#include <stdint.h>
#include "esp_err.h"
static inline esp_err_t config_mgr_flush(uint32_t timeout_ms) { (void)timeout_ms; return ESP_OK; }
