#pragma once
#include "esp_err.h"

typedef enum {
    WEBHOOK_EVENT_ZIGBEE_CRASH,
    WEBHOOK_EVENT_NETWORK_DOWN,
    WEBHOOK_EVENT_NETWORK_FAILOVER,
    WEBHOOK_EVENT_TEMP_HIGH,
    WEBHOOK_EVENT_MEMORY_LOW,
    WEBHOOK_EVENT_CONFIG_BACKUP,
    WEBHOOK_EVENT_INTERFERENCE,
    WEBHOOK_EVENT_OTA_AVAILABLE,
    WEBHOOK_EVENT_OTA_COMPLETE,
    WEBHOOK_EVENT_NETWORK_RECOVERED,   /* 以太网从 failover 中回来（T2.16）；与 network_down 同一开关 */
} webhook_event_t;

esp_err_t webhook_init(void);
void      webhook_post_event(webhook_event_t event, const char *data_json);
