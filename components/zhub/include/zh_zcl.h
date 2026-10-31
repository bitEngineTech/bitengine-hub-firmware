#pragma once
/* zh_zcl —— ZCL 帧的最小编解码，纯逻辑（docs/15_muse_direct_design_v1.md 8.7；M4 第七步 b 访谈只用到读属性）。不依赖 ESP-IDF：test_host/ 原样编它。
 * 空口来的帧一律按恶意输入解析（CHECKLIST Z27、设计 14.1）：每读一个字节前先核剩余长度；不认识 / 不支持的数据类型没法知道它多长，
 * 解析停在那里、本帧剩下的丢掉（调用方计数）。不分配内存：解析结果里的指针都指向调用方给的帧。
 *
 * H1（写代码前答清楚）：
 * 1. 帧头 = [frame control][厂商码 2 B，小端，只在厂商专用位置位时有][序号][命令号]（af.h:2140-2151 各位、:2161-2162 头长 3 / 5；zcl-util.c:50-57 厂商码位置）。
 * 2. 读属性请求的载荷 = [属性号 2 B 小端] × N（general.xml:30 ReadAttributes 的参数是数组；process-global-message.c:179-180）。
 *    应答 = ([属性号 2 B][状态 1 B]，状态 0 时再接 [类型 1 B][值]) × N（process-global-message.c:541）。应答方装不下时写 [属性号][0x89] 然后不再往下处理
 *    （attribute-table.c:587-590、628-630），所以请求的属性在应答里可能缺。
 * 3. 字符串：char / octet string 首字节是长度，0xFF = 无效（没有值）；long 版本首 2 B 是长度，0xFFFF = 无效（util/message.c:215-219）。
 *    其余定长类型的长度见 SDK app/zcl/types.xml 的类型表（:19-75）：zh_zcl_type_len() 按它，test_host/zh_zcl_test 逐个类型对着 types.xml 核。
 *    array / struct / set / bag（0x48 / 0x4C / 0x50 / 0x51）是复合类型，访谈用不到，当"不支持"（解析停）。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 帧控制（af.h） */
#define ZH_ZCL_FC_TYPE_MASK     0x03u   /* af.h:2140 ZCL_FRAME_CONTROL_FRAME_TYPE_MASK */
#define ZH_ZCL_FC_CLUSTER       0x01u   /* af.h:2141 ZCL_CLUSTER_SPECIFIC_COMMAND（0 = 全局命令） */
#define ZH_ZCL_FC_MFG           0x04u   /* af.h:2145 ZCL_MANUFACTURER_SPECIFIC_MASK */
#define ZH_ZCL_FC_S2C           0x08u   /* af.h:2148 ZCL_FRAME_CONTROL_SERVER_TO_CLIENT */
#define ZH_ZCL_FC_DIS_DR        0x10u   /* af.h:2151 ZCL_DISABLE_DEFAULT_RESPONSE_MASK */

/* 全局命令（general.xml） */
#define ZH_ZCL_CMD_READ_ATTR     0x00u  /* general.xml:30 ReadAttributes */
#define ZH_ZCL_CMD_READ_ATTR_RSP 0x01u  /* general.xml:36 ReadAttributesResponse */
#define ZH_ZCL_CMD_DEFAULT_RSP   0x0Bu  /* general.xml:96 DefaultResponse：[命令号][状态] */
#define ZH_ZCL_CMD_REPORT_ATTR   0x0Au  /* general.xml:90 ReportAttributes：([属性号 2 B][类型 1 B][值]) × N（没有状态字节） */
#define ZH_ZCL_CMD_WRITE_ATTR    0x02u  /* general.xml:42 WriteAttributes：([属性号 2 B][类型 1 B][值]) × N */
#define ZH_ZCL_CMD_CFG_REPORT    0x06u  /* general.xml:66 ConfigureReporting：([方向][属性号 2 B][类型][最小间隔 2 B][最大间隔 2 B][变化量：只给模拟量类型]) × N */
#define ZH_ZCL_CMD_CFG_REPORT_RSP 0x07u /* general.xml:72 ConfigureReportingResponse：全成 = 一个 SUCCESS 字节；否则只列失败的 ([状态][方向][属性号 2 B])
                                           （SDK 的应答方 reporting.c:531-549 同） */
#define ZH_ZCL_CMD_WRITE_ATTR_RSP 0x04u /* general.xml:54 WriteAttributesResponse：全成 = 一个 SUCCESS 字节；否则只列失败的 ([状态][属性号 2 B]) × n
                                           （SDK 的应答方 process-global-message.c:340-343 同） */

/* 状态（types.xml） */
#define ZH_ZCL_ST_SUCCESS        0x00u  /* types.xml:215 SUCCESS */
#define ZH_ZCL_ST_UNSUP_ATTR     0x86u  /* types.xml:229 UNSUPPORTED_ATTRIBUTE：设备没有这个属性（不是错，不用再问） */
#define ZH_ZCL_ST_INSUF_SPACE    0x89u  /* types.xml:232 INSUFFICIENT_SPACE：应答装不下，后面的没处理（拆开一个一个再问） */

/* 数据类型（types.xml） */
#define ZH_ZCL_T_BOOL            0x10u  /* types.xml:28 boolean */
#define ZH_ZCL_T_BITMAP8         0x18u  /* types.xml:29 bitmap8 */
#define ZH_ZCL_T_BITMAP16        0x19u  /* types.xml:30 bitmap16 */
#define ZH_ZCL_T_UINT8           0x20u  /* types.xml:37 int8u */
#define ZH_ZCL_T_UINT16          0x21u  /* types.xml:38 int16u */
#define ZH_ZCL_T_INT16           0x29u  /* types.xml:46 int16s */
#define ZH_ZCL_T_ENUM8           0x30u  /* types.xml:53 enum8 */
#define ZH_ZCL_T_ENUM16          0x31u  /* types.xml:54 enum16 */
#define ZH_ZCL_T_CHAR_STR        0x42u  /* types.xml:60 char_string */
#define ZH_ZCL_T_EUI64           0xF0u  /* types.xml:73 ieee_address（8 B，小端：与 EUI64 数组同序） */

/* 簇与属性 */
#define ZH_HA_PROFILE            0x0104u /* ha-devices.xml:23 Home Automation（zhub.c 静态断言它等于端点 1 的 ZH_HOST_EP_PROFILE） */
#define ZH_CL_BASIC              0x0000u /* general.xml:174 Basic */
#define ZH_CL_IAS_ZONE           0x0500u /* ha.xml:1134 IAS Zone */
#define ZH_BASIC_MANUF           0x0004u /* general.xml:184 MANUFACTURER_NAME（CHAR_STRING 32） */
#define ZH_BASIC_MODEL           0x0005u /* general.xml:185 MODEL_IDENTIFIER（CHAR_STRING 32） */
#define ZH_BASIC_POWER           0x0007u /* general.xml:187 POWER_SOURCE（ENUM8） */
#define ZH_BASIC_SW_BUILD        0x4000u /* zll.xml:132 SW_BUILD_ID（CHAR_STRING 16；ZLL 给 Basic 的扩展，不是 general.xml 本身的属性） */
#define ZH_IAS_ZONE_TYPE         0x0001u /* ha.xml:1140 ZONE_TYPE（ENUM16） */

/* 状态用到的簇与属性（M5 第一步：只读状态）。行号指向 SDK XML 里的定义（簇号 / 属性号 / 类型）；单位、无效值与照度公式 XML 里没有，
   出自 ZCL 规范（与 zigpy / herdsman 的换算一致）。读数只收 XML 规定的那一个类型：别的宽度一律不收（第一轮审计 ❌：int32 气压 ×10 有符号溢出） */
#define ZH_CL_POWER_CFG          0x0001u /* general.xml:208 Power Configuration */
#define ZH_CL_ONOFF              0x0006u /* general.xml:535 On/off */
#define ZH_CL_LEVEL              0x0008u /* general.xml:573 Level Control */
#define ZH_CL_ILLUMINANCE        0x0400u /* ha.xml:462 Illuminance Measurement */
#define ZH_CL_TEMPERATURE        0x0402u /* ha.xml:495 Temperature Measurement */
#define ZH_CL_PRESSURE           0x0403u /* ha.xml:513 Pressure Measurement */
#define ZH_CL_HUMIDITY           0x0405u /* ha.xml:554 Relative Humidity Measurement */
#define ZH_CL_OCCUPANCY          0x0406u /* ha.xml:572 Occupancy Sensing */
#define ZH_ATTR_BATTERY_MV100    0x0020u /* general.xml:219 BATTERY_VOLTAGE（INT8U，单位 100 mV） */
#define ZH_ATTR_BATTERY_PCT2     0x0021u /* general.xml:220 BATTERY_PERCENTAGE_REMAINING（INT8U，单位 0.5 %；0xFF 无效） */
#define ZH_ATTR_ON_OFF           0x0000u /* general.xml:540 ON_OFF（BOOLEAN） */
#define ZH_ATTR_CURRENT_LEVEL    0x0000u /* general.xml:578 CURRENT_LEVEL（INT8U，0–254） */
#define ZH_ATTR_ILLUM_MEASURED   0x0000u /* ha.xml:467 ILLUM_MEASURED_VALUE（INT16U：10000·log10(lux)+1；0 = 太暗量不出，0xFFFF 无效） */
#define ZH_ATTR_TEMP_MEASURED    0x0000u /* ha.xml:500 TEMP_MEASURED_VALUE（INT16S，0.01 °C；0x8000 无效） */
#define ZH_ATTR_PRESSURE_MEASURED 0x0000u /* ha.xml:518 PRESSURE_MEASURED_VALUE（INT16S，0.1 kPa = 1 hPa；0x8000 无效） */
#define ZH_ATTR_HUMIDITY_MEASURED 0x0000u /* ha.xml:559 RELATIVE_HUMIDITY_MEASURED_VALUE（INT16U，0.01 %；0xFFFF 无效） */
#define ZH_ATTR_OCCUPANCY        0x0000u /* ha.xml:577 OCCUPANCY（BITMAP8，位 0 = 有人） */
#define ZH_IAS_ZONE_STATUS       0x0002u /* ha.xml:1141 ZONE_STATUS（BITMAP16） */
#define ZH_IAS_CIE_ADDR          0x0010u /* ha.xml:1142 IAS_CIE_ADDRESS（IEEE_ADDRESS、可写）：设备只认这个地址发来的登记应答、只往它报 Zone Status */
#define ZH_IAS_ZONE_STATE        0x0000u /* ha.xml:1139 ZONE_STATE（ENUM8）：登记上了没有 */

/* 控制（设计 8.11；M5 第七步）：簇专用命令，客户端 → 服务端 */
#define ZH_CL_CMD_OFF            0x00u   /* general.xml:542 Off（On/off 簇） */
#define ZH_CL_CMD_ON             0x01u   /* general.xml:547 On */
#define ZH_CL_CMD_TOGGLE         0x02u   /* general.xml:552 Toggle */
#define ZH_CL_CMD_MOVE_TO_LEVEL_ONOFF 0x04u /* general.xml:627 MoveToLevelWithOnOff（Level 簇）：[亮度 0–254][渐变时间 2 B，0.1 s]；亮度 0 = 关 */
#define ZH_IAS_STATE_ENROLLED    0x01u   /* types.xml:450 enrolled（IasZoneState） */
/* IAS Zone 的簇专用命令：服务端 → 客户端的两条、客户端 → 服务端的登记应答 */
#define ZH_IAS_CMD_STATUS_CHANGE 0x00u   /* ha.xml:1165 ZoneStatusChangeNotification：[状态 2 B][扩展状态][zone id][延时 2 B] */
#define ZH_IAS_CMD_ENROLL_REQ    0x01u   /* ha.xml:1174 ZoneEnrollRequest：[ZoneType 2 B][厂商码 2 B] */
#define ZH_IAS_CMD_ENROLL_RSP    0x00u   /* ha.xml:1146 ZoneEnrollResponse：[应答码][zone id] */
#define ZH_IAS_ENROLL_SUCCESS    0x00u   /* types.xml:472 success（IasEnrollResponseCode） */
/* ZoneStatus 的位（types.xml:188-199 IasZoneStatus） */
#define ZH_IAS_ST_ALARM1         0x0001u /* types.xml:189 alarm1：这个区的主报警（人体有人 / 门开 / 漏水 / 烟…，看 ZoneType） */
#define ZH_IAS_ST_TAMPER         0x0004u /* types.xml:191 tamper：防拆 */
#define ZH_IAS_ST_BATTERY        0x0008u /* types.xml:192 battery：电量低 */
/* ZoneType 取值（types.xml:452-469 IasZoneType）。⚠️ 设计 8.6 写的"0x002B CO"不对：0x002B 是燃气，CO 是 0x0227 */
#define ZH_ZONE_MOTION           0x000Du /* types.xml:454 motionSensor */
#define ZH_ZONE_CONTACT          0x0015u /* types.xml:455 contactSwitch */
#define ZH_ZONE_FIRE             0x0028u /* types.xml:456 fireSensor */
#define ZH_ZONE_WATER            0x002Au /* types.xml:457 waterSensor */
#define ZH_ZONE_GAS              0x002Bu /* types.xml:458 gasSensor */
#define ZH_ZONE_VIBRATION        0x002Du /* types.xml:460 vibrationMovementSensor */
#define ZH_ZONE_CO               0x0227u /* types.xml:466 carbonMonoxideSensor */

enum {
    ZH_ZCL_OK = 0,
    ZH_ZCL_E_SHORT = -1,     /* 帧比头 / 字段短 */
    ZH_ZCL_E_TYPE = -2,      /* 不认识 / 不支持的数据类型：不知道多长，本帧剩下的丢掉 */
    ZH_ZCL_E_ARG = -3,
};

typedef struct {
    uint8_t        fc;
    uint16_t       mfg;          /* 厂商码（没有为 0） */
    uint8_t        seq, cmd;
    const uint8_t *pl;           /* 命令载荷（指向调用方的帧） */
    size_t         n;
} zh_zcl_frame_t;

typedef struct {
    uint16_t       id;
    uint8_t        status;
    uint8_t        type;         /* status != 0 时为 0 */
    const uint8_t *val;          /* 值（字符串含长度前缀）；status != 0 时 NULL */
    size_t         len;          /* 值占的字节数（含长度前缀） */
} zh_zcl_attr_t;

typedef struct {
    const uint8_t *p;
    size_t         n;
} zh_zcl_iter_t;

/* 解析帧头。<0 = 太短 */
int zh_zcl_parse(const uint8_t *b, size_t n, zh_zcl_frame_t *f);
/* 读属性请求（全局命令、客户端 → 服务端、不要默认应答：读属性本来就有应答；出错时设备照样回默认应答）。写不下回 0 */
size_t zh_zcl_enc_read_attrs(uint8_t *out, size_t cap, uint8_t seq, const uint16_t *ids, size_t n_ids);
/* 写一个 EUI64 属性（全局命令、客户端 → 服务端，帧控制 0x00）：[fc][seq][0x02][属性号 2 B][0xF0][EUI64 8 B，小端]，共 14 B。
   照 SDK ias-zone-client.c:395-404 的 setCieAddress（WriteAttributes 在 general.xml 里没有 disableDefaultResponse，SDK 发出的帧控制就是 0x00；
   写属性本来就有写属性应答，不会另回默认应答 —— 第一轮审计）。写不下 / 参数为 NULL 回 0 */
size_t zh_zcl_enc_write_eui64(uint8_t *out, size_t cap, uint8_t seq, uint16_t attr, const uint8_t eui[8]);
/* 配一条属性上报（全局命令、客户端 → 服务端、帧控制 0x00，同 SDK：ConfigureReporting 没有 disableDefaultResponse）：
   [fc][seq][0x06][方向 0 = 设备上报][属性号 2 B][类型][最小间隔 2 B][最大间隔 2 B][变化量 chg_len B，小端]。chg_len 0 = 离散类型不带变化量
   （SDK 应答方按类型判模拟 / 离散再读，reporting.c:461-473）；chg_len > 4 回 0。写不下 / out 为 NULL 回 0 */
size_t zh_zcl_enc_cfg_report(uint8_t *out, size_t cap, uint8_t seq, uint16_t attr, uint8_t type, uint16_t min_s, uint16_t max_s, uint32_t change, uint8_t chg_len);
/* 一个值占多少字节（含字符串的长度前缀）。avail = 剩余字节；<0 = 不认识的类型（ZH_ZCL_E_TYPE）或超出剩余（ZH_ZCL_E_SHORT） */
int zh_zcl_type_len(uint8_t type, const uint8_t *v, size_t avail, size_t *len);
/* 逐条取读属性应答的记录。1 = 取到一条，0 = 读完了，<0 = 坏了（停，剩下的丢掉） */
void zh_zcl_iter_init(zh_zcl_iter_t *it, const zh_zcl_frame_t *f);
int  zh_zcl_next_rsp_rec(zh_zcl_iter_t *it, zh_zcl_attr_t *a);
/* 逐条取属性上报的记录（[属性号][类型][值]，没有状态；取出来的 status 恒为 0）。返回值同上 */
int  zh_zcl_next_report_rec(zh_zcl_iter_t *it, zh_zcl_attr_t *a);
/* 有符号整数取值（int8 / int16 / int24 / int32，符号扩展）。类型不对回 false */
bool zh_zcl_attr_int(const zh_zcl_attr_t *a, int32_t *v);
/* char_string 的内容（不含长度前缀）。类型不是 char_string、长度 0xFF（无效）回 false */
bool zh_zcl_attr_str(const zh_zcl_attr_t *a, const uint8_t **s, size_t *len);
/* enum8 / enum16 / 无符号整数取值（最多 4 B）。类型不对回 false */
bool zh_zcl_attr_uint(const zh_zcl_attr_t *a, uint32_t *v);
