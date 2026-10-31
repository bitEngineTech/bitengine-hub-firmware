#pragma once
/* hub_audit_core —— 审计环（docs/15_muse_direct_design_v1.md 6.10）。纯 C，不依赖 ESP-IDF：test_host/ 原样编它。
 *
 * 128 条，满了覆盖最旧的：时间、来源（muse / rest）、命令、设备 id、动作、结果码、耗时。**不记参数里的秘密**：只记 id 与 action
 * 两个参数（附录 A 的命令参数里没有秘密，但将来加了也进不来）。命令名、设备 id、动作可能来自 Muse 下发的报文（不认识的命令也要记），
 * 进环前换掉 [A-Za-z0-9_.-] 以外的字符、截断，所以 JSON 与日志里都是干净的记号。
 * **合并**：与最近 8 条里某一条的来源、命令、设备、动作、结果都相同、且相隔 ≤ 60 s 的，不另占一条：那条 repeat 加一、时间改成这次、
 * 重新分配 seq、挪到最前 —— 列表永远按"最后一次"排、seq 从前往后递减，按 seq 增量读的人也能看到次数变了
 * （2026-10-22 两轮审计：被限速的刷屏客户端、按读限速轮询的本地页面、交替发两三种命令的调用方，一分钟内就能把 128 条冲掉）。
 * 不是线程安全的：调用方（hub_audit.c）持锁调。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hub_json.h"

#define HAU_CAP       128
#define HAU_TOKEN_MAX 40      /* 命令名（附录 A 最长 "hub.matter.commission" 21）、动作、结果码 */
#define HAU_DEV_MAX   24      /* 设备 id（"zb-…" 19） */
#define HAU_MERGE_MS  60000u  /* 相同调用合并成一条的间隔上限 */
#define HAU_MERGE_SCAN 8      /* 往回找几条 */

typedef enum { HAU_SRC_MUSE = 0, HAU_SRC_REST = 1 } hau_src_t;   /* 与 hub_policy.h 的 hub_src_t 取值相同 */

typedef struct {
    uint32_t seq;                    /* 进环时分配，从 1 起；0 = 空槽 */
    uint64_t mono_ms;                /* 开机毫秒：算 age_s */
    char     clock[6];               /* 本地 "HH:MM"（已对时）；没对时空串 */
    uint8_t  src;                    /* hau_src_t */
    char     command[HAU_TOKEN_MAX];
    char     device[HAU_DEV_MAX];    /* 参数 id；没有是空串 */
    char     action[HAU_TOKEN_MAX];  /* 参数 action；没有是空串 */
    char     result[HAU_TOKEN_MAX];  /* "ok" 或错误码（invalid_params / forbidden / rate_limited / unsupported …） */
    uint32_t dur_ms;                 /* 最近一次的耗时 */
    uint16_t repeat;                 /* 合并了几次（≥ 1；到 65535 不再加）；进环时由 hau_add 定，调用方给的忽略 */
} hau_entry_t;

typedef struct {
    hau_entry_t e[HAU_CAP];
    uint32_t    next_seq, head, count;
} hau_ring_t;

void hau_init(hau_ring_t *r);
/* 把 src 的记号拷进 dst（cap 含 NUL）：[A-Za-z0-9_.-] 以外的字节换成 '?'，超长截断；src 为 NULL 得空串 */
void hau_token(char *dst, size_t cap, const char *src);
/* 进环（seq 由这里分配，合并时也分配新的；字符串字段会再过一遍 hau_token，调用方传什么都安全）。返回 seq；命令名为空返回 0、不进环。
   merged 可为 NULL：非 NULL 时告诉调用方这次是不是合并进了已有的一条。 */
uint32_t hau_add(hau_ring_t *r, const hau_entry_t *in, bool *merged);
/* GET /api/v1/muse/audit 的结果：{"entries":[…],"count":N}，新的在前；合并过的条目带 "repeat":N（N ≥ 2）；limit 夹到 1–128；
   放不下撤半条带 "truncated":true */
void hau_write(hj_t *w, const hau_ring_t *r, uint64_t now_ms, uint32_t limit);
