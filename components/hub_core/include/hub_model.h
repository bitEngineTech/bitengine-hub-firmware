#pragma once
/* hub_model —— 统一设备模型里与协议无关的常量（docs/15_muse_direct_design_v1.md 6.1、6.2）。纯 C，不依赖 ESP-IDF。
 * 设备表本身（hub_device_t、PSRAM 里的那张表）随 M3 的 hub.devices.list 一起做；这里先放能力位和它们的对外名字：
 * 动作检查（hub_policy）、列表过滤参数、列表项的 capabilities 数组都用同一份名字。 */
#include <stdint.h>

typedef enum { HUB_PROTO_ZIGBEE = 1, HUB_PROTO_MATTER = 2, HUB_PROTO_THREAD = 3 } hub_proto_t;
/* THREAD = 只有网络层信息的 Thread 成员（不归本网关的 Matter 控制器管） */

typedef enum {                       /* 能力位（6.1）；对外名字见 hub_cap_name */
    HUB_CAP_ONOFF = 1u << 0, HUB_CAP_LEVEL = 1u << 1, HUB_CAP_COLOR_TEMP = 1u << 2, HUB_CAP_COLOR = 1u << 3,
    HUB_CAP_COVER = 1u << 4, HUB_CAP_LOCK = 1u << 5, HUB_CAP_THERMOSTAT = 1u << 6,
    HUB_CAP_TEMPERATURE = 1u << 7, HUB_CAP_HUMIDITY = 1u << 8, HUB_CAP_ILLUMINANCE = 1u << 9,
    HUB_CAP_PRESSURE = 1u << 10, HUB_CAP_CONTACT = 1u << 11, HUB_CAP_MOTION = 1u << 12,
    HUB_CAP_WATER_LEAK = 1u << 13, HUB_CAP_SMOKE = 1u << 14, HUB_CAP_VIBRATION = 1u << 15,
    HUB_CAP_BATTERY = 1u << 16, HUB_CAP_POWER = 1u << 17, HUB_CAP_ENERGY = 1u << 18,
    HUB_CAP_BUTTON = 1u << 19, HUB_CAP_IDENTIFY = 1u << 20,
} hub_cap_t;
#define HUB_CAP_COUNT 21

/* 事件类别（6.7 的表）：决定默认开不开、推送优先级与存活时间（4.9）；去向 2026-10-23 起一律主会话（muse_link.c 的 MUSE_PUSH_MAIN_CHAT） */
typedef enum {
    HUB_EVC_ALARM = 0,      /* 漏水、烟雾、IAS 报警 / 防拆、警报器：默认开、不可按设备关、优先级最高 */
    HUB_EVC_SECURITY,       /* 门磁开合、人体有无：按设备的 notify 开关（默认关） */
    HUB_EVC_AVAILABILITY,   /* 设备离线超过阈值、Thread 成员租约过期 */
    HUB_EVC_LIFECYCLE,      /* 新设备入网、访谈失败、配网完成 / 失败 */
    HUB_EVC_GATEWAY,        /* Zigbee 芯片无响应、Thread BR 掉网、BLE 后 Thread 退化 */
    HUB_EVC_BATTERY,        /* 降到 ≤ 15 %（回到 20 % 以上才重新算） */
    HUB_EVC_COUNT
} hub_evt_class_t;

/* 推送类别开关（6.7、4.4 的 muse/notify）：位 = 1 << hub_evt_class_t。ALARM 恒推、不可关（这一位存了也不看）；其余全部默认开。
   SECURITY 也默认开：6.7 里给 SECURITY 把关的是**每台设备的** notify 开关（默认关，随设备层 M4 / M5），推不推 = 类别位 AND 设备位。
   类别位若默认关，agent 给某个门磁设了 notify=true 照样推不出去（E2E-4；2026-10-22 第二轮审计，趁还没有人存过掩码改的）。 */
#define HUB_NOTIFY_ALL      ((uint16_t)((1u << HUB_EVC_COUNT) - 1u))
#define HUB_NOTIFY_DEFAULT  HUB_NOTIFY_ALL

/* 单个能力位 → 对外名字（"onoff"…）；不是恰好一个已知位返回 NULL */
const char *hub_cap_name(uint32_t cap_bit);
/* 名字 → 能力位；不认识返回 0（大小写敏感：名字是给程序看的） */
uint32_t hub_cap_from_name(const char *name);
