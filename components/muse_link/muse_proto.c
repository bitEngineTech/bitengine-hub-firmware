/* muse_proto —— Muse 会话控制面报文的编解码（见 muse_proto.h）。纯 C，不依赖 ESP-IDF。 */
#include "muse_proto.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "hub_params.h"

void mp_format_uuid(const uint8_t rnd[16], char *out)
{
    static const char hex[] = "0123456789abcdef";
    if (!out) return;
    if (!rnd) {
        out[0] = '\0';
        return;
    }
    uint8_t b[16];
    memcpy(b, rnd, sizeof(b));
    b[6] = (uint8_t)((b[6] & 0x0Fu) | 0x40u);   /* 版本 4 */
    b[8] = (uint8_t)((b[8] & 0x3Fu) | 0x80u);   /* RFC 4122 变体 */
    size_t o = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
        out[o++] = hex[b[i] >> 4];
        out[o++] = hex[b[i] & 0x0F];
    }
    out[o] = '\0';
}

/* ---------------- 注册 ---------------- */

static bool nonempty(const char *s) { return s && s[0]; }

int mp_build_register(char *buf, size_t cap, const char *req_id, const mp_reg_info_t *info, uint8_t perm, uint8_t backends)
{
    if (!buf || cap == 0) return -1;
    buf[0] = '\0';
    if (!nonempty(req_id) || !info || !nonempty(info->node_id) || !info->display_name || !nonempty(info->version)
        || !nonempty(info->device_family) || !nonempty(info->model_id)) {
        return -1;
    }
    if (cap > MP_REGISTER_MAX + 1) cap = MP_REGISTER_MAX + 1;   /* +1：NUL */
    hj_t w;
    hj_init(&w, buf, cap);
    hj_obj_begin(&w);
    hj_kstr(&w, "type", "req");
    hj_kstr(&w, "id", req_id);
    hj_kstr(&w, "method", "link.register");
    hj_key(&w, "params");
    hj_obj_begin(&w);
    hj_kstr(&w, "node_id", info->node_id);
    hj_kstr(&w, "display_name", info->display_name);
    hj_kstr(&w, "platform", "esp32");
    hj_kstr(&w, "version", info->version);
    hj_kstr(&w, "device_family", info->device_family);
    hj_kstr(&w, "model_id", info->model_id);
    hj_kbool(&w, "is_wakeup_supported", false);
    /* 不发 metadata：不送 SSID（4.7，隐私）；两份上游在没有 SSID 时都不发这个键（见 muse_proto.h） */
    hj_key(&w, "commands_v2");
    hub_registry_emit_commands_v2(&w, perm, backends);
    hj_obj_end(&w);
    hj_obj_end(&w);
    if (w.truncated) {                            /* 放不下：不发半张表（上游：放不下的设备永远注册不上） */
        buf[0] = '\0';
        return -1;
    }
    return hj_finish(&w);
}

static bool contains_ci(const char *hay, const char *needle)
{
    const size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t k = 0;
        while (k < n && p[k] && tolower((unsigned char)p[k]) == tolower((unsigned char)needle[k])) k++;
        if (k == n) return true;
    }
    return false;
}

static void put_reason(char *reason, size_t cap, const char *s)
{
    if (!reason || cap == 0) return;
    snprintf(reason, cap, "%s", s ? s : "");
}

mp_reg_reply_t mp_classify_register_reply(const cJSON *msg, const char *req_id, bool *heartbeat, char *reason, size_t reason_cap)
{
    if (heartbeat) *heartbeat = false;
    put_reason(reason, reason_cap, "");
    if (!cJSON_IsObject(msg) || !nonempty(req_id)) return MP_REG_NOT_MINE;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    if (!cJSON_IsString(id) || !id->valuestring || strcmp(id->valuestring, req_id) != 0) return MP_REG_NOT_MINE;

    const cJSON *err = cJSON_GetObjectItemCaseSensitive(msg, "error");
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(msg, "ok");
    const bool err_set = err && !cJSON_IsNull(err) && !cJSON_IsFalse(err);
    if (err_set || cJSON_IsFalse(ok)) {
        const char *why = "no reason given";
        const char *code = NULL;
        if (cJSON_IsString(err) && err->valuestring) {
            why = err->valuestring;
        } else if (cJSON_IsObject(err)) {
            const cJSON *m = cJSON_GetObjectItemCaseSensitive(err, "message");
            const cJSON *k = cJSON_GetObjectItemCaseSensitive(err, "code");
            if (cJSON_IsString(k) && k->valuestring) code = k->valuestring;
            if (cJSON_IsString(m) && m->valuestring) why = m->valuestring;
            else if (code) why = code;
        } else if (!err_set) {
            why = "ok:false";
        }
        put_reason(reason, reason_cap, why);
        const bool proto = contains_ci(why, "unsupported") || contains_ci(why, "version")
                           || (code && (contains_ci(code, "unsupported") || contains_ci(code, "version")));
        return proto ? MP_REG_REJECTED_PROTOCOL : MP_REG_REJECTED;
    }
    if (heartbeat) {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(msg, "type");
        const cJSON *res = cJSON_GetObjectItemCaseSensitive(msg, "result");
        const cJSON *status = cJSON_IsObject(res) ? cJSON_GetObjectItemCaseSensitive(res, "status") : NULL;
        *heartbeat = !err && cJSON_IsString(type) && strcmp(type->valuestring, "res") == 0
                     && cJSON_IsString(status) && strcmp(status->valuestring, "registered") == 0;
    }
    return MP_REG_ACKED;
}

/* ---------------- 入站 ---------------- */

static bool str_is(const cJSON *j, const char *s)
{
    return cJSON_IsString(j) && j->valuestring && strcmp(j->valuestring, s) == 0;
}

mp_in_kind_t mp_classify_inbound(const cJSON *msg, mp_invoke_t *inv)
{
    if (inv) memset(inv, 0, sizeof(*inv));
    if (!cJSON_IsObject(msg)) return MP_IN_IGNORE;
    if (str_is(cJSON_GetObjectItemCaseSensitive(msg, "type"), "event")) {
        const cJSON *ev = cJSON_GetObjectItemCaseSensitive(msg, "event");
        return (str_is(ev, "link.unpaired") || str_is(ev, "node.unpaired")) ? MP_IN_UNPAIRED : MP_IN_IGNORE;
    }
    if (!str_is(cJSON_GetObjectItemCaseSensitive(msg, "method"), "link.invoke")) return MP_IN_IGNORE;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    if (!cJSON_IsString(id) || !nonempty(id->valuestring)) return MP_IN_IGNORE;   /* 没有 id 回不了（同上游） */
    if (inv) inv->id = id->valuestring;
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(msg, "command");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
    if (!cJSON_IsString(cmd) || !cmd->valuestring) return MP_IN_INVOKE_BAD;
    if (params && !cJSON_IsNull(params) && !cJSON_IsObject(params)) return MP_IN_INVOKE_BAD;
    if (inv) {
        inv->command = cmd->valuestring;
        inv->params = cJSON_IsObject(params) ? params : NULL;
    }
    return MP_IN_INVOKE;
}

bool mp_json_shape_ok(const char *json, size_t len, unsigned max_depth, unsigned max_values)
{
    return hub_json_shape_ok(json, len, max_depth, max_values);   /* M3 第七步起与本地 REST 共用 hub_core 的那份 */
}

/* 不分配内存的顶层扫描：只为超长报文取 id。字符串里的转义照跳；嵌套的对象 / 数组靠计数跳过。 */
static size_t sk_ws(const char *s, size_t n, size_t i)
{
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    return i;
}

/* s[i] 是 '"'：返回收尾引号之后的位置；*has_esc 报告里面有没有转义。没收尾返回 n + 1。 */
static size_t sk_string(const char *s, size_t n, size_t i, bool *has_esc)
{
    *has_esc = false;
    for (i++; i < n; i++) {
        if (s[i] == '\\') {
            *has_esc = true;
            i++;
        } else if (s[i] == '"') {
            return i + 1;
        }
    }
    return n + 1;
}

/* 跳过一个值；出错返回 n + 1 */
static size_t sk_value(const char *s, size_t n, size_t i)
{
    if (i >= n) return n + 1;
    bool esc;
    if (s[i] == '"') return sk_string(s, n, i, &esc);
    if (s[i] == '{' || s[i] == '[') {
        size_t depth = 0;
        for (; i < n; i++) {
            if (s[i] == '"') {
                i = sk_string(s, n, i, &esc);
                if (i > n) return n + 1;
                i--;                                      /* for 的 i++ 补回来 */
            } else if (s[i] == '{' || s[i] == '[') {
                depth++;
            } else if (s[i] == '}' || s[i] == ']') {
                if (--depth == 0) return i + 1;
            }
        }
        return n + 1;
    }
    while (i < n && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r') i++;
    return i;
}

bool mp_scan_top_string(const char *json, size_t len, const char *key, char *out, size_t cap)
{
    if (!json || !key || !out || cap == 0) return false;
    out[0] = '\0';
    const size_t kl = strlen(key);
    size_t i = sk_ws(json, len, 0);
    if (i >= len || json[i] != '{') return false;
    i = sk_ws(json, len, i + 1);
    while (i < len && json[i] != '}') {
        if (json[i] != '"') return false;
        bool kesc;
        const size_t ks = i + 1;
        size_t ke = sk_string(json, len, i, &kesc);
        if (ke > len) return false;
        const bool key_match = !kesc && (ke - 1 - ks) == kl && memcmp(json + ks, key, kl) == 0;
        i = sk_ws(json, len, ke);
        if (i >= len || json[i] != ':') return false;
        i = sk_ws(json, len, i + 1);
        if (key_match && i < len && json[i] == '"') {
            bool vesc;
            const size_t vs = i + 1;
            const size_t ve = sk_string(json, len, i, &vesc);
            if (ve > len || vesc) return false;
            const size_t vl = ve - 1 - vs;
            if (vl == 0 || vl >= cap) return false;
            memcpy(out, json + vs, vl);
            out[vl] = '\0';
            return true;
        }
        i = sk_value(json, len, i);
        if (i > len) return false;
        i = sk_ws(json, len, i);
        if (i < len && json[i] == ',') i = sk_ws(json, len, i + 1);
    }
    return false;
}

/* ---------------- 分派 ---------------- */

mp_mgmt_t mp_mgmt_lookup(const char *command)
{
    if (!command) return MP_MGMT_NONE;
    if (strcmp(command, "device.unpair") == 0) return MP_MGMT_UNPAIR;
    if (strcmp(command, "device.list_vms") == 0) return MP_MGMT_LIST_VMS;
    if (strcmp(command, "device.set_vm") == 0) return MP_MGMT_SET_VM;
    if (strcmp(command, "device.reset_vm") == 0) return MP_MGMT_RESET_VM;
    return MP_MGMT_NONE;
}

mp_disp_t mp_dispatch(const char *command, uint8_t reg_perm, uint8_t reg_backends, const hub_cmd_t **cmd, mp_mgmt_t *mgmt)
{
    if (cmd) *cmd = NULL;
    if (mgmt) *mgmt = MP_MGMT_NONE;
    if (!command) return MP_DISP_UNSUPPORTED;
    const mp_mgmt_t m = mp_mgmt_lookup(command);
    if (m != MP_MGMT_NONE) {
        if (mgmt) *mgmt = m;
        return MP_DISP_MGMT;
    }
    const hub_cmd_t *c = hub_registry_find(command);
    if (!c || !hub_cmd_available(c, reg_perm, reg_backends)) return MP_DISP_UNSUPPORTED;
    if (cmd) *cmd = c;
    return MP_DISP_REGISTRY;
}

bool mp_check_params(const hub_cmd_t *c, const cJSON *params, char *err, size_t err_cap)
{
    return hub_params_check(c, params, err, err_cap);         /* M3 第七步起与本地 REST 共用 hub_core 的那份 */
}

/* ---------------- 结果 ---------------- */

void mp_result_begin(hj_t *w, const char *req_id)
{
    hj_obj_begin(w);
    hj_kstr(w, "method", "link.result");
    hj_kstr(w, "id", req_id);
    hj_kbool(w, "ok", true);
    hj_key(w, "payload");
}

void mp_result_end(hj_t *w)
{
    hj_obj_end(w);
}

int mp_build_result_error(char *buf, size_t cap, const char *req_id, const char *code, const char *message)
{
    if (!buf || cap == 0) return -1;
    buf[0] = '\0';
    if (!nonempty(req_id) || !nonempty(code)) return -1;
    char e[448];
    snprintf(e, sizeof(e), "%s: %s", code, message ? message : "");
    hj_t w;
    hj_init(&w, buf, cap);
    hj_obj_begin(&w);
    hj_kstr(&w, "method", "link.result");
    hj_kstr(&w, "id", req_id);
    hj_kbool(&w, "ok", false);
    hj_kstr(&w, "error", e);
    hj_obj_end(&w);
    if (w.truncated) {
        buf[0] = '\0';
        return -1;
    }
    return hj_finish(&w);
}

const char *mp_heartbeat_json(void)
{
    return "{\"method\":\"link.heartbeat\"}";
}
