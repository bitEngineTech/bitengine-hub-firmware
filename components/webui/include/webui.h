#pragma once

#include "esp_err.h"
#include "sdkconfig.h"     /* CONFIG_BITENGINE_ENABLE_ZHUB（register_smlight_handlers 只在 HA 型声明） */
#include "esp_event.h"
#include "esp_http_server.h"
#include "config_mgr.h"   /* AUTH_REQUIRED_UNLESS_OPEN 用 thread_config_t */

#ifdef __cplusplus
extern "C" {
#endif

// ---------------- Constants ----------------
#define MAX_WS_CLIENTS      4
#define MAX_SESSIONS         4
#define SESSION_TOKEN_LEN    32   // 16 bytes hex = 32 chars
#define SESSION_TIMEOUT_S    86400 // 24 hours
#define WEBUI_HTTP_PORT      80

/* 设备密码的字节上限。**必须在后端拦**：改密码接口的 body 缓冲有 256 字节，
   而登录只有 128（auth.c 里那两个 char body[]），中间那段能设出一个自己再也
   登不进来的密码，而 config_mgr_set_password() 只查 >= 4 就直接 SHA256 —— 长度
   信息从此丢失，无从补救，只能恢复出厂。前端也拦了一道，但 curl 和配置导入
   绕得过去，所以这里才是真正的闸门。

   63 的来历（取三个约束里最紧的那个）：
     · 登录 body：`{"password":"…"}` 外壳 15 字节，127 可用 → 密码 ≤ 112
     · HTTP Basic：check_basic_auth 的 decoded[96] 传的是 sizeof-1 = 95，
       装 "user:pass"，用户名按 "otbr" 4 字节算 → 密码 ≤ 90
     · auth_buf[160] 装 "Basic " + base64 → 密码 ≤ 109，从不是瓶颈
   取 63 而不是 90：与前端 maxlength 和 WPA2 PSK 的习惯上限对齐，且给用户名
   留出余量（用户名是客户端随便填的，我们只验密码那一半）。
   ⚠️ 这是**字节**不是字符：63 个汉字是 189 字节。 */
#define PASSWORD_MAX_BYTES   63

// ---------------- Events ----------------
ESP_EVENT_DECLARE_BASE(WEBUI_EVENT);

typedef enum {
    WEBUI_EVENT_OTA_STARTED,
    WEBUI_EVENT_OTA_PROGRESS,
    /* 镜像写完、启动分区已切换、**马上要重启**。本地效果（LED 转 BOOT）订它。
       ⚠️ 别拿它发 webhook：post 之后 500ms 就 esp_restart()，而一次 webhook 要走
          DNS + TCP + TLS，几秒起步 —— 必然送不出去（T2.7 之前就是这么丢的）。 */
    WEBUI_EVENT_OTA_COMPLETE,
    WEBUI_EVENT_OTA_FAILED,
    /* 新固件**第一次成功启动**并通过了 mark-valid（2026-09-11 T2.7）。
       这时网络已就绪、不会马上重启，是发"升级完成"通知的唯一正确时机。
       只在 OTA 后的第一次开机发一次，之后每次开机都不发。 */
    WEBUI_EVENT_OTA_VERIFIED,
} webui_event_id_t;

// ---------------- Public API ----------------
esp_err_t webui_init(void);
esp_err_t webui_stop(void);
/* 看门狗 L3 恢复入口：重启 httpd（被滞留连接堵死时） */
esp_err_t webui_httpd_restart(void);
#if CONFIG_BITENGINE_FAULT_INJECTION
/* 层 H TC-W07：往 httpd 工作队列投一个阻塞项 */
esp_err_t webui_inject_httpd_block(uint32_t ms);
#endif
void      webui_ws_push_event(const char *event_type, const char *data_json);

// ---------------- Internal (cross-file) ----------------

// auth.c
bool check_auth(httpd_req_t *req);

/* OTBR token 用的三态 Basic 比对 —— 实现与完整理由见 auth.c。
   三态是必需的：调用方要能区分"没给凭据"（继续走常规认证，让 WebUI 的会话
   cookie 通过）和"给了但不对"（直接拒，不落到管理密码那条路上）。 */
typedef enum { BASIC_ABSENT, BASIC_MATCH, BASIC_MISMATCH } webui_basic_result_t;
webui_basic_result_t webui_basic_check(httpd_req_t *req, const char *secret);
void register_auth_handlers(httpd_handle_t server);

// rest_api.c
void register_rest_api_handlers(httpd_handle_t server);

// websocket.c
void register_ws_handler(httpd_handle_t server);
void ws_start_push_timer(httpd_handle_t server);
void ws_stop_push_timer(void);
void ws_broadcast(const char *json, int len);

// smlight_compat.c
#if !CONFIG_BITENGINE_ENABLE_ZHUB
void register_smlight_handlers(httpd_handle_t server);   /* 只在 HA 型（smlight_compat.c 直连型不编） */
#endif

// otbr_api.c
void register_otbr_handlers(httpd_handle_t server);

/* 产线自检：GET /api/v1/factory/selftest —— 一次调用一个判定。
   三种结论 pass / fail / manual，**manual 绝不算 pass**：整机 verdict 只有在
   零 fail 且零 manual 时才是 pass。见 factory_api.c 顶部。 */
esp_err_t register_factory_handlers(httpd_handle_t server);

// static_files.c
void register_static_file_handler(httpd_handle_t server);
#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
/* 直连型 + Muse（muse_api.c）：/api/v1/muse/…；配对剖面里另有精简 /status 与进度页（通配，最后注册） */
void register_muse_handlers(httpd_handle_t server);
void register_hub_handlers(httpd_handle_t server);    /* 直连型 + Muse：本地设备层出口与审计（hub_api.c） */
void register_pair_profile_handlers(httpd_handle_t server);
int  muse_api_status_tail(char *out, size_t cap);   /* /api/v1/status 末尾的 muse_state / blew_state，含结尾 } */
#endif
// 前端（www 分区）版本，取自镜像里的 /www/version.json；未知时返回 "unknown"
const char *webui_get_www_version(void);

/* 网页手动上传（app / www）正在写 flash。看门狗整机重启前要问（T9.30 ①）；MG24 与云端各有自己的 busy。 */
bool        webui_ota_busy(void);

// Shared helpers
/* 所有 JSON 回包统一用这个 Content-Type。带 charset 是给 Windows PowerShell 5.1
   的 Invoke-RestMethod 看的：没有 charset 它按 ISO-8859-1 解码响应体，
   factory_api 的 detail 中文会成乱码（pwsh 7 默认 UTF-8 所以测不出）。
   aiohttp（HA / python-otbr-api）的 json_re 只锚开头不锚结尾，带 charset 照样通过
   —— 2026-09-13 在跑着的 HA 容器（aiohttp 3.13.5）里实测。*/
#define WEBUI_JSON_CONTENT_TYPE "application/json; charset=utf-8"
esp_err_t send_json_response(httpd_req_t *req, const char *json);
esp_err_t send_error(httpd_req_t *req, int status, const char *error_msg);
/* json_sanitize_copy / webui_json_escape 在组件私有的 json_escape.h；UTF-8 判据在 components/utf8 */

#define AUTH_REQUIRED(req) \
    do { if (!check_auth(req)) { \
        send_error(req, 401, "login_required"); \
        return ESP_OK; } } while (0)

/* 只读端点用这个：设备处于"开放"状态时放行，用户明确锁定后才要认证。
 *
 * 【为什么这些读口该放行】它们暴露的东西**严格少于 mDNS 已经在匿名广播的**：
 * TXT 记录里本来就有 model / mac / serial_number / fw_version / 主机名和端口，
 * `_meshcop._udp` 更在匿名多播 Thread 的网络名与 xPAN。而在同一个 :80 上，
 * 不给任何凭据就能读走 OTBR 的 NetworkKey + PSKc（rest_auth 默认关）。
 * 用密码保护芯片温度、却把网络密钥摆在门口，是说不通的。
 *
 * 【实际造成的伤害】HA 的集成（我们自己的、SMLIGHT 的）都靠先读一个状态端点
 * 来完成"发现→添加"。读口要认证 ⇒ 用户在添加设备时被一个密码框挡住。
 * 2026-08-21 现场：用户在新环境里折腾很久才发现要密码。而生态里同类产品
 * （SMLIGHT/Shelly/ESPHome/WLED）全都是"先探一次，401 才问"。
 *
 * 【闸门为什么复用 rest_auth】它是设备上唯一表达"我要一台封闭设备"的开关。
 * 用户打开它 = 明确要求锁死，那时读口跟着要认证才自洽；否则会出现
 * "我锁了 Thread，温度还在裸奔"。**一个意图一个开关**，不再新增第二个。
 * ⚠️ 也就是说这个开关的语义已经不止于 OTBR —— WebUI 的文案要跟着改，
 * 不能只写"OTBR 接口认证"。
 *
 * ⚠️ 失效方向：取不到配置就当**开着**处理（要认证）。拿不准时宁可多要一次密码，
 * 不要宁可少要 —— 同 otbr_api.c 的 OTBR_AUTH_REQUIRED。
 *
 * ⚠️ 只能用在**读**端点上。任何会改状态的（POST/PUT/DELETE）一律用 AUTH_REQUIRED。 */
#define AUTH_REQUIRED_UNLESS_OPEN(req) \
    do { const thread_config_t *_gc = config_mgr_get_thread(); \
         if (!_gc || _gc->rest_auth) { AUTH_REQUIRED(req); } } while (0)

/* ---- 跨站守卫（2026-08-23）---- 实现与完整理由见 webui_server.c 顶部那段注释 */
int       webui_cross_origin_check(httpd_req_t *req);
esp_err_t send_error_host(httpd_req_t *req);
void      webui_local_addr(httpd_req_t *req, char *out, size_t out_len);
extern const char *const webui_host_suffixes[];   /* 给 /api/v1/net/access-host 渲染 */
extern const size_t      webui_host_suffix_count;

/* ⚠️ 必须放在 AUTH_REQUIRED **之前**（VERIFY.sh 会验行号顺序，不是验存在）。
   放在后面会造出一个**密码预言机**：check_auth 的最后一条路径是 check_basic_auth()，
   它真的比对密码，于是同一个跨站请求，密码错 ⇒ 401、密码对 ⇒ 403/421 ——
   逐次泄漏"这回猜对了"。而 rebinding 之下攻击者与我们同源、可以自由设
   Authorization 头，这就是一条现成的爆破信道。
   放在前面还顺带解决两件事：跨站请求不再消耗登录节流计数器（局域网外的人锁不住
   管理员）；OTBR 那批路由的行为不再随 thread.rest_auth 开关变化，测试可以断言
   一个恒定值而不是"看开关状态推期望值"。 */
#define CROSS_ORIGIN_GUARD(req) \
    do { int _cs = webui_cross_origin_check(req); \
         if (_cs == 421) { send_error_host(req); return ESP_OK; } \
         if (_cs) { send_error(req, _cs, "cross_origin"); return ESP_OK; } } while (0)

/* 放在文件末尾：HA 型里这几行预处理后不存在，放中间会让后面的行号整体移动（HA 型产物要一个字节不变） */
#if CONFIG_BITENGINE_PROFILE_DIRECT
/* httpd 刚处理完一条慢请求（直连型 /hub/invoke 里同步等了设备 ≥ 1 s 的命令）：这本身就证明 httpd 活着 —— 记一次层 B 心跳。
   2026-10-24：一台连不上的 Zigbee 灯被连点十几次，每次控制同步等 8 s，心跳工作项排在后面轮不上，看门狗把"一直在干活"判成
   "卡死"、L3 之后 L4 整机重启。真卡死在某条请求里时它处理不完、走不到这里，照样会被判；快请求不调，免得页面轮询盖住
   "工作队列还跑不跑得动"这一项 */
void ws_httpd_progress(void);
#endif

#ifdef __cplusplus
}
#endif
