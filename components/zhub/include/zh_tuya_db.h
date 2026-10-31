#pragma once
/* zh_tuya_db —— 涂鸦设备的数据点表（社区设备库导出；docs/15 设计 8.8 注记"社区设备库"）。纯数据 + 纯逻辑，不依赖 ESP-IDF：test_host/ 原样编它。
 *
 * zh_tuya_db.c 由 tools/zhc_export/export_tuya.js 从 zigbee-herdsman-converters（MIT，Copyright (c) 2018 Koen Kanters）生成，别手改；
 * 查找与换算在 zh_tuya.c。设备按访谈读回的 Basic 厂商名（_TZE200_xxx 这类）+ 型号（TS0601 这类）查；表里型号为空的条目匹配任何型号。
 *
 * 每条数据点：dp 号 → 字段（ZH_TF_*）+ 换算：
 *  - ZH_TDB_LIN：目标值 = 原始值 × mul / 10000 + off（四舍五入；目标单位：温度 / 湿度 0.01、照度 lux、电量 %、亮度 %）；
 *  - ZH_TDB_LIN16：同上，但原始值 0x8000–0xFFFF 先当 16 位补码（老式设备把负温度装在 4 字节数值的低 16 位）；
 *  - ZH_TDB_MASK：原始值 0–7 里第 i 位为 1 = "激活"（开着 / 有人 / 门开 / 漏水 / 有烟 / 防拆 / 电量低……），> 7 算不激活。 */
#include <stdbool.h>
#include <stdint.h>

#include "zh_tbl.h"   /* 字段编号 ZH_TF_*、换算种类 ZH_TDB_*（和小米表共用） */

#define ZH_TUYA_DP_FIELDS_MAX  4u        /* 同一台设备同一个 dp 最多几行（导出脚本超了就拒绝生成；zh_dev 按这个开数组） */

typedef struct {
    uint8_t dp;
    uint8_t field;      /* ZH_TF_* */
    uint8_t kind;       /* ZH_TDB_LIN / ZH_TDB_MASK / ZH_TDB_LIN16 */
    uint8_t mask;       /* MASK：原始值 0–7 的激活位 */
    int32_t mul;        /* LIN / LIN16：× mul / 10000 */
    int32_t off;        /* LIN / LIN16：再加 off（目标单位） */
} zh_tdb_row_t;

typedef struct {
    uint16_t row;       /* 这台设备的第一条数据点在 zh_tdb_rows 里的下标 */
    uint8_t  n_rows;
    uint32_t fields;    /* 第 ZH_TF_* 位 = 有这个字段（hub_zcap 据此推能力） */
    uint16_t ctl;       /* 写方向在 zh_tdb_ctls 里的下标；0xFFFF = 不能控制（M5 第九步） */
} zh_tdb_dev_t;

/* 写方向（M5 第九步）：导出脚本喂库的 toZigbee、截获它发出的 0xEF00 命令推出来的。只导"同一个 dp 也读得回来"的（确认看设备回报这个 dp）。
 * 字段顺序就是导出脚本写初始化的顺序（VERIFY 钉着） */
typedef struct {
    uint8_t cmd;        /* 命令号：ZH_TUYA_CMD_DATA_REQUEST / ZH_TUYA_CMD_SEND_DATA（zh_tuya.h） */
    uint8_t onoff_dp;   /* 开关的 dp；0 = 没有 */
    uint8_t onoff_type; /* ZH_TUYA_T_BOOL / ENUM / NUMBER */
    int32_t on_raw, off_raw;
    uint8_t level_dp;   /* 亮度的 dp（类型恒为 number）；0 = 没有 */
    int32_t level_mul, level_off;   /* 原始值 = 百分比 × mul / 10000 + off（四舍五入），再封在 [lo, hi] */
    int32_t level_lo, level_hi;
} zh_tdb_ctl_t;

typedef struct {
    uint16_t mfr;       /* 厂商名在 zh_tdb_str 里的偏移 */
    uint16_t model;     /* 型号的偏移（空串 = 任何型号） */
    uint16_t dev;       /* zh_tdb_devs 的下标 */
} zh_tdb_fp_t;

extern const char         zh_tdb_source[];
extern const zh_tdb_row_t zh_tdb_rows[];
extern const zh_tdb_dev_t zh_tdb_devs[];
extern const zh_tdb_fp_t  zh_tdb_fps[];
extern const zh_tdb_ctl_t zh_tdb_ctls[];
extern const char         zh_tdb_str[];
extern const unsigned     zh_tdb_n_rows, zh_tdb_n_devs, zh_tdb_n_fps, zh_tdb_str_len, zh_tdb_n_ctls;
