#pragma once
/* zh_lumi —— 小米 / Aqara 设备的读数（社区设备库导出的表 zh_lumi_db；docs/15 设计 8.8 注记"社区设备库"，M5 第十步）。纯逻辑，不依赖 ESP-IDF。
 *
 * 小米的私有数据有两处，都是一串 TLV：[标签 1 B][ZCL 类型 1 B][值，小端，长度按类型]：
 *  - Basic 簇属性 0xFF01，类型 0x42（字符串，[长度][TLV…]）。zigbee-herdsman 只对"属性号 0xFF01 + 字符串类型"按小米结构读
 *    （dist/zspec/zcl/definition/foundation.js:45 → buffaloZcl.js:642 readMiStruct，读到帧尾，末尾多一个字节就跳过）；
 *  - Aqara 私有簇 0xFCC0（厂商码 0x115F）属性 0x00F7，类型 0x41（字节串）—— zhc lumi.js numericAttributes2Payload 的 case "247" 交给
 *    buffer2DataObject 按同样的 TLV 解。0xFCC0 的其它属性按属性号查表（cl = 0xFCC0）。
 * 值的长度按 zh_zcl_type_len；能取整数的类型（布尔、位图 8–32、无符号 8–32、有符号 8–32、枚举 8 / 16）进表，别的（40 位以上、浮点、字符串）跳过。
 * 电量：表给曲线就从电池电压算电量 %（zhc lib/utils.js batteryVoltageToPercentage：线性 min / max / vOffset 钳位后四舍五入；3V_2100 分段；
 * 3V_1500_2800 = 235 − 370000 / (mV + 1)）。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zh_lumi_db.h"
#include "zh_tbl.h"

#define ZH_CL_LUMI               0xFCC0u /* zhc lumi.js manuSpecificLumi（Aqara 私有簇） */
#define ZH_ATTR_LUMI_FF01        0xFF01u /* Basic 簇的小米结构（foundation.js:45） */
#define ZH_ATTR_LUMI_F7          0x00F7u /* 0xFCC0 簇的 TLV（numericAttributes2Payload case "247"） */

typedef struct {
    const uint8_t *p;
    size_t         n, off;
} zh_lumi_tlv_t;

void     zh_lumi_tlv_init(zh_lumi_tlv_t *it, const uint8_t *p, size_t n);
/* 下一条：1 = 拿到（*numeric 说值能不能取整数，能的在 *val）；0 = 完了（剩下不到 2 字节也算完：herdsman 跳过末尾那个字节）；
   -1 = 坏了（类型不认识 / 值超出剩余），停 */
int      zh_lumi_tlv_next(zh_lumi_tlv_t *it, uint8_t *tag, int32_t *val, bool *numeric);
/* 按 ZCL 类型把值取成整数（同上那几种类型）；不能的回 false */
bool     zh_lumi_val_int(uint8_t type, const uint8_t *v, size_t len, int32_t *out);

/* 查表：型号串（以 0 结尾）→ 设备下标，-1 = 不在表里 */
int      zh_lumi_find(const char *model);
uint32_t zh_lumi_fields(int dev);
uint16_t zh_lumi_occ_s(int dev);
/* 这台有没有改写这个标准簇（有就整个簇不按通用解析定能力 —— 门磁的 OnOff 是开合、不是开关） */
bool     zh_lumi_has_cl(int dev, uint16_t cl);
/* (cl, id) 的值 raw → 读数，最多 max 条，返回条数；*dropped = 表说这一路丢掉（返回 0） */
unsigned zh_lumi_map(int dev, uint16_t cl, uint16_t id, int32_t raw, zh_tbl_val_t *out, unsigned max, bool *dropped);
/* 电池电压（毫伏）→ 电量 %；这台没有曲线回 -1 */
int      zh_lumi_batt_pct(int dev, int32_t mv);
/* 曲线本身（ZH_LDB_BATT_*；min / max / off 只给 LINEAR 用）；不认识的种类回 -1 */
int      zh_lumi_batt_curve(uint8_t type, int32_t min, int32_t max, int32_t off, int32_t mv);
