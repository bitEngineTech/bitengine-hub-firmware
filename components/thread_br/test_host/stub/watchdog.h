#pragma once
/* 主机侧桩：srp_readv 注册进看门狗登记表只为拿到 stack_left 可见性
 * （wd_kick 内会自采 uxTaskGetStackHighWaterMark(NULL)）。
 *
 * 主机上没有 FreeRTOS 也没有栈高水位可言，所以这里只做两件事：
 *   1. 让编译过得去
 *   2. 记下 kick 次数，供测试断言"任务循环确实在跑"（目前没有用例依赖它，
 *      留着是因为哪天 readv_task 的循环结构改了，这个计数是最直接的判据）
 *
 * ⚠️ 别在这里模拟真实的看门狗语义。真正的超时判定与恢复阶梯在板上，
 * 主机侧测试的边界见 test_host/run.sh 开头那段「不覆盖什么」。
 */
#include "esp_err.h"

typedef enum {
    WD_SEV_OBSERVE = 0,
    WD_SEV_RECOVER,
    WD_SEV_FATAL,
} wd_severity_t;

typedef struct {
    const char   *name;
    uint32_t      deadline_ms;
    wd_severity_t severity;
} wd_task_spec_t;

static int g_stub_wd_slot_next;
static int g_stub_wd_kicks;

static inline int wd_register(const wd_task_spec_t *spec)
{
    (void)spec;
    return g_stub_wd_slot_next++;
}

static inline void wd_kick(int slot)
{
    (void)slot;
    g_stub_wd_kicks++;
}
