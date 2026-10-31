/* muse_pair —— 配对剖面里的配对流程（docs/15_muse_direct_design_v1.md 第五节；策略 C，设计 3.2）。见 include/muse_pair.h。
 *
 * 任务与线程（S2）：
 *  - pair 任务（内部栈）：唯一的状态机。NimBLE host 任务里的回调、按键事件（默认事件循环）、ble_window 的关窗回调、
 *    网页取消都只往它的队列里 0 等待地投递；发送（分片要几秒）也都在这里做，host 任务不阻塞。
 *  - prov 任务（内部栈 8 KB，上游同值：mbedTLS 3.6 的 TLS 握手要这么多）：一次 provision 起一个，做 fetch_vms 验证与
 *    落盘（最坏一分多钟，不能占着 pair 任务）。同一时刻最多一个（s_prov_busy）。
 * 时限（设计 5.1）：等 client_finished 60 s、provisioning 120 s 由上游 link_pairing 自己管；等按键 60 s、确认后等
 * 凭据 120 s 在这里（照上游 app.c 的 confirm_timeout_task / confirmed_idle_timeout_task）；整个窗口由 ble_window 的定时器。
 */
#include "muse_pair.h"

#include <errno.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include "ble_window.h"
#include "button.h"
#include "cJSON.h"
#include "config_mgr.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hub_store.h"
#include "led_ctrl.h"
#include "link_pairing.h"
#include "mbedtls/platform_util.h"
#include "muse_cfg.h"
#include "network_mgr.h"
#include "pair_ble.h"
#include "pair_core.h"
#include "vm_api.h"
#include "watchdog.h"

static const char *TAG = "muse_pair";

#define PAIR_TASK_STACK      6144
#define PAIR_TASK_PRIO       5
#define PROV_TASK_STACK      8192
#define PROV_TASK_PRIO       5
#define PAIR_QUEUE_LEN       12
#define CONFIRM_WAIT_MS      (60 * 1000)     /* 上游 PAIRING_CONFIRM_WINDOW_MS */
#define CONFIRMED_IDLE_MS    (120 * 1000)    /* 上游 PAIRING_CONFIRMED_IDLE_TIMEOUT_MS */
/* 本地期限比 link_pairing 自己的（按键 60 s / 确认后 120 s）早 2 s 到：那时会话密钥还在，pairing_confirm_timeout 加密得出来、
   发得到手机。和上游一样同一时刻到的话，link_pairing 先惰性清掉密钥，手机只看到断开（2026-10-22 审计） */
#define DEADLINE_EARLY_MS    2000
#define PAIR_WD_DEADLINE_MS  30000           /* pair 任务每秒转一圈；卡住 30 s 交给看门狗 */
#define PAIR_GRACE_S         150             /* 绝对期限 = 窗口 + 这么多（够一次 provisioning 120 s 收尾） */
#define AUTH_OK_SETTLE_MS    2000            /* 上游：让 auth_ok 送到手机再重启 */
#define NET_WAIT_MS          30000           /* 配对剖面里再等网的上限（main.c 已经等过 30 s） */
#define PREFLIGHT_TMO_MS     5000
#define RESULT_SHOW_MS       2000            /* 成功绿 / 失败红在 SYS 上停留，然后重启 */
#define FLUSH_TMO_MS         3000

typedef enum {
    ST_IDLE, ST_PREFLIGHT, ST_ADVERTISING, ST_CONNECTED, ST_CONFIRM_WAIT, ST_CONFIRMED, ST_PROVISIONING, ST_FINISHING,
} pair_state_t;

typedef enum {
    EV_CONNECTED, EV_DISCONNECTED, EV_DEVICE_INFO, EV_WIFI_SCAN, EV_CLIENT_FINISHED, EV_BUTTON, EV_PROVISION,
    EV_PROV_DONE, EV_DECRYPT_FAILED, EV_WINDOW_CLOSED, EV_CANCEL,
} pair_ev_kind_t;

typedef struct {
    pair_ev_kind_t kind;
    uint32_t       u32;      /* generation / pair_res_t / blew_close_reason_t */
    pair_prov_t   *prov;     /* EV_PROVISION：所有权随消息转交 */
    uint32_t       gen;      /* EV_PROV_DONE：worker 的 provisioning 会话代号 */
} pair_ev_t;

static QueueHandle_t        s_q;
static volatile pair_state_t s_state = ST_IDLE;
static volatile bool        s_prov_busy;
static volatile int64_t     s_window_open_us;              /* 非 0 = BLE 窗口开过（NimBLE 与 link_pairing 都初始化了） */
static char                 s_sdk_token[MUSE_SDK_TOKEN_LEN + 1];   /* 只放内存，结束前清零；link_pairing 借它的指针 */
static char                 s_node_id[32], s_ble_name[24], s_mac[18], s_device_id[48], s_version[32];
static pair_prov_t * volatile s_commit_prov;                       /* commit 回调没有上下文参数，经它传凭据 */
static pair_res_t           s_last_fail = PAIR_RES_NONE;           /* 窗口里最近一次失败（窗口到时用它作结果） */
static volatile bool        s_committed;                           /* 凭据已落盘：收尾时不管因为什么结束，都按成功记 */
static volatile bool        s_cancel_req;                          /* 取消在开窗前就到了（预检期间）：每步之间看一眼 */
static int                  s_wd_slot = -1;
static uint32_t             s_idle_gen;                            /* 断网失败后停在 provisioning 等重发的那个会话 */

static const char *state_str(pair_state_t st)
{
    switch (st) {
    case ST_IDLE:         return "idle";
    case ST_PREFLIGHT:    return "preflight";
    case ST_ADVERTISING:  return "advertising";
    case ST_CONNECTED:    return "connected";
    case ST_CONFIRM_WAIT: return "confirm_wait";
    case ST_CONFIRMED:    return "confirmed";
    case ST_PROVISIONING: return "provisioning";
    case ST_FINISHING:    return "finishing";
    }
    return "?";
}

static bool post(pair_ev_kind_t kind, uint32_t u32, pair_prov_t *prov)
{
    if (!s_q) return false;
    const pair_ev_t ev = { .kind = kind, .u32 = u32, .prov = prov };
    return xQueueSend(s_q, &ev, 0) == pdTRUE;     /* 0 等待：投递方可能是 host 任务 / 事件循环 / esp_timer */
}

static bool net_online(void)
{
    const net_state_t st = network_mgr_get_state();
    return st == NET_STATE_ETH_ONLINE || st == NET_STATE_WIFI_ONLINE || st == NET_STATE_BOTH_ONLINE;
}

static uint32_t wall_epoch(void)
{
    const time_t now = time(NULL);
    return now > 1700000000 ? (uint32_t)now : 0;   /* 没对时（1970 年）记 0 */
}

/* ---------------------------------------------------------------- 回调（NimBLE host 任务 / 事件循环 / ble_window 任务：只投递） */

static void cb_connected(void)              { (void)post(EV_CONNECTED, 0, NULL); }
static void cb_disconnected(void)           { (void)post(EV_DISCONNECTED, 0, NULL); }
/* host 任务里投递失败（队列 12 条都满，极少）：不在这里发送、不拿 TX 锁 —— 手机会超时重试；client_finished 丢了就作废会话密钥 */
static void cb_wifi_scan(uint32_t gen)      { (void)post(EV_WIFI_SCAN, gen, NULL); }
static void cb_client_finished(uint32_t g)  { if (!post(EV_CLIENT_FINISHED, g, NULL)) (void)link_pairing_reset_session(g); }
static void cb_decrypt_failed(void)         { (void)post(EV_DECRYPT_FAILED, 0, NULL); }

static void cb_device_info(void)
{
    if (!post(EV_DEVICE_INFO, 0, NULL)) pair_ble_device_info_done();   /* 丢了：手机会再问 */
}

static bool cb_provision(pair_prov_t *p)
{
    if (s_prov_busy) return false;                 /* 已经在 provisioning 里：调用方回 operation_in_progress */
    return post(EV_PROVISION, 0, p);
}

/* ble_window 任务里：窗口到时 / BT 出错，BT 还没拆。投给 pair 任务，由它先 pair_ble_shutdown 再 ble_window_close。
   投递失败 ble_window 会在宽限期后强制关，pair 任务的绝对期限再兜一层。 */
static void on_window_expire(blew_close_reason_t why)
{
    (void)post(EV_WINDOW_CLOSED, (uint32_t)why, NULL);
}

/* ble_window 宽限期到了、马上强制拆 BT（ble_window 任务里同步调）：让 pair_ble 此后不再碰 NimBLE */
static void on_window_force_close(void)
{
    pair_ble_mark_dead();
    (void)post(EV_WINDOW_CLOSED, (uint32_t)BLEW_CLOSE_BT_ERROR, NULL);   /* pair 任务若还在，收尾 */
}

static void on_button_confirm(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    (void)post(EV_BUTTON, 0, NULL);
}

/* ---------------------------------------------------------------- 开窗前 */

/* api.muse.ai:443 连得上吗（DNS + TCP，总共 ≤ 约 PREFLIGHT_TMO_MS）。设计 5.3 原本由 M3 的会话任务定期探、/muse/pair 读缓存；
   M2 还没有会话任务，就在配对剖面里开窗之前探一次 —— 连不上就不开窗口，结果记 api_unreachable。 */
static bool preflight_api_reachable(void)
{
    char host[64] = {0};
    const char *u = VM_API_DEFAULT_BASE_URL;
    if (strncmp(u, "https://", 8) == 0) u += 8;
    size_t n = strcspn(u, "/:");
    if (n == 0 || n >= sizeof(host)) return false;
    memcpy(host, u, n);

    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, "443", &hints, &res);
    if (rc != 0 || !res) {
        ESP_LOGW(TAG, "预检：解析 %s 失败（%d）", host, rc);
        return false;
    }
    bool ok = false;
    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s >= 0) {
        const int fl = fcntl(s, F_GETFL, 0);
        (void)fcntl(s, F_SETFL, fl | O_NONBLOCK);
        rc = connect(s, res->ai_addr, res->ai_addrlen);
        if (rc == 0) {
            ok = true;
        } else if (errno == EINPROGRESS) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(s, &wfds);
            struct timeval tv = { .tv_sec = PREFLIGHT_TMO_MS / 1000, .tv_usec = 0 };
            if (select(s + 1, NULL, &wfds, NULL, &tv) == 1) {
                int err = 0;
                socklen_t len = sizeof(err);
                ok = getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0;
            }
        }
        close(s);
    }
    freeaddrinfo(res);
    if (!ok) ESP_LOGW(TAG, "预检：连不上 %s:443", host);
    return ok;
}

/* ---------------------------------------------------------------- provision worker */

static bool commit_cb(void)
{
    /* 在配对锁里同步执行（link_pairing_commit_provisioning 的约定）：只做本地落盘，不碰网络与 BLE */
    pair_prov_t *p = s_commit_prov;
    if (!p) return false;
    const muse_cfg_pairing_t c = {
        .access_token  = p->access_token,
        .refresh_token = p->refresh_token,
        .api_url_v2    = pair_effective_api_url(p->api_url_v2),
        .noise_host    = (p->noise_host && muse_noise_host_valid(p->noise_host)) ? p->noise_host : "",
    };
    const bool ok = muse_cfg_save_pairing(&c, FLUSH_TMO_MS) == ESP_OK;
    if (ok) s_committed = true;
    return ok;
}

static void fail_session(const char *status, uint32_t gen)
{
    pair_ble_send_pairing_status(status, gen);
    vTaskDelay(pdMS_TO_TICKS(100));
    pair_ble_disconnect_pairing_session(gen);
}

static void prov_task(void *arg)
{
    pair_prov_t *p = (pair_prov_t *)arg;
    const uint32_t gen = p->generation;
    pair_res_t res = PAIR_RES_INTERNAL;

    if (!link_pairing_provisioning_session_valid(gen)) {
        res = PAIR_RES_CONFIRM_TIMEOUT;           /* 会话在排队期间过期了 / 被新连接顶掉 */
        goto out;
    }
    pair_ble_send_pairing_status("wifi_connecting", gen);
    if (!net_online()) {
        /* 照上游：停在 provisioning 里让 App 重试（网线插回来再发一次） */
        (void)link_pairing_extend_provisioning_deadline(gen);
        pair_ble_send_pairing_status("wifi_failed", gen);
        res = PAIR_RES_NO_NETWORK;
        goto out;
    }
    pair_ble_send_pairing_status("wifi_connected", gen);

    /* 验证：用新令牌调 fetch_vms（设计 5.5 第 3 步）。最坏约 63.5 s，先把 provisioning 期限续满 120 s */
    (void)link_pairing_extend_provisioning_deadline(gen);
    vm_api_set_base_url(pair_effective_api_url(p->api_url_v2));   /* 配对剖面里只有本任务调 vm_api（UPSTREAM.md 调用方义务） */
    vm_api_set_sdk_token(s_sdk_token);
    vm_info_t *vms = calloc(VM_API_MAX_VMS, sizeof(*vms));
    if (!vms) goto out;
    const int n = vm_api_fetch_vms(p->access_token, vms, VM_API_MAX_VMS);
    if (n > 0) vm_list_free(vms, n);              /* VM 列表与 bearer 只放内存，验证完就扔（设计 4.5） */
    free(vms);
    if (n <= 0) {
        ESP_LOGW(TAG, "fetch_vms 验证不过（%d）—— Muse 拒绝了这个设备（SDK token 无效或已撤销？）", n);
        fail_session("auth_failed", gen);
        res = PAIR_RES_AUTH_FAILED;
        goto out;
    }
    if (!link_pairing_provisioning_session_valid(gen)) {
        res = PAIR_RES_CONFIRM_TIMEOUT;           /* 验证期间手机断了 / 会话过期 */
        goto out;
    }

    s_commit_prov = p;
    const bool committed = link_pairing_commit_provisioning(gen, commit_cb);
    s_commit_prov = NULL;
    if (!committed) {
        fail_session("error_storage", gen);
        res = PAIR_RES_STORAGE_FAILED;
        goto out;
    }
    pair_ble_send_pairing_status("auth_ok", gen);
    vTaskDelay(pdMS_TO_TICKS(AUTH_OK_SETTLE_MS));
    res = PAIR_RES_OK;

out:
    pair_prov_free(p);
    {
        /* 结果不能丢：队列满（极少）就等一下再投 */
        const pair_ev_t ev = { .kind = EV_PROV_DONE, .u32 = (uint32_t)res, .prov = NULL, .gen = gen };
        (void)xQueueSend(s_q, &ev, pdMS_TO_TICKS(2000));
    }
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------- pair 任务里的事件处理 */

static void send_device_info(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    cJSON *root = cJSON_CreateObject();
    char *json = NULL;
    if (root) {
        cJSON_AddStringToObject(root, "type", "device_info");
        cJSON_AddStringToObject(root, "node_id", s_node_id);
        cJSON_AddStringToObject(root, "version", s_version);
        link_pairing_add_device_info(root);
        char build_sha[17] = {0};
        for (int i = 0; desc && i < 8; i++) snprintf(build_sha + i * 2, 3, "%02x", desc->app_elf_sha256[i]);
        cJSON_AddStringToObject(root, "build_sha", build_sha);
        json = cJSON_PrintUnformatted(root);   /* 不发 network_ready（设计 5.3） */
        cJSON_Delete(root);
    }
    if (json) {
        (void)pair_ble_send_chunked(json);
        free(json);
    } else {
        pair_ble_send_status("error_operation_in_progress");
    }
    pair_ble_device_info_done();
}

static int64_t s_deadline_us;      /* CONFIRM_WAIT / CONFIRMED 的时限 */
static uint32_t s_confirm_gen;

static void on_client_finished(uint32_t gen)
{
    if (!link_pairing_session_is_current(gen) || !pair_ble_has_connection() || !link_pairing_confirmation_required()) return;
    /* 照上游：先发加密的提示，再使能按键 —— 提前按下不能让 pairing_confirmed 抢在手机期待的那条记录前面 */
    if (!pair_ble_send_pairing_status("confirm_required", gen)) {
        pair_ble_disconnect_pairing_session(gen);
        return;
    }
    if (!link_pairing_arm_confirmation(gen)) return;
    s_confirm_gen = gen;
    s_deadline_us = esp_timer_get_time() + (int64_t)(CONFIRM_WAIT_MS - DEADLINE_EARLY_MS) * 1000;
    s_state = ST_CONFIRM_WAIT;
    led_ctrl_set_pair_indication(LED_PAIR_CONFIRM);
    ESP_LOGW(TAG, "等按键确认：按住按键 2–10 秒后松开（60 秒内）");
}

static void on_button(void)
{
    if (s_state != ST_CONFIRM_WAIT || !pair_ble_has_connection() || !link_pairing_confirmation_required()) {
        ESP_LOGI(TAG, "按键：现在没有待确认的配对");
        return;
    }
    const uint32_t gen = link_pairing_confirm_active_session();
    if (!gen) return;
    s_confirm_gen = gen;
    s_deadline_us = esp_timer_get_time() + (int64_t)(CONFIRMED_IDLE_MS - DEADLINE_EARLY_MS) * 1000;
    s_state = ST_CONFIRMED;
    led_ctrl_set_pair_indication(LED_PAIR_WINDOW);
    ESP_LOGI(TAG, "按键确认了配对");
    if (!pair_ble_send_pairing_status("pairing_confirmed", gen)) {   /* 上游：加密的 pairing_confirmed 里带 SDK token */
        pair_ble_disconnect_pairing_session(gen);
    }
}

static void check_deadlines(void)
{
    /* CONFIRM_WAIT：等按键；CONFIRMED：确认后等凭据；PROVISIONING 且 worker 已结束：断网失败后等手机重发（上游同样重新计 120 s） */
    const bool idle_prov = (s_state == ST_PROVISIONING && !s_prov_busy);
    if ((s_state != ST_CONFIRM_WAIT && s_state != ST_CONFIRMED && !idle_prov) || s_deadline_us == 0) return;
    if (esp_timer_get_time() < s_deadline_us) return;
    ESP_LOGW(TAG, "%s", s_state == ST_CONFIRM_WAIT ? "60 秒内没有按键确认" : "120 秒内没有收到（新的）凭据");
    s_deadline_us = 0;
    s_last_fail = PAIR_RES_CONFIRM_TIMEOUT;
    const uint32_t gen = idle_prov ? s_idle_gen : s_confirm_gen;
    if (link_pairing_session_is_current(gen)) fail_session("pairing_confirm_timeout", gen);
    s_state = pair_ble_has_connection() ? ST_CONNECTED : ST_ADVERTISING;
    led_ctrl_set_pair_indication(LED_PAIR_WINDOW);
}

static void store_result(pair_res_t r)
{
    uint8_t blob[PAIR_RES_BLOB_LEN];
    pair_res_encode(r, wall_epoch(), blob);
    if (hub_store_put(HUB_STORE_NS_MUSE, "pair_res", 1, blob, sizeof(blob)) != ESP_OK
        || hub_store_flush(FLUSH_TMO_MS) != ESP_OK) {
        ESP_LOGE(TAG, "配对结果没存下来（%s）", pair_res_str(r));
    }
}

/* 结束：关窗口、存结果、亮结果灯、记计划内重启、重启回正常开机。不返回。 */
static void finish_and_restart(pair_res_t r)
{
    wd_suspend(s_wd_slot, 60000);           /* 收尾最长约半分钟（等 TX 锁、关窗、落盘、亮结果灯），别被判停转 */
    s_state = ST_FINISHING;
    ESP_LOGW(TAG, "配对结束：%s —— 关窗口，重启回正常开机", pair_res_str(r));
    (void)esp_event_handler_unregister(BUTTON_EVENT, BUTTON_EVENT_HOLD_CONFIRM, on_button_confirm);
    if (s_window_open_us) {
        /* 窗口没开过（没 token / 没网 / 预检不过 / BT 起不来）就不碰 NimBLE 与 link_pairing：它们还没初始化。
           先停自己的 GATT 收发（pair_ble_shutdown 等在途发送、等在途的 commit —— link_pairing_reset 要拿配对锁），再拆 BT */
        pair_ble_shutdown();
        blew_result_t br;
        (void)ble_window_close(&br);
    }
    if (s_committed && r != PAIR_RES_OK) {
        /* 窗口到时 / 取消的那一刻 worker 正好把凭据落了盘：凭据已经在了，按成功记（不然重启后显示失败、实际已配上） */
        ESP_LOGW(TAG, "收尾时凭据已经落盘 —— 结果按 ok 记（原因本来是 %s）", pair_res_str(r));
        r = PAIR_RES_OK;
    }
    mbedtls_platform_zeroize(s_sdk_token, sizeof(s_sdk_token));   /* link_pairing 已 reset，不再借用它 */
    store_result(r);
    led_ctrl_set_pair_indication(r == PAIR_RES_OK ? LED_PAIR_SUCCESS : LED_PAIR_FAILED);
    vTaskDelay(pdMS_TO_TICKS(RESULT_SHOW_MS));
    wd_forensics_record_planned("muse_pair_exit", true);   /* 一次配对只进一条 NVS 历史（进门那条只记 RTC） */
    esp_restart();
}

static void handle_event(const pair_ev_t *ev)
{
    switch (ev->kind) {
    case EV_CONNECTED:
        if (s_state == ST_ADVERTISING) s_state = ST_CONNECTED;
        break;
    case EV_DISCONNECTED:
        /* 会话已被 pair_ble 作废；provision worker 若还在跑，它会看到会话无效而收手（状态留在 PROVISIONING 等它的结果） */
        if (s_state == ST_CONNECTED || s_state == ST_CONFIRM_WAIT || s_state == ST_CONFIRMED
            || (s_state == ST_PROVISIONING && !s_prov_busy)) {
            if (s_state != ST_CONNECTED) led_ctrl_set_pair_indication(LED_PAIR_WINDOW);
            s_state = ST_ADVERTISING;
            s_deadline_us = 0;
        }
        break;
    case EV_DEVICE_INFO:
        send_device_info();
        break;
    case EV_WIFI_SCAN: {
        char json[160];
        if (pair_wifi_scan_json(net_online(), json, sizeof(json)) == 0
            || !pair_ble_send_encrypted_json(json, ev->u32)) {
            pair_ble_send_status("error_encryption_required");   /* 照上游 send_cached_scan_or_error */
        }
        break;
    }
    case EV_CLIENT_FINISHED:
        on_client_finished(ev->u32);
        break;
    case EV_BUTTON:
        on_button();
        break;
    case EV_PROVISION:
        if (s_prov_busy) {                            /* 两条 provision 前后脚进了队列 */
            const uint32_t gen = ev->prov->generation;
            pair_prov_free(ev->prov);
            pair_ble_send_pairing_status("error_operation_in_progress", gen);
            break;
        }
        s_prov_busy = true;
        s_deadline_us = 0;
        s_state = ST_PROVISIONING;
        if (xTaskCreate(prov_task, "muse_prov", PROV_TASK_STACK, ev->prov, PROV_TASK_PRIO, NULL) != pdPASS) {
            const uint32_t gen = ev->prov->generation;
            pair_prov_free(ev->prov);
            s_prov_busy = false;
            s_state = pair_ble_has_connection() ? ST_CONNECTED : ST_ADVERTISING;
            fail_session("error_operation_in_progress", gen);
        }
        break;
    case EV_PROV_DONE: {
        s_prov_busy = false;
        const pair_res_t r = (pair_res_t)ev->u32;
        if (r == PAIR_RES_OK) finish_and_restart(PAIR_RES_OK);   /* 不返回 */
        s_last_fail = r;                                         /* 失败：窗口照开，手机可以重试（上游同） */
        /* 只在还停在 PROVISIONING 时改状态：手机断开重连、新会话已经走到 CONFIRM_WAIT / CONFIRMED 时，旧 worker 迟到的
           结果不许冲掉它（否则按键被忽略、期限不再计时 —— 2026-10-22 审计 ❌） */
        if (s_state != ST_PROVISIONING) break;
        if (r == PAIR_RES_NO_NETWORK && pair_ble_has_connection() && link_pairing_extend_provisioning_deadline(ev->gen)) {
            /* 停在 provisioning 里等手机重发（上游）：只认 worker 自己那个会话（手机期间重连 / 重新握手就不是它了，
               extend 会返回 false）；link_pairing 的期限此刻刚续满 120 s，本地早 2 s 到，到点发 pairing_confirm_timeout */
            s_idle_gen = ev->gen;
            s_deadline_us = esp_timer_get_time() + (int64_t)(CONFIRMED_IDLE_MS - DEADLINE_EARLY_MS) * 1000;
        } else {
            s_state = pair_ble_has_connection() ? ST_CONNECTED : ST_ADVERTISING;
        }
        break;
    }
    case EV_DECRYPT_FAILED:
        ESP_LOGW(TAG, "解密失败（中间人或版本不匹配？）");
        s_last_fail = PAIR_RES_DECRYPT_FAILED;
        break;
    case EV_WINDOW_CLOSED:
        finish_and_restart((blew_close_reason_t)ev->u32 == BLEW_CLOSE_TIMEOUT
                           ? (s_last_fail != PAIR_RES_NONE ? s_last_fail : PAIR_RES_WINDOW_TIMEOUT)
                           : PAIR_RES_BT_FAILED);
        break;
    case EV_CANCEL:
        finish_and_restart(PAIR_RES_CANCELLED);
        break;
    }
}

static void pair_task(void *arg)
{
    (void)arg;
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        ESP_LOGW(TAG, "esp_read_mac 失败，身份按全零");    /* 不用 esp_efuse_mac_get_default：C5 上越界 */
    }
    pair_identity_format(mac, s_node_id, sizeof(s_node_id), s_ble_name, sizeof(s_ble_name), s_mac, sizeof(s_mac),
                         s_device_id, sizeof(s_device_id));
    const esp_app_desc_t *desc = esp_app_get_description();
    snprintf(s_version, sizeof(s_version), "%s", desc ? desc->version : "unknown");

    const wd_task_spec_t spec = {
        .name = "muse_pair", .deadline_ms = PAIR_WD_DEADLINE_MS, .severity = WD_SEV_RECOVER, .recover = NULL,
    };
    s_wd_slot = wd_register(&spec);     /* 卡住（锁 / 发送）交给看门狗升级：配对剖面里重启 = 回正常开机 */

    s_state = ST_PREFLIGHT;
    /* 先记一条"进行中"：配对剖面里 panic / 断电 / 看门狗重启了，pair_res 不会还显示上一次的结果（可能是 ok） */
    store_result(PAIR_RES_INTERRUPTED);
    if (muse_cfg_get_sdk_token(s_sdk_token, sizeof(s_sdk_token)) != ESP_OK) {
        ESP_LOGE(TAG, "配对剖面里读不到 SDK token");
        finish_and_restart(PAIR_RES_NO_SDK_TOKEN);
    }
    for (int waited = 0; !net_online() && waited < NET_WAIT_MS && !s_cancel_req; waited += 500) {
        wd_kick(s_wd_slot);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (s_cancel_req) finish_and_restart(PAIR_RES_CANCELLED);
    if (!net_online()) finish_and_restart(PAIR_RES_NO_NETWORK);
    wd_suspend(s_wd_slot, 45000);       /* 预检：DNS 不通时 getaddrinfo 要等二三十秒，再加 connect 5 s */
    if (!preflight_api_reachable()) finish_and_restart(PAIR_RES_API_UNREACHABLE);
    wd_kick(s_wd_slot);
    if (s_cancel_req) finish_and_restart(PAIR_RES_CANCELLED);

    link_pairing_init(s_node_id, s_device_id, s_mac, s_version, s_sdk_token);
    const pair_ble_cb_t cbs = {
        .on_client_connected = cb_connected,
        .on_client_disconnected = cb_disconnected,
        .on_get_device_info = cb_device_info,
        .on_wifi_scan = cb_wifi_scan,
        .on_pairing_client_finished = cb_client_finished,
        .on_provision = cb_provision,
        .on_decrypt_failed = cb_decrypt_failed,
    };
    if (pair_ble_init(s_ble_name, &cbs) != ESP_OK) finish_and_restart(PAIR_RES_INTERNAL);
    if (esp_event_handler_register(BUTTON_EVENT, BUTTON_EVENT_HOLD_CONFIRM, on_button_confirm, NULL) != ESP_OK) {
        finish_and_restart(PAIR_RES_INTERNAL);     /* 没有按键确认就配不成，开窗口没意义 */
    }

    const blew_req_t req = {
        .owner = BLEW_OWNER_MUSE_PAIR,
        .role = BLEW_ROLE_PERIPHERAL,
        .max_ms = (uint32_t)CONFIG_BITENGINE_MUSE_PAIR_WINDOW_S * 1000u,
        .svcs = pair_ble_svcs(),
        .device_name = s_ble_name,
        .on_sync = pair_ble_on_sync,
        .on_reset = pair_ble_on_reset,
        .on_expire = on_window_expire,
        .on_force_close = on_window_force_close,
    };
    wd_kick(s_wd_slot);                 /* 开窗最坏 15 s */
    esp_err_t err = ble_window_open(&req);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE 窗口开不起来: %s", esp_err_to_name(err));
        finish_and_restart(PAIR_RES_BT_FAILED);
    }
    s_window_open_us = esp_timer_get_time();
    s_state = ST_ADVERTISING;
    led_ctrl_set_pair_indication(LED_PAIR_WINDOW);
    ESP_LOGW(TAG, "配对窗口已开（%d s）：在 Muse App 里选 %s", CONFIG_BITENGINE_MUSE_PAIR_WINDOW_S, s_ble_name);

    /* 绝对期限：窗口定时器的消息丢了、关窗通知没投进来、owner 宽限期后被强制关窗 —— 任何一环断了，配对剖面都要自己结束 */
    const int64_t hard_deadline_us = s_window_open_us
        + ((int64_t)CONFIG_BITENGINE_MUSE_PAIR_WINDOW_S + PAIR_GRACE_S) * 1000000;
    for (;;) {
        wd_kick(s_wd_slot);
        pair_ev_t ev;
        if (xQueueReceive(s_q, &ev, pdMS_TO_TICKS(1000)) == pdTRUE) handle_event(&ev);
        check_deadlines();
        if (esp_timer_get_time() > hard_deadline_us) {
            ESP_LOGE(TAG, "配对剖面过了绝对期限 —— 收尾");
            finish_and_restart(s_last_fail != PAIR_RES_NONE ? s_last_fail : PAIR_RES_WINDOW_TIMEOUT);
        }
    }
}

/* ---------------------------------------------------------------- API */

esp_err_t muse_pair_boot_run(void)
{
    if (!ble_window_pair_boot_active()) return ESP_ERR_INVALID_STATE;
    if (s_q) return ESP_ERR_INVALID_STATE;
    s_q = xQueueCreate(PAIR_QUEUE_LEN, sizeof(pair_ev_t));
    if (!s_q) return ESP_ERR_NO_MEM;
    /* 配对剖面里短按不重启（2026-10-22 上板：App 说"按一下按钮"，用户短按 = 重启、配对被打断）：确认要按住 2–10 s。在建任务之前设，
       不留"按键任务已经在跑、屏蔽还没设"的空窗（复核）；任务建不起来就撤回 —— 那时短按重启是退回正常开机的路 */
    button_set_restart_inhibit(true);
    if (xTaskCreate(pair_task, "muse_pair", PAIR_TASK_STACK, NULL, PAIR_TASK_PRIO, NULL) != pdPASS) {
        button_set_restart_inhibit(false);
        vQueueDelete(s_q);
        s_q = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t muse_pair_cancel(void)
{
    if (!ble_window_pair_boot_active() || !s_q) return ESP_ERR_INVALID_STATE;
    s_cancel_req = true;                     /* 开窗前（等网 / 预检）也要生效 */
    return post(EV_CANCEL, 0, NULL) ? ESP_OK : ESP_ERR_TIMEOUT;
}

muse_pair_req_t muse_pair_check_request(void)
{
    if (ble_window_pair_boot_active()) return MUSE_PAIR_REQ_IN_PAIR_PROFILE;
    if (wd_safe_mode())                return MUSE_PAIR_REQ_SAFE_MODE;
    if (!config_mgr_has_password())    return MUSE_PAIR_REQ_NO_PASSWORD;
    if (!muse_cfg_sdk_token_set())     return MUSE_PAIR_REQ_NO_SDK_TOKEN;
    if (!muse_cfg_enabled())           return MUSE_PAIR_REQ_DISABLED;
    return MUSE_PAIR_REQ_OK;
}

esp_err_t muse_pair_enter(void)
{
    if (muse_pair_check_request() != MUSE_PAIR_REQ_OK) return ESP_ERR_INVALID_STATE;
    (void)config_mgr_flush(500);              /* 刚改的设置还在防抖里；hubdb 的由 hub_store 的 shutdown handler 落盘 */
    return ble_window_request_isolated(BLEW_OWNER_MUSE_PAIR);   /* 成功不返回 */
}

const char *muse_pair_state_str(void)
{
    return state_str(s_state);
}

void muse_pair_get_status(muse_pair_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->state = state_str(s_state);
    out->pair_boot = ble_window_pair_boot_active();
    out->window_s = CONFIG_BITENGINE_MUSE_PAIR_WINDOW_S;
    const int64_t opened = s_window_open_us;
    if (opened && s_state != ST_FINISHING) {
        const int64_t left = (int64_t)out->window_s * 1000000 - (esp_timer_get_time() - opened);
        out->remaining_s = left > 0 ? (uint32_t)(left / 1000000) : 0;
    }
    if (out->pair_boot) snprintf(out->ble_name, sizeof(out->ble_name), "%s", s_ble_name);
    out->last_result = "none";
    uint8_t blob[PAIR_RES_BLOB_LEN + 8], ver = 0;
    size_t len = sizeof(blob);
    pair_res_t r;
    uint32_t epoch = 0;
    if (hub_store_get(HUB_STORE_NS_MUSE, "pair_res", 1, &ver, blob, &len, 300) == ESP_OK
        && pair_res_decode(blob, len, &r, &epoch)) {
        out->last_result = pair_res_str(r);
        out->last_result_epoch = epoch;
    }
}
