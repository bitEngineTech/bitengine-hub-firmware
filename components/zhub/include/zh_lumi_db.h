#pragma once
/* zh_lumi_db —— 小米 / Aqara（"lumi"）设备表（社区设备库导出；docs/15 设计 8.8 注记"社区设备库"，M5 第十步）。纯数据，test_host/ 原样编它。
 *
 * zh_lumi_db.c 由 tools/zhc_export/export_lumi.js 从 zigbee-herdsman-converters（MIT，Copyright (c) 2018 Koen Kanters）生成，别手改（tools/zhc_export/regen.sh）；
 * 查找与换算在 zh_lumi.c。按型号串查（访谈或主动上报读回的 Basic 0x0005，如 lumi.weather），厂家名不看（小米报 LUMI）。
 * 字段编号与换算种类同涂鸦表（zh_tbl.h）。 */
#include <stdint.h>

#include "zh_tbl.h"

#define ZH_LDB_CL_TLV          0xFFFFu  /* 行的 cl：TLV 标签（Basic 0xFF01 与 0xFCC0 的 0x00F7 里那串）；别的值 = 簇号（0xFCC0 的属性、标准簇的改写） */
enum { ZH_LDB_BATT_NONE = 0, ZH_LDB_BATT_LINEAR = 1, ZH_LDB_BATT_3V_2100 = 2, ZH_LDB_BATT_3V_1500_2800 = 3 };

/* 一行：(cl, id) 这一路的值 → 字段 + 换算。字段 ZH_TF_NONE = 这一路丢掉（库对它什么都不出，如灯的 Occupancy 上报）。字段顺序 = 导出脚本写的顺序 */
typedef struct {
    uint16_t cl;
    uint16_t id;
    uint8_t  field;     /* ZH_TF_*；NONE = 丢掉 */
    uint8_t  kind;      /* ZH_TDB_LIN / MASK / LIN16 */
    uint8_t  mask;
    int32_t  mul, off;
} zh_ldb_row_t;

typedef struct {
    uint16_t row;       /* 第一行在 zh_ldb_rows 里的下标 */
    uint8_t  n_rows;
    uint32_t fields;    /* 第 ZH_TF_* 位 = 这台有这个字段（含标准簇照通用解析的那些、IAS 报警；hub_zcap 定能力用） */
    uint8_t  batt;      /* 电量曲线 ZH_LDB_BATT_*（电量 % 由电池电压算，同 zhc utils.batteryVoltageToPercentage） */
    int16_t  batt_min, batt_max, batt_off;   /* LINEAR：min / max / vOffset（毫伏） */
    uint16_t occ_s;     /* 人体只报"有人"的：这么多秒没再报就当没人（z2m occupancy_timeout 的默认）；0 = 不用 */
} zh_ldb_dev_t;

typedef struct {
    uint16_t model;     /* 型号串在 zh_ldb_str 里的偏移 */
    uint16_t dev;
} zh_ldb_fp_t;

extern const char         zh_ldb_source[];
extern const zh_ldb_row_t zh_ldb_rows[];
extern const zh_ldb_dev_t zh_ldb_devs[];
extern const zh_ldb_fp_t  zh_ldb_fps[];
extern const char         zh_ldb_str[];
extern const unsigned     zh_ldb_n_rows, zh_ldb_n_devs, zh_ldb_n_fps, zh_ldb_str_len;
