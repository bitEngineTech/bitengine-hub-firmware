#pragma once
#include "sdk_plat.h"
#define SL_LEGACY_SERIAL_FIFO   1
#define SL_LEGACY_SERIAL1_MODE  SL_LEGACY_SERIAL_FIFO
sl_status_t sli_legacy_serial_write_byte(uint8_t port, uint8_t b);
sl_status_t sli_legacy_serial_read_byte(uint8_t port, uint8_t *b);
uint16_t    sli_legacy_serial_read_available(uint8_t port);
