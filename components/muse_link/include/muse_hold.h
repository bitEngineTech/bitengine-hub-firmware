/* muse_hold —— 安全模式里"退出并先关掉 Muse"的一次性标记（docs/15 设计附录 E33，2026-10-23 用户定 B）
 *
 * 场景：反复重启的正是 Muse（例如 T9.47 那种 TLS 握手 panic），一小时三次后进安全模式；安全模式下 hub_store 不起，
 * /muse/config 关不了 Muse，一退出安全模式 Muse 照开、接着崩。用户在安全模式的网页上点"退出并先关掉 Muse"，
 * 网关只在主 nvs 记一条留言然后退出安全模式；下次正常开机在 muse_link_init() 之前把 Muse 关掉。
 * 实现与理由见 muse_hold.c 头注。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* 记下留言（主 nvs，当场 commit）。安全模式下可调：不碰 hub_store。失败如实返回，调用方别再退出安全模式 */
esp_err_t muse_hold_request(void);

/* 有没有还没消费的留言（任何读错都当没有）：/muse/status 显示用，muse_hold_clear() 也靠它避免无谓的写 */
bool muse_hold_pending(void);

/* 正常开机、hub_store 起来之后、muse_link_init() 之前调。没有留言或已按留言把 Muse 关掉 → ESP_OK；
   有留言却关不掉 / 读不出有没有留言 → 错误，调用方这次开机不要起 muse_link（留言保留，下次再试） */
esp_err_t muse_hold_apply(void);

/* 用户在 Muse 页明确打开了 Muse：作废还没消费的留言（用户最新的选择为准，审计 ⚠️）。没有留言时不写 flash */
void muse_hold_clear(void);
