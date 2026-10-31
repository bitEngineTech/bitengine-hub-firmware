/* muse_push_core 主机测试（docs/15 设计 4.9、TC-MU13 的主机侧判据：网关不合并、不去重、不限配额，只有队列 / 过期 / 拒收）。编的是产品源文件 ../muse_push_core.c。 */
#include "muse_push_core.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) { g_pass++; } else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const uint64_t T0 = 10000000;

/* key 只是给测试认这一条用的：拼在句子前面（"key|句子"；key 为空时就是句子本身） */
static muse_push_t msg(hub_evt_class_t c, const char *key, const char *text)
{
    muse_push_t m;
    memset(&m, 0, sizeof(m));
    m.cls = c;
    m.main_chat = c == HUB_EVC_ALARM;
    if (key[0] && text[0]) snprintf(m.text, sizeof(m.text), "%s|%s", key, text);
    else snprintf(m.text, sizeof(m.text), "%s", text);
    return m;
}
static bool is(const muse_push_t *m, const char *key) { size_t n = strlen(key); return m && strncmp(m->text, key, n) == 0 && m->text[n] == '|'; }

static void test_order_and_eviction(void)
{
    mpq_t q;
    mpq_init(&q, T0);
    muse_push_t a = msg(HUB_EVC_BATTERY, "b1", "battery low"), b = msg(HUB_EVC_ALARM, "w1", "water leak"),
                c = msg(HUB_EVC_SECURITY, "d1", "door opened");
    CHECK(mpq_enqueue(&q, &a, T0) == MPQ_QUEUED && mpq_enqueue(&q, &b, T0 + 1) == MPQ_QUEUED && mpq_enqueue(&q, &c, T0 + 2) == MPQ_QUEUED, "enqueue 3");
    int i = mpq_next(&q, T0 + 3);
    CHECK(i >= 0 && mpq_get(&q, i)->cls == HUB_EVC_ALARM, "alarm first");
    mpq_mark_sent(&q, i, T0 + 3);
    i = mpq_next(&q, T0 + 4);
    CHECK(i >= 0 && mpq_get(&q, i)->cls == HUB_EVC_SECURITY, "security before battery");
    mpq_mark_sent(&q, i, T0 + 4);
    i = mpq_next(&q, T0 + 5);
    CHECK(i >= 0 && mpq_get(&q, i)->cls == HUB_EVC_BATTERY, "battery last");
    mpq_mark_sent(&q, i, T0 + 5);
    CHECK(mpq_next(&q, T0 + 6) == -1 && mpq_count(&q) == 0 && q.sent == 3, "empty");
    CHECK(mpq_prio(HUB_EVC_ALARM) > mpq_prio(HUB_EVC_SECURITY) && mpq_prio(HUB_EVC_GATEWAY) > mpq_prio(HUB_EVC_BATTERY)
          && mpq_prio(HUB_EVC_COUNT) == 0, "prio order");

    /* 满了：16 条电量 → 来一条报警，挤掉最旧的电量；再来一条电量（与队里最低同级）挤掉队里最旧的同级那条 */
    mpq_init(&q, T0);
    char k[16];
    for (int n = 0; n < MPQ_CAP; n++) {
        snprintf(k, sizeof(k), "b%d", n);
        muse_push_t m = msg(HUB_EVC_BATTERY, k, "battery");
        CHECK(mpq_enqueue(&q, &m, T0 + (uint64_t)n) == MPQ_QUEUED, "fill %d", n);
    }
    muse_push_t al = msg(HUB_EVC_ALARM, "w9", "leak");
    CHECK(mpq_enqueue(&q, &al, T0 + 100) == MPQ_QUEUED_EVICTED && q.dropped_full == 1, "alarm evicts");
    bool b0_gone = true;
    for (int n = 0; n < MPQ_CAP; n++) if (q.slot[n].used && is(&q.slot[n].msg, "b0")) b0_gone = false;
    CHECK(b0_gone, "oldest battery evicted");
    muse_push_t bn = msg(HUB_EVC_BATTERY, "bnew", "battery");
    CHECK(mpq_enqueue(&q, &bn, T0 + 200) == MPQ_QUEUED_EVICTED, "same-prio newcomer evicts the oldest same-prio");
    /* 队里全是报警时，新来的电量直接丢 */
    mpq_init(&q, T0);
    for (int n = 0; n < MPQ_CAP; n++) {
        snprintf(k, sizeof(k), "a%d", n);
        muse_push_t m = msg(HUB_EVC_ALARM, k, "alarm");
        mpq_enqueue(&q, &m, T0 + (uint64_t)n);
    }
    muse_push_t low = msg(HUB_EVC_BATTERY, "bx", "battery");
    CHECK(mpq_enqueue(&q, &low, T0 + 50) == MPQ_DROPPED_FULL && mpq_count(&q) == MPQ_CAP, "lowest newcomer dropped");
}

static void test_expiry(void)
{
    mpq_t q;
    mpq_init(&q, T0);
    muse_push_t s = msg(HUB_EVC_SECURITY, "d", "door"), a = msg(HUB_EVC_ALARM, "w", "leak");
    mpq_enqueue(&q, &s, T0);
    mpq_enqueue(&q, &a, T0);
    CHECK(mpq_next(&q, T0 + 599999) >= 0 && mpq_count(&q) == 2, "both alive just before 600 s");
    int i = mpq_next(&q, T0 + 600000);
    CHECK(mpq_count(&q) == 1 && i >= 0 && mpq_get(&q, i)->cls == HUB_EVC_ALARM && q.dropped_expired == 1, "non-alarm expires at 600 s");
    CHECK(mpq_next(&q, T0 + 1800000) == -1 && q.dropped_expired == 2, "alarm expires at 1800 s");
    CHECK(mpq_max_age_s(HUB_EVC_ALARM) == 1800 && mpq_max_age_s(HUB_EVC_GATEWAY) == 600, "ages");
    /* 满队列时先腾过期的，不白挤掉有效的 */
    mpq_init(&q, T0);
    char k[16];
    for (int n = 0; n < MPQ_CAP; n++) {
        snprintf(k, sizeof(k), "s%d", n);
        muse_push_t m = msg(HUB_EVC_SECURITY, k, "x");
        mpq_enqueue(&q, &m, n == 0 ? T0 : T0 + 500000);
    }
    muse_push_t m = msg(HUB_EVC_BATTERY, "late", "x");
    CHECK(mpq_enqueue(&q, &m, T0 + 600001) == MPQ_QUEUED && q.dropped_full == 0, "expired slot reused before evicting");
}

static void test_body(void)
{
    char buf[1024];
    muse_push_t a = msg(HUB_EVC_ALARM, "w", "Water leak detected by \"Kitchen leak sensor\" (Kitchen) at 14:03. [zb-00158d0001a2b3c4]");
    int n = mpq_build_body(buf, sizeof(buf), &a, "homelink-a1b2c3", "unused");
    cJSON *j = n > 0 ? cJSON_Parse(buf) : NULL;
    char want[600];
    snprintf(want, sizeof(want), "[bitEngine Hub] %s", a.text);   /* 来源前缀（2026-10-23 用户定），手写不读被测常量 */
    CHECK(j && strcmp(cJSON_GetObjectItemCaseSensitive(j, "message")->valuestring, want) == 0
          && strcmp(cJSON_GetObjectItemCaseSensitive(j, "output_modality")->valuestring, "text") == 0
          && strcmp(cJSON_GetObjectItemCaseSensitive(j, "device_id")->valuestring, "homelink-a1b2c3") == 0
          && !cJSON_GetObjectItemCaseSensitive(j, "session_id"), "main chat body: %s", buf);
    cJSON_Delete(j);
    muse_push_t s = msg(HUB_EVC_SECURITY, "d", "\"Front door\" opened at 21:15.");
    n = mpq_build_body(buf, sizeof(buf), &s, "homelink-a1b2c3", "6f1c2d3e-0000-4000-8000-000000000001");
    j = n > 0 ? cJSON_Parse(buf) : NULL;
    CHECK(j && strcmp(cJSON_GetObjectItemCaseSensitive(j, "session_id")->valuestring, "6f1c2d3e-0000-4000-8000-000000000001") == 0, "side chat body");
    cJSON_Delete(j);
    CHECK(mpq_build_body(buf, sizeof(buf), &s, "homelink-a1b2c3", NULL) == -1, "side chat needs chat_sid");
    {                                                              /* 句子顶到上限：加了前缀照样放得进 2048 的请求体，前缀不截掉句子 */
        muse_push_t L = msg(HUB_EVC_GATEWAY, "g", "");
        L.main_chat = true;                                        /* msg() 只给 ALARM 标主会话；推送现在一律主会话 */
        memset(L.text, 'x', MPQ_TEXT_MAX - 1);
        L.text[MPQ_TEXT_MAX - 1] = '\0';
        static char big[2048];
        n = mpq_build_body(big, sizeof(big), &L, "homelink-a1b2c3", NULL);
        j = n > 0 ? cJSON_Parse(big) : NULL;
        const cJSON *mm = j ? cJSON_GetObjectItemCaseSensitive(j, "message") : NULL;
        CHECK(mm && strlen(mm->valuestring) == strlen("[bitEngine Hub] ") + MPQ_TEXT_MAX - 1, "最长的句子加前缀：完整");
        cJSON_Delete(j);
    }
    CHECK(mpq_build_body(buf, 20, &a, "homelink-a1b2c3", NULL) == -1 && buf[0] == 0, "too small");
    CHECK(mpq_build_body(buf, sizeof(buf), &a, "", NULL) == -1, "node id required");
    /* 参数 */
    mpq_t q;
    mpq_init(&q, T0);
    muse_push_t bad = msg(HUB_EVC_COUNT, "x", "x");
    CHECK(mpq_enqueue(&q, &bad, T0) == MPQ_BAD_ARG, "bad class");
    muse_push_t empty = msg(HUB_EVC_GATEWAY, "x", "");
    CHECK(mpq_enqueue(&q, &empty, T0) == MPQ_BAD_ARG, "empty text");
    CHECK(mpq_enqueue(NULL, &empty, T0) == MPQ_BAD_ARG && mpq_next(NULL, T0) == -1 && mpq_get(&q, 99) == NULL, "NULLs");
    mpq_mark_sent(&q, -1, T0);
    mpq_mark_sent(&q, 3, T0);                          /* 空槽：不动 */
    CHECK(q.sent == 0, "mark_sent on empty slot ignored");
}

static void test_drop(void)
{
    mpq_t q;
    mpq_init(&q, T0);
    muse_push_t a = msg(HUB_EVC_GATEWAY, "g1", "hub problem");
    mpq_enqueue(&q, &a, T0);
    const int i = mpq_next(&q, T0);
    mpq_drop(&q, i);
    CHECK(mpq_count(&q) == 0 && q.dropped_rejected == 1 && q.sent == 0, "4xx：丢掉、计 rejected、不算发出");
    CHECK(mpq_enqueue(&q, &a, T0 + 1) == MPQ_QUEUED, "同一条还能再排");
    CHECK(mpq_dropped_total(&q) == 1, "dropped_total 含 rejected");
    mpq_drop(&q, -1);
    mpq_drop(&q, MPQ_CAP);
    mpq_drop(NULL, 0);
    CHECK(q.dropped_rejected == 1, "越界 / NULL 不动");
}

static void test_inflight(void)
{
    mpq_t q;
    mpq_init(&q, T0);
    muse_push_t g = msg(HUB_EVC_GATEWAY, "g1", "gateway");
    mpq_enqueue(&q, &g, T0);
    const int i = mpq_next(&q, T0);
    mpq_begin(&q, i);
    CHECK(mpq_next(&q, T0) == -1, "在途的不再给");
    CHECK(mpq_next(&q, T0 + 3600000) == -1 && mpq_count(&q) == 1, "在途的不过期");
    /* 塞满更高优先级的：在途那条（最低优先级、最旧）也不被挤 */
    for (int k = 0; k < MPQ_CAP + 3; k++) {
        char key[8];
        snprintf(key, sizeof(key), "a%d", k);
        muse_push_t a = msg(HUB_EVC_ALARM, key, "alarm");
        mpq_enqueue(&q, &a, T0 + 1 + (uint64_t)k);
    }
    CHECK(q.slot[i].used && q.slot[i].inflight && is(&q.slot[i].msg, "g1"), "在途那条还在原槽");
    mpq_abort(&q, i);
    CHECK(!q.slot[i].inflight && q.slot[i].used, "abort：留在队列");
    CHECK(mpq_next(&q, T0 + 3600000) != i, "abort 后过期照算（入队 1 h 前，GATEWAY 600 s）");
    CHECK(!q.slot[i].used, "已过期丢掉");

    mpq_init(&q, T0);
    mpq_enqueue(&q, &g, T0);
    const int j = mpq_next(&q, T0);
    mpq_begin(&q, j);
    mpq_mark_sent(&q, j, T0 + 5);
    CHECK(!q.slot[j].used && !q.slot[j].inflight && q.sent == 1, "发成：腾槽、清在途");
    muse_push_t h = msg(HUB_EVC_GATEWAY, "h1", "x");
    mpq_enqueue(&q, &h, T0 + 6);
    CHECK(!q.slot[mpq_next(&q, T0 + 6)].inflight, "新排进同一槽的不带在途标记");
}

static void test_classify(void)
{
    CHECK(mpq_classify_status(200) == MPQ_REPLY_SENT && mpq_classify_status(204) == MPQ_REPLY_SENT
          && mpq_classify_status(299) == MPQ_REPLY_SENT, "2xx 发成");
    CHECK(mpq_classify_status(400) == MPQ_REPLY_REJECTED && mpq_classify_status(403) == MPQ_REPLY_REJECTED
          && mpq_classify_status(413) == MPQ_REPLY_REJECTED, "4xx 拒收");
    CHECK(mpq_classify_status(408) == MPQ_REPLY_RETRY && mpq_classify_status(429) == MPQ_REPLY_RETRY, "408 / 429 重试");
    CHECK(mpq_classify_status(500) == MPQ_REPLY_RETRY && mpq_classify_status(503) == MPQ_REPLY_RETRY, "5xx 重试");
    CHECK(mpq_classify_status(-1) == MPQ_REPLY_RETRY && mpq_classify_status(0) == MPQ_REPLY_RETRY
          && mpq_classify_status(302) == MPQ_REPLY_RETRY && mpq_classify_status(199) == MPQ_REPLY_RETRY, "复位 / 超时 / 其他重试");
}

static void test_clear_queue(void)
{
    mpq_t q;
    mpq_init(&q, T0);
    muse_push_t a = msg(HUB_EVC_GATEWAY, "a", "x"), b = msg(HUB_EVC_GATEWAY, "b", "y");
    mpq_enqueue(&q, &a, T0);
    const int i = mpq_next(&q, T0);
    mpq_mark_sent(&q, i, T0);
    mpq_enqueue(&q, &b, T0 + 1);
    mpq_begin(&q, mpq_next(&q, T0 + 1));
    mpq_clear_queue(&q);
    CHECK(mpq_count(&q) == 0 && q.dropped_cleared == 1, "清掉排着的（含在途的），计 cleared");
    CHECK(q.sent == 1 && mpq_dropped_total(&q) == 1, "发出计数保留");
    CHECK(mpq_enqueue(&q, &a, T0 + 2) == MPQ_QUEUED, "刚发过的同一条照样能排（不去重）");
    mpq_clear_queue(NULL);
}

/* 网关不做人为干预（2026-10-24 用户定）：同一件事连着来、同一台设备来回开关、一小时几百条 —— 每条都原样排、马上能发，
   不合并、不去重、不等配额 */
static void test_no_filtering(void)
{
    mpq_t q;
    mpq_init(&q, T0);
    muse_push_t o = msg(HUB_EVC_SECURITY, "", "\"Front door\" opened at 21:15. [mt-0000000000000005]");
    muse_push_t c = msg(HUB_EVC_SECURITY, "", "\"Front door\" closed at 21:15. [mt-0000000000000005]");
    CHECK(mpq_enqueue(&q, &o, T0) == MPQ_QUEUED && mpq_enqueue(&q, &c, T0 + 1) == MPQ_QUEUED && mpq_enqueue(&q, &o, T0 + 2) == MPQ_QUEUED
          && mpq_count(&q) == 3, "开 → 关 → 又开：三条都排着（一模一样的句子也不去重）");
    int i = mpq_next(&q, T0 + 2);
    CHECK(i >= 0 && strcmp(mpq_get(&q, i)->text, o.text) == 0, "排上就能发（不等合并窗口）：先来的先发");
    mpq_mark_sent(&q, i, T0 + 2);
    i = mpq_next(&q, T0 + 2);
    CHECK(i >= 0 && strcmp(mpq_get(&q, i)->text, c.text) == 0, "同一毫秒里接着发下一条");
    mpq_mark_sent(&q, i, T0 + 2);
    i = mpq_next(&q, T0 + 2);
    CHECK(i >= 0 && strcmp(mpq_get(&q, i)->text, o.text) == 0, "又开的那条：刚发过一模一样的也照发");
    mpq_mark_sent(&q, i, T0 + 2);
    /* 一小时里 500 条普通事件 + 50 条报警：一条一条排、一条一条发，都发得出去（没有配额） */
    mpq_init(&q, T0);
    int sent = 0;
    for (int k = 0; k < 550; k++) {
        const uint64_t t = T0 + (uint64_t)k * 6000u;
        muse_push_t m = msg(k % 11 == 0 ? HUB_EVC_ALARM : HUB_EVC_SECURITY, "", "Motion detected by \"Hallway\" at 10:00. [zb-b40e060fffe26179]");
        if (mpq_enqueue(&q, &m, t) != MPQ_QUEUED) break;
        const int j = mpq_next(&q, t);
        if (j < 0) break;
        mpq_mark_sent(&q, j, t);
        sent++;
    }
    CHECK(sent == 550 && q.sent == 550 && mpq_dropped_total(&q) == 0, "550 条（含 50 条报警）全发出去：%d", sent);
    /* 断线时一下来 16 条以上：只有队列装不下才挤（最低优先级里最旧的），这是唯一会丢的情形 */
    mpq_init(&q, T0);
    for (int k = 0; k < MPQ_CAP; k++) {
        muse_push_t m = msg(HUB_EVC_SECURITY, "", "same");
        CHECK(mpq_enqueue(&q, &m, T0 + (uint64_t)k) == MPQ_QUEUED, "装第 %d 条", k);
    }
    CHECK(mpq_count(&q) == MPQ_CAP, "装满一模一样的都排着");
}

int main(void)
{
    test_order_and_eviction();
    test_expiry();
    test_no_filtering();
    test_body();
    test_drop();
    test_inflight();
    test_clear_queue();
    test_classify();
    printf("muse_push_core: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
