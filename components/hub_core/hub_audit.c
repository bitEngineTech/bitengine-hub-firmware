/* hub_audit —— 审计环的入口（见 include/hub_audit.h）。只在直连型里编。 */
#include "hub_audit.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hub_events.h"

static const char *TAG = "hub_audit";

#define LOCK_TICKS pdMS_TO_TICKS(100)

#define LOG_GATE_MS  60000u                   /* 被拒的 W 日志每类每分钟最多一行 */

/* log_store 的闸（每类一个，s_lock 护着）：窗口开头那次照常打、带上一窗口压下的次数，窗口里其余的只计数（审计环里每条都有） */
typedef struct {
    uint32_t suppressed;
    uint64_t logged_ms;                          /* 0 = 还没打过 */
} log_gate_t;

static hau_ring_t        *s_ring;   /* PSRAM */
static SemaphoreHandle_t  s_lock;
static bool               s_inited;
static log_gate_t         s_gate_forbidden, s_gate_rl;

esp_err_t hub_audit_init(void)
{
    if (s_inited) return ESP_OK;
    s_ring = heap_caps_calloc(1, sizeof(hau_ring_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_lock = xSemaphoreCreateMutex();
    if (!s_ring || !s_lock) {
        heap_caps_free(s_ring);
        if (s_lock) vSemaphoreDelete(s_lock);
        s_ring = NULL;
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    hau_init(s_ring);
    s_inited = true;
    return ESP_OK;
}

static const char *param_str(const cJSON *params, const char *key)
{
    const cJSON *v = params ? cJSON_GetObjectItemCaseSensitive(params, key) : NULL;
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

static bool gate_pass(log_gate_t *g, uint64_t now, uint32_t *suppressed_before)
{
    if (g->logged_ms && now >= g->logged_ms && now - g->logged_ms < LOG_GATE_MS) {
        g->suppressed++;
        return false;
    }
    *suppressed_before = g->suppressed;
    g->suppressed = 0;
    g->logged_ms = now ? now : 1;
    return true;
}

static void record(hau_src_t src, const char *command, const char *device, const char *action, const char *result, uint32_t dur_ms)
{
    if (!s_inited || !command || !command[0]) return;
    hau_entry_t e;
    memset(&e, 0, sizeof(e));
    e.src = (uint8_t)src;
    e.mono_ms = (uint64_t)(esp_timer_get_time() / 1000);
    hub_events_local_clock(e.clock);
    hau_token(e.command, sizeof(e.command), command);
    hau_token(e.device, sizeof(e.device), device);
    hau_token(e.action, sizeof(e.action), action);
    hau_token(e.result, sizeof(e.result), result ? result : "ok");
    e.dur_ms = dur_ms;
    if (xSemaphoreTake(s_lock, LOCK_TICKS) != pdTRUE) return;
    (void)hau_add(s_ring, &e, NULL);
    /* log_store（W 环 2000 条按"能回看 28 小时"定的）写权限被拒与限速被拒（6.5、6.10），但每类每分钟最多一行 ——
       限速放在权限之前就是为了不让刷屏的调用冲掉日志（2026-10-22 第一轮审计 ❌），M5 有了敏感动作后轮换设备 id 刷 forbidden
       也一样（第二轮审计）。压下的次数在下一行里报，每一条都在审计环里。字段已过 hau_token，日志里没有控制字符。 */
    bool log_forbidden = false, log_rl = false;
    uint32_t before = 0;
    if (!strcmp(e.result, "forbidden")) log_forbidden = gate_pass(&s_gate_forbidden, e.mono_ms, &before);
    else if (!strcmp(e.result, "rate_limited")) log_rl = gate_pass(&s_gate_rl, e.mono_ms, &before);
    xSemaphoreGive(s_lock);
    const char *who = src == HAU_SRC_REST ? "本地" : "Muse";
    if (log_forbidden) {
        ESP_LOGW(TAG, "%s 的调用被拒（权限）：%s%s%s%s%s（此前一分钟内另有 %u 次被拒未逐条记，见 /api/v1/muse/audit）", who, e.command,
                 e.device[0] ? " " : "", e.device, e.action[0] ? " " : "", e.action, (unsigned)before);
    }
    if (log_rl) {
        ESP_LOGW(TAG, "%s 的调用被限速：%s（此前一分钟内另有 %u 次被限速未逐条记）", who, e.command, (unsigned)before);
    }
}

/* hub.device.update 没有 action 参数：动作列写改了哪几项（如 "name.room.notify_off"；审计字段只留 [A-Za-z0-9_.-]，hau_token），不写名字原文 —— 审计要分得出
   "关掉了门磁的推送"和"改了个房间"（Muse 的 agent 可能被提示注入，6.10 / 架构 5.5；C17 第二轮审计） */
static const char *update_action(const cJSON *params, char *buf, size_t cap)
{
    const cJSON *nv = cJSON_GetObjectItemCaseSensitive(params, "notify");
    const char *part[3] = {
        cJSON_IsString(cJSON_GetObjectItemCaseSensitive(params, "name")) ? "name" : NULL,
        cJSON_IsString(cJSON_GetObjectItemCaseSensitive(params, "room")) ? "room" : NULL,
        cJSON_IsBool(nv) ? (cJSON_IsTrue(nv) ? "notify_on" : "notify_off") : NULL,
    };
    size_t n = 0;
    buf[0] = '\0';
    for (unsigned k = 0; k < 3; k++) {
        if (!part[k]) continue;
        const int w = snprintf(buf + n, cap - n, "%s%s", n ? "." : "", part[k]);
        if (w < 0 || (size_t)w >= cap - n) break;   /* 放不下就停（"name.room.notify_off" 20 B，HAU_TOKEN_MAX 40 放得下） */
        n += (size_t)w;
    }
    return buf;
}

void hub_audit_record(hau_src_t src, const char *command, const cJSON *params, const char *result, uint32_t dur_ms)
{
    char ub[HAU_TOKEN_MAX];
    const char *act = param_str(params, "action");
    if (!act[0] && params && command && !strcmp(command, "hub.device.update")) act = update_action(params, ub, sizeof(ub));
    record(src, command, param_str(params, "id"), act, result, dur_ms);
}

void hub_audit_note(hau_src_t src, const char *what, const char *detail, const char *result)
{
    record(src, what, "", detail ? detail : "", result, 0);
}

bool hub_audit_ready(void)
{
    return s_inited;
}

bool hub_audit_write(hj_t *w, uint32_t limit)
{
    if (!w || !s_inited || xSemaphoreTake(s_lock, LOCK_TICKS) != pdTRUE) return false;
    hau_write(w, s_ring, (uint64_t)(esp_timer_get_time() / 1000), limit);
    xSemaphoreGive(s_lock);
    return true;
}
