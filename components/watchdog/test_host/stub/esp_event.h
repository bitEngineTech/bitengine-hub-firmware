#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
typedef const char *esp_event_base_t;
#define ESP_EVENT_DECLARE_BASE(x) extern esp_event_base_t x
#define ESP_EVENT_DEFINE_BASE(x)  esp_event_base_t x = #x
extern int g_event_count;
static inline esp_err_t esp_event_post(esp_event_base_t b, int32_t id, void *d, size_t s, uint32_t t)
{ (void)b; (void)id; (void)d; (void)s; (void)t; g_event_count++; return ESP_OK; }
