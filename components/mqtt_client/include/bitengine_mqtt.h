#pragma once
#include "esp_err.h"
#include <stdbool.h>

esp_err_t mqtt_client_init(void);
esp_err_t mqtt_client_stop(void);
esp_err_t mqtt_client_restart(void);
bool      mqtt_client_is_connected(void);
void      mqtt_client_publish(const char *topic, const char *payload);
