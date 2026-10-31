/* components/config_mgr/include/radio_region.h — 射频地区：网关侧的**菜单**（不是表）。
 *
 * 设计正本 docs/14_mg24_tx_power_xncp_design_v1.md 第 7.5 节。
 *
 * 地区 → 功率等级的**表**只在 MG24 固件里（mg24/ncp/tx_power_table.c），C5 不复制它：
 * C5 只把用户选的国家码原样发给 NCP，等级由 NCP 判。C5 上这份只是设置页下拉框能选的
 * 几个码，以及 Thread 那颗射频（在 C5 上）按等级取功率时用的等级归属。
 *
 * 等级编号与 NCP 的 be_txp_class_t 相同（XNCP 0x7000 应答的 class 字段就是这个数）。
 *
 * ⚠️ setter 只收菜单里的码（radio_region_code_valid）：这样 C5 与 NCP 对同一个码的等级
 * 判定一定一致 —— 收任意国家码的话，Thread 会把 "DE" 当 UNKNOWN 而 NCP 当 CE。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RADIO_REGION_CLASS_NONE    = 0,   /* 未设置 */
    RADIO_REGION_CLASS_UNKNOWN = 1,   /* 菜单"其他地区"（ZZ）*/
    RADIO_REGION_CLASS_CE      = 2,   /* 欧盟 / 英国 / 瑞士等 ETSI 地区（EU）*/
    RADIO_REGION_CLASS_FCC     = 3,   /* 美国 / 加拿大（US / CA）*/
} radio_region_class_t;

typedef struct {
    char                 code[3];   /* ISO 3166-1 alpha-2 大写；EU 是发给 NCP 的别名，ZZ 是用户自定义码 */
    radio_region_class_t cls;
} radio_region_menu_t;

#define RADIO_REGION_CODE_LEN 2

/* 菜单：EU / US / CA / ZZ。count 可为 NULL。 */
const radio_region_menu_t *radio_region_menu(size_t *count);

/* "" 或菜单里的码（大小写不敏感）→ true；其它一律 false。NULL → false。 */
bool radio_region_code_valid(const char *cc);

/* "" 或 NULL → NONE；菜单里的码 → 其等级；其它 → UNKNOWN。 */
radio_region_class_t radio_region_class_of(const char *cc);

/* 就地把 ASCII 小写转大写；NULL 安全。 */
void radio_region_normalize(char *cc);

#ifdef __cplusplus
}
#endif
