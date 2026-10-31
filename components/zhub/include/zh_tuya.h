#pragma once
/* zh_tuya —— 涂鸦私有簇 0xEF00 的读数（docs/15_muse_direct_design_v1.md 8.8 注记"社区设备库"；M5 第八步）。纯逻辑，不依赖 ESP-IDF：test_host/ 原样编它。
 *
 * 涂鸦的 TS0601 这类设备不用标准 ZCL 簇报读数，而是把"数据点"（dp）塞进私有簇 0xEF00 的簇专用命令。哪个 dp 是什么、怎么换算，
 * 每个型号都不一样 —— 这张表不自己写，从社区设备库 zigbee-herdsman-converters 导出（zh_tuya_db.c，见 tools/zhc_export/export_tuya.js）。
 *
 * 线上格式（zigbee-herdsman 11.1.1：dist/zspec/zcl/definition/cluster.js:7149-7280 命令表，dist/zspec/zcl/buffaloZcl.js:598-615 数据点列表）：
 *   簇专用命令、服务端 → 客户端：dataResponse 0x01 / dataReport 0x02 / activeStatusReportAlt 0x05 / activeStatusReport 0x06，
 *   负载 = [seq u16（ZCL 小端）] 之后若干条 [dp u8][类型 u8][长度 u16 大端][数据]；最后一条不完整就丢掉它、前面的照收（herdsman 也是 break）。
 *   客户端 → 服务端的 dataQuery 0x03（无负载）让设备把所有数据点报一遍。
 * 类型（zigbee-herdsman-converters 26.123.0：dist/lib/tuya.js:87-93）：raw 0、bool 1、number 2、string 3、enum 4、bitmap 5。
 * 解成整数照 dist/lib/tuya.js:167-213（getDataValue / convertBufferToNumber）：bool = (data[0] == 1)；enum = data[0]；
 *   number / bitmap = 大端逐字节拼（JS 的 << 是 32 位有符号：正好 4 字节时按 int32 补码，不足 4 字节就是无符号）；raw / string 不解。
 *
 * 换算（表的每一行，见 zh_tuya_db.h）：LIN 目标值 = 原始值 × mul / 10000 + off（四舍五入）；LIN16 先把 0x8000–0xFFFF 当 16 位补码（老设备把负温度
 * 装在 4 字节数值的低 16 位）；MASK 原始值 0–7 查激活位、> 7 不激活。
 *
 * 写数据点（M5 第九步）：簇专用、客户端 → 服务端的 dataRequest 0x00 / sendData 0x04，负载格式同上（[seq u16 小端] + 数据点）；
 * 数据点按类型编（zhc 26.123.0 dist/lib/tuya.js:261-269 dpValueFrom*）：number 4 字节大端（int32 补码）、bool / enum 1 字节。库发写命令时
 * 不要默认应答（tuya.js:253-260 sendDataPoints 的 disableDefaultResponse: true），seq 是它自己的 16 位计数。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zh_tuya_db.h"

#define ZH_CL_TUYA               0xEF00u /* cluster.js:7151 manuSpecificTuya */
#define ZH_TUYA_CMD_DATA_QUERY   0x03u   /* cluster.js:7164 dataQuery（客户端 → 服务端，无负载） */
#define ZH_TUYA_CMD_DATA_REQUEST 0x00u   /* cluster.js:7157 dataRequest（客户端 → 服务端：写数据点，负载同读数的那几条命令） */
#define ZH_TUYA_CMD_SEND_DATA    0x04u   /* cluster.js:7168 sendData（同上；定义的 meta.tuyaSendCommand 选这个的设备用它） */
#define ZH_TUYA_CMD_DATA_RSP     0x01u   /* cluster.js commandsResponse.dataResponse */
#define ZH_TUYA_CMD_DATA_REPORT  0x02u   /* cluster.js commandsResponse.dataReport */
#define ZH_TUYA_CMD_STATUS_ALT   0x05u   /* cluster.js commandsResponse.activeStatusReportAlt */
#define ZH_TUYA_CMD_STATUS       0x06u   /* cluster.js commandsResponse.activeStatusReport */
enum { ZH_TUYA_T_RAW = 0, ZH_TUYA_T_BOOL = 1, ZH_TUYA_T_NUMBER = 2, ZH_TUYA_T_STRING = 3, ZH_TUYA_T_ENUM = 4, ZH_TUYA_T_BITMAP = 5 };

typedef struct {
    uint8_t        dp, type;
    uint16_t       len;
    const uint8_t *data;
} zh_tuya_dp_t;

typedef struct {
    const uint8_t *p;
    size_t         n, off;
} zh_tuya_iter_t;

typedef zh_tbl_val_t zh_tuya_val_t;      /* 字段 + 值（zh_tbl.h） */

/* 带读数的四条命令之一 */
bool     zh_tuya_is_data_cmd(uint8_t cmd);
/* 负载（ZCL 头之后）开始迭代；不到 2 字节（连 seq 都没有）返回 false */
bool     zh_tuya_iter_init(zh_tuya_iter_t *it, const uint8_t *pl, size_t n);
/* 下一条数据点：1 = 拿到；0 = 正好完；-1 = 最后一条不完整（丢弃，前面的照收） */
int      zh_tuya_next(zh_tuya_iter_t *it, zh_tuya_dp_t *out);
/* 数据点解成整数；raw / string / 长度不对（bool、enum 要 ≥ 1 字节，number、bitmap 1–4 字节）返回 false */
bool     zh_tuya_dp_int(const zh_tuya_dp_t *dp, int32_t *out);

/* 查表：设备下标，-1 = 不在表里。mfr / model 是访谈读回的 Basic 厂商名 / 型号（以 0 结尾）；先找"厂商名 + 型号"，再找"厂商名 + 任意型号"，再找"只按型号" */
int      zh_tuya_find(const char *mfr, const char *model);
/* 这台设备有哪些字段（第 ZH_TF_* 位）；下标非法回 0 */
uint32_t zh_tuya_fields(int dev);
/* 主报警字段 / 对应的 ZoneType：zh_tbl_alarm_field / zh_tbl_zone_type（zh_tbl.h，和小米表共用） */
/* 设备 dev 的数据点 dp（原始值 raw）→ 读数，最多 max 条，返回条数（不在表里的 dp 回 0） */
unsigned zh_tuya_map(int dev, uint8_t dp, int32_t raw, zh_tuya_val_t *out, unsigned max);

/* ---- 写数据点（M5 第九步） ---- */
enum { ZH_TUYA_ACT_ON = 0, ZH_TUYA_ACT_OFF, ZH_TUYA_ACT_LEVEL };
#define ZH_TUYA_CTL_PL_MAX       22u     /* 写命令负载最长：seq 2 + 两条数据点各 (4 + 4) + 余量 */
/* 能写的字段：第 ZH_TF_ONOFF / ZH_TF_LEVEL 位；下标非法 / 不能控制回 0 */
uint32_t zh_tuya_ctl_fields(int dev);
/* 编一条写命令的负载（ZCL 头之后：[tseq 小端][数据点…]），返回长度；*cmd = 命令号，*confirm_dp / *confirm_raw = 确认看设备回报哪个 dp、值要是多少
 * （只看 dp 不看值会被别的回报顶掉：招呼的 dataQuery 让它全量回报、上一条命令带出的旧亮度 —— 审计 ⚠️）。
 * ON / OFF 写开关 dp；LEVEL：pct 0 = 关（要有开关 dp），pct 1–100 = 有开关 dp 时同一帧先开、再设亮度（同 ZCL 的 MoveToLevelWithOnOff：
 * 关着的灯设亮度也要亮），没有开关 dp 就只设亮度。不支持 / pct > 100 / 写不下回 0 */
size_t   zh_tuya_enc_ctl(int dev, unsigned act, uint8_t pct, uint16_t tseq, uint8_t *out, size_t cap, uint8_t *cmd, uint8_t *confirm_dp, int32_t *confirm_raw);
