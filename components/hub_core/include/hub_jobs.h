#pragma once
/* hub_jobs —— 作业表的加锁外壳（docs/15_muse_direct_design_v1.md 6.6；规则与格式见 hub_jobs_core.h）。只在直连型里编。
 *
 * 耗时命令（hub.matter.commission、本地的移除）的处理函数开一个作业、把它交给后端（matter_ctrl 的 CHIP 任务）就返回；
 * 后端完成时调 hub_job_finish。本地 REST 回 202 {job}、用 GET /api/v1/hub/job?id= 轮询；Muse 在完成时收到 link.result
 * （muse_link 登记的监听者被叫醒，在会话任务里发）。没有 6.6 写的 worker 任务：现有的后端都是异步回调的，不需要谁在作业上等。
 * 过期：hub_svc 每轮调 hub_jobs_tick（10 s），过了期限还没结束的改 TIMEOUT 并通知 —— 后端自己也有更短的期限。
 *
 * 线程：任何任务都能调（锁只护 memcpy 级的操作，等 ≤ 100 ms）；不能在 ISR、esp_timer 回调里调（红线 10）。
 * 监听者在结束作业的那个任务里被调（CHIP 任务 / hub_svc 任务），只许投递、不许阻塞。 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "hub_jobs_core.h"
#include "hub_policy.h"

esp_err_t hub_jobs_init(void);                                        /* 幂等；表在 PSRAM */
/* 开一个作业；在途满（HJC_INFLIGHT）/ 没 init 返回 0 */
uint32_t hub_job_start(const char *cmd, hub_src_t src, uint32_t timeout_ms);
/* 结束（err_code NULL = 成功，result 是 JSON 对象文本）。结束成功会记一条审计（命令名、最终结论、耗时）并通知监听者。
   作业已经超时 / 结束过 / 不存在返回 false（迟到的完成不覆盖） */
bool hub_job_finish(uint32_t id, const char *err_code, const char *err_msg, const char *result_json);
/* 拷一份（轮询用）。没有返回 false */
bool hub_job_get(uint32_t id, hjc_job_t *out);
/* 放弃（后端当场就拒了：调用方已经当场回了错误）。不记审计、不通知 */
void hub_job_abandon(uint32_t id);
void hub_jobs_tick(void);                                             /* hub_svc 每轮调 */
void hub_jobs_set_listener(void (*fn)(uint32_t id));                  /* 一个（muse_link）；NULL 取消 */
