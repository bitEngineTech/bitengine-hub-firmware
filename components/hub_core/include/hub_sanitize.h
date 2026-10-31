#pragma once
/* hub_sanitize —— 字符串卫生（docs/15_muse_direct_design_v1.md 6.8，防提示注入，架构 5.5）。纯 C，不依赖 ESP-IDF。
 *
 * 来自设备的字符串（ZCL Basic 厂商与型号、Matter VendorName / ProductName / NodeLabel、SRP 主机名与实例名、TXT）
 * 进命令层之前一律过 hub_sanitize_device_str；用户起的名字与房间过 hub_sanitize_user_name。
 *
 * 规则（两者共用，逐个码点）：
 *   - 不合法的 UTF-8：每个坏字节换成 '?'；
 *   - 去掉：C0 控制字符里除空白外的全部、0x7F、C1（U+0080–009F）、Unicode 格式字符（Cf 全类：双向控制、零宽、BOM、
 *     标签字符 U+E0000–E007F…）—— Cf 是 6.8 没列的补充，同一目的：它们能让同一串字节在网页上和在模型眼里长得不一样；
 *     用户名里例外保留 ZWNJ / ZWJ（emoji 组合与波斯文要用）；
 *   - 空白（空格、\t \n \v \f \r、U+2028、U+2029、NBSP 与全角空格等 Zs；U+0085 属于 C1，去掉）当作一个空格，
 *     连续的压成一个，首尾去掉；
 *   - 截到 max 字节，不切开多字节序列，截完再去掉结尾空格。
 * 用户名另外：含 \n \r \v \f、U+2028、U+2029（换行类）直接拒绝；净化后超过 max 字节拒绝（不替用户截断）。 */
#include <stdbool.h>
#include <stddef.h>

#define HUB_DEV_STR_MAX   32   /* 设备字符串截到 32 字节（6.8） */
#define HUB_NAME_MAX      32   /* 用户起的设备名 ≤ 32 字节（6.1、附录 A hub.device.update） */
#define HUB_ROOM_MAX      24   /* 房间 ≤ 24 字节 */

/* 返回写进 out 的字节数（不含 NUL）。out_cap 至少 1；结果最多 min(max, out_cap - 1) 字节。in 可不以 NUL 结尾。 */
size_t hub_sanitize_device_str(const char *in, size_t in_len, char *out, size_t out_cap, size_t max);

typedef enum {
    HUB_NAME_OK = 0,
    HUB_NAME_BAD_ARG,      /* in / out 为 NULL，或 out_cap ≤ max */
    HUB_NAME_NEWLINE,      /* 含换行类字符 */
    HUB_NAME_TOO_LONG,     /* 净化后超过 max 字节 */
} hub_name_verdict_t;

/* out_cap 必须 > max。拒绝时 out 置空串。空名字合法（= 清掉）。 */
hub_name_verdict_t hub_sanitize_user_name(const char *in, char *out, size_t out_cap, size_t max);
const char *hub_name_verdict_str(hub_name_verdict_t v);
