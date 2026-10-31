#include "webui.h"
#include "watchdog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_event.h"
#include "config_mgr.h"
#include "led_ctrl.h"        /* NET 灯的"有流量"白闪，见 uri_match_and_blink */
#include "lwip/sockets.h"      /* getsockname / inet_ntop —— 见 send_error_host */
#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
#include "ble_window.h"          /* 配对剖面只注册一小部分端点 */
#endif

static const char *TAG = "webui";

ESP_EVENT_DEFINE_BASE(WEBUI_EVENT);

static httpd_handle_t s_server = NULL;
/* 只挂 OTBR REST 的第二实例（8081，标准 OTBR 端口）。见 webui_init 里的说明。 */
static httpd_handle_t s_otbr_server = NULL;

// Forward declarations
extern esp_err_t webui_mount_spiffs(void);
extern httpd_close_func_t webui_get_close_fn(void);

// ---- Shared helpers ----

/* ── NET 灯的"有流量"白闪：HTTP 这一路（2026-09-11 补接）──────────────
 *
 * 找一个能覆盖**每个** HTTP 请求的中心点，`uri_match_fn` 是最省事的那个：
 * httpd 匹配路由时必定调它，静态文件、API、OTBR REST 一律经过，不必在几十个
 * handler 里各加一行（那种做法下一个新端点必然忘记加）。
 *
 * ⚠️ 它**每个请求会被调多次**（对每条候选路由各调一次，直到匹配上），
 *    所以绝不能在这里做"每次都置位"的事 —— 节流在
 *    led_ctrl_notify_network_activity() 里，那是四个来源的汇合点。
 * ⚠️ WebSocket 升级之后的推送帧不再经过 URI 匹配，所以**不会**让 NET 灯闪。
 *    这是有意的：WS 状态推送是每 2 秒一次的固定节拍，接上去会变成只要有人开着
 *    网页 NET 灯就一直闪，反而盖掉"真的有人在用"这个信息。 */
static bool uri_match_and_blink(const char *tpl, const char *uri, size_t upto)
{
    led_ctrl_notify_network_activity();
    return httpd_uri_match_wildcard(tpl, uri, upto);
}

esp_err_t send_json_response(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, WEBUI_JSON_CONTENT_TYPE);
    return httpd_resp_sendstr(req, json);
}

esp_err_t send_error(httpd_req_t *req, int status, const char *error_msg)
{
    char status_str[32];
    switch (status) {
    case 400: strlcpy(status_str, "400 Bad Request", sizeof(status_str)); break;
    case 401: strlcpy(status_str, "401 Unauthorized", sizeof(status_str)); break;
    case 404: strlcpy(status_str, "404 Not Found", sizeof(status_str)); break;
    /* 409 早就在用了（handle_mode 的 switch_in_progress），一直落到 default
       发成 "409 Error"；413 是 2026-08-12 接配置端点时新加的。 */
    /* 403/421 是跨站守卫（webui_cross_origin_check）用的，2026-08-23 新增。
       落到 default 会发成 "403 Error"/"421 Error"，reason phrase 不合规。 */
    case 403: strlcpy(status_str, "403 Forbidden", sizeof(status_str)); break;
    case 421: strlcpy(status_str, "421 Misdirected Request", sizeof(status_str)); break;
    case 409: strlcpy(status_str, "409 Conflict", sizeof(status_str)); break;
    case 413: strlcpy(status_str, "413 Payload Too Large", sizeof(status_str)); break;
    case 429: strlcpy(status_str, "429 Too Many Requests", sizeof(status_str)); break;
    case 500: strlcpy(status_str, "500 Internal Server Error", sizeof(status_str)); break;
    case 501: strlcpy(status_str, "501 Not Implemented", sizeof(status_str)); break;
    case 503: strlcpy(status_str, "503 Service Unavailable", sizeof(status_str)); break;
    default:  snprintf(status_str, sizeof(status_str), "%d Error", status); break;
    }

    httpd_resp_set_status(req, status_str);
    httpd_resp_set_type(req, WEBUI_JSON_CONTENT_TYPE);

    char body[128];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", error_msg ? error_msg : "unknown");
    return httpd_resp_sendstr(req, body);
}

/* 421 的响应体必须给出一条**能用的**地址 —— 被挡在门外的用户没法进设置页去改
   任何东西，把出路直接写进错误里，"锁死"就降级成"十秒钟的绕路"。做不做这一步
   决定这个特性是可用还是售后灾难，而前端必须真的把它显示出来。
   ⚠️ 取的是**本次连接的本地地址**，不是 network_mgr_get_status()。后者会调
      三个阻塞 ioctl（network_mgr.h 里明确警告过），而本函数**无需凭据**即可
      被触发，挂在单任务 httpd 上等于白送一条阻塞放大器；:8081 那个实例栈只有
      3584 字节，它带进来的 wifi_sta_list_t 也塞不下。
      getsockname 顺带解决了"双上行时报哪个 IP"：请求从哪个接口进来就报哪个。 */
/* 本次连接的**本地**地址，写成能直接放进 URL 的形态（IPv6 带方括号）。
   为什么不是 network_mgr_get_status()：那会调三个阻塞 ioctl（network_mgr.h 里
   明确警告过），而调用它的两处都挂在单任务 httpd 上，其中一处还无需凭据即可
   触发 —— 等于白送一条阻塞放大器。
   附带好处：双上行时"请求从哪个接口进来就报哪个"，比猜一个更准。
   拿不到就写空串，调用方负责降级。 */
void webui_local_addr(httpd_req_t *req, char *out, size_t out_len)
{
    out[0] = '\0';
    struct sockaddr_in6 sa;                 /* v4/v6 共用存储 */
    socklen_t sl = sizeof(sa);
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0 || getsockname(fd, (struct sockaddr *)&sa, &sl) != 0) return;
    if (sa.sin6_family == AF_INET) {
        struct sockaddr_in *v4 = (struct sockaddr_in *)&sa;
        inet_ntop(AF_INET, &v4->sin_addr, out, out_len);
        return;
    }

    /* ⚠️ 双栈监听时，**普通的 IPv4 连接**在这里也是 AF_INET6，装的是 v4-mapped
       地址（::FFFF:a.b.c.d）。直接 inet_ntop 成 IPv6 会得到
       "[::FFFF:192.168.1.44]" —— 语法上合法、HA 那边多半也能连，但它会被原样
       粘进用户的 HA 配置和工单里，没人认得出那就是 192.168.1.44。
       2026-08-23 生成第一个 OTBR token 时实测撞到，只有上板才会暴露。
       所以这里手工识别 v4-mapped 并还原成点分十进制。 */
    const uint8_t *a = (const uint8_t *)&sa.sin6_addr;
    bool v4mapped = true;
    for (int i = 0; i < 10; i++) if (a[i]) { v4mapped = false; break; }
    if (v4mapped && a[10] == 0xFF && a[11] == 0xFF) {
        snprintf(out, out_len, "%u.%u.%u.%u", a[12], a[13], a[14], a[15]);
        return;
    }
    char t[40];
    inet_ntop(AF_INET6, &sa.sin6_addr, t, sizeof(t));
    snprintf(out, out_len, "[%s]", t);   /* 真 IPv6：URL 里必须带方括号 */
}

esp_err_t send_error_host(httpd_req_t *req)
{
    char ip[48];
    webui_local_addr(req, ip, sizeof(ip));
    char body[192];
    if (ip[0]) {
        snprintf(body, sizeof(body),
                 "{\"error\":\"unknown_host\",\"use\":\"http://%s/\"}", ip);
    } else {
        strlcpy(body, "{\"error\":\"unknown_host\"}", sizeof(body));
    }
    httpd_resp_set_status(req, "421 Misdirected Request");
    httpd_resp_set_type(req, WEBUI_JSON_CONTENT_TYPE);
    return httpd_resp_sendstr(req, body);
}

// ---- Init / Stop ----

esp_err_t webui_init(void)
{
    ESP_LOGI(TAG, "Initializing WebUI");

    // Mount SPIFFS (non-fatal if fails)
    webui_mount_spiffs();

    // Create HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* 上限 = CONFIG_LWIP_MAX_SOCKETS - 3（IDF 内部保留），现为 16-3=13；取 7 即可，
       余量留给 EZSP TCP 服务（监听 1 + 客户端 2）、mDNS、SNTP。

       原值 4 会让 WS 长连接被反复踢掉：每个打开 Dashboard 的浏览器标签占 1 个 WS，
       两个标签就用掉一半，剩下 2 个要同时承接页面资源和 API 轮询；套接字一旦不够，
       lru_purge_enable 就关掉"最久未活动"的连接，而 WS 只出不进、在 LRU 眼里永远
       最闲。实测表现为 Dashboard 每 15-40s 断线重连一次，且两个 WS 在 80ms 内成对
       关闭（对应两个轮询请求同时到达）。 */
    config.max_open_sockets  = 7;
    /* ⚠️ 这个数必须留足余量，不能"刚好够"。
       现象一旦发生极难定位：**整个前端 404（"Nothing matches the given URI"），
       而所有 API 正常** —— 因为静态通配 handler 是最后注册的，槽位用满时被挤掉的
       就是它。

       ⚠️ **这个坑踩过两次，第二次是因为第一次只加了运行时日志、没有构建期判据**：
         · 2026-08-11：设成 80，正好用满，加两个 API 端点就出事。
           当时的修法是调到 120 + 在 static_files.c 加一条 ESP_LOGE。
         · 2026-09-16：120 又被**正好用满**（120/120，零余量），我加了一个
           `/api/v1/diag/ncp_line` 就越界。而那条 ESP_LOGE 只在**串口**里，
           金丝雀只测了 API 没打开过首页，于是 358 和 365 两个版本都带着
           "前端全 404"发布了出去，直到同事烧了新板子才发现。

       所以这次除了加余量，更重要的是 `components/webui/VERIFY.sh` 里那条
       **构建期**判据：把各文件注册的 handler 数加起来与本值比，余量不足就红。
       光靠运行时日志不够 —— 没人会在发版前专门去读串口。

       代价只有 max_uri_handlers × 一个指针，加余量几乎不花钱。 */
#if CONFIG_BITENGINE_PROFILE_DIRECT
    config.max_uri_handlers  = 176;     /* 直连型另有 Muse / hub 端点（muse_api.c、hub_api.c）；check_uri_slots.py 按配置分别核余量 */
#else
    config.max_uri_handlers  = 160;
#endif
    /* ⚠️ 8192，实测过不能再裁。2026-08-17 试过 5120，跑完 tools/test_api.sh
       stack_left 只剩 **800** —— 即真实用量 4320，而 OTA 那条最深的路径还没测到。
       同一天的两次误判都记在这里，因为它们是两个不同的坑：
         · 静态估算（objdump 数栈帧）低估 —— ws_push 就是这么被裁到只剩 16 字节的
         · **空载实测同样低估** —— 刷完机直接读 httpd 是 804 字节用量，
           跑一遍回归就变成 4320。高水位只反映"至今跑过的路径"
       要再动这个数：先跑 test_api.sh + 一次真 OTA，再看 /api/v1/diag/watchdog
       的 httpd.stack_left，按最坏值留一倍余量。 */
    config.stack_size        = 8192;
    config.recv_wait_timeout = 120;  // 120s recv timeout (OTA over weak 5GHz WiFi)
    config.send_wait_timeout = 30;   // 30s send timeout
    config.lru_purge_enable  = true;
    config.uri_match_fn      = uri_match_and_blink;
    config.close_fn          = webui_get_close_fn();

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
    /* Muse 配对剖面（策略 C，docs/15 设计 3.2）：这次开机只起了网络、config、hubdb、LED、按键与 muse_pair。
       别的端点读的模块（Thread / MG24 / MQTT …）都没初始化，一个都不注册；8081 的 OTBR 实例也不起。 */
    if (ble_window_pair_boot_active()) {
        register_auth_handlers(s_server);
        register_muse_handlers(s_server);
        register_pair_profile_handlers(s_server);    /* 精简 /status + 进度页（通配，最后） */
        static bool s_pair_recovery_registered;
        if (!s_pair_recovery_registered) {
            s_pair_recovery_registered = true;
            wd_register_recovery("httpd", webui_httpd_restart);
        }
        ESP_LOGI(TAG, "WebUI ready on port %d (Muse pairing profile)", config.server_port);
        return ESP_OK;
    }
#endif

    // Register routes in order (specific first, wildcard last)
    register_auth_handlers(s_server);
    register_rest_api_handlers(s_server);
    register_ws_handler(s_server);
    register_otbr_handlers(s_server);
    register_factory_handlers(s_server);
#if !CONFIG_BITENGINE_ENABLE_ZHUB
    register_smlight_handlers(s_server);     // 直连型不编 SMLIGHT 兼容层（webui/CMakeLists.txt）
#endif
#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
    register_muse_handlers(s_server);        // /api/v1/muse/…（muse_api.c）
    register_hub_handlers(s_server);         // /api/v1/hub/…、/api/v1/muse/audit（hub_api.c）；配对剖面里不注册
#endif
    register_static_file_handler(s_server);  // wildcard — must be last

    // Start WS periodic push timer
    ws_start_push_timer(s_server);

    /* 登记 L3 恢复入口（幂等：wd_register_recovery 内部只在首次登记时生效，
       重启路径会再次调用 webui_init，这里用 static 保证只登记一次）。 */
    static bool s_recovery_registered;
    if (!s_recovery_registered) {
        s_recovery_registered = true;
        wd_register_recovery("httpd", webui_httpd_restart);
    }

    /* 第二个实例：只挂 OTBR REST，监听 8081。
     *
     * 为什么需要：8081 是 OTBR REST 的**标准端口**，按标准写的工具都往那里找。
     * 我们把 OTBR REST 放在 80（docs/13 的决定，为了 HA 手工填 URL 时不带端口），
     * 结果对标准工具等于隐身 —— matterjs-server 的 Thread 诊断
     * （ThreadDiagnosticsService.DEFAULT_REST_PROBE_PORT = 8081）探不到，只能退回
     * MeshCoP DTLS，而那条路又撞上 Border Agent 不响应的问题（espressif/esp-idf#18963）。
     * 8081 一开，它就走 REST 拿 /diagnostics，完全绕开 DTLS。
     *
     * 80 端口原样保留，HA 的 otbr 集成不受影响。
     *
     * ⚠️ 套接字预算：CONFIG_LWIP_MAX_SOCKETS=20，IDF 内部保留 3 → 可用 17。
     * 主 httpd 7 + 本实例 2 + EZSP(监听1+客户端2) + mDNS + SNTP ≈ 14，留有余量。
     * 改这里的 max_open_sockets 前先看主实例那段注释里的账。
     *
     * ⚠️ ctrl_port 必须与主实例不同，两个 httpd 用同一个控制端口会起不来。 */
    httpd_config_t otbr_cfg = HTTPD_DEFAULT_CONFIG();
    otbr_cfg.server_port      = 8081;
    otbr_cfg.ctrl_port        = config.ctrl_port + 1;
    otbr_cfg.max_open_sockets = 2;      /* 诊断客户端偶发访问，够用 */
    otbr_cfg.max_uri_handlers = 32;     /* OTBR 路由约 25 条 */
    /* 3584 而非 6144：objdump 实测这套路由最深的帧是 send_dataset_json 的 1312 字节。
       2026-08-17 上板确认这个实例**确实在跑**（curl :8081/node/state 回 200、启动日志
       "OTBR REST also on port 8081"），所以这 2560 是实打实省下来的。 */
    otbr_cfg.stack_size       = 3584;
    otbr_cfg.lru_purge_enable = true;
    otbr_cfg.uri_match_fn     = uri_match_and_blink;

    err = httpd_start(&s_otbr_server, &otbr_cfg);
    if (err != ESP_OK) {
        /* 非致命：主 WebUI 与 80 端口上的 OTBR REST 照常工作，只是标准端口没开。 */
        ESP_LOGW(TAG, "OTBR REST on 8081 unavailable: %s —— 按标准端口找我们的工具"
                      "（如 matter.js 的 Thread 诊断）将看不到本网关", esp_err_to_name(err));
        s_otbr_server = NULL;
    } else {
        register_otbr_handlers(s_otbr_server);
        ESP_LOGI(TAG, "OTBR REST also on port %d", otbr_cfg.server_port);
    }

    ESP_LOGI(TAG, "WebUI ready on port %d", config.server_port);
    return ESP_OK;
}

/* L3 恢复入口：httpd 被滞留连接堵死时重启它。
   只重建 HTTP 服务，不碰网络与其它子系统——这就是 P3「最小恢复」。 */
esp_err_t webui_httpd_restart(void)
{
    ESP_LOGW(TAG, "重启 httpd（看门狗 L3）");
    esp_err_t err = webui_stop();
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(500));   /* 让 socket 进入 TIME_WAIT 释放 */
    return webui_init();
}

#if CONFIG_BITENGINE_FAULT_INJECTION
/* 层 H：TC-W07。投递一个会睡 ms 毫秒的工作项到 httpd 的队列——httpd 是单任务
   串行处理的，这一项没做完，后面的请求和心跳投递都排在后面，正是 2026-08-10
   那次"全端点无响应但板子活着"的形态。 */
static void httpd_block_work(void *arg)
{
    uint32_t ms = (uint32_t)(uintptr_t)arg;
    ESP_LOGW(TAG, "注入: httpd 阻塞 %"PRIu32"ms", ms);
    vTaskDelay(pdMS_TO_TICKS(ms));
    ESP_LOGW(TAG, "注入: httpd 阻塞结束");
}

esp_err_t webui_inject_httpd_block(uint32_t ms)
{
    if (!s_server) return ESP_ERR_INVALID_STATE;
    return httpd_queue_work(s_server, httpd_block_work, (void *)(uintptr_t)ms);
}
#endif

esp_err_t webui_stop(void)
{
    if (!s_server) return ESP_OK;

    ws_stop_push_timer();
    httpd_stop(s_server);
    s_server = NULL;

    /* 第二实例同生共死：看门狗 L3 重启 httpd 时若只停主实例，8081 会占着端口，
       下次 httpd_start 直接失败，表现为"重启后标准端口悄悄没了"。 */
    if (s_otbr_server) {
        httpd_stop(s_otbr_server);
        s_otbr_server = NULL;
    }

    ESP_LOGI(TAG, "WebUI stopped");
    return ESP_OK;
}
