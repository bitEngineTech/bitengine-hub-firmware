#pragma once
#include <stdint.h>
/* 可控时钟：主机上时间随便跳，3600 秒的自愈窗口不用真跑一小时。 */
extern int64_t g_fake_us;
static inline int64_t esp_timer_get_time(void) { return g_fake_us; }
