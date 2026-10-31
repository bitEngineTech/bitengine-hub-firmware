/* hub_jobs —— 作业表的加锁外壳（见 include/hub_jobs.h）。 */
#include "hub_jobs.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hub_audit.h"

static const char *TAG = "hub_jobs";

#define LOCK_MS 100

static hjc_table_t *s_t;                           /* PSRAM（约 4 KB，别占内部 RAM） */
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static void (*volatile s_listener)(uint32_t id);

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

esp_err_t hub_jobs_init(void)
{
    if (s_t) return ESP_OK;
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    hjc_table_t *t = heap_caps_calloc(1, sizeof(*t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!t) return ESP_ERR_NO_MEM;
    hjc_init(t);
    s_t = t;
    return ESP_OK;
}

void hub_jobs_set_listener(void (*fn)(uint32_t id)) { s_listener = fn; }

uint32_t hub_job_start(const char *cmd, hub_src_t src, uint32_t timeout_ms)
{
    if (!s_t || xSemaphoreTake(s_lock, pdMS_TO_TICKS(LOCK_MS)) != pdTRUE) return 0;
    const uint32_t id = hjc_start(s_t, cmd, (uint8_t)src, now_ms(), timeout_ms);
    xSemaphoreGive(s_lock);
    return id;
}

/* 结束之后：审计 + 通知（锁外） */
static void after_end(const hjc_job_t *j)
{
    hub_audit_record((hau_src_t)j->src, j->cmd, NULL, j->state == HJC_DONE ? "ok" : j->err_code, j->t_end - j->t_start);
    void (*fn)(uint32_t) = s_listener;
    if (fn) fn(j->id);
}

bool hub_job_finish(uint32_t id, const char *err_code, const char *err_msg, const char *result_json)
{
    if (!s_t) return false;
    /* 结束不能因为锁一时忙就丢（作业会一直挂到超时）：等得比 start 久 */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "作业 %lu 结束时锁 1 s 没拿到 —— 留给超时", (unsigned long)id);
        return false;
    }
    const bool ok = hjc_finish(s_t, id, now_ms(), err_code, err_msg, result_json);
    hjc_job_t copy;
    const hjc_job_t *j = ok ? hjc_get(s_t, id) : NULL;
    if (j) copy = *j;
    xSemaphoreGive(s_lock);
    if (j) after_end(&copy);
    return ok;
}

bool hub_job_get(uint32_t id, hjc_job_t *out)
{
    if (!s_t || !out || xSemaphoreTake(s_lock, pdMS_TO_TICKS(LOCK_MS)) != pdTRUE) return false;
    const hjc_job_t *j = hjc_get(s_t, id);
    if (j) *out = *j;
    xSemaphoreGive(s_lock);
    return j != NULL;
}

void hub_job_abandon(uint32_t id)
{
    if (!s_t || xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    (void)hjc_abandon(s_t, id);
    xSemaphoreGive(s_lock);
}

void hub_jobs_tick(void)
{
    /* 一格约 670 B：拷贝放静态区、逐个处理（hub_svc 的栈 6 KB，放 6 格的数组就是 4 KB，审计）。只有 hub_svc 调，不用另加锁 */
    static hjc_job_t s_done;
    if (!s_t || xSemaphoreTake(s_lock, pdMS_TO_TICKS(LOCK_MS)) != pdTRUE) return;
    uint32_t ids[HJC_MAX];
    const int n = hjc_expire(s_t, now_ms(), ids, HJC_MAX);
    xSemaphoreGive(s_lock);
    for (int i = 0; i < n && i < HJC_MAX; i++) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(LOCK_MS)) != pdTRUE) continue;
        const hjc_job_t *j = hjc_get(s_t, ids[i]);
        const bool have = j != NULL;
        if (have) s_done = *j;
        xSemaphoreGive(s_lock);
        if (!have) continue;
        ESP_LOGW(TAG, "作业 %lu（%s）超时", (unsigned long)s_done.id, s_done.cmd);
        after_end(&s_done);
    }
}
