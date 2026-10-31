#pragma once

#include <stdint.h>
#include "esp_err.h"

/**
 * Zigbee-to-Matter Bridge
 *
 * Maps Zigbee devices (via MG24 EZSP) to Matter endpoints,
 * enabling Apple Home / Google Home / Alexa to control Zigbee devices.
 *
 * IMPORTANT: Matter Bridge mode is MUTUALLY EXCLUSIVE with Z2M Bridge mode.
 * Only one can use the UART/EZSP session at a time. User selects mode in WebUI.
 *
 * Status: Framework scaffold. Full ESP-Matter SDK integration pending
 * memory/compatibility verification on ESP32-C5.
 */

esp_err_t matter_bridge_init(void);
esp_err_t matter_bridge_start(void);
esp_err_t matter_bridge_stop(void);
esp_err_t matter_bridge_add_device(uint16_t zigbee_short_addr,
                                    uint8_t endpoint,
                                    const char *device_type);
esp_err_t matter_bridge_remove_device(uint16_t zigbee_short_addr);
