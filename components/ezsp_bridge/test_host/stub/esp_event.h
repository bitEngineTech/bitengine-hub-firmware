#pragma once
/* 只为让 ezsp_bridge.h 在主机上编得过；本测试不涉及事件。 */
#define ESP_EVENT_DECLARE_BASE(x) extern int x
#define ESP_EVENT_DEFINE_BASE(x)  int x
typedef int esp_event_base_t;
