#pragma once
/* hub_call —— 命令处理函数的调用上下文（docs/15_muse_direct_design_v1.md 6.3、6.6）。纯声明，不依赖 ESP-IDF。
 * 同一个处理函数服务两个出口（MD-9）：Muse 的 link.invoke（muse_link）与本地 REST（/api/v1/hub/invoke、/hub/devices，webui 的 hub_api.c）。
 * 调用方先做完参数校验（hub_params_check）、权限检查（hub_policy_check_cmd）、限速（hub_rl_take），再调处理函数。 */
#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "hub_json.h"
#include "hub_policy.h"

typedef struct hub_call {
    hub_src_t    src;
    uint8_t      perm;            /* Muse：调用时的权限位（muse/perm，执行时第 3 道检查的敏感判定要用）；本地 REST 不看（6.5 注记） */
    const cJSON *params;          /* 可为 NULL（= 空对象）；已过类型校验 */
    hj_t        *out;             /* 写入点停在 payload 的值上：处理函数写一个对象 */
    const char  *err_code;        /* 失败时：invalid_params / not_found / unavailable / internal … */
    char         err_msg[160];    /* 失败时：面向用户的一句英文解释（会拼成 "<code>: <msg>"） */
    uint32_t     job;             /* 处理函数开了作业、结论晚到（6.6；hub.matter.commission）：置成作业 id，payload 里写 {job, state}。
                                     Muse 出口这时不回包，等作业结束再发 link.result；本地 REST 回 202。0 = 当场就有结论 */
    bool         rl_refund;       /* 失败时：当场拒了、没真去做 —— 出口把这次扣的限速令牌还回去（MX21；只有配网用：码打错不该罚一分钟） */
} hub_call_t;

/* 处理函数：成功把 payload（一个对象）写进 call->out、返回 0；失败填 err_code / err_msg、返回 -1（out 里的内容作废）。
   payload 太大时用 hj_mark / hj_rollback 截断并带 truncated / next_cursor（6.9），不要返回失败。 */
