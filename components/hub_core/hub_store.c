/* hub_store —— hubdb 分区的唯一写入者（说明见 include/hub_store.h；docs/15 设计 2.5）。
 *
 * 这一层只做胶水：任务、队列、hubdb 上的 NVS 后端、看门狗、失败点灯。防抖 / 读己之写 / 版本规则 / 重试的
 * 判据全在 hub_store_core.c（不依赖 IDF，test_host 测）。
 *
 * 线程（S2）：对外函数可以从任何任务调，它们只往队列里放消息；碰 flash 的只有 hub_store 任务，它的栈在内部
 * 内存（PSRAM 栈的任务一碰 flash 就挂，psram_task.h 规则 1）。同步请求（get / flush）的消息由调用方在堆上分配：
 * 调用方等超时就把它标成"已放弃"，任务做完看到这个标志自己释放 —— 不会往一个已经返回的调用方栈上写东西。
 */
#include "hub_store.h"
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
#include "matter_ctrl.h"   /* 恢复出厂全清时删 matter 分区的绑定记号 */
#include "hub_matter.h"    /* HUB_MATTER_OPTS_KEY（全清时单独删"允许测试设备"） */
#endif
#include "hub_store_core.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"     /* esp_register_shutdown_handler；别靠传递包含 */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "config_mgr.h"     /* CONFIG_EVENT_NVS_ERROR：沿用"设置没能保存"的 5 s 红闪（手册第三节） */
#include "watchdog.h"

static const char *TAG = "hub_store";

#define HUBDB_PART        "hubdb"      /* partitions_direct.csv（设计 2.4） */
#define HUB_STORE_STACK   3072         /* 内部栈；nvs_set_blob 的栈峰值实测约 0.9 KB（docs/04 / T9.20 记录） */
#define HUB_STORE_PRIO    4
#define HUB_STORE_QLEN    16
#define WD_KICK_MAX_MS    30000        /* 空闲时也至少这么久转一圈，喂看门狗（OBSERVE 60 s） */

typedef enum { MSG_PUT, MSG_ERASE, MSG_GET, MSG_FLUSH, MSG_ERASE_NS } msg_type_t;

typedef struct {
    msg_type_t type;
    char       ns[HSC_NAME_MAX + 1];
    char       key[HSC_NAME_MAX + 1];
    uint8_t    ver;                 /* PUT：格式版本；GET：调用方认识的最高版本 */
    /* 同步请求（GET / FLUSH）用 */
    StaticSemaphore_t done_buf;
    SemaphoreHandle_t done;
    bool       abandoned;           /* 调用方等超时了：任务做完负责释放这条消息 */
    bool       finished;
    int        rc;                  /* HSC_* */
    uint8_t    got_ver;
    size_t     len;                 /* PUT：数据长度；GET：进来是容量、出去是负载长度 */
    uint8_t    data[];              /* PUT：数据；GET：结果 */
} msg_t;

static portMUX_TYPE        s_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t       s_q;
static hsc_t               s_core;           /* 只有 hub_store 任务碰 */
static hub_store_status_t  s_status;         /* 对外快照，s_mux 保护 */
static volatile bool       s_frozen;         /* 恢复出厂擦完了：之后的写一律不收（紧接着就重启，别被 shutdown 的 flush 写回去） */
static volatile bool       s_frozen_hub;     /* 这次恢复出厂也擦 hub（wipe_all） */
static bool                s_started;
static esp_err_t           s_start_err = ESP_ERR_INVALID_STATE;
static int                 s_wd_slot = -1;

/* ---------------- 内存：等写的数据放 PSRAM（内部内存留给栈和 DMA） ---------------- */
static void *ps_alloc(size_t n)
{
    void *p = heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n ? n : 1);
}
static void ps_free(void *p) { free(p); }

/* ---------------- 后端：hubdb 分区上的 NVS（只在 hub_store 任务里调） ---------------- */
static int be_get(void *ctx, const char *ns, const char *key, uint8_t *buf, size_t cap, size_t *len)
{
    (void)ctx;
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(HUBDB_PART, ns, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return HSC_ENOENT;          /* 命名空间还没建过 */
    if (err != ESP_OK) return HSC_EIO;
    size_t n = buf ? cap : 0;
    err = nvs_get_blob(h, key, buf, &n);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return HSC_ENOENT;
    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        *len = n;
        return HSC_ETOOBIG;
    }
    if (err != ESP_OK) return HSC_EIO;
    *len = n;
    return HSC_OK;
}

static int be_set(void *ctx, const char *ns, const char *key, const uint8_t *buf, size_t len)
{
    (void)ctx;
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(HUBDB_PART, ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open %s/%s failed: %s", ns, key, esp_err_to_name(err));
        return HSC_EIO;
    }
    err = nvs_set_blob(h, key, buf, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "write %s/%s (%u bytes) failed: %s", ns, key, (unsigned)len, esp_err_to_name(err));
        return HSC_EIO;
    }
    return HSC_OK;
}

static int be_erase(void *ctx, const char *ns, const char *key)
{
    (void)ctx;
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(HUBDB_PART, ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return HSC_EIO;
    err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;               /* 本来就没有 */
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK ? HSC_OK : HSC_EIO;
}

/* 整个命名空间擦掉（恢复出厂，设计第十二节）。命名空间从没建过 = 本来就空。 */
static int be_erase_ns(const char *ns)
{
    nvs_handle_t h;
    /* 先只读探一下：READWRITE 打开不存在的命名空间会把它建出来（hubdb 满了时还会因此报错） */
    esp_err_t err = nvs_open_from_partition(HUBDB_PART, ns, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return HSC_OK;              /* 从没建过 = 本来就空 */
    if (err == ESP_OK) nvs_close(h);
    err = nvs_open_from_partition(HUBDB_PART, ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return HSC_EIO;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK ? HSC_OK : HSC_EIO;
}

/* ---------------- 任务 ---------------- */
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static esp_err_t to_esp(int rc)
{
    switch (rc) {
    case HSC_OK:      return ESP_OK;
    case HSC_ENOENT:  return ESP_ERR_NOT_FOUND;
    case HSC_EINVAL:  return ESP_ERR_INVALID_ARG;
    case HSC_ENOMEM:  return ESP_ERR_NO_MEM;
    case HSC_ENEWER:  return ESP_ERR_INVALID_VERSION;
    case HSC_ETOOBIG: return ESP_ERR_INVALID_SIZE;
    default:          return ESP_FAIL;
    }
}

static void publish_status(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_status.pending       = hsc_pending_count(&s_core);
    s_status.writes        = s_core.stats.writes;
    s_status.coalesced     = s_core.stats.coalesced;
    s_status.retries       = s_core.stats.retries;
    s_status.failures      = s_core.stats.failures;
    s_status.refused_newer = s_core.stats.refused_newer;
    s_status.last_err      = s_core.stats.last_err;
    taskEXIT_CRITICAL(&s_mux);
}

/* 同步请求的消息：信号量是放在消息里的静态对象，释放内存前先从内核摘掉 */
static void free_sync_msg(msg_t *m)
{
    if (m->done) vSemaphoreDelete(m->done);
    free(m);
}

/* 同步请求做完：调用方还在等就叫醒它，已经放弃就由这里释放 */
static void finish(msg_t *m)
{
    bool free_it;
    taskENTER_CRITICAL(&s_mux);
    free_it = m->abandoned;
    m->finished = true;
    taskEXIT_CRITICAL(&s_mux);
    if (free_it) free_sync_msg(m);
    else xSemaphoreGive(m->done);
}

static void handle(msg_t *m)
{
    uint32_t t = now_ms();
    switch (m->type) {
    case MSG_PUT: {
        /* 投递时还没冻、排到这里时已擦完（恢复出厂与改名并发）：别把擦掉的又写回去（M5 第二步审计）。只丢要擦的命名空间 ——
           zhub 是保留的，flush 期间排进来的设备记录照常收、由关机钩子落盘（修复复核） */
        if (s_frozen && (!strcmp(m->ns, HUB_STORE_NS_MUSE) ||
                         (s_frozen_hub && (!strcmp(m->ns, HUB_STORE_NS_HUB) || !strcmp(m->ns, HUB_STORE_NS_MCTRL))))) {
            ESP_LOGW(TAG, "put %s/%s dropped: factory reset in progress", m->ns, m->key);
            free(m);
            break;
        }
        int rc = hsc_put(&s_core, m->ns, m->key, m->ver, m->data, m->len, t);
        if (rc != HSC_OK) ESP_LOGE(TAG, "put %s/%s rejected: %d", m->ns, m->key, rc);
        free(m);
        break;
    }
    case MSG_ERASE: {
        int rc = hsc_erase(&s_core, m->ns, m->key, t);
        if (rc != HSC_OK) ESP_LOGE(TAG, "erase %s/%s rejected: %d", m->ns, m->key, rc);
        free(m);
        break;
    }
    case MSG_GET: {
        size_t len = m->len;
        m->rc = hsc_get(&s_core, m->ns, m->key, m->ver, &m->got_ver, m->data, &len);
        m->len = len;
        /* 规则 4：读到不认识的新版本（固件被降级了）当作没有，但要留痕 —— 否则现场看到的只是"Muse 凭据没了" */
        if (m->rc == HSC_ENEWER) {
            ESP_LOGW(TAG, "%s/%s was written by a newer format (this firmware reads up to v%u) — treated as absent, kept as is",
                     m->ns, m->key, m->ver);
        }
        finish(m);
        break;
    }
    case MSG_FLUSH: {
        uint32_t f0 = s_core.stats.failures;
        hsc_run(&s_core, t, true);
        /* 还在等的（失败了、还会重试的）也算没写完：再跑满重试次数 */
        for (int i = 0; i < HSC_MAX_TRIES && hsc_pending_count(&s_core) > 0; i++) {
            vTaskDelay(pdMS_TO_TICKS(HSC_RETRY_MS));
            hsc_run(&s_core, now_ms(), true);
        }
        m->rc = (s_core.stats.failures != f0 || hsc_pending_count(&s_core) > 0) ? HSC_EIO : HSC_OK;
        finish(m);
        break;
    }
    case MSG_ERASE_NS: {
        /* 调用方已先 flush；flush 之后才进来的、或重试中的该命名空间的写，先丢掉 —— 不然擦完又被写回去 */
        const int dropped = hsc_drop_ns(&s_core, m->ns);
        if (dropped) ESP_LOGW(TAG, "erase namespace %s: dropped %d pending write(s)", m->ns, dropped);
        m->rc = be_erase_ns(m->ns);
        if (m->rc != HSC_OK) ESP_LOGE(TAG, "erase namespace %s failed: %d", m->ns, m->rc);
        finish(m);
        break;
    }
    }
}

static void hub_store_task(void *arg)
{
    (void)arg;
    uint32_t failures_seen = 0, refused_seen = 0;
    for (;;) {
        wd_kick(s_wd_slot);
        uint32_t wait = hsc_next_due_in(&s_core, now_ms());
        if (wait > WD_KICK_MAX_MS) wait = WD_KICK_MAX_MS;
        msg_t *m = NULL;
        if (xQueueReceive(s_q, &m, pdMS_TO_TICKS(wait)) == pdTRUE && m) handle(m);
        hsc_run(&s_core, now_ms(), false);
        /* 规则 3：重试用尽 → 5 s 红闪 + 日志。看计数而不是 hsc_run 的返回值：表满腾格时放弃的也要算上 */
        if (s_core.stats.failures != failures_seen) {
            failures_seen = s_core.stats.failures;
            ESP_LOGE(TAG, "hubdb write failed %d times in a row, giving up (last error %d)",
                     HSC_MAX_TRIES, s_core.stats.last_err);
            esp_event_post(CONFIG_EVENT, CONFIG_EVENT_NVS_ERROR, NULL, 0, 0);
        }
        /* 规则 4：已存的版本更新、这次不覆盖 —— 不是故障（留给再升级回去时用），但要留痕 */
        if (s_core.stats.refused_newer != refused_seen) {
            refused_seen = s_core.stats.refused_newer;
            ESP_LOGW(TAG, "not overwriting %s/%s: stored v%u is newer than v%u (firmware downgraded?)",
                     s_core.stats.last_refused_ns, s_core.stats.last_refused_key,
                     s_core.stats.last_refused_stored, s_core.stats.last_refused_ver);
        }
        publish_status();
    }
}

/* 重启前把还在防抖里的写落盘。esp_restart() 的调用点有十几处（设置页重启、两条 OTA、看门狗自愈、回滚…），
   挂 shutdown handler 一处覆盖，不用每个调用点各补一次 flush（2026-10-21 上板时发现设置页重启只 flush 了 config_mgr）。
   handler 跑在调 esp_restart 的那个任务里、调度器还在跑：flush 只投队列等答复，写 flash 的仍是本任务；最多等 1 s，
   本任务卡死也不拖住重启。panic 不走这里（那时什么都救不了）。
   ⚠️ 槽位全系统只有 5 个（IDF esp_system.c 的 SHUTDOWN_HANDLERS_NO）。HA 型已用满；直连型不链接 Wi-Fi、不带桥，
   esp_wifi_stop 与 tcp_server 的两个都空出来，用了这里一个、还剩一个（见 ezsp_bridge/tcp_server.c 那段槽位说明）；
   登记失败在状态里报、打 E 级日志。
   逆序执行：本 handler 最后登记、最先跑，那时以太网等都还在（flush 也用不着它们）。 */
static void hub_store_on_shutdown(void)
{
    if (s_status.ready) (void)hub_store_flush(1000);
}

/* ---------------- 对外 ---------------- */
static esp_err_t hub_store_start_impl(void);
static volatile bool s_start_done;

/* 可重入：正常开机里 webui 比主任务的 hub_store_start 早几秒起，这时网页触发恢复出厂会并发进来（复核 2026-10-22）。
   后来的一方等先来的做完（≤ 5 s），拿同一个结果。 */
esp_err_t hub_store_start(void)
{
    taskENTER_CRITICAL(&s_mux);
    const bool first = !s_started;
    s_started = true;
    taskEXIT_CRITICAL(&s_mux);
    if (!first) {
        for (int i = 0; i < 50 && !s_start_done; i++) vTaskDelay(pdMS_TO_TICKS(100));
        return s_start_done ? s_start_err : ESP_ERR_TIMEOUT;
    }
    s_start_err = hub_store_start_impl();
    s_start_done = true;
    return s_start_err;
}

static esp_err_t hub_store_start_impl(void)
{

    /* 规则 2：只动 hubdb。坏了 / 版本不认识就只擦它，记日志、状态里置位；主 nvs 一个字节都不碰。 */
    esp_err_t err = nvs_flash_init_partition(HUBDB_PART);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "hubdb unusable (%s) — erasing hubdb only and starting empty", esp_err_to_name(err));
        err = nvs_flash_erase_partition(HUBDB_PART);
        if (err == ESP_OK) err = nvs_flash_init_partition(HUBDB_PART);
        if (err == ESP_OK) s_status.reset_at_init = true;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hubdb init failed: %s — hub_store disabled", esp_err_to_name(err));
        s_status.init_err = err;
        s_start_err = err;
        return err;
    }

    hsc_backend_t be = { .ctx = NULL, .get = be_get, .set = be_set, .erase = be_erase };
    hsc_init(&s_core, &be, ps_alloc, ps_free);

    s_q = xQueueCreate(HUB_STORE_QLEN, sizeof(msg_t *));
    if (!s_q) {
        s_status.init_err = ESP_ERR_NO_MEM;
        s_start_err = ESP_ERR_NO_MEM;
        return s_start_err;
    }
    static const wd_task_spec_t wd_spec = {
        .name = "hub_store", .deadline_ms = 60000, .severity = WD_SEV_OBSERVE,
    };
    s_wd_slot = wd_register(&wd_spec);
    if (xTaskCreate(hub_store_task, "hub_store", HUB_STORE_STACK, NULL, HUB_STORE_PRIO, NULL) != pdPASS) {
        if (s_wd_slot >= 0) wd_slot_offline(s_wd_slot);
        vQueueDelete(s_q);
        s_q = NULL;
        s_status.init_err = ESP_ERR_NO_MEM;
        s_start_err = ESP_ERR_NO_MEM;
        return s_start_err;
    }
    s_status.ready = true;
    s_start_err = ESP_OK;
    esp_err_t reg = esp_register_shutdown_handler(hub_store_on_shutdown);
    s_status.flush_on_restart = (reg == ESP_OK);
    if (reg != ESP_OK) {
        ESP_LOGE(TAG, "pre-restart flush hook not registered (%s) — writes still debouncing at a restart will be lost",
                 esp_err_to_name(reg));
    }
    ESP_LOGI(TAG, "hubdb ready%s", s_status.reset_at_init ? " (was reset at init)" : "");
    return ESP_OK;
}

static msg_t *new_msg(msg_type_t type, const char *ns, const char *key, size_t extra)
{
    if (!hsc_name_ok(ns) || !hsc_name_ok(key)) return NULL;
    msg_t *m = ps_alloc(sizeof(msg_t) + extra);
    if (!m) return NULL;
    memset(m, 0, sizeof(msg_t));
    m->type = type;
    strlcpy(m->ns, ns, sizeof(m->ns));
    strlcpy(m->key, key, sizeof(m->key));
    return m;
}

static esp_err_t post_wait(msg_t *m, uint32_t wait_ms)
{
    esp_err_t err = ESP_OK;
    if (!s_q || !s_status.ready) err = ESP_ERR_INVALID_STATE;
    else if (xQueueSend(s_q, &m, pdMS_TO_TICKS(wait_ms)) != pdTRUE) err = ESP_ERR_TIMEOUT;
    if (err != ESP_OK) free_sync_msg(m);   /* 异步消息 done 为 NULL，等同 free */
    return err;
}

static esp_err_t post(msg_t *m) { return post_wait(m, 1000); }

/* 同步请求：投递、等，超时就标"已放弃"（任务做完会释放）。返回时 *out 是还归调用方所有的消息（要 free），或 NULL。 */
static esp_err_t post_and_wait(msg_t *m, uint32_t timeout_ms, msg_t **out)
{
    *out = NULL;
    m->done = xSemaphoreCreateBinaryStatic(&m->done_buf);
    esp_err_t err = post(m);                     /* 失败时 post 已释放 */
    if (err != ESP_OK) return err;
    if (xSemaphoreTake(m->done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        *out = m;
        return ESP_OK;
    }
    bool finished;
    taskENTER_CRITICAL(&s_mux);
    finished = m->finished;
    if (!finished) m->abandoned = true;
    taskEXIT_CRITICAL(&s_mux);
    if (!finished) return ESP_ERR_TIMEOUT;       /* 任务做完会自己释放 */
    /* 刚好在超时那一刻做完：finished 已置、但任务可能还没走到 xSemaphoreGive（finish() 出临界区之后被抢占）。
       这里必须等到那次 give 再释放消息，否则任务会对已释放的内存 give（审计 2026-10-21 ❌）。
       有界：finished 且没被放弃，任务接下来唯一的动作就是 give。 */
    xSemaphoreTake(m->done, portMAX_DELAY);
    *out = m;
    return ESP_OK;
}

esp_err_t hub_store_put(const char *ns, const char *key, uint8_t ver, const void *data, size_t len)
{
    return hub_store_put_wait(ns, key, ver, data, len, 1000);
}

esp_err_t hub_store_put_wait(const char *ns, const char *key, uint8_t ver, const void *data, size_t len, uint32_t wait_ms)
{
    if (ver == 0 || len > HSC_BLOB_MAX || (len && !data)) return ESP_ERR_INVALID_ARG;
    if (s_frozen) return ESP_ERR_INVALID_STATE;          /* 恢复出厂擦完了，马上重启 */
    msg_t *m = new_msg(MSG_PUT, ns, key, len);
    if (!m) return hsc_name_ok(ns) && hsc_name_ok(key) ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
    m->ver = ver;
    m->len = len;
    if (len) memcpy(m->data, data, len);
    return post_wait(m, wait_ms);
}

esp_err_t hub_store_erase(const char *ns, const char *key)
{
    msg_t *m = new_msg(MSG_ERASE, ns, key, 0);
    if (!m) return hsc_name_ok(ns) && hsc_name_ok(key) ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
    return post(m);
}

esp_err_t hub_store_get(const char *ns, const char *key, uint8_t max_ver, uint8_t *ver,
                        void *buf, size_t *len_inout, uint32_t timeout_ms)
{
    if (!ver || !len_inout || (*len_inout && !buf) || *len_inout > HSC_BLOB_MAX) return ESP_ERR_INVALID_ARG;
    msg_t *m = new_msg(MSG_GET, ns, key, *len_inout);
    if (!m) return hsc_name_ok(ns) && hsc_name_ok(key) ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
    m->ver = max_ver;
    m->len = *len_inout;
    msg_t *r;
    esp_err_t err = post_and_wait(m, timeout_ms, &r);
    if (err != ESP_OK) return err;
    err = to_esp(r->rc);
    if (r->rc == HSC_OK) {
        *ver = r->got_ver;
        memcpy(buf, r->data, r->len);
        *len_inout = r->len;
    } else if (r->rc == HSC_ETOOBIG) {
        *len_inout = r->len;
    }
    free_sync_msg(r);
    return err;
}

esp_err_t hub_store_flush(uint32_t timeout_ms)
{
    msg_t *m = ps_alloc(sizeof(msg_t));
    if (!m) return ESP_ERR_NO_MEM;
    memset(m, 0, sizeof(msg_t));
    m->type = MSG_FLUSH;
    msg_t *r;
    esp_err_t err = post_and_wait(m, timeout_ms, &r);
    if (err != ESP_OK) return err;
    err = to_esp(r->rc);
    free_sync_msg(r);
    return err;
}

esp_err_t hub_store_erase_ns(const char *ns, uint32_t timeout_ms)
{
    if (!hsc_name_ok(ns)) return ESP_ERR_INVALID_ARG;
    msg_t *m = new_msg(MSG_ERASE_NS, ns, "x", 0);       /* key 不用，new_msg 要求合法名字 */
    if (!m) return ESP_ERR_NO_MEM;
    msg_t *r;
    esp_err_t err = post_and_wait(m, timeout_ms, &r);
    if (err != ESP_OK) return err;
    err = to_esp(r->rc);
    free_sync_msg(r);
    return err;
}

/* 恢复出厂（设计第十二节）：两种都清 muse（含 Muse 秘密），全清时再清 hub（设备元数据）；zhub（Zigbee 设备库）两种都保留
   —— Zigbee 网络在 MG24 里、恢复出厂碰不到它，设备库要跟着它。mctrl / matter 分区等 P3。
   安全模式下 hub_store 没起来：这条路径自己起（不起就清不掉 Muse 令牌，设备转手就带着别人的凭据）。 */
esp_err_t hub_store_factory_reset(bool wipe_all)
{
    esp_err_t err = hub_store_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "factory reset: hubdb unavailable (%s) —— Muse 凭据没清", esp_err_to_name(err));
        return err;
    }
    (void)hub_store_flush(2000);                         /* 先把要保留的（zhub）防抖中的写落盘 */
    s_frozen_hub = wipe_all;                             /* 先于 s_frozen：任务看到冻结时一定看得到范围 */
    s_frozen = true;                                     /* 从这里起不再收写：擦完之后别被并发的写（网页改设置 / 配对落盘）补回去 */
    err = hub_store_erase_ns(HUB_STORE_NS_MUSE, 3000);
    if (err == ESP_OK && wipe_all) err = hub_store_erase_ns(HUB_STORE_NS_HUB, 3000);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "factory reset: hubdb 的 muse%s 已清，zhub 保留", wipe_all ? " 与 hub" : "");
    } else {
        ESP_LOGE(TAG, "factory reset: hubdb 清除失败 (%s)", esp_err_to_name(err));
    }
    return err;
}

/* config_mgr 恢复出厂时调的钩子（config_mgr.c 里是弱定义：config_mgr 不能依赖 hub_core，否则组件依赖成环）。
   本文件链接进来（直连型 main.c 调 hub_store_start）就由这里的强定义生效。 */
esp_err_t config_mgr_hubdb_factory_reset(bool wipe_all)
{
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
    /* 全清也清 Matter（设计第十二节"Matter fabric 与节点"一行）：先删 matter 分区里的绑定记号、再删 hubdb 的身份（mctrl）——
       中途断电最坏是"身份还在、记号没了"，下次开机按身份重绑、设备照旧（重置没做完，但不坏）；反过来（身份没了、记号还在）
       matter_ctrl 会停在 ident_lost 等人处理。记号删不掉就这次保留 Matter。matter 分区本身下次开机由 matter_bind 擦（记号对不上） */
    const bool forget_matter = wipe_all && matter_ctrl_forget_binding() == ESP_OK;
    if (wipe_all && !forget_matter) ESP_LOGE(TAG, "factory reset: matter 分区的绑定记号没删掉 —— 这次保留 Matter 的 fabric 身份");
    /* "允许测试设备"不管身份留不留都清回关（审计：设备转手前全清，记号恰好删不掉时开关不能跟着留下来）。排在 hub_store_factory_reset 冻结之前 */
    if (wipe_all) (void)hub_store_erase(HUB_STORE_NS_MCTRL, HUB_MATTER_OPTS_KEY);
#endif
    esp_err_t err = hub_store_factory_reset(wipe_all);
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
    if (err == ESP_OK && forget_matter) {
        err = hub_store_erase_ns(HUB_STORE_NS_MCTRL, 3000);
        if (err != ESP_OK) ESP_LOGE(TAG, "factory reset: hubdb 的 mctrl 没清 (%s)", esp_err_to_name(err));
    }
#endif
    return err;
}

void hub_store_get_status(hub_store_status_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_mux);
    *out = s_status;
    taskEXIT_CRITICAL(&s_mux);
}
