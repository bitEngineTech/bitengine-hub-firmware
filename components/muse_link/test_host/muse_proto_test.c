/* muse_proto 主机测试（docs/15 设计 4.2 注 3、4.7、4.8、4.13、14.1 ②③④）。
 * 编的是产品源文件 ../muse_proto.c 与 hub_core 的 hub_json.c / hub_registry.c，cJSON 用仓库里的 managed_components。 */
#include "hub_params.h"
#include "muse_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) { g_pass++; } else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const mp_reg_info_t k_info = { "homelink-a1b2c3", "bitEngine Hub \"Lab\" 1234", "v1.0.0-12-gdeadbee", "homehub", "bitengine-hub-c5" };
static char g_buf[70000];

static void test_uuid(void)
{
    uint8_t r[16];
    memset(r, 0xFF, sizeof(r));
    char u[MP_UUID_LEN + 1];
    mp_format_uuid(r, u);
    CHECK(strlen(u) == 36 && u[8] == '-' && u[13] == '-' && u[18] == '-' && u[23] == '-', "uuid shape %s", u);
    CHECK(u[14] == '4' && (u[19] == '8' || u[19] == '9' || u[19] == 'a' || u[19] == 'b'), "uuid v4 variant %s", u);
    memset(r, 0, sizeof(r));
    mp_format_uuid(r, u);
    CHECK(strcmp(u, "00000000-0000-4000-8000-000000000000") == 0, "uuid zero %s", u);
    mp_format_uuid(NULL, u);
    CHECK(u[0] == 0, "uuid NULL rnd");
}

static int count_keys(const cJSON *o) { int n = 0; const cJSON *c; cJSON_ArrayForEach(c, o) n++; return n; }

static void test_register(void)
{
    const char *rid = "11111111-2222-4333-8444-555555555555";
    int n = mp_build_register(g_buf, sizeof(g_buf), rid, &k_info, HUB_PERM_DEFAULT, 0);
    CHECK(n > 0, "register M3 default");
    cJSON *j = cJSON_Parse(g_buf);
    CHECK(j != NULL, "register parses");
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(j, "params");
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(j, "type")->valuestring, "req") == 0, "type req");
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(j, "id")->valuestring, rid) == 0, "id");
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(j, "method")->valuestring, "link.register") == 0, "method");
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(p, "node_id")->valuestring, k_info.node_id) == 0, "node_id");
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(p, "display_name")->valuestring, k_info.display_name) == 0, "display_name escaped round trip");
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(p, "platform")->valuestring, "esp32") == 0, "platform");
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(p, "version")->valuestring, k_info.version) == 0, "version");
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(p, "device_family")->valuestring, "homehub") == 0, "device_family");
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(p, "model_id")->valuestring, "bitengine-hub-c5") == 0, "model_id");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p, "is_wakeup_supported")), "is_wakeup_supported false");
    CHECK(cJSON_GetObjectItemCaseSensitive(p, "metadata") == NULL, "no metadata key (no SSID; same as both upstreams)");
    const cJSON *cv = cJSON_GetObjectItemCaseSensitive(p, "commands_v2");
    CHECK(cJSON_IsObject(cv) && count_keys(cv) == 5, "M3 default registers 5 read commands (%d)", count_keys(cv));
    CHECK(count_keys(p) == 8, "params has exactly 8 keys (%d)", count_keys(p));
    cJSON_Delete(j);

    /* 全开：≤ 7 KB，9 条；永不注册的命令都不在 */
    n = mp_build_register(g_buf, sizeof(g_buf), rid, &k_info, HUB_PERMBIT_ALL, HUB_BACKEND_ALL);
    CHECK(n > 0 && n <= MP_REGISTER_MAX, "full register %d bytes (limit %d)", n, MP_REGISTER_MAX);
    j = cJSON_Parse(g_buf);
    cv = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "params"), "commands_v2");
    CHECK(count_keys(cv) == 9, "full register has 9 commands");
    static const char *never[] = { "device.unpair", "device.list_vms", "device.set_vm", "device.reset_vm", "device.ota", "device.discover" };
    for (size_t i = 0; i < sizeof(never) / sizeof(never[0]); i++) CHECK(!cJSON_GetObjectItemCaseSensitive(cv, never[i]), "%s registered", never[i]);
    cJSON_Delete(j);
    CHECK(strstr(g_buf, "network_ssid") == NULL && strstr(g_buf, "ssid") == NULL, "no SSID anywhere");

    /* cap 超过 7 KB 也按 7 KB 算：结果与 cap = 7169 时相同 */
    char small[MP_REGISTER_MAX + 1];
    int n2 = mp_build_register(small, sizeof(small), rid, &k_info, HUB_PERMBIT_ALL, HUB_BACKEND_ALL);
    CHECK(n2 == n && memcmp(small, g_buf, (size_t)n) == 0, "cap clamp");
    /* 放不下：-1，缓冲清空（不发半张表） */
    char tiny[2000];
    CHECK(mp_build_register(tiny, sizeof(tiny), rid, &k_info, HUB_PERMBIT_ALL, HUB_BACKEND_ALL) == -1 && tiny[0] == 0, "too small -> -1, no half table");
    /* 缺字段 */
    mp_reg_info_t bad = k_info;
    bad.node_id = "";
    CHECK(mp_build_register(g_buf, sizeof(g_buf), rid, &bad, 0, 0) == -1, "empty node_id");
    bad = k_info; bad.device_family = NULL;
    CHECK(mp_build_register(g_buf, sizeof(g_buf), rid, &bad, 0, 0) == -1, "NULL family");
    bad = k_info; bad.display_name = "";
    CHECK(mp_build_register(g_buf, sizeof(g_buf), rid, &bad, 0, 0) > 0, "empty display name allowed");
    CHECK(mp_build_register(g_buf, sizeof(g_buf), "", &k_info, 0, 0) == -1, "empty req id");
    CHECK(mp_build_register(g_buf, sizeof(g_buf), rid, NULL, 0, 0) == -1, "NULL info");
    CHECK(mp_build_register(NULL, 10, rid, &k_info, 0, 0) == -1, "NULL buf");
}

static mp_reg_reply_t classify(const char *json, const char *rid, bool *hb, char *why)
{
    cJSON *j = cJSON_Parse(json);
    mp_reg_reply_t r = mp_classify_register_reply(j, rid, hb, why, 64);
    cJSON_Delete(j);
    return r;
}

static void test_register_reply(void)
{
    const char *rid = "abc";
    bool hb;
    char why[64];
    struct { const char *json; mp_reg_reply_t want; bool hb; const char *why; } t[] = {
        { "{\"type\":\"res\",\"id\":\"abc\",\"result\":{\"status\":\"registered\"}}", MP_REG_ACKED, true, "" },
        { "{\"type\":\"res\",\"id\":\"abc\",\"result\":{\"status\":\"pending\"}}", MP_REG_ACKED, false, "" },
        { "{\"type\":\"res\",\"id\":\"abc\",\"error\":null,\"result\":{\"status\":\"registered\"}}", MP_REG_ACKED, false, "" },   /* 上游：有 error 键就不发心跳 */
        { "{\"type\":\"res\",\"id\":\"abc\",\"error\":false,\"result\":{\"status\":\"registered\"}}", MP_REG_ACKED, false, "" },
        { "{\"type\":\"event\",\"id\":\"abc\",\"result\":{\"status\":\"registered\"}}", MP_REG_ACKED, false, "" },              /* type 不是 res */
        { "{\"type\":\"res\",\"id\":\"abc\",\"error\":{\"code\":\"unsupported_version\"}}", MP_REG_REJECTED_PROTOCOL, false, "unsupported_version" },
        { "{\"type\":\"res\",\"id\":\"abc\",\"error\":{\"code\":\"quota\",\"message\":\"try later\"}}", MP_REG_REJECTED, false, "try later" },
        { "{\"type\":\"res\",\"id\":\"abc\",\"error\":{\"code\":\"unsupported_version\",\"message\":\"please upgrade\"}}", MP_REG_REJECTED_PROTOCOL, false, "please upgrade" },
        { "{\"id\":\"abc\"}", MP_REG_ACKED, false, "" },
        { "{\"type\":\"res\",\"id\":\"abd\",\"result\":{\"status\":\"registered\"}}", MP_REG_NOT_MINE, false, "" },
        { "{\"type\":\"res\",\"ID\":\"abc\"}", MP_REG_NOT_MINE, false, "" },
        { "[1]", MP_REG_NOT_MINE, false, "" },
        { "{\"type\":\"res\",\"id\":\"abc\",\"error\":\"tunnel quota exceeded\"}", MP_REG_REJECTED, false, "tunnel quota exceeded" },
        { "{\"type\":\"res\",\"id\":\"abc\",\"error\":{\"code\":\"x\",\"message\":\"Unsupported device_family\"}}", MP_REG_REJECTED_PROTOCOL, false, "Unsupported device_family" },
        { "{\"type\":\"res\",\"id\":\"abc\",\"error\":\"protocol version too old\"}", MP_REG_REJECTED_PROTOCOL, false, "protocol version too old" },
        { "{\"type\":\"res\",\"id\":\"abc\",\"ok\":false}", MP_REG_REJECTED, false, "ok:false" },
        { "{\"type\":\"res\",\"id\":\"abc\",\"ok\":false,\"result\":{\"status\":\"registered\"}}", MP_REG_REJECTED, false, "ok:false" },
        { "{\"type\":\"res\",\"id\":\"abc\",\"error\":\"\"}", MP_REG_REJECTED, false, "" },
        { "{\"type\":\"res\",\"id\":\"abc\",\"error\":{\"code\":\"x\"}}", MP_REG_REJECTED, false, "x" },              /* 没 message 用 code */
        { "{\"type\":\"res\",\"id\":\"abc\",\"error\":{\"detail\":\"x\"}}", MP_REG_REJECTED, false, "no reason given" },
        { "{\"type\":\"res\",\"id\":\"abc\",\"error\":7}", MP_REG_REJECTED, false, "no reason given" },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        hb = !t[i].hb;
        mp_reg_reply_t r = classify(t[i].json, rid, &hb, why);
        CHECK(r == t[i].want, "reply #%zu: %d want %d", i, r, t[i].want);
        CHECK(hb == t[i].hb, "reply #%zu: heartbeat %d", i, hb);
        CHECK(strcmp(why, t[i].why) == 0, "reply #%zu: reason [%s] want [%s]", i, why, t[i].why);
    }
    CHECK(classify("{\"id\":\"abc\"}", "", &hb, why) == MP_REG_NOT_MINE, "empty req id");
    CHECK(mp_classify_register_reply(NULL, rid, NULL, NULL, 0) == MP_REG_NOT_MINE, "NULL msg, NULL outs");
}

static cJSON *g_keep;   /* inbound() 解析出的树要活到断言看完 inv 的指针 */
static mp_in_kind_t inbound(const char *json, mp_invoke_t *inv)
{
    cJSON_Delete(g_keep);
    g_keep = cJSON_Parse(json);
    return mp_classify_inbound(g_keep, inv);
}

static void test_inbound(void)
{
    mp_invoke_t inv;
    CHECK(inbound("{\"type\":\"event\",\"event\":\"link.unpaired\"}", &inv) == MP_IN_UNPAIRED, "link.unpaired");
    CHECK(inbound("{\"type\":\"event\",\"event\":\"node.unpaired\"}", &inv) == MP_IN_UNPAIRED, "node.unpaired");
    CHECK(inbound("{\"type\":\"event\",\"event\":\"agent.status\"}", &inv) == MP_IN_IGNORE, "other event");
    CHECK(inbound("{\"type\":\"event\",\"event\":\"link.unpaired\",\"method\":\"link.invoke\",\"id\":\"1\",\"command\":\"x\"}", &inv) == MP_IN_UNPAIRED, "event wins");
    CHECK(inbound("{\"method\":\"link.invoke\",\"id\":\"r1\",\"command\":\"hub.device.get\",\"params\":{\"id\":\"zb-1\"}}", &inv) == MP_IN_INVOKE
          && strcmp(inv.id, "r1") == 0 && strcmp(inv.command, "hub.device.get") == 0 && cJSON_IsObject(inv.params), "invoke");
    CHECK(inbound("{\"method\":\"link.invoke\",\"id\":\"r1\",\"command\":\"device.health\"}", &inv) == MP_IN_INVOKE && inv.params == NULL, "invoke without params");
    CHECK(inbound("{\"method\":\"link.invoke\",\"id\":\"r1\",\"command\":\"device.health\",\"params\":null}", &inv) == MP_IN_INVOKE && inv.params == NULL, "params null");
    CHECK(inbound("{\"method\":\"link.invoke\",\"id\":\"r1\",\"command\":5}", &inv) == MP_IN_INVOKE_BAD && strcmp(inv.id, "r1") == 0, "command not string -> bad with id");
    CHECK(inbound("{\"method\":\"link.invoke\",\"id\":\"r1\",\"command\":\"x\",\"params\":[1]}", &inv) == MP_IN_INVOKE_BAD, "params array -> bad");
    CHECK(inbound("{\"method\":\"link.invoke\",\"command\":\"x\"}", &inv) == MP_IN_IGNORE, "no id -> ignore");
    CHECK(inbound("{\"method\":\"link.invoke\",\"id\":\"\",\"command\":\"x\"}", &inv) == MP_IN_IGNORE, "empty id -> ignore");
    CHECK(inbound("{\"method\":\"link.invoke\",\"id\":7,\"command\":\"x\"}", &inv) == MP_IN_IGNORE, "numeric id -> ignore");
    CHECK(inbound("{\"method\":\"LINK.INVOKE\",\"id\":\"1\",\"command\":\"x\"}", &inv) == MP_IN_IGNORE, "method case-sensitive");
    CHECK(inbound("{\"type\":\"res\",\"id\":\"abc\",\"result\":{}}", &inv) == MP_IN_IGNORE, "register reply ignored here");
    CHECK(inbound("\"str\"", &inv) == MP_IN_IGNORE && mp_classify_inbound(NULL, NULL) == MP_IN_IGNORE, "non-object");
}

static void test_scan(void)
{
    char out[64];
    CHECK(mp_scan_top_string("{\"id\":\"r-1\",\"x\":1}", 18, "id", out, sizeof(out)) && strcmp(out, "r-1") == 0, "scan first");
    const char *s = "{ \"params\" : {\"id\":\"inner\",\"a\":[1,{\"id\":\"deep\"}],\"s\":\"q\\\"}\"}, \"method\":\"link.invoke\", \"id\" : \"outer\" }";
    CHECK(mp_scan_top_string(s, strlen(s), "id", out, sizeof(out)) && strcmp(out, "outer") == 0, "scan skips nested and escaped: %s", out);
    CHECK(mp_scan_top_string(s, strlen(s), "method", out, sizeof(out)) && strcmp(out, "link.invoke") == 0, "scan method");
    CHECK(!mp_scan_top_string("{\"id\":\"a\\\"b\"}", 13, "id", out, sizeof(out)), "escaped value refused");
    CHECK(!mp_scan_top_string("{\"i\\u0064\":\"x\"}", 15, "id", out, sizeof(out)), "escaped key not matched");
    CHECK(!mp_scan_top_string("{\"id\":5}", 8, "id", out, sizeof(out)), "non-string value");
    CHECK(!mp_scan_top_string("{\"id\":\"\"}", 9, "id", out, sizeof(out)), "empty value");
    CHECK(!mp_scan_top_string("{\"id\":\"abcdef\"}", 15, "id", out, 4), "too long for out");
    /* 边界：值长度 = cap - 1 放得下（加 NUL 正好），= cap 放不下 */
    CHECK(mp_scan_top_string("{\"id\":\"abc\"}", 12, "id", out, 4) && strcmp(out, "abc") == 0, "value len cap-1 fits");
    CHECK(!mp_scan_top_string("{\"id\":\"abcd\"}", 13, "id", out, 4), "value len == cap refused");
    CHECK(!mp_scan_top_string("{\"id\":\"abc", 10, "id", out, sizeof(out)), "unterminated");
    CHECK(!mp_scan_top_string("[\"id\",\"x\"]", 10, "id", out, sizeof(out)), "not an object");
    CHECK(!mp_scan_top_string("{\"a\":{\"b\":1", 11, "id", out, sizeof(out)), "unterminated nested");
    CHECK(!mp_scan_top_string("{\"a\" 1}", 7, "id", out, sizeof(out)), "missing colon");
    CHECK(!mp_scan_top_string("{\"id\":\"x\"}", 0, "id", out, sizeof(out)), "len 0");
    CHECK(!mp_scan_top_string(NULL, 5, "id", out, sizeof(out)) && !mp_scan_top_string("{}", 2, "id", NULL, 5), "NULLs");
    /* 20 KB 的超长调用，id 在最后：照样扫得到（这正是它的用途） */
    static char big[20500];
    int o = snprintf(big, sizeof(big), "{\"method\":\"link.invoke\",\"command\":\"hub.device.update\",\"params\":{\"name\":\"");
    memset(big + o, 'a', 20000); o += 20000;
    o += snprintf(big + o, sizeof(big) - (size_t)o, "\"},\"id\":\"big-1\"}");
    CHECK(mp_scan_top_string(big, (size_t)o, "id", out, sizeof(out)) && strcmp(out, "big-1") == 0, "scan 20 KB");
    /* 长度只读 len：截在 id 之前就找不到 */
    CHECK(!mp_scan_top_string(big, 20050, "id", out, sizeof(out)), "len respected");
}

/* 审计 a5：进 cJSON 之前的形状预检 */
static void test_shape(void)
{
    char buf[8192];
    /* 嵌套 16 层可以，17 层不行 */
    int n = 0;
    for (int i = 0; i < 16; i++) buf[n++] = '[';
    for (int i = 0; i < 16; i++) buf[n++] = ']';
    CHECK(mp_json_shape_ok(buf, (size_t)n, MP_JSON_MAX_DEPTH, MP_JSON_MAX_VALUES), "depth 16 ok");
    n = 0;
    for (int i = 0; i < 17; i++) buf[n++] = '[';
    for (int i = 0; i < 17; i++) buf[n++] = ']';
    CHECK(!mp_json_shape_ok(buf, (size_t)n, MP_JSON_MAX_DEPTH, MP_JSON_MAX_VALUES), "depth 17 refused");
    /* 990 层的 2 KB 炸弹 */
    n = 0;
    for (int i = 0; i < 990; i++) buf[n++] = '[';
    CHECK(!mp_json_shape_ok(buf, (size_t)n, MP_JSON_MAX_DEPTH, MP_JSON_MAX_VALUES), "nesting bomb refused");
    /* 值的个数上限 64 */
    n = snprintf(buf, sizeof(buf), "[");
    for (int i = 0; i < 62; i++) n += snprintf(buf + n, sizeof(buf) - (size_t)n, "1,");
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "1]");
    const bool ok63 = mp_json_shape_ok(buf, (size_t)n, MP_JSON_MAX_DEPTH, MP_JSON_MAX_VALUES);
    n = snprintf(buf, sizeof(buf), "[");
    for (int i = 0; i < 63; i++) n += snprintf(buf + n, sizeof(buf) - (size_t)n, "1,");
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "1]");
    const bool ok64 = mp_json_shape_ok(buf, (size_t)n, MP_JSON_MAX_DEPTH, MP_JSON_MAX_VALUES);
    n = snprintf(buf, sizeof(buf), "[");
    for (int i = 0; i < 64; i++) n += snprintf(buf + n, sizeof(buf) - (size_t)n, "1,");
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "1]");
    const bool ok65 = mp_json_shape_ok(buf, (size_t)n, MP_JSON_MAX_DEPTH, MP_JSON_MAX_VALUES);
    CHECK(MP_JSON_MAX_VALUES == 64, "上限 64（内部 RAM 预算，4.8）");
    /* 计数：开头 1 + "[" 1 + 逗号（元素数 − 1）= 元素数 + 1 */
    CHECK(ok63 && !ok64 && !ok65, "63 个元素（计 64）可以、64 个（计 65）不行（%d %d %d）", ok63, ok64, ok65);
    n = snprintf(buf, sizeof(buf), "[");
    for (int i = 0; i < 300; i++) n += snprintf(buf + n, sizeof(buf) - (size_t)n, "1,");
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "1]");
    CHECK(!mp_json_shape_ok(buf, (size_t)n, MP_JSON_MAX_DEPTH, MP_JSON_MAX_VALUES), "flat array of 301 refused");
    /* 字符串里的括号、逗号、转义引号不算 */
    const char *s = "{\"a\":\"[[[[[[[[[[[[[[[[[[[[,,,,,,\\\"]]]\",\"b\":1}";
    CHECK(mp_json_shape_ok(s, strlen(s), 2, 5), "brackets inside strings ignored");
    CHECK(!mp_json_shape_ok("{\"a\":\"x}", 9, 16, 256), "unterminated string refused");
    CHECK(!mp_json_shape_ok("]", 1, 16, 256), "close without open refused");
    CHECK(!mp_json_shape_ok(NULL, 3, 16, 256), "NULL");
    /* 真实的调用报文过得去 */
    const char *inv = "{\"method\":\"link.invoke\",\"id\":\"r1\",\"command\":\"hub.devices.list\",\"params\":{\"protocol\":\"thread\",\"cursor\":\"8\"}}";
    CHECK(mp_json_shape_ok(inv, strlen(inv), MP_JSON_MAX_DEPTH, MP_JSON_MAX_VALUES), "real invoke ok");
}

static void test_dispatch(void)
{
    const hub_cmd_t *c;
    mp_mgmt_t m;
    CHECK(mp_dispatch("device.unpair", 0, 0, &c, &m) == MP_DISP_MGMT && m == MP_MGMT_UNPAIR && c == NULL, "unpair is mgmt");
    CHECK(mp_dispatch("device.list_vms", 0, 0, &c, &m) == MP_DISP_MGMT && m == MP_MGMT_LIST_VMS, "list_vms");
    CHECK(mp_dispatch("device.set_vm", 0, 0, &c, &m) == MP_DISP_MGMT && m == MP_MGMT_SET_VM, "set_vm");
    CHECK(mp_dispatch("device.reset_vm", 0, 0, &c, &m) == MP_DISP_MGMT && m == MP_MGMT_RESET_VM, "reset_vm");
    CHECK(mp_dispatch("device.ota", 0xFF, 0xFF, &c, &m) == MP_DISP_UNSUPPORTED, "device.ota unsupported");
    CHECK(mp_dispatch("device.discover", 0xFF, 0xFF, &c, &m) == MP_DISP_UNSUPPORTED, "device.discover unsupported");
    CHECK(mp_dispatch("hub.devices.list", HUB_PERM_DEFAULT, 0, &c, &m) == MP_DISP_REGISTRY && c && strcmp(c->name, "hub.devices.list") == 0, "read cmd");
    CHECK(mp_dispatch("hub.device.control", HUB_PERM_DEFAULT, 0, &c, &m) == MP_DISP_UNSUPPORTED, "control not registered in M3");
    CHECK(mp_dispatch("hub.device.control", 0, HUB_BACKEND_ZHUB, &c, &m) == MP_DISP_UNSUPPORTED, "control not registered without CONTROL bit");
    CHECK(mp_dispatch("hub.device.control", HUB_PERMBIT_CONTROL, HUB_BACKEND_ZHUB, &c, &m) == MP_DISP_UNSUPPORTED, "control not registered while the control path is not ready (C17)");
    CHECK(mp_dispatch("hub.device.control", HUB_PERMBIT_CONTROL, HUB_BACKEND_ZHUB | HUB_BACKEND_CONTROL, &c, &m) == MP_DISP_REGISTRY, "control registered");
    CHECK(mp_dispatch("hub.device.update", HUB_PERMBIT_MANAGE, HUB_BACKEND_ZHUB, &c, &m) == MP_DISP_REGISTRY, "update registered once zhub runs (C17)");
    CHECK(mp_dispatch("hub.device.update", HUB_PERMBIT_MANAGE, 0, &c, &m) == MP_DISP_UNSUPPORTED, "update not registered without a backend");
    CHECK(mp_dispatch("hub.zigbee.permit_join", HUB_PERMBIT_MANAGE, HUB_BACKEND_ZHUB, &c, &m) == MP_DISP_UNSUPPORTED, "permit_join waits for its own readiness bit (Z21)");
    CHECK(mp_dispatch("", 0xFF, 0xFF, &c, &m) == MP_DISP_UNSUPPORTED && mp_dispatch(NULL, 0xFF, 0xFF, NULL, NULL) == MP_DISP_UNSUPPORTED, "empty / NULL");
    CHECK(mp_mgmt_lookup("device.unpair ") == MP_MGMT_NONE && mp_mgmt_lookup(NULL) == MP_MGMT_NONE, "mgmt exact");
}

static bool params_ok(const char *cmd, const char *json, char *err)
{
    cJSON *j = json ? cJSON_Parse(json) : NULL;
    bool r = mp_check_params(hub_registry_find(cmd), j, err, 96);
    cJSON_Delete(j);
    return r;
}

static void test_params(void)
{
    char err[96];
    CHECK(params_ok("device.health", NULL, err), "no params needed");
    CHECK(params_ok("device.health", "{\"junk\":1}", err), "unknown params ignored");
    CHECK(!params_ok("hub.device.get", NULL, err) && strcmp(err, "missing required parameter 'id'") == 0, "missing id: %s", err);
    CHECK(!params_ok("hub.device.get", "{\"id\":null}", err) && strstr(err, "missing"), "null id");
    CHECK(!params_ok("hub.device.get", "{\"id\":5}", err) && strcmp(err, "parameter 'id' must be a string") == 0, "id type: %s", err);
    CHECK(params_ok("hub.device.get", "{\"id\":\"zb-1\"}", err), "id ok");
    CHECK(params_ok("hub.events.recent", "{\"limit\":3}", err), "integer ok");
    CHECK(!params_ok("hub.events.recent", "{\"limit\":3.5}", err) && strcmp(err, "parameter 'limit' must be an integer") == 0, "fraction: %s", err);
    CHECK(!params_ok("hub.events.recent", "{\"limit\":\"3\"}", err), "string for integer");
    CHECK(!params_ok("hub.events.recent", "{\"limit\":1e20}", err), "integer overflow");
    CHECK(params_ok("hub.events.recent", "{\"limit\":null}", err), "optional null ok");
    CHECK(!params_ok("hub.devices.list", "{\"protocol\":1}", err), "optional wrong type");
    CHECK(!params_ok("hub.device.update", "{\"id\":\"x\",\"notify\":\"yes\"}", err) && strcmp(err, "parameter 'notify' must be a boolean") == 0, "boolean: %s", err);
    CHECK(params_ok("hub.device.update", "{\"id\":\"x\",\"notify\":true}", err), "boolean ok");
    CHECK(!params_ok("hub.device.get", "[1]", err) && strcmp(err, "params must be an object") == 0, "params array");
    CHECK(!mp_check_params(NULL, NULL, err, sizeof(err)), "NULL cmd");
}

static void test_results(void)
{
    hj_t w;
    hj_init(&w, g_buf, MP_RESULT_MAX);
    mp_result_begin(&w, "req-9");
    hj_obj_begin(&w); hj_kint(&w, "uptime_s", 42); hj_obj_end(&w);
    mp_result_end(&w);
    int n = hj_finish(&w);
    cJSON *j = n > 0 ? cJSON_Parse(g_buf) : NULL;
    CHECK(j && strcmp(cJSON_GetObjectItemCaseSensitive(j, "method")->valuestring, "link.result") == 0
          && strcmp(cJSON_GetObjectItemCaseSensitive(j, "id")->valuestring, "req-9") == 0
          && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "ok"))
          && cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "payload"), "uptime_s")->valueint == 42, "ok result: %s", g_buf);
    cJSON_Delete(j);
    n = mp_build_result_error(g_buf, sizeof(g_buf), "req-9", "forbidden", "unlocking is disabled on this hub (the owner can allow it in the hub's \"web\" page)");
    j = n > 0 ? cJSON_Parse(g_buf) : NULL;
    CHECK(j && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(j, "ok"))
          && strcmp(cJSON_GetObjectItemCaseSensitive(j, "error")->valuestring, "forbidden: unlocking is disabled on this hub (the owner can allow it in the hub's \"web\" page)") == 0
          && !cJSON_GetObjectItemCaseSensitive(j, "payload"), "error result: %s", g_buf);
    cJSON_Delete(j);
    /* 远端给的命令名回显进错误：转义、坏 UTF-8 换掉，仍是合法 JSON */
    n = mp_build_result_error(g_buf, sizeof(g_buf), "r", "unsupported", "x\"\n\xff");
    j = n > 0 ? cJSON_Parse(g_buf) : NULL;
    CHECK(j != NULL, "echoed junk still valid JSON: %s", g_buf);
    cJSON_Delete(j);
    CHECK(mp_build_result_error(g_buf, sizeof(g_buf), "r", "", "m") == -1 && mp_build_result_error(g_buf, sizeof(g_buf), NULL, "c", "m") == -1, "missing code/id");
    char tiny[20];
    CHECK(mp_build_result_error(tiny, sizeof(tiny), "r", "c", "a long message") == -1 && tiny[0] == 0, "too small");
    j = cJSON_Parse(mp_heartbeat_json());
    CHECK(j && strcmp(cJSON_GetObjectItemCaseSensitive(j, "method")->valuestring, "link.heartbeat") == 0, "heartbeat");
    cJSON_Delete(j);
}

/* hub_params_rl_exempt：只有 hub.zigbee.permit_join 的 seconds:0（关窗）不进限速桶（6.5 注记）；别的值、别的命令、没给 seconds 都要进 */
static void test_rl_exempt(void)
{
    const hub_cmd_t *pj = hub_registry_find("hub.zigbee.permit_join");
    const hub_cmd_t *mc = hub_registry_find("hub.matter.commission");
    cJSON *z = cJSON_Parse("{\"seconds\":0}"), *o = cJSON_Parse("{\"seconds\":120}"), *e = cJSON_Parse("{}"), *s = cJSON_Parse("{\"seconds\":\"0\"}");
    CHECK(pj && hub_params_rl_exempt(pj, z), "rl_exempt：permit_join seconds 0 豁免");
    CHECK(!hub_params_rl_exempt(pj, o) && !hub_params_rl_exempt(pj, e) && !hub_params_rl_exempt(pj, NULL) && !hub_params_rl_exempt(pj, s),
          "rl_exempt：seconds 120 / 没给 / NULL / 字符串 \"0\" 不豁免（字符串由 hub_params_check 先拒）");
    static const char *k_bad[] = { "{\"seconds\":10}", "{\"seconds\":29}", "{\"seconds\":255}", "{\"seconds\":60.5}", "{\"seconds\":-1}" };
    static const char *k_ok[] = { "{\"seconds\":30}", "{\"seconds\":254}" };
    int bad_ex = 0;
    for (size_t i = 0; i < sizeof(k_bad) / sizeof(k_bad[0]); i++) { cJSON *j = cJSON_Parse(k_bad[i]); if (!hub_params_rl_exempt(pj, j)) bad_ex++; cJSON_Delete(j); }
    for (size_t i = 0; i < sizeof(k_ok) / sizeof(k_ok[0]); i++) { cJSON *j = cJSON_Parse(k_ok[i]); if (hub_params_rl_exempt(pj, j)) bad_ex++; cJSON_Delete(j); }
    CHECK(bad_ex == 0, "rl_exempt：注定被拒的 10 / 29 / 255 / 60.5 / -1 不扣令牌，30 / 254 照扣（第六步上板：写错一次占掉 60 s 名额）");
    CHECK(mc && !hub_params_rl_exempt(mc, z) && !hub_params_rl_exempt(NULL, z), "rl_exempt：别的命令 / NULL 命令不豁免");
    cJSON_Delete(z); cJSON_Delete(o); cJSON_Delete(e); cJSON_Delete(s);
}

int main(void)
{
    test_rl_exempt();
    test_uuid();
    test_register();
    test_register_reply();
    test_inbound();
    test_scan();
    test_shape();
    test_dispatch();
    test_params();
    test_results();
    cJSON_Delete(g_keep);
    printf("muse_proto: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
