/* ble_window —— BLE 窗口的任务、BT 生命周期与超时（docs/15_muse_direct_design_v1.md 3.1、3.2、3.4）。
 *
 * 所有 BT 操作都在 ble_window 自己的任务里串行做：API 调用方投一条消息、在信号量上等结果（有上限，带序号，
 * 超时之后迟到的答复不会被下一次调用误收）。窗口超时的 esp_timer 回调只投一条消息（红线 10）。
 *
 * H1（IDF v6.0.2，nimble_port.c 已读）：
 *  - nimble_port_init() 自己做 esp_bt_controller_init + enable(BLE)，nimble_port_deinit() 自己做 disable + deinit：
 *    不许再手动调控制器 API（重复调用返回 INVALID_STATE；设计 3.4 原文的顺序据此订正）。
 *  - nimble_port_stop() 里两次 BLE_NPL_TIME_FOREVER 永久等待，等的是 host 任务（nimble_port_run）处理它投的事件：
 *    host 任务没起来之前失败只能 nimble_port_deinit()，起来之后才 stop → deinit。stop 若真卡住，看门狗槽位
 *    还在线（BT_DEINIT 期间照样要求 kick），停转会被判出来。
 *  - GATT 服务必须在 nimble_port_init() 与 nimble_port_freertos_init() 之间注册（ble_svc_gap/gatt_init → count_cfg → add_svcs）。
 */
#include <string.h>

#include "ble_window.h"
#include "blew_core.h"
#include "blew_priv.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "watchdog.h"

static const char *TAG = "ble_window";

_Static_assert((int)BLEW_OWNER_MUSE_PAIR == BLEW_CORE_OWNER_MUSE_PAIR, "blew_core 常量与枚举对不上");
_Static_assert((int)BLEW_OWNER_MATTER == BLEW_CORE_OWNER_MATTER, "blew_core 常量与枚举对不上");
_Static_assert((int)BLEW_ROLE_PERIPHERAL == BLEW_CORE_ROLE_PERIPHERAL, "blew_core 常量与枚举对不上");
_Static_assert((int)BLEW_ROLE_CENTRAL == BLEW_CORE_ROLE_CENTRAL, "blew_core 常量与枚举对不上");

#define BLEW_TASK_STACK      4096          /* 设计 3.1：内部栈 4 KB（xTaskCreate，不是 PSRAM 栈） */
#define BLEW_TASK_PRIO       5
#define BLEW_QUEUE_LEN       4
#define BLEW_CALL_TIMEOUT_MS 15000         /* 调用方等 open / close 的上限（BT 起停实测在 1 s 内，留足余量） */
#define BLEW_WD_DEADLINE_MS  20000         /* > BLEW_CALL_TIMEOUT_MS：调用方先超时，看门狗再判停转 */
#define BLEW_IDLE_POLL_MS    1000
/* 内存闸门（3.4）：上游在同一颗芯片上量的 NimBLE 常驻峰值约 35 KiB + 8 KiB 余量；DMA ≥ 4 KiB。G0-4 (d) 实测后改。 */
#define BLEW_GATE_INTERNAL   (43 * 1024)
#define BLEW_GATE_DMA        (4 * 1024)

typedef enum { MSG_OPEN, MSG_CLOSE, MSG_TIMEOUT, MSG_HOST_RESET, MSG_GRACE } blew_msg_kind_t;
typedef struct {
    blew_msg_kind_t kind;
    uint32_t        seq;       /* OPEN / CLOSE 的调用序号，答复带回去 */
} blew_msg_t;

typedef enum { ST_IDLE, ST_BT_INIT, ST_OPEN, ST_BT_DEINIT } blew_state_t;

static QueueHandle_t      s_q;
static SemaphoreHandle_t  s_api_lock;      /* 同一时刻只有一个 open / close 在等答复 */
static SemaphoreHandle_t  s_done;          /* ble_window 任务 → 调用方 */
static volatile uint32_t  s_seq;           /* 调用序号（只在 s_api_lock 里改） */
static volatile uint32_t  s_done_seq;
static volatile esp_err_t s_done_err;
static esp_timer_handle_t s_timer;           /* 窗口时限（max_ms） */
static esp_timer_handle_t s_grace_timer;     /* on_expire 之后的宽限期：独立定时器 + 独立消息，旧的窗口超时消息不会被当成宽限到点 */
static int                s_wd_slot = -1;
static bool               s_started;       /* 任务与对象都建好了 */
static portMUX_TYPE       s_start_mux = portMUX_INITIALIZER_UNLOCKED;
static bool               s_starting;

/* 下面只在 ble_window 任务里写（get_status 只读快照） */
static blew_req_t         s_req;           /* 当前窗口的请求副本 */
static volatile blew_state_t s_state = ST_IDLE;
static bool               s_host_started;  /* nimble_port_freertos_init 之后为真：此后才能 nimble_port_stop */
static bool               s_expiring;      /* 已经 on_expire 了、在等 owner close（宽限定时器到了就强制关） */
static volatile bool      s_bt_stuck;      /* nimble_port_stop 失败：BT 与 host 任务还活着，只能等重启（粘滞） */
static int64_t            s_opened_us;
static uint32_t           s_int_at_open, s_dma_at_open, s_int_min_open;
static volatile uint32_t  s_host_stack_free;   /* NimBLE host 任务栈的高水位（字节，host 任务结束时记；0 = 还没记） */
static volatile int       s_last_err;

static const char *state_str(blew_state_t st)
{
    switch (st) {
    case ST_IDLE:      return "idle";
    case ST_BT_INIT:   return "bt_init";
    case ST_OPEN:      return "open";
    case ST_BT_DEINIT: return "bt_deinit";
    }
    return "?";
}

/* ---------------------------------------------------------------- 回调（都只投递） */

/* esp_timer 回调：只投一条消息，0 等待（红线 10）。队列满说明任务正忙着处理上一条，丢了也会由 owner 的总时限兜底。 */
static void blew_timer_cb(void *arg)
{
    (void)arg;
    const blew_msg_t m = { .kind = MSG_TIMEOUT, .seq = 0 };
    (void)xQueueSend(s_q, &m, 0);
}

static void blew_grace_cb(void *arg)
{
    (void)arg;
    const blew_msg_t m = { .kind = MSG_GRACE, .seq = 0 };
    (void)xQueueSend(s_q, &m, 0);
}

/* NimBLE host 任务里：确保有地址，然后交给 owner 开广播 */
static void blew_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr rc=%d —— 没有可用地址，窗口按 BT 出错关掉", rc);
        const blew_msg_t m = { .kind = MSG_HOST_RESET, .seq = 0 };
        (void)xQueueSend(s_q, &m, 0);
        return;
    }
    if (s_req.on_sync) s_req.on_sync();
}

/* NimBLE host 任务里：host 复位（控制器出错等）—— 通知 owner，窗口交给任务关 */
static void blew_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset reason=%d", reason);
    if (s_req.on_reset) s_req.on_reset(reason);
    const blew_msg_t m = { .kind = MSG_HOST_RESET, .seq = 0 };
    (void)xQueueSend(s_q, &m, 0);
}

static void blew_host_task(void *arg)
{
    (void)arg;
    nimble_port_run();              /* nimble_port_stop() 之后返回 */
    /* ECDH / 解密 / cJSON 都在这个任务里跑（照上游）：记下最紧的时候还剩多少，按实测调 CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE */
    s_host_stack_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    nimble_port_freertos_deinit();
}

/* ---------------------------------------------------------------- 任务里的活 */

static bool blew_mem_gate_ok(uint32_t *internal, uint32_t *dma)
{
    *internal = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    *dma      = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DMA);
    return blew_mem_gate(*internal, *dma, BLEW_GATE_INTERNAL, BLEW_GATE_DMA);
}

/* 返回 ESP_FAIL = host 停不下来：此时不能 deinit（host 任务还阻塞在事件队列上），BT 留着，由调用方重启收场
   （上游重试 3 次后 esp_restart；策略 C 里 owner 关完窗口本来就要重启）。 */
static esp_err_t blew_bt_down(void)
{
    if (s_host_started) {
        int rc = -1;
        for (int attempt = 0; attempt < 3 && rc != 0; attempt++) {
            rc = nimble_port_stop();
            if (rc != 0) {
                ESP_LOGW(TAG, "nimble_port_stop rc=%d（第 %d/3 次）", rc, attempt + 1);
                vTaskDelay(pdMS_TO_TICKS(200));
            }
        }
        if (rc != 0) {
            ESP_LOGE(TAG, "nimble_port_stop 失败 —— 不 deinit（host 任务还在），BT 留给重启收场");
            s_bt_stuck = true;
            return ESP_FAIL;
        }
        s_host_started = false;
    }
    esp_err_t err = nimble_port_deinit();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nimble_port_deinit: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

static esp_err_t blew_do_open(void)
{
    if (s_state != ST_IDLE) return ESP_ERR_INVALID_STATE;   /* API 已经判过，这里再兜一次 */
    s_state = ST_BT_INIT;
    wd_kick(s_wd_slot);

    uint32_t internal = 0, dma = 0;
    if (!blew_mem_gate_ok(&internal, &dma)) {
        ESP_LOGW(TAG, "内存闸门不过：内部空闲 %u（要 %u）、DMA 空闲 %u（要 %u）—— 不开 BT",
                 (unsigned)internal, BLEW_GATE_INTERNAL, (unsigned)dma, BLEW_GATE_DMA);
        s_state = ST_IDLE;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        s_state = ST_IDLE;
        return err;
    }
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_req.svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_req.svcs);
    if (rc == 0) rc = ble_svc_gap_device_name_set(s_req.device_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT / GAP 注册失败 rc=%d", rc);
        (void)blew_bt_down();       /* host 任务还没起：只 deinit */
        s_state = ST_IDLE;
        return ESP_FAIL;
    }
    ble_hs_cfg.sync_cb  = blew_on_sync;
    ble_hs_cfg.reset_cb = blew_on_reset;
    nimble_port_freertos_init(blew_host_task);
    s_host_started = true;

    err = esp_timer_start_once(s_timer, (uint64_t)s_req.max_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "窗口定时器起不来: %s —— 没有时限的窗口不开", esp_err_to_name(err));
        (void)blew_bt_down();
        s_state = ST_IDLE;
        return err;
    }
    s_opened_us    = esp_timer_get_time();
    s_int_at_open  = internal;
    s_dma_at_open  = dma;
    s_int_min_open = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_state = ST_OPEN;
    blew_selfcheck_note_bt_used();      /* 下次正常开机的 Thread 自检据此知道 BLE 真的用过（设计 3.3） */
    ESP_LOGI(TAG, "窗口已开：owner=%d 时限 %u s，开窗前内部空闲 %u、DMA 空闲 %u，BT 起来后内部空闲 %u",
             (int)s_req.owner, (unsigned)(s_req.max_ms / 1000), (unsigned)internal, (unsigned)dma,
             (unsigned)s_int_min_open);
    return ESP_OK;
}

static esp_err_t blew_do_close(void)
{
    if (s_state == ST_IDLE) return ESP_OK;
    if (s_state != ST_OPEN) return ESP_ERR_INVALID_STATE;    /* BT_INIT / BT_DEINIT 只在任务里短暂出现，到不了这里 */
    s_state = ST_BT_DEINIT;
    wd_kick(s_wd_slot);
    (void)esp_timer_stop(s_timer);  /* 已经触发过的话回 INVALID_STATE，无妨 */
    (void)esp_timer_stop(s_grace_timer);
    const esp_err_t err = blew_bt_down();
    for (int i = 0; i < 10 && s_host_stack_free == 0; i++) vTaskDelay(pdMS_TO_TICKS(10));   /* host 任务收尾时才记 */
    ESP_LOGI(TAG, "窗口已关：开了 %u s，期间内部空闲最低 %u，现在 %u；NimBLE host 栈最紧时还剩 %u 字节",
             (unsigned)((esp_timer_get_time() - s_opened_us) / 1000000), (unsigned)s_int_min_open,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)s_host_stack_free);
    s_opened_us = 0;
    s_expiring = false;
    s_state = ST_IDLE;
    return err;
}

/* 看门狗：窗口开着（或 BT 起停中）要按时转圈；空闲时槽位下线，别被当成停转 */
static void blew_wd_tick(void)
{
    static bool online;
    if (s_state == ST_IDLE) {
        if (online) wd_slot_offline(s_wd_slot);   /* 只在开 → 关的那一下下线 */
        online = false;
        return;
    }
    online = true;
    wd_kick(s_wd_slot);
    if (s_state == ST_OPEN) {
        uint32_t now = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (now < s_int_min_open) s_int_min_open = now;
    }
}

static void blew_task(void *arg)
{
    (void)arg;
    for (;;) {
        blew_msg_t m;
        if (xQueueReceive(s_q, &m, pdMS_TO_TICKS(BLEW_IDLE_POLL_MS)) == pdTRUE) {
            switch (m.kind) {
            case MSG_OPEN:
            case MSG_CLOSE: {
                esp_err_t err = (m.kind == MSG_OPEN) ? blew_do_open() : blew_do_close();
                if (err != ESP_OK) s_last_err = err;
                s_done_err = err;
                s_done_seq = m.seq;
                xSemaphoreGive(s_done);
                break;
            }
            case MSG_TIMEOUT:
            case MSG_HOST_RESET:
                /* 窗口已经关了（owner 刚 close 过）/ 已经在宽限期里等 owner：迟到或重复的消息，丢弃 */
                if (s_state != ST_OPEN || s_expiring) break;
                {
                    /* 先通知、不拆 BT：owner 要先停掉自己的 NimBLE 调用再关（见 ble_window.h 的 on_expire） */
                    const blew_close_reason_t why = (m.kind == MSG_TIMEOUT) ? BLEW_CLOSE_TIMEOUT : BLEW_CLOSE_BT_ERROR;
                    ESP_LOGW(TAG, "窗口%s —— 通知 owner 收尾（%d s 内不关就强制关）",
                             why == BLEW_CLOSE_TIMEOUT ? "到时" : "因 BT 出错要关", BLEW_OWNER_GRACE_MS / 1000);
                    s_expiring = true;
                    (void)esp_timer_stop(s_timer);
                    (void)esp_timer_start_once(s_grace_timer, (uint64_t)BLEW_OWNER_GRACE_MS * 1000ULL);
                    /* 在本任务里回调：owner 不能在回调里调 ble_window_close / open（会等自己），只该投递给它自己的任务 */
                    if (s_req.on_expire) s_req.on_expire(why);
                }
                break;
            case MSG_GRACE:
                if (s_state != ST_OPEN || !s_expiring) break;   /* owner 已经关了 */
                ESP_LOGE(TAG, "owner %d s 内没关窗口 —— 强制关", BLEW_OWNER_GRACE_MS / 1000);
                /* 拆 BT 之前让 owner 先断掉对 NimBLE 的一切调用（同步，可阻塞 ≤ 15 s；看门狗期限 20 s，先 kick） */
                wd_kick(s_wd_slot);
                if (s_req.on_force_close) s_req.on_force_close();
                wd_kick(s_wd_slot);
                s_last_err = ESP_ERR_TIMEOUT;                    /* 粘滞：状态里看得出被强制关过 */
                {
                    const esp_err_t ferr = blew_do_close();
                    if (ferr != ESP_OK) s_last_err = ferr;
                }
                break;
            }
        }
        blew_wd_tick();
    }
}

/* ---------------------------------------------------------------- 起任务（第一次 open 时） */

static esp_err_t blew_ensure_started(void)
{
    taskENTER_CRITICAL(&s_start_mux);
    const bool started = s_started;
    const bool mine = !started && !s_starting;
    if (mine) s_starting = true;
    taskEXIT_CRITICAL(&s_start_mux);
    if (started) return ESP_OK;
    if (!mine) return ESP_ERR_INVALID_STATE;   /* 另一个任务正在第一次 open；本组件按单一调用方设计（muse_pair） */

    esp_err_t err = ESP_ERR_NO_MEM;
    s_q        = xQueueCreate(BLEW_QUEUE_LEN, sizeof(blew_msg_t));
    s_api_lock = xSemaphoreCreateMutex();
    s_done     = xSemaphoreCreateBinary();
    if (!s_q || !s_api_lock || !s_done) goto fail;

    const esp_timer_create_args_t targs = {
        .callback        = blew_timer_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "blew_to",
    };
    err = esp_timer_create(&targs, &s_timer);
    if (err != ESP_OK) goto fail;
    const esp_timer_create_args_t gargs = {
        .callback        = blew_grace_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "blew_grace",
    };
    err = esp_timer_create(&gargs, &s_grace_timer);
    if (err != ESP_OK) goto fail;

    const wd_task_spec_t spec = {
        .name        = "ble_window",
        .deadline_ms = BLEW_WD_DEADLINE_MS,
        .severity    = WD_SEV_RECOVER,        /* BT 起停卡死：没有定向恢复手段，交给看门狗升级（配对剖面里重启 = 回正常开机） */
        .recover     = NULL,
    };
    s_wd_slot = wd_register(&spec);
    if (s_wd_slot < 0) {
        ESP_LOGW(TAG, "看门狗槽位满了 —— ble_window 任务卡死时不会被发现");
    }
    if (xTaskCreate(blew_task, "ble_window", BLEW_TASK_STACK, NULL, BLEW_TASK_PRIO, NULL) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    wd_slot_offline(s_wd_slot);
    taskENTER_CRITICAL(&s_start_mux);
    s_started = true;
    s_starting = false;
    taskEXIT_CRITICAL(&s_start_mux);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "ble_window 任务起不来: %s", esp_err_to_name(err));
    if (s_timer) { esp_timer_delete(s_timer); s_timer = NULL; }
    if (s_grace_timer) { esp_timer_delete(s_grace_timer); s_grace_timer = NULL; }
    if (s_done) { vSemaphoreDelete(s_done); s_done = NULL; }
    if (s_api_lock) { vSemaphoreDelete(s_api_lock); s_api_lock = NULL; }
    if (s_q) { vQueueDelete(s_q); s_q = NULL; }
    if (s_wd_slot >= 0) wd_slot_offline(s_wd_slot);   /* 注册表是静态数组、没有注销；下线即可 */
    taskENTER_CRITICAL(&s_start_mux);
    s_starting = false;
    taskEXIT_CRITICAL(&s_start_mux);
    return err;
}

/* 投一条 OPEN / CLOSE，等带同一序号的答复 */
static esp_err_t blew_call(blew_msg_kind_t kind)
{
    if (xSemaphoreTake(s_api_lock, pdMS_TO_TICKS(BLEW_CALL_TIMEOUT_MS)) != pdTRUE) return ESP_ERR_TIMEOUT;
    const uint32_t seq = ++s_seq;
    const blew_msg_t m = { .kind = kind, .seq = seq };
    esp_err_t err = ESP_ERR_TIMEOUT;
    if (xQueueSend(s_q, &m, pdMS_TO_TICKS(1000)) == pdTRUE) {
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(BLEW_CALL_TIMEOUT_MS);
        for (;;) {
            const TickType_t now = xTaskGetTickCount();
            if ((int32_t)(deadline - now) <= 0) break;
            if (xSemaphoreTake(s_done, deadline - now) != pdTRUE) break;
            if (s_done_seq == seq) { err = s_done_err; break; }
            /* 上一次超时的调用迟到的答复：丢掉，接着等自己的 */
        }
    }
    xSemaphoreGive(s_api_lock);
    if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "%s 没在 %d s 内完成", kind == MSG_OPEN ? "open" : "close", BLEW_CALL_TIMEOUT_MS / 1000);
    }
    return err;
}

/* ---------------------------------------------------------------- API */

esp_err_t ble_window_open(const blew_req_t *req)
{
    if (!req) return ESP_ERR_INVALID_ARG;
    const blew_req_verdict_t v = blew_req_check((int)req->owner, (int)req->role, req->max_ms,
                                                s_state != ST_IDLE, ble_window_pair_boot_active());
    switch (v) {
    case BLEW_REQ_OK:            break;
    case BLEW_REQ_BAD_ARG:       return ESP_ERR_INVALID_ARG;
    case BLEW_REQ_BUSY:          return ESP_ERR_INVALID_STATE;
    case BLEW_REQ_NOT_SUPPORTED: return ESP_ERR_NOT_SUPPORTED;
    }
    if (req->role == BLEW_ROLE_PERIPHERAL && (!req->svcs || !req->device_name || !req->device_name[0])) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = blew_ensure_started();
    if (err != ESP_OK) return err;
    if (s_state != ST_IDLE || s_bt_stuck) return ESP_ERR_INVALID_STATE;
    s_req = *req;                  /* 任务空闲时才写，任务只在 OPEN 消息到了之后读 */
    s_expiring = false;
    s_host_stack_free = 0;
    return blew_call(MSG_OPEN);
}

esp_err_t ble_window_close(blew_result_t *out)
{
    esp_err_t err = ESP_OK;
    if (s_started) err = blew_call(MSG_CLOSE);
    if (out) {
        out->ok = (err == ESP_OK);
        out->thread_paused = false;      /* 策略 C：这次开机没起 Thread */
        out->thread_paused_ms = 0;
        out->selfcheck = -1;             /* 策略 C 的自检在重启回正常开机后做（blew_selfcheck.c，设计 3.3） */
    }
    return err;
}

esp_err_t ble_window_scan_begin(void)
{
    return ESP_ERR_NOT_SUPPORTED;        /* 中心角色（Matter 配网扫描）是 P3 */
}

esp_err_t ble_window_scan_end(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_window_resume_thread_early(void)
{
    return ESP_ERR_NOT_SUPPORTED;        /* Matter 时序（设计 9.4），P3 */
}

blew_policy_t ble_window_effective_policy(void)
{
    return BLEW_POLICY_ISOLATED;         /* Kconfig 里眼下只有 C；A / B 等 G0-4 */
}

void ble_window_get_status(blew_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    const blew_state_t st = s_state;
    out->state = s_bt_stuck ? "bt_stuck" : state_str(st);
    out->policy = ble_window_effective_policy();
    out->pair_boot = ble_window_pair_boot_active();
    out->bt_mem_released = blew_bt_mem_released();
    const int64_t opened = s_opened_us;
    out->opened_ms = (st == ST_OPEN && opened) ? (uint32_t)((esp_timer_get_time() - opened) / 1000) : 0;
    out->internal_free_at_open = s_int_at_open;
    out->dma_free_at_open = s_dma_at_open;
    out->internal_min_open = s_int_min_open;
    out->last_err = s_last_err;
    out->host_stack_free = s_host_stack_free;
    blew_res_t rec;
    blew_selfcheck_status(&out->selfcheck, &rec);
    out->sc_before_pm = rec.before_pm;
    out->sc_after_pm = rec.after_pm;
    out->sc_children_before = rec.children_before;
    out->sc_children_after = rec.children_after;
    out->sc_consecutive_degraded = rec.consecutive_degraded;
    out->sc_total = rec.total;
}
