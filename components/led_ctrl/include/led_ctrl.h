#pragma once

#include "esp_err.h"

typedef enum {
    LED_STATE_BOOT,
    LED_STATE_SOFTAP,
    LED_STATE_CONNECTING,
    LED_STATE_NORMAL,
    LED_STATE_OTA,
    LED_STATE_ERROR,
    LED_STATE_NIGHT,
} led_state_t;

#include <stdint.h>

esp_err_t   led_ctrl_init(void);
/* Factory/per-LED test: light one LED (index 0-3, -1 = all) at r/g/b for
   hold_ms (1..60000), overriding normal state rendering, then auto-resume.
   Returns ESP_ERR_INVALID_ARG on bad index/hold; no-op without WS2812B. */
/* 灯位数量与名字 —— 唯一出处在 led_ctrl.c 的 s_led_names[]。
   GET /api/v1/led/test 用它下发 index→名字 的映射，前端据此画按钮，
   谁都不要再自己写一份。越界返回 NULL。 */
int         led_ctrl_count(void);

/* 回读最后一次写进 strip 的像素（RGB）。返回填了几个。
   语义状态（led_state）和实际像素之间隔着覆盖层、夜间抑制、RMT 就绪三道关，
   没有这个出口就只能靠眼睛判——而眼睛看不见 50ms 一帧的闪烁。 */
int         led_ctrl_get_pixels(uint8_t out[][3], int max_leds);
const char *led_ctrl_name(int index);

/* WS2812 位时序（高电平宽度，ns）。位周期固定 1200ns，低电平 = 1200 - 高电平。
 *
 * 这块板的三极管电平转换会把每个脉冲展宽 250-400ns，所以要**预补偿**：写小一点，
 * 展宽之后才是标称值。T1H 给大了低电平会被吃光，连续的 1 bit 粘成一条长高电平，
 * LED 丢失位时钟 —— 表现为「该字节之后的数据全部作废」，菊花链越靠后越严重。
 *
 * set 只改内存，**重启回 Kconfig 默认值**：现场调参用，调坏了拔电重来。
 * 找到好值后写进 sdkconfig.defaults 固化。 */
esp_err_t   led_ctrl_set_timing(uint16_t t0h_ns, uint16_t t1h_ns);
void        led_ctrl_get_timing(uint16_t *t0h_ns, uint16_t *t1h_ns);

esp_err_t   led_ctrl_probe(uint32_t seconds);
esp_err_t   led_ctrl_sweep(void);
esp_err_t   led_ctrl_test(int index, uint8_t r, uint8_t g, uint8_t b,
                          uint32_t hold_ms);
void        led_ctrl_set_state(led_state_t state);

/* 夜间模式（全局熄灯）。与 led_ctrl_set_state() 正交 —— 事件照常更新底层状态，
   夜间只在渲染时抑制输出。别再用 set_state(LED_STATE_NIGHT)：那样任何网络事件
   都会把它顶掉，而 scheduler 的判定是边沿触发的，不会再设回来（灯亮到天亮）。
   LED_STATE_NIGHT 保留只为对外上报。 */
void        led_ctrl_set_night(bool on);
led_state_t led_ctrl_get_state(void);
/* SYS 灯当前语义的字符串形式，供 /api/v1/status 的 led_state 字段上报。
   有了它 "SYS 灯颜色对不对" 才是可断言的；在此之前只能靠眼睛看，
   于是 s_state 卡在 boot（SYS 黄常亮而其余三颗全绿）藏了很久没被发现。 */
const char *led_ctrl_state_name(led_state_t state);
void        led_ctrl_notify_zigbee_activity(void);
void        led_ctrl_notify_network_activity(void);
void        led_ctrl_set_eth_led(bool on);



/* thread 那颗灯的全部输入。由 thread_br 侧的定时器每 2s 推一次；渲染循环不能
 * 自己去查 OpenThread（要持 OT 锁，会和 mainloop 抢）。
 *   running       —— BR 在不在跑（false → 灯灭）
 *   radio_stalled —— MAC 计数器长时间不动（true → 红闪）
 *   attached      —— role 是 child/router/leader（false → 琥珀慢闪 = 还没入网）
 *
 * ⚠️ 2026-08-15 删掉了 led_ctrl_set_thread_recovery(total, online)。那个判据只
 * 统计网关的**直连**子节点，而网状网里终端设备会挂到离它更近的路由器下面——
 * 实测门窗传感器改挂到另一台 FTD 之后，网关子节点表为空、HA 一切正常，而一条
 * 再也回不来的 restoring 残留条目让灯黄闪了好几分钟；反方向 role=detached 时
 * 灯却是绿的。加回来之前先读 docs/13。 */
void        led_ctrl_set_thread_health(bool running, bool radio_stalled, bool attached);
/* 边界路由是否在跑（otBorderRoutingGetState == RUNNING）。在网但边界路由停了 = HA 连不到
   任何 Thread 设备，灯要琥珀慢闪而不是绿（2026-10-05 拔网线实测，测试 6c）。 */
void        led_ctrl_set_thread_border_routing(bool running);

#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
/* Muse 配对指示（直连型，docs/15 设计 3.5 / 第十一节）：只管 SYS 灯，压过夜间模式（其余三颗照旧受夜间抑制）。
   配对剖面里由 muse_pair 驱动；NONE = 不叠加。 */
typedef enum {
    LED_PAIR_NONE = 0,
    LED_PAIR_WINDOW,     /* 青色呼吸：BLE 窗口开着，等 Muse App */
    LED_PAIR_CONFIRM,    /* 青色快闪：按住按键 2–10 秒后松开 */
    LED_PAIR_SUCCESS,    /* 绿常亮：配好了（随后重启） */
    LED_PAIR_FAILED,     /* 红快闪：没配成（随后重启） */
} led_pair_ind_t;
void        led_ctrl_set_pair_indication(led_pair_ind_t ind);
#endif

#if CONFIG_BITENGINE_ENABLE_ZHUB
/* 直连型 ZIGBEE 灯的输入（CHECKLIST Z54，2026-10-24 用户定）：直连型没有桥（桥的数据面恒为 DISABLED，按 HA 型的判据灯恒灭），
   改看网关自己的 Zigbee 主机 zhub —— 它的状态变了就推一次；设备发来消息时的蓝色短闪照旧走 led_ctrl_notify_zigbee_activity（zhub 调，调用方节流）。
   颜色语义同另三颗灯：绿常亮 = 正常，黄慢闪 = 进行中 / 等操作，红慢闪 = 故障，灭 = 没在跑 */
typedef enum {
    LED_ZB_HUB_OFF = 0,      /* zhub 没起来：灭 */
    LED_ZB_HUB_STARTING,     /* 开机建网中、链路复位中、让出 UART（MG24 升级 / 基准测试）：黄慢闪 */
    LED_ZB_HUB_UP,           /* Zigbee 网络在线：绿常亮（设备发来消息时蓝色短闪） */
    LED_ZB_HUB_JOIN,         /* 入网窗口开着（正在配对新设备）：黄慢闪 */
    LED_ZB_HUB_FAIL,         /* Zigbee 芯片不应答（zhub 连着失败）：红慢闪 */
} led_zb_hub_t;
void        led_ctrl_set_zigbee_hub(led_zb_hub_t st);
#endif
