#pragma once
/* 探针测试用的最小桩。测试里 network_mgr_get_status 恒失败，probe_network 直接返回、不参与断言。 */
#include <stdbool.h>
#include "esp_err.h"

typedef enum { NET_IF_NONE = 0, NET_IF_ETH, NET_IF_WIFI } net_if_t;
typedef struct {
    net_if_t active_interface;
    bool     eth_has_ip;
    bool     wifi_has_ip;
    bool     softap_active;
} network_status_t;

esp_err_t network_mgr_get_status(network_status_t *out);
