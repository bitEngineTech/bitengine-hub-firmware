#pragma once
/* zh_tbl —— 社区设备库导出的表（涂鸦 zh_tuya_db、小米 zh_lumi_db）共用的字段编号、换算种类与换算（docs/15 设计 8.8 注记"社区设备库"）。
 * 纯逻辑，不依赖 ESP-IDF：test_host/ 原样编它。
 *
 * 字段（ZH_TF_*）的顺序就是导出脚本 tools/zhc_export/zhc_sim.js 里 FIELDS 的顺序（改了两边一起改；zhub/VERIFY.sh 对一遍）。目标单位：
 * 温度 / 湿度 0.01、照度 lux、电量 %、亮度 %、电池电压 mV、气压 0.1 hPa；开关 / 报警类是 0 / 1。
 * 换算：ZH_TDB_LIN 目标值 = 原始值 × mul / 10000 + off（四舍五入）；ZH_TDB_LIN16 同上但原始值 0x8000–0xFFFF 先当 16 位补码；
 * ZH_TDB_MASK 原始值 0–7 里第 i 位为 1 = 激活，> 7 算不激活；ZH_TDB_ANY 不管值多少都是 1。 */
#include <stdbool.h>
#include <stdint.h>

enum {
    ZH_TF_NONE = 0,
    ZH_TF_ONOFF, ZH_TF_LEVEL, ZH_TF_BATTERY_PCT, ZH_TF_BATTERY_LOW,
    ZH_TF_TEMP, ZH_TF_HUMIDITY, ZH_TF_LUX, ZH_TF_OCCUPANCY,
    ZH_TF_CONTACT, ZH_TF_WATER, ZH_TF_SMOKE, ZH_TF_GAS, ZH_TF_CO, ZH_TF_TAMPER, ZH_TF_VIBRATION,
    ZH_TF_VOLTAGE, ZH_TF_PRESSURE,
    ZH_TF_COUNT
};
enum { ZH_TDB_LIN = 1, ZH_TDB_MASK = 2, ZH_TDB_LIN16 = 3, ZH_TDB_ANY = 4 };   /* ANY：来一条就算激活（值不看；小米表用：新式 Aqara 人体的 0xFCC0 illuminance 每报一次就是有人） */

typedef struct {
    uint8_t field;                       /* ZH_TF_* */
    int32_t val;                         /* LIN：目标单位；MASK：0 / 1 */
} zh_tbl_val_t;

/* 一行的换算；kind 不认识回 false */
bool     zh_tbl_conv(uint8_t kind, uint8_t mask, int32_t mul, int32_t off, int32_t raw, int32_t *out);
/* 主报警字段（烟雾 > CO > 燃气 > 漏水 > 门磁 > 振动，取第一个有的）：只有它进 IAS alarm1，别的报警字段不理（二合一不把 CO 报成烟）。没有回 ZH_TF_NONE */
unsigned zh_tbl_alarm_field(uint32_t fields);
/* 主报警字段对应的 IAS ZoneType（zh_zcl.h 的 ZH_ZONE_*）；没有回 0xFFFF */
uint16_t zh_tbl_zone_type(uint32_t fields);
