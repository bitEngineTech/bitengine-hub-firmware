/*
 * otbr_api.c — OTBR REST API (ot-br-posix compatible)
 *
 * 25 URI handlers on /node、/diagnostics 与 /api/v1/diag/thread。
 * 全部走 AUTH_REQUIRED。All OpenThread API calls protected by esp_openthread_lock.
 *
 * H1 verified: all OT API signatures confirmed from ESP-IDF headers.
 *
 * ── 认证：2026-08-15 补全，此前 25 条里只有 /api/v1/diag/thread 一条有 ──
 *
 * 原注释写的是「读端点不加认证 —— HA 的 OTBR 集成需要」。那个理由**不成立**：
 * python-otbr-api 自己确实不发任何凭据，但它把 URL 原样交给 aiohttp，而
 * aiohttp 会把 URL 里的 userinfo 变成**预发**的 Authorization: Basic
 * （client_reqrep.py: `if url.raw_user or url.raw_password: ... encode_basic_auth`）。
 * HA 的 otbr 配置流又正好是一个自由文本 URL 框（config_flow.py 的
 * `vol.Schema({CONF_URL: str})`）。而 check_basic_auth()（auth.c）本来就收
 * Basic 且只验密码那一半、忽略用户名。所以：
 *
 *     http://otbr:<设备密码>@<网关IP>:8081
 *
 * 用**未打补丁的 HA** 就能过认证。⚠️ 但密码必须是 ASCII 且不含 / ? #：
 * aiohttp 按 latin1 编 Basic（非 ASCII 直接 UnicodeEncodeError），而 / 会让
 * yarl 在建 URL 时就抛 ValueError —— 两者都发生在客户端，我们这边收不到任何
 * 请求，日志上什么都看不到。实测表见 docs/13 决策 1.5。
 * 代价是密码会明文落在 HA 的 .storage 里，
 * 失败时还会被 config_flow 打进日志 —— 但它保护的是 Thread NetworkKey，
 * 在此之前那玩意儿是局域网内 curl 一下就能拿走的。
 * ⚠️ hassio 自动发现建的条目 URL 不带 userinfo，必须删掉重新手工添加。
 *
 * ⚠️ 绝不能改成「在 httpd 上挂一个全局认证过滤器」：
 * python-otbr-api 的每个公开方法开头都会打一次 GET /api/actions 来探测
 * 字段命名风格，**只接受 200 或 404**，收到 401 会抛 OTBRError →
 * ConfigEntryNotReady → 集成根本加载不起来。我们没注册 /api/actions，
 * 80 端口靠 static_files.c 的 /api 反 SPA 兜底规则 404、8081 端口直接 404，
 * 逐个 handler 加宏才保得住这个行为。
 *
 * ⚠️ 也没有发 WWW-Authenticate：已知客户端（aiohttp 带 userinfo、pysmlight、
 * curl -u）都是预发 Basic，不需要挑战；而发了它会让浏览器在 XHR 401 时弹出
 * 原生认证框，把 SPA 的登录页顶掉。
 *
 * 上游 ot-br-posix 同样没有认证，但它默认只绑 127.0.0.1:8081，靠网络隔离；
 * 我们两个端口都绑 0.0.0.0，所以必须自己补上这一层。
 */
#include "webui.h"
#include "config_mgr.h"
#include "thread_br.h"
#include "json_escape.h"   /* S1：网络名转义，输出保证合法 UTF-8 */

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "otbr_api";

#ifdef CONFIG_OPENTHREAD_ENABLED

#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "openthread/border_agent.h"
#include "openthread/dataset.h"
#include "openthread/dataset_ftd.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"
#include "openthread/border_routing.h"
#include "openthread/platform/radio.h"   /* T3.5: otPlatRadioGetVersionString */
#include "esp_heap_caps.h"   /* 子节点/邻居快照放 PSRAM */

#define OT_LOCK_TIMEOUT_TICKS  pdMS_TO_TICKS(5000)

/* OTBR 标准路由（/node、/diagnostics）的认证闸门 —— 两个来源，都默认关。
 *
 * ① thread.rest_auth（旧）：打开后这批路由要**管理密码**。
 * ② thread.otbr_token（2026-08-23 新增）：一把只在这 25 条路由上有效的 token。
 *
 * 为什么要 ② —— 死结在上游：HA 的 otbr 集成**不支持任何认证参数**
 * （python-otbr-api 把 URL 原样拼接，凭据只能靠 aiohttp 的 userinfo 走 Basic），
 * 所以开 ① 就等于把**网关管理密码明文**塞进 HA 的 .storage 和它的 DEBUG 日志
 * （config_flow.py:148/175 会把整条 URL 打出来）—— 为了保护 NetworkKey 而扩散
 * 一把权限更大的钥匙。② 把它拆开：泄露的后果只是"能读写 Thread 数据集"，
 * 正是这把钥匙本该有的权限。
 *
 * ⚠️ 设了 token 之后，这批路由上的 Basic **只认 token，不再认管理密码**。
 *    这不是疏漏，是防一条真实的拒绝服务：HA 若拿着过期 token 反复轮询，
 *    每次失败都会撞进管理密码的比对路径，把登录节流的计数器顶上去，
 *    最终**把管理员自己锁在门外**，而节奏由第三方的轮询频率决定。
 *    没带 Basic 的请求仍然走常规认证，所以 WebUI 的会话 cookie 照常能读。
 *
 * ⚠️ 关掉时（两者都没设）**直接放行**，不能退化成 send_error —— python-otbr-api
 *    对非 200/404 一律抛 OTBRError → ConfigEntryNotReady → 集成加载不起来。
 * ⚠️ /api/v1/diag/thread 不用这个宏：它在我们自己的命名空间下，一直是无条件
 *    AUTH_REQUIRED，与 ot-br-posix 的兼容性无关。
 */
#define OTBR_AUTH_REQUIRED(req)                                            \
    do {                                                                   \
        const thread_config_t *_tc = config_mgr_get_thread();              \
        /* 取不到配置就当**开着**处理：拿不准时宁可要认证。 */                 \
        if (!_tc) { AUTH_REQUIRED(req); break; }                           \
        if (_tc->otbr_token[0]) {                                          \
            webui_basic_result_t _b =                                      \
                webui_basic_check(req, _tc->otbr_token);                   \
            if (_b == BASIC_MATCH) break;          /* token 对，放行 */      \
            if (_b == BASIC_MISMATCH) {            /* 给了但不对，到此为止 */ \
                ESP_LOGW(TAG, "OTBR token 不匹配，拒绝");                    \
                send_error(req, 401, "login_required");                    \
                return ESP_OK;                                             \
            }                                                              \
            /* BASIC_ABSENT：可能是 WebUI 的会话 cookie，交给常规认证 */       \
            AUTH_REQUIRED(req);                                            \
            break;                                                         \
        }                                                                  \
        if (_tc->rest_auth) { AUTH_REQUIRED(req); }                        \
    } while (0)

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

/* Return OT instance or send 503 and return NULL. */
static otInstance *require_ot(httpd_req_t *req)
{
    if (!thread_br_is_running()) {
        send_error(req, 503, "thread_not_running");
        return NULL;
    }
    otInstance *inst = esp_openthread_get_instance();
    if (!inst) {
        send_error(req, 503, "thread_not_running");
    }
    return inst;
}

/* Acquire OT lock with timeout; send 503 on failure. */
static bool ot_lock(httpd_req_t *req)
{
    if (!esp_openthread_lock_acquire(OT_LOCK_TIMEOUT_TICKS)) {
        send_error(req, 503, "thread_busy");
        return false;
    }
    return true;
}

static inline void ot_unlock(void) { esp_openthread_lock_release(); }

static void bytes_to_hex(const uint8_t *src, size_t len, char *dst)
{
    for (size_t i = 0; i < len; i++)
        sprintf(dst + i * 2, "%02x", src[i]);
    dst[len * 2] = '\0';
}

/* Decode hex string to bytes. Returns byte count or -1 on error. */
static int hex_to_bytes(const char *hex, uint8_t *dst, size_t max_len)
{
    size_t slen = strlen(hex);
    if (slen == 0 || slen % 2 != 0) return -1;
    size_t blen = slen / 2;
    if (blen > max_len) return -1;
    for (size_t i = 0; i < blen; i++) {
        unsigned int v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) return -1;
        dst[i] = (uint8_t)v;
    }
    return (int)blen;
}

static const char *role_str(otDeviceRole r)
{
    switch (r) {
    case OT_DEVICE_ROLE_DISABLED: return "disabled";
    case OT_DEVICE_ROLE_DETACHED: return "detached";
    case OT_DEVICE_ROLE_CHILD:    return "child";
    case OT_DEVICE_ROLE_ROUTER:   return "router";
    case OT_DEVICE_ROLE_LEADER:   return "leader";
    default:                      return "unknown";
    }
}

/* Read the COMPLETE httpd body (httpd_req_recv may return partial data);
   returns length or sends error and returns -1 */
static int recv_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    int total = req->content_len;
    if (total <= 0) {
        send_error(req, 400, "empty_body");
        return -1;
    }
    if ((size_t)total >= buf_size) {
        send_error(req, 400, "body_too_large");
        return -1;
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) {
            send_error(req, 500, "recv_failed");
            return -1;
        }
        got += r;
    }
    buf[got] = '\0';
    return got;
}

/* ------------------------------------------------------------------ */
/*  GET /node — aggregate node info                                   */
/* ------------------------------------------------------------------ */

static esp_err_t handle_node_get(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    OTBR_AUTH_REQUIRED(req);
    otInstance *inst = require_ot(req);
    if (!inst) return ESP_OK;
    if (!ot_lock(req)) return ESP_OK;

    /* Copy all data inside lock (S2: pointers to internal OT data) */
    otDeviceRole role = otThreadGetDeviceRole(inst);

    char net_name[OT_NETWORK_NAME_MAX_SIZE + 1];
    const char *nn = otThreadGetNetworkName(inst);
    strlcpy(net_name, nn ? nn : "", sizeof(net_name));

    otExtendedPanId xpanid = {0};
    const otExtendedPanId *xp_ptr = otThreadGetExtendedPanId(inst);
    if (xp_ptr) memcpy(&xpanid, xp_ptr, sizeof(xpanid));

    uint16_t rloc16 = otThreadGetRloc16(inst);

    otIp6Address rloc = {{{0}}};
    const otIp6Address *rloc_ptr = otThreadGetRloc(inst);
    if (rloc_ptr) memcpy(&rloc, rloc_ptr, sizeof(rloc));

    otExtAddress ext_addr = {0};
    const otExtAddress *ea_ptr = otLinkGetExtendedAddress(inst);
    if (ea_ptr) memcpy(&ext_addr, ea_ptr, sizeof(ext_addr));

    otBorderAgentId ba_id;
    otError ba_err = otBorderAgentGetId(inst, &ba_id);

    otLeaderData leader;
    otError ld_err = otThreadGetLeaderData(inst, &leader);

    /* NumOfRouter = 网络里实际存在的路由器数量。
       曾经错填成 otThreadGetMaxAllowedChildren()——那是"最多允许几个**子设备**"
       （本项目 CONFIG_OPENTHREAD_MLE_MAX_CHILDREN=10），于是一个只有 1 个 leader
       和 1 个休眠子设备的网络会报 "10 routers"。字段语义完全错配，而 HA 的
       otbr 集成也读这个字段。
       H1 verified（openthread/thread_ftd.h:598,611）：
         - otThreadGetMaxRouterId() 返回最大 router id（含），所以循环用 <=
         - otThreadGetRouterInfo() 对不存在的 id 返回 OT_ERROR_NOT_FOUND */
    uint16_t num_routers = 0;
    uint8_t max_router_id = otThreadGetMaxRouterId(inst);
    for (uint16_t rid = 0; rid <= max_router_id; rid++) {
        otRouterInfo ri;
        if (otThreadGetRouterInfo(inst, rid, &ri) == OT_ERROR_NONE) num_routers++;
    }

    ot_unlock();

    /* Format outside lock */
    char xpanid_hex[OT_EXT_PAN_ID_SIZE * 2 + 1];
    bytes_to_hex(xpanid.m8, OT_EXT_PAN_ID_SIZE, xpanid_hex);

    char rloc_str[OT_IP6_ADDRESS_STRING_SIZE];
    otIp6AddressToString(&rloc, rloc_str, sizeof(rloc_str));

    char ext_hex[OT_EXT_ADDRESS_SIZE * 2 + 1];
    bytes_to_hex(ext_addr.m8, OT_EXT_ADDRESS_SIZE, ext_hex);

    char ba_hex[OT_BORDER_AGENT_ID_LENGTH * 2 + 1] = "";
    if (ba_err == OT_ERROR_NONE)
        bytes_to_hex(ba_id.mId, OT_BORDER_AGENT_ID_LENGTH, ba_hex);

    /* S1: escape network name for JSON safety */
    char name_esc[OT_NETWORK_NAME_MAX_SIZE * 6 + 1];
    webui_json_escape(name_esc, sizeof(name_esc), net_name);

    char buf[768];
    int n = snprintf(buf, sizeof(buf),
        "{"
        "\"State\":\"%s\","
        "\"NetworkName\":\"%s\","
        "\"ExtPanId\":\"%s\","
        "\"Rloc16\":%u,"
        "\"Rloc\":\"%s\","
        "\"ExtAddress\":\"%s\","
        "\"BorderAgentId\":\"%s\","
        "\"LeaderData\":{"
          "\"PartitionId\":%"PRIu32","
          "\"Weighting\":%u,"
          "\"DataVersion\":%u,"
          "\"StableDataVersion\":%u,"
          "\"LeaderRouterId\":%u},"
        "\"NumOfRouter\":%u"
        "}",
        role_str(role), name_esc, xpanid_hex, rloc16, rloc_str,
        ext_hex, ba_hex,
        ld_err == OT_ERROR_NONE ? leader.mPartitionId : 0,
        ld_err == OT_ERROR_NONE ? leader.mWeighting : 0,
        ld_err == OT_ERROR_NONE ? leader.mDataVersion : 0,
        ld_err == OT_ERROR_NONE ? leader.mStableDataVersion : 0,
        ld_err == OT_ERROR_NONE ? leader.mLeaderRouterId : 0,
        num_routers);

    if (n >= (int)sizeof(buf))
        return send_error(req, 500, "json_overflow");
    return send_json_response(req, buf);
}

/* ------------------------------------------------------------------ */
/*  DELETE /node — Thread factory reset                               */
/* ------------------------------------------------------------------ */

static esp_err_t handle_node_delete(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    OTBR_AUTH_REQUIRED(req);
    /* Return 405 so python-otbr-api falls back to delete_active_dataset.
       otInstanceFactoryReset() reboots the chip which breaks HA's multi-step
       reset flow (set_enabled→factory_reset→create_dataset→set_enabled). */
    httpd_resp_set_status(req, "405 Method Not Allowed");
    return httpd_resp_sendstr(req, "");
}

/* ------------------------------------------------------------------ */
/*  GET /node/ba-id                                                   */
/* ------------------------------------------------------------------ */

static esp_err_t handle_ba_id(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    OTBR_AUTH_REQUIRED(req);
    otInstance *inst = require_ot(req);
    if (!inst) return ESP_OK;
    if (!ot_lock(req)) return ESP_OK;

    otBorderAgentId ba;
    otError err = otBorderAgentGetId(inst, &ba);
    ot_unlock();

    if (err != OT_ERROR_NONE)
        return send_error(req, 500, "ba_id_failed");

    char hex[OT_BORDER_AGENT_ID_LENGTH * 2 + 1];
    bytes_to_hex(ba.mId, OT_BORDER_AGENT_ID_LENGTH, hex);

    /* python-otbr-api: bytes.fromhex(await response.json()) — expects JSON string */
    char buf[OT_BORDER_AGENT_ID_LENGTH * 2 + 3];
    snprintf(buf, sizeof(buf), "\"%s\"", hex);
    return send_json_response(req, buf);
}

/* ------------------------------------------------------------------ */
/*  GET /node/rloc                                                    */
/* ------------------------------------------------------------------ */

static esp_err_t handle_rloc(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    OTBR_AUTH_REQUIRED(req);
    otInstance *inst = require_ot(req);
    if (!inst) return ESP_OK;
    if (!ot_lock(req)) return ESP_OK;

    otIp6Address addr = {{{0}}};
    const otIp6Address *ptr = otThreadGetRloc(inst);
    if (ptr) memcpy(&addr, ptr, sizeof(addr));
    ot_unlock();

    char str[OT_IP6_ADDRESS_STRING_SIZE];
    otIp6AddressToString(&addr, str, sizeof(str));

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, str);
}

/* ------------------------------------------------------------------ */
/*  GET /node/coprocessor/version     (T3.5)                          */
/* ------------------------------------------------------------------ */
/* python-otbr-api 有这条，HA 的 OTBR 集成用它显示"协处理器版本"。
   本产品的 802.15.4 射频在 **C5 片内**（Thread 走 C5，Zigbee 才走 MG24），
   所以这里报的是 C5 的 radio 版本串，与 /api/v1/status 的 ncp_version
   （MG24 的 EmberZNet 版本）是**两颗芯片的两件事**，别混。 */
static esp_err_t handle_coprocessor_version(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    OTBR_AUTH_REQUIRED(req);
    otInstance *inst = require_ot(req);
    if (!inst) return ESP_OK;
    if (!ot_lock(req)) return ESP_OK;

    const char *ver = otPlatRadioGetVersionString(inst);
    char buf[128];
    strlcpy(buf, ver ? ver : "unknown", sizeof(buf));
    ot_unlock();

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, buf);
}

/* ------------------------------------------------------------------ */
/*  GET /node/rloc16                                                  */
/* ------------------------------------------------------------------ */

static esp_err_t handle_rloc16(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    OTBR_AUTH_REQUIRED(req);
    otInstance *inst = require_ot(req);
    if (!inst) return ESP_OK;
    if (!ot_lock(req)) return ESP_OK;

    uint16_t val = otThreadGetRloc16(inst);
    ot_unlock();

    char buf[16];
    snprintf(buf, sizeof(buf), "0x%04X", val);

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, buf);
}

/* ------------------------------------------------------------------ */
/*  GET /node/ext-address                                             */
/* ------------------------------------------------------------------ */

static esp_err_t handle_ext_address(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    OTBR_AUTH_REQUIRED(req);
    otInstance *inst = require_ot(req);
    if (!inst) return ESP_OK;
    if (!ot_lock(req)) return ESP_OK;

    otExtAddress addr = {0};
    const otExtAddress *ptr = otLinkGetExtendedAddress(inst);
    if (ptr) memcpy(&addr, ptr, sizeof(addr));
    ot_unlock();

    char hex[OT_EXT_ADDRESS_SIZE * 2 + 1];
    bytes_to_hex(addr.m8, OT_EXT_ADDRESS_SIZE, hex);

    /* python-otbr-api: bytes.fromhex(await response.json()) — expects JSON string */
    char buf[OT_EXT_ADDRESS_SIZE * 2 + 3];
    snprintf(buf, sizeof(buf), "\"%s\"", hex);
    return send_json_response(req, buf);
}

/* ------------------------------------------------------------------ */
/*  GET /node/state                                                   */
/* ------------------------------------------------------------------ */

static esp_err_t handle_state_get(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    OTBR_AUTH_REQUIRED(req);
    otInstance *inst = require_ot(req);
    if (!inst) return ESP_OK;
    if (!ot_lock(req)) return ESP_OK;

    otDeviceRole role = otThreadGetDeviceRole(inst);
    ot_unlock();

    /* ot-br-posix returns role string as plain text */
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, role_str(role));
}

/* ------------------------------------------------------------------ */
/*  PUT /node/state — "enable" / "disable"                            */
/* ------------------------------------------------------------------ */

static esp_err_t handle_state_put(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    OTBR_AUTH_REQUIRED(req);
    otInstance *inst = require_ot(req);
    if (!inst) return ESP_OK;

    char body[32];
    int len = recv_body(req, body, sizeof(body));
    if (len < 0) return ESP_OK;

    /* Trim JSON quotes: "enable" → enable */
    char *val = body;
    size_t vlen = strlen(val);
    if (vlen >= 2 && val[0] == '"' && val[vlen - 1] == '"') {
        val[vlen - 1] = '\0';
        val++;
    }

    bool enable;
    if (strcmp(val, "enable") == 0)       enable = true;
    else if (strcmp(val, "disable") == 0) enable = false;
    else return send_error(req, 400, "invalid_state");

    if (!ot_lock(req)) return ESP_OK;

    otError err;
    if (enable) {
        /* Enable IPv6 first, then Thread */
        err = otIp6SetEnabled(inst, true);
        if (err == OT_ERROR_NONE) {
            otError t_err = otThreadSetEnabled(inst, true);
            if (t_err != OT_ERROR_NONE) {
                otIp6SetEnabled(inst, false); /* best-effort rollback */
                err = t_err;
            }
        }
    } else {
        /* Disable Thread first, then IPv6 */
        err = otThreadSetEnabled(inst, false);
        if (err == OT_ERROR_NONE)
            err = otIp6SetEnabled(inst, false);
    }
    ot_unlock();

    if (err != OT_ERROR_NONE)
        return send_error(req, 500, "state_change_failed");

    /* ── 落盘（2026-09-11 T3.3）──
     *
     * 此前这里**只改运行时状态**：用户点 Disable，协议栈确实停了，但配置没存。
     * 下次重启 thread_br_init() 照着 cfg->enabled=true 又把它拉起来 ——
     * 用户以为关掉了，重启后自己回来了，而且没有任何提示。
     *
     * 两个分支都要落：只落 disable 的话，用户再点 Enable 又会变成"重启后消失"，
     * 只是方向反过来，同样难查。
     * 落盘失败不回滚运行时状态：协议栈已经停/起了，回滚会让运行态与回包不符；
     * 报 500 让调用方知道"这次改动活不过重启"更诚实。 */
    thread_config_t tc = *config_mgr_get_thread();
    if (tc.enabled != enable) {
        tc.enabled = enable;
        esp_err_t save = config_mgr_set_thread(&tc);
        if (save != ESP_OK) {
            ESP_LOGE(TAG, "Thread %s 已生效但未能落盘 —— 重启后会回到原状态",
                     enable ? "enable" : "disable");
            return send_error(req, 500, "state_applied_but_not_saved");
        }
    }

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "");
}

/* ------------------------------------------------------------------ */
/*  GET /node/network-name                                            */
/* ------------------------------------------------------------------ */

static esp_err_t handle_network_name(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    OTBR_AUTH_REQUIRED(req);
    otInstance *inst = require_ot(req);
    if (!inst) return ESP_OK;
    if (!ot_lock(req)) return ESP_OK;

    char name[OT_NETWORK_NAME_MAX_SIZE + 1];
    const char *nn = otThreadGetNetworkName(inst);
    strlcpy(name, nn ? nn : "", sizeof(name));
    ot_unlock();

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, name);
}

/* ------------------------------------------------------------------ */
/*  GET /node/leader-data                                             */
/* ------------------------------------------------------------------ */

static esp_err_t handle_leader_data(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    OTBR_AUTH_REQUIRED(req);
    otInstance *inst = require_ot(req);
    if (!inst) return ESP_OK;
    if (!ot_lock(req)) return ESP_OK;

    otLeaderData ld;
    otError err = otThreadGetLeaderData(inst, &ld);
    ot_unlock();

    if (err != OT_ERROR_NONE)
        return send_error(req, 500, "leader_data_failed");

    char buf[192];
    snprintf(buf, sizeof(buf),
        "{\"PartitionId\":%"PRIu32","
        "\"Weighting\":%u,"
        "\"DataVersion\":%u,"
        "\"StableDataVersion\":%u,"
        "\"LeaderRouterId\":%u}",
        ld.mPartitionId, ld.mWeighting,
        ld.mDataVersion, ld.mStableDataVersion,
        ld.mLeaderRouterId);
    return send_json_response(req, buf);
}

/* ------------------------------------------------------------------ */
/*  GET /node/ext-panid                                               */
/* ------------------------------------------------------------------ */

static esp_err_t handle_ext_panid(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    OTBR_AUTH_REQUIRED(req);
    otInstance *inst = require_ot(req);
    if (!inst) return ESP_OK;
    if (!ot_lock(req)) return ESP_OK;

    otExtendedPanId xp = {0};
    const otExtendedPanId *ptr = otThreadGetExtendedPanId(inst);
    if (ptr) memcpy(&xp, ptr, sizeof(xp));
    ot_unlock();

    char hex[OT_EXT_PAN_ID_SIZE * 2 + 1];
    bytes_to_hex(xp.m8, OT_EXT_PAN_ID_SIZE, hex);

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, hex);
}

/* ------------------------------------------------------------------ */
/*  GET /node/num-of-router                                           */
/* ------------------------------------------------------------------ */

static esp_err_t handle_num_of_router(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    OTBR_AUTH_REQUIRED(req);
    otInstance *inst = require_ot(req);
    if (!inst) return ESP_OK;
    if (!ot_lock(req)) return ESP_OK;

    /* 曾经这里返回 otThreadGetMaxAllowedChildren() —— 那是"最多允许几个**子设备**"
       （当前 CONFIG_OPENTHREAD_MLE_MAX_CHILDREN=64），语义完全错配，实测恒为 64。
       同一个 bug 在 handle_node 里已经修过（见那里的长注释），这份漏了。
       HA 的 otbr 集成也读这个字段。逻辑与 handle_node 保持一致：
       otThreadGetMaxRouterId() 返回最大 router id（含），故循环用 <=；
       otThreadGetRouterInfo() 对不存在的 id 返回 OT_ERROR_NOT_FOUND。 */
    uint16_t n = 0;
    uint8_t max_router_id = otThreadGetMaxRouterId(inst);
    for (uint16_t rid = 0; rid <= max_router_id; rid++) {
        otRouterInfo ri;
        if (otThreadGetRouterInfo(inst, rid, &ri) == OT_ERROR_NONE) n++;
    }
    ot_unlock();

    char buf[16];
    snprintf(buf, sizeof(buf), "%u", n);

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, buf);
}

/* ------------------------------------------------------------------ */
/*  Dataset helpers                                                   */
/* ------------------------------------------------------------------ */

/* Check if client prefers text/plain (used by get_active_dataset_tlvs) */
static bool accepts_text_plain(httpd_req_t *req)
{
    char accept[128] = "";
    httpd_req_get_hdr_value_str(req, "Accept", accept, sizeof(accept));
    return strstr(accept, "text/plain") != NULL;
}

/* Return dataset as hex TLV (text/plain) */
static esp_err_t send_dataset_tlvs(httpd_req_t *req,
    otError (*getter)(otInstance *, otOperationalDatasetTlvs *),
    const char *not_found_msg)
{
    otInstance *inst = require_ot(req);
    if (!inst) return ESP_OK;
    if (!ot_lock(req)) return ESP_OK;

    otOperationalDatasetTlvs ds;
    otError err = getter(inst, &ds);
    ot_unlock();

    if (err != OT_ERROR_NONE || ds.mLength == 0)
        return send_error(req, 404, not_found_msg);

    char hex[OT_OPERATIONAL_DATASET_MAX_LENGTH * 2 + 1];
    bytes_to_hex(ds.mTlvs, ds.mLength, hex);

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, hex);
}

/* Return dataset as parsed JSON (application/json) */
static esp_err_t send_dataset_json(httpd_req_t *req,
    otError (*getter)(otInstance *, otOperationalDataset *),
    const char *not_found_msg)
{
    otInstance *inst = require_ot(req);
    if (!inst) return ESP_OK;
    if (!ot_lock(req)) return ESP_OK;

    otOperationalDataset ds;
    otError err = getter(inst, &ds);
    ot_unlock();

    if (err != OT_ERROR_NONE)
        return send_error(req, 404, not_found_msg);

    const otOperationalDatasetComponents *c = &ds.mComponents;
    char buf[1024];
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n, "{");

    if (c->mIsActiveTimestampPresent)
        n += snprintf(buf + n, sizeof(buf) - n,
            "\"ActiveTimestamp\":{\"Seconds\":%llu,\"Ticks\":%u,\"Authoritative\":%s},",
            (unsigned long long)ds.mActiveTimestamp.mSeconds,
            ds.mActiveTimestamp.mTicks,
            ds.mActiveTimestamp.mAuthoritative ? "true" : "false");

    if (c->mIsChannelPresent)
        n += snprintf(buf + n, sizeof(buf) - n, "\"Channel\":%u,", ds.mChannel);

    if (c->mIsChannelMaskPresent)
        n += snprintf(buf + n, sizeof(buf) - n, "\"ChannelMask\":%lu,",
                      (unsigned long)ds.mChannelMask);

    if (c->mIsExtendedPanIdPresent) {
        char hex[OT_EXT_PAN_ID_SIZE * 2 + 1];
        bytes_to_hex(ds.mExtendedPanId.m8, OT_EXT_PAN_ID_SIZE, hex);
        n += snprintf(buf + n, sizeof(buf) - n, "\"ExtPanId\":\"%s\",", hex);
    }

    if (c->mIsMeshLocalPrefixPresent) {
        char pfx[OT_IP6_ADDRESS_STRING_SIZE];
        otIp6AddressToString((const otIp6Address *)&ds.mMeshLocalPrefix, pfx, sizeof(pfx));
        n += snprintf(buf + n, sizeof(buf) - n, "\"MeshLocalPrefix\":\"%s/64\",", pfx);
    }

    if (c->mIsNetworkKeyPresent) {
        char hex[OT_NETWORK_KEY_SIZE * 2 + 1];
        bytes_to_hex(ds.mNetworkKey.m8, OT_NETWORK_KEY_SIZE, hex);
        n += snprintf(buf + n, sizeof(buf) - n, "\"NetworkKey\":\"%s\",", hex);
    }

    if (c->mIsNetworkNamePresent) {
        char esc[OT_NETWORK_NAME_MAX_SIZE * 6 + 1];
        webui_json_escape(esc, sizeof(esc), ds.mNetworkName.m8);
        n += snprintf(buf + n, sizeof(buf) - n, "\"NetworkName\":\"%s\",", esc);
    }

    if (c->mIsPanIdPresent)
        n += snprintf(buf + n, sizeof(buf) - n, "\"PanId\":%u,", ds.mPanId);

    if (c->mIsPskcPresent) {
        char hex[OT_PSKC_MAX_SIZE * 2 + 1];
        bytes_to_hex(ds.mPskc.m8, OT_PSKC_MAX_SIZE, hex);
        n += snprintf(buf + n, sizeof(buf) - n, "\"PSKc\":\"%s\",", hex);
    }

    if (c->mIsSecurityPolicyPresent)
        n += snprintf(buf + n, sizeof(buf) - n,
            "\"SecurityPolicy\":{\"RotationTime\":%u,\"ObtainNetworkKey\":%s,"
            "\"NativeCommissioning\":%s,\"Routers\":%s,"
            "\"ExternalCommissioning\":%s,\"CommercialCommissioning\":%s,"
            "\"AutonomousEnrollment\":%s,\"NetworkKeyProvisioning\":%s},",
            ds.mSecurityPolicy.mRotationTime,
            ds.mSecurityPolicy.mObtainNetworkKeyEnabled ? "true" : "false",
            ds.mSecurityPolicy.mNativeCommissioningEnabled ? "true" : "false",
            ds.mSecurityPolicy.mRoutersEnabled ? "true" : "false",
            ds.mSecurityPolicy.mExternalCommissioningEnabled ? "true" : "false",
            ds.mSecurityPolicy.mCommercialCommissioningEnabled ? "true" : "false",
            ds.mSecurityPolicy.mAutonomousEnrollmentEnabled ? "true" : "false",
            ds.mSecurityPolicy.mNetworkKeyProvisioningEnabled ? "true" : "false");

    /* Remove trailing comma, close brace */
    if (n > 1 && buf[n - 1] == ',') n--;
    n += snprintf(buf + n, sizeof(buf) - n, "}");

    if (n >= (int)sizeof(buf))
        return send_error(req, 500, "json_overflow");
    return send_json_response(req, buf);
}

/*
 * PUT /node/dataset/active — two formats:
 *  text/plain (hex TLV) from set_active_dataset_tlvs
 *  application/json from create_active_dataset (partial, auto-fills missing)
 * HA flow: Thread is already disabled when this is called.
 */
static esp_err_t handle_dataset_put(httpd_req_t *req, bool pending)
{
    otInstance *inst = require_ot(req);
    if (!inst) return ESP_OK;

    char body[OT_OPERATIONAL_DATASET_MAX_LENGTH * 2 + 64];
    int len = recv_body(req, body, sizeof(body));
    if (len < 0) return ESP_OK;

    while (len > 0 && (body[len - 1] == '\n' || body[len - 1] == '\r' ||
                       body[len - 1] == ' '  || body[len - 1] == '\t'))
        body[--len] = '\0';

    if (!ot_lock(req)) return ESP_OK;

    /* ot-br-posix semantics: replacing the ACTIVE dataset requires Thread to
       be disabled first (409 otherwise). Pending datasets are legitimately
       set while running — that is how channel migration works. */
    if (!pending && otThreadGetDeviceRole(inst) > OT_DEVICE_ROLE_DISABLED) {
        ot_unlock();
        return send_error(req, 409, "thread_network_active");
    }

    otError ot_err;

    if (body[0] == '{') {
        /* JSON body: create dataset with auto-filled defaults + overrides.
         *
         * ⚠️ 这条路径**建的是一个全新网络，不能用来做信道迁移**。
         * otDatasetCreateNewNetwork() 会重新生成 NetworkKey / PanId /
         * ExtPanId / PSKc，下面只覆盖 Channel / PanId / NetworkName 三项 ——
         * 结果是换了张网，已入网的设备全部被甩掉，且 HA 存的凭据全部作废。
         *
         * 要改信道（哪怕只改一个字节）必须走下面的 TLV 分支：读回当前
         * active dataset 的 TLV，只替换 Channel TLV、递增 ActiveTimestamp、
         * 补 PendingTimestamp + DelayTimer，再 PUT 到 /node/dataset/pending。
         * 2026-08-12 在板上按这个做法迁移 25→15→25 验证过：网络身份逐字节不变，
         * 子节点全程 1/1 没掉，延时到点后全网一起切换。
         *
         * HA 的 Thread 集成用的是 TLV 路径，所以它没踩到这个坑；将来我们自己
         * 实现"迁移按钮"时别图省事发 JSON。
         */
        otOperationalDataset ds;
        if (otDatasetCreateNewNetwork(inst, &ds) != OT_ERROR_NONE) {
            ot_unlock();
            return send_error(req, 500, "dataset_create_failed");
        }

        const char *p;
        if ((p = strstr(body, "\"Channel\":")) != NULL) {
            int ch = 0; sscanf(p + 10, "%d", &ch);
            if (ch >= 11 && ch <= 26) {
                ds.mChannel = ch;
                ds.mComponents.mIsChannelPresent = true;
            }
        }
        if ((p = strstr(body, "\"PanId\":")) != NULL) {
            unsigned pan = 0; sscanf(p + 8, "%u", &pan);
            ds.mPanId = (uint16_t)pan;
            ds.mComponents.mIsPanIdPresent = true;
        }
        if ((p = strstr(body, "\"NetworkName\":\"")) != NULL) {
            p += 15;
            char name[OT_NETWORK_NAME_MAX_SIZE + 1] = {0};
            for (int i = 0; p[i] && p[i] != '"' && i < OT_NETWORK_NAME_MAX_SIZE; i++)
                name[i] = p[i];
            strlcpy(ds.mNetworkName.m8, name, sizeof(ds.mNetworkName.m8));
            ds.mComponents.mIsNetworkNamePresent = true;
        }
        ot_err = pending ? otDatasetSetPending(inst, &ds)
                         : otDatasetSetActive(inst, &ds);
    } else {
        /* Hex TLV body */
        otOperationalDatasetTlvs ds;
        int blen = hex_to_bytes(body, ds.mTlvs, OT_OPERATIONAL_DATASET_MAX_LENGTH);
        if (blen < 0) { ot_unlock(); return send_error(req, 400, "invalid_dataset"); }
        ds.mLength = (uint8_t)blen;
        ot_err = pending ? otDatasetSetPendingTlvs(inst, &ds)
                         : otDatasetSetActiveTlvs(inst, &ds);
    }

    /* 共存检查：HA 的 Thread 集成会把别的 BR（Apple TV / Nest）的首选数据集
       原样推过来，那个信道是在完全不知道本机有 Zigbee 的情况下选的。
       **只告警不拒绝** —— 回 4xx 会破坏 ot-br-posix 的契约和 HA 的数据集采纳
       流程，为了躲一次可能的干扰去弄坏一条必然要走的路，代价不对等。 */
    if (ot_err == OT_ERROR_NONE) {
        otOperationalDataset now;
        otError ge = pending ? otDatasetGetPending(inst, &now)
                             : otDatasetGetActive(inst, &now);
        uint8_t zb_ch = 0;
        if (ge == OT_ERROR_NONE && now.mComponents.mIsChannelPresent &&
            thread_br_channel_too_close(now.mChannel, &zb_ch)) {
            ESP_LOGW(TAG, "推入的%s数据集信道 %u 与 Zigbee 的 %u 相距 %d "
                          "(≤%d 视为太近) — 已接受，但两者会互相压制",
                     pending ? "待定" : "活动", now.mChannel, zb_ch,
                     now.mChannel > zb_ch ? now.mChannel - zb_ch : zb_ch - now.mChannel,
                     COEX_TOO_CLOSE_MAX);
        }
    }

    /* 不用自己更新 mDNS：活动数据集一变，BR 库就重发 _meshcop._udp（xa/nn/xp 都是新值，见 mdns_svc.h）。
       以前这里再写一遍，只是重复库的值，而且扩展地址在重新入网后可能还会变，读到的反而可能是旧的。 */
    ot_unlock();

    if (ot_err != OT_ERROR_NONE) {
        char msg[48];
        snprintf(msg, sizeof(msg), "dataset_set_failed_%d", ot_err);
        return send_error(req, 500, msg);
    }

    httpd_resp_set_status(req, "201 Created");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "");
}

static esp_err_t handle_dataset_delete(httpd_req_t *req, bool pending)
{
    otInstance *inst = require_ot(req);
    if (!inst) return ESP_OK;
    if (!ot_lock(req)) return ESP_OK;

    /* Active dataset: Thread must be disabled first (HA calls
       set_enabled(false) before this). Deleting a PENDING dataset while
       running is fine — that cancels a scheduled migration. */
    if (!pending) {
        otDeviceRole role = otThreadGetDeviceRole(inst);
        if (role > OT_DEVICE_ROLE_DISABLED) {
            ot_unlock();
            return send_error(req, 409, "thread_network_active");
        }
    }

    /* Erase dataset by setting an empty one */
    otOperationalDatasetTlvs empty = { .mLength = 0 };
    if (pending) otDatasetSetPendingTlvs(inst, &empty);
    else         otDatasetSetActiveTlvs(inst, &empty);
    ot_unlock();

    return send_json_response(req, "{\"status\":\"ok\"}");
}

/* ---- GET/PUT/DELETE /node/dataset/active ---- */

static esp_err_t handle_dataset_active_get(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    OTBR_AUTH_REQUIRED(req);
    /* python-otbr-api: get_active_dataset_tlvs() sends Accept: text/plain
       python-otbr-api: get_active_dataset() sends default Accept → JSON */
    if (accepts_text_plain(req))
        return send_dataset_tlvs(req, otDatasetGetActiveTlvs, "no_active_dataset");
    return send_dataset_json(req, otDatasetGetActive, "no_active_dataset");
}

static esp_err_t handle_dataset_active_put(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    OTBR_AUTH_REQUIRED(req);
    return handle_dataset_put(req, false);
}

static esp_err_t handle_dataset_active_delete(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    OTBR_AUTH_REQUIRED(req);
    return handle_dataset_delete(req, false);
}

/* ---- GET/PUT/DELETE /node/dataset/pending ---- */

static esp_err_t handle_dataset_pending_get(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    OTBR_AUTH_REQUIRED(req);
    if (accepts_text_plain(req))
        return send_dataset_tlvs(req, otDatasetGetPendingTlvs, "no_pending_dataset");
    return send_dataset_json(req, otDatasetGetPending, "no_pending_dataset");
}

static esp_err_t handle_dataset_pending_put(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    OTBR_AUTH_REQUIRED(req);
    return handle_dataset_put(req, true);
}

static esp_err_t handle_dataset_pending_delete(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    OTBR_AUTH_REQUIRED(req);
    return handle_dataset_delete(req, true);
}

/* ------------------------------------------------------------------ */
/*  GET /diagnostics                                                  */
/* ------------------------------------------------------------------ */

static esp_err_t handle_diagnostics(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    OTBR_AUTH_REQUIRED(req);
    otInstance *inst = require_ot(req);
    if (!inst) return ESP_OK;
    if (!ot_lock(req)) return ESP_OK;

    otDeviceRole role = otThreadGetDeviceRole(inst);

    char net_name[OT_NETWORK_NAME_MAX_SIZE + 1];
    const char *nn = otThreadGetNetworkName(inst);
    strlcpy(net_name, nn ? nn : "", sizeof(net_name));

    uint16_t rloc16 = otThreadGetRloc16(inst);

    otExtendedPanId xp = {0};
    const otExtendedPanId *xp_ptr = otThreadGetExtendedPanId(inst);
    if (xp_ptr) memcpy(&xp, xp_ptr, sizeof(xp));

    /* 实际运行信道。以前这里报的是 thread_br_get_channel()（= NVS 配置值），
       HA 或别的 BR 推过数据集做完信道迁移之后就一直是错的 —— 同一个页面上
       dataset 表格（读 otDatasetGetActive）显示新信道，这里显示旧配置值。
       在已经持有的锁里顺手读掉，不额外取锁、不需要回退路径：
       require_ot() 已经保证 OT 在跑。 */
    uint8_t channel = otLinkGetChannel(inst);

    ot_unlock();

    char xp_hex[OT_EXT_PAN_ID_SIZE * 2 + 1];
    bytes_to_hex(xp.m8, OT_EXT_PAN_ID_SIZE, xp_hex);

    /* S1: escape network name for JSON safety */
    char name_esc[OT_NETWORK_NAME_MAX_SIZE * 6 + 1];
    webui_json_escape(name_esc, sizeof(name_esc), net_name);

    char buf[320];
    snprintf(buf, sizeof(buf),
        "[{\"Type\":\"border_router\","
        "\"State\":\"%s\","
        "\"Rloc16\":%u,"
        "\"NetworkName\":\"%s\","
        "\"ExtPanId\":\"%s\","
        "\"Channel\":%u}]",
        role_str(role), rloc16, name_esc, xp_hex, channel);
    return send_json_response(req, buf);
}

/* ------------------------------------------------------------------ */
/*  GET /api/v1/diag/thread — 发送队列 / 消息缓冲快照（只读）          */
/* ------------------------------------------------------------------ */
/*
 * 为什么存在：2026-08-09 反复出现"Thread 整体死掉"——MAC 计数器长时间纹丝不动、
 * MLE 仍在排队（串口有 Send Advertisement）但一帧都发不出去、随后刷
 * `MeshForwarder: Dropping (dir queue full)`，只有整板重启能恢复。
 * 时间线显示休眠子节点先超时被摘，之后 matter-server 仍持续往它地址发包，
 * 推断是这些投递不出去的报文堵死了发送队列（队首阻塞）。该推断缺实测——
 * 本端点把队列深度和缓冲占用直接吐出来，卡死时一个请求即可证实或推翻它。
 *
 * 判读：`queues.6lo_send.msgs` 持续高位不降 + `buffers.free` 触底
 *       = 队列堵死坐实；若队列是空的，则说明卡在更下层（射频/驱动）。
 *
 * H1 verified（openthread/message.h:425-517）：
 *   - otMessageGetBufferInfo(inst, &info) 无返回值，须持 OT 锁
 *   - mTotalBuffers / mFreeBuffers 为 0xffff 时表示"未知"，不是真实计数
 *   - m6loSendQueue 才是 MeshForwarder 的发送队列，本故障要看的就是它
 *   - otMessageQueueInfo: mNumMessages/mNumBuffers 是 uint16，mTotalBytes 是 uint32
 */
static esp_err_t handle_diag_thread(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    otInstance *inst = require_ot(req);
    if (!inst) return ESP_OK;
    if (!ot_lock(req)) return ESP_OK;

    otBufferInfo bi;
    otMessageGetBufferInfo(inst, &bi);
    /* 计数器也一并带出：判断"队列在涨而射频不动"只需要这一个请求 */
    otMacCounters mac = *otLinkGetCounters(inst);
    otDeviceRole role = otThreadGetDeviceRole(inst);

    /* 子节点数与其待发消息数——休眠子节点掉线后这里会堆积 */
    uint16_t max_children = otThreadGetMaxAllowedChildren(inst);
    unsigned nchild = 0, child_queued = 0;
    for (uint16_t i = 0; i < max_children; i++) {
        otChildInfo ci;
        if (otThreadGetChildInfoByIndex(inst, i, &ci) != OT_ERROR_NONE) continue;
        nchild++;
        child_queued += ci.mQueuedMessageCnt;
    }
    ot_unlock();

    /* 单次 snprintf + 一次越界检查，避免分段拼接时的边界错误 */
    char buf[900];
    int n = snprintf(buf, sizeof(buf),
        "{\"role\":\"%s\",\"children\":%u,\"child_queued_msgs\":%u,"
        "\"tx_total\":%lu,\"rx_total\":%lu,"
        "\"buffers\":{\"total\":%u,\"free\":%u,\"max_used\":%u},"
        "\"queues\":{"
          "\"6lo_send\":{\"msgs\":%u,\"bufs\":%u,\"bytes\":%lu},"
          "\"6lo_reass\":{\"msgs\":%u,\"bufs\":%u,\"bytes\":%lu},"
          "\"ip6\":{\"msgs\":%u,\"bufs\":%u,\"bytes\":%lu},"
          "\"mpl\":{\"msgs\":%u,\"bufs\":%u,\"bytes\":%lu},"
          "\"mle\":{\"msgs\":%u,\"bufs\":%u,\"bytes\":%lu},"
          "\"coap\":{\"msgs\":%u,\"bufs\":%u,\"bytes\":%lu},"
          "\"coap_secure\":{\"msgs\":%u,\"bufs\":%u,\"bytes\":%lu},"
          "\"app_coap\":{\"msgs\":%u,\"bufs\":%u,\"bytes\":%lu}}}",
        role_str(role), nchild, child_queued,
        (unsigned long)mac.mTxTotal, (unsigned long)mac.mRxTotal,
        bi.mTotalBuffers, bi.mFreeBuffers, bi.mMaxUsedBuffers,
        bi.m6loSendQueue.mNumMessages,       bi.m6loSendQueue.mNumBuffers,       (unsigned long)bi.m6loSendQueue.mTotalBytes,
        bi.m6loReassemblyQueue.mNumMessages, bi.m6loReassemblyQueue.mNumBuffers, (unsigned long)bi.m6loReassemblyQueue.mTotalBytes,
        bi.mIp6Queue.mNumMessages,           bi.mIp6Queue.mNumBuffers,           (unsigned long)bi.mIp6Queue.mTotalBytes,
        bi.mMplQueue.mNumMessages,           bi.mMplQueue.mNumBuffers,           (unsigned long)bi.mMplQueue.mTotalBytes,
        bi.mMleQueue.mNumMessages,           bi.mMleQueue.mNumBuffers,           (unsigned long)bi.mMleQueue.mTotalBytes,
        bi.mCoapQueue.mNumMessages,          bi.mCoapQueue.mNumBuffers,          (unsigned long)bi.mCoapQueue.mTotalBytes,
        bi.mCoapSecureQueue.mNumMessages,    bi.mCoapSecureQueue.mNumBuffers,    (unsigned long)bi.mCoapSecureQueue.mTotalBytes,
        bi.mApplicationCoapQueue.mNumMessages, bi.mApplicationCoapQueue.mNumBuffers, (unsigned long)bi.mApplicationCoapQueue.mTotalBytes);

    if (n < 0 || n >= (int)sizeof(buf)) return send_error(req, 500, "json_overflow");
    return send_json_response(req, buf);
}

/*
 * ⚠️ 曾在此实现 POST /api/v1/thread/restart（thread_br_stop() + start()），
 * 想做成"不整板重启就能救活卡死的 Thread"。**实测不可行，已移除，别再加回来**
 * ——除非先修好下面这两个缺陷（2026-08-09 在真板上验证）：
 *
 *  1. thread_br_stop()/start() 无法往返。stop 之后 start 报
 *       E OPENTHREAD: esp_openthread_platform_init(142): OpenThread platform already initialized
 *       E OPENTHREAD: esp_openthread_init(80): Failed to initialize OpenThread platform driver
 *     即 esp_openthread_platform_deinit() 没跑到（它第一件事就是把
 *     s_openthread_platform_initialized 置 false，既然再 init 时仍为 true，
 *     说明 esp_openthread_deinit() 这条路没走通）。
 *
 *  2. 失败后 thread_br_stop() 变成空转：它开头就是 `if (!s_running) return ESP_OK;`，
 *     而首次失败已把 s_running 置 false，于是 platform 再也清不掉——
 *     Thread 从此关闭，只有整板重启能恢复。
 *
 *  3. thread_br_start() 这种情况下仍返回 ESP_OK（只打 "Thread BR degraded" 警告），
 *     调用方看到的是"成功"，掩盖了失败。
 *
 * 也就是说这个端点不是救场手段，而是把 Thread 打死的陷阱。
 * 要重做，先让 stop/start 真正往返并让 start 如实返回错误。
 *
 * ⚠️ 2026-10-05 复核：上面第 1 条只在那天"能量扫描后 MAC 永久卡死"的异常态下试过一次
 * （a69b21a 修的就是那个卡死）；按 IDF v6.0.2 源码，esp_openthread_platform_deinit() 第一步就
 * 清掉 initialized 标志，正常态应能往返 —— 但**正常态尚未复测**（实验 2 待做），结论悬而未决。
 *
 * ⚠️ 2026-10-07 实验 2（T9.18）：**正常态同样不可用**。诊断固件 + 故障注入 {"kind":"l3","target":"thread"}
 * （经看门狗恢复引擎、在 wd_action 任务里跑真实 L3），两台 Matter 设备从 HA 主机 ping 判活：
 *  a. 第一次 L3：新主循环第一圈就退出，Thread 永久停，start 照样返回 ESP_OK、日志照打 "fully started"。
 *     根因是 IDF 注销不掉 "dns_upstream_query" 工作流（名字超长，见 thread_br.c 的 br_deinit_locked）——
 *     **已绕过**。08-09 的第 1 条很可能是同一根因：主循环先静默退出，再 start 就 "already initialized"。
 *  b. 绕过之后：两次 L3 都在 2 s 内回到工作状态、两台 Matter 8 s 内可达、任务数 29→29；
 *     **第三次 panic 整机重启**：ot_main 里 Mac::HandleTransmitDone 断言（mac.cpp:1465）。推断是上一个实例
 *     遗留的"发送完成"事件 —— esp_openthread_radio.c 的 s_txrx_events 是静态的，radio init / deinit 都不清，
 *     停的那一刻若有帧在发就会留到新实例。**未修**（在 IDF 里，本仓库够不着）。
 *  c. 每次 L3 之后都有 "BbrManager: Start Backbone TMF agent: Failed"（开机没有）。推断旧实例的骨干 TMF socket
 *     没关、61631 端口被占，可能每次漏一个 lwIP UDP PCB（上限 16）。**未取证**。
 *  另：wd_action（栈 3072）跑一次 L3 后余量只剩 920 字节。
 * 同日用户拍板（T9.18 方案 A）：Thread 不做整栈重启，L2 救不回就整机重启；thread_br_restart() 与
 * 实验用的 {"kind":"l3"} 注入都已删除（策略在 watchdog/wd_recovery.c 的 s_subsys_override）。
 * 要重做实验 2，得把这两样从 git 历史里捞回来。
 * 以太网断线换口**不走这条路**：只拆/建边界路由这一层（thread_br_switch_backbone，
 * 实测 10 次往返 + 真拔线 21 分钟通过，见 docs/test_cases_stability.md 测试 6c）。
 */

/* ---- GET /node/children ---- */
/* 分块发送而不是攒进一个 2048 的缓冲：原实现在装满时静默 break（"drop the
   truncated entry, keep valid JSON"），**没有任何截断标记**，约 10 个子节点就
   只看得到前面那些 —— 而"到底有几个设备在线"正是扩容实验的地面真值。
   快照与发送必须分开：otChildInfo 在持 OT 锁时取，放锁之后再做网络 I/O，
   否则 httpd 的发送阻塞会把整个 Thread 协议栈卡住。 */
static esp_err_t handle_children(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    OTBR_AUTH_REQUIRED(req);
    otInstance *inst = require_ot(req);
    if (!inst) return ESP_OK;

    /* 上限固定（MLE_MAX_CHILDREN），一次性按上限取，落 PSRAM 不吃内部 SRAM */
    uint16_t cap = 0;
    otChildInfo *ci = NULL;

    if (!ot_lock(req)) return ESP_OK;
    cap = otThreadGetMaxAllowedChildren(inst);
    if (cap) {
        ci = heap_caps_malloc((size_t)cap * sizeof(*ci),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!ci) ci = malloc((size_t)cap * sizeof(*ci));
    }
    if (!ci) { ot_unlock(); return send_error(req, 500, "out_of_memory"); }

    uint16_t cnt = 0;
    for (uint16_t i = 0; i < cap; i++) {
        if (otThreadGetChildInfoByIndex(inst, i, &ci[cnt]) == OT_ERROR_NONE) cnt++;
    }
    ot_unlock();

    httpd_resp_set_type(req, WEBUI_JSON_CONTENT_TYPE);
    httpd_resp_send_chunk(req, "[", 1);

    char item[320];
    for (uint16_t i = 0; i < cnt; i++) {
        char ext[17];
        for (int j = 0; j < 8; j++) sprintf(ext + j * 2, "%02x", ci[i].mExtAddress.m8[j]);
        int r = snprintf(item, sizeof(item),
            "%s{\"rloc16\":\"0x%04X\",\"ext_addr\":\"%s\","
            "\"timeout\":%lu,\"age\":%lu,\"connection_time\":%llu,"
            "\"rssi_avg\":%d,\"rssi_last\":%d,\"lqi\":%u,"
            "\"rx_on_idle\":%s,\"ftd\":%s,\"child_id\":%u,"
            "\"queued_msgs\":%u,\"version\":%u}",
            i ? "," : "",
            ci[i].mRloc16, ext,
            (unsigned long)ci[i].mTimeout, (unsigned long)ci[i].mAge,
            (unsigned long long)ci[i].mConnectionTime,
            ci[i].mAverageRssi, ci[i].mLastRssi, ci[i].mLinkQualityIn,
            ci[i].mRxOnWhenIdle ? "true" : "false",
            ci[i].mFullThreadDevice ? "true" : "false",
            ci[i].mChildId, ci[i].mQueuedMessageCnt, ci[i].mVersion);
        /* item 装不下才是真异常（单条约 200 字节，320 有余量）；宁可少一条也不发半条 */
        if (r > 0 && r < (int)sizeof(item)) httpd_resp_send_chunk(req, item, r);
    }
    free(ci);

    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ---- GET /node/neighbors ---- */
/* 同 handle_children：快照 → 放锁 → 分块发。
   原实现除了 2048 截断，还有一处比 children 更危险的写法：
   `n += snprintf(...)` **不检查返回值**，而 snprintf 返回的是"本该写多少"，
   n 因此可能越过 2048，随后 buf[n++]=']' 就是堆越界写。
   handle_children 的注释里恰好写明过这个坑，这份没落实。 */
static esp_err_t handle_neighbors(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    OTBR_AUTH_REQUIRED(req);
    otInstance *inst = require_ot(req);
    if (!inst) return ESP_OK;

    /* 邻居 = 子设备 + 路由器，按各自上限之和给足；落 PSRAM */
    uint16_t cap = 0;
    otNeighborInfo *nb = NULL;

    if (!ot_lock(req)) return ESP_OK;
    cap = otThreadGetMaxAllowedChildren(inst) + 32 /* kMaxRouters */;
    nb = heap_caps_malloc((size_t)cap * sizeof(*nb), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!nb) nb = malloc((size_t)cap * sizeof(*nb));
    if (!nb) { ot_unlock(); return send_error(req, 500, "out_of_memory"); }

    uint16_t cnt = 0;
    otNeighborInfoIterator iter = OT_NEIGHBOR_INFO_ITERATOR_INIT;
    while (cnt < cap &&
           otThreadGetNextNeighborInfo(inst, &iter, &nb[cnt]) == OT_ERROR_NONE) cnt++;
    ot_unlock();

    httpd_resp_set_type(req, WEBUI_JSON_CONTENT_TYPE);
    httpd_resp_send_chunk(req, "[", 1);

    char item[320];
    for (uint16_t i = 0; i < cnt; i++) {
        char ext[17];
        for (int j = 0; j < 8; j++) sprintf(ext + j * 2, "%02x", nb[i].mExtAddress.m8[j]);
        int r = snprintf(item, sizeof(item),
            "%s{\"rloc16\":\"0x%04X\",\"ext_addr\":\"%s\","
            "\"age\":%lu,\"rssi_avg\":%d,\"rssi_last\":%d,"
            "\"lqi\":%u,\"link_margin\":%u,"
            "\"rx_on_idle\":%s,\"ftd\":%s,\"is_child\":%s,\"version\":%u}",
            i ? "," : "",
            nb[i].mRloc16, ext,
            (unsigned long)nb[i].mAge, nb[i].mAverageRssi, nb[i].mLastRssi,
            nb[i].mLinkQualityIn, nb[i].mLinkMargin,
            nb[i].mRxOnWhenIdle ? "true" : "false",
            nb[i].mFullThreadDevice ? "true" : "false",
            nb[i].mIsChild ? "true" : "false",
            nb[i].mVersion);
        if (r > 0 && r < (int)sizeof(item)) httpd_resp_send_chunk(req, item, r);
    }
    free(nb);

    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ---- GET /node/ipaddrs ---- */
static esp_err_t handle_ipaddrs(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    OTBR_AUTH_REQUIRED(req);
    otInstance *inst = require_ot(req);
    if (!inst) return ESP_OK;
    if (!ot_lock(req)) return ESP_OK;

    char *buf = malloc(1024);
    if (!buf) { ot_unlock(); return send_error(req, 500, "out_of_memory"); }
    int n = 0;
    buf[n++] = '[';

    bool first = true;
    const otNetifAddress *addr = otIp6GetUnicastAddresses(inst);
    while (addr) {
        char ip6[40];
        otIp6AddressToString(&addr->mAddress, ip6, sizeof(ip6));
        n += snprintf(buf + n, 1024 - n, "%s\"%s/%u\"",
                      first ? "" : ",", ip6, addr->mPrefixLength);
        first = false;
        addr = addr->mNext;
        if (n > 900) break;
    }
    buf[n++] = ']'; buf[n] = '\0';
    ot_unlock();
    esp_err_t ret = send_json_response(req, buf);
    free(buf);
    return ret;
}

/* ---- GET /node/border-routing ---- */
static esp_err_t handle_border_routing(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    OTBR_AUTH_REQUIRED(req);
    otInstance *inst = require_ot(req);
    if (!inst) return ESP_OK;
    if (!ot_lock(req)) return ESP_OK;

    char buf[416];
    char omr[48] = "", onlink[48] = "";

    otIp6Prefix prefix;
    if (otBorderRoutingGetOmrPrefix(inst, &prefix) == OT_ERROR_NONE) {
        char ip6[40];
        otIp6AddressToString(&prefix.mPrefix, ip6, sizeof(ip6));
        snprintf(omr, sizeof(omr), "%s/%u", ip6, prefix.mLength);
    }
    if (otBorderRoutingGetOnLinkPrefix(inst, &prefix) == OT_ERROR_NONE) {
        char ip6[40];
        otIp6AddressToString(&prefix.mPrefix, ip6, sizeof(ip6));
        snprintf(onlink, sizeof(onlink), "%s/%u", ip6, prefix.mLength);
    }

    const char *state_str = "unknown";
    otBorderRoutingState brState = otBorderRoutingGetState(inst);
    switch (brState) {
        case OT_BORDER_ROUTING_STATE_DISABLED: state_str = "disabled"; break;
        case OT_BORDER_ROUTING_STATE_STOPPED:  state_str = "stopped"; break;
        case OT_BORDER_ROUTING_STATE_RUNNING:  state_str = "running"; break;
        default: break;
    }

    ot_unlock();
    /* backbone 是本机扩展字段（ot-br-posix 没有）：stopped 时一眼看出是不是"边界路由
       还挂在已经断掉的以太网口上"（测试 6c）。HA 的 otbr 集成不读这个端点。 */
    snprintf(buf, sizeof(buf),
        "{\"state\":\"%s\",\"omr_prefix\":\"%s\",\"onlink_prefix\":\"%s\",\"backbone\":\"%s\"}",
        state_str, omr, onlink, thread_br_backbone_name());
    return send_json_response(req, buf);
}

/* ---- GET /node/counters ---- */
static esp_err_t handle_counters(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    OTBR_AUTH_REQUIRED(req);
    otInstance *inst = require_ot(req);
    if (!inst) return ESP_OK;
    if (!ot_lock(req)) return ESP_OK;

    /* Copy under the lock — the pointer targets OT-internal state that the
       OpenThread mainloop keeps mutating after we release the lock. */
    otMacCounters counters = *otLinkGetCounters(inst);
    const otMacCounters *mc = &counters;
    ot_unlock();

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"tx_total\":%lu,\"tx_unicast\":%lu,\"tx_broadcast\":%lu,"
        "\"tx_ack_req\":%lu,\"tx_acked\":%lu,"
        "\"tx_err_abort\":%lu,\"tx_err_cca\":%lu,"
        "\"rx_total\":%lu,\"rx_unicast\":%lu,\"rx_broadcast\":%lu,"
        "\"rx_err_fcs\":%lu,\"rx_err_sec\":%lu,\"rx_err_other\":%lu,"
        "\"rx_duplicated\":%lu}",
        (unsigned long)mc->mTxTotal, (unsigned long)mc->mTxUnicast,
        (unsigned long)mc->mTxBroadcast, (unsigned long)mc->mTxAckRequested,
        (unsigned long)mc->mTxAcked,
        (unsigned long)mc->mTxErrAbort, (unsigned long)mc->mTxErrBusyChannel,
        (unsigned long)mc->mRxTotal, (unsigned long)mc->mRxUnicast,
        (unsigned long)mc->mRxBroadcast,
        (unsigned long)mc->mRxErrFcs, (unsigned long)mc->mRxErrSec,
        (unsigned long)mc->mRxErrOther, (unsigned long)mc->mRxDuplicated);
    return send_json_response(req, buf);
}

/* ------------------------------------------------------------------ */
/*  Registration                                                      */
/* ------------------------------------------------------------------ */

void register_otbr_handlers(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {  /* static：同 rest_api.c 的路由表，别放栈上 */
        { .uri = "/node",                 .method = HTTP_GET,    .handler = handle_node_get },
        { .uri = "/node",                 .method = HTTP_DELETE, .handler = handle_node_delete },
        { .uri = "/node/ba-id",           .method = HTTP_GET,    .handler = handle_ba_id },
        { .uri = "/node/rloc",            .method = HTTP_GET,    .handler = handle_rloc },
        { .uri = "/node/rloc16",          .method = HTTP_GET,    .handler = handle_rloc16 },
        { .uri = "/node/coprocessor/version", .method = HTTP_GET, .handler = handle_coprocessor_version },
        { .uri = "/node/ext-address",     .method = HTTP_GET,    .handler = handle_ext_address },
        { .uri = "/node/state",           .method = HTTP_GET,    .handler = handle_state_get },
        { .uri = "/node/state",           .method = HTTP_PUT,    .handler = handle_state_put },
        { .uri = "/node/network-name",    .method = HTTP_GET,    .handler = handle_network_name },
        { .uri = "/node/leader-data",     .method = HTTP_GET,    .handler = handle_leader_data },
        { .uri = "/node/ext-panid",       .method = HTTP_GET,    .handler = handle_ext_panid },
        { .uri = "/node/num-of-router",   .method = HTTP_GET,    .handler = handle_num_of_router },
        { .uri = "/node/dataset/active",  .method = HTTP_GET,    .handler = handle_dataset_active_get },
        { .uri = "/node/dataset/active",  .method = HTTP_PUT,    .handler = handle_dataset_active_put },
        { .uri = "/node/dataset/active",  .method = HTTP_DELETE, .handler = handle_dataset_active_delete },
        { .uri = "/node/dataset/pending", .method = HTTP_GET,    .handler = handle_dataset_pending_get },
        { .uri = "/node/dataset/pending", .method = HTTP_PUT,    .handler = handle_dataset_pending_put },
        { .uri = "/node/dataset/pending", .method = HTTP_DELETE, .handler = handle_dataset_pending_delete },
        { .uri = "/diagnostics",          .method = HTTP_GET,    .handler = handle_diagnostics },
        { .uri = "/node/children",        .method = HTTP_GET,    .handler = handle_children },
        { .uri = "/node/neighbors",       .method = HTTP_GET,    .handler = handle_neighbors },
        { .uri = "/node/ipaddrs",         .method = HTTP_GET,    .handler = handle_ipaddrs },
        { .uri = "/node/border-routing",  .method = HTTP_GET,    .handler = handle_border_routing },
        { .uri = "/node/counters",        .method = HTTP_GET,    .handler = handle_counters },
        /* Thread 卡死排查：发送队列 / 消息缓冲快照（只读） */
        { .uri = "/api/v1/diag/thread",   .method = HTTP_GET,    .handler = handle_diag_thread },
    };
    int registered = 0;
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register %s: %s", uris[i].uri, esp_err_to_name(err));
        } else {
            registered++;
        }
    }
    ESP_LOGI(TAG, "OTBR REST API: %d/%d endpoints registered", registered,
             (int)(sizeof(uris) / sizeof(uris[0])));
}

#else /* CONFIG_OPENTHREAD_ENABLED not set */

void register_otbr_handlers(httpd_handle_t server)
{
    (void)server;
    /* Thread not compiled in — no OTBR endpoints */
}

#endif /* CONFIG_OPENTHREAD_ENABLED */
