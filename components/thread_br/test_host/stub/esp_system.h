#pragma once
/* 主机侧桩：srp_readv_start() 用 esp_register_shutdown_handler() 挂重启前的 bye。
 *
 * 桩记下最后一次注册的处理函数并可被测试读取 —— 只验"注册这个动作发生了"。
 * bye 是否真的上线取决于 mdns 组件内部的 PCB 状态机，主机上判不了，
 * 那部分见 docs/test_cases_srp_readvertise.md 的 TC-S01~S04（要板子 + tcpdump）。
 */
#include "esp_err.h"

typedef void (*shutdown_handler_t)(void);

static shutdown_handler_t g_stub_shutdown_handler;
static int                g_stub_shutdown_reg_count;

static inline esp_err_t esp_register_shutdown_handler(shutdown_handler_t h)
{
    /* 与 IDF 行为一致：同一个 handler 重复注册返回 INVALID_STATE。
       srp_readv 的 stop→start 会走到第二次，它按这个返回值判断并忽略。 */
    if (g_stub_shutdown_handler == h) return ESP_ERR_INVALID_STATE;
    g_stub_shutdown_handler = h;
    g_stub_shutdown_reg_count++;
    return ESP_OK;
}

static inline esp_err_t esp_unregister_shutdown_handler(shutdown_handler_t h)
{
    if (g_stub_shutdown_handler != h) return ESP_ERR_INVALID_STATE;
    g_stub_shutdown_handler = NULL;
    return ESP_OK;
}
