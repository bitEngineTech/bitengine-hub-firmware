#include "tcp_server.h"
#include "uart_bridge.h"
#include "client_buffer.h"
#include "bridge_stats.h"
#include "ezsp_bridge.h"
#include "config_mgr.h"
#include "watchdog.h"
#include "esp_timer.h"
#include "esp_system.h"   /* esp_register_shutdown_handler */
#include "esp_task_wdt.h"

#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "led_ctrl.h"
#include "esp_event.h"

/* 层 B 心跳槽。accept 任务不注册：它常态阻塞在 accept() 上没有上界，
   注册进来只会误报（设计文档第五节 5.1 的"必须有上界等待"约束）。 */
static int s_wd_slot_u2t = -1;
static int s_wd_slot_t2u = -1;

static void shutdown_close_clients(void);   /* 定义在文件末尾，注册点在 start() 里 */

static const char *TAG = "tcp_server";

// ---- Client state (shared) ----
typedef struct {
    int           fd;
    ring_buffer_t tx_buf;
    uint32_t      connected_at;
    uint32_t      bytes_rx;
    uint32_t      bytes_tx;
    char          ip_str[16];
} tcp_client_t;

static tcp_client_t     s_clients[MAX_TCP_CLIENTS];
static SemaphoreHandle_t s_client_mutex;     /* created once, never deleted */
static SemaphoreHandle_t s_task_exit_sem;    /* counting: tasks signal exit */
static int               s_tasks_started = 0;
static int               s_server_fd = -1;
static volatile bool     s_running   = false;

/* ASH RSTACK passive detection — scans UART→TCP byte stream without modification.
   RSTACK pattern: 0x1A (cancel) 0xC1 (RSTACK) 0x02 (ASHv2) [reset_code] */
typedef enum {
    RSTACK_IDLE,
    RSTACK_SAW_CANCEL,  /* saw 0x1A */
    RSTACK_SAW_C1,      /* saw 0x1A 0xC1 */
    RSTACK_SAW_VER,     /* saw 0x1A 0xC1 0x02 — next byte is reset_code */
} rstack_detect_state_t;

static rstack_detect_state_t s_rstack_state = RSTACK_IDLE;
static volatile bool         s_ncp_reset_pending = false;
static volatile bool         s_client_rst_sent = false;  /* client sent ASH RST, expect RSTACK */
static volatile TickType_t   s_client_rst_tick = 0;      /* when the RST was seen */
/* An expected RSTACK should arrive within this window; afterwards a RSTACK
   is treated as unsolicited again (NCP crashed on its own). */
#define CLIENT_RST_EXPECT_MS  10000
/* Client→NCP RST detection state survives recv() boundaries: the 2-byte
   pattern 0x1A 0xC0 can be split across two recv calls. */
static bool s_tx_saw_cancel = false;

// ---- Forward declarations ----
static void tcp_accept_task(void *arg);
static void uart_to_tcp_task(void *arg);
static void tcp_to_uart_task(void *arg);
static void close_client(int index);
static void flush_client_tx(int index);

/* ── 被拒的连接：accept 之后**不主动关**，等对端自己断 ──────────────────────
 *
 * 下面两处"拒绝"（白名单不放行 / 槽位满）原本都是 accept() 完立刻 close(fd)。
 * 那恰好是 zigpy/bellows#751 的确定性复现动作，而它在 ZHA 侧的后果不是
 * "连不上、然后重试"，是**卡死**。
 *
 * 2026-09-23 A/B 实测，两轮 × 每格 n=12。**完整表格与跑法只在一处**：
 * docs/upstream/bellows-reject-ab.py 的头注（别在这里再抄一份）。
 * 对这段代码有决定意义的是两行：
 *
 *   accept 后立刻 close  →  6-7/12 (bellows 0.49.0) / 11/12 (1.0.2) 让 HA 记成
 *                           永久 setup_error（**不重试**），其中约一半还伴随
 *                           unload 挂死（条目永远卸载不完）
 *   accept 后不主动关    →  0/12。两个版本、两轮，全都是干净超时 + 正常断开 + 零警告
 *
 *   顺带：accept 后 3s 才关（落在 MG24 探测窗口之后的那种）不产生永久失败，
 *   但 12/12 每次都走完整条 TypeError 级联 —— 所以「晚点关」不是出路。
 *
 * ⚠️ 头一行是竞态，逐轮有浮动；「不主动关」那行两轮完全一致。
 *
 * "永久失败" = 逃出来的是 CancelledError（BaseException），HA 的 config_entries
 * 把它记成 setup_error 而**不重试**；"断开挂死" = unload 永远完不成。两者合起来
 * 就是现场那个签名：Zigbee 全没了、日志全静、只能整体重启 HA。
 * ⚠️ **新版更糟**（11/12 vs 6-7/12）—— 不能指望「让用户升级 HA」来解决。
 *
 * 所以拒绝时只是不把它接进桥，**关闭一律由对端发起**：bellows 于是走 startup
 * 超时那条干净路径（实测 10.5s / 13.5s 失败，然后被 HA 正常重试）。
 *
 * ⚠️ 别改成"挂 N 秒再关"：那会正好压在 bellows 最终的 disconnect() 上，
 *    变成 #745 那个竞态。下面的 cap 只用来兜住 fd，不是正常出口——
 *    正常出口是对端 EOF。
 * ⚠️ 也别改成发 RST：RST 与 FIN 都走 connection_lost，对这个 bug 一样致命
 *    （况且本档 CONFIG_LWIP_SO_LINGER 关着，见本文件末尾那段考证）。
 * ⚠️ 这不是替代那三行 `use_thread: false`：它只堵**网关主动拒绝**这一类；
 *    重启/升级导致的断开仍然要靠那三行。见 docs/guide/ZHA连接不自愈.md。
 *
 * 槽位只给 2 个：lwIP 一共 CONFIG_LWIP_MAX_SOCKETS=20，httpd/mDNS/MQTT 都在抢。
 * 满了就退回旧行为（直接 close）—— 不比今天更差；正常场景也占不满，因为每个
 * hold 都在 ~13s 内由对端自己断开，而 HA 的重试退避比这更长。 */
#define MAX_REJECT_HOLDS    2
#define REJECT_HOLD_CAP_S   60      /* 只为兜住 fd：正常出口是对端 EOF */

typedef struct {
    int      fd;
    uint32_t since;     /* 秒，与 s_clients[].connected_at 同时基 */
} reject_hold_t;

static reject_hold_t s_holds[MAX_REJECT_HOLDS];

/* 调用方必须已持 s_client_mutex（槽位满那一处就在锁内）。 */
static void reject_hold_locked(int fd)
{
    for (int i = 0; i < MAX_REJECT_HOLDS; i++) {
        if (s_holds[i].fd >= 0) continue;
        /* ⚠️ 非阻塞设不上就**别挂**：service 里的 recv 是持着 s_client_mutex 调的，
           一旦阻塞就是整座桥停转（UART↔TCP 两条通路都要这把锁）。
           宁可退回旧行为——那只是 ZHA 可能卡死，不是网关卡死。 */
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            ESP_LOGW(TAG, "拒绝连接设非阻塞失败（%d），直接关闭", errno);
            close(fd);
            return;
        }
        s_holds[i].fd    = fd;
        s_holds[i].since = (uint32_t)(esp_timer_get_time() / 1000000);
        return;
    }
    /* 挂不下了：退回旧行为。
       ⚠️ "不比今天更差"这句有个例外：这 2 个槽位**可被任意远端占住**（连上不发
          不关即可），占满期间被拒的合法 ZHA 会恢复旧的卡死行为。需要白名单开着
          + 第三方持续占位才成立，风险低，但不是零。 */
    ESP_LOGW(TAG, "拒绝槽位已满，直接关闭（ZHA 可能因此卡死，见本文件 reject_hold）");
    close(fd);
}

/* 由 tcp_to_uart_task 每轮循环调一次：只读不回，等对端 EOF。
   ⚠️ **不是**每 100ms —— 100ms 只是空闲时的节拍（select 超时 / 无客户端时的
   vTaskDelay）。满速转发时 select 立刻返回，这里每几毫秒就转一圈。常态下
   （s_holds 全空）成本是一次无争用的 take/give，可以忽略；真要节流再说。 */
static void reject_holds_service(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
    uint8_t  junk[64];

    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_REJECT_HOLDS; i++) {
        int fd = s_holds[i].fd;
        if (fd < 0) continue;

        /* 读到的一律丢弃：这条连接永远不会接进桥。 */
        int n = recv(fd, junk, sizeof(junk), 0);
        bool peer_gone = (n == 0) || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
        bool too_old   = (now - s_holds[i].since) >= REJECT_HOLD_CAP_S;

        if (peer_gone || too_old) {
            close(fd);
            s_holds[i].fd = -1;
        }
    }
    xSemaphoreGive(s_client_mutex);
}

static void reject_holds_close_all_locked(void)
{
    for (int i = 0; i < MAX_REJECT_HOLDS; i++) {
        if (s_holds[i].fd >= 0) {
            close(s_holds[i].fd);
            s_holds[i].fd = -1;
        }
    }
}

/* Scan UART→TCP byte stream for ASH RSTACK pattern (passive, no data modification) */
/* ── Zigbee 灯的"有流量"闪烁（2026-09-11 T4.4）──────────────────────────
 *
 * led_ctrl_notify_zigbee_activity() 渲染那一档一直写好着，却**零调用者**，
 * 而 docs/07 写着"Zigbee 活动蓝闪"—— 又一个文档承诺了、代码里没人接的功能。
 *
 * ⚠️ 必须节流。EZSP 是字节流，Z2M 正常工作时每秒几十到几百次读写，
 *    每次都置位等于让渲染线程每帧都在闪，既看不出"有活动"，又白占 CPU。
 * ⚠️ 节流 400ms 要和 led_ctrl 的 ZB_ACTIVITY_HOLD_MS(120ms) 一起看：
 *    连续流量下的观感是"绿底上每 400ms 闪一下蓝"。保持时间若长过节流间隔
 *    就变成常蓝，丢掉绿=正常这个基色；节流若短到接近保持时间则同理。
 * ⚠️ 用 esp_timer 而不是 xTaskGetTickCount()：这个函数在 TX 路径上会被
 *    持着客户端锁调用，tick 计数在此没问题，但 esp_timer 语义更直白，
 *    且与 bridge_stats 的其余时间戳同源。
 * 置位本身是一个 volatile bool 写，渲染侧读完即清，不需要锁。 */
static void led_activity_tick(void)
{
    static int64_t s_last_us = 0;
    int64_t now = esp_timer_get_time();
    if (now - s_last_us < 400000) return;   /* 400ms 节流，见下 */
    s_last_us = now;
    led_ctrl_notify_zigbee_activity();
    /* 同一批字节既过 UART 也过网口，所以 NET 灯一起闪 —— 它的语义是"网口上有
       数据进出"，而桥接流量正是这台设备最主要的网络流量。
       ⚠️ 这里**不为 NET 再节流**：那边自己有 400ms 的中心节流，因为它还有另外
          三个来源（WebUI / MQTT / webhook），节流只能放在汇合点。 */
    led_ctrl_notify_network_activity();
}


static void scan_for_rstack(const uint8_t *data, int len)
{
    for (int i = 0; i < len; i++) {
        uint8_t b = data[i];
        switch (s_rstack_state) {
        case RSTACK_IDLE:
            if (b == 0x1A) s_rstack_state = RSTACK_SAW_CANCEL;
            break;
        case RSTACK_SAW_CANCEL:
            if (b == 0xC1) s_rstack_state = RSTACK_SAW_C1;
            else if (b != 0x1A) s_rstack_state = RSTACK_IDLE;
            /* consecutive 0x1A: stay in SAW_CANCEL */
            break;
        case RSTACK_SAW_C1:
            if (b == 0x02) s_rstack_state = RSTACK_SAW_VER;
            else s_rstack_state = (b == 0x1A) ? RSTACK_SAW_CANCEL : RSTACK_IDLE;
            break;
        case RSTACK_SAW_VER:
            /* b = reset_code */
            bridge_stats_get()->ncp_resets_detected++;
            esp_event_post(BRIDGE_EVENT, BRIDGE_EVENT_NCP_RESET,
                           &b, sizeof(b), 0);
            /* A stale "expected" flag must not mask a real NCP crash */
            if (s_client_rst_sent &&
                (xTaskGetTickCount() - s_client_rst_tick) >
                    pdMS_TO_TICKS(CLIENT_RST_EXPECT_MS)) {
                s_client_rst_sent = false;
            }
            if (s_client_rst_sent) {
                /* Expected RSTACK — response to client's ASH RST (normal handshake).
                   Forward the data, do NOT close the connection. */
                ESP_LOGI(TAG, "NCP RSTACK (code=0x%02X) — expected (client sent RST)", b);
                s_client_rst_sent = false;
            } else {
                /* Unsolicited — NCP reset on its own (crash/watchdog).
                   Close active connections so bellows can reconnect cleanly. */
                ESP_LOGW(TAG, "NCP RSTACK (code=0x%02X) — unsolicited NCP reset", b);
                s_ncp_reset_pending = true;
            }
            s_rstack_state = RSTACK_IDLE;
            break;
        }
    }
}

// ---- Configure client socket ----
static void configure_client_socket(int fd)
{
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    /* Aggressive keepalive: detect dead connection in ~12s (was 45s).
       Helps ZHA detect device reboot faster. */
    int keepalive = 1, idle = 5, interval = 3, count = 2;
    setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE, &keepalive, sizeof(int));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,      sizeof(int));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval,  sizeof(int));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &count,     sizeof(int));

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// ---- Public: start TCP server ----
esp_err_t tcp_server_start(void)
{
    /* Synchronization objects are created once and never deleted: tasks from
       a previous instance may still reference them while winding down. */
    if (!s_client_mutex) {
        s_client_mutex = xSemaphoreCreateMutex();
        if (!s_client_mutex) return ESP_ERR_NO_MEM;
    }
    if (!s_task_exit_sem) {
        s_task_exit_sem = xSemaphoreCreateCounting(3, 0);
        if (!s_task_exit_sem) return ESP_ERR_NO_MEM;
    }
    if (s_tasks_started > 0) {
        ESP_LOGE(TAG, "start while previous instance still running");
        return ESP_ERR_INVALID_STATE;
    }

    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        s_clients[i].fd = -1;
        ring_buffer_reset(&s_clients[i].tx_buf);
    }
    for (int i = 0; i < MAX_REJECT_HOLDS; i++) s_holds[i].fd = -1;

    s_server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_server_fd < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        return ESP_FAIL;
    }

    int opt = 1;
    setsockopt(s_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(CONFIG_BITENGINE_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(s_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed: %d", errno);
        close(s_server_fd);
        s_server_fd = -1;
        return ESP_FAIL;
    }

    if (listen(s_server_fd, 2) < 0) {
        ESP_LOGE(TAG, "listen() failed: %d", errno);
        close(s_server_fd);
        s_server_fd = -1;
        return ESP_FAIL;
    }

    s_running = true;

    /* 层 B 心跳：数据通路停转 = Zigbee 整体不通，属 FATAL。
       deadline 30s 远大于它们的空闲轮询周期（50ms-1s）。 */
    static const wd_task_spec_t u2t_spec = {
        .name = "uart_to_tcp", .deadline_ms = 30000, .severity = WD_SEV_FATAL,
    };
    static const wd_task_spec_t t2u_spec = {
        .name = "tcp_to_uart", .deadline_ms = 30000, .severity = WD_SEV_FATAL,
    };
    s_wd_slot_u2t = wd_register(&u2t_spec);
    s_wd_slot_t2u = wd_register(&t2u_spec);

    static const struct { TaskFunction_t fn; const char *name; UBaseType_t pri; } tasks[] = {
        { tcp_accept_task,  "tcp_accept",  18 },
        { uart_to_tcp_task, "uart_to_tcp", 21 },
        { tcp_to_uart_task, "tcp_to_uart", 20 },
    };
    for (int i = 0; i < 3; i++) {
        if (xTaskCreate(tasks[i].fn, tasks[i].name, 3072, NULL,
                        tasks[i].pri, NULL) != pdPASS) {
            ESP_LOGE(TAG, "xTaskCreate(%s) failed", tasks[i].name);
            tcp_server_stop();   /* joins whatever did start, closes fd */
            return ESP_ERR_NO_MEM;
        }
        s_tasks_started++;
    }

    /* 覆盖所有 esp_restart() 路径，不用每个调用点各加一次。重复注册返回
       INVALID_STATE（stop→start 会走到第二次），忽略即可。 */
    esp_err_t reg = esp_register_shutdown_handler(shutdown_close_clients);
    if (reg != ESP_OK && reg != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "重启前断开钩子未注册（%s）—— 网关重启后 ZHA 会卡死不自愈",
                 esp_err_to_name(reg));
    }

    ESP_LOGI(TAG, "Listening on 0.0.0.0:%d (max %d clients)",
             CONFIG_BITENGINE_TCP_PORT, MAX_TCP_CLIENTS);
    return ESP_OK;
}

void tcp_server_stop(void)
{
    s_running = false;

    /* Close the listen socket first: wakes the accept task and prevents new
       clients from being added while we tear down. */
    if (s_server_fd >= 0) {
        close(s_server_fd);
        s_server_fd = -1;
    }

    /* Join all bridge tasks before touching client state — otherwise a
       subsequent start() could flip s_running back to true before the old
       tasks observed false, leaving duplicate readers on the UART. */
    for (int i = 0; i < s_tasks_started; i++) {
        if (xSemaphoreTake(s_task_exit_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
            ESP_LOGE(TAG, "bridge task did not exit within 3s");
        }
    }
    s_tasks_started = 0;

    /* 告诉看门狗这两个槽当前无主。任务是**按计划**结束的，不是卡死，
       但它们的 deadline 是 30s —— 停在 MG24 bootloader 期间桥一直不起来，
       停转检测会在 30s 后判 FATAL 并走 L4 整机重启（2026-08-11 实测到）。
       用 wd_slot_offline 而不是 wd_suspend：停留时长由用户决定、无上界，
       固定宽限期得反复续期，忘了续就是一次误判重启。
       不需要配对的"上线"调用：桥重启后第一次 wd_kick 会自动清除。 */
    wd_slot_offline(s_wd_slot_u2t);
    wd_slot_offline(s_wd_slot_t2u);

    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        close_client(i);
    }
    reject_holds_close_all_locked();
    xSemaphoreGive(s_client_mutex);
}

// ---- Accept task ----
static void tcp_accept_task(void *arg)
{
    while (s_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int fd = accept(s_server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (fd < 0) {
            if (s_running) vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* MG24 空闲探测窗口避让（看门狗层 C）。
           无客户端时探针会发 ASH RST 拿存活证据；若此刻放行一个新客户端，
           它的 ASH 握手会和探测的 RST 撞在一起。探测上界是 EZSP 查询超时
           2.5s，这里最多等到窗口结束——TCP 侧只表现为建连慢了不到 3 秒，
           Z2M/ZHA 无感知。绝不能因此丢弃连接：宁可慢，不可断。 */
        for (int i = 0; i < 30 && wd_mg24_probe_active(); i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        // TCP whitelist enforcement
        const tcp_whitelist_config_t *wl = config_mgr_get_whitelist();
        if (wl->enabled && wl->count > 0) {
            uint32_t client_ip = client_addr.sin_addr.s_addr;
            bool allowed = false;
            for (int i = 0; i < wl->count; i++) {
                if (wl->allowed_ips[i] == client_ip) { allowed = true; break; }
            }
            if (!allowed) {
                ESP_LOGW(TAG, "Whitelist: rejecting %s",
                         inet_ntoa(client_addr.sin_addr));
                /* 不 close：关闭必须由对端发起，否则 ZHA 会卡死。见 reject_hold */
                xSemaphoreTake(s_client_mutex, portMAX_DELAY);
                if (s_running) reject_hold_locked(fd);
                else           close(fd);      /* 同下：停机中不再登记 */
                xSemaphoreGive(s_client_mutex);
                continue;
            }
        }

        xSemaphoreTake(s_client_mutex, portMAX_DELAY);

        /* ⚠️ 必须在锁内重新确认还在运行：accept() 返回之后隔着最长 3s 的探测
           避让，而 tcp_server_stop() 对每个任务只 join 3000ms、超时就继续往下
           清表。错过那一轮清理的 fd，会在下次 start() 被"只抹不关"地丢掉
           （s_clients / s_holds 两处初始化都只写 -1）→ fd 永久泄漏，20 个
           lwIP socket 的预算里少一个，只有整机重启才回收。
           stop() 是先置 s_running=false 再取这把锁，所以锁内读到的值可靠。
           ⚠️ 客户端登记那一路是**原有**漏洞，不是本次引入的，一并闭合。 */
        if (!s_running) {
            xSemaphoreGive(s_client_mutex);
            close(fd);
            continue;
        }

        /* Evict stale connections from the same IP (ZHA reconnect after reboot) */
        char new_ip[16];
        inet_ntoa_r(client_addr.sin_addr, new_ip, sizeof(new_ip));
        for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
            if (s_clients[i].fd >= 0 && strcmp(s_clients[i].ip_str, new_ip) == 0) {
                ESP_LOGW(TAG, "Evicting stale client %d from %s (same IP reconnect)",
                         i, new_ip);
                close_client(i);
            }
        }

        int slot = -1;
        for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
            if (s_clients[i].fd < 0) { slot = i; break; }
        }

        if (slot < 0) {
            /* 槽位全满时 s_clients[0] 必然有人。把占着的 IP 打出来：用户同时开了
               ZHA 和 Z2M 时，这一行就是答案。 */
            ESP_LOGW(TAG, "Zigbee 上位机 %s 已连着，拒绝 %s（EZSP 只能有一个上位机）",
                     s_clients[0].ip_str, new_ip);
            reject_hold_locked(fd);   /* 同上：不主动关 */
        } else {
            configure_client_socket(fd);
            s_clients[slot].fd = fd;
            ring_buffer_reset(&s_clients[slot].tx_buf);
            /* 秒，与 rest_api.c 计算连接时长时用的时基一致。
               这里曾经存的是 xTaskGetTickCount()（毫秒 tick），而那边拿秒去减，
               单位不一致 → uint32 下溢 → WebUI 上客户端显示"已连接 136 年"。 */
            s_clients[slot].connected_at = (uint32_t)(esp_timer_get_time() / 1000000);
            s_clients[slot].bytes_rx = 0;
            s_clients[slot].bytes_tx = 0;
            strlcpy(s_clients[slot].ip_str, new_ip, sizeof(s_clients[slot].ip_str));

            bridge_stats_t *st = bridge_stats_get();
            st->tcp_connects++;
            st->active_clients++;

            ESP_LOGI(TAG, "Client %d connected from %s (total: %d)",
                     slot, s_clients[slot].ip_str, st->active_clients);

            esp_event_post(BRIDGE_EVENT, BRIDGE_EVENT_CLIENT_CONNECTED,
                           &slot, sizeof(slot), 0);
        }
        xSemaphoreGive(s_client_mutex);
    }
    xSemaphoreGive(s_task_exit_sem);
    vTaskDelete(NULL);
}

/* Task WDT 的订阅与退订必须成对 —— 这是 2026-08-11 定位的整板重启真因。
 *
 * 本文件的两个桥接任务在 while(s_running) 结束后直接 vTaskDelete(NULL) 自删，
 * 而**从不退订** Task WDT。切到 BRIDGE_MODE_DISABLED（MG24 OTA / 进 bootloader
 * 都会走）就会删掉它们，TWDT 的订阅表里却留着指向**已释放 TCB** 的句柄，
 * 没人再喂，5s 后 CONFIG_ESP_TASK_WDT_PANIC=y 直接 abort 重启整板。
 * 串口现场：任务被删 302530ms → task_wdt 触发 307381ms，间隔 4851ms ≈ 5s 超时；
 * 打印出来的任务名是乱码，因为它顺着悬空句柄去读已释放的 TCB。
 *
 * 为什么长期没被发现：任务重建时也会订阅一次，**若新任务的 TCB 恰好复用了刚
 * 释放的那块内存**，句柄值相同，IDF 返回 ESP_ERR_INVALID_ARG（已订阅），
 * 悬空条目就被新任务喂上了，什么事都没有。而原来两处 add 都没查返回值，
 * 这个状态是静默的。所以它取决于堆的分配运气，表现为间歇性重启。
 */
static void twdt_subscribe(const char *who)
{
    esp_err_t err = esp_task_wdt_add(NULL);
    if (err == ESP_ERR_INVALID_ARG) {
        /* 句柄已在订阅表里 —— 正常情况下不该发生（我们退订是成对的）。
           出现即说明有一条上一轮遗留的悬空条目，必须报出来。 */
        ESP_LOGW(TAG, "%s: Task WDT 已有同句柄订阅（疑似上一轮未退订）", who);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: Task WDT 订阅失败: %s", who, esp_err_to_name(err));
    }
}

static void twdt_unsubscribe(const char *who)
{
    esp_err_t err = esp_task_wdt_delete(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: Task WDT 退订失败: %s —— 本任务自删后会留下悬空订阅，"
                      "5s 后整板重启", who, esp_err_to_name(err));
    }
}

// ---- UART → TCP: read UART, broadcast to all clients ----
static void uart_to_tcp_task(void *arg)
{
    uint8_t buf[256];

    /* 订阅 IDF Task WDT：这两个任务优先级 20/21，一旦死循环会饿死 idle，
       Task WDT 本来就会响；订阅进去的收益是 panic 时的 backtrace 直接指向
       肇事任务，coredump 里能看出是谁（设计文档第四节）。
       ⚠️ 退出前必须 esp_task_wdt_delete()，见本函数末尾。 */
    twdt_subscribe("uart_to_tcp");

    while (s_running) {
        wd_kick(s_wd_slot_u2t);
        esp_task_wdt_reset();
        /* Keep reading during DRAINING — this task IS the drain: switch_mode
           waits for the UART RX buffer to empty, which only happens if we
           keep forwarding. Pause only while actually SWITCHING. */
        bridge_state_t st = ezsp_bridge_get_state();
        if (st == BRIDGE_STATE_SWITCHING) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Skip UART read when no TCP clients — let health_probe use UART exclusively
        bool has_clients = false;
        xSemaphoreTake(s_client_mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
            if (s_clients[i].fd >= 0) { has_clients = true; break; }
        }
        xSemaphoreGive(s_client_mutex);
        if (!has_clients) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int len = uart_bridge_read(buf, sizeof(buf), 20);
        if (len <= 0) {
            /* No new UART data, but a previous send() may have been partial
               (EAGAIN): keep flushing pending ring data so it doesn't stall
               until the NCP next talks. */
            xSemaphoreTake(s_client_mutex, portMAX_DELAY);
            for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
                if (s_clients[i].fd >= 0 && ring_buffer_pending(&s_clients[i].tx_buf) > 0) {
                    flush_client_tx(i);
                }
            }
            xSemaphoreGive(s_client_mutex);
            continue;
        }

        bridge_stats_get()->uart_rx_bytes += len;
        led_activity_tick();   /* Zigbee 灯"有流量"闪一下（T4.4） */

        /* Passive RSTACK detection — scan without modifying data */
        scan_for_rstack(buf, len);

        xSemaphoreTake(s_client_mutex, portMAX_DELAY);

        /* On NCP reset: forward RSTACK to clients, then close connections.
           This gives bellows a clean TCP FIN after seeing the RSTACK frame,
           so it can do a proper reconnect instead of hanging. */
        if (s_ncp_reset_pending) {
            s_ncp_reset_pending = false;
            for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
                if (s_clients[i].fd < 0) continue;
                if (ring_buffer_free(&s_clients[i].tx_buf) >= len) {
                    ring_buffer_write(&s_clients[i].tx_buf, buf, len);
                }
                flush_client_tx(i);
                ESP_LOGW(TAG, "Closing client %d (%s): NCP RSTACK",
                         i, s_clients[i].ip_str);
                close_client(i);
            }
            xSemaphoreGive(s_client_mutex);
            continue;
        }

        for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
            if (s_clients[i].fd < 0) continue;

            if (ring_buffer_free(&s_clients[i].tx_buf) < len) {
                ESP_LOGW(TAG, "Client %d TX buffer full, dropping", i);
                bridge_stats_get()->tcp_client_drops++;
                close_client(i);
                esp_event_post(BRIDGE_EVENT, BRIDGE_EVENT_CLIENT_DROPPED,
                               &i, sizeof(i), 0);
                continue;
            }
            ring_buffer_write(&s_clients[i].tx_buf, buf, len);
        }

        for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
            if (s_clients[i].fd < 0) continue;
            flush_client_tx(i);
        }
        xSemaphoreGive(s_client_mutex);
    }
    /* 退订必须早于 give(s_task_exit_sem)：信号量一放，switch_mode 就认为本任务
       已经收干净、可以起新任务了。若在那之后才退订，中间被抢占就会出现
       "新任务已订阅 + 旧句柄仍在表里"的窗口。 */
    twdt_unsubscribe("uart_to_tcp");
    xSemaphoreGive(s_task_exit_sem);
    vTaskDelete(NULL);
}

// ---- TCP → UART: recv from clients, write to UART ----
static void tcp_to_uart_task(void *arg)
{
    twdt_subscribe("tcp_to_uart");

    uint8_t buf[256];

    while (s_running) {
        wd_kick(s_wd_slot_t2u);
        esp_task_wdt_reset();

        /* 必须在下面那个早返回**之前**：桥不在 RUNNING 时（MG24 升级、模式切换）
           accept 任务照样在拒绝连接，hold 的 fd 得有人回收。 */
        reject_holds_service();

        if (ezsp_bridge_get_state() != BRIDGE_STATE_RUNNING) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        fd_set read_fds;
        FD_ZERO(&read_fds);
        int max_fd = -1;

        xSemaphoreTake(s_client_mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
            if (s_clients[i].fd >= 0) {
                FD_SET(s_clients[i].fd, &read_fds);
                if (s_clients[i].fd > max_fd) max_fd = s_clients[i].fd;
            }
        }
        xSemaphoreGive(s_client_mutex);

        if (max_fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        int ready = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        if (ready <= 0) continue;

        for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
            xSemaphoreTake(s_client_mutex, portMAX_DELAY);
            if (s_clients[i].fd < 0 || !FD_ISSET(s_clients[i].fd, &read_fds)) {
                xSemaphoreGive(s_client_mutex);
                continue;
            }

            /* Socket is non-blocking: recv under the mutex never blocks */
            int n = recv(s_clients[i].fd, buf, sizeof(buf), 0);
            if (n > 0) {
                bridge_stats_get()->tcp_rx_bytes += n;
                s_clients[i].bytes_rx += n;
            } else if (n == 0) {
                ESP_LOGI(TAG, "Client %d disconnected", i);
                close_client(i);
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                /* Spurious select wakeups yield EAGAIN — not a dead client */
                ESP_LOGW(TAG, "Client %d recv error: %d", i, errno);
                close_client(i);
            }
            xSemaphoreGive(s_client_mutex);
            if (n <= 0) continue;

            /* Detect ASH RST from client: 0x1A 0xC0. State survives recv()
               boundaries — the two bytes can arrive in separate segments. */
            for (int j = 0; j < n; j++) {
                if (s_tx_saw_cancel && buf[j] == 0xC0) {
                    s_client_rst_sent = true;
                    s_client_rst_tick = xTaskGetTickCount();
                }
                s_tx_saw_cancel = (buf[j] == 0x1A);
            }

            /* UART write can block up to 1s on CTS backpressure — do it
               OUTSIDE the client mutex so it can't stall the UART→TCP path */
            int written = uart_bridge_write(buf, n, 1000);
            bridge_stats_t *st = bridge_stats_get();
            st->uart_tx_bytes += (written > 0 ? written : 0);
            if (written > 0) led_activity_tick();   /* 同上，发送方向 */
            if (written < n) {
                ESP_LOGW(TAG, "UART write short: %d/%d", written, n);
                st->uart_tx_timeouts++;
            }
        }
    }
    twdt_unsubscribe("tcp_to_uart");   /* 必须早于 give()，理由见 uart_to_tcp */
    xSemaphoreGive(s_task_exit_sem);
    vTaskDelete(NULL);
}

// ---- Close client (must hold s_client_mutex or be called from within lock) ----
static void close_client(int index)
{
    if (s_clients[index].fd >= 0) {
        shutdown(s_clients[index].fd, SHUT_RDWR);
        close(s_clients[index].fd);
        s_clients[index].fd = -1;

        bridge_stats_t *st = bridge_stats_get();
        st->tcp_disconnects++;
        if (st->active_clients > 0) st->active_clients--;

        ESP_LOGI(TAG, "Client %d closed (total: %d)", index, st->active_clients);
        esp_event_post(BRIDGE_EVENT, BRIDGE_EVENT_CLIENT_DISCONNECTED,
                       &index, sizeof(index), 0);
    }
}

// ---- Flush client TX buffer (must hold s_client_mutex) ----
static void flush_client_tx(int index)
{
    uint8_t tmp[256];
    int avail = ring_buffer_peek(&s_clients[index].tx_buf, tmp, sizeof(tmp));
    if (avail <= 0) return;

    int sent = send(s_clients[index].fd, tmp, avail, MSG_DONTWAIT);
    if (sent > 0) {
        ring_buffer_consume(&s_clients[index].tx_buf, sent);
        s_clients[index].bytes_tx += sent;
        bridge_stats_get()->tcp_tx_bytes += sent;
    } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGW(TAG, "Client %d send error: %d, closing", index, errno);
        close_client(index);
    }
}

// ---- Get client info for API ----
void tcp_server_get_clients(tcp_client_info_t *out, int *out_count)
{
    int count = 0;
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (s_clients[i].fd >= 0) {
            out[count].index = i;
            memcpy(out[count].ip_str, s_clients[i].ip_str, sizeof(s_clients[i].ip_str));
            out[count].connected_at = s_clients[i].connected_at;
            out[count].bytes_rx = s_clients[i].bytes_rx;
            out[count].bytes_tx = s_clients[i].bytes_tx;
            count++;
        }
    }
    xSemaphoreGive(s_client_mutex);
    *out_count = count;
}

/* 按 IP 断开。之所以要这一支而不是让调用方"先查快照拿下标、再按下标断开"：
   两步之间槽位可能已被别人复用（MAX_TCP_CLIENTS 只有 2，Z2M/ZHA 掉线后重连很快），
   那样会踢掉一个无辜的连接。这里在同一次持锁内完成匹配与关闭，没有这个窗口。
   同一台主机开两条连接是正常的（重连期新旧并存），所以匹配到几条就关几条。 */
esp_err_t tcp_server_disconnect_by_ip(const char *ip, int *out_closed)
{
    if (!ip || !ip[0]) return ESP_ERR_INVALID_ARG;

    int closed = 0;
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (s_clients[i].fd >= 0 && strcmp(s_clients[i].ip_str, ip) == 0) {
            close_client(i);
            closed++;
        }
    }
    xSemaphoreGive(s_client_mutex);

    if (out_closed) *out_closed = closed;
    return closed > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* ---- 重启前把 EZSP 客户端干净地断开 ----
 *
 * 为什么需要：esp_restart() 会让网络栈直接消失，对端**收不到 FIN**。ZHA 的
 * bellows 那侧于是握着一个"看起来还活着"的 socket，30s 后 _watchdog_feed 去
 * read_counters()，写进死连接、ASH 层超时、内部把 transport 置成 None，随后
 * 另一条协程 await 那个 None ——
 *     TypeError: 'NoneType' object can't be awaited
 * 这个异常类型不在 bellows 的重连恢复路径里，于是它**不重试也不报错，直接不动了**。
 * 2026-08-20 实测：网关 08-19 00:37 重启，ZHA 挣扎到 08-20 15:47 后彻底放弃，
 * 网关侧 tcp_clients 一直是 0，只能重启 HA 才恢复。
 *
 * 干净关闭（shutdown 发 FIN）能让 bellows 走它正常的 connection_lost 路径，
 * HA 据此重载 ZHA 配置项并按退避重连。
 *
 * 与 srp_readv_bye_all() 是同一件事的两半：那边替 Matter/Thread 撤 mDNS 记录，
 * 这边替 Zigbee 送 FIN。都是"重启前主动告知对端"，否则对端握着死连接等超时。
 *
 * 挂 shutdown handler 而不是在每个重启点各调一次：esp_restart() 有十几处调用点
 * （按键、WebUI、两条 OTA、恢复出厂、看门狗自愈…），漏一个就是一次静默退化。 */
/* 关掉客户端之后、真正复位之前要等多久 —— 让 tcpip 任务把 FIN 真正发出去。
 *
 * ⚠️⚠️ 2026-08-22 订正：下面这批数据的**前提是错的** —— 当时以为在发 RST，
 * 其实一直在发 FIN。见 tcp_server_close_all_clients() 里的说明。所以这三行
 * 比较的不是「RST vs 不发」，而是「FIN + 不同等待时长」。数据本身照录，
 * 但不要再用它论证 RST 有效。
 *
 * 实测数据（2026-08-20，每次试验前都把 ZHA 恢复到健康起点）：
 *     不发任何东西                0/2 自愈
 *     关闭 + 200ms                2/3 自愈
 *     关闭 + 2500ms + 二次清扫    2/3 自愈
 * 加长窗口**没有可测量的收益**（n=3 分辨不出差别），却给每次重启都加 2.5 秒，
 * 包括 OTA 的重启。所以砍掉没证据的部分（长窗口、二次清扫），留 500ms。
 * 谁要再加长，先拿出比 n=3 更有力的数据。
 *
 * 剩下那 1/3 的根因 2026-08-21 已查明并解决（不在本文件）：bellows 断线时先
 * force_stop 掉自己的事件循环(uart.py:148)、再通知上层清理(:89)，上层
 * await 的协程排进死循环，unload 永久挂起。**是竞态，所以是约一半概率。**
 * 规避是在用户 HA 的 configuration.yaml 加 zha.zigpy_config.use_thread=false，
 * 实测断电后 43 秒自愈、拔网线 3 秒自愈。详见 docs/guide/ZHA连接不自愈.md。 */
#define CLIENT_CLOSE_SETTLE_MS  500

/* ⚠️ 两条隐式依赖，改 main.c 初始化顺序或加新 shutdown handler 前必读：
 *
 * 1. **执行顺序**。esp_restart() 是**逆序**跑 handler 的
 *    （esp_system.c:42 `for i = SHUTDOWN_HANDLERS_NO-1; i>=0; i--`），
 *    而注册顺序 = main.c 的初始化顺序：
 *        network_mgr(eth_mgr) → ezsp_bridge(本文件) → thread_br(srp)
 *    于是实际执行是   srp bye → 本函数发 FIN → eth PHY 按住复位。
 *    这个顺序**必须保持**：eth_mgr 的 handler 会把 DM9051 按在复位态，
 *    它一旦先跑，以太网就没了，FIN 和 mDNS goodbye 都发不出去。
 *    以太网(PoE)是主部署形态，所以这不是理论问题。
 *    →  ezsp_bridge_init() 必须晚于 network_mgr_init()。
 *
 * 2. **槽位只有 5 个**（esp_system.c:12 `SHUTDOWN_HANDLERS_NO 5`），HA 型现已用满：
 *        esp_timer / esp_wifi_stop / eth_mgr / 本函数 / srp_readv
 *    （直连型不链接 Wi-Fi、也不编本文件（不带桥），只用 esp_timer / eth_mgr / srp_readv /
 *     hub_store 的重启前落盘四个，空一个。）
 *    再加第 6 个会拿到 ESP_ERR_NO_MEM 并**静默失效**。真要加，先把本项目这三个
 *    合并成一个 pre_reboot 钩子，别指望注册成功。 */

/* shutdown handler 签名不带参数，包一层。重启路径要等 FIN 真的发出去。 */
static void shutdown_close_clients(void) { tcp_server_close_all_clients(true); }

void tcp_server_close_all_clients(bool settle)
{
    /* ⚠️ **故意不碰 s_holds**。这个函数存在的全部目的是给桥上的客户端发 FIN，
       而 hold 的对端按定义是被我们**拒掉**的连接 —— 对它发 FIN 正是 reject_hold
       那段实测出来的卡死触发器（bellows 1.0.2 上 11/12 永久失败）。让它随复位
       静默消失，对端走 startup 超时那条干净路径。别"顺手补全"。 */
    int closed = 0;

    /* 带超时取锁：这里跑在关机路径上，拿不到锁也绝不能把重启卡死。
       拿不到就退而求其次只发 shutdown（FIN 由它发出，close 交给复位去做）。 */
    bool locked = (s_client_mutex &&
                   xSemaphoreTake(s_client_mutex, pdMS_TO_TICKS(500)) == pdTRUE);
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        int fd = s_clients[i].fd;
        if (fd < 0) continue;

        /* ⚠️ 这里发的是 **FIN，不是 RST**。2026-08-22 订正。
         *
         * 原来这里有一行 `setsockopt(fd, SOL_SOCKET, SO_LINGER, {1,0})`，注释和
         * 日志都声称"已使其变成 RST"。**那是空操作**，逐条核对过：
         *   · sdkconfig:3653      `# CONFIG_LWIP_SO_LINGER is not set`
         *   · lwipopts.h:1024     于是 `#define LWIP_SO_LINGER 0`
         *   · sockets.c:3466      `case SO_LINGER` 整段在 `#if LWIP_SO_LINGER` 里，
         *                         被编掉，落 default 返回 ENOPROTOOPT（我们没检查）
         * 所以那行已删。删的是死代码，行为一个字节都没变。
         *
         * ⚠️ **不要去打开 CONFIG_LWIP_SO_LINGER**，它买不到东西：
         *   api_msg.c:1030 的 abort 条件是
         *   `(conn->linger >= 0) && (pcb->unsent || pcb->unacked)` ——
         *   空闲的 EZSP 连接两者都空，照样落到普通 close → 还是 FIN。
         *   代价却是全局 lwIP 配置变更 + 全量重编 + 现有 A/B 字节基线作废。
         *   真要 abortive close，得走别的路（比如 lwip_shutdown 之外的手段），
         *   而在那之前先问一句：现在还需要吗？见上方对 1/3 根因的订正 ——
         *   那个"必须发 RST"的动机来自 bellows 的卸载死锁，而它已经在 HA 侧解决。 */
        if (locked) {
            close_client(i);        /* 内部 shutdown+close → FIN */
        } else {
            close(fd);              /* 拿不到锁也要把连接关掉 */
        }
        closed++;
    }
    if (locked) xSemaphoreGive(s_client_mutex);

    if (closed == 0) return;

    /* 不能立刻返回：FIN 由 tcpip 任务实际发送，而且对端的拆解要在网关还活着时
       跑完（见上面 CLIENT_CLOSE_SETTLE_MS 的说明）。shutdown handler 跑在调用方
       任务上下文、调度器还活着，vTaskDelay 是安全的（同 srp_readv_bye_all）。 */
    /* settle=false 目前**没有调用者** —— 唯一那个（「对外地址变了就踢客户端」）
       已于 2026-08-22 撤回，见 network_mgr.c 状态机末尾那段说明。
       参数保留：将来若有「调用方跑在事件循环任务上、不能被拖住半秒」的场景，
       这里是现成的出口。 */
    if (settle) vTaskDelay(pdMS_TO_TICKS(CLIENT_CLOSE_SETTLE_MS));

    ESP_LOGW(TAG, "已强制断开 %d 个 EZSP 客户端（FIN，让 ZHA/Z2M 自己重连）",
             closed);
}

esp_err_t tcp_server_disconnect_client(int index)
{
    if (index < 0 || index >= MAX_TCP_CLIENTS) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    bool had = (s_clients[index].fd >= 0);
    if (had) close_client(index);
    xSemaphoreGive(s_client_mutex);
    /* 空槽位返回 NOT_FOUND，不返回 OK：这个函数此前零调用者，现在要挂到 REST 上，
       "槽位本来就是空的"和"真踢掉了一个"必须分得开，否则端点会对着空槽位回
       200 ok，用户以为踢了、其实什么也没发生。 */
    return had ? ESP_OK : ESP_ERR_NOT_FOUND;
}
