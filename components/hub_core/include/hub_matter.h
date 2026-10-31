/* hub_matter —— hub_core 这一侧的 Matter 控制器接线（hub_matter.c）。只在 CONFIG_BITENGINE_ENABLE_MATTER_CTRL 下有内容。 */
#pragma once
#include "sdkconfig.h"
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "hub_call.h"
/* 给 matter_ctrl 登记身份 / 节点表的存取钩子（hub_store）。hub_svc_start 里调，main.c 里 hub_svc 在 matter_ctrl_start 之前 */
void hub_matter_init(void);
/* hub.matter.commission 的处理函数（附录 A；hub_cmds.c 的表绑定）：开作业、交给 matter_ctrl，call->job = 作业 id */
int hub_matter_cmd_commission(hub_call_t *call);
/* 列表 / 详情（附录 A 的列表项；7.3：本网关 fabric 上的 Matter 设备按 mt- 呈现，Thread 信息从清单里按 cfid + 节点号找来挂上）。
   raw / l 可为 NULL（清单这会儿拿不到：不挂 Thread 信息）。返回 HUB_MT_*；start / next / n_out 是过滤后的序号（同 hub_zigbee_write_list） */
enum { HUB_MT_DONE = 0, HUB_MT_TRUNCATED, HUB_MT_NOT_FOUND, HUB_MT_BUSY };
#include "hub_json.h"
#include "hub_ti_core.h"
int hub_matter_write_list(hj_t *w, unsigned long start, unsigned long *next, unsigned long *n_out, uint32_t cap_filter, const char *room,
                          const ti_raw_t *raw, const ti_list_t *l);
int hub_matter_write_device(hj_t *w, const char *id, const ti_raw_t *raw, const ti_list_t *l);
/* hub.device.control 用（M7 第六步）：节点号、能力、控制路径驱动得了的能力（开关 / 亮度）、敏感标记。返回 HUB_MT_* */
int hub_matter_ctl_info(const char *id, uint64_t *node, uint32_t *caps, uint32_t *ctl_caps, bool *sensitive);
/* 控制之后回读：在 w 的当前位置写 "state" 键与对象（同 hub_zigbee_write_state） */
int hub_matter_write_state(hj_t *w, const char *id);
/* 配好了的 Matter 设备有几台（device.health 的 total；同列表的口径）；PSRAM 分配失败 -1 */
int hub_matter_count(void);
/* Thread 清单里哪些成员已经按 mt- 列了（7.3）：实例属于本网关 fabric、**且**节点号是节点表里配好了的。只看 fabric 不够（审计）——
   强制移除的设备没收到 RemoveFabric 还按本 fabric 注册着、节点表没读回、配到一半，这些按 th- 照列，否则整台从列表里消失。
   own_get 拷一份快照（PSRAM；控制器没在跑 / 分配失败返回 NULL = 一个都不跳过）；own_has 命中时 mt_out（可 NULL）写它的 mt- id */
typedef struct hub_mt_own_s hub_mt_own_t;
hub_mt_own_t *hub_matter_own_get(void);
bool hub_matter_own_has(const hub_mt_own_t *o, const ti_raw_t *raw, const ti_entry_t *e, char mt_out[20]);
void hub_matter_own_free(hub_mt_own_t *o);
/* "允许测试设备"（9.7、10.1；只在本地，handler 记审计）：开 = 落盘读回确认后再生效；关 = 先生效再存（安全的一边）。
   ESP_ERR_NOT_FINISHED = 存上了、CD 测试密钥那一半要重启后才跟上（CHIP 任务没排上） */
esp_err_t hub_matter_set_allow_test(bool on);
#define HUB_MATTER_OPTS_KEY "opts"      /* hubdb mctrl 里存它的键（恢复出厂全清在 hub_store.c 单独删它） */
/* 重置 Matter 控制器（10.1；只在 ident_lost 时受理，否则 ESP_ERR_INVALID_STATE）。成功后调用方负责重启；why 写不受理 / 失败的原因 */
esp_err_t hub_matter_reset(char *why, size_t why_len);
/* 本地移除一台 Matter 设备（设计 10.1 /api/v1/hub/device/remove，只在本地）：开作业，*job_out = 作业 id。
   不受理时 why 写原因：ESP_ERR_INVALID_ARG（id 不对）/ ESP_ERR_NOT_FOUND（没有这台）/ ESP_ERR_INVALID_STATE（没在跑 / 忙） */
esp_err_t hub_matter_remove(const char *id, bool force, uint32_t *job_out, char *why, size_t why_len);
#endif
