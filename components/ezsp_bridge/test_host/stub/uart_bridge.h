#pragma once
#include <stdint.h>
#include "esp_err.h"
int  uart_bridge_read(uint8_t *buf, int max_len, int timeout_ms);
int  uart_bridge_write(const uint8_t *buf, int len, int timeout_ms);
void uart_bridge_flush_input(void);
