#include "webui.h"
#include "watchdog.h"
#include "network_mgr.h"
#include "ezsp_bridge.h"
#include "json_escape.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <unistd.h>
#include "poe.h"           /* status_update 里的 poe_present */

static const char *TAG = "webui_ws";

typedef struct {
    int  fd;
    bool active;
} ws_client_t;

static ws_client_t     s_clients[MAX_WS_CLIENTS];
static httpd_handle_t  s_server = NULL;
static volatile bool   s_running = false;
static SemaphoreHandle_t s_push_exit_sem;  /* created once, signals task exit */

// ---- Close callback: clean up stale WS fds ----
static void on_socket_close(httpd_handle_t hd, int fd)
{
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) {
            s_clients[i].active = false;
            ESP_LOGI(TAG, "WS client fd=%d closed (slot %d)", fd, i);
        }
    }
    close(fd);  // httpd expects close_fn to actually close the fd
}

// ---- Broadcast to all active WS clients ----
// Sends are marshalled onto the httpd task via httpd_queue_work: writing the
// socket directly from ws_push_task could interleave with httpd's own writes
// (control frames) on the same fd and corrupt the WS stream.
typedef struct {
    httpd_handle_t hd;
    int            fd;
    size_t         len;
    char           data[];
} ws_send_ctx_t;

static void ws_send_work(void *arg)
{
    ws_send_ctx_t *ctx = arg;
    httpd_ws_frame_t frame = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)ctx->data,
        .len     = ctx->len,
    };
    if (httpd_ws_send_frame_async(ctx->hd, ctx->fd, &frame) != ESP_OK) {
        for (int i = 0; i < MAX_WS_CLIENTS; i++) {
            if (s_clients[i].active && s_clients[i].fd == ctx->fd) {
                ESP_LOGW(TAG, "WS client fd=%d send failed, removing", ctx->fd);
                s_clients[i].active = false;
            }
        }
    }
    free(ctx);
}

void ws_broadcast(const char *json, int len)
{
    if (!s_server || len <= 0) return;

    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (!s_clients[i].active) continue;
        ws_send_ctx_t *ctx = malloc(sizeof(*ctx) + len);
        if (!ctx) return;
        ctx->hd  = s_server;
        ctx->fd  = s_clients[i].fd;
        ctx->len = len;
        memcpy(ctx->data, json, len);
        if (httpd_queue_work(s_server, ws_send_work, ctx) != ESP_OK) {
            free(ctx);
        }
    }
}

/* ---- 层 B 心跳 ----
 *
 * httpd 是单任务串行处理的：被滞留连接堵住时任务本身没死，普通心跳测不出来
 * （2026-08-10 实测被堵了两分钟，五层看门狗一层没报）。所以把 kick 通过
 * httpd_queue_work 投递到 httpd **自己的工作队列**里执行——测的正好是
 * "httpd 还能不能取出并执行一个工作项"，恰恰是被堵住时会失败的那件事。 */
static int s_wd_slot_ws   = -1;
static int s_wd_slot_httpd = -1;

static volatile bool s_hb_in_flight;

static void httpd_heartbeat_cb(void *arg)
{
    wd_kick((int)(intptr_t)arg);
    s_hb_in_flight = false;
}

// ---- Periodic push task (dedicated task, not Timer) ----
static void ws_push_task(void *arg)
{
    while (s_running) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        wd_kick(s_wd_slot_ws);

        /* 每 5 次推送（10s）投递一次 httpd 心跳。失败说明队列已满或 httpd
           已停——两者都是它不健康的证据，不补 kick，让 deadline 去判定。 */
        /* 上一条还没被执行就不再投递：httpd 的控制 socket 是个缓冲很小的
           本地 UDP，堵住时继续灌会把它填满，连 httpd_stop() 的关停消息都挤掉
           ——实测这会让 L3 恢复卡死在 httpd_stop() 里（TC-W07 抓到）。
           在途未完成本身就是"httpd 没在干活"的证据，不需要补投。 */
        static uint8_t s_hb_div;
        if (s_wd_slot_httpd >= 0 && !s_hb_in_flight && ++s_hb_div >= 5) {
            s_hb_div = 0;
            s_hb_in_flight = true;
            if (httpd_queue_work(s_server, httpd_heartbeat_cb,
                                 (void *)(intptr_t)s_wd_slot_httpd) != ESP_OK) {
                s_hb_in_flight = false;
            }
        }

        const health_status_t *hs = watchdog_get_status();
        const bridge_stats_t  *bs = ezsp_bridge_get_stats();
        network_status_t ns;
        network_mgr_get_status(&ns);

        char safe_ssid[33];
        json_sanitize_copy(safe_ssid, ns.wifi_ssid, sizeof(safe_ssid));

        char buf[512];
        int n = snprintf(buf, sizeof(buf),
            "{\"type\":\"status_update\",\"data\":{"
            "\"uptime\":%"PRIu32","
            "\"temperature\":%.1f,"
            "\"free_heap\":%"PRIu32","
            "\"wifi_rssi\":%d,"
            "\"wifi_ssid\":\"%s\","
            "\"wifi_connected\":%s,"
            "\"eth_link_up\":%s,"
            "\"active_interface\":\"%s\","
            "\"uart_rx_bytes\":%"PRIu32","
            "\"uart_tx_bytes\":%"PRIu32","
            "\"tcp_clients\":%d,"
            "\"mg24_alive\":%s,"
            "\"poe_present\":%s"
            "}}",
            hs ? hs->uptime_seconds : 0,
            hs ? hs->temperature : 0.0f,
            hs ? hs->free_heap : 0,
            (int)ns.wifi_rssi,
            safe_ssid,
            ns.wifi_connected ? "true" : "false",
            ns.eth_link_up ? "true" : "false",
            ns.active_interface == NET_IF_ETH ? "eth" :
            ns.active_interface == NET_IF_WIFI ? "wifi" : "none",
            bs ? bs->uart_rx_bytes : 0,
            bs ? bs->uart_tx_bytes : 0,
            bs ? (int)bs->active_clients : 0,
            (hs && hs->mg24_alive) ? "true" : "false",
            poe_is_present() ? "true" : "false");

        if (n > 0 && n < (int)sizeof(buf)) {
            ws_broadcast(buf, n);
        } else {
            /* 溢出时**不能静默丢**。这条路径以前只有 `if (放得下) 发`，
               没有 else —— 加一个字段撑爆缓冲的表现是"前端实时数据不再更新"，
               而后端一切正常、日志一片安静，属于最难查的一类。
               下面 webui_ws_push_event() 早就有这条告警了，这里一直缺。
               2026-09-11 实测：本条消息实长 260 字节、理论最坏 349，
               512 还有余量；这条告警是给**下一个加字段的人**留的。 */
            ESP_LOGW(TAG, "status_update 超出 %u 字节缓冲（需 %d），已丢弃 —— "
                          "加过字段吗？该把 buf 调大了", (unsigned)sizeof(buf), n);
        }
    }
    xSemaphoreGive(s_push_exit_sem);
    vTaskDelete(NULL);
}

// ---- Public: push event to all WS clients ----
void webui_ws_push_event(const char *event_type, const char *data_json)
{
    if (!event_type) return;

    char buf[512];
    int n;
    if (data_json) {
        n = snprintf(buf, sizeof(buf),
                     "{\"type\":\"%s\",\"data\":%s}", event_type, data_json);
    } else {
        n = snprintf(buf, sizeof(buf),
                     "{\"type\":\"%s\",\"data\":null}", event_type);
    }

    if (n > 0 && n < (int)sizeof(buf)) {
        ws_broadcast(buf, n);
    } else {
        /* 静默丢弃是最难查的一类故障：前端只是"实时数据不更新"，
           后端一切正常。频谱结果加了个字段就撞过一次上限。 */
        ESP_LOGW(TAG, "WS 事件 %s 超出 %u 字节缓冲（需 %d），已丢弃",
                 event_type, (unsigned)sizeof(buf), n);
    }
}

/* ---- WS client registration (post-handshake) ----

   必须挂在 ws_post_handshake_cb 上，不能放在 uri->handler 里。
   esp_http_server 的 httpd_uri.c 在完成握手后直接 return ESP_OK，并附注释
   "If the request is websocket handshake, then do not call the uri->handler"。
   即握手全部由 httpd 内部处理（客户端确实拿到 101 Switching Protocols），
   uri->handler 只在之后收到 WS **数据帧** 时才被调用。而我们的前端从不主动发帧，
   所以登记代码若放在 handler 里就永远执行不到 —— 广播表恒为空，推送任务每 2 秒
   对着 0 个客户端广播，Dashboard 只在页面加载时靠 API.status() 更新一次，之后
   所有数字静止。实测：浏览器 readyState=OPEN 但 7 秒收到 0 帧，串口无本函数日志。

   需要 CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y（默认 n，已在 sdkconfig.defaults
   显式打开）。回调返回非 ESP_OK 时 httpd 会关闭该连接，所以认证放在这里同样有效。 */
static esp_err_t ws_client_registered(httpd_req_t *req)
{
    /* 跨站守卫，必须在 check_auth **之前**（理由同 webui.h 里 CROSS_ORIGIN_GUARD
       那段：放在后面会让"密码对不对"从返回码里泄漏出去）。
       WebSocket 是这套防护里最需要它的一处：跨站脚本可以直接 new WebSocket()
       到任意源，浏览器**不做 CORS 预检**，而 SameSite 只管 cookie 不管连接本身。
       好消息是浏览器的 WS 握手**一定**带 Origin，所以这里 Origin 检查是实打实的。
       ⚠️ 不能用 CROSS_ORIGIN_GUARD 宏 —— 它 return ESP_OK，而在本回调里 ESP_OK
          的意思正是"接受这个连接"，套上去等于把拒绝写成了放行。而且此刻 101
          已经发出去了（握手由 httpd 内部完成，见上面那段注释），没法再发 403，
          唯一的拒绝方式就是返回 ESP_FAIL 让 httpd 关掉连接。 */
    if (webui_cross_origin_check(req) != 0) {
        ESP_LOGW(TAG, "WS 握手拒绝：跨站或陌生 Host");
        return ESP_FAIL;
    }

    /* Same auth as the REST API: the status feed leaks SSID, uptime,
       client list etc. Returning an error closes the connection. */
    if (!check_auth(req)) {
        ESP_LOGW(TAG, "WS handshake rejected: not authenticated");
        return ESP_FAIL;
    }
    int fd = httpd_req_to_sockfd(req);
    // Check for existing slot with same fd (fd reuse)
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_clients[i].fd == fd) {
            s_clients[i].active = true;
            ESP_LOGI(TAG, "WS client reconnected (fd=%d, slot=%d)", fd, i);
            return ESP_OK;
        }
    }
    // Find free slot
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (!s_clients[i].active) {
            s_clients[i].fd = fd;
            s_clients[i].active = true;
            ESP_LOGI(TAG, "WS client connected (fd=%d, slot=%d)", fd, i);
            return ESP_OK;
        }
    }
    /* Table full: evict slot 0 and actually close the old connection so
       it doesn't linger as an orphaned socket. */
    int old_fd = s_clients[0].fd;
    ESP_LOGW(TAG, "WS clients full, evicting slot 0 (fd=%d)", old_fd);
    if (old_fd >= 0 && old_fd != fd) {
        httpd_sess_trigger_close(s_server, old_fd);
    }
    s_clients[0].fd = fd;
    s_clients[0].active = true;
    return ESP_OK;
}

// ---- WS frame handler (only called for actual data frames, never handshake) ----
static esp_err_t handle_ws(httpd_req_t *req)
{
    // Incoming WS message — currently ignored
    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT };
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) return err;

    if (frame.len > 0) {
        frame.payload = malloc(frame.len + 1);
        if (!frame.payload) return ESP_ERR_NO_MEM;
        err = httpd_ws_recv_frame(req, &frame, frame.len);
        free(frame.payload);
    }

    return ESP_OK;
}

// ---- Registration ----
void register_ws_handler(httpd_handle_t server)
{
    s_server = server;
    memset(s_clients, 0, sizeof(s_clients));

    httpd_uri_t ws_uri = {
        .uri          = "/ws",
        .method       = HTTP_GET,
        .handler      = handle_ws,
        .is_websocket = true,
        /* 客户端登记只能在这里做，见 ws_client_registered() 的注释。 */
        .ws_post_handshake_cb = ws_client_registered,
    };
    httpd_register_uri_handler(server, &ws_uri);
    ESP_LOGI(TAG, "WebSocket handler registered at /ws (max %d clients)", MAX_WS_CLIENTS);
}

void ws_start_push_timer(httpd_handle_t server)
{
    if (!s_push_exit_sem) {
        s_push_exit_sem = xSemaphoreCreateBinary();
        if (!s_push_exit_sem) {
            ESP_LOGE(TAG, "push task sem alloc failed");
            return;
        }
    }
    if (s_running) return;  /* already started */
    s_server = server;
    s_running = true;
    static const wd_task_spec_t ws_spec = {
        .name = "ws_push", .deadline_ms = 10000, .severity = WD_SEV_OBSERVE,
    };
    static const wd_task_spec_t httpd_spec = {
        /* deadline 60s：远大于 10s 的投递周期，容忍一次 OTA 上传占住 httpd */
        .name = "httpd", .deadline_ms = 60000, .severity = WD_SEV_RECOVER,
    };
    s_wd_slot_ws    = wd_register(&ws_spec);
    s_wd_slot_httpd = wd_register(&httpd_spec);

    /* ⚠️ 3072，别再往下调。2026-08-17 曾按「objdump 实测帧 704 字节」的静态估算
       缩到 1536，上板 /api/v1/diag/watchdog 立刻报 stack_low_task=ws_push、
       **余量只剩 16 字节** —— 实际用量约 1520，静态估算漏了被调函数的帧
       （httpd_queue_work / snprintf 那一串）。金丝雀开着，真溢出就是 panic 重启。
       教训：栈只能按 uxTaskGetStackHighWaterMark 的实测值裁，不能按 objdump 估。
       不迁 PSRAM 是因为它 vTaskDelete(NULL) 自删，xTaskCreateWithCaps 的栈得用
       vTaskDeleteWithCaps 才不漏（psram_task.h 规则 2）。 */
    if (xTaskCreate(ws_push_task, "ws_push", 3072, NULL, 8, NULL) != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "WS push task create failed");
        return;
    }
    ESP_LOGI(TAG, "WS push task started (2s interval)");
}

void ws_stop_push_timer(void)
{
    if (!s_running) return;
    /* 我们自己要停它，不是它出故障——先豁免，否则重启期间会被判"任务停转"
       而误报一次 L1（TC-W07 实测到这条误报）。60s 覆盖 httpd 重启全过程。 */
    wd_suspend(s_wd_slot_ws, 60000);
    wd_suspend(s_wd_slot_httpd, 60000);
    s_running = false;
    /* Join: the caller (webui_stop) destroys the httpd handle next — the
       push task must not touch it afterwards. Push loop sleeps 2s max. */
    if (xSemaphoreTake(s_push_exit_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "WS push task did not exit within 5s");
    }
    s_server = NULL;
}

// ---- Socket close callback getter ----
httpd_close_func_t webui_get_close_fn(void)
{
    return on_socket_close;
}

/* 放在文件末尾（HA 型产物一个字节不变：别让前面的行号移动） */
#if CONFIG_BITENGINE_PROFILE_DIRECT
void ws_httpd_progress(void)
{
    if (s_wd_slot_httpd >= 0) wd_kick(s_wd_slot_httpd);   /* httpd 任务里调：这条请求处理完了（webui.h 的说明） */
}
#endif
