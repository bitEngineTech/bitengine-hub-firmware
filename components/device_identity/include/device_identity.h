#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 出厂设备身份 —— 只读，来自 `fctry` NVS 分区。
 *
 * ── 这是什么，不是什么 ──────────────────────────────────────────────
 *
 * 产线在烧完固件之后，用 tools/generate_nvs.py 生成一个 16KB 的 NVS 镜像写进
 * `fctry` 分区（0x6b0000）。里面只有三个字符串：
 *
 *     serial_number   贴在机壳标签和二维码上的那个号，售后与保修按它查
 *     hw_revision     板子版本，如 AM1 / AM2
 *     mfg_date        YYYY-MM-DD
 *
 * ⚠️ 键名短是被迫的：NVS 的键名上限是 15 字符，docs/10 §6.2 写的
 *    hardware_revision(17) / manufacture_date(16) 都超限，那份 CSV 根本
 *    生成不出来。改键名要两边一起改（tools/generate_nvs.py），只改一边的
 *    表现是"分区在、能打开、一个键都读不到"。
 *
 * **没有 device_secret。** docs/10 §6.2 原设计里有一个 32 字节密钥用于云端
 * OTA 的 HMAC 认证，2026-09-09 决策不做：云端 OTA 靠固件自带的签名验证，
 * 谁下载都一样，密钥没有消费者。一个没人读的密钥落在 Flash 里是纯负债 ——
 * 开源要脱敏、日志要脱敏、配置导出要脱敏，每一处漏掉都是事故。
 *
 * ── ⚠️ 只读，一个字节都不写 ─────────────────────────────────────────
 *
 * 本模块只有 nvs_get_*，没有 nvs_set_*、没有 nvs_erase_*、没有
 * nvs_flash_erase_partition()，打开分区时用的是 NVS_READONLY。
 * 理由：擦掉 fctry = 这台设备**永久**失去身份，而且外部零症状 —— 一切功能
 * 照常，直到几个月后售后要查 SN 才发现，那时已经无从恢复（产线的那份记录
 * 如果也没留，就真没了）。VERIFY.sh 里有一组反向不变量专门盯这件事。
 *
 * 恢复出厂**不影响**身份：那只擦 `nvs` 分区里我们自己的命名空间，fctry 是
 * 另一个分区，天然不在范围内。
 *
 * ── ⚠️ 缺身份必须能正常启动 ─────────────────────────────────────────
 *
 * 产线顺序是「先烧固件 → 跑自检 → 再写身份」，所以固件**必然**会遇到没有
 * 身份的板子；开发板则永远没有。因此：
 *
 *   · device_identity_init() 在分区不存在、未格式化、键缺失时一律返回
 *     ESP_OK，只是把状态记成"无身份"。它不是致命错误。
 *   · 三个取值函数任何时候都返回**可直接 printf 的空串**，绝不返回 NULL。
 *     调用方不必判空 —— 判空这种事总有一处会忘。
 *   · device_identity_valid() 才是"这台设备有合法身份吗"的唯一判据。
 *
 * 唯一会因此变红的地方是产线自检的 identity 项：没有合法 SN 就报 fail。
 * 那正是它的用途 —— 拦住"忘了写身份就出货"，这是产线上唯一能拦的时刻。
 *
 * ── ⚠️ 与 mDNS 的 serial_number 同名但无关 ──────────────────────────
 *
 * mdns_svc 广播的 TXT 里有一个 `serial_number`，值是 **MAC**。那是 ZHA 用来
 * 认设备的键（缺了 ZHA 直接 async_abort(invalid_zeroconf_data)），和这里的
 * 产品 SN 是两回事。**别把它改成产品 SN** —— 那会换掉 ZHA 认设备的标识，
 * 已配好的 ZHA 条目会认不出这台网关。HA 集成的 unique_id 同理，仍用 MAC。
 */

/* 三个字段各自的缓冲区大小（含结尾 NUL）。调用方要拷贝时按它开数组，
   别自己写一个数 —— 那种数字总有一天会和这里对不上。 */
#define DEVICE_ID_MAX   32

/* 读一次 fctry 并缓存。重复调用是空操作。
 * 永远返回 ESP_OK —— 没有身份不是错误，见上文。 */
esp_err_t device_identity_init(void);

/* 这台设备是否有一个**格式合法**的序列号。
 *
 * 合法 = 非空、长度 ≤ 31、只含可打印 ASCII 且无空白、形如
 * `<段>-<段>[-<段>…]`（段由字母数字组成，至少两段）。
 * 刻意**不校验前缀**：前缀属于产品命名，由 tools/generate_nvs.py 决定，
 * 哪天改了不该让已出货设备的固件把自己判成非法。固件只管形状与字符集，
 * 那足以拦住空值、截断、乱码和写飞。 */
bool device_identity_valid(void);

/* 三个取值。未写入 / 未初始化时返回 ""（不是 NULL）。 */
const char *device_identity_serial(void);
const char *device_identity_hw_revision(void);
const char *device_identity_mfg_date(void);

#ifdef __cplusplus
}
#endif
