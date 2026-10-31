/* muse_api —— /api/v1/muse/… 端点与配对剖面的精简 WebUI（docs/15_muse_direct_design_v1.md 10.1、3.2）。
 * 只在 BITENGINE_ENABLE_MUSE_LINK 下编（webui/CMakeLists.txt）；HA 型里这个文件不存在。
 *
 * 写端点首行 CROSS_ORIGIN_GUARD，然后 AUTH_REQUIRED（红线 5：反过来就是密码预言机）。读端点也用 AUTH_REQUIRED：
 * 设备没设密码时 check_auth 放行，但这里读出来的都不是秘密（SDK token 只报 set / unset）。
 * httpd 是单任务：这里不做网络 IO（配对前的"连不连得上 Muse"放在配对剖面里查，muse_pair.c）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble_window.h"
#include "cJSON.h"
#include "config_mgr.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hub_audit.h"
#include "hub_events.h"
#include "hub_model.h"
#include "hub_params.h"
#include "hub_registry.h"
#include "hub_store.h"
#include "hub_svc.h"
#include "json_escape.h"
#include "mbedtls/platform_util.h"
#include "muse_cfg.h"
#include "muse_hold.h"      /* /muse/status 的 hold_pending（附录 E33） */
#include "muse_link.h"
#include "muse_pair.h"
#include "ota_cloud.h"
#include "ota_mg24.h"
#include "thread_inventory.h"
#include "watchdog.h"
#include "webui.h"

static const char *TAG = "muse_api";

/* 同 rest_api.c 的 recv_body（那边是 static，导出它要动 HA 型的代码）：>0 长度；-1 空 / 读失败；-2 超出缓冲 */
static int muse_recv_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    int total = req->content_len;
    if (total <= 0) return -1;
    if ((size_t)total >= buf_size) return -2;
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) return -1;
        got += r;
    }
    buf[got] = '\0';
    return got;
}

/* hubdb 还没就绪（安全模式不起 hub_store；正常开机里 webui 比 hub_store 早起几秒）：设置读不到也写不进，
   回 503 让前端稍后重试，别误报成"没设 token" / "没启用"（2026-10-22 审计） */
static bool hubdb_ready(void)
{
    hub_store_status_t st;
    hub_store_get_status(&st);
    return st.ready;
}

/* 进 cJSON 之前的形状预检（hub_json_shape_ok）：几百层 [[[ 就能把 httpd 的 8 KB 栈打爆（cJSON 每层约 64 B，2026-10-22 审计实测） */
#define CFG_SHAPE_DEPTH   4
#define CFG_SHAPE_VALUES  48

/* 权限位存储格式不对（降级过 / 坏了）：muse_cfg_read_perm 按只读给 0（N4）。这是永久的 —— 回 503 会让设置读不出、也写不回去，
   只剩恢复出厂（2026-10-22 审计）。所以照只读显示、带 perm_unreadable，POST 给了 perm 就整份覆盖；读超时这类暂时的错才回 503。 */
static bool perm_format_bad(esp_err_t e)
{
    return e == ESP_ERR_INVALID_RESPONSE || e == ESP_ERR_INVALID_VERSION || e == ESP_ERR_INVALID_SIZE;
}

/* 本地改了什么记审计（6.10；第二轮审计：5.5 靠"敏感权限只能本地开"，这一环要留痕）。开 SENSITIVE 另写一行 W 日志进 log_store。
   动作列只写哪一项变成了什么，不写 token 本身 */
static void audit_config_change(const cJSON *tok, const cJSON *name, const cJSON *en, uint8_t perm_new, bool perm_changed,
                                uint8_t perm_old, uint16_t ntf_new, bool ntf_changed)
{
    if (tok) hub_audit_note(HAU_SRC_REST, "muse.config", cJSON_IsString(tok) && tok->valuestring[0] ? "sdk_token_set" : "sdk_token_removed", "ok");
    if (name) hub_audit_note(HAU_SRC_REST, "muse.config", "display_name", "ok");
    if (en) hub_audit_note(HAU_SRC_REST, "muse.config", cJSON_IsTrue(en) ? "enabled" : "disabled", "ok");
    if (perm_changed) {
        char a[24];
        snprintf(a, sizeof(a), "perm_%s%s%s%s", perm_new & HUB_PERMBIT_CONTROL ? "c" : "", perm_new & HUB_PERMBIT_MANAGE ? "m" : "",
                 perm_new & HUB_PERMBIT_SENSITIVE ? "s" : "", (perm_new & HUB_PERMBIT_ALL) ? "" : "none");
        hub_audit_note(HAU_SRC_REST, "muse.config", a, "ok");
        if ((perm_new & HUB_PERMBIT_SENSITIVE) && !(perm_old & HUB_PERMBIT_SENSITIVE)) {
            ESP_LOGW(TAG, "本地打开了 Muse 的敏感权限（开门锁、操作标为敏感的设备）");
        }
    }
    if (ntf_changed) {
        char a[16];
        snprintf(a, sizeof(a), "notify_%02x", (unsigned)ntf_new);
        hub_audit_note(HAU_SRC_REST, "muse.config", a, "ok");
    }
}

/* ---------------------------------------------------------------- GET /api/v1/muse/config */

static esp_err_t handle_muse_config_get(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    if (!hubdb_ready()) return send_error(req, 503, "hubdb_not_ready");
    char name[MUSE_DISPLAY_NAME_MAX + 1];
    muse_cfg_get_display_name(name, sizeof(name));
    char esc[MUSE_DISPLAY_NAME_MAX * 6 + 1];
    webui_json_escape(esc, sizeof(esc), name);
    /* 权限位与推送类别：读不准就别显示一个错的（权限读不准时固件按只读算，页面上显示成"关"会误导） */
    uint8_t perm = 0;
    uint16_t notify = 0;
    const esp_err_t pe = muse_cfg_read_perm(&perm);
    const bool perm_bad = pe != ESP_OK && perm_format_bad(pe);
    if ((pe != ESP_OK && !perm_bad) || muse_cfg_read_notify(&notify) != ESP_OK) return send_error(req, 503, "hubdb_busy");
#define B(x) ((x) ? "true" : "false")
    char buf[640];
    int n = snprintf(buf, sizeof(buf),
                     "{\"enabled\":%s,\"display_name\":\"%s\",\"sdk_token\":\"%s\",\"paired\":%s,"
                     "\"password_set\":%s,\"perm\":{\"control\":%s,\"manage\":%s,\"sensitive\":%s},\"perm_unreadable\":%s,"
                     "\"notify\":{\"alarm\":true,\"security\":%s,\"availability\":%s,\"lifecycle\":%s,\"gateway\":%s,\"battery\":%s}}",
                     B(muse_cfg_enabled()), esc, muse_cfg_sdk_token_set() ? "set" : "unset", B(muse_cfg_paired()),
                     B(config_mgr_has_password()), B(perm & HUB_PERMBIT_CONTROL), B(perm & HUB_PERMBIT_MANAGE),
                     B(perm & HUB_PERMBIT_SENSITIVE), B(perm_bad), B(notify & (1u << HUB_EVC_SECURITY)), B(notify & (1u << HUB_EVC_AVAILABILITY)),
                     B(notify & (1u << HUB_EVC_LIFECYCLE)), B(notify & (1u << HUB_EVC_GATEWAY)), B(notify & (1u << HUB_EVC_BATTERY)));
#undef B
    if (n < 0 || n >= (int)sizeof(buf)) return send_error(req, 500, "response_too_large");
    return send_json_response(req, buf);
}

/* ---------------------------------------------------------------- POST /api/v1/muse/config */

static esp_err_t handle_muse_config_post(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    if (!hubdb_ready()) return send_error(req, 503, "hubdb_not_ready");
    char body[768];
    int blen = muse_recv_body(req, body, sizeof(body));
    if (blen == -2) return send_error(req, 413, "body_too_large");
    if (blen <= 0) return send_error(req, 400, "empty_body");
    cJSON *root = hub_json_shape_ok(body, (size_t)blen, CFG_SHAPE_DEPTH, CFG_SHAPE_VALUES) ? cJSON_Parse(body) : NULL;
    mbedtls_platform_zeroize(body, sizeof(body));          /* 可能带 SDK token */
    if (!root) return send_error(req, 400, "invalid_json");

    const cJSON *en   = cJSON_GetObjectItem(root, "enabled");
    const cJSON *tok  = cJSON_GetObjectItem(root, "sdk_token");
    const cJSON *name = cJSON_GetObjectItem(root, "display_name");
    const cJSON *perm = cJSON_GetObjectItem(root, "perm");
    const cJSON *ntf  = cJSON_GetObjectItem(root, "notify");
    /* 权限位 / 推送类别：在现值上改给出的那几项（没给的不动）；键必须认识、值必须是布尔（拼错的键不该被悄悄忽略） */
    uint8_t perm_cur = 0, perm_new = 0;
    uint16_t ntf_cur = 0, ntf_new = 0;
    const char *bad = NULL;
    bool perm_bad = false;                                 /* 存着的格式不对：perm_cur 是只读的 0，给了 perm 就写（覆盖掉坏的） */
    if (perm || ntf) {
        const esp_err_t pe = muse_cfg_read_perm(&perm_cur);
        perm_bad = pe != ESP_OK && perm_format_bad(pe);
        if ((pe != ESP_OK && !perm_bad) || muse_cfg_read_notify(&ntf_cur) != ESP_OK) {
            cJSON_Delete(root);
            return send_error(req, 503, "hubdb_busy");     /* 读超时之类：别在一个错的现值上改 */
        }
    }
    perm_new = perm_cur;
    ntf_new = ntf_cur;
    if (perm) {
        static const struct { const char *k; uint8_t bit; } kp[] = {
            { "control", HUB_PERMBIT_CONTROL }, { "manage", HUB_PERMBIT_MANAGE }, { "sensitive", HUB_PERMBIT_SENSITIVE } };
        if (!cJSON_IsObject(perm)) bad = "invalid_perm";
        for (const cJSON *it = perm && cJSON_IsObject(perm) ? perm->child : NULL; it && !bad; it = it->next) {
            size_t i = 0;
            while (i < sizeof(kp) / sizeof(kp[0]) && strcmp(kp[i].k, it->string)) i++;
            if (i == sizeof(kp) / sizeof(kp[0]) || !cJSON_IsBool(it)) { bad = "invalid_perm"; break; }
            perm_new = cJSON_IsTrue(it) ? (uint8_t)(perm_new | kp[i].bit) : (uint8_t)(perm_new & ~kp[i].bit);
        }
    }
    if (ntf && !bad) {
        static const struct { const char *k; hub_evt_class_t c; } kn[] = {
            { "security", HUB_EVC_SECURITY }, { "availability", HUB_EVC_AVAILABILITY }, { "lifecycle", HUB_EVC_LIFECYCLE },
            { "gateway", HUB_EVC_GATEWAY }, { "battery", HUB_EVC_BATTERY } };
        if (!cJSON_IsObject(ntf)) bad = "invalid_notify";
        for (const cJSON *it = ntf && cJSON_IsObject(ntf) ? ntf->child : NULL; it && !bad; it = it->next) {
            if (!strcmp(it->string, "alarm")) {            /* 报警恒推（6.7）：给 true 可以，给 false 拒 */
                if (!cJSON_IsTrue(it)) bad = "alarm_cannot_be_disabled";
                continue;
            }
            size_t i = 0;
            while (i < sizeof(kn) / sizeof(kn[0]) && strcmp(kn[i].k, it->string)) i++;
            if (i == sizeof(kn) / sizeof(kn[0]) || !cJSON_IsBool(it)) { bad = "invalid_notify"; break; }
            ntf_new = cJSON_IsTrue(it) ? (uint16_t)(ntf_new | (1u << kn[i].c)) : (uint16_t)(ntf_new & ~(1u << kn[i].c));
        }
    }
    const char *err_msg = NULL;
    int err_status = 400;

    /* 先全部校验，再写：不留"写了一半"（任一字段不对整份拒收） */
    if (bad) err_msg = bad;
    else if (en && !cJSON_IsBool(en)) err_msg = "invalid_enabled";
    else if (tok && !cJSON_IsString(tok) && !cJSON_IsNull(tok)) err_msg = "invalid_sdk_token";
    else if (cJSON_IsString(tok) && tok->valuestring[0] && !muse_sdk_token_valid(tok->valuestring)) err_msg = "invalid_sdk_token";
    else if (name && !cJSON_IsString(name)) err_msg = "invalid_display_name";
    else if (cJSON_IsString(name) && name->valuestring[0] && !muse_display_name_valid(name->valuestring)) err_msg = "invalid_display_name";
    else if (cJSON_IsTrue(en) && !config_mgr_has_password()) { err_msg = "password_required"; err_status = 409; }  /* 设计 10.1 */
    /* 没设密码时局域网里谁都能改设置：SDK token 也要求先设密码，免得被人预先塞进一个 token（审计 B5） */
    else if (cJSON_IsString(tok) && tok->valuestring[0] && !config_mgr_has_password()) { err_msg = "password_required"; err_status = 409; }
    /* 开敏感权限要求已设管理密码（10.1；没密码时局域网里谁都能改设置） */
    else if ((perm_new & HUB_PERMBIT_SENSITIVE) && !(perm_cur & HUB_PERMBIT_SENSITIVE) && !config_mgr_has_password()) {
        err_msg = "password_required";
        err_status = 409;
    }
    else if (!en && !tok && !name && !perm && !ntf) err_msg = "nothing_to_set";

    esp_err_t err = ESP_OK;
    if (!err_msg) {
        if (tok) err = muse_cfg_set_sdk_token(cJSON_IsString(tok) ? tok->valuestring : NULL);
        if (err == ESP_OK && name) err = muse_cfg_set_display_name(name->valuestring);
        if (err == ESP_OK && en) err = muse_cfg_set_enabled(cJSON_IsTrue(en));
        if (err == ESP_OK && perm && (perm_new != perm_cur || perm_bad)) err = muse_cfg_set_perm(perm_new);
        if (err == ESP_OK && ntf && ntf_new != ntf_cur) err = muse_cfg_set_notify(ntf_new);
        if (err != ESP_OK) { err_msg = "save_failed"; err_status = 500; }
    }
    if (!err_msg) {
        /* 告诉会话任务（只投事件、不等；配对剖面 / 安全模式里 muse_link 没起，返回 INVALID_STATE，忽略）：
           token 变了要重新喂给 vm_api；显示名变了要重新注册（14.5 用 registrations 证明真的重注册了） */
        if (tok || name || (ntf && ntf_new != ntf_cur)) (void)muse_link_config_changed();   /* 推送类别由会话任务重新读进 hub_events */
        if (name || (perm && (perm_new != perm_cur || perm_bad))) (void)muse_link_reregister();   /* 权限位变了命令表要重发（14.5） */
        if (en) (void)muse_link_set_enabled(cJSON_IsTrue(en));
        if (cJSON_IsTrue(en)) muse_hold_clear();   /* 附录 E33：用户最新的选择为准，别让安全模式那次的留言下次开机又把它关掉 */
        audit_config_change(tok, name, en, perm_new, perm && (perm_new != perm_cur || perm_bad), perm_cur,
                            ntf_new, ntf && ntf_new != ntf_cur);
    }
    if (cJSON_IsString(tok)) mbedtls_platform_zeroize(tok->valuestring, strlen(tok->valuestring));
    cJSON_Delete(root);
    if (err_msg) return send_error(req, err_status, err_msg);
    return send_json_response(req, "{\"status\":\"ok\"}");
}

/* ---------------------------------------------------------------- GET /api/v1/muse/status */

static esp_err_t handle_muse_status(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    muse_pair_status_t ms;
    muse_pair_get_status(&ms);
    blew_status_t bs;
    ble_window_get_status(&bs);
    /* 会话（muse_link）的状态：配对剖面 / 安全模式里没起，报 not_running。来自服务端的文字（原因、VM 名）已净化，这里再转义 */
    muse_status_t ls;
    const bool link_ok = muse_link_get_status(&ls) == ESP_OK;
    char link[900];
    if (link_ok) {
        char err_e[200], vm_e[100], name_e[100];
        webui_json_escape(err_e, sizeof(err_e), ls.last_error);
        webui_json_escape(vm_e, sizeof(vm_e), ls.vm_name);
        webui_json_escape(name_e, sizeof(name_e), ls.display_name);
        const int ln = snprintf(link, sizeof(link),
                 "{\"state\":\"%s\",\"since_s\":%u,\"last_error\":\"%s\",\"node_id\":\"%s\",\"display_name\":\"%s\","
                 "\"vm_name\":\"%s\",\"registrations\":%u,\"reg_backends\":%u,\"heartbeat\":%s,"
                 "\"invokes\":{\"total\":%u,\"denied\":%u,\"failed\":%u},\"stack_free\":%u,"
                 "\"pushes\":{\"sent\":%u,\"dropped\":%u,\"queued\":%u}}",
                 mlc_state_str(ls.state), (unsigned)ls.state_since_s, err_e, ls.node_id, name_e, vm_e,
                 (unsigned)ls.registrations, (unsigned)ls.reg_backends, ls.heartbeat ? "true" : "false",
                 (unsigned)ls.invokes_total, (unsigned)ls.invokes_denied, (unsigned)ls.invokes_failed,
                 (unsigned)ls.stack_min_free,
                 (unsigned)ls.pushes_sent, (unsigned)ls.pushes_dropped, (unsigned)ls.pushes_queued);
        if (ln < 0 || ln >= (int)sizeof(link)) snprintf(link, sizeof(link), "{\"state\":\"%s\"}", mlc_state_str(ls.state));
    } else {
        snprintf(link, sizeof(link), "{\"state\":\"not_running\"}");
    }
    /* Thread 网络层清单的统计（7.1 的"持 OT 锁 < 20 ms"上板看 lock_max_us；252 台满载时与 srp_readvertise 的 scan_lock 一起看） */
    thread_inventory_stats_t ti;
    thread_inventory_get_stats(&ti);
    hub_svc_status_t hs;                          /* 事件管线 / 清单的常驻任务（上板确认栈够不够） */
    hub_svc_get_status(&hs);
    const size_t cap = 2560;                      /* 最坏约 2.2 KB（link 对象满载 + blew 段 + 清单统计 + hub_svc），留余量 */
    char *buf = malloc(cap);                      /* 不放 httpd 的 8 KB 栈 */
    if (!buf) return send_error(req, 500, "no_memory");
    int n = snprintf(buf, cap,
        "{\"hubdb_ready\":%s,\"state\":\"%s\",\"pair_boot\":%s,\"window_s\":%u,\"remaining_s\":%u,\"ble_name\":\"%s\","
        "\"last_result\":\"%s\",\"last_result_epoch\":%u,\"paired\":%s,\"sdk_token\":\"%s\",\"enabled\":%s,\"hold_pending\":%s,"
        "\"blew\":{\"state\":\"%s\",\"policy\":\"isolated\",\"bt_mem_released\":%s,\"opened_ms\":%u,"
        "\"internal_free_at_open\":%u,\"dma_free_at_open\":%u,\"internal_min_open\":%u,\"host_stack_free\":%u,\"last_err\":\"%s\","
        "\"selfcheck\":{\"state\":\"%s\",\"ack_before_pm\":%u,\"ack_after_pm\":%u,\"children_before\":%u,\"children_after\":%u,"
        "\"consecutive_degraded\":%u,\"total\":%u}},\"link\":%s,"
        "\"thread_inventory\":{\"entries\":%u,\"hosts\":%u,\"children\":%u,\"routers\":%u,\"hosts_truncated\":%s,"
        "\"refreshes\":%u,\"ot_busy\":%u,\"lock_us\":%u,\"lock_max_us\":%u,\"refresh_us\":%u,\"refresh_max_us\":%u,"
        "\"joined\":%u,\"left\":%u},\"notify_mask\":%u,\"hub_svc\":{\"running\":%s,\"rounds\":%u,\"stack_free\":%u}}",
        hubdb_ready() ? "true" : "false", ms.state, ms.pair_boot ? "true" : "false", (unsigned)ms.window_s, (unsigned)ms.remaining_s, ms.ble_name,
        ms.last_result, (unsigned)ms.last_result_epoch, muse_cfg_paired() ? "true" : "false",
        muse_cfg_sdk_token_set() ? "set" : "unset", muse_cfg_enabled() ? "true" : "false",
        muse_hold_pending() ? "true" : "false",      /* 安全模式退出时留的"先关掉 Muse"还没被开机消费（附录 E33） */
        bs.state, bs.bt_mem_released ? "true" : "false", (unsigned)bs.opened_ms, (unsigned)bs.internal_free_at_open,
        (unsigned)bs.dma_free_at_open, (unsigned)bs.internal_min_open, (unsigned)bs.host_stack_free,
        bs.last_err ? esp_err_to_name(bs.last_err) : "none",
        bs.selfcheck, (unsigned)bs.sc_before_pm, (unsigned)bs.sc_after_pm, (unsigned)bs.sc_children_before,
        (unsigned)bs.sc_children_after, (unsigned)bs.sc_consecutive_degraded, (unsigned)bs.sc_total, link,
        (unsigned)ti.entries, (unsigned)ti.hosts, (unsigned)ti.children, (unsigned)ti.routers, ti.hosts_truncated ? "true" : "false",
        (unsigned)ti.refreshes, (unsigned)ti.ot_busy, (unsigned)ti.last_lock_us, (unsigned)ti.max_lock_us,
        (unsigned)ti.last_refresh_us, (unsigned)ti.max_refresh_us, (unsigned)ti.joined, (unsigned)ti.left,
        (unsigned)hub_events_get_notify(), hs.running ? "true" : "false", (unsigned)hs.rounds, (unsigned)hs.stack_min_free);   /* 生效中的推送类别（会话任务读进 hub_events 的那份，不是存储里的）：test_api 据此证明改了真的生效 */
    if (n < 0 || n >= (int)cap) {
        free(buf);
        return send_error(req, 500, "response_too_large");
    }
    const esp_err_t r = send_json_response(req, buf);
    free(buf);
    return r;
}

/* ---------------------------------------------------------------- POST /api/v1/muse/unpair（4.10、10.1） */

static esp_err_t handle_muse_unpair(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    /* 先判 muse_link 在不在（配对剖面 / 安全模式里没起：409，前端不该重试），再判存储（暂时没就绪：503，可以重试） */
    muse_status_t ls;
    if (muse_link_get_status(&ls) == ESP_ERR_INVALID_STATE) return send_error(req, 409, "muse_not_running");
    if (!hubdb_ready()) return send_error(req, 503, "hubdb_not_ready");   /* 存储没就绪时凭据删不掉，别回 200 */
    char body[64];
    const int blen = muse_recv_body(req, body, sizeof(body));
    if (blen == -2) return send_error(req, 413, "body_too_large");
    if (blen <= 0) return send_error(req, 400, "empty_body");
    cJSON *root = hub_json_shape_ok(body, (size_t)blen, CFG_SHAPE_DEPTH, CFG_SHAPE_VALUES) ? cJSON_Parse(body) : NULL;
    const cJSON *c = root ? cJSON_GetObjectItemCaseSensitive(root, "confirm") : NULL;
    const bool confirmed = cJSON_IsString(c) && strcmp(c->valuestring, "UNPAIR") == 0;
    cJSON_Delete(root);
    if (!confirmed) return send_error(req, 400, "confirm_required");     /* {"confirm":"UNPAIR"}：防误点 */
    /* 会话任务去断开、删 acc_tok / ref_tok / vm_id（SDK token 与设置保留）。服务端仍会显示它离线：提示用户去 App 里删设备 */
    const esp_err_t ue = muse_link_unpair_local();
    if (ue == ESP_ERR_INVALID_STATE) return send_error(req, 409, "muse_not_running");
    if (ue != ESP_OK) return send_error(req, 503, "busy_try_again");    /* 会话任务的事件队列满了，什么都没做 */
    hub_audit_note(HAU_SRC_REST, "muse.unpair", "", "ok");
    return send_json_response(req, "{\"status\":\"ok\",\"hint\":\"remove the device in the Muse app too\"}");
}

/* ---------------------------------------------------------------- POST /api/v1/muse/test_push（10.1、4.9） */

static esp_err_t handle_muse_test_push(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    /* 在线时排一条测试消息进推送队列（会话任务去发，回包结果看 /muse/status 的 link.pushes）；不在线 409，前端提示先连上 */
    char q[32], v[4] = "";                         /* 默认进主会话（同真推送，6.7）；?main=0 推侧边会话，诊断用（2026-10-23：侧边会话 App 里找不到） */
    const esp_err_t qe = httpd_req_get_url_query_str(req, q, sizeof(q));
    const bool main_chat = !((qe == ESP_OK || qe == ESP_ERR_HTTPD_RESULT_TRUNC) && httpd_query_key_value(q, "main", v, sizeof(v)) != ESP_ERR_NOT_FOUND && v[0] == '0');
    const esp_err_t e = muse_link_test_push(main_chat);
    if (e == ESP_ERR_INVALID_STATE) return send_error(req, 409, "muse_not_running");
    if (e == ESP_ERR_NOT_ALLOWED) return send_error(req, 409, "not_online");
    if (e != ESP_OK) return send_error(req, 503, "busy_try_again");
    httpd_resp_set_status(req, "202 Accepted");
    return send_json_response(req, "{\"status\":\"queued\"}");
}

/* ---------------------------------------------------------------- POST /api/v1/muse/pair */

/* 这版固件确认过了吗（策略 C 要重启两次；没确认的新固件在配对剖面里重启就可能被回滚，设计 2.6） */
static bool running_image_confirmed(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (!running || esp_ota_get_state_partition(running, &st) != ESP_OK) return true;   /* 没有 otadata 状态 = 不走回滚 */
    return st != ESP_OTA_IMG_PENDING_VERIFY && st != ESP_OTA_IMG_NEW;
}

static esp_err_t handle_muse_pair(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    /* 安全模式先判：安全模式下 hub_store 不起，放在 hubdb_ready 后面就永远只回 503（复核 2026-10-22） */
    if (wd_safe_mode())   return send_error(req, 409, "safe_mode");
    if (!hubdb_ready())   return send_error(req, 503, "hubdb_not_ready");
    switch (muse_pair_check_request()) {
    case MUSE_PAIR_REQ_OK:              break;
    case MUSE_PAIR_REQ_NO_PASSWORD:     return send_error(req, 409, "password_required");
    case MUSE_PAIR_REQ_NO_SDK_TOKEN:    return send_error(req, 409, "sdk_token_required");
    case MUSE_PAIR_REQ_DISABLED:        return send_error(req, 409, "muse_disabled");
    case MUSE_PAIR_REQ_IN_PAIR_PROFILE: return send_error(req, 409, "already_pairing");
    case MUSE_PAIR_REQ_SAFE_MODE:       return send_error(req, 409, "safe_mode");
    }
    /* 同 /system/reboot：升级写 flash 时重启会把 MG24 留在 bootloader 里 / 把镜像写一半 */
    if (ota_mg24_busy())  return send_error(req, 409, "mg24_ota_in_progress");
    if (ota_cloud_busy()) return send_error(req, 409, "cloud_ota_in_progress");
    if (webui_ota_busy()) return send_error(req, 409, "ota_in_progress");
    if (!running_image_confirmed()) return send_error(req, 409, "firmware_not_confirmed");

    char buf[128];
    snprintf(buf, sizeof(buf), "{\"status\":\"accepted\",\"window_s\":%d,\"policy\":\"isolated\"}",
             CONFIG_BITENGINE_MUSE_PAIR_WINDOW_S);
    httpd_resp_set_status(req, "202 Accepted");
    send_json_response(req, buf);
    ESP_LOGW(TAG, "Muse 配对请求：重启进配对剖面（策略 C）");
    vTaskDelay(pdMS_TO_TICKS(500));                 /* 让回包发出去 */
    esp_err_t err = muse_pair_enter();              /* 成功不返回 */
    ESP_LOGE(TAG, "进不了配对剖面: %s", esp_err_to_name(err));
    return ESP_OK;
}

/* ---------------------------------------------------------------- POST /api/v1/muse/pair/cancel */

static esp_err_t handle_muse_pair_cancel(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    if (!ble_window_pair_boot_active()) return send_error(req, 409, "not_pairing");
    esp_err_t err = muse_pair_cancel();
    if (err != ESP_OK) return send_error(req, 503, "busy");
    httpd_resp_set_status(req, "202 Accepted");
    return send_json_response(req, "{\"status\":\"cancelling\"}");
}

/* ---------------------------------------------------------------- 配对剖面：精简 /api/v1/status 与进度页 */

/* 配对剖面里正常的 /api/v1/status 不注册（它读的 Thread / MG24 / MQTT 这次开机都没起）。这个只报够用的东西，
   不挂守卫与认证（同正常的 /status：HA 集成与探活脚本靠它）。 */
static esp_err_t handle_pair_status(httpd_req_t *req)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    muse_pair_status_t ms;
    muse_pair_get_status(&ms);
    char buf[320];
    int n = snprintf(buf, sizeof(buf),
                     "{\"profile\":\"direct\",\"mode\":\"muse_pair\",\"uptime\":%u,\"firmware\":\"%s\","
                     "\"muse_state\":\"%s\",\"remaining_s\":%u}",
                     (unsigned)(esp_timer_get_time() / 1000000), desc ? desc->version : "unknown", ms.state,
                     (unsigned)ms.remaining_s);
    if (n < 0 || n >= (int)sizeof(buf)) return send_error(req, 500, "response_too_large");
    return send_json_response(req, buf);
}

/* 配对进度页。共用前端的页面会去调这次开机没注册的端点、全是 404，所以配对剖面里首页换成这张小页（英文，同前端
   默认语言）。只调 /api/v1/status、/api/v1/auth/login、/api/v1/muse/status、/api/v1/muse/pair/cancel。
   /api/v1/status（不要登录）不再报 mode:"muse_pair" 就是已经重启回正常开机了：跳回完整界面的 Muse 页，配对结果在那里显示。 */
static const char PAIR_PAGE[] =
"<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Muse pairing</title><style>body{font:16px system-ui,sans-serif;margin:0;padding:24px 16px;max-width:520px;"
"margin:auto;color:#222;background:#fafafa}@media(prefers-color-scheme:dark){body{background:#111;color:#eee}}"
"h1{font-size:22px}.box{border:1px solid #8884;border-radius:10px;padding:14px;margin:12px 0}"
"button{font:inherit;padding:8px 16px;border-radius:8px;border:1px solid #8888;background:none;color:inherit;cursor:pointer}"
"#s{font-weight:600}.hide{display:none}input{font:inherit;padding:6px;width:60%}</style></head><body>"
"<h1>Muse pairing</h1>"
"<div id=login class='box hide'><p>Enter the admin password to see pairing progress.</p>"
"<input id=pw type=password autocomplete=current-password> <button id=go>Log in</button><p id=le></p></div>"
"<div id=main class=box><p>Status: <span id=s>…</span></p><p id=r></p>"
"<ol><li>Open the Muse app, enable Developer mode, add a gadget.</li>"
"<li>Choose <b id=n>MuseGadget-…</b>.</li>"
"<li>When the app asks for confirmation, hold the hub's button for 2–10 seconds, then release. A press shorter than 2 seconds does "
"nothing while pairing (use Cancel pairing below to stop); 10 seconds or longer resets it to factory settings.</li></ol>"
"<p><b>Only press the button when your own Muse app is asking for it.</b> The hub cannot tell which phone is connected.</p>"
"<p>The hub restarts back to normal when pairing finishes, fails, times out or is cancelled.</p>"
"<button id=c>Cancel pairing</button></div>"
"<script>"
"const $=i=>document.getElementById(i);"
"const L={idle:'Starting',advertising:'Waiting for the Muse app',connected:'Phone connected',confirm_wait:'Press and hold the button (2–10 s), then release',"
"confirmed:'Confirmed — waiting for the app',provisioning:'Saving the pairing',finishing:'Finishing — the hub will restart',preflight:'Checking the connection to Muse'};"
"async function poll(){try{const g=await (await fetch('/api/v1/status',{cache:'no-store'})).json();"
"if(g.mode!=='muse_pair'){location.hash='muse';location.reload();return}"
"const r=await fetch('/api/v1/muse/status',{credentials:'same-origin'});"
"if(r.status==401){$('login').classList.remove('hide');$('main').classList.add('hide');return}"
"const j=await r.json();$('login').classList.add('hide');$('main').classList.remove('hide');"
"$('s').textContent=L[j.state]||j.state;if(j.ble_name)$('n').textContent=j.ble_name;"
"$('r').textContent=j.remaining_s?('Window closes in '+j.remaining_s+' s'):''}catch(e){$('s').textContent='Hub restarting…'}}"
"$('go').onclick=async()=>{const r=await fetch('/api/v1/auth/login',{method:'POST',credentials:'same-origin',"
"headers:{'Content-Type':'application/json'},body:JSON.stringify({password:$('pw').value})});"
"$('le').textContent=r.ok?'':(r.status==429?'Too many attempts, try later':'Wrong password');if(r.ok)poll()};"
"$('c').onclick=async()=>{await fetch('/api/v1/muse/pair/cancel',{method:'POST',credentials:'same-origin'});poll()};"
"poll();setInterval(poll,2000);"
"</script></body></html>";

static esp_err_t handle_pair_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PAIR_PAGE, sizeof(PAIR_PAGE) - 1);
}

/* ---------------------------------------------------------------- 注册 */

void register_muse_handlers(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/v1/muse/config",      .method = HTTP_GET,  .handler = handle_muse_config_get },
        { .uri = "/api/v1/muse/config",      .method = HTTP_POST, .handler = handle_muse_config_post },
        { .uri = "/api/v1/muse/status",      .method = HTTP_GET,  .handler = handle_muse_status },
        { .uri = "/api/v1/muse/pair",        .method = HTTP_POST, .handler = handle_muse_pair },
        { .uri = "/api/v1/muse/pair/cancel", .method = HTTP_POST, .handler = handle_muse_pair_cancel },
        { .uri = "/api/v1/muse/unpair",      .method = HTTP_POST, .handler = handle_muse_unpair },
        { .uri = "/api/v1/muse/test_push",   .method = HTTP_POST, .handler = handle_muse_test_push },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(server, &uris[i]);
}

/* 配对剖面专用：精简状态 + 进度页（通配，必须最后注册） */
void register_pair_profile_handlers(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/v1/status", .method = HTTP_GET, .handler = handle_pair_status },
        { .uri = "/*",             .method = HTTP_GET, .handler = handle_pair_page },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(server, &uris[i]);
}

/* /api/v1/status 末尾追加的两个直连型字段（rest_api.c 调）：只读内存，不碰 hubdb —— /status 被前端频繁轮询 */
int muse_api_status_tail(char *out, size_t cap)
{
    blew_status_t bs;
    ble_window_get_status(&bs);
    muse_status_t ls;
    const char *link = muse_link_get_status(&ls) == ESP_OK ? mlc_state_str(ls.state) : "not_running";
    return snprintf(out, cap, ",\"muse_state\":\"%s\",\"blew_state\":\"%s\",\"muse_link\":\"%s\"}", muse_pair_state_str(), bs.state, link);
}
