#pragma once
#include "esp_err.h"
#include "esp_event.h"
#include <stdint.h>
#include <stdbool.h>

esp_err_t button_init(void);

/* 按键唯一的事件（2026-09-14，T1.2 步骤 3）：2-10 s 按住后松开 —— 那一档本身
 * 什么都不做（保护带），现在会向默认事件循环发这一条，payload 是 uint32_t
 * held_ms。用途只有一个：给"用户已经在别处发起、需要人在设备旁边确认"的事情
 * 当物理确认（ota_mgr 的未签名 NCP 固件开发者解锁）。没人发起时按下去仍然
 * 什么都不发生 —— 事件本身不是动作，docs/07 §2.2 "只有两个动作"仍成立。
 * 其他档位不发事件：短按重启、>10 s 恢复出厂都不返回，也不该被订阅。 */
ESP_EVENT_DECLARE_BASE(BUTTON_EVENT);
enum {
    BUTTON_EVENT_HOLD_CONFIRM = 1,
};

/* 按键留痕，给产测用。
 *
 * 为什么需要：按键是整机唯一一个**没有任何可观测性**的硬件输入。它的四个动作
 * 里，短按会重启、>10s 会擦掉配置，产线都不能碰；唯一安全的是 2-10 秒那一档
 * （落在保护带里，不触发任何动作）。但在此之前那一按不留任何痕迹，于是"按键
 * 焊没焊好、去抖对不对"在产线上根本测不了。
 *
 * 现在每次完整按压（去抖后按下 → 去抖后松开）都记一笔，产测流程是：
 *   1 读一次 press_count
 *   2 让操作员按住约 5 秒后松开
 *   3 再读：press_count 必须 +1，last_duration_ms 应在 2000-10000 之间，
 *     last_action 必须是 "none"（证明保护带也是对的）
 * 整条链路——GPIO、上拉、去抖、计时、分档——一次按压全部覆盖。 */
typedef struct {
    uint32_t press_count;        /* 开机以来完整按压次数 */
    uint32_t last_duration_ms;   /* 最后一次按了多久；无按压时为 0 */
    const char *last_action;     /* "none" / "restart" / "factory_reset" / "stuck"（直连型配对剖面里短按另有 "ignored"） */
    bool     pressed_now;        /* 此刻是否正被按住（产线夹具可轮询） */
} button_stats_t;

void button_get_stats(button_stats_t *out);

#include "sdkconfig.h"
#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
/* 直连型（Muse 配对剖面）：打开后短按（< 2 s）不重启，只记 last_action = "ignored"；2–10 s 的确认事件与 >10 s 恢复出厂照旧 */
void button_set_restart_inhibit(bool on);
bool button_restart_inhibited(void);
#endif
