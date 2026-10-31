/* mc_sub_core —— 订阅的纯逻辑（见 mc_sub.cpp；主机测试 test_host/mc_sub_test.c）：订哪些路径、报告值怎么换算进读数、
 * 哪些翻转要出事件、什么时候算离线、重订退避。不碰 CHIP。 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "matter_ctrl_iv.h"
#include "matter_ctrl_live.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MC_SUB_PATHS_MAX   10          /* 一条订阅最多几条路径（规范只保证设备支持 3 条：设备拒了就收窄到前 3 条，mc_sub.cpp） */
#define MC_SUB_NARROW      3
#define MC_SUB_MIN_IV_S    1           /* 9.5：最小间隔 1 s、最大间隔上限 300 s（ICD 自己会报它能做到的） */
#define MC_SUB_MAX_IV_S    300
#define MC_BATT_LOW_PCT    15u         /* 同 Zigbee（ZH_ST_BATT_LOW_PCT，6.7） */
#define MC_BATT_REARM_PCT  20u         /* 同 Zigbee（ZH_ST_BATT_REARM_PCT）：报过低之后回到这里及以上才重新算 */

typedef struct {
    uint16_t ep;
    uint16_t cl;
    uint16_t at;
    uint8_t  field;                    /* MC_ST_*；0xFF = 只为保活（没有可读的能力时订 NodeLabel） */
} mc_path_t;
#define MC_FIELD_NONE 0xFF

/* 按访谈结果挑路径（优先级：开关、亮度、门磁 / 漏水、烟雾、人体、温湿度气压照度、功率、电量、电能）；同一字段只订第一个
   非根端点（PowerSource 例外：根端点也认、每个都订，哪个报出电量就用哪个）。一条都没有时订根端点的 NodeLabel 保活。返回条数 */
int mc_sub_paths(const mc_iv_t *iv, mc_path_t *out, int max);
/* 第一个有这个簇的非根端点；没有 -1（控制要往哪个端点发，同订阅挑路径的规则） */
int mc_sub_first_ep(const mc_iv_t *iv, uint16_t cl);
/* 两份路径是否相同（逐字段比：结构体末尾有填充字节，别用 memcmp） */
bool mc_sub_paths_eq(const mc_path_t *a, const mc_path_t *b, int n);

typedef enum { MC_V_NULL = 0, MC_V_BOOL, MC_V_UINT, MC_V_INT } mc_vtype_t;
typedef struct {
    mc_vtype_t t;
    uint64_t   u;                      /* MC_V_UINT / MC_V_BOOL（0/1） */
    int64_t    i;                      /* MC_V_INT */
} mc_val_t;

/* 读数翻转的事件位 MC_SEV_* 在 matter_ctrl_live.h（钩子约定的一部分） */

/* 把一个报告值放进读数。类型不对 / 超范围的丢掉（返回 false，读数不变）；null 清掉该字段。*sev 带出翻转事件位 */
bool mc_sub_apply(mc_state_t *st, uint8_t field, const mc_val_t *v, int64_t now_ms, unsigned *sev);

/* 在线判定（9.5「离线判定」：订阅断了、超过 2 × 最大间隔没听到它 —— 这段时间里 CHIP 一直在重建 CASE 重订）。
   up = 订阅现在是建好的；ref_ms = 最后听到它的时刻，没听到过就是开始订它的时刻。返回新的 online（1 / 0 / -1 不变） */
int8_t mc_sub_judge(int8_t cur, bool up, int64_t ref_ms, uint16_t max_iv_s, int64_t now_ms);

/* ---- 控制（M7 第六步；9.5「命令」：3 s 内收到属性报告 = confirmed，只有 invoke 成功 = delivered） ---- */
#define MC_CTL_CONFIRM_MS  3000
/* 亮度 % → CurrentLevel（1–254；0 % 不走这个，发 Off）。与订阅那边的反算（level → %）互逆到 ±1 % */
uint8_t mc_ctl_level(uint8_t pct);
/* 发出之后读数到没到目标：对应字段在 sent_ms 之后报过、值对得上。act 是 MATTER_CTRL_ACT_*；want_on 1 / 0 / -1（切换且不知道之前的状态：报过就算） */
bool mc_ctl_reached(const mc_state_t *st, int act, int8_t want_on, uint8_t want_pct, int64_t sent_ms);
/* 发之前就已经是目标（设备不会因为没变化而报）：开 / 关 / 亮度（亮着、亮度 ±1 %）。切换永远不是 */
bool mc_ctl_already(const mc_state_t *st, int act, uint8_t want_pct);

/* 重订退避（9.5：5 → 30 → 120 s，之后一直 120 s）。tries 从 1 起 */
uint32_t mc_sub_backoff_ms(uint32_t tries);
/* ±20 % 抖动（rnd 随便一个随机数）：一批设备同时断（Thread 掉网恢复）时别按同一节奏一起抢 CASE 客户端（只有 4 个） */
uint32_t mc_sub_jitter(uint32_t ms, uint32_t rnd);

#ifdef __cplusplus
}
#endif
