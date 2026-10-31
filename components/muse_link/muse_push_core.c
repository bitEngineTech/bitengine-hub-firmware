/* muse_push_core —— 推送队列（见 muse_push_core.h：网关不合并、不去重、不限配额）。纯 C，不依赖 ESP-IDF。 */
#include "muse_push_core.h"

#include <stdio.h>
#include <string.h>

void mpq_init(mpq_t *q, uint64_t now_ms)
{
    (void)now_ms;                                  /* 不需要起点（保留参数：调用方不用改） */
    if (!q) return;
    memset(q, 0, sizeof(*q));
}

uint8_t mpq_prio(hub_evt_class_t c)
{
    return (unsigned)c < HUB_EVC_COUNT ? (uint8_t)(HUB_EVC_COUNT - (unsigned)c) : 0;   /* ALARM = 6 … BATTERY = 1 */
}

uint32_t mpq_max_age_s(hub_evt_class_t c)
{
    return c == HUB_EVC_ALARM ? MPQ_ALARM_AGE_S : MPQ_OTHER_AGE_S;
}

static bool expired(const mpq_t *q, int i, uint64_t now)
{
    if (q->slot[i].inflight) return false;                 /* 在途的等回包再说 */
    const uint64_t age = now >= q->slot[i].enq_ms ? now - q->slot[i].enq_ms : 0;
    return age >= (uint64_t)mpq_max_age_s(q->slot[i].msg.cls) * 1000u;
}

/* 最低优先级里最旧的那个槽 */
static int victim(const mpq_t *q)
{
    int v = -1;
    for (int i = 0; i < MPQ_CAP; i++) {
        if (!q->slot[i].used || q->slot[i].inflight) continue;      /* 在途的不挤 */
        if (v < 0) { v = i; continue; }
        const uint8_t pi = mpq_prio(q->slot[i].msg.cls), pv = mpq_prio(q->slot[v].msg.cls);
        if (pi < pv || (pi == pv && q->slot[i].enq_ms < q->slot[v].enq_ms)) v = i;
    }
    return v;
}

mpq_result_t mpq_enqueue(mpq_t *q, const muse_push_t *m, uint64_t now_ms)
{
    if (!q || !m || (unsigned)m->cls >= HUB_EVC_COUNT || !m->text[0] || !memchr(m->text, '\0', sizeof(m->text))) {
        return MPQ_BAD_ARG;
    }
    for (int i = 0; i < MPQ_CAP; i++) {                      /* 先把过期的腾掉，免得白挤掉一条有效的 */
        if (q->slot[i].used && expired(q, i, now_ms)) {
            q->slot[i].used = false;
            q->dropped_expired++;
        }
    }
    int free_i = -1;
    for (int i = 0; i < MPQ_CAP && free_i < 0; i++) {
        if (!q->slot[i].used) free_i = i;
    }
    mpq_result_t r = MPQ_QUEUED;
    if (free_i < 0) {
        const int v = victim(q);
        /* 新来的比最低的还低、或同级（新来的更新，"最旧的"是队里那条）：同级时丢队里那条最旧的；
           只剩在途的那条可挤（v < 0，队列容量 ≥ 2 时不会发生）就丢新来的 */
        if (v < 0 || mpq_prio(m->cls) < mpq_prio(q->slot[v].msg.cls)) {
            q->dropped_full++;
            return MPQ_DROPPED_FULL;
        }
        q->slot[v].used = false;
        q->dropped_full++;
        free_i = v;
        r = MPQ_QUEUED_EVICTED;
    }
    q->slot[free_i].msg = *m;
    q->slot[free_i].enq_ms = now_ms;
    q->slot[free_i].used = true;
    q->slot[free_i].inflight = false;
    return r;
}

int mpq_next(mpq_t *q, uint64_t now_ms)
{
    if (!q) return -1;
    int best = -1;
    for (int i = 0; i < MPQ_CAP; i++) {
        if (!q->slot[i].used || q->slot[i].inflight) continue;
        if (expired(q, i, now_ms)) {
            q->slot[i].used = false;
            q->dropped_expired++;
            continue;
        }
        if (best < 0) { best = i; continue; }
        const uint8_t pi = mpq_prio(q->slot[i].msg.cls), pb = mpq_prio(q->slot[best].msg.cls);
        if (pi > pb || (pi == pb && q->slot[i].enq_ms < q->slot[best].enq_ms)) best = i;
    }
    return best;
}

void mpq_begin(mpq_t *q, int idx)
{
    if (q && idx >= 0 && idx < MPQ_CAP && q->slot[idx].used) q->slot[idx].inflight = true;
}

void mpq_abort(mpq_t *q, int idx)
{
    if (q && idx >= 0 && idx < MPQ_CAP && q->slot[idx].used) q->slot[idx].inflight = false;
}

void mpq_mark_sent(mpq_t *q, int idx, uint64_t now_ms)
{
    (void)now_ms;                                  /* 不记发出时刻：没有配额、不去重（头注） */
    if (!q || idx < 0 || idx >= MPQ_CAP || !q->slot[idx].used) return;
    q->slot[idx].used = q->slot[idx].inflight = false;
    q->sent++;
}

void mpq_drop(mpq_t *q, int idx)
{
    if (!q || idx < 0 || idx >= MPQ_CAP || !q->slot[idx].used) return;
    q->slot[idx].used = q->slot[idx].inflight = false;
    q->dropped_rejected++;
}

mpq_reply_t mpq_classify_status(int status)
{
    if (status >= 200 && status < 300) return MPQ_REPLY_SENT;
    if (status >= 400 && status < 500 && status != 408 && status != 429) return MPQ_REPLY_REJECTED;
    return MPQ_REPLY_RETRY;
}

void mpq_clear_queue(mpq_t *q)
{
    for (int i = 0; q && i < MPQ_CAP; i++) {
        if (!q->slot[i].used) continue;
        q->slot[i].used = q->slot[i].inflight = false;
        q->dropped_cleared++;
    }
}

uint32_t mpq_dropped_total(const mpq_t *q)
{
    return q ? q->dropped_full + q->dropped_expired + q->dropped_rejected + q->dropped_cleared : 0;
}

const muse_push_t *mpq_get(const mpq_t *q, int idx)
{
    if (!q || idx < 0 || idx >= MPQ_CAP || !q->slot[idx].used) return NULL;
    return &q->slot[idx].msg;
}

size_t mpq_count(const mpq_t *q)
{
    size_t n = 0;
    for (int i = 0; q && i < MPQ_CAP; i++) n += q->slot[i].used ? 1u : 0u;
    return n;
}

int mpq_build_body(char *buf, size_t cap, const muse_push_t *m, const char *node_id, const char *chat_sid)
{
    if (!buf || cap == 0) return -1;
    buf[0] = '\0';
    if (!m || !node_id || !node_id[0] || (!m->main_chat && (!chat_sid || !chat_sid[0]))) return -1;
    hj_t w;
    hj_init(&w, buf, cap);
    hj_obj_begin(&w);
    char msg[sizeof(MUSE_PUSH_PREFIX) + MPQ_TEXT_MAX];
    snprintf(msg, sizeof(msg), "%s%s", MUSE_PUSH_PREFIX, m->text);
    hj_kstr(&w, "message", msg);
    hj_kstr(&w, "output_modality", "text");
    hj_kstr(&w, "device_id", node_id);
    if (!m->main_chat) hj_kstr(&w, "session_id", chat_sid);
    hj_obj_end(&w);
    if (w.truncated) {
        buf[0] = '\0';
        return -1;
    }
    return hj_finish(&w);
}
