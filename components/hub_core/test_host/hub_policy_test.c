/* hub_policy / hub_model 主机测试（docs/15 设计 6.2、6.5、14.1 权限矩阵：权限 × 动作 × 敏感、限速）。
 * 编的是产品源文件 ../hub_policy.c、../hub_model.c、../hub_registry.c、../hub_json.c。 */
#include "hub_policy.h"

#include <stdio.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) { g_pass++; } else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_caps(void)
{
    /* 附录 A hub.devices.list 的 capability 参数说明里列的 20 个名字，逐个能换回来 */
    static const char *listed[] = { "onoff", "level", "color_temp", "color", "cover", "lock", "thermostat", "temperature",
                                    "humidity", "illuminance", "pressure", "contact", "motion", "water_leak", "smoke",
                                    "vibration", "battery", "power", "energy", "button" };
    uint32_t seen = 0;
    for (size_t i = 0; i < sizeof(listed) / sizeof(listed[0]); i++) {
        uint32_t b = hub_cap_from_name(listed[i]);
        CHECK(b != 0 && (b & (b - 1)) == 0, "cap %s", listed[i]);
        CHECK(!(seen & b), "cap %s duplicated", listed[i]);
        seen |= b;
        CHECK(hub_cap_name(b) && strcmp(hub_cap_name(b), listed[i]) == 0, "name round trip %s", listed[i]);
    }
    CHECK(hub_cap_from_name("identify") == HUB_CAP_IDENTIFY, "identify");
    CHECK(hub_cap_from_name("OnOff") == 0 && hub_cap_from_name("") == 0 && hub_cap_from_name(NULL) == 0, "unknown names");
    CHECK(hub_cap_name(0) == NULL && hub_cap_name(HUB_CAP_ONOFF | HUB_CAP_LEVEL) == NULL && hub_cap_name(1u << 21) == NULL, "bad bits");
    /* 21 个名字逐个对 6.1 的枚举（审计：只钉两个时，对调 temperature / humidity 测不出来） */
    struct { const char *n; uint32_t b; } all[] = {
        { "onoff", HUB_CAP_ONOFF }, { "level", HUB_CAP_LEVEL }, { "color_temp", HUB_CAP_COLOR_TEMP }, { "color", HUB_CAP_COLOR },
        { "cover", HUB_CAP_COVER }, { "lock", HUB_CAP_LOCK }, { "thermostat", HUB_CAP_THERMOSTAT },
        { "temperature", HUB_CAP_TEMPERATURE }, { "humidity", HUB_CAP_HUMIDITY }, { "illuminance", HUB_CAP_ILLUMINANCE },
        { "pressure", HUB_CAP_PRESSURE }, { "contact", HUB_CAP_CONTACT }, { "motion", HUB_CAP_MOTION },
        { "water_leak", HUB_CAP_WATER_LEAK }, { "smoke", HUB_CAP_SMOKE }, { "vibration", HUB_CAP_VIBRATION },
        { "battery", HUB_CAP_BATTERY }, { "power", HUB_CAP_POWER }, { "energy", HUB_CAP_ENERGY },
        { "button", HUB_CAP_BUTTON }, { "identify", HUB_CAP_IDENTIFY },
    };
    CHECK(sizeof(all) / sizeof(all[0]) == HUB_CAP_COUNT, "all caps listed");
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        CHECK(hub_cap_from_name(all[i].n) == all[i].b, "cap %s -> bit", all[i].n);
        CHECK(hub_cap_name(all[i].b) && strcmp(hub_cap_name(all[i].b), all[i].n) == 0, "bit -> %s", all[i].n);
    }
}

/* 动作表：与附录 A hub.device.control 说明里列的 14 个动作一一对应；能力、值类型、敏感性与 6.2 的表一致（期望单独写） */
typedef struct { const char *name; uint32_t cap; hub_val_kind_t val; hub_sens_t sens; } act_expect_t;
static const act_expect_t k_act[] = {
    { "turn_on", HUB_CAP_ONOFF, HUB_VAL_NONE, HUB_SENS_IF_DEVICE },
    { "turn_off", HUB_CAP_ONOFF, HUB_VAL_NONE, HUB_SENS_IF_DEVICE },
    { "toggle", HUB_CAP_ONOFF, HUB_VAL_NONE, HUB_SENS_IF_DEVICE },
    { "set_brightness", HUB_CAP_LEVEL, HUB_VAL_PERCENT, HUB_SENS_IF_DEVICE },      /* MoveToLevelWithOnOff 能开能关：不拦就绕过了开关的敏感检查 */
    { "set_color_temp", HUB_CAP_COLOR_TEMP, HUB_VAL_KELVIN, HUB_SENS_IF_DEVICE },
    { "set_color", HUB_CAP_COLOR, HUB_VAL_RGB, HUB_SENS_IF_DEVICE },
    { "set_position", HUB_CAP_COVER, HUB_VAL_PERCENT, HUB_SENS_IF_DEVICE },
    { "open", HUB_CAP_COVER, HUB_VAL_NONE, HUB_SENS_IF_DEVICE },
    { "close", HUB_CAP_COVER, HUB_VAL_NONE, HUB_SENS_IF_DEVICE },
    { "stop", HUB_CAP_COVER, HUB_VAL_NONE, HUB_SENS_IF_DEVICE },
    { "lock", HUB_CAP_LOCK, HUB_VAL_NONE, HUB_SENS_NEVER },                         /* 锁上是安全方向 */
    { "unlock", HUB_CAP_LOCK, HUB_VAL_NONE, HUB_SENS_ALWAYS },
    { "set_heating_setpoint", HUB_CAP_THERMOSTAT, HUB_VAL_CELSIUS, HUB_SENS_IF_DEVICE },
    { "identify", HUB_CAP_IDENTIFY, HUB_VAL_SECONDS, HUB_SENS_IF_DEVICE },       /* IAS 警报器的所有动作（6.2） */
};

static void test_actions(void)
{
    CHECK(sizeof(k_act) / sizeof(k_act[0]) == HUB_ACT_COUNT, "action count");
    const hub_cmd_t *ctl = hub_registry_find("hub.device.control");
    for (size_t i = 0; i < sizeof(k_act) / sizeof(k_act[0]); i++) {
        const hub_action_info_t *a = hub_action_lookup(k_act[i].name);
        CHECK(a != NULL, "missing action %s", k_act[i].name);
        if (!a) continue;
        CHECK(a->cap == k_act[i].cap && a->val == k_act[i].val && a->sens == k_act[i].sens, "action %s fields", a->name);
        CHECK(hub_action_info(a->act) == a, "index/enum order for %s", a->name);
        /* 每个动作名都出现在命令说明里（说明是给模型看的唯一动作清单） */
        CHECK(ctl && strstr(ctl->description, k_act[i].name) != NULL, "%s not in hub.device.control description", k_act[i].name);
    }
    CHECK(hub_action_lookup("Unlock") == NULL && hub_action_lookup("open_door") == NULL && hub_action_lookup(NULL) == NULL, "unknown actions");
    CHECK(hub_action_info(HUB_ACT_COUNT) == NULL && hub_action_info((hub_action_t)-1) == NULL, "info out of range");
}

static void test_values(void)
{
    int32_t v;
    struct { hub_val_kind_t k; const char *in; hub_val_verdict_t want; int32_t out; } t[] = {
        { HUB_VAL_PERCENT, "40", HUB_VAL_OK, 40 }, { HUB_VAL_PERCENT, " 0 ", HUB_VAL_OK, 0 }, { HUB_VAL_PERCENT, "100", HUB_VAL_OK, 100 },
        { HUB_VAL_PERCENT, "101", HUB_VAL_RANGE, 0 }, { HUB_VAL_PERCENT, "-1", HUB_VAL_RANGE, 0 }, { HUB_VAL_PERCENT, "40.5", HUB_VAL_SYNTAX, 0 },
        { HUB_VAL_PERCENT, "4e1", HUB_VAL_SYNTAX, 0 }, { HUB_VAL_PERCENT, "0x10", HUB_VAL_SYNTAX, 0 }, { HUB_VAL_PERCENT, "", HUB_VAL_MISSING, 0 },
        { HUB_VAL_PERCENT, "   ", HUB_VAL_MISSING, 0 }, { HUB_VAL_PERCENT, NULL, HUB_VAL_MISSING, 0 }, { HUB_VAL_PERCENT, "9999999999", HUB_VAL_SYNTAX, 0 },
        { HUB_VAL_PERCENT, "+", HUB_VAL_SYNTAX, 0 }, { HUB_VAL_PERCENT, "4 0", HUB_VAL_SYNTAX, 0 },
        { HUB_VAL_KELVIN, "2700", HUB_VAL_OK, 2700 }, { HUB_VAL_KELVIN, "1500", HUB_VAL_OK, 1500 }, { HUB_VAL_KELVIN, "9000", HUB_VAL_OK, 9000 },
        { HUB_VAL_KELVIN, "1499", HUB_VAL_RANGE, 0 }, { HUB_VAL_KELVIN, "9001", HUB_VAL_RANGE, 0 }, { HUB_VAL_KELVIN, "2700K", HUB_VAL_SYNTAX, 0 },
        { HUB_VAL_RGB, "#FF8800", HUB_VAL_OK, 0xFF8800 }, { HUB_VAL_RGB, "#ff8800", HUB_VAL_OK, 0xFF8800 }, { HUB_VAL_RGB, " #000000 ", HUB_VAL_OK, 0 },
        { HUB_VAL_RGB, "FF8800", HUB_VAL_SYNTAX, 0 }, { HUB_VAL_RGB, "#FF880", HUB_VAL_SYNTAX, 0 }, { HUB_VAL_RGB, "#GG8800", HUB_VAL_SYNTAX, 0 },
        { HUB_VAL_RGB, "#FF88000", HUB_VAL_SYNTAX, 0 }, { HUB_VAL_RGB, NULL, HUB_VAL_MISSING, 0 },
        { HUB_VAL_CELSIUS, "21.5", HUB_VAL_OK, 2150 }, { HUB_VAL_CELSIUS, "21", HUB_VAL_OK, 2100 }, { HUB_VAL_CELSIUS, "21.25", HUB_VAL_OK, 2125 },
        { HUB_VAL_CELSIUS, "5", HUB_VAL_OK, 500 }, { HUB_VAL_CELSIUS, "30.0", HUB_VAL_OK, 3000 }, { HUB_VAL_CELSIUS, "30.01", HUB_VAL_RANGE, 0 },
        { HUB_VAL_CELSIUS, "4.99", HUB_VAL_RANGE, 0 }, { HUB_VAL_CELSIUS, "-10", HUB_VAL_RANGE, 0 }, { HUB_VAL_CELSIUS, "21.125", HUB_VAL_SYNTAX, 0 },
        { HUB_VAL_CELSIUS, ".5", HUB_VAL_SYNTAX, 0 }, { HUB_VAL_CELSIUS, "21.", HUB_VAL_SYNTAX, 0 }, { HUB_VAL_CELSIUS, "21,5", HUB_VAL_SYNTAX, 0 },
        { HUB_VAL_CELSIUS, "1234567", HUB_VAL_SYNTAX, 0 },
        { HUB_VAL_SECONDS, NULL, HUB_VAL_OK, 10 }, { HUB_VAL_SECONDS, "", HUB_VAL_OK, 10 }, { HUB_VAL_SECONDS, "1", HUB_VAL_OK, 1 },
        { HUB_VAL_SECONDS, "60", HUB_VAL_OK, 60 }, { HUB_VAL_SECONDS, "0", HUB_VAL_RANGE, 0 }, { HUB_VAL_SECONDS, "61", HUB_VAL_RANGE, 0 },
        { HUB_VAL_NONE, "anything", HUB_VAL_OK, 0 }, { HUB_VAL_NONE, NULL, HUB_VAL_OK, 0 },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        v = -12345;
        hub_val_verdict_t r = hub_val_parse(t[i].k, t[i].in, &v);
        CHECK(r == t[i].want, "value #%zu (%s): verdict %d want %d", i, t[i].in ? t[i].in : "NULL", r, t[i].want);
        if (r == HUB_VAL_OK && t[i].want == HUB_VAL_OK) CHECK(v == t[i].out, "value #%zu: out %d want %d", i, (int)v, (int)t[i].out);
    }
    CHECK(hub_val_parse(HUB_VAL_PERCENT, "5", NULL) == HUB_VAL_SYNTAX, "NULL out");
    CHECK(hub_val_parse((hub_val_kind_t)99, "5", &v) == HUB_VAL_SYNTAX, "unknown kind");
}

/* 权限矩阵：8 种权限位 × 14 个动作 × 设备敏感与否 × 来源；能力单独测（期望按 6.5 的规则独立写） */
static void test_perm_matrix(void)
{
    for (unsigned perm = 0; perm < 8; perm++) {
        for (size_t i = 0; i < sizeof(k_act) / sizeof(k_act[0]); i++) {
            const hub_action_info_t *a = hub_action_lookup(k_act[i].name);
            for (int ds = 0; ds < 2; ds++) {
                bool sensitive = k_act[i].sens == HUB_SENS_ALWAYS || (k_act[i].sens == HUB_SENS_IF_DEVICE && ds);
                hub_chk_t want = !(perm & HUB_PERMBIT_CONTROL) ? HUB_CHK_NO_PERM
                               : (sensitive && !(perm & HUB_PERMBIT_SENSITIVE)) ? HUB_CHK_SENSITIVE : HUB_CHK_OK;
                hub_chk_t got = hub_policy_check_action(a, (uint8_t)perm, HUB_SRC_MUSE, ds, 0xFFFFFFFFu);
                CHECK(got == want, "muse perm %u %s dev_sens %d: %s want %s", perm, a->name, ds, hub_chk_str(got), hub_chk_str(want));
                CHECK(hub_action_is_sensitive(a, ds) == sensitive, "is_sensitive %s %d", a->name, ds);
                /* 本地 REST：不看权限位 */
                CHECK(hub_policy_check_action(a, (uint8_t)perm, HUB_SRC_REST, ds, 0xFFFFFFFFu) == HUB_CHK_OK, "rest bypasses perm %s", a->name);
                /* 能力不够：两个来源都拒（Muse 先过了权限才轮到能力） */
                hub_chk_t nocap = hub_policy_check_action(a, (uint8_t)perm, HUB_SRC_REST, ds, ~a->cap);
                CHECK(nocap == HUB_CHK_NO_CAP, "rest no cap %s", a->name);
                if (want == HUB_CHK_OK) CHECK(hub_policy_check_action(a, (uint8_t)perm, HUB_SRC_MUSE, ds, ~a->cap) == HUB_CHK_NO_CAP, "muse no cap %s", a->name);
            }
        }
    }
    /* 审计 A1 的绕过：设备被标敏感时，set_brightness 与 turn_on 一样被拦 */
    CHECK(hub_policy_check_action(hub_action_lookup("set_brightness"), HUB_PERM_DEFAULT, HUB_SRC_MUSE, true, HUB_CAP_LEVEL) == HUB_CHK_SENSITIVE,
          "set_brightness on a sensitive device needs SENSITIVE");
    for (int i = 0; i < HUB_ACT_COUNT; i++) {
        const hub_action_info_t *a = hub_action_info((hub_action_t)i);
        if (a->act != HUB_ACT_LOCK) CHECK(hub_action_is_sensitive(a, true), "%s must be sensitive on a sensitive device", a->name);
    }
    /* 默认权限（SENSITIVE 关）：unlock 被拒，lock 放行 —— TC-MU12 的判据 */
    CHECK(hub_policy_check_action(hub_action_lookup("unlock"), HUB_PERM_DEFAULT, HUB_SRC_MUSE, false, HUB_CAP_LOCK) == HUB_CHK_SENSITIVE, "unlock refused by default");
    CHECK(hub_policy_check_action(hub_action_lookup("lock"), HUB_PERM_DEFAULT, HUB_SRC_MUSE, false, HUB_CAP_LOCK) == HUB_CHK_OK, "lock allowed by default");
    CHECK(hub_policy_check_action(NULL, 0xFF, HUB_SRC_MUSE, false, 0) == HUB_CHK_BAD_ARG, "NULL action");
    CHECK(hub_policy_check_action(hub_action_lookup("lock"), 0xFF, (hub_src_t)7, false, HUB_CAP_LOCK) == HUB_CHK_BAD_ARG, "bad source");
    CHECK(hub_action_is_sensitive(NULL, false), "NULL action counts as sensitive");

    /* 命令级：权限位 × 来源 */
    for (size_t i = 0; i < hub_registry_count(); i++) {
        const hub_cmd_t *c = hub_registry_at(i);
        for (unsigned perm = 0; perm < 8; perm++) {
            bool ok = c->perm == HUB_PERM_READ || (c->perm == HUB_PERM_CONTROL && (perm & HUB_PERMBIT_CONTROL))
                      || (c->perm == HUB_PERM_MANAGE && (perm & HUB_PERMBIT_MANAGE));
            CHECK(hub_policy_check_cmd(c, (uint8_t)perm, HUB_SRC_MUSE) == (ok ? HUB_CHK_OK : HUB_CHK_NO_PERM), "cmd %s perm %u", c->name, perm);
            CHECK(hub_policy_check_cmd(c, (uint8_t)perm, HUB_SRC_REST) == HUB_CHK_OK, "rest cmd %s", c->name);
            /* 执行检查与注册过滤一致：后端与就绪位都在时，注册了的就放行 */
            CHECK(hub_cmd_available(c, (uint8_t)perm, HUB_BACKEND_ALL) == ok, "available vs check %s", c->name);
        }
    }
    CHECK(hub_policy_check_cmd(NULL, 0xFF, HUB_SRC_MUSE) == HUB_CHK_BAD_ARG, "NULL cmd");
}

/* 限速：6.5 的表 */
static void test_rate_limits(void)
{
    struct { hub_rl_class_t c; uint16_t cap; uint32_t period; } spec[] = {
        { HUB_RL_READ, 5, 500 }, { HUB_RL_CONTROL, 10, 200 }, { HUB_RL_UPDATE, 10, 6000 },
        { HUB_RL_PERMIT_JOIN, 1, 60000 }, { HUB_RL_COMMISSION, 1, 60000 },
    };
    for (size_t i = 0; i < sizeof(spec) / sizeof(spec[0]); i++) {
        hub_rl_spec_t s = hub_rl_class_spec(spec[i].c);
        CHECK(s.capacity == spec[i].cap && s.period_ms == spec[i].period, "spec class %d", (int)spec[i].c);
    }
    CHECK(hub_rl_total_spec().capacity == 10 && hub_rl_total_spec().period_ms == 100, "total spec");
    CHECK(hub_rl_class_spec(HUB_RL_CLASS_COUNT).capacity == 0, "spec out of range");

    hub_rl_t rl;
    uint64_t t = 1000000;
    /* 读：突发 5 次，第 6 次拒；500 ms 后恢复 1 次 */
    hub_rl_init(&rl, t);
    for (int i = 0; i < 5; i++) CHECK(hub_rl_take(&rl, HUB_RL_READ, t), "read burst %d", i);
    CHECK(!hub_rl_take(&rl, HUB_RL_READ, t), "read 6th refused");
    CHECK(!hub_rl_take(&rl, HUB_RL_READ, t + 499), "read before period");
    CHECK(hub_rl_take(&rl, HUB_RL_READ, t + 500), "read after 500 ms");
    CHECK(!hub_rl_take(&rl, HUB_RL_READ, t + 500), "only one refilled");
    /* 频繁调用不吃零头：每 100 ms 试一次，第 1000 ms 时（自上次放行 500 ms）恰好放行 */
    bool got = false;
    for (uint64_t k = t + 600; k < t + 1000; k += 100) got |= hub_rl_take(&rl, HUB_RL_READ, k);
    CHECK(!got, "no token before 500 ms elapsed");
    CHECK(hub_rl_take(&rl, HUB_RL_READ, t + 1000), "fractional time kept across frequent calls");
    /* 补令牌时带余数：1700 ms 补 1 个（参考点到 1500，余 200 ms 留着），2000 ms 应再补 1 个 */
    CHECK(hub_rl_take(&rl, HUB_RL_READ, t + 1700), "refill at 1700");
    CHECK(hub_rl_take(&rl, HUB_RL_READ, t + 2000), "remainder kept when a refill happens (ref advanced by whole periods)");

    /* 退还（MX21）：配网 1 次 / 60 s，当场拒了还回去就能马上再来；退还不超过容量、总桶也退 */
    hub_rl_init(&rl, t);
    CHECK(hub_rl_take(&rl, HUB_RL_COMMISSION, t), "commission 1st");
    CHECK(!hub_rl_take(&rl, HUB_RL_COMMISSION, t + 1), "commission 2nd refused");
    hub_rl_refund(&rl, HUB_RL_COMMISSION, t + 2);
    CHECK(hub_rl_take(&rl, HUB_RL_COMMISSION, t + 3), "after refund: allowed again");
    hub_rl_refund(&rl, HUB_RL_COMMISSION, t + 4);
    hub_rl_refund(&rl, HUB_RL_COMMISSION, t + 5);
    CHECK(hub_rl_take(&rl, HUB_RL_COMMISSION, t + 6) && !hub_rl_take(&rl, HUB_RL_COMMISSION, t + 7), "refund capped at capacity (1)");
    hub_rl_init(&rl, t);
    for (int i = 0; i < 10; i++) (void)hub_rl_take(&rl, HUB_RL_CONTROL, t);
    CHECK(!hub_rl_take(&rl, HUB_RL_READ, t), "total bucket empty");
    hub_rl_refund(&rl, HUB_RL_CONTROL, t);
    CHECK(hub_rl_take(&rl, HUB_RL_READ, t), "refund also returns the total-bucket token");
    hub_rl_refund(NULL, HUB_RL_READ, t);
    hub_rl_refund(&rl, HUB_RL_CLASS_COUNT, t);
    CHECK(true, "refund with bad args does nothing");

    /* permit_join：每 60 s 1 次 */
    hub_rl_init(&rl, t);
    CHECK(hub_rl_take(&rl, HUB_RL_PERMIT_JOIN, t), "permit 1st");
    CHECK(!hub_rl_take(&rl, HUB_RL_PERMIT_JOIN, t + 59999), "permit within 60 s");
    CHECK(hub_rl_take(&rl, HUB_RL_PERMIT_JOIN, t + 60000), "permit after 60 s");
    CHECK(hub_rl_take(&rl, HUB_RL_COMMISSION, t + 60000), "commission has its own bucket");

    /* update：每分钟 10 次 */
    hub_rl_init(&rl, t);
    int n = 0;
    for (int i = 0; i < 20; i++) n += hub_rl_take(&rl, HUB_RL_UPDATE, t + (uint64_t)i * 100);
    CHECK(n == 10, "update 10 per minute burst (%d)", n);
    CHECK(hub_rl_take(&rl, HUB_RL_UPDATE, t + 6000), "update refill after 6 s");

    /* 合计：每秒 10 次，跨类别共享；被类别桶拒的不扣合计 */
    hub_rl_init(&rl, t);
    n = 0;
    for (int i = 0; i < 10; i++) n += hub_rl_take(&rl, HUB_RL_CONTROL, t);
    CHECK(n == 10, "control burst 10");
    CHECK(!hub_rl_take(&rl, HUB_RL_READ, t), "total exhausted blocks other classes");
    hub_rl_init(&rl, t);
    for (int i = 0; i < 5; i++) (void)hub_rl_take(&rl, HUB_RL_READ, t);
    for (int i = 0; i < 20; i++) (void)hub_rl_take(&rl, HUB_RL_READ, t);     /* 全被读桶拒：不该扣合计 */
    n = 0;
    for (int i = 0; i < 10; i++) n += hub_rl_take(&rl, HUB_RL_CONTROL, t);
    CHECK(n == 5, "denied calls do not consume the total bucket (%d of remaining 5)", n);

    /* 长时间空闲：只补到容量 */
    hub_rl_init(&rl, t);
    for (int i = 0; i < 10; i++) (void)hub_rl_take(&rl, HUB_RL_CONTROL, t);
    n = 0;
    for (int i = 0; i < 30; i++) n += hub_rl_take(&rl, HUB_RL_CONTROL, t + 3600000);
    CHECK(n == 10, "refill capped at capacity (%d)", n);
    /* 时间倒退：当场不给，时间回到原处也不多给（参考点不跟着倒退） */
    hub_rl_init(&rl, t);
    for (int i = 0; i < 5; i++) (void)hub_rl_take(&rl, HUB_RL_READ, t);
    CHECK(!hub_rl_take(&rl, HUB_RL_READ, t - 100000), "clock going backwards gives nothing");
    CHECK(!hub_rl_take(&rl, HUB_RL_READ, t), "and nothing extra when the clock comes back");
    CHECK(hub_rl_take(&rl, HUB_RL_READ, t + 500), "normal refill afterwards");
    CHECK(!hub_rl_take(NULL, HUB_RL_READ, t) && !hub_rl_take(&rl, HUB_RL_CLASS_COUNT, t), "bad args");
    hub_rl_init(NULL, t);
}

int main(void)
{
    test_caps();
    test_actions();
    test_values();
    test_perm_matrix();
    test_rate_limits();
    printf("hub_policy: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
