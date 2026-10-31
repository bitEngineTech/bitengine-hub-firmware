/* hub_evlog 的真值表（docs/15 设计 6.7 最近事件环）。编的是产品源文件 ../hub_evlog.c 与 ../hub_json.c，结果用 cJSON 解析回来比。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "hub_evlog.h"

static int s_fail, s_pass;
#define CHECK(cond, ...) do { if (cond) { s_pass++; } else { s_fail++; printf("❌ %s:%d ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static hev_t mk(hub_evt_class_t c, const char *kind, const char *text, uint64_t t_ms)
{
    hev_t e;
    memset(&e, 0, sizeof(e));
    e.cls = c;
    e.mono_ms = t_ms;
    snprintf(e.kind, sizeof(e.kind), "%s", kind);
    snprintf(e.text, sizeof(e.text), "%s", text);
    return e;
}

static cJSON *recent(const hev_ring_t *r, uint64_t now, uint32_t limit, uint32_t since, size_t cap, int *len)
{
    static char buf[16384];
    hj_t w;
    hj_init(&w, buf, cap);
    hev_write_recent(&w, r, now, limit, since);
    *len = hj_finish(&w);
    return *len > 0 ? cJSON_Parse(buf) : NULL;
}

static void test_add_and_order(void)
{
    static hev_ring_t r;
    hev_ring_init(&r);
    CHECK(hev_ring_newest(&r, 0) == NULL, "空环");
    hev_t e = mk(HUB_EVC_GATEWAY, "thread_down", "Hub: the Thread network is down.", 1000);
    CHECK(hev_ring_add(&r, &e) == 1, "第一条 seq = 1");
    e = mk(HUB_EVC_GATEWAY, "thread_up", "Hub: the Thread network is back.", 2000);
    CHECK(hev_ring_add(&r, &e) == 2, "第二条 seq = 2");
    CHECK(strcmp(hev_ring_newest(&r, 0)->kind, "thread_up") == 0 && strcmp(hev_ring_newest(&r, 1)->kind, "thread_down") == 0,
          "新的在前");
    CHECK(hev_ring_newest(&r, 2) == NULL, "越界 NULL");

    for (int i = 0; i < 100; i++) {
        char t[40];
        snprintf(t, sizeof(t), "event %d", i);
        e = mk(HUB_EVC_SECURITY, "contact", t, 3000 + (uint64_t)i);
        hev_ring_add(&r, &e);
    }
    CHECK(r.count == HEV_RING_CAP, "满了是 64 条");
    CHECK(strcmp(hev_ring_newest(&r, 0)->text, "event 99") == 0, "最新的是最后加的");
    CHECK(strcmp(hev_ring_newest(&r, 63)->text, "event 36") == 0, "最旧的被覆盖掉（剩 36..99）");
    CHECK(hev_ring_newest(&r, 0)->seq == 102, "seq 连续递增：%u", (unsigned)hev_ring_newest(&r, 0)->seq);
}

static void test_reject(void)
{
    static hev_ring_t r;
    hev_ring_init(&r);
    hev_t e = mk(HUB_EVC_COUNT, "x", "t", 0);
    CHECK(hev_ring_add(&r, &e) == 0, "类别越界拒");
    e = mk(HUB_EVC_ALARM, "", "t", 0);
    CHECK(hev_ring_add(&r, &e) == 0, "kind 空拒");
    e = mk(HUB_EVC_ALARM, "Water-Leak", "t", 0);
    CHECK(hev_ring_add(&r, &e) == 0, "kind 只许 [a-z0-9_]（它原样进 JSON，也进去重键）");
    e = mk(HUB_EVC_ALARM, "leak", "", 0);
    CHECK(hev_ring_add(&r, &e) == 0, "文字空拒");
    e = mk(HUB_EVC_ALARM, "leak", "t", 0);
    memset(e.text, 'a', sizeof(e.text));
    CHECK(hev_ring_add(&r, &e) == 0, "文字没 NUL 拒（不越界读）");
    e = mk(HUB_EVC_ALARM, "leak", "t", 0);
    memcpy(e.clock, "25:00", 6);
    CHECK(hev_ring_add(&r, &e) == 0, "时钟格式不对拒");
    memcpy(e.clock, "23:59", 6);
    CHECK(hev_ring_add(&r, &e) == 1, "23:59 收");
    CHECK(hev_ring_add(NULL, &e) == 0 && hev_ring_add(&r, NULL) == 0, "NULL");
    CHECK(r.count == 1, "拒掉的都没进环");
}

static void test_json(void)
{
    static hev_ring_t r;
    hev_ring_init(&r);
    hev_t e = mk(HUB_EVC_ALARM, "water_leak", "Water leak detected by \"Kitchen\" at 14:03.", 1000);
    snprintf(e.dev_id, sizeof(e.dev_id), "zb-00158d0001a2b3c4");
    memcpy(e.clock, "14:03", 6);
    hev_ring_add(&r, &e);
    e = mk(HUB_EVC_GATEWAY, "thread_down", "Hub: the Thread network is down.", 61000);
    hev_ring_add(&r, &e);

    int len = 0;
    cJSON *j = recent(&r, 121000, 20, 0, 4096, &len);
    const cJSON *ev = cJSON_GetObjectItemCaseSensitive(j, "events");
    CHECK(cJSON_GetArraySize(ev) == 2 && cJSON_GetObjectItemCaseSensitive(j, "count")->valueint == 2, "两条");
    const cJSON *a = cJSON_GetArrayItem(ev, 0), *b = cJSON_GetArrayItem(ev, 1);
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(a, "kind")->valuestring, "thread_down") == 0, "新的在前");
    CHECK(cJSON_GetObjectItemCaseSensitive(a, "device") == NULL, "网关自己的事件没有 device 键");
    CHECK(cJSON_GetObjectItemCaseSensitive(a, "time") == NULL, "没对时没有 time 键");
    CHECK(cJSON_GetObjectItemCaseSensitive(a, "age_s")->valueint == 60, "age_s 按开机毫秒算");
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(b, "class")->valuestring, "alarm") == 0, "类别名");
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(b, "device")->valuestring, "zb-00158d0001a2b3c4") == 0, "设备 id");
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(b, "text")->valuestring, "Water leak detected by \"Kitchen\" at 14:03.") == 0,
          "文字里的引号转义后原样回来");
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(b, "time")->valuestring, "14:03") == 0, "time");
    CHECK(cJSON_GetObjectItemCaseSensitive(j, "truncated") == NULL, "没截断就没有 truncated");
    cJSON_Delete(j);

    j = recent(&r, 121000, 20, 90, 4096, &len);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(j, "events")) == 1, "since_s 90：只剩 60 s 前那条");
    cJSON_Delete(j);
    j = recent(&r, 121000, 1, 0, 4096, &len);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(j, "events")) == 1, "limit 1");
    cJSON_Delete(j);
    j = recent(&r, 121000, 0, 0, 4096, &len);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(j, "events")) == 1, "limit 0 夹到 1");
    cJSON_Delete(j);

    /* 小缓冲：放不下第二条 → 撤掉、带 truncated，JSON 仍然合法 */
    j = recent(&r, 121000, 20, 0, 200, &len);
    CHECK(j != NULL, "截断后仍是合法 JSON（%d）", len);
    CHECK(j && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "truncated")), "truncated");
    CHECK(j && cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(j, "events"))
                   == cJSON_GetObjectItemCaseSensitive(j, "count")->valueint, "count 等于实际写出的条数");
    cJSON_Delete(j);

    /* 64 条长文字、limit 50、64 KB：全放得下 */
    hev_ring_init(&r);
    for (int i = 0; i < 64; i++) {
        e = mk(HUB_EVC_SECURITY, "contact", "", (uint64_t)i * 1000);
        memset(e.text, 'x', HEV_TEXT_MAX - 1);
        e.text[HEV_TEXT_MAX - 1] = 0;
        hev_ring_add(&r, &e);
    }
    static char big[65536];
    hj_t w;
    hj_init(&w, big, sizeof(big));
    hev_write_recent(&w, &r, 100000, 99, 0);
    len = hj_finish(&w);
    j = cJSON_Parse(big);
    CHECK(j && cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(j, "events")) == 50, "limit 超 50 夹到 50");
    cJSON_Delete(j);
}

int main(void)
{
    test_add_and_order();
    test_reject();
    test_json();
    CHECK(strcmp(hev_class_name(HUB_EVC_BATTERY), "battery") == 0 && hev_class_name(HUB_EVC_COUNT) == NULL, "类别名边界");
    printf("hub_evlog: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
