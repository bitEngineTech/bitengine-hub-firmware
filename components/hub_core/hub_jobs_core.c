/* hub_jobs_core —— 作业表（规则见 include/hub_jobs_core.h）。纯逻辑，test_host/ 原样编。 */
#include "hub_jobs_core.h"

#include <string.h>

static void cpy(char *dst, size_t cap, const char *src)
{
    if (!cap) return;
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

void hjc_init(hjc_table_t *t)
{
    if (!t) return;
    memset(t, 0, sizeof(*t));
    t->next_id = 1;
}

int hjc_inflight(const hjc_table_t *t)
{
    int n = 0;
    for (int i = 0; t && i < HJC_MAX; i++) n += t->jobs[i].state == HJC_RUNNING;
    return n;
}

uint32_t hjc_start(hjc_table_t *t, const char *cmd, uint8_t src, uint32_t now, uint32_t timeout_ms)
{
    if (!t || !cmd || !*cmd || timeout_ms == 0) return 0;
    if (hjc_inflight(t) >= HJC_INFLIGHT) return 0;
    int slot = -1;
    for (int i = 0; i < HJC_MAX && slot < 0; i++) if (t->jobs[i].state == HJC_FREE) slot = i;
    if (slot < 0) {                                /* 复用最早结束的那一格 */
        uint32_t oldest = 0;
        for (int i = 0; i < HJC_MAX; i++) {
            const hjc_job_t *j = &t->jobs[i];
            if (j->state == HJC_RUNNING) continue;
            const uint32_t age = now - j->t_end;
            if (slot < 0 || age > oldest) { slot = i; oldest = age; }
        }
    }
    if (slot < 0) return 0;
    hjc_job_t *j = &t->jobs[slot];
    memset(j, 0, sizeof(*j));
    if (t->next_id == 0) t->next_id = 1;
    j->id = t->next_id++;
    if (t->next_id == 0) t->next_id = 1;
    j->state = HJC_RUNNING;
    j->src = src;
    cpy(j->cmd, sizeof(j->cmd), cmd);
    j->t_start = now;
    j->deadline = now + timeout_ms;
    return j->id;
}

static hjc_job_t *find(hjc_table_t *t, uint32_t id)
{
    for (int i = 0; t && id && i < HJC_MAX; i++) if (t->jobs[i].state != HJC_FREE && t->jobs[i].id == id) return &t->jobs[i];
    return NULL;
}

const hjc_job_t *hjc_get(const hjc_table_t *t, uint32_t id) { return find((hjc_table_t *)t, id); }

bool hjc_abandon(hjc_table_t *t, uint32_t id)
{
    hjc_job_t *j = find(t, id);
    if (!j || j->state != HJC_RUNNING) return false;
    memset(j, 0, sizeof(*j));
    return true;
}

bool hjc_finish(hjc_table_t *t, uint32_t id, uint32_t now, const char *err_code, const char *err_msg, const char *result)
{
    hjc_job_t *j = find(t, id);
    if (!j || j->state != HJC_RUNNING) return false;
    j->t_end = now;
    if (err_code) {
        j->state = HJC_FAILED;
        cpy(j->err_code, sizeof(j->err_code), err_code);
        cpy(j->err_msg, sizeof(j->err_msg), err_msg);
    } else {
        j->state = HJC_DONE;
        cpy(j->result, sizeof(j->result), result && *result ? result : "{}");
    }
    return true;
}

int hjc_expire(hjc_table_t *t, uint32_t now, uint32_t *ids, int max)
{
    int n = 0;
    for (int i = 0; t && i < HJC_MAX; i++) {
        hjc_job_t *j = &t->jobs[i];
        if (j->state != HJC_RUNNING || (int32_t)(now - j->deadline) < 0) continue;
        j->state = HJC_TIMEOUT;
        j->t_end = now;
        cpy(j->err_code, sizeof(j->err_code), "timeout");
        cpy(j->err_msg, sizeof(j->err_msg), "the hub did not finish in time");
        if (ids && n < max) ids[n] = j->id;
        n++;
    }
    return n;
}

const char *hjc_state_str(uint8_t st)
{
    switch (st) {
    case HJC_RUNNING: return "running";
    case HJC_DONE:    return "done";
    case HJC_FAILED:  return "failed";
    case HJC_TIMEOUT: return "timeout";
    default:          return "unknown";
    }
}
