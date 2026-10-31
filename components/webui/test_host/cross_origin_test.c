/* 主机侧跨站守卫测试 —— 编的是**产品源文件本身**（../cross_origin.c），不是副本。
 *
 * 为什么必须有：这台开发机上唯一那块生产 PCB 没有串口，而 /api/v1/ota/esp32
 * 自己也挂着这个守卫。守卫一旦误伤 curl（不带 Origin、不带 Sec-Fetch-Site），
 * 就既推不上新固件、也没法串口救砖。上板前这里必须全绿。
 *
 * 桩只模拟三件事：头存在/不存在/超长（TRUNC），以及设备的 hostname 和 extra_host。 */
#include <stdio.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <stdbool.h>
#include <stdlib.h>

/* ---- ESP-IDF 桩 ---- */
typedef int esp_err_t;
#define ESP_OK                      0
#define ESP_ERR_NOT_FOUND           0x105
#define ESP_ERR_HTTPD_RESULT_TRUNC  0x8003
typedef struct httpd_req { int _; } httpd_req_t;
static const char *esp_err_to_name(esp_err_t e) { (void)e; return "stub"; }
#define ESP_LOGW(tag, ...) do { (void)(tag); } while (0)

/* 本次请求的三个头。NULL = 头不存在 */
static const char *g_host, *g_sfs, *g_origin;

static esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field,
                                             char *buf, size_t sz)
{
    (void)r;
    const char *v = NULL;
    if (!strcasecmp(field, "Host"))                v = g_host;
    else if (!strcasecmp(field, "Sec-Fetch-Site")) v = g_sfs;
    else if (!strcasecmp(field, "Origin"))         v = g_origin;
    if (!v) return ESP_ERR_NOT_FOUND;
    /* 复刻 httpd_parse.c:1126-1129 的语义：装不下返回 TRUNC，**不是** NOT_FOUND。
       整层防护的成败就在调用方有没有区分这两个。 */
    size_t full = strlen(v) + 1;
    if (full > sz) { memcpy(buf, v, sz - 1); buf[sz - 1] = 0; return ESP_ERR_HTTPD_RESULT_TRUNC; }
    memcpy(buf, v, full);
    return ESP_OK;
}

/* ---- config_mgr 桩 ---- */
typedef struct { char hostname[32]; char extra_host[64]; } sys_config_t;
static sys_config_t g_sys;
static const sys_config_t *config_mgr_get_sys(void) { return &g_sys; }

/* ---- 被测源文件本体 ---- */
#define WEBUI_H_STUBBED 1
#include "../cross_origin.c"

/* ---- 断言 ---- */
static int pass = 0, fail = 0;
static void T(const char *what, const char *host, const char *sfs, const char *origin, int want)
{
    g_host = host; g_sfs = sfs; g_origin = origin;
    httpd_req_t req;
    int got = webui_cross_origin_check(&req);
    if (got == want) { printf("✅ %s\n", what); pass++; }
    else { printf("❌ %s —— 期望 %d，实得 %d\n", what, want, got); fail++; }
}

int main(void)
{
    strcpy(g_sys.hostname, "bitengine");
    g_sys.extra_host[0] = 0;

    printf("── 合法客户端必须放行（红了 = 产线脚本和 HA 集成全线死掉）──\n");
    T("curl 不带任何浏览器头",        "bitengine.local", NULL, NULL, 0);
    T("HTTP/1.0 连 Host 都不带",      NULL,              NULL, NULL, 0);
    T("裸主机名",                     "bitengine",       NULL, NULL, 0);
    T("IPv4 字面量",                  "192.168.1.44",   NULL, NULL, 0);
    T("IPv4 带端口",                  "192.168.1.44:8081", NULL, NULL, 0);
    T("IPv6 字面量",                  "[fd11::1]",       NULL, NULL, 0);
    T("IPv6 带端口",                  "[fd11::1]:8081",  NULL, NULL, 0);
    T("localhost",                    "localhost",       NULL, NULL, 0);
    T("路由器给的 .lan（我们自己让它建的）", "bitengine.lan", NULL, NULL, 0);
    T("尾点 FQDN",                    "bitengine.local.", NULL, NULL, 0);
    T("带端口的 mDNS 名",             "bitengine.local:80", NULL, NULL, 0);
    T("前端同源 POST",                "bitengine.local", "same-origin", "http://bitengine.local", 0);
    T("同源带端口",                   "bitengine.local:8081", "same-origin", "http://bitengine.local:8081", 0);
    T("Sec-Fetch-Site: none（地址栏直接输入）", "bitengine.local", "none", NULL, 0);
    T("其余私有后缀 home.arpa",        "bitengine.home.arpa", NULL, NULL, 0);

    printf("\n── 攻击形态必须被拒 ──\n");
    T("经典 CSRF：第三方 Origin",      "bitengine.local", NULL, "http://evil.com", 403);
    T("DNS rebinding：Origin 与 Host 一致", "evil.com", NULL, "http://evil.com", 421);
    T("rebinding 且不带 Origin",       "evil.com",        NULL, NULL, 421);
    T("主机名前缀绕过",                "bitengine.attacker.com", NULL, NULL, 421);
    T("主机名后接连字符",              "bitengine-evil.com", NULL, NULL, 421);
    /* ⚠️ 这一条才是 `h[n] != '.'` 那行边界检查真正防的东西，别和上面两条搞混：
       `bitengine.attacker.com` 是被**后缀表**拦下的（attacker.com 不在表里），
       删掉边界检查它照样被拒。而下面这个不同 —— 比对是从 h+n+1 开始的，
       少了边界检查就会跳过那个 '-' 直接拿 "local" 去比后缀，于是**放行**。
       2026-08-23 的变异测试就是在这里发现判据与变异配错了：删掉边界检查，
       "前缀绕过"那条判据纹丝不动地绿着。 */
    T("分隔符不是点也被当成点",        "bitengine-local", NULL, NULL, 421);
    T("分隔符是任意字符",              "bitengineXlocal", NULL, NULL, 421);
    T("img/script 跨站 GET（无 Origin）", "bitengine.local", "cross-site", NULL, 403);
    T("same-site（不同端口/子域）",     "bitengine.local", "same-site", NULL, 403);
    T("Origin: null（sandbox iframe）", "bitengine.local", NULL, "null", 403);
    T("端口不同的 Origin",             "bitengine.local", NULL, "http://bitengine.local:8081", 403);

    printf("\n── TRUNC：攻击者自选长度的静默绕过 ──\n");
    {
        static char longhost[600], longorigin[600];
        memset(longhost, 'a', 500); strcpy(longhost + 500, ".com");
        snprintf(longorigin, sizeof(longorigin), "http://%s", longhost);
        T("超长 Host 必须拒（不能当成头缺失）",  longhost, NULL, NULL, 421);
        T("超长 Origin 必须拒",                  "bitengine.local", NULL, longorigin, 403);
        static char longsfs[64];
        memset(longsfs, 'x', 60); longsfs[60] = 0;
        T("超长 Sec-Fetch-Site 必须拒",          "bitengine.local", longsfs, NULL, 403);
    }

    printf("\n── extra_host：421 的唯一出路 ──\n");
    strcpy(g_sys.extra_host, "gw.mynet");
    T("登记过的名字放行",              "gw.mynet",  NULL, NULL, 0);
    T("登记名带端口也放行",            "gw.mynet:8123", NULL, NULL, 0);
    T("没登记的仍拒",                  "other.mynet", NULL, NULL, 421);
    T("登记名的前缀绕过仍拒",          "gw.mynet.evil.com", NULL, NULL, 421);
    g_sys.extra_host[0] = 0;
    T("清空后立即失效",                "gw.mynet",  NULL, NULL, 421);

    printf("\n── 边界 ──\n");
    strcpy(g_sys.hostname, "");
    T("hostname 为空时不放行任意名字", "whatever.local", NULL, NULL, 421);
    T("hostname 为空时 IP 仍可用",     "192.168.1.1", NULL, NULL, 0);
    strcpy(g_sys.hostname, "bitengine");
    T("畸形 IPv6（无右括号）不当作 IP", "[evil", NULL, NULL, 421);
    T("三段 IP 不算字面量",            "1.2.3",  NULL, NULL, 421);

    printf("\nResult: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
