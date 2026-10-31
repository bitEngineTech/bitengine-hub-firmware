#pragma once
/* ble_window 组件内部共享（blew_boot.c ↔ ble_window.c），不对外。 */
#include <stdbool.h>

bool blew_bt_mem_released(void);

#include "blew_core.h"
/* blew_selfcheck.c：请求进配对剖面时记自检基线（request_isolated 里、重启之前）；状态页读结果 */
void blew_selfcheck_capture_baseline(void);
void blew_selfcheck_note_bt_used(void);   /* 配对剖面里窗口真开起来了（blew_do_open 成功）*/
void blew_selfcheck_status(const char **state, blew_res_t *rec);
