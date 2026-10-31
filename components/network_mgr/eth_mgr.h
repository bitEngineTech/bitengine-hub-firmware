#pragma once

#include "esp_err.h"
#include "esp_netif.h"
#include <stdbool.h>

esp_err_t    eth_mgr_init(void);
bool         eth_mgr_link_is_up(void);
esp_netif_t *eth_mgr_get_netif(void);
