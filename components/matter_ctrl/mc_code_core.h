/* mc_code_core —— 配网码的预处理与待配网记录的比对（docs/15 设计 9.4"配网码解析"；M7 第三步）。纯逻辑，test_host/ 原样编。
 * 真正的解码（Verhoeff 校验、QR 的 Base38、字段拆分）交给 CHIP 的 ManualSetupPayloadParser / QRCodeSetupPayloadParser；
 * 这里只做 CHIP 不管的两件事：把用户 / Muse 写的码规整成 CHIP 认的形状，和本机代播的 _matterc 记录的 D 值比对 discriminator。 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MC_CODE_MAX  128               /* 规整后的上限（QR 载荷一般 22 字符，多个设备串在一起也远低于这个） */

typedef enum {
    MC_CODE_MANUAL = 1,                /* 11 或 21 位数字 */
    MC_CODE_QR     = 2,               /* "MT:" 开头 */
} mc_code_kind_t;

/* 规整：去掉首尾空白；手动码去掉中间的空格与连字符（App 常显示成 "3497-011-2332"）；QR 原样（大小写敏感，Base38 有大写字母）。
   成功返回种类、out 是以 NUL 结尾的规整结果；不认识的形状返回 0（字符不对、位数不是 11 / 21、QR 里有空白、太长）。 */
int mc_code_normalize(const char *in, char *out, size_t cap);

/* 本机代播的 _matterc 记录的 TXT D（十进制长 discriminator，0..4095）是否与配网码里的对得上：
   长 discriminator（QR / 21 位码）要相等；短的（11 位码：长值的高 4 位）比高 4 位。D 解析不了 → false */
bool mc_code_disc_match(bool is_short, uint16_t disc, const char *txt_d);

#ifdef __cplusplus
}
#endif
