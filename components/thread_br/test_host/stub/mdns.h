#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct { const char *key; const char *value; } mdns_txt_item_t;

/* mdns 桩：只需可编译。真正的 bye/重新宣告时序无法在主机上验证
   （取决于 mdns 组件内部 PCB 状态机），那是在板用例。 */
static inline bool mdns_service_exists_with_instance(const char *i, const char *t,
                                                     const char *p, const char *h)
{ (void)i; (void)t; (void)p; (void)h; return false; }
static inline esp_err_t mdns_service_remove_for_host(const char *i, const char *t,
                                                     const char *p, const char *h)
{ (void)i; (void)t; (void)p; (void)h; return ESP_OK; }
static inline esp_err_t mdns_service_add_for_host(const char *i, const char *t, const char *p,
                                                  const char *h, uint16_t port,
                                                  mdns_txt_item_t txt[], size_t n)
{ (void)i; (void)t; (void)p; (void)h; (void)port; (void)txt; (void)n; return ESP_OK; }
static inline esp_err_t mdns_service_subtype_add_for_host(const char *i, const char *t,
                                                          const char *p, const char *h,
                                                          const char *sub)
{ (void)i; (void)t; (void)p; (void)h; (void)sub; return ESP_OK; }
