#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t uart_bridge_init(void);
int       uart_bridge_read(uint8_t *buf, int max_len, int timeout_ms);
int       uart_bridge_write(const uint8_t *buf, int len, int timeout_ms);
esp_err_t uart_bridge_get_rx_buffered(size_t *out_len);

/**
 * Change the line rate at runtime.
 *
 * Only for the MG24 OTA path: the Gecko Bootloader's XMODEM menu runs at its
 * own compile-time baud (CONFIG_BITENGINE_MG24_BOOTLOADER_BAUD), which may
 * differ from EZSP_UART_BAUD. Waits for the TX FIFO to drain before switching
 * so the last byte sent at the old rate is not truncated mid-frame, then
 * discards RX — bytes clocked in at the previous rate are garbage.
 *
 * The caller must own the UART (bridge in BRIDGE_STATE_OTA); there is no
 * internal lock, and switching under a live ASH session corrupts it.
 */
esp_err_t uart_bridge_set_baud(int baud);

/** Current line rate, for restore-what-was-there logic. */
int uart_bridge_get_baud(void);

/** Drop everything buffered in the RX path. */
void uart_bridge_flush_input(void);

/** Block until the TX FIFO has drained or @p timeout_ms elapses. */
esp_err_t uart_bridge_wait_tx_done(int timeout_ms);

/* 线路电平采样：声明在 ezsp_bridge.h（公开头），rest_api 的诊断端点要用。 */
