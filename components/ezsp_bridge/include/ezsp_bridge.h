#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_event.h"
#include "sdkconfig.h"          /* CONFIG_BITENGINE_ENABLE_ZHUB 决定下面声明哪个 init */

#ifdef __cplusplus
extern "C" {
#endif

// ---------------- Constants ----------------
/* 只接一个上位机（2026-09-23 由 2 改 1）。EZSP/ASH 是点对点协议：第二个上位机连上时
   发的 RST 会把第一个正在用的 NCP 复位掉。早先"ASH 序列号会 NAK 其中一个、属协议
   容错"的理由不成立。同 IP 重连仍会顶掉旧连接（HA 重启场景）；另一台主机来连则
   走 reject_hold，挂着等它自己断开。见 docs/02 §1 与 TC-M12。 */
#define MAX_TCP_CLIENTS     1
#define CLIENT_TX_BUF_SIZE  2048

// ---------------- Types ----------------

typedef enum {
    BRIDGE_MODE_LAN,
    BRIDGE_MODE_USB,
    BRIDGE_MODE_DISABLED,
} bridge_mode_t;

typedef enum {
    BRIDGE_STATE_INIT,
    BRIDGE_STATE_RUNNING,
    BRIDGE_STATE_DRAINING,
    BRIDGE_STATE_SWITCHING,
    BRIDGE_STATE_ERROR,
    /* MG24 firmware update owns the UART exclusively. Everything that polls
       the NCP keys off "state != RUNNING" and therefore stands down on its
       own — in particular health_monitor's ASH RST probe, which would
       otherwise fire mid-XMODEM and eat our bytes. */
    BRIDGE_STATE_OTA,
#if CONFIG_BITENGINE_ENABLE_ZHUB
    /* 直连型：zhub（网关自己的 Zigbee 主机）持有 UART。和 OTA 一样 != RUNNING，所有"轮询 NCP"的路径照旧自己让开；
       不借 BRIDGE_STATE_OTA：ota_mg24_exit_bootloader() 拿 state == OTA 判断"UART 已经是我的"（docs/15 设计 8.1，components/zhub/zhub.h） */
    BRIDGE_STATE_ZHUB,
#endif
} bridge_state_t;

typedef struct {
    uint32_t uart_rx_bytes;
    uint32_t uart_tx_bytes;
    uint32_t uart_rx_errors;
    uint32_t uart_tx_timeouts;

    uint32_t tcp_rx_bytes;
    uint32_t tcp_tx_bytes;
    uint32_t tcp_connects;
    uint32_t tcp_disconnects;
    uint32_t tcp_client_drops;

    uint32_t usb_rx_bytes;
    uint32_t usb_tx_bytes;

    uint32_t ncp_resets_detected;

    uint8_t  active_clients;
    /* 最近一次"有协调器连着"的 uptime 秒（有客户端时持续刷新；开机起从没连过
       则为 0）。用来回答"Zigbee 实际断了多久" —— 没有 EZSP 主机时 NCP 的协议栈
       是未初始化的（网关自己拉不起来，wd_probes.c），所以这个时长就是用户
       真正关心的停服时长。mg24_alive 回答的是另一个问题（芯片在不在）。 */
    uint32_t last_client_seen_s;
    uint32_t uptime_seconds;
} bridge_stats_t;

typedef struct {
    int      index;
    char     ip_str[16];
    /* Seconds since boot, same clock as esp_timer_get_time()/1000000.
       Was FreeRTOS ticks once: the REST layer subtracts it from a seconds
       value, so the mismatched units underflowed uint32 and the WebUI showed
       clients as connected for ~136 years. Keep the unit or fix both sides. */
    uint32_t connected_at;
    uint32_t bytes_rx;
    uint32_t bytes_tx;
} tcp_client_info_t;

// ---------------- Events ----------------
ESP_EVENT_DECLARE_BASE(BRIDGE_EVENT);

typedef enum {
    BRIDGE_EVENT_CLIENT_CONNECTED,
    BRIDGE_EVENT_CLIENT_DISCONNECTED,
    BRIDGE_EVENT_MODE_CHANGED,
    BRIDGE_EVENT_UART_ERROR,
    BRIDGE_EVENT_CLIENT_DROPPED,
    BRIDGE_EVENT_NCP_RESET,         /* NCP sent RSTACK (data = reset_code byte) */
} bridge_event_id_t;

// ---------------- Public API ----------------

#if CONFIG_BITENGINE_ENABLE_ZHUB
/* 直连型（docs/15 设计 2.6 第 6 步、架构 5.8 R-2）：只建 MG24 的 UART（+ 开机读 NCP 版本），没有 :6638、
   没有经 USB 的桥，数据面恒为 BRIDGE_MODE_DISABLED。UART 留给 MG24 升级（ota_claim/release）与 M4 的 zhub。
   完整桥的 ezsp_bridge_init() 在直连型里不声明 —— 误调就编译不过。 */
esp_err_t          ezsp_bridge_init_uart_only(void);
#else
esp_err_t          ezsp_bridge_init(void);
#endif
/* 直连型只接受 BRIDGE_MODE_DISABLED，LAN / USB 回 ESP_ERR_NOT_SUPPORTED。 */
esp_err_t          ezsp_bridge_switch_mode(bridge_mode_t mode);
bridge_mode_t      ezsp_bridge_get_mode(void);
bridge_state_t     ezsp_bridge_get_state(void);
const bridge_stats_t *ezsp_bridge_get_stats(void);

/* ---- 线路电平采样（只读诊断，2026-09-15）----
 *
 * 目的：找一个**不破坏协议栈**的 MG24 存活证据。当时的空闲探测靠发 ASH RST 拿
 * 证据，而 RST 会把 NCP 的 Zigbee 协议栈打回未初始化 —— 用摧毁网络来证明网络还在。
 * T9.31（2026-10-08）起无客户端时不再探测，wd_probes.c 只用这里的判据判死。
 *
 * 取的是 UART_STATUS_REG 的 ctsn(bit14) 与 rxd(bit15)：**外设内部那根信号**，
 * 不是 GPIO 输入寄存器，所以不受 gpio_matrix_input() 是否开 IE 的影响。
 *   ctsn：MG24 的 RTS（PA07→IO3）。低 = 对端断言了 CTS = 它的 UART 活着且能收。
 *   rxd ：MG24 的 TX（PA05→IO4）。空闲应为 1（marking）。
 *
 * ⚠️ 这只是**候选**判据，成不成立要看实测：MG24 复位期间 PA07 是高阻，而这条
 * 网上没有任何外部上下拉（BOM 只有 R33/R34 两个 470Ω 串阻），悬空读什么未知。
 * ⚠️ 即使成立也只证明"芯片在、UART 外设配起来了"，证明不了固件主循环没死。
 *
 * 纯读寄存器，不改任何配置，不发一个字节。
 */
typedef struct {
    uint32_t duration_ms;
    uint32_t samples;
    uint32_t cts_deasserted;      /* ctsn==1 的采样数 */
    uint32_t cts_transitions;
    uint32_t cts_longest_deassert_ms;
    uint32_t rxd_low;             /* rxd==0 的采样数（非 marking） */
    uint8_t  cts_now;
    uint8_t  rxd_now;
} uart_line_sample_t;

void uart_bridge_sample_lines(uint32_t ms, uart_line_sample_t *out);

/* 线路是否在整个采样窗口里都说"对端没在驱动"（ctsn 恒为 1）。
 * true = MG24 的 USART 没配起来（复位中 / 掉电 / 没跑起来）。**强负面证据**。
 * false = 芯片有电且 USART 配好了 —— 但**证明不了固件主循环还活着**
 *         （外设状态在硬件里，CPU 死循环时 RTS 照样断言，而 NCP 工程
 *          bitengine_ncp.slcp 里没有看门狗组件，不会退化成可见的复位环）。
 * 所以只能当负面判据用，不能当"活着"的正面证据。 */
bool uart_bridge_line_says_absent(uint32_t ms);
void               ezsp_bridge_reset_stats(void);
/* 直连型没有 TCP 客户端：get_clients 恒回 0 条，两个 disconnect 回 ESP_ERR_NOT_SUPPORTED。 */
void               ezsp_bridge_get_clients(tcp_client_info_t *out_clients, int *out_count);
esp_err_t          ezsp_bridge_disconnect_client(int client_index);
/* 按 IP 断开（可能命中多条：同一主机重连期新旧连接会并存）。
   匹配与关闭在同一次持锁内完成，没有"查到下标再去关、槽位已被复用"的窗口。
   out_closed 可为 NULL。一条都没命中返回 ESP_ERR_NOT_FOUND。 */
esp_err_t          ezsp_bridge_disconnect_client_by_ip(const char *ip, int *out_closed);

/**
 * Health probe: send ASH RST frame and wait for RSTACK response.
 * Only valid when active_clients == 0 (no active ASH session).
 * @param timeout_ms  Max time to wait for RSTACK
 * @return true if RSTACK received, false on timeout or clients active
 */
bool               ezsp_bridge_health_probe(int timeout_ms);

/**
 * Claim the UART for an MG24 firmware update.
 *
 * Moves the bridge to BRIDGE_STATE_OTA so no other task touches the line.
 * The caller must already have switched to BRIDGE_MODE_DISABLED, which is
 * what actually tears down the TCP server and closes client sockets; this
 * only fences off the pollers.
 *
 * @return ESP_ERR_INVALID_STATE if a mode switch is in flight or OTA is
 *         already claimed.
 */
esp_err_t          ezsp_bridge_ota_claim(void);

/** Release the claim, returning the bridge to BRIDGE_STATE_RUNNING. */
void               ezsp_bridge_ota_release(void);

/**
 * 独占 UART 跑一次 NCP 会话（版本 + 下发网关地区，docs/14 第 7.5 节）。
 * 会切 DISABLED 断开所有 TCP 客户端、软复位 MG24，然后切回原模式。
 * 从 httpd 任务调，耗时约 2-4 s。OTA 进行中返回 ESP_ERR_INVALID_STATE，配置已存、下次开机生效。
 */
esp_err_t          ezsp_bridge_ncp_resync(void);
#if CONFIG_BITENGINE_ENABLE_ZHUB
/* ---- 直连型：UART 归 zhub（docs/15 设计 8.1；components/zhub/zhub.h） ----
   zhub 认领 / 归还 UART：BRIDGE_STATE_RUNNING ↔ BRIDGE_STATE_ZHUB 的 CAS。别人（MG24 升级、uart_bench、ncp_resync、bootloader 进出）
   照旧调 ezsp_bridge_ota_claim()：它先经钩子请 zhub 让出（同步，最多 EZSP_ZHUB_YIELD_MS），ezsp_bridge_ota_release() 之后放它回来。
   suspend 失败（超时）时 zhub 已自己撤回请求；suspend 成功而认领失败时由 ota_claim 配一次 resume。 */
#define EZSP_ZHUB_YIELD_MS 3000
esp_err_t          ezsp_bridge_zhub_claim(void);
void               ezsp_bridge_zhub_release(void);
void               ezsp_bridge_set_zhub_hooks(esp_err_t (*suspend)(uint32_t timeout_ms), void (*resume)(void));
/* zhub 收发的字节计入 bridge_stats（/status 的 uart_rx/tx_bytes、产测 ezsp_uart 项、health_monitor 错误率的分母都读它；K13）。
   tx_short：这一帧没写完（流控卡住超过写超时），计 uart_tx_timeouts。只从 zhub 任务调。 */
void               ezsp_bridge_note_uart(uint32_t rx, uint32_t tx, bool tx_short);
#endif

#ifdef __cplusplus
}
#endif
