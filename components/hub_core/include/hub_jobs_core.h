#pragma once
/* hub_jobs_core —— 作业表（docs/15_muse_direct_design_v1.md 6.6；M7 第三步：第一个要用它的是 hub.matter.commission，最长 3 分钟）。
 * 纯逻辑，不依赖 IDF（test_host/ 原样编）；加锁、时钟、完成通知在 hub_jobs.c。
 *
 * 一个作业 = 一次耗时命令的执行实例：RUNNING → DONE / FAILED / TIMEOUT，结论连同结果体 / 错误句子留在表里，
 * 直到这一格被新作业复用（本地 REST 用 GET /api/v1/hub/job?id= 轮询；Muse 在完成时收到 link.result）。
 * 同时在途最多 HJC_MAX 个（6.6 写 4）；表满时复用最早结束的那一格，全都在途就拒（调用方回 busy）。
 * id 从 1 起递增、不回绕到 0（0 = 没有作业）。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HJC_MAX         6
#define HJC_INFLIGHT    4          /* 6.6：最多 4 个同时在途 */
#define HJC_CMD_MAX     32
#define HJC_CODE_MAX    24
#define HJC_MSG_MAX     208
#define HJC_RESULT_MAX  384        /* 结果体（一个 JSON 对象的文本），放不下的由生产者截短（完成时只放小结论） */

typedef enum { HJC_FREE = 0, HJC_RUNNING, HJC_DONE, HJC_FAILED, HJC_TIMEOUT } hjc_state_t;

typedef struct {
    uint32_t id;
    uint8_t  state;                /* hjc_state_t */
    uint8_t  src;                  /* hub_src_t（Muse / 本地 REST），审计用 */
    char     cmd[HJC_CMD_MAX];
    uint32_t t_start, deadline, t_end;   /* 毫秒（调用方的单调时钟，回绕按差值算） */
    char     err_code[HJC_CODE_MAX];
    char     err_msg[HJC_MSG_MAX];
    char     result[HJC_RESULT_MAX];
} hjc_job_t;

typedef struct {
    hjc_job_t jobs[HJC_MAX];
    uint32_t  next_id;
} hjc_table_t;

void hjc_init(hjc_table_t *t);
/* 开一个作业：返回 id；在途已满 / 参数不对返回 0 */
uint32_t hjc_start(hjc_table_t *t, const char *cmd, uint8_t src, uint32_t now, uint32_t timeout_ms);
/* 结束：err_code 为 NULL = 成功（result 是结果体，可为 NULL = "{}"），否则失败（err_msg 一句话）。
   只有 RUNNING 的作业能结束；已经超时 / 结束过 / 不存在返回 false（迟到的完成不覆盖超时的结论） */
bool hjc_finish(hjc_table_t *t, uint32_t id, uint32_t now, const char *err_code, const char *err_msg, const char *result);
/* 把过了期限还在 RUNNING 的改成 TIMEOUT，把它们的 id 写进 ids（最多 max 个），返回个数 */
int hjc_expire(hjc_table_t *t, uint32_t now, uint32_t *ids, int max);
const hjc_job_t *hjc_get(const hjc_table_t *t, uint32_t id);
/* 放弃一个还在跑的作业：这一格直接回到空闲（不留结论）。给"开了作业、后端当场就拒了"用：调用方已经当场回了错误，
   不该再记一条审计、也不该挤掉别人还在轮询的旧结论（M7 第三步审计）。不是 RUNNING 返回 false */
bool hjc_abandon(hjc_table_t *t, uint32_t id);
int hjc_inflight(const hjc_table_t *t);
const char *hjc_state_str(uint8_t st);
