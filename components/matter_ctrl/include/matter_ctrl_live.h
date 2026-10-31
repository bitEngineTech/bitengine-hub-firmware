/* matter_ctrl_live —— Matter 设备的订阅结果：在线与读数（docs/15 设计 9.5；M7 第五步）。
 * 订阅在 CHIP 任务里（mc_sub.cpp）；别的任务经 matter_ctrl_node_live 加锁拷一份。读数的键名与 Zigbee 那一套一致（hub_core 写 JSON）。 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 读数字段（state 对象里的一个键；at_ms 下标） */
enum {
    MC_ST_ONOFF = 0,   /* onoff：OnOff 0x0006 / 0x0000 */
    MC_ST_LEVEL,       /* level_pct：LevelControl 0x0008 / 0x0000 CurrentLevel（1–254 → %） */
    MC_ST_CONTACT,     /* contact："open" / "closed"：BooleanState 0x0045 / 0x0000（Contact Sensor：true = 合上） */
    MC_ST_LEAK,        /* water_leak：BooleanState（Water Leak Detector：true = 漏水） */
    MC_ST_SMOKE,       /* smoke：SmokeCoAlarm 0x005C / 0x0001 SmokeState（≠ 0 = 有烟） */
    MC_ST_MOTION,      /* motion：OccupancySensing 0x0406 / 0x0000（bit0） */
    MC_ST_BATTERY,     /* battery_pct：PowerSource 0x002F / 0x000C BatPercentRemaining（单位 0.5 %） */
    MC_ST_TEMP,        /* temperature_c：0x0402 / 0x0000（0.01 °C） */
    MC_ST_HUM,         /* humidity_pct：0x0405 / 0x0000（0.01 %） */
    MC_ST_PRESS,       /* pressure_hpa：0x0403 / 0x0000（0.1 kPa = 1 hPa；存 0.1 hPa） */
    MC_ST_LUX,         /* illuminance_lux：0x0400 / 0x0000（10000·log10(lux) + 1） */
    MC_ST_POWER,       /* power_w：ElectricalPowerMeasurement 0x0090 / 0x0008 ActivePower（mW） */
    MC_ST_ENERGY,      /* energy_kwh：ElectricalEnergyMeasurement 0x0091 / 0x0001 CumulativeEnergyImported.energy（mWh） */
    MC_ST_COUNT
};

typedef struct mc_state_s {
    uint32_t have;                 /* 1u << MC_ST_*：这次开机收到过（null 值清掉该位） */
    uint8_t  onoff, level_pct, contact_open, leak, smoke, motion, battery_pct;
    uint8_t  batt_low;             /* 电量低已报、还没回到 MC_BATT_REARM_PCT（只在内存） */
    int16_t  temp_c100;
    uint16_t hum_c100;
    int16_t  press_hpa10;
    uint32_t lux;
    int64_t  power_mw;
    int64_t  energy_mwh;
    int64_t  at_ms[MC_ST_COUNT];   /* 收到的时刻（esp_timer 毫秒） */
} mc_state_t;

/* 读数翻转的事件位（MATTER_CTRL_EV_STATE 的 bits；hub_core 按 6.7 出事件）。同 Zigbee：没收到过的读数按"正常"（合上 / 不漏 / 无烟 / 无人 / 电量满）算，
   所以订阅首报就是报警态的照样出事件（停电期间漏了水，网关回来首报 water_leak=true 要推）；首报是正常态的不出 */
#define MC_SEV_CONTACT    0x01u        /* 门磁开 / 合变了（值看 st.contact_open） */
#define MC_SEV_MOTION_ON  0x02u        /* 人体 0 → 1 */
#define MC_SEV_LEAK       0x04u        /* 漏水 / 解除 */
#define MC_SEV_SMOKE      0x08u        /* 有烟 / 解除 */
#define MC_SEV_BATT_LOW   0x10u        /* 电量降到 ≤ 15 %（回到 20 % 以上才重新算） */

/* 控制的动作（matter_ctrl_control；M7 第六步） */
enum { MATTER_CTRL_ACT_ON = 1, MATTER_CTRL_ACT_OFF, MATTER_CTRL_ACT_TOGGLE, MATTER_CTRL_ACT_LEVEL };

enum { MC_SUB_NONE = 0, MC_SUB_SETUP, MC_SUB_UP, MC_SUB_WAIT };   /* 没订 / 正在建 / 建好了 / 断了等重订 */

typedef struct mc_live_s {
    mc_state_t st;
    int8_t     online;             /* 1 在线 / 0 离线 / -1 还不知道（这次开机还没建好过订阅、也没到判离线的时候） */
    uint8_t    sub;                /* MC_SUB_* */
    uint16_t   max_iv_s;           /* 设备答应的最大上报间隔（建好之前是我们要的上限） */
    int64_t    last_ms;            /* 最后一次听到它（报告 / 订阅建好）；0 = 这次开机没听到过 */
    uint32_t   resubs;             /* 这次开机重订了几次（诊断） */
} mc_live_t;

/* 拷一份（任何任务）。没有这台 / 订阅表没起来返回 false */
bool matter_ctrl_node_live(uint64_t node, mc_live_t *out);

#ifdef __cplusplus
}
#endif
