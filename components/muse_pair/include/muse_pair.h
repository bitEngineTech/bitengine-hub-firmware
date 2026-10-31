#pragma once
/* muse_pair —— Muse 社区配对 v5 的胶水层（docs/15_muse_direct_design_v1.md 第五节；只在 BITENGINE_ENABLE_MUSE_LINK 下编）。
 *
 * 策略 C（设计 3.2）：
 *  - 正常开机：网页 POST /api/v1/muse/pair → muse_pair_check_request() 判前提 → 回 202 → muse_pair_enter()
 *    （落盘设置、ble_window_request_isolated()：重启进配对剖面）。
 *  - 配对剖面：main.c 调 muse_pair_boot_run()，它起一个任务做完全部流程（读 SDK token → 开窗前预检 → 开 BLE 窗口 →
 *    协议 → 按键确认 → 凭据 → fetch_vms 验证 → 落盘 → auth_ok），结束（成功 / 失败 / 超时 / 取消）后把结果写进
 *    hubdb muse/pair_res，记一笔计划内重启，esp_restart() 回正常开机。
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    MUSE_PAIR_REQ_OK = 0,
    MUSE_PAIR_REQ_NO_PASSWORD,      /* 没设管理密码（设计 5.6：409） */
    MUSE_PAIR_REQ_NO_SDK_TOKEN,     /* 没设 SDK token（409） */
    MUSE_PAIR_REQ_DISABLED,         /* Muse 没启用（409） */
    MUSE_PAIR_REQ_IN_PAIR_PROFILE,  /* 已经在配对剖面里（409） */
    MUSE_PAIR_REQ_SAFE_MODE,        /* 安全模式：重启后也进不了配对剖面（安全模式优先），白重启一次（409） */
} muse_pair_req_t;

typedef struct {
    const char *state;              /* 见 muse_pair.c 的 state_str()；正常开机恒为 "idle" */
    bool        pair_boot;          /* 这次开机是配对剖面 */
    uint32_t    window_s;           /* 配对窗口时长（Kconfig） */
    uint32_t    remaining_s;        /* 窗口还剩多久（没开为 0） */
    const char *last_result;        /* hubdb 里上次配对的结果（pair_core.h 的 pair_res_str），没有为 "none" */
    uint32_t    last_result_epoch;  /* 上次结果的墙钟（没对时为 0） */
    char        ble_name[24];       /* 手机 App 里要选的设备名（配对剖面里才有） */
} muse_pair_status_t;

/* 正常开机：/muse/pair 的前提判断（不碰网络；要读两次 hubdb，每次最多约 0.5 s）。 */
muse_pair_req_t muse_pair_check_request(void);
/* 正常开机：进配对剖面。先回 HTTP 包、再调（内部会 esp_restart()，成功不返回）。调用方必须在内部栈的任务里。 */
esp_err_t muse_pair_enter(void);

/* 配对剖面：起配对任务。 */
esp_err_t muse_pair_boot_run(void);
/* 配对剖面：用户取消（结果记 cancelled，随后重启回正常开机）。正常开机里返回 ESP_ERR_INVALID_STATE。 */
esp_err_t muse_pair_cancel(void);

void muse_pair_get_status(muse_pair_status_t *out);
/* 只读内存的状态串（给 /api/v1/status 这种被频繁轮询的地方；muse_pair_get_status 会读 hubdb） */
const char *muse_pair_state_str(void);
