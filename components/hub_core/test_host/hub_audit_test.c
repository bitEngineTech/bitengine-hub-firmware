/* hub_audit_core 的真值表（docs/15 设计 6.10）。编的是产品源文件 ../hub_audit_core.c 与 ../hub_json.c，结果用 cJSON 解析回来比。 */
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "hub_audit_core.h"

static int s_fail, s_pass;
#define CHECK(cond, ...) do { if (cond) { s_pass++; } else { s_fail++; printf("❌ %s:%d ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static hau_ring_t s_r;

static hau_entry_t mk(const char *cmd, const char *dev, const char *act, const char *res, uint64_t t, uint8_t src)
{
    hau_entry_t e;
    memset(&e, 0, sizeof(e));
    snprintf(e.command, sizeof(e.command), "%s", cmd);
    snprintf(e.device, sizeof(e.device), "%s", dev);
    snprintf(e.action, sizeof(e.action), "%s", act);
    snprintf(e.result, sizeof(e.result), "%s", res);
    e.mono_ms = t;
    e.src = src;
    e.dur_ms = 7;
    return e;
}

static cJSON *dump(uint64_t now, uint32_t limit, size_t cap)
{
    static char buf[65536];
    hj_t w;
    hj_init(&w, buf, cap);
    hau_write(&w, &s_r, now, limit);
    return hj_finish(&w) > 0 ? cJSON_Parse(buf) : NULL;
}

int main(void)
{
    char t[16];
    hau_token(t, sizeof(t), "hub.devices.list");
    CHECK(strcmp(t, "hub.devices.lis") == 0, "超长截断（%s）", t);
    hau_token(t, sizeof(t), "a\"b\n<c>");
    CHECK(strcmp(t, "a?b??c?") == 0, "引号、换行、尖括号换成 ?（%s）", t);
    hau_token(t, sizeof(t), NULL);
    CHECK(t[0] == 0, "NULL → 空串");

    hau_init(&s_r);
    hau_entry_t e = mk("device.health", "", "", "ok", 1000, HAU_SRC_MUSE);
    CHECK(hau_add(&s_r, &e, NULL) == 1, "第一条 seq 1");
    e = mk("hub.device.get", "th-1011121314151601", "", "not_found", 2000, HAU_SRC_REST);
    hau_add(&s_r, &e, NULL);
    e = mk("evil\"cmd\n", "th-x\"", "turn_on ", "unsupported", 3000, HAU_SRC_MUSE);
    hau_add(&s_r, &e, NULL);
    e = mk("", "", "", "ok", 4000, HAU_SRC_MUSE);
    CHECK(hau_add(&s_r, &e, NULL) == 0, "命令名空：不进环");

    cJSON *j = dump(63000, 50, sizeof(char[65536]));
    const cJSON *en = cJSON_GetObjectItemCaseSensitive(j, "entries");
    CHECK(cJSON_GetArraySize(en) == 3 && cJSON_GetObjectItemCaseSensitive(j, "count")->valueint == 3, "三条");
    const cJSON *a = cJSON_GetArrayItem(en, 0), *b = cJSON_GetArrayItem(en, 1), *c = cJSON_GetArrayItem(en, 2);
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(a, "command")->valuestring, "evil?cmd?") == 0, "新的在前、命令名净化");
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(a, "device")->valuestring, "th-x?") == 0, "设备 id 净化");
    CHECK(strstr(cJSON_GetObjectItemCaseSensitive(a, "action")->valuestring, "turn_on") == cJSON_GetObjectItemCaseSensitive(a, "action")->valuestring
          && strchr(cJSON_GetObjectItemCaseSensitive(a, "action")->valuestring, '?') != NULL, "非 ASCII 换成 ?");
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(b, "source")->valuestring, "rest") == 0
          && strcmp(cJSON_GetObjectItemCaseSensitive(b, "result")->valuestring, "not_found") == 0, "来源与结果码");
    CHECK(cJSON_GetObjectItemCaseSensitive(c, "device") == NULL && cJSON_GetObjectItemCaseSensitive(c, "action") == NULL, "没有的字段不出现");
    CHECK(cJSON_GetObjectItemCaseSensitive(c, "age_s")->valueint == 62 && cJSON_GetObjectItemCaseSensitive(c, "ms")->valueint == 7, "age_s / ms");
    CHECK(cJSON_GetObjectItemCaseSensitive(c, "time") == NULL, "没对时没有 time");
    cJSON_Delete(j);

    j = dump(63000, 0, 65536);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(j, "entries")) == 1, "limit 0 夹到 1");
    cJSON_Delete(j);

    /* 字段没有 NUL：按容量读、不越界 */
    memset(&e, 'A', sizeof(e));
    e.src = 9;
    memcpy(e.clock, "99:99", 6);
    CHECK(hau_add(&s_r, &e, NULL) != 0, "全 'A' 的条目也能进（不越界读）");
    j = dump(63000, 1, 65536);
    const cJSON *x = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j, "entries"), 0);
    CHECK(x && strlen(cJSON_GetObjectItemCaseSensitive(x, "command")->valuestring) == HAU_TOKEN_MAX - 1
          && strcmp(cJSON_GetObjectItemCaseSensitive(x, "source")->valuestring, "muse") == 0 && cJSON_GetObjectItemCaseSensitive(x, "time") == NULL,
          "截到容量、来源越界按 muse、坏时钟不记");
    cJSON_Delete(j);

    /* 满环覆盖、limit 128、小缓冲截断 */
    hau_init(&s_r);
    for (int i = 0; i < 300; i++) {
        char cmd[24];
        snprintf(cmd, sizeof(cmd), "cmd%d", i);
        e = mk(cmd, "zb-00158d0001a2b3c4", "set_brightness", "ok", (uint64_t)i, HAU_SRC_REST);
        hau_add(&s_r, &e, NULL);
    }
    j = dump(1000, 999, 65536);
    en = cJSON_GetObjectItemCaseSensitive(j, "entries");
    CHECK(cJSON_GetArraySize(en) == HAU_CAP && strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(en, 0), "command")->valuestring, "cmd299") == 0
          && strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(en, HAU_CAP - 1), "command")->valuestring, "cmd172") == 0,
          "满了覆盖最旧的：留 172..299");
    cJSON_Delete(j);
    j = dump(1000, 128, 600);
    CHECK(j && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "truncated"))
          && cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(j, "entries")) == cJSON_GetObjectItemCaseSensitive(j, "count")->valueint,
          "小缓冲：撤半条、带 truncated、count 与实际一致");
    cJSON_Delete(j);

    /* 合并：与最近 8 条里的某条相同、≤ 60 s —— 次数加一、新 seq、挪到最前 */
    hau_init(&s_r);
    bool merged = true;
#define TOP() (&s_r.e[(s_r.head + HAU_CAP - 1) % HAU_CAP])
    e = mk("hub.devices.list", "", "", "rate_limited", 1000, HAU_SRC_REST);
    CHECK(hau_add(&s_r, &e, &merged) == 1 && !merged, "第一条不算合并");
    e = mk("hub.devices.list", "", "", "rate_limited", 30000, HAU_SRC_REST);
    CHECK(hau_add(&s_r, &e, &merged) == 2 && merged && s_r.count == 1, "相同、30 s 内：合并（新 seq、不占新条）");
    e = mk("hub.devices.list", "", "", "rate_limited", 30000 + HAU_MERGE_MS, HAU_SRC_REST);
    CHECK(hau_add(&s_r, &e, &merged) == 3 && merged && s_r.count == 1, "正好 60 s：还合并（从上次合并算起）");
    j = dump(100000, 50, 65536);
    x = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j, "entries"), 0);
    CHECK(x && cJSON_GetObjectItemCaseSensitive(x, "repeat") && cJSON_GetObjectItemCaseSensitive(x, "repeat")->valueint == 3
          && cJSON_GetObjectItemCaseSensitive(x, "age_s")->valueint == 10 && cJSON_GetObjectItemCaseSensitive(x, "seq")->valueint == 3,
          "repeat 3、age_s 与 seq 按最后一次");
    cJSON_Delete(j);
    uint64_t t0 = 30000 + 2 * HAU_MERGE_MS + 1;
    e = mk("hub.devices.list", "", "", "rate_limited", t0, HAU_SRC_REST);
    CHECK(hau_add(&s_r, &e, &merged) == 4 && !merged && s_r.count == 2, "超过 60 s：新的一条");
    /* 交替的两种调用：各自合并，最后一次的挪到最前 */
    e = mk("device.health", "", "", "ok", t0 + 1, HAU_SRC_REST);
    CHECK(hau_add(&s_r, &e, &merged) == 5 && !merged, "交替：B 新的一条");
    e = mk("hub.devices.list", "", "", "rate_limited", t0 + 2, HAU_SRC_REST);
    CHECK(hau_add(&s_r, &e, &merged) == 6 && merged && s_r.count == 3 && !strcmp(TOP()->command, "hub.devices.list") && TOP()->repeat == 2,
          "交替：A 合并进第二新的那条、挪到最前");
    e = mk("device.health", "", "", "ok", t0 + 3, HAU_SRC_REST);
    CHECK(hau_add(&s_r, &e, &merged) == 7 && merged && s_r.count == 3 && !strcmp(TOP()->command, "device.health"), "交替：B 也合并");
    j = dump(t0 + 3, 50, 65536);
    en = cJSON_GetObjectItemCaseSensitive(j, "entries");
    int prev = 1 << 30, mono = 1;
    for (int i = 0; i < cJSON_GetArraySize(en); i++) {
        const int sq = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(en, i), "seq")->valueint;
        mono = mono && sq < prev;
        prev = sq;
    }
    CHECK(mono && cJSON_GetArraySize(en) == 3, "挪动之后 seq 仍从前往后递减、条数不变");
    cJSON_Delete(j);
    /* 往回只找 8 条 */
    for (int i = 0; i < HAU_MERGE_SCAN; i++) {
        char cmd[16];
        snprintf(cmd, sizeof(cmd), "c%d", i);
        e = mk(cmd, "", "", "ok", t0 + 10 + (uint64_t)i, HAU_SRC_REST);
        hau_add(&s_r, &e, NULL);
    }
    e = mk("device.health", "", "", "ok", t0 + 20, HAU_SRC_REST);
    hau_add(&s_r, &e, &merged);
    CHECK(!merged, "相同的那条在 8 条之外：不合并");
    e = mk("c1", "", "", "ok", t0 + 21, HAU_SRC_REST);    /* 现在从新到旧：device.health、c7 … c1（第 8 条）、c0（第 9 条） */
    hau_add(&s_r, &e, &merged);
    CHECK(merged && TOP()->repeat == 2, "第 8 条还在范围里：合并");
    e = mk("c0", "", "", "ok", t0 + 22, HAU_SRC_REST);
    hau_add(&s_r, &e, &merged);
    CHECK(!merged, "第 9 条不在范围里");
    /* 各维度不同都不合并 */
    hau_init(&s_r);
    e = mk("hub.device.get", "th-1011121314151602", "turn_on", "ok", 5000, HAU_SRC_MUSE);
    hau_add(&s_r, &e, NULL);
    const struct { const char *cmd, *dev, *act, *res; uint64_t t; uint8_t src; const char *why; } nm[] = {
        { "hub.device.get", "th-1011121314151602", "turn_on", "ok", 4999, HAU_SRC_MUSE, "时间倒退：不合并" },
        { "hub.device.get", "th-1011121314151602", "turn_on", "ok", 5001, HAU_SRC_REST, "来源不同：不合并" },
        { "hub.device.gex", "th-1011121314151602", "turn_on", "ok", 5002, HAU_SRC_MUSE, "命令不同：不合并" },
        { "hub.device.get", "th-1011121314151603", "turn_on", "ok", 5003, HAU_SRC_MUSE, "设备不同：不合并" },
        { "hub.device.get", "th-1011121314151602", "turn_of", "ok", 5004, HAU_SRC_MUSE, "动作不同：不合并" },
        { "hub.device.get", "th-1011121314151602", "turn_on", "forbidden", 5005, HAU_SRC_MUSE, "结果不同：不合并" },
    };
    for (size_t i = 0; i < sizeof(nm) / sizeof(nm[0]); i++) {
        hau_init(&s_r);
        e = mk("hub.device.get", "th-1011121314151602", "turn_on", "ok", 5000, HAU_SRC_MUSE);
        hau_add(&s_r, &e, NULL);
        e = mk(nm[i].cmd, nm[i].dev, nm[i].act, nm[i].res, nm[i].t, nm[i].src);
        hau_add(&s_r, &e, &merged);
        CHECK(!merged && s_r.count == 2, "%s", nm[i].why);
    }
    hau_init(&s_r);
    e = mk("hub.device.get", "th-1011121314151602", "turn_on", "ok", 5, HAU_SRC_MUSE);
    hau_add(&s_r, &e, NULL);
    TOP()->repeat = UINT16_MAX;
    e = mk("hub.device.get", "th-1011121314151602", "turn_on", "ok", 6, HAU_SRC_MUSE);
    hau_add(&s_r, &e, &merged);
    CHECK(merged && TOP()->repeat == UINT16_MAX, "repeat 到顶不回绕");
    e = mk("device.health", "", "", "ok", 7, HAU_SRC_MUSE);
    hau_add(&s_r, &e, NULL);
    j = dump(100000, 50, 65536);
    x = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j, "entries"), 0);
    CHECK(x && cJSON_GetObjectItemCaseSensitive(x, "repeat") == NULL, "没合并过的不带 repeat");
    cJSON_Delete(j);
#undef TOP

    printf("hub_audit: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
