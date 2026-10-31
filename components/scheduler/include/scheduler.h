#pragma once
#include "esp_err.h"

esp_err_t scheduler_init(void);

/* 当前是否落在夜间时间窗内（即 LED 是否正被夜间模式压着）。
   night_mode_enabled=true 只表示功能打开，不代表现在就该熄灯 —— REST 层要靠
   这个值告诉用户"开关是开的，但现在不在时段内，所以灯还亮着"，否则用户会当
   成没生效。SNTP 未同步时恒为 false（scheduler 此时根本不判时间窗）。 */
bool scheduler_night_active(void);
