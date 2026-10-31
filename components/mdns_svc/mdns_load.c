/*
 * mdns_load.c —— 合成 mDNS 负载（扩容实验 C 方案）
 * 契约与"为什么"见 include/mdns_load.h。整个文件被 Kconfig 包住：关着时
 * 编译出空目标文件，链接器丢掉，出厂固件里这段代码不存在。
 */

#include "sdkconfig.h"

#if CONFIG_BITENGINE_ENABLE_MDNS_LOAD

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "lwip/def.h"
#include "mdns.h"
#include "mdns_load.h"

static const char *TAG = "mdns_load";

/* 名字全部由序号确定性推出 —— 所以撤回时不需要任何跟踪表，
   也就不存在"表和实际注册状态不同步"这个缺陷类。 */
#define FABRIC_DEFAULT "FFFFFFFFFFFFFFFF" /* 全 F：长度与真实 fabric id 一致，但一眼看得出是假的 */

/* ⚠️ fabric 可配的唯一理由：做"合成 ≡ 真实"的等价性比对时，合成用的 fabric 与
   真实不同会多出一条 `_I<fabric>` subtype 名字（第一次出现要付全长），边际字节
   就对不上真实设备。把它设成真实 fabric 即可排除这个干扰项。
   ⚠️ 名字（含 instance 与 subtype）是由 fabric + 序号确定性推出的，所以
   **有条目活着时不许改** —— 改了之后 remove_one() 会算出错误的名字，
   旧记录永远撤不掉。set_fabric() 用 s_active != 0 挡住这件事。 */
static char s_fabric[17] = FABRIC_DEFAULT;
#define NODE_BASE    0xBE57000000000000ULL /* BE57 = bench，同上 */
#define HOST_BASE    0xBE57000000000000ULL
#define SVC_TYPE     "_matter"
#define SVC_PROTO    "_tcp"
#define SVC_PORT     5540

static int s_active = 0;

static void synth_names(int i, char *host, size_t hl, char *inst, size_t il)
{
    snprintf(host, hl, "%016llX", (unsigned long long)(HOST_BASE + (unsigned)i));
    snprintf(inst, il, "%s-%016llX", s_fabric,
             (unsigned long long)(NODE_BASE + (unsigned)i));
}

static esp_err_t add_one(int i)
{
    char host[24], inst[48];
    synth_names(i, host, sizeof(host), inst, sizeof(inst));

    /* AAAA 一条：真实 Matter 设备播的是 Thread 网内 OMR 地址（抓包见
       srp_readvertise.c 抬头），这里用同形状的 ULA。它不可达 —— 本模块只为
       让**线上字节数**与真实记录等价，不为让谁真的连上去。 */
    mdns_ip_addr_t addr = { 0 };
    addr.addr.type = ESP_IPADDR_TYPE_V6;
    addr.addr.u_addr.ip6.addr[0] = lwip_htonl(0xFD162B4BUL);
    addr.addr.u_addr.ip6.addr[1] = lwip_htonl(0x919C0001UL);
    addr.addr.u_addr.ip6.addr[2] = 0;
    addr.addr.u_addr.ip6.addr[3] = lwip_htonl((uint32_t)(0x1000 + i));
    addr.next = NULL;

    esp_err_t err = mdns_delegate_hostname_add(host, &addr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[%d] delegate host %s: %s", i, host, esp_err_to_name(err));
        return err;
    }

    mdns_txt_item_t txt[] = {
        { "SII", "5000" },
        { "SAI", "300"  },
        { "SAT", "4000" },
    };
    err = mdns_service_add_for_host(inst, SVC_TYPE, SVC_PROTO, host, SVC_PORT,
                                    txt, sizeof(txt) / sizeof(txt[0]));
    if (err != ESP_OK) {
        /* ⚠️ 这一条失败正是实验要找的墙（MDNS_MAX_SERVICES 满时返回 ESP_ERR_NO_MEM）。
           必须把刚加的 delegate host 撤掉，否则每撞一次墙就漏一个主机记录，
           后续读数全被污染。 */
        ESP_LOGW(TAG, "[%d] add service %s: %s —— 撞墙了？", i, inst, esp_err_to_name(err));
        mdns_delegate_hostname_remove(host);
        return err;
    }

    /* 子类型 _I<fabric>：同一 fabric 的设备共享一条，与真实部署一致。
       失败不算致命（控制器浏览的是 _matter._tcp 本体），但要吼一声。 */
    char subtype[20];
    snprintf(subtype, sizeof(subtype), "_I%s", s_fabric);
    esp_err_t serr = mdns_service_subtype_add_for_host(inst, SVC_TYPE, SVC_PROTO,
                                                       host, subtype);
    if (serr != ESP_OK) {
        ESP_LOGW(TAG, "[%d] subtype: %s", i, esp_err_to_name(serr));
    }
    return ESP_OK;
}

static void remove_one(int i)
{
    char host[24], inst[48];
    synth_names(i, host, sizeof(host), inst, sizeof(inst));
    /* 顺序要紧：先撤服务（内部会发 TTL=0 的 bye），再撤主机。 */
    esp_err_t err = mdns_service_remove_for_host(inst, SVC_TYPE, SVC_PROTO, host);
    if (err != ESP_OK) ESP_LOGW(TAG, "[%d] remove service: %s", i, esp_err_to_name(err));
    err = mdns_delegate_hostname_remove(host);
    if (err != ESP_OK) ESP_LOGW(TAG, "[%d] remove host: %s", i, esp_err_to_name(err));
}

esp_err_t mdns_load_set(int n, int *out_active, esp_err_t *out_err)
{
    if (n < 0 || n > MDNS_LOAD_MAX) return ESP_ERR_INVALID_ARG;

    esp_err_t first_err = ESP_OK;

    /* 先缩，再涨 —— 顺序反了会在缩容目标下白撞一次上限。 */
    while (s_active > n) {
        remove_one(s_active - 1);
        s_active--;
    }
    while (s_active < n) {
        esp_err_t err = add_one(s_active);
        if (err != ESP_OK) {
            /* 撞墙即停：继续试下去只会刷屏，而"停在第几条"正是要的读数。 */
            if (first_err == ESP_OK) first_err = err;
            break;
        }
        s_active++;
    }

    ESP_LOGW(TAG, "合成负载 = %d 条（目标 %d）%s", s_active, n,
             first_err == ESP_OK ? "" : " —— 撞到上限");
    if (out_active) *out_active = s_active;
    if (out_err) *out_err = first_err;
    return ESP_OK;
}

esp_err_t mdns_load_set_fabric(const char *hex16)
{
    if (!hex16) return ESP_ERR_INVALID_ARG;
    size_t n = strlen(hex16);
    if (n != 16) return ESP_ERR_INVALID_ARG;
    /* 传同一个值是空操作 —— 不能因为"有条目活着"就拒。否则调用方在循环里每次都带
       上 fabric 参数（很自然的写法）会在第二次调用起收到 409，实验脚本直接卡住。
       2026-09-15 用它做 60→100→124 扫描时踩到。 */
    if (strncmp(s_fabric, hex16, 16) == 0) return ESP_OK;
    if (s_active != 0) return ESP_ERR_INVALID_STATE;   /* 见 s_fabric 上方的 ⚠️ */
    for (size_t i = 0; i < 16; i++) {
        char c = hex16[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
            return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_fabric, hex16, 16);
    s_fabric[16] = '\0';
    ESP_LOGW(TAG, "fabric = %s", s_fabric);
    return ESP_OK;
}

const char *mdns_load_fabric(void) { return s_fabric; }

int mdns_load_count(void) { return s_active; }

#endif /* CONFIG_BITENGINE_ENABLE_MDNS_LOAD */
