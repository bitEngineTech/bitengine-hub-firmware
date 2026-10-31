#pragma once
/* hub_audit —— 审计环的入口（docs/15_muse_direct_design_v1.md 6.10；只在直连型里编）。环本身见 hub_audit_core.h。
 * Muse 的每次调用（muse_link 的 dispatch，含 4 个管理命令；回了错的畸形调用、busy 也记）和本地 REST 的 /api/v1/hub/invoke、/hub/devices
 * 都记一条；本地改 Muse 设置、解绑、App 里删设备也记（hub_audit_note）。读取 GET /api/v1/muse/audit。相同的连续调用合并（hub_audit_core.h）。
 * 被拒的（权限、限速）另打 W 级日志进 log_store（管理员在日志页能看到），每类每分钟最多一行、带压下的次数。
 * 线程：任何任务都能调（锁只护 memcpy，100 ms 拿不到就丢这条）；不能在 ISR / esp_timer 回调里调。 */
#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "hub_audit_core.h"
#include "hub_json.h"

esp_err_t hub_audit_init(void);   /* PSRAM 环（sizeof(hau_ring_t) 约 22 KB）+ 锁；幂等 */
/* 记一条：参数里只取 id 与 action（不记别的，6.10"不记参数里的秘密"）；result = "ok" 或错误码 */
void hub_audit_record(hau_src_t src, const char *command, const cJSON *params, const char *result, uint32_t dur_ms);
/* 不是命令调用的操作（本地改设置、解绑、App 里删设备）：what 如 "muse.config"，detail 写变了什么（进 action 列）；result 同上 */
void hub_audit_note(hau_src_t src, const char *what, const char *detail, const char *result);
bool hub_audit_ready(void);       /* init 过了吗（没有时 /muse/audit 回 audit_unavailable，别说成"忙"） */
/* GET /api/v1/muse/audit 的结果体；没 init 或锁忙返回 false、什么都不写 */
bool hub_audit_write(hj_t *w, uint32_t limit);
