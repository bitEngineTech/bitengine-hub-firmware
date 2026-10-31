/* components/webui/cross_origin.c —— 跨站守卫
 *
 * 单独一个文件不是为了整洁，是为了**能在主机上测**：这台开发机上唯一那块生产
 * PCB 没有串口（本板也没有 BOOT 键），而 /api/v1/ota/esp32 自己也挂着这个守卫。
 * 一旦守卫误伤了 curl，就既推不上新固件、也没法串口救砖。所以这里的每条判定
 * 必须在上板之前用 test_host/ 跑穿 —— 见该目录下的 run.sh。
 */
#include "webui.h"
#include "config_mgr.h"
#include "esp_log.h"
#include <string.h>
#include <stdbool.h>

static const char *TAG = "webui";

/* ================= 跨站守卫（2026-08-23）=================
 *
 * 补的不是「你是谁」——那是 check_auth 的事，三条凭据路径（Cookie/Bearer/Basic）
 * 都只回答那个问题。补的是「**这个请求是谁让浏览器发的**」。
 *
 * 三道检查挡三条**互不覆盖**的链，缺一条那条就整条通：
 *
 *   · Host 白名单      挡 DNS rebinding。攻击者把 evil.com 重绑到网关 IP，浏览器
 *                      认为同源 ⇒ 响应可读、PUT/DELETE 不再预检。此时 Origin 与
 *                      Host **都是** evil.com、两者相等 —— 下面那条 Origin 一致性
 *                      检查对它天然失明。
 *   · Sec-Fetch-Site   挡**不带 Origin** 的跨站 GET。<img src=".../spectrum/scan">
 *                      会真的让射频跑一次能量扫描（rest_api.c，同步阻塞），而按
 *                      Fetch 规范这类子资源请求根本不带 Origin —— 这不是"老浏览器
 *                      问题"，是所有浏览器的正常行为。
 *   · Origin 一致性    挡经典 CSRF（第三方页面的 no-cors POST）。此时 Host 是 IP
 *                      字面量，上面第一条又天然放行。
 *
 * 三条共用同一个失效方向：**头缺失即放行**。curl / aiohttp(HA) / pysmlight /
 * tools 下的 sh 脚本一律不发 Origin 也不发 Sec-Fetch-Site，HTTP/1.0 客户端不发 Host。
 * 要求它们必须发 = 一次性打死全部产线脚本和 HA 集成，而现场症状是"设备突然
 * 全不通"，最容易被误判成网络或硬件问题。
 *
 * ⚠️ 但「缺失」≠「读失败」，这条是整层防护的成败所在。
 *    httpd_req_get_hdr_value_str() 在值超出缓冲时返回 ESP_ERR_HTTPD_RESULT_TRUNC
 *    （httpd_parse.c:1126-1129），**不是** ESP_ERR_NOT_FOUND；只有头确实不存在
 *    才是后者。写成 `!= ESP_OK → 放行` 等于给攻击者一个开关：域名长度由他自选，
 *    换一个 300 字节的域名，整层守卫**静默**失效 —— 日志不打、VERIFY 全绿、
 *    行为测试也全绿（测试用的都是短域名）。所以下面每处都显式区分 NOT_FOUND。
 *    CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024，攻击者有充足空间构造超长值。
 *
 * ⚠️ 绝不能做成全局过滤器。/api/actions 必须继续走 static_files.c 的 404 兜底：
 *    python-otbr-api 对非 200/404 一律抛 OTBRError → ConfigEntryNotReady →
 *    HA 的 otbr 集成加载不起来（docs/13 决策 1.5）。只能逐 handler 挂宏。
 */

/* Host 是 IP 字面量 ⇒ 浏览器根本没做过 DNS 解析 ⇒ 不可能是 rebinding。
   这条是整个守卫的支点：靠它，设备**不需要知道自己有哪些 IP** —— 双上行、
   静态 IP、SoftAP 的 192.168.4.1、IPv6 link-local 带 %zone、8081 端口，
   全部自动覆盖，也就没有"网络一变白名单就过期"这个维护负担。 */
static bool host_is_ip_literal(const char *h)
{
    /* IPv6 字面量必须成对方括号。只查 '[' 会让 "[evil" 这种畸形值被当成 IP
       从而跳过整个名字检查 —— 浏览器构造不出来，但没有理由留着。 */
    if (h[0] == '[') return strchr(h, ']') != NULL;
    int dots = 0;
    for (const char *p = h; *p; p++) {
        if (*p >= '0' && *p <= '9') continue;
        if (*p == '.') { dots++; continue; }
        return false;                          /* 出现别的字符 ⇒ 这是个名字 */
    }
    return dots == 3;
}

/* 允许跟在主机名后面的后缀。判据不是"看起来像内网"，而是**这些后缀在公网根区里
   不存在，浏览器无法通过公共 DNS 把它们解析到攻击者的服务器** —— 而 rebinding
   恰恰要求受害者浏览器能解析攻击者的域名。
   .lan 尤其重要：wifi_mgr.c 与 eth_mgr.c 都会把 hostname 作为 DHCP option 12
   报给路由器，于是 bitengine.lan 这个名字**是我们自己的固件让路由器建的**，
   也是用户最常用的入口。漏掉它 = 把用户锁在自己造的门外。
   fritz.box 是唯一例外（.box 已是真 gTLD），收它是因为 AVM 路由器在欧洲装机量
   太大，且要利用它得先注册 fritz.box 这个二级域，成本远高于随便注册一个 evil.com。
   ⚠️ 不要为了"兼容性"往这里加 .com/.net/.io 之流，加一个整条防御归零。
   路由器自定义 local domain / 反向代理 / 将来的 DDNS 走 sys.extra_host。 */
const char *const webui_host_suffixes[] = {
    "local", "lan", "home", "home.arpa", "internal", "localdomain", "fritz.box",
};
const size_t webui_host_suffix_count =
    sizeof(webui_host_suffixes) / sizeof(webui_host_suffixes[0]);

/* ⚠️ 必须是**整标签**匹配。放宽成前缀匹配（strncasecmp 不检查边界字符）会让
   bitengine.attacker.com 通过 —— 整条防御归零。下面 h[n] 那两行就是边界。 */
static bool host_name_ok(const char *h)
{
    if (strcasecmp(h, "localhost") == 0) return true;

    const sys_config_t *sc = config_mgr_get_sys();
    if (!sc) return false;                     /* 拿不准就拒，同 OTBR_AUTH_REQUIRED */

    /* 用户自填的额外访问名：反向代理、路由器自定义 local domain、将来的 DDNS。
       设备**永远猜不到**这一类（DHCP 给不到 option 15），没有它，这些场景只能
       靠恢复出厂脱身。字符校验在 config_mgr_set_extra_host()，这里只比。 */
    if (sc->extra_host[0] && strcasecmp(h, sc->extra_host) == 0) return true;

    size_t n = strlen(sc->hostname);
    if (n == 0 || strncasecmp(h, sc->hostname, n) != 0) return false;
    if (h[n] == '\0') return true;             /* 裸主机名（search domain 补全前） */
    if (h[n] != '.')  return false;            /* bitengine-evil.com 不算 */
    for (size_t i = 0; i < webui_host_suffix_count; i++) {
        if (strcasecmp(h + n + 1, webui_host_suffixes[i]) == 0) return true;
    }
    return false;
}

/* 返回 0 = 放行；否则返回应发的状态码（403 跨站 / 421 陌生 Host）。 */
int webui_cross_origin_check(httpd_req_t *req)
{
    /* 264 = 域名上限 253 + ":65535" + NUL 的余量。缓冲小于它 ⇒ 合法的长 mDNS 名
       会被判 421；而只要 TRUNC 判为拒（下面那行），缓冲小只是误伤不是漏洞。 */
    char host[264];
    esp_err_t e = httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    if (e == ESP_ERR_NOT_FOUND) return 0;                 /* HTTP/1.0，不是浏览器 */
    if (e != ESP_OK) {                                     /* TRUNC 等一律拒 */
        ESP_LOGW(TAG, "拒绝：Host 头异常/超长（%s）", esp_err_to_name(e));
        return 421;
    }

    /* 名字匹配前临时切掉 :port 和尾点；比 Origin 时要用**带端口的原文**，
       所以只是临时置 NUL，比完立刻还原。
       IPv6 的冒号在方括号里，必须先跳过 ']'，否则 [fd11::1] 会被从第一个冒号
       截断成 "[fd11"，所有 IPv6 访问全被 421。 */
    char *pc = (host[0] == '[') ? strchr(host, ']') : host;
    size_t cut = 0, dot = 0;
    if (pc && (pc = strchr(pc, ':')) != NULL) { cut = (size_t)(pc - host); *pc = '\0'; }
    size_t hl = strlen(host);
    /* 尾点 FQDN：Chrome/Firefox 会原样保留 http://bitengine.local./ 里的那个点 */
    if (hl && host[hl - 1] == '.') { dot = hl - 1; host[dot] = '\0'; }

    bool host_ok = host_is_ip_literal(host) || host_name_ok(host);

    if (dot) host[dot] = '.';
    if (cut) host[cut] = ':';
    if (!host_ok) {
        ESP_LOGW(TAG, "拒绝陌生 Host \"%s\"（疑似 DNS rebinding）", host);
        return 421;
    }

    /* Sec-Fetch-Site：same-origin / none 放行，same-site 与 cross-site 拒。
       same-site 也拒是有意的 —— 它意味着注册域相同但源不同（不同端口/子域），
       那依然是别的页面在替用户发请求。本 SPA 只用相对路径，永远是 same-origin，
       零误伤。将来谁写了"从 :80 的页面 fetch :8081"，症状会是 403 而不是 CORS 错。 */
    char sfs[24];
    e = httpd_req_get_hdr_value_str(req, "Sec-Fetch-Site", sfs, sizeof(sfs));
    if (e == ESP_OK) {
        if (strcasecmp(sfs, "same-origin") != 0 && strcasecmp(sfs, "none") != 0) {
            ESP_LOGW(TAG, "拒绝跨站请求 Sec-Fetch-Site=%s Host=%s", sfs, host);
            return 403;
        }
    } else if (e != ESP_ERR_NOT_FOUND) {
        return 403;                            /* 超长 = 构造出来的，真值最长 11 字节 */
    }

    char origin[280];
    e = httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin));
    if (e == ESP_ERR_NOT_FOUND) return 0;                  /* 非浏览器客户端 */
    if (e != ESP_OK) { ESP_LOGW(TAG, "拒绝：Origin 头异常/超长"); return 403; }

    /* 比的是**带端口的** Host 原文：:8081 上 Origin 是 http://gw:8081。
       Origin: null（sandbox iframe / file://）没有 "://"，落到下面 403 —— 想要的。 */
    const char *p = strstr(origin, "://");
    if (p && strcasecmp(p + 3, host) == 0) return 0;
    ESP_LOGW(TAG, "拒绝跨站请求 Origin=%s Host=%s", origin, host);
    return 403;
}

