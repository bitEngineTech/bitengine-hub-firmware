#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t ble_proxy_init(void);
esp_err_t ble_proxy_stop(void);
bool      ble_proxy_is_running(void);
