#include "webui.h"
#include "config_mgr.h"
#include "utf8.h"
/* network_mgr.h 已不再需要：唯一的使用者是 2026-08-23 删掉的那段死代码
   （见 check_auth 里的说明）。留着会让人以为认证与网络状态有关联。 */

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "mbedtls/base64.h"

static const char *TAG = "webui_auth";

// ---- Session storage ----
typedef struct {
    char     token[SESSION_TOKEN_LEN + 1];
    uint32_t created_at;
    uint32_t last_active;
    bool     active;
} session_t;

static session_t s_sessions[MAX_SESSIONS];

static uint32_t now_seconds(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

/* ---- 密码验证节流（2026-08-23）----
 *
 * 没有它，跨站/rebinding 就不只是"读走点状态"，而是"由受害者浏览器同源全速爆破，
 * 拿到 token 后走 Bearer 接管全部写端点"。而密码下限只有 4 字符，哈希是**无盐
 * 单轮 SHA256** —— 在线爆破的成本本来就低得离谱。
 *
 * ⚠️ 必须**拒绝到底**：锁定期间即使密码正确也拒。写成"错了才 429、对了照样放行"
 *    的话，攻击者一次都不会慢下来，429 纯属装饰 —— 而这两种实现**用错密码测
 *    返回值完全相同**，所以判据必须是"锁定期间用正确密码打，也必须被拒"。
 *    tools/test_auth_throttle.sh 里那条就是专门用来把两者分开的。
 *
 * ⚠️ 绝不能用 vTaskDelay 做延迟：httpd 是单任务，睡在这里 = 全部端点无响应。
 *    这里只记时刻、立即返回。
 *
 * ⚠️ 计数器是**全局**的，不分来源 IP。代价：局域网内的人能把管理员挡在门外最多
 *    32 秒。接受 —— 他本来就能占满 httpd 的 socket，也能直接打无认证的 6638。
 *    分 IP 统计要一张表，而 rebinding 场景下所有请求本来就来自同一个受害者 IP，
 *    分了也拦不住那条真正危险的路径。
 *
 * ⚠️ 只节流**密码比对**，不节流会话令牌：已经登录的管理员不会被别人的爆破锁在
 *    外面。跨站请求也不会消耗这个计数器，因为 CROSS_ORIGIN_GUARD 排在
 *    AUTH_REQUIRED 之前，根本走不到这里。
 */
static uint32_t s_auth_fails;
static uint32_t s_auth_open_at;

#define AUTH_FAIL_THRESHOLD  5

static bool auth_throttled(void)
{
    return s_auth_fails >= AUTH_FAIL_THRESHOLD && now_seconds() < s_auth_open_at;
}

/* 只在**真的做过一次密码比对**之后调用。没带 Authorization 头的请求不该计数，
   否则一个没配密码的监控脚本就能把管理员锁死。 */
static void auth_note(bool ok)
{
    if (ok) { s_auth_fails = 0; s_auth_open_at = 0; return; }
    s_auth_fails++;
    if (s_auth_fails >= AUTH_FAIL_THRESHOLD) {
        uint32_t shift = s_auth_fails - AUTH_FAIL_THRESHOLD;
        if (shift > 4) shift = 4;                       /* 上限 2<<4 = 32s */
        uint32_t win = 2u << shift;
        s_auth_open_at = now_seconds() + win;
        ESP_LOGW(TAG, "密码连续失败 %" PRIu32 " 次，锁 %" PRIu32 " 秒",
                 s_auth_fails, win);
    }
}

// ---- Token generation ----
static void generate_token(char *out)
{
    uint8_t raw[16];
    esp_fill_random(raw, sizeof(raw));
    for (int i = 0; i < 16; i++) {
        sprintf(out + i * 2, "%02x", raw[i]);
    }
    out[32] = '\0';
}

// ---- Session management ----
static session_t *create_session(void)
{
    // Find free slot
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!s_sessions[i].active) {
            s_sessions[i].active = true;
            generate_token(s_sessions[i].token);
            s_sessions[i].created_at  = now_seconds();
            s_sessions[i].last_active = s_sessions[i].created_at;
            return &s_sessions[i];
        }
    }
    // Full — evict oldest
    int oldest_idx = 0;
    uint32_t oldest_time = UINT32_MAX;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].last_active < oldest_time) {
            oldest_time = s_sessions[i].last_active;
            oldest_idx = i;
        }
    }
    ESP_LOGW(TAG, "Session table full, evicting slot %d", oldest_idx);
    s_sessions[oldest_idx].active = true;
    generate_token(s_sessions[oldest_idx].token);
    s_sessions[oldest_idx].created_at  = now_seconds();
    s_sessions[oldest_idx].last_active = s_sessions[oldest_idx].created_at;
    return &s_sessions[oldest_idx];
}

/* Constant-time comparison — session tokens are secrets, strcmp would leak
   match length through timing (project rule S1). */
static bool token_equal(const char *a, const char *b)
{
    volatile uint8_t diff = 0;
    for (int i = 0; i < SESSION_TOKEN_LEN; i++) {
        diff |= (uint8_t)a[i] ^ (uint8_t)b[i];
        if (a[i] == '\0' || b[i] == '\0') {
            return false;  /* short token can never match a full-length one */
        }
    }
    return diff == 0;
}

/* 设/改密码那一刻，旧会话必须真的失效。
   原来的洞：攻击者趁**无密码窗口**打一次 /api/v1/auth/login，no_password 分支
   会白送一个有效期 24 小时的 token；用户随后老老实实设了密码，攻击者那个会话
   **继续有效到第二天** —— 用户以为自己关上了门。
   保留发起本次修改的那个 slot，否则用户刚设完密码立刻被弹回登录页。 */
static void invalidate_sessions_except(const char *keep)
{
    int killed = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!s_sessions[i].active) continue;
        if (keep && token_equal(s_sessions[i].token, keep)) continue;
        s_sessions[i].active = false;
        killed++;
    }
    if (killed) ESP_LOGW(TAG, "密码变更：作废了 %d 个既有会话", killed);
}

static session_t *find_session(const char *token)
{
    if (!token || strlen(token) != SESSION_TOKEN_LEN) return NULL;
    uint32_t now = now_seconds();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!s_sessions[i].active) continue;
        if (now - s_sessions[i].created_at > SESSION_TIMEOUT_S) {
            s_sessions[i].active = false;  // expired
            continue;
        }
        if (token_equal(s_sessions[i].token, token)) {
            s_sessions[i].last_active = now;
            return &s_sessions[i];
        }
    }
    return NULL;
}

// ---- Token extraction from request ----
static bool extract_token(httpd_req_t *req, char *out, size_t out_len)
{
    // Try Cookie first
    char cookie_buf[256];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie_buf, sizeof(cookie_buf)) == ESP_OK) {
        /* Match "token=" only at the start of a cookie name — a plain strstr
           would also match e.g. "csrftoken=..." */
        char *tok = NULL;
        char *p = cookie_buf;
        while ((p = strstr(p, "token=")) != NULL) {
            if (p == cookie_buf ||
                (p >= cookie_buf + 2 && p[-1] == ' ' && p[-2] == ';') ||
                (p > cookie_buf && p[-1] == ';')) {
                tok = p;
                break;
            }
            p += 6;
        }
        if (tok) {
            tok += 6;  // skip "token="
            char *end = strchr(tok, ';');
            size_t len = end ? (size_t)(end - tok) : strlen(tok);
            if (len > 0 && len < out_len) {
                memcpy(out, tok, len);
                out[len] = '\0';
                return true;
            }
        }
    }

    // Try Authorization: Bearer xxx
    char auth_buf[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_buf, sizeof(auth_buf)) == ESP_OK) {
        if (strncmp(auth_buf, "Bearer ", 7) == 0) {
            strlcpy(out, auth_buf + 7, out_len);
            return true;
        }
    }

    return false;
}

/* HTTP Basic support — used by the HA SMLIGHT integration (pysmlight),
   which cannot do the cookie/Bearer session flow. Password part of
   "user:password" is verified against the device password. */
static bool check_basic_auth(httpd_req_t *req)
{
    char auth_buf[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_buf,
                                    sizeof(auth_buf)) != ESP_OK) {
        return false;
    }
    if (strncmp(auth_buf, "Basic ", 6) != 0) return false;

    unsigned char decoded[96];
    size_t out_len = 0;
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &out_len,
                              (const unsigned char *)auth_buf + 6,
                              strlen(auth_buf + 6)) != 0) {
        return false;
    }
    decoded[out_len] = '\0';

    char *colon = strchr((char *)decoded, ':');
    if (!colon) return false;
    /* 锁定期间连比都不比。Basic 这条路此前连一条失败日志都没有 —— 也就是说
       通过它爆破在设备侧完全静默。 */
    if (auth_throttled()) return false;
    bool ok = config_mgr_verify_password(colon + 1);
    auth_note(ok);
    if (!ok) ESP_LOGW(TAG, "Basic 认证失败");
    return ok;
}

/* 给 otbr_api.c 的跨作用域凭据比对（2026-08-23）。
 *
 * 与 check_basic_auth 的三点关键区别，每一点都是有意的：
 *   1. 比的是**调用方给的 secret**，不是管理密码 —— 这就是"作用域"本身。
 *      同一个 token 打 /api/v1 下的任何端点 永远不会走到这里，check_auth 根本不认识它。
 *   2. **不碰登录节流计数器**。HA 若拿着过期 token 反复轮询，走管理密码那条路
 *      每次失败都会把计数器顶上去，最终把**管理员自己**锁在门外 —— 一个由
 *      第三方轮询频率决定的拒绝服务。token 是 128 位随机，在线爆破不成立，
 *      不需要节流。
 *   3. 常量时间比较，且长度不同直接判负（避免用长度做侧信道）。
 *
 * 返回值区分三态，调用方需要靠它决定"是拒绝还是继续走常规认证"：
 *   BASIC_ABSENT   请求里根本没有 Basic 头
 *   BASIC_MATCH    有，且等于 secret
 *   BASIC_MISMATCH 有，但不等于
 */
webui_basic_result_t webui_basic_check(httpd_req_t *req, const char *secret)
{
    char auth_buf[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_buf,
                                    sizeof(auth_buf)) != ESP_OK) {
        return BASIC_ABSENT;
    }
    if (strncmp(auth_buf, "Basic ", 6) != 0) return BASIC_ABSENT;

    unsigned char decoded[96];
    size_t out_len = 0;
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &out_len,
                              (const unsigned char *)auth_buf + 6,
                              strlen(auth_buf + 6)) != 0) {
        return BASIC_MISMATCH;
    }
    decoded[out_len] = '\0';
    char *colon = strchr((char *)decoded, ':');
    if (!colon) return BASIC_MISMATCH;

    const char *got = colon + 1;
    size_t want_len = strlen(secret);
    if (strlen(got) != want_len) return BASIC_MISMATCH;
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < want_len; i++) {
        diff |= (uint8_t)got[i] ^ (uint8_t)secret[i];
    }
    return diff == 0 ? BASIC_MATCH : BASIC_MISMATCH;
}

// ---- Public: check_auth ----
bool check_auth(httpd_req_t *req)
{
    /* 出厂无密码 → 放行。这是有意的：首开机得让人进得来把密码设上。
       ⚠️ 这条同时意味着**出厂态下一切端点无凭据可达** —— 跨站守卫补的正是这个
       窗口（它管"谁让浏览器发的"，与有没有密码无关）。前端 2026-08-15 起会强制
       首次设密码，但那只是前端的门，curl 不受影响。 */
    if (!config_mgr_has_password()) return true;

    /* 2026-08-23 删掉了紧跟其后的一段死代码：
         if (network_mgr_get_state() == NET_STATE_SOFTAP_ONLY &&
             !config_mgr_has_password()) return true;
       上一行已经在 !has_password 时 return true 了，所以那个 && 的右半永远为假，
       整个分支不可达。它读起来像"SoftAP 配网时特殊放行"，实际什么也没做 ——
       留着只会让人以为 SoftAP 有一条独立的放行路径而不敢动。 */

    char token[SESSION_TOKEN_LEN + 1];
    if (extract_token(req, token, sizeof(token)) &&
        find_session(token) != NULL) {
        return true;
    }

    return check_basic_auth(req);
}

// ---- Handlers ----

static esp_err_t handle_auth_status(httpd_req_t *req)
{
    bool need_password = config_mgr_has_password();
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"password_required\":%s}",
             need_password ? "true" : "false");
    return send_json_response(req, buf);
}

static esp_err_t handle_auth_login(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    // If no password, return success immediately
    if (!config_mgr_has_password()) {
        session_t *s = create_session();
        char resp[128];
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"no_password\",\"token\":\"%s\"}", s->token);
        char cookie[128];
        snprintf(cookie, sizeof(cookie),
                 "token=%s; HttpOnly; SameSite=Strict; Path=/; Max-Age=%d",
                 s->token, SESSION_TIMEOUT_S);
        httpd_resp_set_hdr(req, "Set-Cookie", cookie);
        return send_json_response(req, resp);
    }

    // Parse body
    char body[128];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) return send_error(req, 400, "empty_body");
    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");

    cJSON *pwd = cJSON_GetObjectItem(root, "password");
    if (!pwd || !cJSON_IsString(pwd)) {
        cJSON_Delete(root);
        return send_error(req, 400, "missing_password");
    }

    /* 锁定期间即使密码对也拒 —— 见 auth_throttled 上面那段：只在密码错时才 429
       的实现，攻击者一次都不会慢下来。429 与 401 分开是给人看的，前端据此
       提示"稍后再试"而不是"密码错了"。 */
    if (auth_throttled()) {
        cJSON_Delete(root);
        return send_error(req, 429, "too_many_attempts");
    }
    bool pw_ok = config_mgr_verify_password(pwd->valuestring);
    auth_note(pw_ok);
    if (!pw_ok) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "Login failed: wrong password");
        return send_error(req, 401, "wrong_password");
    }

    cJSON_Delete(root);

    session_t *s = create_session();
    char cookie[128];
    snprintf(cookie, sizeof(cookie),
             "token=%s; HttpOnly; SameSite=Strict; Path=/; Max-Age=%d",
             s->token, SESSION_TIMEOUT_S);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);

    char resp[96];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"token\":\"%s\"}", s->token);
    ESP_LOGI(TAG, "Login successful");
    return send_json_response(req, resp);
}

static esp_err_t handle_auth_logout(httpd_req_t *req)
{
    AUTH_REQUIRED(req);

    char token[SESSION_TOKEN_LEN + 1];
    if (extract_token(req, token, sizeof(token))) {
        session_t *s = find_session(token);
        if (s) s->active = false;
    }

    // Clear cookie
    httpd_resp_set_hdr(req, "Set-Cookie",
                       "token=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
    return send_json_response(req, "{\"status\":\"ok\"}");
}

static esp_err_t handle_auth_password(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[256];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) return send_error(req, 400, "empty_body");
    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");

    cJSON *pwd = cJSON_GetObjectItem(root, "new_password");
    if (!pwd || !cJSON_IsString(pwd) || strlen(pwd->valuestring) < 4) {
        cJSON_Delete(root);
        return send_error(req, 400, "password_too_short");
    }
    /* T9.33：写进配置的字符串一律是合法 UTF-8（config_mgr_set_password 也拦，这里先回 400，
       否则会被下面当成 save_failed 报成 500）。 */
    if (!utf8_valid(pwd->valuestring)) {
        cJSON_Delete(root);
        return send_error(req, 400, "invalid_utf8");
    }
    /* 上限必须在这里拦：本接口的 body 缓冲比登录的大一倍，中间那段能设出一个
       自己再也登不进来的密码，而 set_password 立刻 SHA256、长度信息就没了。
       推导见 webui.h 的 PASSWORD_MAX_BYTES。 */
    if (strlen(pwd->valuestring) > PASSWORD_MAX_BYTES) {
        cJSON_Delete(root);
        return send_error(req, 400, "password_too_long");
    }

    esp_err_t err = config_mgr_set_password(pwd->valuestring);
    cJSON_Delete(root);

    if (err != ESP_OK) return send_error(req, 500, "save_failed");

    /* 把除本次请求之外的会话全部作废 —— 见 invalidate_sessions_except 的说明。
       ⚠️ 取 token 必须在 set_password **之后**也仍然有效：这里只是读请求头，
       不依赖任何配置状态。取不到（比如用 Basic 认证改的密码）就传 NULL，
       全部作废 —— 那种情况下调用方本来就不靠会话。 */
    char cur[SESSION_TOKEN_LEN + 1];
    invalidate_sessions_except(extract_token(req, cur, sizeof(cur)) ? cur : NULL);

    /* 密码换了，之前的失败计数没有意义了；不清的话用户会带着别人留下的
       锁定窗口进入新密码，表现为"我刚设的密码登不进去"。 */
    auth_note(true);

    ESP_LOGI(TAG, "Password changed");
    return send_json_response(req, "{\"status\":\"ok\"}");
}

// ---- Route registration ----
void register_auth_handlers(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {  /* static：同 rest_api.c 的路由表，别放栈上 */
        { .uri = "/api/v1/auth/status",   .method = HTTP_GET,  .handler = handle_auth_status, .is_websocket = false },
        { .uri = "/api/v1/auth/login",    .method = HTTP_POST, .handler = handle_auth_login, .is_websocket = false },
        { .uri = "/api/v1/auth/logout",   .method = HTTP_GET,  .handler = handle_auth_logout, .is_websocket = false },
        { .uri = "/api/v1/auth/password", .method = HTTP_POST, .handler = handle_auth_password, .is_websocket = false },
    };
    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    ESP_LOGI(TAG, "Auth handlers registered (4 endpoints)");
}
