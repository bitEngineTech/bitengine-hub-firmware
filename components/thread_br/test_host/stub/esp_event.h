#pragma once
/* config_mgr.h 只用到事件基的声明宏；主机测试不跑事件循环。 */
typedef void *esp_event_base_t;
#define ESP_EVENT_DECLARE_BASE(id) extern esp_event_base_t id
