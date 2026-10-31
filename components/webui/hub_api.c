/* hub_api —— 直连型的本地设备层出口（docs/15_muse_direct_design_v1.md 10.1、MD-9"一张表两个出口"）与审计读取（6.10）。
 * 只在直连型 + Muse 下编（CMakeLists.txt），配对剖面里不注册。
 *
 *   POST /api/v1/hub/invoke   {command, params}：与 Muse 的调用走同一张命令表、同一份参数校验、同一组处理函数（hub_cmds）；
 *                             本地管理员不受 Muse 权限位约束（6.5 的 1、2 两道不查），但参数校验、限速（REST 自己的桶）、审计照常。
 *                             读类 200 {"ok":true,"result":…}；写类命令（M4 第六步起有 hub.zigbee.permit_join）要求已设管理密码，否则 409 password_required。
 *   GET  /api/v1/hub/devices  ?protocol=&cursor=：等同于 hub.devices.list
 *   GET  /api/v1/muse/audit   ?limit=50：审计环（6.10），新的在前
 *   POST /api/v1/hub/device/sensitive {"id","sensitive":true|false}：设备的敏感标记（6.2：**只在本地**，Muse 改不了；附录 A 的 hub.device.update
 *                             没有这个参数）。要已设管理密码；只认这个网关管着的设备；记审计（6.10）
 *   GET  /api/v1/zigbee/ncp_diag ?refresh=1：NCP 自己的计数器与子设备表（zhub.h；放这里而不是 rest_api.c：只在直连型编，
 *                             tools/check_uri_slots.py 才不把它算进 HA 型的槽位）
 *   GET  /api/v1/hub/job ?id=：作业的结论（6.6；耗时命令回 202 {job} 之后用它轮询）
 *   POST /api/v1/hub/device/remove {"id":"mt-…","confirm":"REMOVE","force"?}：本地移除一台 Matter 设备（10.1；作业，202 {job}）
 *   GET  /api/v1/matter/status：Matter 控制器的状态（matter_ctrl.h；docs/15 第九节，M7 第一步）：state / 失败在哪一步、防崩溃循环计数、
 *                             fabric ID 与压缩 fabric ID（运行态记录实例名的前半段）、控制器节点号 / 厂商号、起 CHIP 前后的内部 SRAM
 *
 * 线程：httpd 是单任务，REST 的限速桶不用锁。清单比 15 s 旧时处理函数在 httpd 里现刷一次（第七节）：等清单锁 ≤ 500 ms、OT 锁 ≤ 500 ms、
 * 关联，再等一次清单锁 ≤ 500 ms —— 最坏约 1.5 s 加计算，每 15 s 最多一次（hub_svc 每 60 s 刷新，平时读的是缓存）。
 * 结果缓冲在 PSRAM，不占 httpd 的 8 KB 栈。 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "config_mgr.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hub_audit.h"
#include "hub_call.h"
#include "hub_cmds.h"
#include "hub_json.h"
#include "hub_params.h"
#include "hub_policy.h"
#include "hub_registry.h"
#include "json_escape.h"
#include "webui.h"
#include "hub_meta.h"
#include "hub_jobs.h"
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
#include <inttypes.h>
#include "esp_system.h"
#include "freertos/task.h"
#include "hub_matter.h"
#include "hub_store.h"
#include "matter_ctrl.h"
#include "ota_cloud.h"
#include "ota_mg24.h"
#endif
#if CONFIG_BITENGINE_ENABLE_ZHUB
#include <inttypes.h>
#include "hub_zigbee.h"
#include "zhub.h"
#endif

static const char *TAG = "hub_api";

#define INVOKE_BODY_MAX  4096     /* 同 Muse 的调用上限（4.8） */
#define RESULT_MAX       32768    /* 结果（清单一页、事件环 50 条都在里面）；放不下由处理函数撤半条带 truncated */
#define AUDIT_MAX        40960    /* 审计 128 条，字段全满时约 35 KB（PSRAM） */
#define SHAPE_DEPTH      16       /* 同 muse_proto 的 MP_JSON_MAX_DEPTH / MP_JSON_MAX_VALUES */
#define SHAPE_VALUES     64

static hub_rl_t s_rl;             /* REST 的限速桶（6.5：按来源分 muse / rest） */
static bool     s_rl_init;

static int recv_body(httpd_req_t *req, char *buf, size_t cap)
{
    const int total = req->content_len;
    if (total <= 0) return -1;
    if ((size_t)total >= cap) return -2;
    int got = 0;
    while (got < total) {
        const int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) return -1;
        got += r;
    }
    buf[got] = '\0';
    return got;
}

static int status_for(const char *code)
{
    if (!strcmp(code, "invalid_params")) return 400;
    if (!strcmp(code, "not_found") || !strcmp(code, "unsupported")) return 404;
    if (!strcmp(code, "forbidden")) return 403;
    if (!strcmp(code, "password_required") || !strcmp(code, "conflict")) return 409;   /* conflict：现在的状态不让做（例：控制器身份没丢不必重置） */
    if (!strcmp(code, "rate_limited")) return 429;
    if (!strcmp(code, "unavailable") || !strcmp(code, "busy")) return 503;
    return 500;
}

/* 错误体 {"error":code,"message":…}（message 是固件里的英文，可能带参数名，转义后放进去） */
static esp_err_t send_err_msg(httpd_req_t *req, const char *code, const char *msg)
{
    char esc[400];
    webui_json_escape(esc, sizeof(esc), msg ? msg : "");
    char body[480];
    snprintf(body, sizeof(body), "{\"error\":\"%s\",\"message\":\"%s\"}", code, esc);
    char st[40];
    const int s = status_for(code);
    snprintf(st, sizeof(st), "%d %s", s, s == 400 ? "Bad Request" : s == 403 ? "Forbidden" : s == 404 ? "Not Found" : s == 409 ? "Conflict"
                                         : s == 429 ? "Too Many Requests" : s == 503 ? "Service Unavailable" : "Internal Server Error");
    httpd_resp_set_status(req, st);
    return send_json_response(req, body);
}

/* 跑一条命令并把结果发出去；返回审计用的结果码 */
static const char *run_command(httpd_req_t *req, const char *command, const cJSON *params)
{
    const hub_cmd_t *cmd = hub_registry_find(command);
    if (!cmd) {
        send_err_msg(req, "unsupported", "not a command of this hub");
        return "unsupported";
    }
    char err[128];
    if (!hub_params_check(cmd, params, err, sizeof(err))) {
        send_err_msg(req, "invalid_params", err);
        return "invalid_params";
    }
    if (!s_rl_init) {
        hub_rl_init(&s_rl, (uint64_t)(esp_timer_get_time() / 1000));
        s_rl_init = true;
    }
    const bool rl_exempt = hub_params_rl_exempt(cmd, params);
    if (!rl_exempt && !hub_rl_take(&s_rl, cmd->rl, (uint64_t)(esp_timer_get_time() / 1000))) {
        send_err_msg(req, "rate_limited", "too many requests; wait a few seconds and try again");
        return "rate_limited";
    }
    /* 本地 REST 跳过 Muse 权限位（6.5 注记），前提是已设密码：没密码时写类命令拒（M4 第六步的 hub.zigbee.permit_join 是第一条写命令，从它起生效） */
    if (cmd->perm != HUB_PERM_READ && !config_mgr_has_password()) {
        send_err_msg(req, "password_required", "set an admin password before changing devices from the local API");
        return "password_required";
    }
    const hub_cmd_fn fn = hub_cmds_find(cmd->name);
    if (!fn) {
        send_err_msg(req, "unsupported", "this command is not available in this hub's firmware yet");
        return "unsupported";
    }
    char *buf = heap_caps_malloc(RESULT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        send_err_msg(req, "internal", "out of memory");
        return "internal";
    }
    hj_t w;
    hj_init(&w, buf, RESULT_MAX);
    hj_obj_begin(&w);
    hj_kbool(&w, "ok", true);
    hj_key(&w, "result");
    hub_call_t call = { .src = HUB_SRC_REST, .perm = HUB_PERMBIT_ALL, .params = params, .out = &w, .err_code = NULL };   /* 本地 REST 不看权限位（6.5 注记；写命令另要管理密码） */
    const char *res = "ok";
    if (fn(&call) != 0) {
        /* 当场拒了、没真去做（配网码打错）：这次扣的令牌还回去（MX21；Muse 出口 muse_link.c 同样） */
        if (call.rl_refund && !rl_exempt) hub_rl_refund(&s_rl, cmd->rl, (uint64_t)(esp_timer_get_time() / 1000));
        res = call.err_code ? call.err_code : "internal";
        send_err_msg(req, res, call.err_msg[0] ? call.err_msg : "the hub could not do that");
    } else {
        hj_obj_end(&w);
        if (hj_finish(&w) < 0) {
            res = "internal";
            send_err_msg(req, res, "the result did not fit; try a narrower request");
        } else {
            /* 结论晚到（作业，6.6）：202 + {job, state}，用 GET /api/v1/hub/job?id= 轮询；审计这一条记 accepted，作业结束时 hub_jobs 再记 */
            if (call.job) {
                httpd_resp_set_status(req, "202 Accepted");
                res = "accepted";
            }
            send_json_response(req, buf);
        }
    }
    heap_caps_free(buf);
    return res;
}

/* ---------------------------------------------------------------- POST /api/v1/hub/invoke */

static esp_err_t handle_invoke(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    char *body = heap_caps_malloc(INVOKE_BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) return send_error(req, 500, "no_memory");
    const int n = recv_body(req, body, INVOKE_BODY_MAX);
    if (n == -2) {
        heap_caps_free(body);
        hub_audit_record(HAU_SRC_REST, "invalid_request", NULL, "body_too_large", 0);   /* 6.10：回了错的都记 */
        return send_error(req, 413, "body_too_large");
    }
    if (n <= 0 || !hub_json_shape_ok(body, (size_t)n, SHAPE_DEPTH, SHAPE_VALUES)) {   /* 嵌套炸弹进不了 cJSON（httpd 栈 8 KB） */
        heap_caps_free(body);
        hub_audit_record(HAU_SRC_REST, "invalid_request", NULL, n <= 0 ? "empty_body" : "invalid_json", 0);
        return send_error(req, 400, n <= 0 ? "empty_body" : "invalid_json");
    }
    cJSON *root = cJSON_ParseWithLength(body, (size_t)n);
    heap_caps_free(body);
    const cJSON *c = root ? cJSON_GetObjectItemCaseSensitive(root, "command") : NULL;
    const cJSON *p = root ? cJSON_GetObjectItemCaseSensitive(root, "params") : NULL;
    if (!cJSON_IsString(c) || !c->valuestring[0] || (p && !cJSON_IsObject(p) && !cJSON_IsNull(p))) {
        const char *why = root ? "command_and_params_required" : "invalid_json";
        cJSON_Delete(root);
        hub_audit_record(HAU_SRC_REST, "invalid_request", NULL, why, 0);
        return send_error(req, 400, "command_and_params_required");
    }
    const int64_t t0 = esp_timer_get_time();
    const cJSON *params = cJSON_IsObject(p) ? p : NULL;
    const char *res = run_command(req, c->valuestring, params);
    const uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    /* 同步等了设备（控制最长约 8 s）：httpd 活着，记心跳（webui.h）。快的不记：设备页每 10 s 的轮询不能盖住层 B 本来要测的
       "httpd 的工作队列还跑不跑得动"（审计 ⚠️） */
    if (ms >= 1000u) ws_httpd_progress();
    hub_audit_record(HAU_SRC_REST, c->valuestring, params, res, ms);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ---------------------------------------------------------------- GET /api/v1/hub/devices */

static esp_err_t handle_devices(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    char q[96] = "", v[24];
    cJSON *params = cJSON_CreateObject();
    if (!params) return send_error(req, 500, "no_memory");
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "protocol", v, sizeof(v)) == ESP_OK) cJSON_AddStringToObject(params, "protocol", v);
        if (httpd_query_key_value(q, "cursor", v, sizeof(v)) == ESP_OK) cJSON_AddStringToObject(params, "cursor", v);
    }
    const int64_t t0 = esp_timer_get_time();
    const char *res = run_command(req, "hub.devices.list", params);
    hub_audit_record(HAU_SRC_REST, "hub.devices.list", params, res, (uint32_t)((esp_timer_get_time() - t0) / 1000));
    cJSON_Delete(params);
    return ESP_OK;
}

/* ---------------------------------------------------------------- GET /api/v1/muse/audit */

static esp_err_t handle_audit(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    uint32_t limit = 50;
    char q[32], v[8];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "limit", v, sizeof(v)) == ESP_OK) {
        char *end = NULL;
        const unsigned long l = strtoul(v, &end, 10);
        if (!end || *end || l < 1 || l > HAU_CAP) return send_error(req, 400, "invalid_limit");
        limit = (uint32_t)l;
    }
    if (!hub_audit_ready()) return send_error(req, 503, "audit_unavailable");   /* 审计环随 hub_svc 起（安全模式、配对剖面不起）；没起不是"忙" */
    char *buf = heap_caps_malloc(AUDIT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return send_error(req, 500, "no_memory");
    hj_t w;
    hj_init(&w, buf, AUDIT_MAX);
    if (!hub_audit_write(&w, limit) || hj_finish(&w) < 0) {
        heap_caps_free(buf);
        return send_error(req, 503, "audit_busy");
    }
    const esp_err_t r = send_json_response(req, buf);
    heap_caps_free(buf);
    return r;
}

#if CONFIG_BITENGINE_ENABLE_ZHUB
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
/* ---------------------------------------------------------------- Matter 的两个本地专用操作（设计 10.1；只在本地、不给 Muse，记审计） */

/* POST /api/v1/matter/allow_test_devices {"allow":bool} → {"allow":bool}。"允许测试设备"（9.7；默认关）：落盘读回确认后立即生效 */
static esp_err_t handle_matter_allow_test(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    if (!config_mgr_has_password()) {              /* 放宽设备认证：同写类命令，没密码不许 */
        hub_audit_note(HAU_SRC_REST, "matter.allow_test_devices", NULL, "password_required");
        return send_err_msg(req, "password_required", "set an admin password before changing Matter settings from the local API");
    }
    char body[64];
    const int n = recv_body(req, body, sizeof(body));
    cJSON *root = n > 0 ? cJSON_Parse(body) : NULL;
    const cJSON *ja = root ? cJSON_GetObjectItemCaseSensitive(root, "allow") : NULL;
    const bool ok_args = cJSON_IsBool(ja);
    const bool on = ok_args && cJSON_IsTrue(ja);
    cJSON_Delete(root);
    if (!ok_args) {
        hub_audit_note(HAU_SRC_REST, "matter.allow_test_devices", NULL, "invalid_params");
        return send_err_msg(req, "invalid_params", "body must be {\"allow\": true|false}");
    }
    const esp_err_t e = hub_matter_set_allow_test(on);
    hub_audit_note(HAU_SRC_REST, "matter.allow_test_devices", on ? "on" : "off", e == ESP_OK || e == ESP_ERR_NOT_FINISHED ? "ok" : "unavailable");
    if (e == ESP_ERR_NOT_FINISHED) {
        return send_json_response(req, on ? "{\"allow\":true,\"message\":\"Saved; restart the hub for it to take full effect.\"}"
                                          : "{\"allow\":false,\"message\":\"Saved; restart the hub for it to take full effect.\"}");
    }
    if (e != ESP_OK) {
        return send_err_msg(req, "unavailable", on ? "the hub could not save the setting, so test devices are still refused; try again in a minute"
                                                   : "test devices are refused for now, but the hub could not save that; it may allow them again after a restart, so try again");
    }
    return send_json_response(req, on ? "{\"allow\":true}" : "{\"allow\":false}");
}

/* POST /api/v1/matter/reset {"confirm":"RESET"} → 重启。只在控制器的身份丢了（ident_lost）时受理（2026-10-24 用户定）：
   建新身份，原来的 Matter 设备都要在各自的 App 里重新分享给网关 */
static esp_err_t handle_matter_reset(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    if (!config_mgr_has_password()) {
        hub_audit_note(HAU_SRC_REST, "matter.reset", NULL, "password_required");
        return send_err_msg(req, "password_required", "set an admin password before resetting the Matter controller");
    }
    char body[64];
    const int n = recv_body(req, body, sizeof(body));
    cJSON *root = n > 0 ? cJSON_Parse(body) : NULL;
    const cJSON *jc = root ? cJSON_GetObjectItemCaseSensitive(root, "confirm") : NULL;
    const bool confirmed = cJSON_IsString(jc) && jc->valuestring && !strcmp(jc->valuestring, "RESET");
    cJSON_Delete(root);
    if (!confirmed) {
        hub_audit_note(HAU_SRC_REST, "matter.reset", NULL, "invalid_params");
        return send_err_msg(req, "invalid_params", "body must be {\"confirm\": \"RESET\"}");
    }
    /* 重置成功就重启：同 /system/reboot，升级进行中不许（MG24 被打断会停在 bootloader 或半个镜像；审计） */
    if (ota_mg24_busy() || ota_cloud_busy() || webui_ota_busy()) {
        hub_audit_note(HAU_SRC_REST, "matter.reset", NULL, "busy");
        return send_err_msg(req, "busy", "a firmware update is in progress; reset the Matter controller after it finishes");
    }
    char why[160] = "";
    const esp_err_t e = hub_matter_reset(why, sizeof(why));
    if (e == ESP_ERR_INVALID_STATE) {
        hub_audit_note(HAU_SRC_REST, "matter.reset", NULL, "conflict");
        return send_err_msg(req, "conflict", why);
    }
    if (e != ESP_OK) {
        hub_audit_note(HAU_SRC_REST, "matter.reset", NULL, "unavailable");
        return send_err_msg(req, "unavailable", why[0] ? why : "the hub could not reset the Matter controller; try again");
    }
    hub_audit_note(HAU_SRC_REST, "matter.reset", NULL, "ok");
    send_json_response(req, "{\"status\":\"ok\",\"message\":\"rebooting\"}");
    ESP_LOGW(TAG, "Matter 控制器重置：重启");
    (void)config_mgr_flush(500);                   /* 刚改的设置还在 2 s 防抖里 */
    (void)hub_store_flush(2000);                   /* 审计与清掉的数据落盘 */
    vTaskDelay(pdMS_TO_TICKS(500));                /* 让回包发出去 */
    esp_restart();                                 /* CPU 复位，不重采 strap（不撞 IO28） */
    return ESP_OK;
}
#endif

/* ---------------------------------------------------------------- POST /api/v1/hub/device/sensitive（6.2、第十节：只在本地） */

static esp_err_t handle_device_sensitive(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    if (!config_mgr_has_password()) {              /* 同写类命令（run_command）：没密码不许改设备 */
        hub_audit_note(HAU_SRC_REST, "hub.device.sensitive", NULL, "password_required");
        return send_err_msg(req, "password_required", "set an admin password before changing devices from the local API");
    }
    char body[160];
    const int n = recv_body(req, body, sizeof(body));
    cJSON *root = n > 0 ? cJSON_Parse(body) : NULL;
    const cJSON *jid = root ? cJSON_GetObjectItemCaseSensitive(root, "id") : NULL;
    const cJSON *js = root ? cJSON_GetObjectItemCaseSensitive(root, "sensitive") : NULL;
    char id[24] = "";
    const bool ok_args = cJSON_IsString(jid) && jid->valuestring && strlen(jid->valuestring) < sizeof(id) && cJSON_IsBool(js);
    if (ok_args) snprintf(id, sizeof(id), "%s", jid->valuestring);
    const bool on = ok_args && cJSON_IsTrue(js);
    cJSON_Delete(root);
    if (!ok_args) {
        hub_audit_note(HAU_SRC_REST, "hub.device.sensitive", NULL, "invalid_params");
        return send_err_msg(req, "invalid_params", "body must be {\"id\": \"zb-…\" or \"mt-…\", \"sensitive\": true|false}");
    }
    char detail[40];
    snprintf(detail, sizeof(detail), "%s.%s", id, on ? "on" : "off");   /* 审计字段只认 [A-Za-z0-9_.-]（hau_token），冒号会变成问号 */
    /* 只认这个网关管着的设备（同 hub.device.update：Zigbee，与本网关 fabric 上的 Matter 设备 —— M7 第四步） */
    int zr = HUB_ZB_NOT_FOUND;
    if (strncasecmp(id, "zb-", 3) == 0) zr = hub_zigbee_write_device(NULL, id);
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
    else if (strncasecmp(id, "mt-", 3) == 0) {
        const int mr = hub_matter_write_device(NULL, id, NULL, NULL);
        zr = mr == HUB_MT_DONE ? HUB_ZB_DONE : mr == HUB_MT_BUSY ? HUB_ZB_BUSY : HUB_ZB_NOT_FOUND;
    }
#endif
    if (zr == HUB_ZB_BUSY) {
        hub_audit_note(HAU_SRC_REST, "hub.device.sensitive", detail, "unavailable");
        return send_err_msg(req, "unavailable", "the device list is busy, try again in a moment");
    }
    if (zr != HUB_ZB_DONE) {
        hub_audit_note(HAU_SRC_REST, "hub.device.sensitive", detail, "not_found");
        return send_err_msg(req, "not_found", "no device with that id on this hub (ids come from hub.devices.list)");
    }
    const hub_meta_change_t ch = { .name = NULL, .room = NULL, .notify = -1, .sensitive = on ? HUB_META_SENS_ON : HUB_META_SENS_OFF };
    hub_meta_ent_t e;
    const hub_meta_res_t r = hub_meta_update(id, &ch, &e);
    const char *code = r == HUB_META_OK ? "ok" : r == HUB_META_BAD_ID ? "not_found" : "unavailable";
    hub_audit_note(HAU_SRC_REST, "hub.device.sensitive", detail, code);
    if (r == HUB_META_BAD_ID) return send_err_msg(req, "not_found", "no device with that id on this hub (ids come from hub.devices.list)");
    if (r == HUB_META_FULL) return send_err_msg(req, "unavailable", "the hub has no room for more device settings; clear another device's settings first");
    if (r == HUB_META_STORE) return send_err_msg(req, "unavailable", "the hub could not read its saved device settings when it started; restart the hub, then try again");
    if (r != HUB_META_OK) return send_err_msg(req, "unavailable", "the hub could not save the change right now; try again in a minute");
    /* id 按列表里的写法（小写），同 hub.device.update 的回读（审计）。从传进来的 id 转（上面已核过是列表里的 zb- 设备）：
       别从 e.id8 拼 —— 只剩敏感位的条目关掉后整条回收清零，拼出来是 zb-0000…；sensitive 也照请求回（回收了就是 false） */
    for (char *p = id; *p; p++) *p = (char)tolower((unsigned char)*p);
    char out[96];
    snprintf(out, sizeof(out), "{\"ok\":true,\"result\":{\"id\":\"%s\",\"sensitive\":%s}}", id, on ? "true" : "false");
    return send_json_response(req, out);
}

// ---- GET /api/v1/zigbee/ncp_diag[?refresh=1] ---- 直连型：NCP 自己的计数器与子设备表（zhub.h 的 zhub_ncp_diag_t）
/* 回的是最近读完的那份（age_ms）；refresh=1 或还没读过时顺带投递一次重读（约 1 s），过一会儿再取。只读，不动 NCP 状态。
   计数器是 NCP 上电以来的累计（16 位封顶），看的是两次之间的差。子设备的 EUI64 与 /hub/devices 的 id 同一份信息，不是秘密 */
static esp_err_t handle_zigbee_ncp_diag(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    bool refresh = false;
    char q[32], v[4] = "";
    const esp_err_t qe = httpd_req_get_url_query_str(req, q, sizeof(q));   /* 太长回 TRUNC（已截断、以 0 结尾）：照样认前面的 refresh */
    if ((qe == ESP_OK || qe == ESP_ERR_HTTPD_RESULT_TRUNC) && httpd_query_key_value(q, "refresh", v, sizeof(v)) != ESP_ERR_NOT_FOUND) refresh = v[0] == '1';
    zhub_ncp_diag_t *d = heap_caps_malloc(sizeof(*d), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!d) return send_error(req, 500, "no_memory");
    zhub_ncp_diag_get(d);
    const esp_err_t rq = (refresh || !d->valid) ? zhub_ncp_diag_request() : ESP_OK;
    enum { CAP = 3072 };                           /* 计数器 42 × 约 26 B + 子设备 16 × 约 90 B + 头，约 2.6 KB */
    char *buf = heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        heap_caps_free(d);
        return send_error(req, 500, "no_memory");
    }
    size_t n = 0;
    int w = snprintf(buf, CAP, "{\"valid\":%s,\"busy\":%s,\"requested\":%s,\"age_ms\":%"PRIu32",\"reads\":%"PRIu32",\"fails\":%"PRIu32",\"security_bitmask\":%u,\"counters\":{",
                     d->valid ? "true" : "false", (d->busy || (rq == ESP_OK && (refresh || !d->valid))) ? "true" : "false",
                     rq == ESP_OK && (refresh || !d->valid) ? "true" : "false", d->age_ms, d->reads, d->fails, (unsigned)d->sec_bitmask);
    bool ok = w > 0 && (size_t)w < CAP;
    if (ok) n = (size_t)w;
    for (unsigned i = 0; ok && i < ZHUB_NCP_CTR_N; i++) {
        w = snprintf(buf + n, CAP - n, "%s\"%s\":%u", i ? "," : "", zhub_ncp_counter_name(i), (unsigned)d->ctr[i]);
        ok = w > 0 && (size_t)w < CAP - n;
        if (ok) n += (size_t)w;
    }
    if (ok) {
        w = snprintf(buf + n, CAP - n, "},\"children\":[");
        ok = w > 0 && (size_t)w < CAP - n;
        if (ok) n += (size_t)w;
    }
    for (unsigned i = 0; ok && i < d->n_child && i < ZHUB_NCP_CHILD_MAX; i++) {
        const zhub_ncp_child_t *c = &d->child[i];
        w = snprintf(buf + n, CAP - n, "%s{\"ieee\":\"%02X%02X%02X%02X%02X%02X%02X%02X\",\"nwk\":\"0x%04X\",\"type\":%u,\"timeout\":%u,\"remaining\":%"PRIu32"}",
                     i ? "," : "", c->eui64[7], c->eui64[6], c->eui64[5], c->eui64[4], c->eui64[3], c->eui64[2], c->eui64[1], c->eui64[0],
                     (unsigned)c->id, (unsigned)c->type, (unsigned)c->timeout, c->remaining);
        ok = w > 0 && (size_t)w < CAP - n;
        if (ok) n += (size_t)w;
    }
    if (ok) {
        w = snprintf(buf + n, CAP - n, "],\"children_more\":%s}", d->child_more ? "true" : "false");
        ok = w > 0 && (size_t)w < CAP - n;
    }
    heap_caps_free(d);
    const esp_err_t r = ok ? send_json_response(req, buf) : send_error(req, 500, "truncated");
    heap_caps_free(buf);
    return r;
}
#endif

#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
static esp_err_t handle_matter_status(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    matter_ctrl_status_t st;
    matter_ctrl_get_status(&st);
    char buf[512];
    const int w = snprintf(buf, sizeof(buf),
                           "{\"state\":\"%s\",\"fail_step\":%s%s%s,\"fail_err\":%" PRId32 ",\"tries\":%" PRIu32 ",\"ident_new\":%s,\"matter_rebound\":%s,"
                           "\"fabric_id\":\"%016" PRIX64 "\",\"compressed_fabric_id\":\"%016" PRIX64 "\",\"node_id\":%" PRIu64 ","
                           "\"vendor_id\":%u,\"started_ms\":%" PRIu32 ",\"heap_before\":%" PRIu32 ",\"heap_after\":%" PRIu32 ","
                           "\"nodes_ok\":%s,\"nodes\":%u,\"busy\":%s,\"subs\":%u,\"subs_up\":%u,\"allow_test_devices\":%s}",
                           matter_ctrl_state_str(st.state),
                           /* fail_step 是 matter_ctrl.cpp 里的静态字面量（只有字母和下划线），不必转义 */
                           st.fail_step ? "\"" : "", st.fail_step ? st.fail_step : "null", st.fail_step ? "\"" : "",
                           st.fail_err, st.tries, st.ident_new ? "true" : "false", st.matter_rebound ? "true" : "false", st.fabric_id, st.compressed_fabric_id, st.node_id,
                           (unsigned)st.vendor_id, st.started_ms, st.heap_before, st.heap_after,
                           st.nodes_ok ? "true" : "false", (unsigned)st.nodes, st.busy ? "true" : "false", (unsigned)st.subs, (unsigned)st.subs_up, st.allow_test_devices ? "true" : "false");
    if (w <= 0 || (size_t)w >= sizeof(buf)) return send_error(req, 500, "truncated");
    return send_json_response(req, buf);
}
#endif

/* ---------------------------------------------------------------- GET /api/v1/hub/job?id= （6.6、10.1） */

/* 作业的结论：{"ok":true,"result":{"job","command","state":"running|done|failed|timeout","age_s"[,"result":{…}|"error","message"]}}。
   没有这个 id（从没开过、或那一格已被新作业复用）404 not_found */
static esp_err_t handle_job(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    char q[32], v[12];
    unsigned long id = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "id", v, sizeof(v)) == ESP_OK) {
        char *end = NULL;
        id = strtoul(v, &end, 10);
        if (!end || *end) id = 0;
    }
    if (!id) return send_err_msg(req, "invalid_params", "id must be the job number returned by the command");
    hjc_job_t *j = heap_caps_malloc(sizeof(*j), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *buf = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!j || !buf) {
        heap_caps_free(j);
        heap_caps_free(buf);
        return send_error(req, 500, "no_memory");
    }
    esp_err_t r;
    if (!hub_job_get((uint32_t)id, j)) {
        r = send_err_msg(req, "not_found", "no such job (it may be too old)");
    } else {
        hj_t w;
        hj_init(&w, buf, 1024);
        hj_obj_begin(&w);
        hj_kbool(&w, "ok", true);
        hj_key(&w, "result");
        hj_obj_begin(&w);
        char jid[12];
        snprintf(jid, sizeof(jid), "%lu", (unsigned long)j->id);
        hj_kstr(&w, "job", jid);
        hj_kstr(&w, "command", j->cmd);
        hj_kstr(&w, "state", hjc_state_str(j->state));
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        hj_kint(&w, "age_s", (int64_t)((now - j->t_start) / 1000));
        if (j->state == HJC_DONE) {
            hj_key(&w, "result");
            hj_raw(&w, j->result);
        } else if (j->state != HJC_RUNNING) {
            hj_kstr(&w, "error", j->err_code);
            hj_kstr(&w, "message", j->err_msg);
        }
        hj_obj_end(&w);
        hj_obj_end(&w);
        r = hj_finish(&w) < 0 ? send_error(req, 500, "truncated") : send_json_response(req, buf);
    }
    heap_caps_free(j);
    heap_caps_free(buf);
    return r;
}

#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
/* ---------------------------------------------------------------- POST /api/v1/hub/device/remove （10.1：只在本地） */

/* {"id":"mt-…","confirm":"REMOVE","force":false} → 202 {job}。Matter：先给设备发 RemoveFabric，设备确认了才删；force = 设备不答话也删
   （设备上还留着网关的 fabric，要用户在它自己的 App 里重置）。要已设管理密码；记审计（受理一条 accepted、结束一条最终结论）。
   Zigbee（ZDO Leave）还没做：zb- 回 unsupported */
static esp_err_t handle_device_remove(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    if (!config_mgr_has_password()) {
        hub_audit_record(HAU_SRC_REST, "hub.device.remove", NULL, "password_required", 0);
        return send_err_msg(req, "password_required", "set an admin password before removing devices");
    }
    char body[192];
    const int n = recv_body(req, body, sizeof(body));
    if (n <= 0) return send_err_msg(req, "invalid_params", "body must be {\"id\":\"mt-…\",\"confirm\":\"REMOVE\"}");
    cJSON *root = cJSON_ParseWithLength(body, (size_t)n);
    const cJSON *id = root ? cJSON_GetObjectItemCaseSensitive(root, "id") : NULL;
    const cJSON *confirm = root ? cJSON_GetObjectItemCaseSensitive(root, "confirm") : NULL;
    const cJSON *force = root ? cJSON_GetObjectItemCaseSensitive(root, "force") : NULL;
    esp_err_t r;
    if (!cJSON_IsString(id) || !cJSON_IsString(confirm) || strcmp(confirm->valuestring, "REMOVE") != 0 ||
        (force && !cJSON_IsBool(force))) {
        hub_audit_record(HAU_SRC_REST, "hub.device.remove", NULL, "invalid_params", 0);
        r = send_err_msg(req, "invalid_params", "body must be {\"id\":\"mt-…\",\"confirm\":\"REMOVE\"} (optional \"force\":true)");
    } else if (strncmp(id->valuestring, "mt-", 3) != 0) {
        hub_audit_record(HAU_SRC_REST, "hub.device.remove", NULL, "unsupported", 0);
        r = send_err_msg(req, "unsupported", "only Matter devices (mt-…) can be removed from the hub so far");
    } else {
        uint32_t job = 0;
        char why[200] = "";
        const esp_err_t e = hub_matter_remove(id->valuestring, cJSON_IsTrue(force), &job, why, sizeof(why));
        if (e != ESP_OK) {
            const char *code = e == ESP_ERR_INVALID_ARG ? "invalid_params" : e == ESP_ERR_NOT_FOUND ? "not_found" : "busy";
            hub_audit_record(HAU_SRC_REST, "hub.device.remove", NULL, code, 0);
            r = send_err_msg(req, code, why);
        } else {
            hub_audit_record(HAU_SRC_REST, "hub.device.remove", NULL, "accepted", 0);
            char out[96];
            snprintf(out, sizeof(out), "{\"ok\":true,\"result\":{\"job\":\"%lu\",\"state\":\"running\"}}", (unsigned long)job);
            httpd_resp_set_status(req, "202 Accepted");
            r = send_json_response(req, out);
        }
    }
    cJSON_Delete(root);
    return r;
}
#endif

void register_hub_handlers(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/v1/hub/invoke",  .method = HTTP_POST, .handler = handle_invoke },
        { .uri = "/api/v1/hub/devices", .method = HTTP_GET,  .handler = handle_devices },
        { .uri = "/api/v1/muse/audit",  .method = HTTP_GET,  .handler = handle_audit },
        { .uri = "/api/v1/hub/job",     .method = HTTP_GET,  .handler = handle_job },
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
        { .uri = "/api/v1/matter/status", .method = HTTP_GET, .handler = handle_matter_status },
        { .uri = "/api/v1/matter/allow_test_devices", .method = HTTP_POST, .handler = handle_matter_allow_test },
        { .uri = "/api/v1/matter/reset", .method = HTTP_POST, .handler = handle_matter_reset },
        { .uri = "/api/v1/hub/device/remove", .method = HTTP_POST, .handler = handle_device_remove },
#endif
#if CONFIG_BITENGINE_ENABLE_ZHUB
        { .uri = "/api/v1/zigbee/ncp_diag", .method = HTTP_GET, .handler = handle_zigbee_ncp_diag },
        { .uri = "/api/v1/hub/device/sensitive", .method = HTTP_POST, .handler = handle_device_sensitive },
#endif
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        if (httpd_register_uri_handler(server, &uris[i]) != ESP_OK) ESP_LOGE(TAG, "注册 %s 失败", uris[i].uri);
    }
}
