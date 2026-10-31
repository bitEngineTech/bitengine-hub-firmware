/* hub_params —— 命令参数按注册表校验（见 include/hub_params.h）。纯 C，不依赖 ESP-IDF。 */
#include "hub_params.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static bool type_ok(const cJSON *v, const char *type)
{
    if (strcmp(type, "string") == 0) return cJSON_IsString(v);
    if (strcmp(type, "boolean") == 0) return cJSON_IsBool(v);
    if (strcmp(type, "integer") == 0) {
        if (!cJSON_IsNumber(v)) return false;
        const double d = v->valuedouble;
        return d >= -2147483648.0 && d <= 2147483647.0 && d == (double)(int32_t)d;
    }
    return false;                                         /* 表里写了不认识的类型：当作不合格，别放过 */
}

bool hub_params_check(const hub_cmd_t *c, const cJSON *params, char *err, size_t err_cap)
{
    if (err && err_cap) err[0] = '\0';
    if (!c) return false;
    if (params && !cJSON_IsObject(params)) {
        if (err) snprintf(err, err_cap, "params must be an object");
        return false;
    }
    for (uint8_t i = 0; i < c->n_required; i++) {
        const hub_param_t *p = &c->required[i];
        const cJSON *v = params ? cJSON_GetObjectItemCaseSensitive(params, p->name) : NULL;
        if (!v || cJSON_IsNull(v)) {
            if (err) snprintf(err, err_cap, "missing required parameter '%s'", p->name);
            return false;
        }
        if (!type_ok(v, p->type)) {
            if (err) snprintf(err, err_cap, "parameter '%s' must be a%s %s", p->name, p->type[0] == 'i' ? "n" : "", p->type);
            return false;
        }
    }
    for (uint8_t i = 0; i < c->n_optional; i++) {
        const hub_param_t *p = &c->optional[i];
        const cJSON *v = params ? cJSON_GetObjectItemCaseSensitive(params, p->name) : NULL;
        if (v && !cJSON_IsNull(v) && !type_ok(v, p->type)) {
            if (err) snprintf(err, err_cap, "parameter '%s' must be a%s %s", p->name, p->type[0] == 'i' ? "n" : "", p->type);
            return false;
        }
    }
    return true;
}

bool hub_json_shape_ok(const char *json, size_t len, unsigned max_depth, unsigned max_values)
{
    if (!json) return false;
    unsigned depth = 0, values = 1;
    bool in_str = false;
    for (size_t i = 0; i < len; i++) {
        const char c = json[i];
        if (in_str) {
            if (c == '\\') i++;                                     /* 转义：跳过下一个字节（\uXXXX 的后 4 位是普通字符） */
            else if (c == '"') in_str = false;
            continue;
        }
        switch (c) {
        case '"':
            in_str = true;
            break;
        case '{': case '[':
            if (++depth > max_depth || ++values > max_values) return false;
            break;
        case '}': case ']':
            if (depth == 0) return false;
            depth--;
            break;
        case ',':
            if (++values > max_values) return false;
            break;
        default:
            break;
        }
    }
    return !in_str;
}

bool hub_params_rl_exempt(const hub_cmd_t *c, const cJSON *params)
{
    if (!c || c->rl != HUB_RL_PERMIT_JOIN || !params) return false;
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(params, "seconds");
    if (!cJSON_IsNumber(s)) return false;          /* 没给：默认 120，照常扣 */
    const double d = s->valuedouble;
    if (d == 0.0) return true;                     /* 关窗 */
    const bool valid = d >= (double)HUB_PJ_MIN_S && d <= (double)HUB_PJ_MAX_S && d == (double)(uint32_t)d;
    return !valid;                                 /* 处理函数会按参数错拒掉：不扣 */
}
