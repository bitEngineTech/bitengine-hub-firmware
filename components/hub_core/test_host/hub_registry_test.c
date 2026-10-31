/* hub_registry 主机测试（docs/15 设计 6.3–6.5、附录 A、14.1）。编的是产品源文件 ../hub_registry.c、../hub_json.c。
 *   hub_registry_test            跑断言
 *   hub_registry_test --emit     打印权限全开、后端全在时的 commands_v2（check_commands_v2.py 拿它和附录 A 比） */
#include "hub_registry.h"

#include <stdio.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) { g_pass++; } else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char g_buf[16384];

static int emit(uint8_t perm, uint8_t backends, size_t cap)
{
    hj_t w;
    hj_init(&w, g_buf, cap);
    hub_registry_emit_commands_v2(&w, perm, backends);
    return hj_finish(&w);
}

/* 期望表与实现分开写：哪条命令要哪个权限位、哪个后端（6.4 与附录 A 的注册规则） */
typedef struct { const char *name; uint8_t permbit; uint8_t needs; hub_rl_class_t rl; } expect_t;
#define ANY 0x80
static const expect_t k_expect[] = {
    { "device.health",          0,                   0,                 HUB_RL_READ },
    { "hub.devices.list",       0,                   0,                 HUB_RL_READ },
    { "hub.device.get",         0,                   0,                 HUB_RL_READ },
    { "hub.thread.network",     0,                   0,                 HUB_RL_READ },
    { "hub.events.recent",      0,                   0,                 HUB_RL_READ },
    { "hub.device.control",     HUB_PERMBIT_CONTROL, ANY | HUB_BACKEND_CONTROL, HUB_RL_CONTROL },
    { "hub.device.update",      HUB_PERMBIT_MANAGE,  ANY,               HUB_RL_UPDATE },
    { "hub.zigbee.permit_join", HUB_PERMBIT_MANAGE,  HUB_BACKEND_ZHUB | HUB_BACKEND_ZB_JOIN, HUB_RL_PERMIT_JOIN },
    { "hub.matter.commission",  HUB_PERMBIT_MANAGE,  HUB_BACKEND_MCTRL, HUB_RL_COMMISSION },
};
#define N_EXPECT (sizeof(k_expect) / sizeof(k_expect[0]))

static bool expect_available(const expect_t *e, uint8_t perm, uint8_t backends)
{
    if (e->permbit && !(perm & e->permbit)) return false;
    if ((e->needs & ANY) && !(backends & (HUB_BACKEND_ZHUB | HUB_BACKEND_MCTRL))) return false;
    const uint8_t rest = (uint8_t)(e->needs & ~ANY);
    return (backends & rest) == rest;
}

static void test_table(void)
{
    CHECK(hub_registry_count() == N_EXPECT, "count %zu", hub_registry_count());
    CHECK(hub_registry_at(hub_registry_count()) == NULL, "at() out of range");
    for (size_t i = 0; i < N_EXPECT; i++) {
        const hub_cmd_t *c = hub_registry_find(k_expect[i].name);
        CHECK(c != NULL, "missing %s", k_expect[i].name);
        if (!c) continue;
        CHECK(c == hub_registry_at(i), "%s out of appendix-A order", c->name);
        CHECK(strchr(c->description, '%') == NULL, "%s: description has %%", c->name);   /* 6.8 */
        CHECK(c->timeout_ms > 1000 && c->timeout_ms <= 180000, "%s: timeout %u (job deadline = timeout - 1 s, 6.6)", c->name, (unsigned)c->timeout_ms);
        for (uint8_t k = 0; k < c->n_required; k++) CHECK(strchr(c->required[k].description, '%') == NULL, "%s param has %%", c->name);
        for (uint8_t k = 0; k < c->n_optional; k++) CHECK(strchr(c->optional[k].description, '%') == NULL, "%s param has %%", c->name);
        CHECK(c->handler == NULL, "%s: M3 step 2 has no handlers yet", c->name);   /* 接 handler 时改这条 */
        CHECK(c->rl == k_expect[i].rl, "%s: rate-limit class %d", c->name, (int)c->rl);
        CHECK((c->perm == HUB_PERM_READ) == (c->rl == HUB_RL_READ), "%s: read perm <-> read rate class", c->name);
    }
    CHECK(hub_registry_find(NULL) == NULL && hub_registry_find("hub.device") == NULL && hub_registry_find("") == NULL, "find misses");
    /* 永不注册（6.4、4.13）：管理命令、OTA、发现、上游 ble.* */
    static const char *never[] = { "device.ota", "device.discover", "device.unpair", "device.list_vms", "device.set_vm",
                                   "device.reset_vm", "ble.scan", "link.register" };
    for (size_t i = 0; i < sizeof(never) / sizeof(never[0]); i++) CHECK(hub_registry_find(never[i]) == NULL, "%s must not be in the table", never[i]);
    for (size_t i = 0; i < hub_registry_count(); i++) {
        const char *n = hub_registry_at(i)->name;
        CHECK(strncmp(n, "ble.", 4) != 0 && strstr(n, "ota") == NULL && strstr(n, "remove") == NULL && strstr(n, "reset") == NULL
              && strstr(n, "key") == NULL, "forbidden-looking command %s", n);
    }
}

/* 过滤矩阵：8 种权限位 × 16 种后端组合（zhub / matter_ctrl / 控制就绪 / 入网就绪），每条命令出不出现与期望表一致；输出永远是合法对象 */
static void test_filter_matrix(void)
{
    for (unsigned perm = 0; perm < 8; perm++) {
        for (unsigned be = 0; be < 16; be++) {
            int n = emit((uint8_t)perm, (uint8_t)be, sizeof(g_buf));
            CHECK(n > 0, "emit perm %u be %u", perm, be);
            for (size_t i = 0; i < N_EXPECT; i++) {
                char key[64];
                snprintf(key, sizeof(key), "\"%s\":{\"description\"", k_expect[i].name);
                bool present = strstr(g_buf, key) != NULL;
                bool want = expect_available(&k_expect[i], (uint8_t)perm, (uint8_t)be);
                CHECK(present == want, "perm %u be %u: %s present=%d want=%d", perm, be, k_expect[i].name, present, want);
                CHECK(hub_cmd_available(hub_registry_find(k_expect[i].name), (uint8_t)perm, (uint8_t)be) == want, "available() %s", k_expect[i].name);
            }
        }
    }
    /* M3 出厂：默认权限、没有 zhub / matter_ctrl → 正好 5 条读命令 */
    int n = emit(HUB_PERM_DEFAULT, 0, sizeof(g_buf));
    int cnt = 0;
    for (const char *p = g_buf; (p = strstr(p, "\"description\"")); p++) cnt++;
    /* 每条命令 1 个 description + 每个参数 1 个：5 条读命令共 1+0 + 1+4 + 1+1 + 1+0 + 1+2 = 12 */
    CHECK(n > 0 && cnt == 12, "M3 default emits only the 5 read commands (descriptions %d)", cnt);
    CHECK(strstr(g_buf, "hub.device.control") == NULL && strstr(g_buf, "permit_join") == NULL, "no control in M3 default");
    CHECK(HUB_PERM_DEFAULT == (HUB_PERMBIT_CONTROL | HUB_PERMBIT_MANAGE) && !(HUB_PERM_DEFAULT & HUB_PERMBIT_SENSITIVE), "SENSITIVE off by default (6.5)");
    CHECK(!hub_cmd_available(NULL, 0xFF, 0xFF), "available(NULL)");
    /* M5 第三步（CHECKLIST C17）：默认权限、只有 zhub 在跑（控制路径与入网前提都没做）→ 5 条读命令 + hub.device.update；
       update 的说明 1 + 参数 4（id / name / room / notify）= 5 个 description */
    n = emit(HUB_PERM_DEFAULT, HUB_BACKEND_ZHUB, sizeof(g_buf));
    cnt = 0;
    for (const char *p = g_buf; (p = strstr(p, "\"description\"")); p++) cnt++;
    CHECK(n > 0 && cnt == 17 && strstr(g_buf, "\"hub.device.update\"") != NULL, "zhub only: reads + update (descriptions %d)", cnt);
    CHECK(strstr(g_buf, "hub.device.control") == NULL && strstr(g_buf, "permit_join") == NULL, "zhub only: no control / permit_join yet");
    CHECK(HUB_BACKEND_ALL == 0x0Fu, "HUB_BACKEND_ALL covers every readiness bit");
}

/* 体积：全开时紧凑 JSON ≤ 6 KB（设计 4.7：整条注册要钉在 7 KB 内，外层 params 约 0.3 KB；整条在 muse_link 的 muse_proto_test 里钉） */
static void test_size(void)
{
    int n = emit(HUB_PERMBIT_ALL, HUB_BACKEND_ALL, sizeof(g_buf));
    CHECK(n > 0 && n <= 6144, "full commands_v2 %d bytes (limit 6144)", n);
    /* 缓冲不够时截断但仍可 finish（调用方据 truncated 判注册失败，而不是发半张表） */
    hj_t w;
    hj_init(&w, g_buf, 1000);
    hub_registry_emit_commands_v2(&w, HUB_PERMBIT_ALL, HUB_BACKEND_ALL);
    CHECK(w.truncated, "small buffer -> truncated flag set");
    hub_registry_emit_commands_v2(NULL, 0, 0);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--emit") == 0) {
        int n = emit(HUB_PERMBIT_ALL, HUB_BACKEND_ALL, sizeof(g_buf));
        if (n < 0) return 1;
        fputs(g_buf, stdout);
        return 0;
    }
    test_table();
    test_filter_matrix();
    test_size();
    printf("hub_registry: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
