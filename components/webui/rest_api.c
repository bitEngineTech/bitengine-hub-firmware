#include "webui.h"
#include "config_mgr.h"
#include "network_mgr.h"
#include "wifi_probe.h"
#include "esp_coex_i154.h"   /* 诊断：运行时切 802.15.4 的共存状态位 */
#include "watchdog.h"
#include "wd_heaptrace.h"
#include "ezsp_bridge.h"
#include "spectrum.h"
#include "thread_br.h"
#include "thread_br_diag.h"   /* 诊断构建专用的声明都在 #if 里，出厂构建等于空 */
#include "srp_readvertise.h"
#if CONFIG_BITENGINE_ENABLE_MDNS_LOAD
#include "mdns_load.h"
#endif
#include "led_ctrl.h"
#include "log_store.h"          /* GET /api/v1/logs */
#include "json_escape.h"        /* 外来字符串进 JSON：转义 / 剥离 / 修 UTF-8 */
#include "utf8.h"               /* 写入口拒收非法 UTF-8（T9.33） */
#include "poe.h"                /* GET /api/v1/poe/status */
#include "device_identity.h"    /* /status 的 serial_number */
#include "devkit_pins.h"        /* POE_DETECT_PIN —— 回包里报出实际引脚号 */
#include "scheduler.h"          /* scheduler_night_active() —— 夜间时段是否生效 */
#include "bitengine_mqtt.h"     /* mqtt_client_is_connected() */
#include "mg24_image_record.h"
#include "ncp_version.h"
#include "ota_mg24.h"
#include "ota_cloud.h"     /* 云端更新：/ota/check 与 /ota/cloud */
#include "www_image.h"     /* www 镜像形状校验 + 写入，与云端更新共用 */
#include "uart_bench.h"
#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
#include "muse_hold.h"     /* 安全模式退出时"先关掉 Muse"的留言（docs/15 附录 E33） */
#endif
#if CONFIG_BITENGINE_ENABLE_ZHUB
#include "zhub.h"          /* 直连型的 Zigbee 主机：/zigbee/status 的 hub 对象、/zigbee/restart（docs/15 第八节） */
#include "hub_meta.h"      /* 设备名字 / 房间（M5 第二步）：/zigbee/status 的 hub.meta（哪页这次开机只读、改名失败几次） */
#endif
#include "esp_netif.h"   /* esp_netif_get_handle_from_ifkey / esp_netif_get_mac —— 以太网 MAC 必须问网卡要 */

#include <string.h>
#include <stdlib.h>     /* atoi —— ?dwell_ms= 解析 */
#include <stdio.h>
#include <time.h>       /* /api/v1/ntp 回包里的 local_time */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_mac.h"          /* /status 的 mac —— HA 集成用它做 unique_id */
#include "esp_wifi_types.h"   /* WIFI_REASON_* —— 断连原因归类 */
#include "esp_secure_boot.h"    /* /status 的 app_signed —— 产线首烧判据 */
#if CONFIG_BITENGINE_DIAG_COREDUMP
#include "esp_core_dump.h"      /* GET /api/v1/diag/coredump（诊断构建）*/
#endif
#include "esp_event.h"
#include "cJSON.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "webui_api";
static volatile bool s_ota_in_progress = false;

bool webui_ota_busy(void) { return s_ota_in_progress; }

// IP address byte extraction helper
#define IP_B0(addr) ((uint8_t)((addr) & 0xFF))
#define IP_B1(addr) ((uint8_t)(((addr) >> 8) & 0xFF))
#define IP_B2(addr) ((uint8_t)(((addr) >> 16) & 0xFF))
#define IP_B3(addr) ((uint8_t)(((addr) >> 24) & 0xFF))

/* Receive a complete request body. A single httpd_req_recv can return less
   than content_len (TCP segmentation) and would make valid requests fail. */
/* 返回 >0 = 实际长度；-1 = 空/读失败；-2 = 超出缓冲区。
   分开 -1/-2 是因为两者的用户动作完全不同：前者"你没发内容"，后者"你的 broker
   地址太长了"。都报 empty_body 会把人引去查网络而不是查输入。老调用方一律
   判 `<= 0`，不受影响。 */
static int recv_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    int total = req->content_len;
    if (total <= 0) return -1;                      /* 必须先判负：(size_t)(-1) 是天文数字 */
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

/* Accept an IPv4 field as either a numeric (uint32, network order) or a
   dotted-quad string — the shipped frontend sends strings. */
static bool parse_ip_field(const cJSON *item, uint32_t *out)
{
    if (!item) return false;
    if (cJSON_IsNumber(item)) {
        *out = (uint32_t)item->valuedouble;
        return true;
    }
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
        ip4_addr_t a;
        if (ip4addr_aton(item->valuestring, &a)) {
            *out = a.addr;
            return true;
        }
    }
    return false;
}

/* ---- 可选字段提取（配置类 POST 共用）----
 * 三个 helper 的共同契约：**字段缺失 = 保持原值**（配置端点都是部分更新，前端
 * 只提交用户改过的那几项）；**字段存在但不合法 = 返回 false**，调用方一律回
 * 400，绝不"尽力而为"地吞掉。
 *
 * 为什么值得单独抽出来：这三件事（类型不对 / 超范围 / 超长）就是配置端点全部
 * 的失败面。散在五个 handler 里手写必然有人漏一个 —— 05 模块 14 个 setter 漏
 * 了 11 个输入校验，就是这么来的。
 */
static bool json_opt_int(const cJSON *root, const char *key, int lo, int hi, int *out)
{
    const cJSON *it = cJSON_GetObjectItem(root, key);
    if (!it) return true;
    if (!cJSON_IsNumber(it)) return false;
    /* 用 valuedouble 判范围：cJSON 的 valueint 会把超大数饱和到 INT_MAX，
       先饱和再比较的话 1e9 会被当成合法的 INT_MAX 放过去。 */
    double v = it->valuedouble;
    if (!(v >= (double)lo && v <= (double)hi)) return false;
    *out = (int)v;
    return true;
}

static bool json_opt_bool(const cJSON *root, const char *key, bool *out)
{
    const cJSON *it = cJSON_GetObjectItem(root, key);
    if (!it) return true;
    if (!cJSON_IsBool(it)) return false;
    *out = cJSON_IsTrue(it);
    return true;
}

/* 超长输入一律拒绝，**不截断**。截断一个 broker 主机名或 webhook URL 会得到
   一个语法合法、却指向别处的地址，然后网关拿着它去连 —— 静默截断在这里等于
   把请求悄悄改道。
   非法 UTF-8 同样拒绝（T9.33）：cJSON 解析不校验，坏字节会原样进 NVS，之后
   /config/export 与 webhook 自动备份吐出来的就不再是合法 JSON。浏览器发不出这种
   字节，只有脚本会撞上，所以打一行 W 说清是哪个字段。 */
static bool json_opt_str(const cJSON *root, const char *key, char *dst, size_t dst_size)
{
    const cJSON *it = cJSON_GetObjectItem(root, key);
    if (!it) return true;
    if (!cJSON_IsString(it) || !it->valuestring) return false;
    if (strlen(it->valuestring) >= dst_size) return false;
    if (!utf8_valid(it->valuestring)) {
        ESP_LOGW(TAG, "字段 %s 不是合法 UTF-8，拒收", key);
        return false;
    }
    strlcpy(dst, it->valuestring, dst_size);
    return true;
}

/* 定义在 /radio/region 那组（文件后半），handle_zigbee_status 先用到 */
static int txp_json(char *out, size_t cap);

// ---- GET /api/v1/status ----
/* 直连型多 "profile":"direct"（docs/15 设计 10.1：没有 profile 字段即 HA 型，工具据此判型），纯以太网剖面
   再多一个 "wifi":"absent"（设计 2.2）；wifi_* 字段照旧在（全是 0 / false），读它们的脚本不用改。
   HA 型两个宏都是空串，/status 一个字节都不变（架构 5.8 R-3，交叉清单 B-11）。 */
#if CONFIG_BITENGINE_PROFILE_DIRECT
#define STATUS_PROFILE_FIELD "\"profile\":\"direct\","
#else
#define STATUS_PROFILE_FIELD ""
#endif
#if CONFIG_BITENGINE_NET_ETH_ONLY
#define STATUS_WIFI_ABSENT_FIELD "\"wifi\":\"absent\","
#else
#define STATUS_WIFI_ABSENT_FIELD ""
#endif
/* T1.2 步骤 3：未签名 NCP 固件此刻是否被放行（按键确认过的开发者解锁）。
   放进 /status 是为了让 Dashboard、自检脚本一眼看到"这台设备现在收任何 .gbl"。 */
static bool ncp_signing_unlocked_now(void)
{
    ota_mg24_unlock_status_t u;
    ota_mg24_unlock_get(&u);
    return u.unlocked;
}

static esp_err_t handle_status(httpd_req_t *req)
{
    AUTH_REQUIRED_UNLESS_OPEN(req);

    const health_status_t  *hs = watchdog_get_status();
    const bridge_stats_t   *bs = ezsp_bridge_get_stats();
    const sys_config_t     *sc = config_mgr_get_sys();
    const esp_app_desc_t   *app = esp_app_get_description();
    network_status_t ns;
    network_mgr_get_status(&ns);

    // String representations for bridge_mode and active_interface
    const char *mode_str = "lan";
    bridge_mode_t bm = ezsp_bridge_get_mode();
    if (bm == BRIDGE_MODE_USB) mode_str = "usb";
    else if (bm == BRIDGE_MODE_DISABLED) mode_str = "disabled";

    const char *iface_str = "none";
    if (ns.active_interface == NET_IF_ETH)  iface_str = "eth";
    if (ns.active_interface == NET_IF_WIFI) iface_str = "wifi";

    // Sanitize strings that go into JSON via snprintf.
    // www_version 来自 www 分区里的 version.json，而 www 分区可被 OTA 覆写，
    // 所以和 SSID/hostname 一样按不可信字符串处理。
    char safe_ssid[33], safe_hostname[32], safe_www[40];
    json_sanitize_copy(safe_ssid, ns.wifi_ssid, sizeof(safe_ssid));
    json_sanitize_copy(safe_hostname, sc->hostname, sizeof(safe_hostname));
    json_sanitize_copy(safe_www, webui_get_www_version(), sizeof(safe_www));

    /* 硅版本，MXX 格式（100 = v1.0，102 = v1.2/ECO3）。装机之后这是唯一还能
       远程读到的批次凭据——插线跑 esptool 只在板子在手上时才可行。
       固定值，不进 WS 周期推送（那里只放动态字段）。 */
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    char chip_rev[12];
    snprintf(chip_rev, sizeof(chip_rev), "v%u.%u",
             (unsigned)(chip.revision / 100), (unsigned)(chip.revision % 100));

    /* 设备 MAC。用 ESP_MAC_WIFI_STA 而不是 eFuse 基址：这是网络上**真正可见**的
       那个（路由器 DHCP 列表、ARP 表里就是它），用户能对上号，HA 也用它把设备
       跟网络发现关联起来。与 smlight_compat.c 保持一致。
       ⚠️ 与 factory_api.c 的 identity 项**故意不同**：那里用
       esp_read_mac(ESP_MAC_EFUSE_FACTORY) 取乐鑫烧进 eFuse 的出厂 MAC 做产线追溯，
       两者本来就是两个用途。（2026-09-11 订正：那里原来用的是
       esp_efuse_mac_get_default()，在 C5 这种 802.15.4 芯片上它返回 8 字节 EUI-64，
       接进 6 字节数组既越界又打出一个网络上不存在的值。） */
    /* 最近一次 WiFi 断连的归类。配网页面靠它告诉用户到底哪儿错了 ——
       只给一个裸数字（reason=15）用户看不懂，只能盲改密码。
       归类而不是逐码翻译：IDF 有 40+ 个 reason，用户能采取的行动只有三类。
       原始码同时也给出去（wifi_last_reason），便于我们远程排查。 */
    /* 交互式配网的进行状态。配网页面靠它决定显示"连接中"、"成功"还是
       "失败+原因"，而不是像以前那样盲等 10 秒刷新页面碰运气。 */
    prov_state_t pst = network_mgr_provision_state();
    const char *prov = (pst == PROV_CONNECTING) ? "connecting"
                     : (pst == PROV_OK)         ? "ok"
                     : (pst == PROV_FAILED)     ? "failed" : "idle";

    int wreason = network_mgr_wifi_last_reason();
    const char *werr =
        (wreason == 0)   ? "none" :
        /* 15/202/204 都是"认证过不去" —— 绝大多数情况就是密码错 */
        (wreason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
         wreason == WIFI_REASON_AUTH_FAIL ||
         wreason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
         wreason == WIFI_REASON_AUTH_EXPIRE)          ? "auth_failed" :
        /* 201 找不到：SSID 打错、路由器没开、或**不是 5GHz**（本机只支持 5G） */
        (wreason == WIFI_REASON_NO_AP_FOUND)          ? "ap_not_found" :
        /* 200 信标超时 = 连上过但信号丢了，通常是路由器重启或距离太远 */
        (wreason == WIFI_REASON_BEACON_TIMEOUT)       ? "signal_lost" :
                                                        "other";

    config_nvs_health_t nvs_health;
    config_mgr_get_nvs_health(&nvs_health);

    const esp_partition_t *run_part = esp_ota_get_running_partition();
    const char *ota_slot_label = run_part ? run_part->label : "unknown";
    bool ota_rolled_back = (esp_ota_get_last_invalid_partition() != NULL);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* 序列号是 JSON 的**值**位置：有身份时是带引号的字符串，没有时是裸 null。
       device_identity 只在形状校验通过后才让它非空（字母数字加连字符），
       所以这里不需要再转义 —— 但仍然走 json_sanitize_copy，因为"上游保证过了"
       这种理由在本仓库已经错过一次（www_version 那次）。 */
    char serial_buf[48];
    if (device_identity_valid()) {
        char safe_sn[DEVICE_ID_MAX];
        json_sanitize_copy(safe_sn, device_identity_serial(), sizeof(safe_sn));
        snprintf(serial_buf, sizeof(serial_buf), "\"%s\"", safe_sn);
    } else {
        strlcpy(serial_buf, "null", sizeof(serial_buf));
    }

    /* 以太网 MAC 是**另一个** —— 两个接口在路由器眼里是两台设备，
       要让 IP 不再乱跳就得给**两条**都做 DHCP 保留。以前只报 WiFi 的，
       用户照着做完还是会在切到以太网时换 IP。

       ⚠️ 2026-08-22 订正：这里原来用 esp_read_mac(emac, ESP_MAC_ETH)，
       **报出来的值和线上真实的差一个 bit**，于是用户照着做的 DHCP 保留
       永远匹配不上、静默失效 —— 而这正是我们让他做保留要解决的问题。
       实测对照（ARP 表是线上真相）：
           .43 真实   3a:44:be:12:34:57
           我们上报   38:44:BE:12:34:57     ← 差本地管理位(0x02)
       原因：DM9051 是 SPI 以太网模块，自身没有 MAC。managed_components/
       espressif__ethernet_init/ethernet_init.c 先 esp_read_mac(ESP_MAC_ETH)
       取到 38:…，再 esp_derive_local_mac() 派生出 3a:…（置 UL 位），
       最后 esp_iface_mac_addr_set() 把**派生后**的写进网卡。
       esp_read_mac 拿到的是派生**之前**那个。

       正确读法是问网卡本身要。拿不到就报空串 —— 报一个错的 MAC 比不报更糟，
       用户会拿它去配保留。 */
    char eth_mac_str[18] = "";
    esp_netif_t *eth_nif_mac = esp_netif_get_handle_from_ifkey("ETH_DEF");
    uint8_t emac[6] = {0};
    if (eth_nif_mac && esp_netif_get_mac(eth_nif_mac, emac) == ESP_OK) {
        snprintf(eth_mac_str, sizeof(eth_mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 emac[0], emac[1], emac[2], emac[3], emac[4], emac[5]);
    }

    /* 运行中的固件带不带有效签名块。**产线首烧必须是签名版**：本项目的 OTA
       信任锚是运行中 app 自己的签名块，不是 eFuse（见 sdkconfig.defaults 末尾
       与 docs/guide/固件签名与密钥保管.md）。刷进未签名固件的设备会拒绝一切
       后续 OTA，设备侧只有一条 ESP_LOGE，从外面完全看不出来 —— 只能拆机串口
       重刷。所以这件事必须在产线上被机器断言，而不是等用户第一次升级时才暴露。
       与 chip_revision 同类：出厂即固定，不进 WS 周期推送。
       只读一次缓存：该调用要从 flash 读签名扇区，而 /status 是被前端轮询的。 */
    static int s_app_signed = -1;   /* -1=未查 0=否 1=是 */
    if (s_app_signed < 0) {
        esp_image_sig_public_key_digests_t sig = {0};
        /* digest_public_keys=false —— 只要块数，不需要算公钥摘要 */
        s_app_signed = (esp_secure_boot_get_signature_blocks_for_running_app(false, &sig) == ESP_OK
                        && sig.num_digests > 0) ? 1 : 0;
        if (!s_app_signed) {
            ESP_LOGE(TAG, "运行中的固件**没有签名块** —— 本机将拒绝一切 OTA 升级，"
                          "只能串口重刷。产线首烧刷错成 bitengine-unsigned.bin 了？");
        }
    }

    /* NCP firmware, read from the MG24 itself at boot (ncp_version.c). Fixed
       for the life of the boot, so it belongs here and not in the WS push. */
    char ncp_ver[40];
    ncp_version_format(ncp_ver, sizeof(ncp_ver));

    /* Thread 子节点恢复进度（Thread BR 未运行时为 0/0） */
    uint8_t th_total = 0, th_online = 0;
    thread_br_get_child_stats(&th_total, &th_online);

    /* v2：内部堆水位与分配失败数。min_free_heap 那个聚合值留着只是为了前端
       兼容，判断内存健康要看这两个（见 docs/04_watchdog_design_v2.md 第七节）。 */
    wd_resource_t wd_res;
    wd_resource_get(&wd_res);

    /* Zigbee 信道/功率一律报 NCP 实读值，读不到就报 null。绝不拿 config 里的
       zigbee.channel 顶替：那个值从来没有下发给 MG24（桥是纯字节流透传，建网
       归 Z2M/ZHA 管），把它当实际信道报出去正是这次要修的病。前端显示 '--'
       比一个看起来言之凿凿的假信道有用。 */
    const zigbee_config_t *zbc = config_mgr_get_zigbee();
    char zb_ch_buf[8];
    if (zbc && zbc->channel_declared) {
        snprintf(zb_ch_buf, sizeof(zb_ch_buf), "%u", zbc->channel);
    } else {
        strlcpy(zb_ch_buf, "null", sizeof(zb_ch_buf));
    }

    /* 共存告警：两颗射频在同一块板上，信道贴太近会互相压制。判定规则只在
       thread_br_get_coex_status() 里有一份。两边都读到实测值才给结论。 */
    /* 声明值过期检测：本 PAN 出现在别的信道上 = 用户改过 Z2M/ZHA 忘了同步。
       这是声明值唯一的硬伤（会悄悄过期），靠 PAN 精确匹配把它变成可发现的。 */
    char host_absent_buf[16] = "null";
#if !CONFIG_BITENGINE_ENABLE_ZHUB
    /* 直连型没有上位机这回事（没有 :6638），恒为 null = 不适用。换配置时 NVS 原样带过来的 host_ever_seen
       不能让它变成"协调器已断开 N 分钟"（首页横幅，审计 2026-10-22）。 */
    if (bs && zbc && zbc->host_ever_seen) {
        snprintf(host_absent_buf, sizeof(host_absent_buf), "%"PRIu32,
                 bs->active_clients ? 0u
                                    : (bs->uptime_seconds - bs->last_client_seen_s));
    }
#endif

    zigbee_channel_check_t zchk;
    thread_br_get_zigbee_check(&zchk);
    /* 跑过就如实报状态，别用 null 同时表示"正常"和"从没跑过" —— 那两件事
       对用户的含义完全不同。state: ok | mismatch | not_found */
    char zchk_buf[192] = "null";
    if (zchk.valid) {
        char det[8];
        if (zchk.mismatch) snprintf(det, sizeof(det), "%u", zchk.detected_channel);
        else               strlcpy(det, "null", sizeof(det));
        snprintf(zchk_buf, sizeof(zchk_buf),
                 "{\"state\":\"%s\",\"pan_id\":\"0x%04X\",\"declared\":%u,"
                 "\"detected\":%s,\"rssi\":%d,\"checked_at\":%"PRIu32"}",
                 zchk.mismatch ? "mismatch" : (zchk.not_found ? "not_found" : "ok"),
                 zchk.pan_id, zchk.declared_channel, det,
                 zchk.rssi, zchk.checked_at);
    }

    coex_status_t coex;
    thread_br_get_coex_status(&coex);
    char coex_buf[160];
    if (coex.known && coex.too_close) {
        snprintf(coex_buf, sizeof(coex_buf),
                 "{\"zigbee_channel\":%u,\"thread_channel\":%u,"
                 "\"separation\":%u,\"limit\":%d}",
                 coex.zigbee_channel, coex.thread_channel, coex.separation,
                 COEX_TOO_CLOSE_MAX);
    } else {
        strlcpy(coex_buf, "null", sizeof(coex_buf));
    }

    /* WiFi 回传诊断：信道、是否 DFS、断线次数与时长（network_mgr.h 的 wifi_link_stats_t）。
       没连上时信道和 DFS 报 null —— 0 和 false 都会被误读成"在 0 信道 / 不在 DFS 上"。
       一次断线都没恢复过时，两个时长报 null，而不是 0（0 秒是一个真实可能的值）。 */
    wifi_link_stats_t wls;
    network_mgr_wifi_link_stats(&wls);
    char wch_buf[8] = "null", wdfs_buf[8] = "null", wlast_buf[12] = "null", wlong_buf[12] = "null";
    if (ns.wifi_connected && ns.wifi_channel) {
        snprintf(wch_buf, sizeof(wch_buf), "%u", ns.wifi_channel);
        strlcpy(wdfs_buf, network_mgr_wifi_channel_is_dfs(ns.wifi_channel) ? "true" : "false",
                sizeof(wdfs_buf));
    }
    if (wls.outages_done) {
        snprintf(wlast_buf, sizeof(wlast_buf), "%"PRIu32, wls.last_outage_s);
        snprintf(wlong_buf, sizeof(wlong_buf), "%"PRIu32, wls.longest_outage_s);
    }

    /* 1536 → 1792（2026-10-05 加 WiFi 诊断六个字段，约 +140 字节）。实测 1189 字节，
       最坏情况（SSID 满 32 字节且全要转义）仍在界内；httpd 栈余量负载下 3868，够。
       2026-10-06 加 svc_takeover（最长 "denied_clock_backwards"，约 +45 字节），仍在界内。
       2026-10-07 加 T9.37 服务身份四个字段（最长约 +100 字节），仍在界内。 */
    uint32_t svc_ip = network_mgr_svc_ip();   /* 网络序，同 ns.eth_ip.addr */
#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
    char buf[1920];     /* 末尾还要追加 muse_state / blew_state / muse_link（最长约 85 字节）；HA 型照旧 1792 */
#else
    char buf[1792];
#endif
    int n = snprintf(buf, sizeof(buf),
        "{"
        STATUS_PROFILE_FIELD
        STATUS_WIFI_ABSENT_FIELD
        "\"uptime\":%"PRIu32","
        "\"temperature\":%.1f,"
        "\"free_heap\":%"PRIu32","
        "\"min_free_heap\":%"PRIu32","
        "\"bridge_mode\":\"%s\","
        "\"active_interface\":\"%s\","
        "\"eth_link_up\":%s,"
        "\"eth_ip\":\"%u.%u.%u.%u\","
        "\"wifi_connected\":%s,"
        "\"wifi_ip\":\"%u.%u.%u.%u\","
        /* T9.20：网线断着时重启，开机有没有把以太网服务 IP 接管到 WiFi（含义见
           network_mgr.h 的 network_mgr_svc_takeover_state；行为正本是 Kconfig help） */
        "\"svc_takeover\":\"%s\","
        /* T9.37 服务身份：服务地址（HA / ZHA / Z2M 该连的那个）由哪个 DHCP 身份持有、此刻在哪张卡上
           （含义见 network_mgr.h 的 network_mgr_svc_identity 等；行为正本是 Kconfig BITENGINE_SVC_IDENTITY 的 help） */
        "\"svc_identity\":\"%s\","
        "\"svc_ip\":\"%u.%u.%u.%u\","
        "\"svc_holder\":\"%s\","
        "\"svc_id_mode\":\"%s\","
        "\"wifi_rssi\":%d,"
        "\"wifi_ssid\":\"%s\","
        "\"wifi_channel\":%s,"
        "\"wifi_dfs\":%s,"
        "\"wifi_disconnects\":%"PRIu32","
        "\"wifi_down_s\":%"PRIu32","
        "\"wifi_last_outage_s\":%s,"
        "\"wifi_longest_outage_s\":%s,"
        "\"zigbee_channel\":%s,"
        "\"zigbee_channel_source\":\"%s\","
        /* 2026-09-10 (T1.5): no zigbee_tx_power value any more. The radio
           power is owned by the host: Z2M/ZHA send setRadioPower on every
           start (zigpy 1.2.2 application.py:259-273, bellows 0.49.0
           application.py:172) and the bridge never does. The old field was an
           NVS preference that a "Turbo" switch rendered as "(20 dBm Turbo)" on
           a fresh board while the NCP actually ran 8 dBm. Only the ownership
           tag remains so a reader of the JSON knows where to look. */
        "\"zigbee_tx_power_source\":\"host\","
        "\"coex_warning\":%s,"
        "\"zigbee_channel_check\":%s,"
        "\"uart_rx_bytes\":%"PRIu32","
        "\"uart_tx_bytes\":%"PRIu32","
        "\"tcp_clients\":%d,"
        "\"zigbee_host_absent_s\":%s,"
        "\"mg24_alive\":%s,"
        "\"mg24_resets\":%"PRIu32","
        "\"ncp_version\":\"%s\","
        /* true = 这次开机没读 NCP，ncp_version 是上次的读数（C5 单独重启，T9.31 ②）——
           这时它证明不了网关↔MG24 的链路现在是好的，排障看 mg24_alive / tcp_clients */
        "\"ncp_version_cached\":%s,"
        "\"hostname\":\"%s\","
        "\"fw_version\":\"%s\","
        "\"www_version\":\"%s\","
        "\"chip_revision\":\"%s\","
        /* 出厂序列号：产线写进 fctry 分区的那个，贴在标签上、售后按它查。
           没写身份的板子（开发板、产线还没走到那步）报 null —— 空串会让调用方
           分不清"没有身份"和"身份是空的"。
           ⚠️ 与 mDNS TXT 里那个 serial_number 不是一回事：那个是 MAC，是 ZHA
           认设备用的，别混。 */
        "\"serial_number\":%s,"
        "\"mac\":\"%s\","
        "\"eth_mac\":\"%s\","
        "\"wifi_provision\":\"%s\","
        "\"wifi_last_error\":\"%s\","
        "\"wifi_last_reason\":%d,"
        "\"app_signed\":%s,"
        "\"ncp_signing_unlocked\":%s,"
        "\"last_reset_reason\":%d,"
        /* Thread 子节点恢复进度：重启后休眠设备要 4-6 分钟才重新入网，
           前端据此显示"设备重连中 x/y"，免得用户以为网关坏了 */
        "\"thread_children\":%u,"
        "\"thread_children_total\":%u,"
        /* v2 新增：min_free_heap 是含 8MB PSRAM 的聚合值，对判断内存健康没有
           意义（保留仅为前端兼容）。真正该看的是下面三个。 */
        "\"internal_free_heap\":%u,"
        "\"alloc_failures\":%u,"
        /* SYS 灯当前语义。之前 SYS 显示什么只有眼睛能判，于是"事件错过导致
           s_state 永久卡在 boot"（SYS 黄常亮而其余三颗全绿）无法回归测试。
           网络在线时这里必须是 normal 或 night，是 boot 就是又漏事件了。 */
        "\"led_state\":\"%s\","
        /* 供电方式。此前只有 /poe/status 有，而 poe_is_present() 零调用者 ——
           docs/01 的 status 样例本来就写着这个字段。放进 /status 才能让
           Dashboard、WS 推送、HA 集成都拿到（T2.5）。
           ⚠️ 判据只看 no_pull 那一档，理由见 poe.h：上拉那档区分不了
           "引脚悬空"和"正常无 PoE"，拿它当判据会误报硬件故障。 */
        /* 配置落盘失败累计次数。非 0 = 这台设备保存设置时出过错，用户很可能
           遇到过"改完重启又回去了"。前端在非 0 时出横幅。
           ⚠️ 只增不减（重启清零）—— 它是"有没有发生过"的证据，不是当前状态。 */
        /* 跑在哪个 OTA 槽，以及上一次 OTA 有没有被回滚（T2.7）。
           两个都是**现算**的，不存状态：
             ota_slot            esp_ota_get_running_partition()->label
             last_ota_rolled_back  有没有分区被标记成启动失败
           回滚以前完全不可见：机制是对的，但没人查过，设备默默回到旧版本，
           用户只觉得"升级没生效"。ota_slot 还能让回归脚本确认 OTA 真的换了槽。 */
        "\"ota_slot\":\"%s\","
        "\"last_ota_rolled_back\":%s,"
        "\"nvs_write_failures\":%u,"
        "\"poe_present\":%s,"
        "\"safe_mode\":%s"
        "}",
        hs ? hs->uptime_seconds : 0,
        hs ? hs->temperature : 0.0f,
        hs ? hs->free_heap : 0,
        hs ? hs->min_free_heap : 0,
        mode_str,
        iface_str,
        ns.eth_link_up ? "true" : "false",
        IP_B0(ns.eth_ip.addr), IP_B1(ns.eth_ip.addr),
        IP_B2(ns.eth_ip.addr), IP_B3(ns.eth_ip.addr),
        ns.wifi_connected ? "true" : "false",
        IP_B0(ns.wifi_ip.addr), IP_B1(ns.wifi_ip.addr),
        IP_B2(ns.wifi_ip.addr), IP_B3(ns.wifi_ip.addr),
        network_mgr_svc_takeover_state(),
        network_mgr_svc_identity(),
        IP_B0(svc_ip), IP_B1(svc_ip), IP_B2(svc_ip), IP_B3(svc_ip),
        network_mgr_svc_holder(),
        network_mgr_svc_id_mode(),
        (int)ns.wifi_rssi,
        safe_ssid,
        wch_buf, wdfs_buf, wls.disconnects, wls.down_s, wlast_buf, wlong_buf,
        zb_ch_buf,
        (zbc && zbc->channel_declared) ? "declared" : "unknown",
        coex_buf,
        zchk_buf,
        bs ? bs->uart_rx_bytes : 0,
        bs ? bs->uart_tx_bytes : 0,
        bs ? (int)bs->active_clients : 0,
        /* 没有协调器连着时 HA 里的 Zigbee 实体全不可用（网关只转发字节，
           不代答任何 EZSP 请求）。
           ⚠️ 三态，不是一个数：
             null —— **不适用**。这台网关从来没有过协调器（只用 Thread/Matter，
                     或还没装 ZHA）。这是正常状态，不能报警：用户既无法消除
                     也不该消除它。判据是 zigbee.host_ever_seen（粘性，恢复出厂才清）。
             0    —— 现在连着
             N    —— 曾经连着，已经断了 N 秒
           判定只在这一处，前端不作判断。 */
        host_absent_buf,
        (hs && hs->mg24_alive) ? "true" : "false",
        hs ? hs->mg24_total_resets : 0,
        ncp_ver,
        ncp_version_get()->from_cache ? "true" : "false",
        safe_hostname,
        app->version,
        safe_www,
        chip_rev,
        serial_buf,
        mac_str,
        eth_mac_str,
        prov,
        werr,
        wreason,
        s_app_signed ? "true" : "false",
        ncp_signing_unlocked_now() ? "true" : "false",
        hs ? (int)hs->last_reset_reason : 0,
        (unsigned)th_online, (unsigned)th_total,
        (unsigned)wd_res.internal_free,
        (unsigned)wd_res.alloc_fail_total,
        led_ctrl_state_name(led_ctrl_get_state()),
        ota_slot_label,
        ota_rolled_back ? "true" : "false",
        (unsigned)nvs_health.count,
        poe_is_present() ? "true" : "false",
        wd_safe_mode() ? "true" : "false");

#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
    /* 直连型 + Muse：把结尾的 } 换成 muse_state / blew_state 再收口（设计 10.1；只读内存，/status 被频繁轮询） */
    if (n > 0 && n < (int)sizeof(buf)) {
        const int m = muse_api_status_tail(buf + n - 1, sizeof(buf) - (size_t)(n - 1));
        n = (m < 0) ? (int)sizeof(buf) : n - 1 + m;
    }
#endif
    if (n >= (int)sizeof(buf)) {
        ESP_LOGE(TAG, "Status JSON truncated (%d >= %d)", n, (int)sizeof(buf));
        return send_error(req, 500, "status_json_overflow");
    }
    return send_json_response(req, buf);
}

#if !CONFIG_BITENGINE_NET_ETH_ONLY   /* 纯以太网剖面没有 Wi-Fi，不编也不注册（docs/15 设计 2.2） */
// ---- GET /api/v1/wifi/scan ----
static esp_err_t handle_wifi_scan(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    wifi_ap_record_t results[20];
    uint16_t count = 20;
    esp_err_t err = network_mgr_scan_wifi(results, &count);
    if (err != ESP_OK) return send_error(req, 500, "scan_failed");

    cJSON *arr = cJSON_CreateArray();
    if (!arr) return send_error(req, 500, "out_of_memory");

    for (int i = 0; i < count; i++) {
        cJSON *ap = cJSON_CreateObject();
        if (!ap) break;
        /* SSID 是周围 AP 定的任意字节（GBK 编码的中文名就不是 UTF-8）；cJSON 只转义不校验 */
        char ssid[sizeof(results[i].ssid) * 3 + 1];
        utf8_repair_copy(ssid, sizeof(ssid), (const char *)results[i].ssid, NULL);
        cJSON_AddStringToObject(ap, "ssid", ssid);
        cJSON_AddNumberToObject(ap, "rssi", results[i].rssi);
        cJSON_AddNumberToObject(ap, "channel", results[i].primary);
        cJSON_AddNumberToObject(ap, "auth", results[i].authmode);
        cJSON_AddItemToArray(arr, ap);
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!json) return send_error(req, 500, "out_of_memory");

    esp_err_t ret = send_json_response(req, json);
    free(json);
    return ret;
}

// ---- POST /api/v1/wifi/config ----
static esp_err_t handle_wifi_config(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[256];
    int len = recv_body(req, body, sizeof(body));
    if (len <= 0) return send_error(req, 400, "empty_body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");

    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass = cJSON_GetObjectItem(root, "password");
    if (!ssid || !cJSON_IsString(ssid) || strlen(ssid->valuestring) == 0 ||
        strlen(ssid->valuestring) > 32) {
        cJSON_Delete(root);
        return send_error(req, 400, "invalid_ssid");
    }

    const char *pw = (pass && cJSON_IsString(pass)) ? pass->valuestring : "";
    if (strlen(pw) > 64 || (pw[0] != '\0' && strlen(pw) < 8)) {
        /* WPA2 passwords are 8..64 chars; silently truncating would store
           credentials that can never authenticate */
        cJSON_Delete(root);
        return send_error(req, 400, "invalid_password_length");
    }
    /* T9.33：写进配置的字符串必须是合法 UTF-8 —— **SSID 除外**（2026-10-07 定）。
       SSID 是 AP 定的任意字节（GBK 编码的中文名就不是 UTF-8），拒了就连不上那台 AP；
       导出时它另存成 ssid_hex，备份文件照样合法（config_export.c）。
       WiFi 密码不例外：WPA 口令按规范只是可打印 ASCII，非 UTF-8 的口令没有正当来源。 */
    if (!utf8_valid(pw)) {
        cJSON_Delete(root);
        return send_error(req, 400, "invalid_utf8");
    }
    esp_err_t err = network_mgr_set_wifi(ssid->valuestring, pw);
    cJSON_Delete(root);

    if (err != ESP_OK) return send_error(req, 500, "wifi_config_failed");
    return send_json_response(req, "{\"status\":\"ok\",\"message\":\"connecting\"}");
}

#endif /* !CONFIG_BITENGINE_NET_ETH_ONLY */

// ---- GET /api/v1/ethernet/status ----
static esp_err_t handle_eth_status(httpd_req_t *req)
{
    AUTH_REQUIRED(req);

    network_status_t ns;
    network_mgr_get_status(&ns);

    char buf[256];
    extern const char *eth_mgr_get_diag(void);
    snprintf(buf, sizeof(buf),
        "{\"link_up\":%s,\"has_ip\":%s,\"ip\":\"%u.%u.%u.%u\","
        "\"gateway\":\"%u.%u.%u.%u\",\"netmask\":\"%u.%u.%u.%u\","
        "\"diag\":\"%s\"}",
        ns.eth_link_up ? "true" : "false",
        ns.eth_has_ip ? "true" : "false",
        IP_B0(ns.eth_ip.addr), IP_B1(ns.eth_ip.addr),
        IP_B2(ns.eth_ip.addr), IP_B3(ns.eth_ip.addr),
        IP_B0(ns.eth_gateway.addr), IP_B1(ns.eth_gateway.addr),
        IP_B2(ns.eth_gateway.addr), IP_B3(ns.eth_gateway.addr),
        IP_B0(ns.eth_netmask.addr), IP_B1(ns.eth_netmask.addr),
        IP_B2(ns.eth_netmask.addr), IP_B3(ns.eth_netmask.addr),
        eth_mgr_get_diag());
    return send_json_response(req, buf);
}

// ---- POST /api/v1/ethernet/config ----
static esp_err_t handle_eth_config(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[256];
    int len = recv_body(req, body, sizeof(body));
    if (len <= 0) return send_error(req, 400, "empty_body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");

    cJSON *dhcp = cJSON_GetObjectItem(root, "dhcp");
    eth_config_t eth = *config_mgr_get_eth();

    if (dhcp && cJSON_IsBool(dhcp)) {
        eth.dhcp = cJSON_IsTrue(dhcp);
    }
    // Static IP fields if present — frontend sends dotted strings,
    // API clients may send numeric form; accept both
    parse_ip_field(cJSON_GetObjectItem(root, "static_ip"), &eth.static_ip);
    parse_ip_field(cJSON_GetObjectItem(root, "static_gateway"), &eth.static_gateway);
    parse_ip_field(cJSON_GetObjectItem(root, "static_netmask"), &eth.static_netmask);
    /* static_dns 此前从没被解析过，而 apply_static_ip_config()（network_mgr.c:443）
       确实会下发它 —— 结果是以太网静态 IP 配好了却没有 DNS，域名全不通。 */
    parse_ip_field(cJSON_GetObjectItem(root, "static_dns"), &eth.static_dns);

    cJSON_Delete(root);

    if (!eth.dhcp && eth.static_ip == 0) {
        return send_error(req, 400, "static_ip_required");
    }

    /* 唯一生效点是 network_mgr_init() 里的 apply_static_ip_config()，即**下次重启**。
       此前回包只有 "ok"，用户改完以为立刻生效，实际要等重启 —— 同 /wifi/static 的口径。
       返回值也必须检查：NVS 写失败时不能报成功（CONFIG_EVENT_NVS_ERROR 现在由 led_ctrl
       订阅红闪 5s、/status 计 nvs_write_failures —— T2.6；但 HTTP 这一层仍要自己报 500）。 */
    esp_err_t err = config_mgr_set_eth(&eth);
    if (err != ESP_OK) return send_error(req, 500, "save_failed");

    return send_json_response(req,
        eth.dhcp ? "{\"status\":\"ok\",\"dhcp\":true,\"takes_effect\":\"reboot\"}"
                 : "{\"status\":\"ok\",\"dhcp\":false,\"takes_effect\":\"reboot\"}");
}

#if CONFIG_BITENGINE_ENABLE_ZHUB
/* /zigbee/status 末尾的 hub 对象（直连型；CHECKLIST Z18 的第一份实读值）：zhub 的状态快照。buf 里已有以 "]," 结尾的前半截，
   这里接上 "\"hub\":{…}}"。放不下返回 false（调用方回 500，不发半截 JSON）。全是数字、固定词表与十六进制，不需要转义；
   扩展 PAN ID 不是秘密（信标里明文广播），网络密钥从不经过 zhub 的快照（Z17） */
static bool zigbee_hub_json(char *buf, size_t cap)
{
    zhub_status_t z;
    zhub_get_status(&z);
    const size_t n = strlen(buf);
    if (n >= cap) return false;
    /* 读不到的项给 null，不给 0（0 是合法值：信道 0 不存在，但 PAN 0、功率 0 存在） */
    char st_type[8] = "null", st_ver[8] = "null", ch[8] = "null", pan[8] = "null", txp[8] = "null", nt[8] = "null", ext[20] = "null";
    if (z.have_version) {
        snprintf(st_type, sizeof(st_type), "%u", (unsigned)z.stack_type);
        snprintf(st_ver, sizeof(st_ver), "%u", (unsigned)z.stack_version);
    }
    if (z.have_params) {
        snprintf(ch, sizeof(ch), "%u", (unsigned)z.channel);
        snprintf(pan, sizeof(pan), "%u", (unsigned)z.pan_id);
        snprintf(txp, sizeof(txp), "%d", (int)z.tx_power);
        snprintf(nt, sizeof(nt), "%u", (unsigned)z.node_type);
        ext[0] = '"';
        for (int i = 0; i < 8; i++) snprintf(ext + 1 + 2 * i, 3, "%02X", z.ext_pan_id[7 - i]);   /* 惯用写法：高字节在前 */
        ext[17] = '"';
        ext[18] = '\0';
    }
    char eui[20] = "null";
    if (z.have_eui64) {
        eui[0] = '"';
        for (int i = 0; i < 8; i++) snprintf(eui + 1 + 2 * i, 3, "%02X", z.eui64[7 - i]);           /* 同上：高字节在前 */
        eui[17] = '"';
        eui[18] = '\0';
    }
    /* 最近一条设备消息的来源（载荷不出网关）；没收到过给 null */
    char last_rx[128] = "null";
    if (z.have_last_rx)
        snprintf(last_rx, sizeof(last_rx), "{\"node\":%u,\"profile\":%u,\"cluster\":%u,\"ep\":%u,\"lqi\":%u,\"rssi\":%d,\"age_ms\":%"PRIu32"}",
                 (unsigned)z.last_rx_node, (unsigned)z.last_rx_profile, (unsigned)z.last_rx_cluster, (unsigned)z.last_rx_ep,
                 (unsigned)z.last_rx_lqi, (int)z.last_rx_rssi, z.last_rx_age_ms);
    /* 入网窗口与入网观察（第六步）：最近一次入网的 EUI64 高字节在前，同 eui64 */
    char win[288];                                 /* 字段全取上限约 245 B */
    const int wn = snprintf(win, sizeof(win), "{\"state\":\"%s\",\"left_s\":%"PRIu32",\"seconds\":%u,\"quota_left_s\":%"PRIu32",\"ncp_open\":%s,\"opens\":%"PRIu32",\"fails\":%"PRIu32","
             "\"closes\":%"PRIu32",\"expired\":%"PRIu32",\"close_retries\":%"PRIu32",\"skipped\":%"PRIu32",\"last_err\":%"PRIu32"}",
             z.win_state ? z.win_state : "closed", z.win_left_s, (unsigned)z.win_s, z.win_quota_left_s, z.ncp_open ? "true" : "false", z.win_opens, z.win_open_fails,
             z.win_closes, z.win_expired, z.win_close_retries, z.win_skipped, z.win_last_err);
    if (wn < 0 || (size_t)wn >= sizeof(win)) return false;   /* 拼不下就回 500，别发半截 JSON */
    char joins[256];
    char lj[128] = "null";
    if (z.have_last_join) {
        char je[17];
        for (int i = 0; i < 8; i++) snprintf(je + 2 * i, 3, "%02X", z.last_join_eui64[7 - i]);
        snprintf(lj, sizeof(lj), "{\"node\":%u,\"ieee\":\"%s\",\"status\":%u,\"decision\":%u,\"parent\":%u,\"age_ms\":%"PRIu32"}",
                 (unsigned)z.last_join_node, je, (unsigned)z.last_join_status, (unsigned)z.last_join_decision, (unsigned)z.last_join_parent, z.last_join_age_ms);
    }
    const int jn = snprintf(joins, sizeof(joins), "{\"joined\":%"PRIu32",\"rejoined\":%"PRIu32",\"left\":%"PRIu32",\"denied\":%"PRIu32",\"annces\":%"PRIu32","
             "\"bad\":%"PRIu32",\"last\":%s}", z.joined, z.rejoined, z.left, z.denied, z.annces, z.tc_bad, lj);
    if (jn < 0 || (size_t)jn >= sizeof(joins)) return false;
    /* 设备层（第七步 a）：设备表条数与被动学习的计数（上板看现网设备有没有被认出来、表满了没有） */
    char devs[1536];                               /* 字段全取上限约 1400 B（第十一步 av_*、第十二步 ctlq_*；M5 第四步加 cie_* / enroll_* / iv_send_fail，第五步 bind_* / report_* / cfg_skipped，第六步 batt_reads，第七步 ctl_*，第八步 tuya_*，第九步 ctl_tuya，第十步 lumi_* / learned；iv_retries） */
    const zh_dev_stats_t *ds = &z.dev_st;
    const int dn = snprintf(devs, sizeof(devs), "{\"count\":%u,\"added\":%"PRIu32",\"updated\":%"PRIu32",\"left\":%"PRIu32",\"back\":%"PRIu32","
             "\"nwk_changes\":%"PRIu32",\"full\":%"PRIu32",\"unknown_rx\":%"PRIu32",\"lookups\":%"PRIu32",\"lookup_hits\":%"PRIu32","
             "\"ieee_reqs\":%"PRIu32",\"ieee_rsps\":%"PRIu32",\"learn_fails\":%"PRIu32",\"send_busy\":%"PRIu32",\"bad_eui\":%"PRIu32","
             "\"bad_nwk\":%"PRIu32",\"deferred\":%"PRIu32",\"reserved\":%"PRIu32","
             "\"iv_reqs\":%"PRIu32",\"iv_timeouts\":%"PRIu32",\"iv_done\":%"PRIu32",\"iv_failed\":%"PRIu32",\"iv_bad\":%"PRIu32","
             "\"reports\":%"PRIu32",\"report_bad\":%"PRIu32",\"enroll_reqs\":%"PRIu32",\"enroll_rsps\":%"PRIu32","
             "\"cie_ok\":%"PRIu32",\"cie_fail\":%"PRIu32",\"cie_skipped\":%"PRIu32",\"enroll_ok\":%"PRIu32",\"enroll_unconfirmed\":%"PRIu32","
             "\"cie_resumes\":%"PRIu32",\"iv_send_fail\":%"PRIu32",\"bind_ok\":%"PRIu32",\"bind_fail\":%"PRIu32",\"report_ok\":%"PRIu32","
             "\"report_fail\":%"PRIu32",\"cfg_skipped\":%"PRIu32",\"batt_reads\":%"PRIu32",\"ctl_sent\":%"PRIu32",\"ctl_confirmed\":%"PRIu32","
             "\"ctl_delivered\":%"PRIu32",\"ctl_failed\":%"PRIu32",\"tuya_frames\":%"PRIu32",\"tuya_unknown\":%"PRIu32",\"tuya_unmapped\":%"PRIu32","
             "\"tuya_hellos\":%"PRIu32",\"ctl_tuya\":%"PRIu32",\"lumi_frames\":%"PRIu32",\"lumi_unknown\":%"PRIu32",\"learned\":%"PRIu32","
             "\"av_pings\":%"PRIu32",\"av_offline\":%"PRIu32",\"av_online\":%"PRIu32",\"av_quiet\":%"PRIu32",\"ctlq_queued\":%"PRIu32",\"ctlq_sent\":%"PRIu32","
             "\"ctlq_delivered\":%"PRIu32",\"ctlq_expired\":%"PRIu32",\"iv_retries\":%"PRIu32"}",
             z.devices, ds->added, ds->updated, ds->left, ds->back, ds->nwk_changes, ds->full, ds->unknown_rx, ds->lookups, ds->lookup_hits,
             ds->ieee_reqs, ds->ieee_rsps, ds->learn_fails, ds->send_busy, ds->bad_eui, ds->bad_nwk, ds->deferred, ds->reserved,
             ds->iv_reqs, ds->iv_timeouts, ds->iv_done, ds->iv_failed, ds->iv_bad, ds->reports, ds->report_bad, ds->enroll_reqs, ds->enroll_rsps,
             ds->cie_ok, ds->cie_fail, ds->cie_skipped, ds->enroll_ok, ds->enroll_unconfirmed, ds->cie_resumes, ds->iv_send_fail,
             ds->bind_ok, ds->bind_fail, ds->report_ok, ds->report_fail, ds->cfg_skipped, ds->batt_reads,
             ds->ctl_sent, ds->ctl_confirmed, ds->ctl_delivered, ds->ctl_failed, ds->tuya_frames, ds->tuya_unknown, ds->tuya_unmapped, ds->tuya_hellos, ds->ctl_tuya, ds->lumi_frames, ds->lumi_unknown, ds->learned, ds->av_pings, ds->av_offline, ds->av_online, ds->av_quiet, ds->ctlq_queued, ds->ctlq_sent, ds->ctlq_delivered, ds->ctlq_expired, ds->iv_retries);
    if (dn < 0 || (size_t)dn >= sizeof(devs)) return false;
    /* 设备元数据（M5 第二步，两轮审计：只读页本板没串口看不到）：pages_ro 第 i 位 = 第 i 页这次开机只读（新名字建不了，要重启） */
    hub_meta_status_t ms;
    hub_meta_get_status(&ms);
    char meta[192];                                /* 字段全取上限约 150 B */
    const int mn = snprintf(meta, sizeof(meta), "{\"ready\":%s,\"pages_ro\":%u,\"used\":%u,\"updates\":%"PRIu32",\"update_fail\":%"PRIu32","
             "\"lookup_busy\":%"PRIu32",\"dropped\":%"PRIu32"}", ms.ready ? "true" : "false", (unsigned)ms.pages_ro, (unsigned)ms.used,
             ms.updates, ms.update_fail, ms.lookup_busy, ms.dropped);
    if (mn < 0 || (size_t)mn >= sizeof(meta)) return false;
    /* 主动发现（M6 第一步，设计 8.12 ④）：每次开机一轮。上板看它爬没爬、回环（发给协调器自己）通不通、建了几条；
       xpan 等于 entries = 扩展 PAN 的字节序对不上（zh_dev.h）。字段全取上限约 380 B（DISC_CAP 400）*/
    static const char *const k_disc_state[] = { "idle", "waiting", "running", "done" };
    enum { DISC_CAP = 400 };
    char *disc = heap_caps_malloc(DISC_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   /* 不放 httpd 栈：这个函数的局部量已经很大（审计 ⚠️） */
    if (!disc) return false;
    const int qn = snprintf(disc, DISC_CAP, "{\"state\":\"%s\",\"routers_queued\":%u,\"routers_done\":%u,\"reqs\":%"PRIu32",\"rsps\":%"PRIu32","
             "\"timeouts\":%"PRIu32",\"send_fail\":%"PRIu32",\"routers\":%"PRIu32",\"unsupported\":%"PRIu32",\"gave_up\":%"PRIu32","
             "\"entries\":%"PRIu32",\"added\":%"PRIu32",\"known\":%"PRIu32",\"conflict\":%"PRIu32",\"xpan\":%"PRIu32",\"skipped\":%"PRIu32","
             "\"bad\":%"PRIu32",\"rounds\":%"PRIu32",\"q_full\":%"PRIu32"}",
             z.disc_state < sizeof(k_disc_state) / sizeof(k_disc_state[0]) ? k_disc_state[z.disc_state] : "unknown",
             (unsigned)z.disc_q_n, (unsigned)z.disc_q_i, ds->disc_reqs, ds->disc_rsps, ds->disc_timeouts, ds->disc_send_fail, ds->disc_routers,
             ds->disc_unsupported, ds->disc_gave_up, ds->disc_entries, ds->disc_added, ds->disc_known, ds->disc_conflict, ds->disc_xpan,
             ds->disc_skipped, ds->disc_bad, ds->disc_rounds, ds->disc_q_full);
    if (qn < 0 || (size_t)qn >= DISC_CAP) {
        heap_caps_free(disc);
        return false;
    }
    /* 最近一条解不开的设备消息（上板定位解码问题用）：错误码、帧控制、参数长度、参数前 31 字节（消息头，不到载荷） */
    char bad[128] = "null";
    if (z.incoming_bad) {
        char hx[2 * sizeof(z.bad_head) + 1] = "";
        for (unsigned i = 0; i < z.bad_head_n && i < sizeof(z.bad_head); i++) snprintf(hx + 2 * i, 3, "%02X", z.bad_head[i]);
        snprintf(bad, sizeof(bad), "{\"err\":%d,\"fc\":%u,\"fc_hi\":%u,\"n\":%u,\"head\":\"%s\"}", (int)z.bad_err, (unsigned)z.bad_fc,
                 (unsigned)z.bad_fc_hi, (unsigned)z.bad_n, hx);
    }
    const int w = snprintf(buf + n, cap - n,
        "\"hub\":{\"started\":%s,\"uart\":%s,\"suspended\":%s,\"state\":\"%s\",\"net\":\"%s\","
        "\"fails\":%"PRIu32",\"failing\":%s,\"last_fail\":\"%s\",\"last_fail_code\":%"PRIu32",\"net_status\":%"PRIu32","
        "\"stack_type\":%s,\"stack_version\":%s,"
        "\"channel\":%s,\"pan_id\":%s,\"ext_pan_id\":%s,\"tx_power\":%s,\"node_type\":%s,"
        "\"eui64\":%s,\"concentrator\":%s,\"up_bad\":%"PRIu32",\"init_opt_rejects\":%"PRIu32",\"incoming\":%"PRIu32",\"incoming_bad\":%"PRIu32",\"incoming_bad_last\":%s,\"last_rx\":%s,\"join_window\":%s,\"joins\":%s,\"devices\":%s,\"meta\":%s,\"discovery\":%s,"
        "\"evidence\":%s,\"evidence_age_ms\":%"PRIu32","
        "\"resets\":%"PRIu32",\"readies\":%"PRIu32",\"net_inits\":%"PRIu32",\"callbacks\":%"PRIu32",\"restarts\":%"PRIu32","
        "\"ash\":{\"tx\":%"PRIu32",\"rx\":%"PRIu32",\"retx\":%"PRIu32",\"crc\":%"PRIu32",\"ack_timeouts\":%"PRIu32","
        "\"ncp_resets\":%"PRIu32",\"ncp_errors\":%"PRIu32"},"
        "\"ses\":{\"timeouts\":%"PRIu32",\"echoes\":%"PRIu32",\"rejected\":%"PRIu32",\"overflow\":%"PRIu32","
        "\"lost_callbacks\":%"PRIu32",\"keepalives\":%"PRIu32"},"
        "\"uart_tx_short\":%"PRIu32"}}",
        z.started ? "true" : "false", z.holding_uart ? "true" : "false", z.suspended ? "true" : "false",
        z.state ? z.state : "idle", z.net ? z.net : "unknown",
        z.fails, z.failing ? "true" : "false", z.last_fail ? z.last_fail : "none", z.last_fail_code, z.last_net_status,
        st_type, st_ver,
        ch, pan, ext, txp, nt,
        eui, z.concentrator ? "true" : "false", z.up_bad, z.init_opt_rejects, z.incoming, z.incoming_bad, bad, last_rx, win, joins, devs, meta, disc,
        z.have_evidence ? "true" : "false", z.evidence_age_ms,
        z.resets, z.readies, z.net_inits, z.callbacks, z.restarts,
        z.ash_tx_frames, z.ash_rx_frames, z.ash_retx, z.ash_crc, z.ash_ack_timeouts, z.ash_ncp_resets, z.ash_ncp_errors,
        z.ses_timeouts, z.ses_echoes, z.ses_rejected, z.ses_overflow, z.ses_lost_callbacks, z.ses_keepalives,
        z.uart_tx_short);
    heap_caps_free(disc);
    return w > 0 && (size_t)w < cap - n;
}
#endif

// ---- GET /api/v1/zigbee/status ----
static esp_err_t handle_zigbee_status(httpd_req_t *req)
{
    AUTH_REQUIRED(req);

    const zigbee_config_t *zc = config_mgr_get_zigbee();
    const health_status_t *hs = watchdog_get_status();
    const bridge_stats_t *bs = ezsp_bridge_get_stats();

    tcp_client_info_t clients[MAX_TCP_CLIENTS];
    int client_count = 0;
    ezsp_bridge_get_clients(clients, &client_count);

    /* Two different identities, and they answer different questions:
       ncp_* comes from the chip (which EmberZNet release is running), image_*
       from NVS (which .gbl this gateway flashed — the only value that maps to
       a line in mg24/release/SHA256SUMS). A unit flashed only on the
       production line has the former and not the latter. */
    const ncp_version_t *nv = ncp_version_get();
    const mg24_image_record_t *rec = mg24_image_record_get();
    char ncp_ver[40];
    ncp_version_format(ncp_ver, sizeof(ncp_ver));
    char ncp_bs[sizeof(nv->build_string)];
    json_sanitize_copy(ncp_bs, nv->build_string, sizeof(ncp_bs));

    /* We only ever write lowercase hex here, but it comes back out of NVS and
       goes straight into JSON — same rule as www_version above: anything read
       from writable storage is sanitized, no exceptions for "we wrote it". */
    char safe_sha[65];
    json_sanitize_copy(safe_sha, rec->valid ? rec->sha256 : "", sizeof(safe_sha));

    /* configured_channel = NVS 里的用户声明值，不是实读（读不到，理由见
       ncp_version.h）。没有 configured_tx_power：功率归 Z2M/ZHA（T1.5）。
       channel_source: "declared" 用户声明 | "unknown" 未声明。
       没有 "实测" 这一档 —— 读不到，理由见 ncp_version.h。 */
    const char *ch_source = zc->channel_declared ? "declared" : "unknown";
    char zb_ch_buf[8];
    if (zc->channel_declared) {
        snprintf(zb_ch_buf, sizeof(zb_ch_buf), "%u", zc->channel);
    } else {
        strlcpy(zb_ch_buf, "null", sizeof(zb_ch_buf));
    }

    /* Thread 实际信道一并给出：channel_source=="not_joined" 时前端要据此告诉
       用户"把 Z2M 设成离它 >COEX_TOO_CLOSE_MAX 的信道"。那是冲突发生**之前**
       唯一能介入的时刻 —— 信道由 Z2M/ZHA 的配置决定，固件插不进那个决定。 */
    uint8_t th_ch = 0;
    char th_ch_buf[8];
    /* 可用信道由固件算好下发，前端不再自己写一遍距离判断 —— 那会变成第二份
       规则，边界改了不会同步。计算在 thread_br_list_safe_zigbee_channels()，
       和挑信道、共存告警共用同一个 COEX_TOO_CLOSE_MAX。 */
    char advice_buf[128] = "null";
    if (thread_br_get_active_channel(&th_ch) == ESP_OK) {
        snprintf(th_ch_buf, sizeof(th_ch_buf), "%u", th_ch);

        uint8_t ok[16];
        uint8_t n_ok = thread_br_list_safe_zigbee_channels(th_ch, ok, sizeof(ok));
        if (n_ok > 0) {
            int p = snprintf(advice_buf, sizeof(advice_buf),
                             "{\"avoid_lo\":%d,\"avoid_hi\":%d,\"ok\":[",
                             th_ch > 11 + COEX_TOO_CLOSE_MAX
                                 ? th_ch - COEX_TOO_CLOSE_MAX : 11,
                             th_ch + COEX_TOO_CLOSE_MAX < 26
                                 ? th_ch + COEX_TOO_CLOSE_MAX : 26);
            for (uint8_t i = 0; i < n_ok && p < (int)sizeof(advice_buf); i++) {
                p += snprintf(advice_buf + p, sizeof(advice_buf) - p,
                              "%s%u", i ? "," : "", ok[i]);
            }
            if (p < (int)sizeof(advice_buf) - 2) {
                snprintf(advice_buf + p, sizeof(advice_buf) - p, "]}");
            } else {
                strlcpy(advice_buf, "null", sizeof(advice_buf));  /* 拼不下就别发半截 */
            }
        }
    } else {
        strlcpy(th_ch_buf, "null", sizeof(th_ch_buf));
    }

    /* 1088 → 1600（2026-09-11 T3.4）。不是拍脑袋：加 10 个计数器之前实测回包
       811 字节（带 1 个客户端），而这些计数器全取 uint32 上限要再占 280 字节，
       811 + 280 = 1091 就已经越过 1088 了。
       ⚠️ 这个缓冲溢出时回 500 —— 不是静默，但 Zigbee 页会整页打不开。
       再加字段前先照这个办法量一次：`curl .../zigbee/status | wc -c`。 */
    /* 地区/功率快照（docs/14）。约 170 字节，1091 + 170 仍在 1600 内；再加字段先量。 */
    char txp_buf[224];
    txp_json(txp_buf, sizeof(txp_buf));

#if CONFIG_BITENGINE_ENABLE_ZHUB
    /* 直连型多一个 hub 对象：按格式串估，hub 字段全取上限约 1580 B（第四步约 820 B；第五步 EUI64 / 集中器 / 收到的消息约 245 B、解不开的那条
       约 115 B；第六步入网窗口与入网观察约 400 B），整包极端约 3070 B。第六步起改在 PSRAM 上分配：放栈上再加大，这个处理函数连同
       zigbee_hub_json 的局部量要吃掉 httpd 8 KB 栈的大半（webui_server.c）。溢出回 500，整个 Zigbee 页打不开。直连型没有 TCP 客户端，clients 恒为空。
       之后又加了设备层计数（第七步 a / b、M5 第一步，约 640 B）与元数据（M5 第二步，约 150 B）：极端约 3.9 KB，4096 太贴边 → 5120；
       M5 第四步再加约 190 B（CIE / 登记计数）、第五步约 120 B（绑定 / 上报计数）：按格式串算的上界约 4.9 KB → 6144；
       第六到十一步又加约 400 B（batt_reads、ctl_*、tuya_*、ctl_tuya、lumi_* / learned、av_*），上界约 5.4 KB，仍在 6144 内；
       M6 第一步加主动发现约 330 B（hub.discovery），上界约 5.8 KB 太贴边 → 7168（PSRAM，没有栈的代价） */
    enum { ZS_BUF = 7168 };
    char *buf = heap_caps_malloc(ZS_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return send_error(req, 500, "no_memory");
#define ZS_CAP ((size_t)ZS_BUF)
#else
    char buf[1600];
#define ZS_CAP sizeof(buf)
#endif
    int n = snprintf(buf, ZS_CAP,
        "{\"tx_power\":%s,\"channel\":%s,"
        "\"channel_source\":\"%s\","
        "\"thread_channel\":%s,\"coex_limit\":%d,"
        "\"channel_advice\":%s,"
        "\"configured_channel\":%d,"
        "\"mg24_alive\":%d,\"mg24_resets\":%"PRIu32","
        "\"uart_rx\":%"PRIu32",\"uart_tx\":%"PRIu32","
        /* bridge_stats 的其余计数（T3.4）。此前 14 个字段只出了 uart_rx/tx 两个。
           tcp_client_drops 只算"TX ring buffer 满被踢"（tcp_server.c 唯一 ++ 处）：
           非 0 说明是桥这边把慢客户端踢了；为 0 不排除桥侧别的断开（NCP 自发
           RSTACK、同 IP 重连挤掉、send 出错、手动断开 —— 这些只进 tcp_disconnects）。
           uart_rx_errors / uart_tx_timeouts 指向 MG24 那条链路（流控没成对、
           波特率不一致）。ncp_resets_detected 是被动扫到的 RSTACK 数，上位机每次
           握手（ASH RST）引出的那个也算，且认不出 ASH ERROR 帧 —— 不能直接当
           "NCP 自己重启"的次数，自发的要看日志 "unsolicited NCP reset"。 */
        "\"uart_rx_errors\":%"PRIu32",\"uart_tx_timeouts\":%"PRIu32","
        "\"tcp_rx_bytes\":%"PRIu32",\"tcp_tx_bytes\":%"PRIu32","
        "\"tcp_connects\":%"PRIu32",\"tcp_disconnects\":%"PRIu32","
        "\"tcp_client_drops\":%"PRIu32","
        "\"usb_rx_bytes\":%"PRIu32",\"usb_tx_bytes\":%"PRIu32","
        "\"ncp_resets_detected\":%"PRIu32","
        "\"ncp_version\":\"%s\",\"ncp_build\":%u,"
        "\"ncp_ezsp_protocol\":%u,\"ncp_version_known\":%s,\"ncp_version_cached\":%s,"
        /* XNCP 构建串：没有指纹时云端拿它比（T9.19 ⑦）；来自芯片，进 JSON 前过一遍 json_sanitize_copy */
        "\"ncp_build_string\":\"%s\","
        /* NCP 表容量 —— Zigbee 能接多少设备的约束全在 MG24 侧，C5 这边
           一个字节都看不到。这是开机独占 UART 时读的上电值；10-06 实测上位机多数表只能调小。
           例外是直连子设备：Z2M 每次握手都写回 stack_config.json 的值（默认 32，不用用户动手），
           所以接 Z2M 时这里的 max_end_device_children 不是运行值（mg24/README.md「关于设备数」）。 */
        "\"ncp_tables\":{\"known\":%s,\"max_end_device_children\":%u,"
        "\"neighbor_table\":%u,\"address_table\":%u,\"source_route_table\":%u,"
        "\"key_table\":%u,\"route_table\":%u,\"aps_unicast_msgs\":%u,"
        "\"packet_buffer_heap\":%u},"
        "\"image_sha256\":\"%s\",\"image_size\":%"PRIu32","
        "\"image_flashed_at\":%lld,\"image_source\":\"%s\","
        /* T9.25：这块芯片的 NVM3 缓存底线 —— 比它小的 NCP 一律拒（mg24_image_record.h） */
        "\"ncp_nvm3_floor\":%d,"
        "\"restart_resets_board\":%s,"
        "\"clients\":[",
        txp_buf,
        zb_ch_buf,
        ch_source,
        th_ch_buf, COEX_TOO_CLOSE_MAX,
        advice_buf,
        zc->channel,
        hs ? hs->mg24_alive : 0,
        hs ? hs->mg24_total_resets : 0,
        bs ? bs->uart_rx_bytes : 0,
        bs ? bs->uart_tx_bytes : 0,
        bs ? bs->uart_rx_errors : 0,
        bs ? bs->uart_tx_timeouts : 0,
        bs ? bs->tcp_rx_bytes : 0,
        bs ? bs->tcp_tx_bytes : 0,
        bs ? bs->tcp_connects : 0,
        bs ? bs->tcp_disconnects : 0,
        bs ? bs->tcp_client_drops : 0,
        bs ? bs->usb_rx_bytes : 0,
        bs ? bs->usb_tx_bytes : 0,
        bs ? bs->ncp_resets_detected : 0,
        ncp_ver,
        (unsigned)(nv->detail_valid ? nv->build : 0),
        (unsigned)nv->ezsp_protocol,
        nv->valid ? "true" : "false",
        nv->from_cache ? "true" : "false",
        ncp_bs,
        nv->cfg.valid ? "true" : "false", (unsigned)nv->cfg.max_end_device_children,
        (unsigned)nv->cfg.neighbor_table, (unsigned)nv->cfg.address_table,
        (unsigned)nv->cfg.source_route_table, (unsigned)nv->cfg.key_table,
        (unsigned)nv->cfg.route_table, (unsigned)nv->cfg.aps_unicast_msgs,
        (unsigned)nv->cfg.packet_buffer_heap,
        safe_sha,
        rec->size,
        (long long)rec->flashed_at,
        rec->valid ? "gateway_ota" : "factory",
        (int)mg24_image_record_nvm3_floor(),
        /* 让 UI 的警告跟着固件的实际行为走。写死文案的代价刚踩过：Zigbee 页
           一直显示"Connected devices may briefly disconnect"，而本板上这一下
           是整机重启。 */
#if CONFIG_BITENGINE_ENABLE_ZHUB
        "false");   /* 直连型的 /zigbee/restart 不碰 IO23：zhub 软重启（ASH RST + 重新 networkInit，见 handle_zigbee_restart） */
#else
        mg24_reset_takes_board_down() ? "true" : "false");
#endif

    for (int i = 0; i < client_count; i++) {
        /* index 是 tcp_server 里的**槽位号**，不是本数组的下标（这个数组是压缩过的
           快照，空槽位不出现）。POST /zigbee/clients/disconnect 按它寻址，
           照数组下标传会踢错人。 */
        n += snprintf(buf + n, ZS_CAP - n,
            "%s{\"index\":%d,\"ip\":\"%s\",\"uptime\":%"PRIu32",\"rx\":%"PRIu32",\"tx\":%"PRIu32"}",
            i > 0 ? "," : "",
            clients[i].index,
            clients[i].ip_str,
            (uint32_t)(esp_timer_get_time() / 1000000) - clients[i].connected_at,
            clients[i].bytes_rx,
            clients[i].bytes_tx);
    }
#if CONFIG_BITENGINE_ENABLE_ZHUB
    strlcat(buf, "],", ZS_CAP);
    if (!zigbee_hub_json(buf, ZS_CAP)) {
        heap_caps_free(buf);
        return send_error(req, 500, "response_too_large");
    }
    const esp_err_t sent = send_json_response(req, buf);
    heap_caps_free(buf);
    return sent;
#else
    strlcat(buf, "]}", ZS_CAP);
    return send_json_response(req, buf);
#endif
}
#undef ZS_CAP

// ---- POST /api/v1/zigbee/restart ----
static esp_err_t handle_zigbee_restart(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
#if CONFIG_BITENGINE_ENABLE_ZHUB
    /* 直连型绝不走 watchdog_reset_mg24()（IO23 的毛刺拖垮 EN；网线常在线，芯片级复位还会撞 IO28）。
       设计 2.8 / CHECKLIST Z48：zhub 软重启 —— ASH RST、重新握手、networkInit（NCP 里的网络不受影响），只投递请求、不等结果；
       进度看 /zigbee/status 的 hub.state。zhub 没在持有 UART（没起、或 UART 正被 MG24 升级 / 基准测试占着）时回 409 */
    const esp_err_t zr = zhub_request_restart();
    if (zr == ESP_ERR_NOT_ALLOWED) return send_error(req, 429, "cooldown_active");   /* 它跳过退避：限个最短间隔（zhub.c 的 ZHUB_RESTART_MIN_MS） */
    if (zr != ESP_OK) return send_error(req, 409, "zhub_not_running");
    return send_json_response(req, "{\"status\":\"ok\"}");
#else
    esp_err_t err = watchdog_reset_mg24();
    if (err == ESP_ERR_NOT_ALLOWED)
        return send_error(req, 429, "cooldown_active");
    if (err != ESP_OK)
        return send_error(req, 500, "reset_failed");
    return send_json_response(req, "{\"status\":\"ok\"}");
#endif
}

#if CONFIG_BITENGINE_ENABLE_ZHUB
// ---- POST /api/v1/system/reboot ---- 直连型的网页重启（HA 型的设置页走 /api2/reboot，那是 SMLIGHT 兼容层，直连型不编）
static esp_err_t handle_system_reboot(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    if (ota_mg24_busy())   return send_error(req, 409, "mg24_ota_in_progress");
    if (ota_cloud_busy())  return send_error(req, 409, "cloud_ota_in_progress");
    if (s_ota_in_progress) return send_error(req, 409, "ota_in_progress");
    send_json_response(req, "{\"status\":\"ok\"}");
    ESP_LOGW(TAG, "Reboot requested via /api/v1/system/reboot");
    (void)config_mgr_flush(500);       /* 刚改的设置还在 2 s 防抖里；hubdb 的由 hub_store 的 shutdown handler 落盘 */
    vTaskDelay(pdMS_TO_TICKS(500));    /* 让回包发出去 */
    esp_restart();                     /* CPU 复位，不重采 strap（不撞 IO28） */
    return ESP_OK;
}
#endif

// ---- POST /api/v1/ota/www ----
// Upload a new SPIFFS image for the www (frontend) partition. Body = raw
// image bytes (build: spiffsgen.py 0x80000 www www.bin). Device reboots
// after a successful write so the HTTP server reopens the fresh SPIFFS.
static esp_err_t handle_ota_www(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    /* MG24 烧录是后台任务，s_ota_in_progress 管不到它。写完 www 会 esp_restart()，
       正好把 XMODEM 打断在半路 —— 而 MG24 的恢复依赖"NCP 应用还活着"+ nBOOT，
       IO23 硬复位不可用。同 handle_ota_zigbee / handle_diag_uart 的护栏。 */
    if (ota_mg24_busy())   return send_error(req, 409, "mg24_ota_in_progress");
    if (ota_cloud_busy())  return send_error(req, 409, "cloud_ota_in_progress");
    if (s_ota_in_progress) return send_error(req, 409, "ota_already_in_progress");
    s_ota_in_progress = true;

    size_t part = www_image_partition_size();
    if (part == 0) {
        s_ota_in_progress = false;
        return send_error(req, 500, "no_www_partition");
    }

    int content_len = req->content_len;
    if (content_len <= 0) { s_ota_in_progress = false; return send_error(req, 400, "empty_body"); }
    /* spiffsgen 的输出恒等于分区大小（本板 524288）。不等的东西不是给这个分区做的镜像，
       收下来也会被 www_image_validate 拒掉 —— 提前拒，省得先收 512KB（审计 A19）。 */
    if ((size_t)content_len != part) { s_ota_in_progress = false; return send_error(req, 400, "www_size_mismatch"); }

    /* Two-phase like the app OTA: buffer in PSRAM first so a slow/broken
       upload can't leave the partition half-erased. */
    char *buf = malloc(content_len);
    if (!buf) { s_ota_in_progress = false; return send_error(req, 500, "alloc_failed"); }

    int received = 0;
    while (received < content_len) {
        int want = content_len - received;
        if (want > 16 * 1024) want = 16 * 1024;
        int ret = httpd_req_recv(req, buf + received, want);
        if (ret <= 0) {
            ESP_LOGE(TAG, "www OTA recv error at %d/%d", received, content_len);
            free(buf);
            s_ota_in_progress = false;
            return send_error(req, 400, "recv_failed");
        }
        received += ret;
    }

    /* 形状校验：首末块的 SPIFFS magic 必须是按本分区算出来的值。
       与云端更新共用 www_image_validate（components/ota_mgr/www_image.c）。 */
    const char *why = NULL;
    if (www_image_validate((const uint8_t *)buf, (size_t)received, &why) != ESP_OK) {
        ESP_LOGW(TAG, "www OTA rejected: %s", why ? why : "www_invalid");
        free(buf);
        s_ota_in_progress = false;
        return send_error(req, 400, why ? why : "www_invalid");
    }

    /* Erase + write sector-by-sector inside www_image_write: short flash stalls
       only, so the RS706 feed task keeps running (same lesson as the app OTA). */
    ESP_LOGI(TAG, "www OTA: writing %d bytes...", received);
    esp_err_t err = www_image_write((const uint8_t *)buf, (size_t)received);
    free(buf);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "www OTA flash failed: %s", esp_err_to_name(err));
        s_ota_in_progress = false;
        return send_error(req, 500, "flash_failed");
    }

    ESP_LOGI(TAG, "www OTA complete (%d bytes), rebooting...", received);
    send_json_response(req, "{\"status\":\"ok\",\"message\":\"rebooting\"}");
    (void)config_mgr_flush(500);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// ---- POST /api/v1/led/sweep ----
// No body. Runs the TX-variant sweep (~40s); watch which step renders four
// clean WHITE LEDs and report that step number from the serial log.
// 判据是白色不是单色 —— 理由见 led_ctrl.c 的 k_sweep 注释（单色看不出
// "1 bit 之后丢字节"这个故障，2026-08-13 因此选错过一次时序）。
static esp_err_t handle_led_sweep(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    esp_err_t err = led_ctrl_sweep();
    if (err == ESP_ERR_NOT_SUPPORTED) return send_error(req, 501, "no_ws2812b_hardware");
    return send_json_response(req,
        "{\"status\":\"ok\",\"note\":\"sweep running ~40s, find the step showing four clean WHITE LEDs\"}");
}

// ---- POST /api/v1/led/probe ----
// {"seconds": 1-60} — 1Hz square wave on the WS2812 data GPIO for
// multimeter tracing of the level-shifter chain (no scope needed).
static esp_err_t handle_led_probe(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    char body[48];
    int len = recv_body(req, body, sizeof(body));
    if (len <= 0) return send_error(req, 400, "empty_body");
    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");
    cJSON *jsec = cJSON_GetObjectItem(root, "seconds");
    int sec = cJSON_IsNumber(jsec) ? jsec->valueint : 0;
    cJSON_Delete(root);
    if (sec < 1 || sec > 60) return send_error(req, 400, "invalid_seconds");
    esp_err_t err = led_ctrl_probe((uint32_t)sec);
    if (err == ESP_ERR_NOT_SUPPORTED) return send_error(req, 501, "no_ws2812b_hardware");
    if (err != ESP_OK) return send_error(req, 400, "invalid_arg");
    return send_json_response(req, "{\"status\":\"ok\",\"note\":\"1Hz square wave running\"}");
}

// ---- GET / POST /api/v1/led/timing ----
/* 运行时改 WS2812 位时序，用来在网页上现场找这块板的电平转换补偿量。
   只改内存，重启回 Kconfig 默认值 —— 调坏了拔电重来，找到好值再固化。
   回包里一并给出低电平宽度：故障的直接原因就是 T1L 被展宽吃光，
   让用户看着这个数调，比盯着 T1H 直观。 */
static esp_err_t send_led_timing_json(httpd_req_t *req)
{
    uint16_t t0h = 0, t1h = 0;
    led_ctrl_get_timing(&t0h, &t1h);
    char json[256];
    int n = snprintf(json, sizeof(json),
        "{\"supported\":%s,\"bit_period_ns\":1200,"
        "\"t0h_ns\":%u,\"t1h_ns\":%u,\"t0l_ns\":%u,\"t1l_ns\":%u,"
        "\"default_t0h_ns\":%d,\"default_t1h_ns\":%d,\"persisted\":false}",
#ifdef CONFIG_BITENGINE_HAS_WS2812B
        "true",
#else
        "false",
#endif
        t0h, t1h, (unsigned)(1200 - t0h), (unsigned)(1200 - t1h),
        CONFIG_BITENGINE_WS2812_T0H_NS, CONFIG_BITENGINE_WS2812_T1H_NS);
    if (n < 0 || n >= (int)sizeof(json)) return send_error(req, 500, "json_overflow");
    return send_json_response(req, json);
}

static esp_err_t handle_led_timing_get(httpd_req_t *req)
{
    AUTH_REQUIRED(req);
    return send_led_timing_json(req);
}

static esp_err_t handle_led_timing_set(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[128];
    int len = recv_body(req, body, sizeof(body));
    if (len == -2) return send_error(req, 413, "body_too_large");
    if (len <= 0)  return send_error(req, 400, "empty_body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");

    uint16_t cur0 = 0, cur1 = 0;
    led_ctrl_get_timing(&cur0, &cur1);
    int t0h = cur0, t1h = cur1;
    bool ok = json_opt_int(root, "t0h_ns", 50, 1000, &t0h)
           && json_opt_int(root, "t1h_ns", 50, 1000, &t1h);
    cJSON_Delete(root);
    if (!ok) return send_error(req, 400, "invalid_field");

    esp_err_t err = led_ctrl_set_timing((uint16_t)t0h, (uint16_t)t1h);
    if (err == ESP_ERR_NOT_SUPPORTED) return send_error(req, 501, "no_ws2812b_hardware");
    /* T0H >= T1H 会让 0 和 1 分不开 —— 这是唯一 setter 自己才知道的约束 */
    if (err != ESP_OK) return send_error(req, 400, "invalid_timing");

    return send_led_timing_json(req);
}

// ---- GET /api/v1/led/test ----
/* 测试面板的自描述：有几颗灯、每颗叫什么、参数范围多少。
   前端不该把 index→灯名 的映射写死 —— 灯的顺序由硬件走线决定（原理图 Sheet5），
   将来改板子只会动 led_ctrl 里那张表，前端跟着这个端点走就自动对。
   supported=false 表示这块板子没上 WS2812B，面板应整体禁用而不是让用户点了没反应。 */
static esp_err_t handle_led_test_info(httpd_req_t *req)
{
    AUTH_REQUIRED(req);

    char leds[256];
    int off = 0;
    int n_led = led_ctrl_count();
    for (int i = 0; i < n_led; i++) {
        const char *name = led_ctrl_name(i);
        if (!name) continue;
        int w = snprintf(leds + off, sizeof(leds) - off, "%s{\"index\":%d,\"name\":\"%s\"}",
                         off ? "," : "", i, name);
        if (w < 0 || w >= (int)sizeof(leds) - off) return send_error(req, 500, "json_overflow");
        off += w;
    }

    /* pixels：灯此刻**实际**在显示什么（RGB）。语义状态和像素之间隔着覆盖层、
       夜间抑制、RMT 就绪三道关，没有它"夜间是否真的全灭"只能靠眼睛判。 */
    char px[160];
    int poff = 0;
    uint8_t buf[8][3];
    int npx = led_ctrl_get_pixels(buf, 8);
    for (int i = 0; i < npx; i++) {
        int w = snprintf(px + poff, sizeof(px) - poff, "%s[%u,%u,%u]",
                         poff ? "," : "", buf[i][0], buf[i][1], buf[i][2]);
        if (w < 0 || w >= (int)sizeof(px) - poff) return send_error(req, 500, "json_overflow");
        poff += w;
    }
    px[poff] = '\0';

    char json[640];
    int n = snprintf(json, sizeof(json),
        "{\"supported\":%s,\"leds\":[%s],\"all_index\":-1,"
        "\"pixels\":[%s],"
        "\"color\":\"RRGGBB\",\"hold_s\":{\"min\":1,\"max\":60}}",
#ifdef CONFIG_BITENGINE_HAS_WS2812B
        "true",
#else
        "false",
#endif
        leds, px);
    if (n < 0 || n >= (int)sizeof(json)) return send_error(req, 500, "json_overflow");
    return send_json_response(req, json);
}

// ---- POST /api/v1/led/test ----
// Body: {"index": 0-3 or -1 for all, "color": "RRGGBB", "hold_s": 1-60}
static esp_err_t handle_led_test(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    char body[96];
    int len = recv_body(req, body, sizeof(body));
    if (len <= 0) return send_error(req, 400, "empty_body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");

    cJSON *jidx = cJSON_GetObjectItem(root, "index");
    cJSON *jcol = cJSON_GetObjectItem(root, "color");
    int index  = cJSON_IsNumber(jidx) ? jidx->valueint : -2;
    int hold_s = 5;
    cJSON *jhold = cJSON_GetObjectItem(root, "hold_s");
    if (cJSON_IsNumber(jhold)) hold_s = jhold->valueint;

    /* color must be exactly 6 hex chars (untrusted input — parse strictly) */
    unsigned r = 255, g = 0, b = 0;
    if (cJSON_IsString(jcol)) {
        const char *c = jcol->valuestring;
        if (strlen(c) != 6 || sscanf(c, "%02x%02x%02x", &r, &g, &b) != 3) {
            cJSON_Delete(root);
            return send_error(req, 400, "invalid_color");
        }
    }
    cJSON_Delete(root);

    if (index < -1 || index > 3) return send_error(req, 400, "invalid_index");
    if (hold_s < 1 || hold_s > 60) return send_error(req, 400, "invalid_hold");

    esp_err_t err = led_ctrl_test(index, (uint8_t)r, (uint8_t)g, (uint8_t)b,
                                  (uint32_t)hold_s * 1000);
    if (err == ESP_ERR_NOT_SUPPORTED)
        return send_error(req, 501, "no_ws2812b_hardware");
    if (err != ESP_OK)
        return send_error(req, 400, "invalid_arg");
    return send_json_response(req, "{\"status\":\"ok\"}");
}

/* LED 配置的统一回包。两个端点都用它，用户改完立刻看到的是**生效后的完整
   状态**，不用再 GET 一次。
   night_active 单独给，是因为它和 night_mode_enabled 经常不一致：开关打开但
   现在不在时段内，灯照亮 —— 不明说的话这就是一条"设了没用"的工单。 */
static esp_err_t send_led_config_json(httpd_req_t *req)
{
    const led_config_t *lc = config_mgr_get_led();
    char json[288];
    int n = snprintf(json, sizeof(json),
        "{\"status\":\"ok\",\"brightness\":%u,\"night_mode_enabled\":%s,"
        "\"night_start\":\"%02u:%02u\",\"night_end\":\"%02u:%02u\","
        "\"night_active\":%s,\"colors_configurable\":false}",
        lc->brightness,
        lc->night_mode_enabled ? "true" : "false",
        lc->night_start_hour, lc->night_start_min,
        lc->night_end_hour, lc->night_end_min,
        scheduler_night_active() ? "true" : "false");
    if (n < 0 || n >= (int)sizeof(json)) return send_error(req, 500, "json_overflow");
    return send_json_response(req, json);
}

/* "HH:MM" → 时 + 分。严格解析：必须正好 5 字符、第 3 位是冒号、其余全是数字。
   sscanf("%u:%u") 会放过 "7:5"、"07:05abc"、"99:99"，配置字段不接受这种宽松。 */
static bool parse_hhmm(const char *s, uint8_t *hh, uint8_t *mm)
{
    if (!s || strlen(s) != 5 || s[2] != ':') return false;
    for (int i = 0; i < 5; i++) {
        if (i == 2) continue;
        if (s[i] < '0' || s[i] > '9') return false;
    }
    int h = (s[0] - '0') * 10 + (s[1] - '0');
    int m = (s[3] - '0') * 10 + (s[4] - '0');
    if (h > 23 || m > 59) return false;
    *hh = (uint8_t)h;
    *mm = (uint8_t)m;
    return true;
}

// ---- POST /api/v1/led/config ----
/* Body（字段全部可选，缺省 = 保持原值）：
     {"brightness":0-255, "night_mode_enabled":bool,
      "night_start_hour":0-23, "night_start_min":0-59,
      "night_end_hour":0-23,   "night_end_min":0-59}
 *
 * ⚠️ 五个 color_* 字段（booting/normal/zigbee/error/ota）**故意不接**。
 * led_ctrl 的渲染器给四颗灯定了一套共用色语义（绿=正常 / 黄=注意 / 红=故障 /
 * 灭=未启用），是有意设计，见 led_ctrl.c 里那段注释；config 里那 5 个字段至今
 * 零读取。接进来只会存下一份永不生效的配置，正是本项目一贯拒绝的假功能。
 * 要做得先决定渲染器换不换配色方案 —— 那是产品决策，不是接线。
 * 回包里的 colors_configurable:false 就是给前端看的，别渲染颜色选择器。
 */
// ---- GET /api/v1/led/config ----
/* 同 webhook：此前只有 POST，读现状只能 POST 空对象。前端 LED 页面要显示
   当前亮度/夜间时间窗，必须有这个。 */
static esp_err_t handle_led_config_get(httpd_req_t *req)
{
    AUTH_REQUIRED_UNLESS_OPEN(req);
    return send_led_config_json(req);
}

static esp_err_t handle_led_config(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[256];
    int len = recv_body(req, body, sizeof(body));
    if (len == -2) return send_error(req, 413, "body_too_large");
    if (len <= 0)  return send_error(req, 400, "empty_body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");

    led_config_t led = *config_mgr_get_led();
    int brightness = led.brightness;
    int sh = led.night_start_hour, sm = led.night_start_min;
    int eh = led.night_end_hour,   em = led.night_end_min;

    bool ok = json_opt_int(root, "brightness",       0, 255, &brightness)
           && json_opt_int(root, "night_start_hour", 0, 23,  &sh)
           && json_opt_int(root, "night_start_min",  0, 59,  &sm)
           && json_opt_int(root, "night_end_hour",   0, 23,  &eh)
           && json_opt_int(root, "night_end_min",    0, 59,  &em)
           && json_opt_bool(root, "night_mode_enabled", &led.night_mode_enabled);
    cJSON_Delete(root);
    if (!ok) return send_error(req, 400, "invalid_field");

    led.brightness       = (uint8_t)brightness;
    led.night_start_hour = (uint8_t)sh;
    led.night_start_min  = (uint8_t)sm;
    led.night_end_hour   = (uint8_t)eh;
    led.night_end_min    = (uint8_t)em;

    /* setter 自己还会再查一遍时分范围。重复是故意的：这里挡的是 HTTP 输入，
       那里挡的是所有调用方（含 config import）。 */
    if (config_mgr_set_led(&led) != ESP_OK)
        return send_error(req, 400, "invalid_config");

    return send_led_config_json(req);
}

// ---- POST /api/v1/led/nightmode ----
/* Body：{"enabled":bool, "start":"HH:MM", "end":"HH:MM"}
 *
 * led/config 的便捷子集（docs/12 §3.9 把夜间模式单列为一个表单）。时间收
 * "HH:MM" 字符串而不是四个整数，因为这个端点直接对着 <input type="time">。
 *
 * start == end 在 scheduler 里被当作"零时长 = 不启用"（scheduler.c:53），
 * 所以这里不拦，但要在回包里能看出来 —— night_active 会是 false。
 */
static esp_err_t handle_led_nightmode(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[128];
    int len = recv_body(req, body, sizeof(body));
    if (len == -2) return send_error(req, 413, "body_too_large");
    if (len <= 0)  return send_error(req, 400, "empty_body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");

    led_config_t led = *config_mgr_get_led();
    bool ok = json_opt_bool(root, "enabled", &led.night_mode_enabled);

    const cJSON *js = cJSON_GetObjectItem(root, "start");
    if (ok && js) {
        ok = cJSON_IsString(js) &&
             parse_hhmm(js->valuestring, &led.night_start_hour, &led.night_start_min);
    }
    const cJSON *je = cJSON_GetObjectItem(root, "end");
    if (ok && je) {
        ok = cJSON_IsString(je) &&
             parse_hhmm(je->valuestring, &led.night_end_hour, &led.night_end_min);
    }
    cJSON_Delete(root);
    if (!ok) return send_error(req, 400, "invalid_field");

    if (config_mgr_set_led(&led) != ESP_OK)
        return send_error(req, 400, "invalid_config");

    return send_led_config_json(req);
}

// ---- GET /api/v1/config/export ----
static esp_err_t handle_config_export(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    char *json = config_mgr_export_json();
    if (!json) return send_error(req, 500, "out_of_memory");
    esp_err_t ret = send_json_response(req, json);
    free(json);
    return ret;
}

// ---- POST /api/v1/config/import ----
static esp_err_t handle_config_import(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 4096)
        return send_error(req, 400, "invalid_size");

    char *body = malloc(content_len + 1);
    if (!body) return send_error(req, 500, "out_of_memory");

    int received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(req, body + received, content_len - received);
        if (ret <= 0) { free(body); return send_error(req, 400, "recv_failed"); }
        received += ret;
    }
    body[content_len] = '\0';

    esp_err_t err = config_mgr_import_json(body);
    free(body);

    if (err != ESP_OK) return send_error(req, 400, "import_failed");
    return send_json_response(req, "{\"status\":\"ok\"}");
}

// ---- POST /api/v1/config/factory ----
/* body 可选：{"wipe_thread":true} 连 Thread 网络一起清。缺省保留——Thread 的
   网络数据集和网关配置同在 nvs 分区，清掉等于所有 Thread / Matter 设备变孤儿，
   这种后果必须用户显式选，不能是默认值。 */
static esp_err_t handle_factory_reset(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    factory_reset_scope_t scope = FACTORY_RESET_KEEP_THREAD;
    char body[64];
    int len = recv_body(req, body, sizeof(body));
    if (len > 0) {
        cJSON *root = cJSON_Parse(body);
        if (root) {
            bool wipe = false;
            if (json_opt_bool(root, "wipe_thread", &wipe) && wipe) {
                scope = FACTORY_RESET_WIPE_ALL;
            }
            cJSON_Delete(root);
        }
    }

    send_json_response(req, "{\"status\":\"ok\",\"message\":\"resetting\"}");
    config_mgr_factory_reset(scope);  // does not return (calls esp_restart)
    return ESP_OK;
}

// ---- POST /api/v1/ota/esp32 ----
static esp_err_t handle_ota_esp32(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    /* 同 handle_ota_www：MG24 烧录在后台跑，写完 app 的 esp_restart() 会把它打断。 */
    if (ota_mg24_busy())   return send_error(req, 409, "mg24_ota_in_progress");
    if (ota_cloud_busy())  return send_error(req, 409, "cloud_ota_in_progress");
    if (s_ota_in_progress) return send_error(req, 409, "ota_already_in_progress");
    s_ota_in_progress = true;

    int content_len = req->content_len;
    if (content_len <= 0) { s_ota_in_progress = false; return send_error(req, 400, "empty_body"); }
#if CONFIG_BITENGINE_PROFILE_DIRECT
    /* 直连型的 app 槽是 3.5 MB（docs/15 设计 2.4），带 Matter 控制器的镜像约 3 MB（M7）：写死的 3 MB 再加几步代码就会把
       合法的固件拒掉 —— 本板没有 BOOT 键，网页 OTA 拒收就只剩云端那一条路（红线 1；CHECKLIST MC06）。按目标槽的实际大小判 */
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        s_ota_in_progress = false;
        return send_error(req, 500, "no_ota_partition");
    }
    if ((size_t)content_len > update_part->size) { s_ota_in_progress = false; return send_error(req, 400, "file_too_large"); }
#else
    if (content_len > 3 * 1024 * 1024) { s_ota_in_progress = false; return send_error(req, 400, "file_too_large"); }

    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        s_ota_in_progress = false;
        return send_error(req, 500, "no_ota_partition");
    }
#endif

    esp_event_post(WEBUI_EVENT, WEBUI_EVENT_OTA_STARTED, NULL, 0, 0);

    /* Keep EZSP bridge + Thread BR running during OTA.
       Disabling them causes WiFi deauth (reason=6) and breaks bellows'
       event loop, making ZHA unrecoverable without HA restart.
       Two-phase approach: Phase 1 recv coexists with bridge traffic,
       Phase 2 flash write is only a few seconds of UART pause. */
    ESP_LOGI(TAG, "OTA: starting transfer, content_len=%d, free_heap=%lu",
             content_len, (unsigned long)esp_get_free_heap_size());

    /* Two-phase OTA: recv into PSRAM first, then write to flash.
       Avoids MSPI bus contention between flash writes and PSRAM access. */
    char *fw_buf = malloc(content_len);  /* PSRAM */
    char *bounce = malloc(16 * 1024);   /* PSRAM is fine for recv bounce */
    if (!fw_buf || !bounce) {
        ESP_LOGE(TAG, "OTA alloc failed: fw_buf=%p bounce=%p", fw_buf, bounce);
        free(fw_buf); free(bounce);
        goto ota_fail;
    }

    int received = 0;
    while (received < content_len) {
        int want = content_len - received;
        if (want > 16 * 1024) want = 16 * 1024;
        int ret = httpd_req_recv(req, bounce, want);
        if (ret <= 0) {
            ESP_LOGE(TAG, "OTA recv error at %d/%d (err=%d)", received, content_len, ret);
            free(fw_buf); free(bounce);
            goto ota_fail;
        }
        memcpy(fw_buf + received, bounce, ret);
        received += ret;
        if (received % 131072 < (unsigned)ret) {
            int pct = received * 100 / content_len;
            ESP_LOGI(TAG, "OTA recv: %d/%d (%d%%)", received, content_len, pct);
            esp_event_post(WEBUI_EVENT, WEBUI_EVENT_OTA_PROGRESS,
                           &pct, sizeof(pct), 0);
        }
    }
    free(bounce);

    /* 防串刷（docs/15 设计 2.7、架构 5.8 R-9）：HA 型与直连型同一把签名钥匙，esp_ota_end 的验签挡不住
       "另一种固件"。整包已在 PSRAM 里，写 flash 之前比对工程名，不一致就一个字节都不写、什么都不动。
       409 = 是 ESP 镜像但不是这种设备的（profile_mismatch / profile_unknown）；400 = 根本不是 app 镜像
       （比如传成了 www.bin）；500 = 守卫自己出错（internal_error，正常走不到）。 */
    const char *pwhy = ota_cloud_profile_check_image(fw_buf, (size_t)received);
    if (pwhy) {
        free(fw_buf);
        s_ota_in_progress = false;
        esp_event_post(WEBUI_EVENT, WEBUI_EVENT_OTA_FAILED, NULL, 0, 0);
        int st = strcmp(pwhy, "image_invalid") == 0 ? 400 : (strcmp(pwhy, "internal_error") == 0 ? 500 : 409);
        return send_error(req, st, pwhy);
    }

    ESP_LOGI(TAG, "OTA: received %d bytes, writing to flash...", received);
    esp_ota_handle_t ota_handle;
    /* OTA_WITH_SEQUENTIAL_WRITES → each esp_ota_write() erases only the sectors it is about
       to write (esp_ota_ops.c, IDF v6.0.2), so there is never one multi-second erase burst.
       Such a burst stalls flash-resident code (incl. wdt_feed_task) past the RS706 1.6s window
       and, once the board can actually reset on WDO (AM2 A6; today it cannot, X7), would reset
       it mid-OTA. ⚠️ This used to pass OTA_SIZE_UNKNOWN with the same comment, but that value
       erases the WHOLE slot in esp_ota_begin() — the opposite (2026-10-21 audit). The writes
       below are strictly sequential 16 KB chunks, which is what this mode requires. The cloud
       path already erases incrementally (esp_https_ota default, bulk_flash_erase = false). */
    esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        free(fw_buf);
        goto ota_fail;
    }

    int written = 0;
    while (written < received) {
        int chunk = received - written;
        if (chunk > 16 * 1024) chunk = 16 * 1024;
        err = esp_ota_write(ota_handle, fw_buf + written, chunk);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed at %d: %s", written, esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            free(fw_buf);
            goto ota_fail;
        }
        written += chunk;
    }

    free(fw_buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
        goto ota_fail;
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA set boot partition failed: %s", esp_err_to_name(err));
        goto ota_fail;
    }

    ESP_LOGI(TAG, "OTA complete (%d bytes), rebooting...", received);

    esp_event_post(WEBUI_EVENT, WEBUI_EVENT_OTA_COMPLETE, NULL, 0, 0);
#if CONFIG_BITENGINE_PROFILE_DIRECT
    /* 直连型没有 ZHA：网关自己是 Zigbee 主机，重启后自己接回网络 */
    send_json_response(req,
        "{\"status\":\"ok\",\"message\":\"rebooting\","
        "\"note\":\"Zigbee/Thread devices should reconnect within 60s.\"}");
#else
    send_json_response(req,
        "{\"status\":\"ok\",\"message\":\"rebooting\","
        "\"note\":\"Zigbee/Thread devices should reconnect within 60s. "
        "If not, reload ZHA integration in Home Assistant settings.\"}");
#endif

    ezsp_bridge_switch_mode(BRIDGE_MODE_DISABLED);
    (void)config_mgr_flush(500);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;

ota_fail:
    /* No reboot on failure — bridge + Thread BR were never disabled,
       everything is still running. Just report the error. */
    s_ota_in_progress = false;
    esp_event_post(WEBUI_EVENT, WEBUI_EVENT_OTA_FAILED, NULL, 0, 0);
    ESP_LOGE(TAG, "OTA failed");
    return send_error(req, 500, "ota_failed");
}

#if !CONFIG_BITENGINE_ENABLE_ZHUB   /* /mode 与 /zigbee/clients/disconnect 只服务桥（:6638 / USB），直连型不编（R-2、A-04） */
// ---- POST /api/v1/mode ----
static esp_err_t handle_mode(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[64];
    int len = recv_body(req, body, sizeof(body));
    if (len <= 0) return send_error(req, 400, "empty_body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");

    cJSON *mode = cJSON_GetObjectItem(root, "mode");
    if (!mode || !cJSON_IsNumber(mode) ||
        mode->valueint < BRIDGE_MODE_LAN || mode->valueint > BRIDGE_MODE_DISABLED) {
        cJSON_Delete(root);
        return send_error(req, 400, "invalid_mode");
    }

    int wanted = mode->valueint;
    esp_err_t err = ezsp_bridge_switch_mode((bridge_mode_t)wanted);
    cJSON_Delete(root);

    if (err == ESP_ERR_INVALID_ARG)   return send_error(req, 400, "invalid_mode");
    if (err == ESP_ERR_INVALID_STATE) return send_error(req, 409, "switch_in_progress");
    if (err != ESP_OK) return send_error(req, 500, "switch_failed");

    /* 落盘 —— 这里是**唯一**代表"用户选择了这个模式"的地方。
       绝不能挪进 ezsp_bridge_switch_mode()：MG24 OTA（ota_mg24.c）、UART 健康检查
       （uart_bench.c）、ESP OTA（本文件 handle_ota_*）都会先切 DISABLED 干完活再切回来，
       落在那一层会把临时态写进 NVS —— OTA 中途掉电，网关就永久带着 DISABLED 回来。
       落盘失败只记日志：模式已经切了，回 500 会让用户以为没切成而重试。 */
    esp_err_t perr = config_mgr_set_bridge_mode((uint8_t)wanted);
    if (perr != ESP_OK) {
        ESP_LOGW(TAG, "bridge mode %d applied but not persisted (%s) — 重启后会回到旧模式",
                 wanted, esp_err_to_name(perr));
    }
    return send_json_response(req, "{\"status\":\"ok\"}");
}

/* ---- POST /api/v1/zigbee/clients/disconnect ----
   踢掉一个 EZSP TCP 客户端。/status 一直列得出每个客户端，却没有任何办法把它断开
   （ezsp_bridge_disconnect_client() 零调用点）。真实场景：网关重启后 ZHA 会卡死不
   自愈，此前只能整体重启 HA —— 踢掉那条僵连接让它自己重连要轻得多。

   两种寻址：{"ip":"192.168.1.5"} 或 {"index":0}。推荐用 ip —— index 是 tcp_server
   的槽位号（/status 里那个 index 字段），而按 ip 的匹配与关闭在同一次持锁内完成，
   没有"查到下标、槽位已被别人复用"的窗口。 */
static esp_err_t handle_clients_disconnect(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[128];
    int len = recv_body(req, body, sizeof(body));
    if (len <= 0) return send_error(req, 400, "empty_body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");

    cJSON *ip  = cJSON_GetObjectItem(root, "ip");
    cJSON *idx = cJSON_GetObjectItem(root, "index");

    esp_err_t err;
    int closed = 0;
    if (ip && cJSON_IsString(ip) && ip->valuestring && ip->valuestring[0]) {
        err = ezsp_bridge_disconnect_client_by_ip(ip->valuestring, &closed);
    } else if (idx && cJSON_IsNumber(idx)) {
        err = ezsp_bridge_disconnect_client(idx->valueint);
        if (err == ESP_OK) closed = 1;
    } else {
        cJSON_Delete(root);
        return send_error(req, 400, "ip_or_index_required");
    }
    cJSON_Delete(root);

    if (err == ESP_ERR_INVALID_ARG) return send_error(req, 400, "invalid_index");
    /* 没命中回 404 而不是 200：用户点了"踢掉"却什么也没发生，必须看得出来。 */
    if (err == ESP_ERR_NOT_FOUND)   return send_error(req, 404, "client_not_connected");
    if (err != ESP_OK)              return send_error(req, 500, "disconnect_failed");

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"closed\":%d}", closed);
    return send_json_response(req, resp);
}
#endif /* !CONFIG_BITENGINE_ENABLE_ZHUB */

// ---- POST /api/v1/zigbee/stats/reset ----
/* 桥接统计清零。排障时想"清零后看 5 分钟涨多少"，此前只能重启整机
   （ezsp_bridge_reset_stats() 零调用点）。 */
static esp_err_t handle_stats_reset(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    ezsp_bridge_reset_stats();
    return send_json_response(req, "{\"status\":\"ok\"}");
}

#if !CONFIG_BITENGINE_NET_ETH_ONLY   /* 纯以太网剖面没有 Wi-Fi，不编也不注册（docs/15 设计 2.2） */
// ---- GET/POST /api/v1/network/softap ----
/* 手动开关配网 AP。此前 SoftAP 只能由状态机在"STA 连不上"时自动兜底
   （network_mgr_start_softap/stop_softap 零调用点），两个方向都用得上：
   有线在网时临时开 AP 给手机配网；或为了 Thread 稳定显式关掉它。 */
static esp_err_t handle_softap_get(httpd_req_t *req)
{
    AUTH_REQUIRED(req);
    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"active\":%s,\"exclusive_with_thread\":true}",
             network_mgr_softap_active() ? "true" : "false");
    return send_json_response(req, resp);
}

static esp_err_t handle_softap_set(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[64];
    int len = recv_body(req, body, sizeof(body));
    if (len <= 0) return send_error(req, 400, "empty_body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");

    cJSON *en = cJSON_GetObjectItem(root, "enabled");
    if (!en || !cJSON_IsBool(en)) {
        cJSON_Delete(root);
        return send_error(req, 400, "invalid_enabled");
    }
    bool want = cJSON_IsTrue(en);
    cJSON_Delete(root);

    if (want == network_mgr_softap_active()) {
        return send_json_response(req, "{\"status\":\"ok\",\"changed\":false}");
    }

    /* ⚠️ SoftAP 与 Thread BR 互斥（2026-08-08 定位）：AP 一起来，802.15.4 就收不到帧。
       thread_br 已订阅 SoftAP 起停事件自行暂停/恢复，这里不重复处理，但要在回包里
       说清楚 —— 用户点一下开关就让 Thread 设备全掉线，不告诉他是不合适的。 */
    esp_err_t err = want ? network_mgr_start_softap() : network_mgr_stop_softap();
    if (err != ESP_OK) return send_error(req, 500, "softap_switch_failed");

    return send_json_response(req, want
        ? "{\"status\":\"ok\",\"changed\":true,\"warning\":\"thread_paused_while_softap_on\"}"
        : "{\"status\":\"ok\",\"changed\":true}");
}

// ---- POST /api/v1/wifi/static ----
/* WiFi 静态 IP。以太网侧一直是完整的，WiFi 侧的 config_mgr_set_wifi_static_ip()
   零调用点 —— 只能靠导入配置来设。
   单独一个端点而不是并进 /wifi/config：后者强制要 ssid，改个 IP 却要重发密码，
   而且它会触发重连。
   ⚠️ 只落配置，不即时下发。生效点是 network_mgr_init() 的 apply_static_ip_config()，
   即**下次重启**。即时下发会立刻改掉本机 STA 的 IP，正在应答的这条 HTTP 连接当场断掉,
   用户看到的是"请求失败"而其实成功了。回包里的 takes_effect 就是说这件事。 */
static esp_err_t handle_wifi_static(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[256];
    int len = recv_body(req, body, sizeof(body));
    if (len <= 0) return send_error(req, 400, "empty_body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");

    const wifi_config_data_t *cur = config_mgr_get_wifi();
    uint32_t ip = cur->static_ip, gw = cur->static_gateway;
    uint32_t nm = cur->static_netmask, dns = cur->static_dns;

    /* dhcp:true 等价于清空静态配置——和以太网侧 dhcp 字段的语义对齐 */
    cJSON *dhcp = cJSON_GetObjectItem(root, "dhcp");
    bool clear = dhcp && cJSON_IsBool(dhcp) && cJSON_IsTrue(dhcp);

    if (!clear) {
        parse_ip_field(cJSON_GetObjectItem(root, "static_ip"), &ip);
        parse_ip_field(cJSON_GetObjectItem(root, "static_gateway"), &gw);
        parse_ip_field(cJSON_GetObjectItem(root, "static_netmask"), &nm);
        parse_ip_field(cJSON_GetObjectItem(root, "static_dns"), &dns);
    }
    cJSON_Delete(root);

    if (clear) {
        config_mgr_set_wifi_static_ip(NULL, 0);
        return send_json_response(req,
            "{\"status\":\"ok\",\"dhcp\":true,\"takes_effect\":\"reboot\"}");
    }

    if (ip == 0) return send_error(req, 400, "static_ip_required");

    esp_netif_ip_info_t info = {
        .ip.addr = ip, .gw.addr = gw,
        .netmask.addr = nm ? nm : esp_netif_htonl(0xFFFFFF00),
    };
    esp_err_t err = config_mgr_set_wifi_static_ip(&info, dns);
    if (err != ESP_OK) return send_error(req, 500, "save_failed");

    return send_json_response(req,
        "{\"status\":\"ok\",\"dhcp\":false,\"takes_effect\":\"reboot\"}");
}

#endif /* !CONFIG_BITENGINE_NET_ETH_ONLY */

// ---- Spectrum endpoints (Phase 3) ----

/* 能量扫描的失败原因必须分得开：ESP_ERR_TIMEOUT 表示 ED 被 WiFi 共存仲裁器拒了、
   本次一个有效读数都没有（不是普通内部错误）。笼统回 "scan_failed" 会让人以为是
   偶发故障，而这在 WiFi 常驻的板子上是稳定复现的——见 thread_br.c 里
   force_finish_energy_scan() 的说明。 */
static esp_err_t send_energy_scan_error(httpd_req_t *req, esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_STATE: return send_error(req, 503, "thread_not_running");
    case ESP_ERR_NOT_SUPPORTED: return send_error(req, 501, "energy_scan_unsupported");
    case ESP_ERR_TIMEOUT:       return send_error(req, 503, "energy_scan_blocked_by_coex");
    default:                    return send_error(req, 500, "scan_failed");
    }
}

/* 可选 ?dwell_ms=N —— 每信道驻留时长，默认 100（原行为）。
 *
 * 为什么开这个参数：/api/v1/diag/ed 的实测对照显示，共存仲裁器对 128µs 的 CCA
 * 一次都没拒过（cca_fail=0），对 100ms 的 ED 却是 16/16 全拒（ed_scan_coex=16，
 * 两次复现）。判别变量很可能是**连续独占时长**而不是优先级，这个参数就是用来
 * 扫这条曲线的：跑一遍 1/2/5/20/100ms，看 ed_scan_coex 是否随驻留变短而下降。
 *
 * ⚠️ 上界 1000：otPlatRadioEnergyScan (esp_openthread_radio.c:429) 把 ms 换算成
 *    symbol（×1000/16）后塞进 ieee802154_ll_set_ed_duration()，形参是 uint16_t
 *    （hal/ieee802154_common_ll.h:367）—— 超过 1048ms 会**静默截断**成一个完全
 *    不同的时长，测出来的数就全是错的。
 * ⚠️ 下界 1：dwell=0 会走 OT 的同步路径（mac.cpp:283-291）用 mLinks.GetRssi()
 *    取值，在本平台落到 esp_ieee802154_get_recent_rssi() = 最近一次**收帧**的锁存
 *    RSSI —— 那是假数据不是信道能量。OT 的 API 以 ms 为单位，所以 1ms（62 symbol）
 *    是经 otLinkEnergyScan 能达到的最短驻留，仍是 CCA 的 ~8 倍。
 */
/* 返回 0 表示"没指定，用固件默认值"。⚠️ 不要在这里写死 100 —— 那是本平台上
   必然被共存拒绝的参数，会让默认扫描永远拿不到数据。 */
static uint16_t scan_dwell_from_query(httpd_req_t *req)
{
    char q[64] = {0};
    char v[12] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return 0;
    if (httpd_query_key_value(q, "dwell_ms", v, sizeof(v)) != ESP_OK) return 0;

    int n = atoi(v);
    if (n < 1)    n = 1;
    if (n > 1000) n = 1000;
    return (uint16_t)n;
}

static esp_err_t handle_spectrum_scan(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    uint16_t dwell_ms = scan_dwell_from_query(req);

    spectrum_scan_result_t result;
    esp_err_t err = spectrum_energy_scan(dwell_ms, &result);
    if (err != ESP_OK) return send_energy_scan_error(req, err);

    /* 900 而不是 512：每信道多了 valid 字段。下面的 sizeof(buf)-n 一旦 n 越界
       就会回绕成一个巨大的 size_t，snprintf 反而不再截断——所以每轮都得先
       确认 n 还在界内。 */
    char buf[900];
    int valid_count = __builtin_popcount(result.valid_mask);
    /* dwell_ms + passes 一起给：读数的含义完全由它们决定（1 轮 1ms 是瞬时底噪，
       16 轮取最大是"这一秒最吵能到多少"，两者能差 40dB）。少给一个就没法解释。 */
    int n = snprintf(buf, sizeof(buf),
                     "{\"timestamp\":%"PRIu32",\"dwell_ms\":%d,\"passes\":%d,"
                     "\"duration_ms\":%d,\"valid_count\":%d,\"channels\":[",
                     result.timestamp, result.scan_duration_ms, result.passes,
                     result.scan_duration_ms, valid_count);
    for (int i = 0; i < SPECTRUM_NUM_CHANNELS && n > 0 && n < (int)sizeof(buf); i++) {
        /* 没测到的信道给 energy:null 而不是 -100。-100 会被前端画成"最矮最绿"
           = 看起来最干净，正好是与事实相反的结论。
           ⚠️ 不要再加 "valid":true/false 这类冗余字段：energy===null 本身就是
           唯一判据，而这份 JSON 会经 webui_ws_push_event() 推给 WS 客户端，
           那里是 buf[512] 且**超长直接静默丢弃**（websocket.c:195）。
           加过一次 valid 字段就把负载顶到约 658 字节，推送全没了还不报错。 */
        if (spectrum_ch_valid(&result, i)) {
            n += snprintf(buf + n, sizeof(buf) - n,
                          "%s{\"ch\":%d,\"energy\":%d}",
                          i > 0 ? "," : "", SPECTRUM_FIRST_CHANNEL + i,
                          result.energy[i]);
        } else {
            n += snprintf(buf + n, sizeof(buf) - n,
                          "%s{\"ch\":%d,\"energy\":null}",
                          i > 0 ? "," : "", SPECTRUM_FIRST_CHANNEL + i);
        }
    }
    if (n < 0 || n >= (int)sizeof(buf)) return send_error(req, 500, "json_overflow");
    strlcat(buf, "]}", sizeof(buf));

    /* 2026-09-11(T4.5)：删掉了这里的 webui_ws_push_event("spectrum_result", buf)。
       只有生产者没有消费者 —— 前端只订阅 status_update，频谱页走的是本函数的
       HTTP 响应体那条路。而且这个负载**正卡在 512 字节的丢弃线上**：16 信道全量
       约 510-520 字节，曾经给 scan JSON 加一个 valid 字段就顶到 658，推送全没了
       （41cebe0 那次）。留着一个随时会静默失效、又没人听的推送，只是给未来的
       排查添乱。真要做实时频谱，前端先订阅、缓冲再一起放大。 */
    return send_json_response(req, buf);
}

static esp_err_t handle_spectrum_beacon(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    /* ?dwell_ms= 与 /spectrum/scan、/diag/beacon 同语义：缺省 0 = 固件默认（400），
       上下钳位在 spectrum 里做。约 16 × dwell 的阻塞，单任务 httpd 会卡这么久。 */
    beacon_scan_result_t result;
    esp_err_t err = spectrum_beacon_scan_ex(scan_dwell_from_query(req), &result);
    switch (err) {
    case ESP_OK: break;
    /* Thread BR 没跑，或 OT 正忙（zigbee_check 的周期探测也在用主动扫描）。
       OT 构型下射频归 OpenThread，Thread 停着时没有别的路能发 beacon request ——
       所以这不是"暂时不可用"，而是"先把 Thread 打开"。 */
    case ESP_ERR_INVALID_STATE: return send_error(req, 503, "thread_not_running_or_busy");
    case ESP_ERR_TIMEOUT:       return send_error(req, 503, "spectrum_busy");
    default:                    return send_error(req, 500, "beacon_scan_failed");
    }

    /* 2026-09-13 起是对象而不是裸数组：completed 必须能往外报（预算掐断时
       "ch26 没邻居"和"ch26 没扫到"分不开），self 让前端把自家协调器标出来。 */
    cJSON *root = cJSON_CreateObject();
    if (!root) return send_error(req, 500, "out_of_memory");
    cJSON *arr = cJSON_AddArrayToObject(root, "networks");
    if (!arr) { cJSON_Delete(root); return send_error(req, 500, "out_of_memory"); }
    cJSON_AddBoolToObject(root, "completed", result.completed);
    cJSON_AddNumberToObject(root, "dwell_ms", result.dwell_ms);
    cJSON_AddNumberToObject(root, "count", result.count);
    for (int i = 0; i < result.count; i++) {
        cJSON *net = cJSON_CreateObject();
        if (!net) { cJSON_Delete(root); return send_error(req, 500, "out_of_memory"); }
        cJSON_AddNumberToObject(net, "pan_id", result.networks[i].pan_id);
        cJSON_AddNumberToObject(net, "channel", result.networks[i].channel);
        cJSON_AddNumberToObject(net, "rssi", result.networks[i].rssi);
        cJSON_AddNumberToObject(net, "lqi", result.networks[i].lqi);
        cJSON_AddBoolToObject(net, "self", result.networks[i].self);
        cJSON_AddItemToArray(arr, net);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return send_error(req, 500, "out_of_memory");
    esp_err_t ret = send_json_response(req, json);
    free(json);
    return ret;
}

static esp_err_t handle_spectrum_recommend(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    spectrum_scan_result_t energy;
    beacon_scan_result_t beacons;
    /* 拒绝而不是拿 -100 去推荐信道：那会推出一个"最干净"的假结论。
       也认 ?dwell_ms=：与 /spectrum/scan 保持一致，同时让"有效信道不足"
       这条分支可以被真正走一遍验证（默认 1ms 几乎总是 16/16，测不到）。 */
    esp_err_t err = spectrum_energy_scan(scan_dwell_from_query(req), &energy);
    if (err != ESP_OK) return send_energy_scan_error(req, err);
    /* 邻居失败不致命（当 0 个邻居），但要把结果清干净再用；120s 内复用上一次：
       前端点一次"扫描"会先后打 /beacon 和 /recommend，不复用就要连扫两遍、再卡 7s。 */
    if (spectrum_beacon_scan_cached(&beacons, 120) != ESP_OK) memset(&beacons, 0, sizeof(beacons));
    /* 推荐算法里 thread_channel==0 的既定含义是"Thread 未启用"（设计文档
       08 第 101 行），所以只有真的没在跑才传 0。Thread 在跑却读不到信道时，
       退回配置值继续扣共存分并标注来源 —— 传 0 会把"我没读到"伪装成
       "板上没有 Thread 射频"，而后者明确是假的：迁移过信道的板子是少数，
       配置值绝大多数时候就是对的，而完全不扣分在任何情况下都是错的。 */
    uint8_t thread_ch = 0;
    const char *thread_ch_source = "disabled";
    if (thread_br_is_running()) {
        if (thread_br_get_active_channel(&thread_ch) == ESP_OK) {
            thread_ch_source = "active";
        } else {
            thread_ch = thread_br_get_configured_channel();
            thread_ch_source = "config";
        }
    }
    int8_t recommended = spectrum_recommend_channel(&energy, &beacons, thread_ch);
    int valid_count = __builtin_popcount(energy.valid_mask);

    /* 有效信道不够时如实说"给不出推荐"，而不是从半张频谱里挑一个报出去。 */
    if (recommended == SPECTRUM_NO_RECOMMENDATION) {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "{\"error\":\"insufficient_spectrum_data\",\"valid_count\":%d,"
                 "\"required\":%d,\"thread_channel\":%d,"
                 "\"thread_channel_source\":\"%s\"}",
                 valid_count, SPECTRUM_MIN_VALID_FOR_RECOMMEND, thread_ch,
                 thread_ch_source);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return send_json_response(req, buf);
    }

    char buf[224];
    snprintf(buf, sizeof(buf),
             "{\"recommended_channel\":%d,\"thread_channel\":%d,"
             "\"thread_channel_source\":\"%s\",\"valid_count\":%d}",
             recommended, thread_ch, thread_ch_source, valid_count);
    return send_json_response(req, buf);
}

// ---- GET/POST /api/v1/thread/config ----
/*  {"enabled":bool, "channel":11-26}
 *
 *  ── 为什么需要这个端点（T3.3）──
 *  config_mgr_set_thread() 此前唯一的调用者是 OTBR 认证开关，它只改 rest_auth，
 *  从不碰 enabled 和 channel。于是：
 *    · WebUI 点 Disable 只停协议栈，**不落盘** —— 重启后 thread_br_init() 照着
 *      enabled=true 又拉起来，用户以为关掉了（这一条现在也在 PUT /node/state 修了）
 *    · 初始信道根本没有地方能改
 *
 *  ⚠️ channel 只对**尚未组网**的初始 dataset 生效。已经组网的网络换信道要走
 *     PUT /node/dataset/pending（那是带生效延迟的标准流程，子设备才跟得上）。
 *     回包里明说这件事，否则用户改了以为生效了 —— 这正是"存了 ≠ 生效"的翻版。
 *  ⚠️ enabled 的改动**下次重启才生效**：这里只落盘，不去停/起运行中的协议栈。
 *     要立刻停/起用 PUT /node/state。分开是有意的：一个动配置，一个动运行态，
 *     混在一起会让"我到底改了哪个"无从分辨。 */
static esp_err_t handle_thread_config_get(httpd_req_t *req)
{
    AUTH_REQUIRED(req);
    const thread_config_t *t = config_mgr_get_thread();
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"enabled\":%s,\"channel\":%u,"
             "\"note\":\"enabled takes effect on next reboot; channel applies only to "
             "a not-yet-formed network (use PUT /node/dataset/pending to move a live one)\"}",
             t->enabled ? "true" : "false", (unsigned)t->channel);
    return send_json_response(req, buf);
}

static esp_err_t handle_thread_config_set(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[128];
    int len = recv_body(req, body, sizeof(body));
    if (len <= 0) return send_error(req, 400, "empty_body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");

    thread_config_t t = *config_mgr_get_thread();
    bool touched = false;

    cJSON *en = cJSON_GetObjectItem(root, "enabled");
    if (en) {
        if (!cJSON_IsBool(en)) { cJSON_Delete(root); return send_error(req, 400, "enabled_not_bool"); }
        t.enabled = cJSON_IsTrue(en);
        touched = true;
    }
    cJSON *ch = cJSON_GetObjectItem(root, "channel");
    if (ch) {
        if (!cJSON_IsNumber(ch) || ch->valueint < 11 || ch->valueint > 26) {
            cJSON_Delete(root);
            return send_error(req, 400, "channel_out_of_range");
        }
        t.channel = (uint8_t)ch->valueint;
        touched = true;
    }
    cJSON_Delete(root);

    if (!touched) return send_error(req, 400, "nothing_to_set");

    esp_err_t err = config_mgr_set_thread(&t);
    if (err == ESP_ERR_INVALID_ARG) return send_error(req, 400, "invalid_config");
    if (err != ESP_OK)              return send_error(req, 500, "save_failed");

    char buf[320];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"enabled\":%s,\"channel\":%u,"
             "\"takes_effect\":\"reboot\","
             "\"note\":\"channel applies only to a not-yet-formed network; "
             "to move a live network use PUT /node/dataset/pending. "
             "Use PUT /node/state to stop or start Thread right now.\"}",
             t.enabled ? "true" : "false", (unsigned)t.channel);
    return send_json_response(req, buf);
}

// ---- POST /api/v1/system/hostname ----
/*  {"hostname":"bitengine"}  →  改主机名（mDNS 广播名、bitengine.local 的那个名）
 *
 *  此前 config_mgr_set_hostname() 与 mdns_svc_update_hostname() **都是零调用者**
 *  （后者 2026-09-13 已删：主机名变更由 mdns_svc 订阅事件自己处理，不需要谁来调它）：
 *  写好了从没被谁调过，设置页只把它显示成一张只读卡片（T3.2）。
 *
 *  ⚠️ 校验在 config_mgr_set_hostname() 里做，不在这里 —— 配置导入那条路径
 *     也必须受同一套规则管。extra_host 吃过这个亏：校验只长在 setter 外面，
 *     于是 API 拒掉的值可以从一份备份里绕进来。
 *  ⚠️ mDNS 立刻改名，但**已经连着的客户端不受影响**（它们用的是 IP）。
 *     HA 侧靠 mDNS 发现的条目可能需要重新发现，回包里说明这一点。 */
static esp_err_t handle_system_hostname(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[128];
    int len = recv_body(req, body, sizeof(body));
    if (len <= 0) return send_error(req, 400, "empty_body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");
    cJSON *hn = cJSON_GetObjectItem(root, "hostname");
    if (!cJSON_IsString(hn) || !hn->valuestring) {
        cJSON_Delete(root);
        return send_error(req, 400, "missing_hostname");
    }

    /* ⚠️ 先判长度再拷贝。strlcpy 会**静默截断** —— 第一版就是直接拷进 32 字节
       缓冲，于是一个 36 字符的名字被截成 31 字符的合法名字然后**通过**了校验，
       设备用了一个用户没打算要的名字。tools/test_api.sh 的"超长名被拒"抓到的
       就是这一条。同 device_identity 里"截断出来的 SN 会静默指向另一台设备"。 */
    size_t want_len = strlen(hn->valuestring);
    if (want_len == 0 || want_len >= HOSTNAME_MAX_LEN) {
        cJSON_Delete(root);
        return send_error(req, 400, "invalid_hostname");
    }
    char want[HOSTNAME_MAX_LEN];
    strlcpy(want, hn->valuestring, sizeof(want));
    cJSON_Delete(root);

    esp_err_t err = config_mgr_set_hostname(want);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_error(req, 400, "invalid_hostname");
    }
    if (err != ESP_OK) return send_error(req, 500, "save_failed");

    /* 192 不够：那句 note 本身 133 字节，加上转义后最长 62 字节的主机名会截断
       —— 编译器的 -Wformat-truncation 直接拦下了。按最坏情况给足。 */
    char out[320];
    char safe[HOSTNAME_MAX_LEN * 2];
    json_sanitize_copy(safe, config_mgr_get_sys()->hostname, sizeof(safe));
    snprintf(out, sizeof(out),
             "{\"status\":\"ok\",\"hostname\":\"%s\","
             "\"note\":\"mDNS renamed immediately; existing connections keep working. "
             "Home Assistant entries found via mDNS may need re-discovery.\"}",
             safe);
    return send_json_response(req, out);
}

// ---- POST /api/v1/zigbee/channel ----
// 用户声明"我在 Z2M/ZHA 里把信道设成了几"。固件读不到实测值（见
// ncp_version.h），共存判断只能建立在这个诚实输入上。
// body: {"channel": 11..26}  或 {"channel": null} 撤销声明。
static esp_err_t handle_zigbee_channel(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    char body[64];
    int len = recv_body(req, body, sizeof(body));
    if (len <= 0) return send_error(req, 400, "empty_body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");
    cJSON *ch = cJSON_GetObjectItem(root, "channel");
    if (!ch) { cJSON_Delete(root); return send_error(req, 400, "missing_channel"); }

    zigbee_config_t zb = *config_mgr_get_zigbee();
    const bool    prev_declared = zb.channel_declared;
    const uint8_t prev_channel  = zb.channel;
    if (cJSON_IsNull(ch)) {
        zb.channel_declared = false;
        zb.channel = 0;
    } else if (cJSON_IsNumber(ch) && ch->valueint >= 11 && ch->valueint <= 26) {
        zb.channel_declared = true;
        zb.channel = (uint8_t)ch->valueint;
    } else {
        cJSON_Delete(root);
        return send_error(req, 400, "channel_out_of_range");
    }
    cJSON_Delete(root);

    /* 声明的同时实测一次：把该信道上最强的 beacon 的 PAN 记下来。有了 PAN，
       以后判断"这个 beacon 是不是我们自己的"就是精确匹配，也才能发现声明值
       过期。自家协调器在同板几厘米外，实测比邻居强 61 dB（-20 vs -81），
       取最强者足够可靠。扫不到就只存信道，功能降级但不出错。 */
    uint16_t pan = 0;
    int8_t   rssi = 0;
    bool     pan_ok = false;
    if (zb.channel_declared &&
        thread_br_probe_pan(zb.channel, &pan, &rssi) == ESP_OK) {
        zb.pan_id    = pan;
        zb.pan_known = true;
        pan_ok = true;
    } else if (zb.channel_declared) {
        /* 探不到。只有**信道真的变了**才丢掉旧 PAN —— 它是在旧信道上学到的，
           对新声明不成立。信道没变（用户在原地重测）时必须保留：射频那一刻
           被 WiFi 占住就把 PAN 擦掉的话，过期检测从此静默停摆
           （zigbee_check_once 首行就 `!pan_known` 直接返回）。2026-09-15。 */
        if (!prev_declared || prev_channel != zb.channel) zb.pan_known = false;
    } else {
        zb.pan_known = false;
        zb.pan_id    = 0;
    }

    esp_err_t err = config_mgr_set_zigbee(&zb);
    if (err != ESP_OK) return send_error(req, 500, "config_write_failed");

    /* 刚刚那次 probe_pan 就是周期检测的窄扫阶段，结论直接刷进去 —— 否则仪表盘
       的横幅会挂着旧的 not_found 干等一个 6 小时周期（2026-09-15）。探不到 PAN
       就作废旧结论：它针对的是旧声明，别让它继续代表新的。 */
    thread_br_zigbee_check_seed(zb.channel, zb.pan_known, zb.pan_id, rssi);

    char buf[288];
    if (!zb.channel_declared)
        strlcpy(buf, "{\"status\":\"ok\",\"channel\":null}", sizeof(buf));
    else if (pan_ok)
        snprintf(buf, sizeof(buf),
                 "{\"status\":\"ok\",\"channel\":%u,\"pan_id\":\"0x%04X\",\"rssi\":%d}",
                 zb.channel, pan, rssi);
    else {
        /* 声明的信道上没东西 —— 很可能填错了。全频段找一下最强的网络给出提示。
           仍然照用户说的存（那是他的决定），但把矛盾摆出来。 */
        uint8_t s_ch = 0; uint16_t s_pan = 0; int8_t s_rssi = 0;
        if (thread_br_find_strongest(&s_ch, &s_pan, &s_rssi) == ESP_OK &&
            s_ch != zb.channel) {
            snprintf(buf, sizeof(buf),
                     "{\"status\":\"ok\",\"channel\":%u,\"pan_id\":null,"
                     "\"suggest\":{\"channel\":%u,\"pan_id\":\"0x%04X\",\"rssi\":%d},"
                     "\"note\":\"该信道上没扫到 beacon；最强的网络在 CH%u\"}",
                     zb.channel, s_ch, s_pan, s_rssi, s_ch);
        } else {
            snprintf(buf, sizeof(buf),
                     "{\"status\":\"ok\",\"channel\":%u,\"pan_id\":null,"
                     "\"note\":\"该信道上没扫到 beacon，无法自动校验是否过期\"}",
                     zb.channel);
        }
    }
    return send_json_response(req, buf);
}

// ---- GET /api/v1/diag/ncp_line ----
/* 只读：采 UART 的 ctsn/rxd 内部信号电平（uart_bridge.h 有完整背景）。
   存在的唯一目的是回答"能不能用线路电平当 MG24 的非破坏性存活证据" ——
   当时的空闲探测靠发 ASH RST 拿证据，而那会把 Zigbee 协议栈打掉（T9.31 起无客户端不再探测，
   wd_probes.c 只用这条线路判据判死）。
   不参与任何判断，也不改任何配置。?ms=N 控制采样时长（默认 1000，上限 5000）。 */
static esp_err_t handle_diag_ncp_line(httpd_req_t *req)
{
    AUTH_REQUIRED(req);

    uint32_t ms = 1000;
    char q[32];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char v[12];
        if (httpd_query_key_value(q, "ms", v, sizeof(v)) == ESP_OK) {
            long n = strtol(v, NULL, 10);
            if (n > 0) ms = (uint32_t)n;
        }
    }

    uart_line_sample_t s;
    uart_bridge_sample_lines(ms, &s);

    const bridge_stats_t *bs = ezsp_bridge_get_stats();
    char buf[320];
    snprintf(buf, sizeof(buf),
             "{\"duration_ms\":%"PRIu32",\"samples\":%"PRIu32","
             "\"cts_deasserted\":%"PRIu32",\"cts_transitions\":%"PRIu32","
             "\"cts_longest_deassert_ms\":%"PRIu32",\"rxd_low\":%"PRIu32","
             "\"cts_now\":%u,\"rxd_now\":%u,\"tcp_clients\":%u}",
             s.duration_ms, s.samples, s.cts_deasserted, s.cts_transitions,
             s.cts_longest_deassert_ms, s.rxd_low, s.cts_now, s.rxd_now,
             bs ? bs->active_clients : 0);
    return send_json_response(req, buf);
}

// ---- GET /api/v1/diag/beacon ----
// 纯观测：用 OT 的 802.15.4 主动扫描找附近的 Zigbee/Thread beacon。
// 存在的唯一目的是回答"共存仲裁器放不放行 beacon 扫描"——不参与任何判断，
// 也不会自动填信道（哪个 beacon 是本机 MG24 的只能靠 RSSI 推断，是启发式）。
// ?dwell_ms=  每信道驻留（默认 200，钳 [20,1000]）
// ?channel=   只扫单个信道（11-26），省时间；缺省扫 ch11-26
static esp_err_t handle_diag_beacon(httpd_req_t *req)
{
    AUTH_REQUIRED(req);

    uint16_t dwell = 0;
    uint32_t mask  = 0;
    char q[64];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char v[8];
        if (httpd_query_key_value(q, "dwell_ms", v, sizeof(v)) == ESP_OK) {
            int d = atoi(v);
            if (d < 20)   d = 20;
            if (d > 1000) d = 1000;
            dwell = (uint16_t)d;
        }
        if (httpd_query_key_value(q, "channel", v, sizeof(v)) == ESP_OK) {
            int c = atoi(v);
            if (c >= 11 && c <= 26) mask = 1u << c;
        }
    }

    thread_br_beacon_scan_t r;
    esp_err_t err = thread_br_active_scan(dwell, mask, &r);
    if (err == ESP_ERR_INVALID_STATE)
        return send_error(req, 503, "thread_not_running_or_busy");
    if (err == ESP_ERR_NOT_SUPPORTED)
        return send_error(req, 501, "openthread_not_built_in");

    /* 超时也把已经收到的 beacon 报出去 —— 部分结果对诊断有价值，而
       completed=false 已经如实说明它没跑完。 */
    char buf[3072];
    int n = snprintf(buf, sizeof(buf),
        "{\"completed\":%s,\"dwell_ms\":%u,\"elapsed_ms\":%"PRIu32","
        "\"count\":%u,\"beacons\":[",
        r.completed ? "true" : "false", r.dwell_ms, r.elapsed_ms, r.count);
    for (uint8_t i = 0; i < r.count && n < (int)sizeof(buf); i++) {
        const thread_br_beacon_t *b = &r.nets[i];
        n += snprintf(buf + n, sizeof(buf) - n,
            "%s{\"pan_id\":\"0x%04X\",\"channel\":%u,\"rssi\":%d,\"lqi\":%u,"
            "\"ext_addr\":\"%02x%02x%02x%02x%02x%02x%02x%02x\"}",
            i ? "," : "", b->pan_id, b->channel, b->rssi, b->lqi,
            b->ext_addr[0], b->ext_addr[1], b->ext_addr[2], b->ext_addr[3],
            b->ext_addr[4], b->ext_addr[5], b->ext_addr[6], b->ext_addr[7]);
    }
    if (n < (int)sizeof(buf)) n += snprintf(buf + n, sizeof(buf) - n, "],");

    /* 同一次扫描的原始视角。raw_count > count 就证明 OT 丢了一部分 ——
       它的结果解析器要求源地址是 64 位扩展地址（scan_result.cpp:50），而
       Zigbee 协调器用 16 位短地址发 beacon。 */
    n += snprintf(buf + n, sizeof(buf) - n,
                  "\"pcap\":{\"rx_total\":%"PRIu32",\"beacon\":%"PRIu32","
                  "\"data\":%"PRIu32",\"ack\":%"PRIu32",\"cmd\":%"PRIu32","
                  "\"tx_total\":%"PRIu32"},"
                  "\"raw_count\":%u,\"raw_dropped\":%u,\"raw\":[",
                  r.pcap_rx_total, r.pcap_rx_beacon, r.pcap_rx_data,
                  r.pcap_rx_ack, r.pcap_rx_cmd, r.pcap_tx_total,
                  r.raw_count, r.raw_dropped);
    for (uint8_t i = 0; i < r.raw_count && n < (int)sizeof(buf); i++) {
        const thread_br_raw_beacon_t *b = &r.raw[i];
        n += snprintf(buf + n, sizeof(buf) - n,
            "%s{\"pan_id\":\"0x%04X\",\"channel\":%u,\"rssi\":%d,\"lqi\":%u,"
            "\"src_extended\":%s,\"src\":\"",
            i ? "," : "", b->pan_id, b->channel, b->rssi, b->lqi,
            b->src_extended ? "true" : "false");
        int alen = b->src_extended ? 8 : 2;
        for (int k = 0; k < alen && n < (int)sizeof(buf); k++)
            n += snprintf(buf + n, sizeof(buf) - n, "%02x", b->src_addr[k]);
        n += snprintf(buf + n, sizeof(buf) - n, "\",\"hdr\":\"");
        for (int k = 0; k < b->hdr_len && n < (int)sizeof(buf); k++)
            n += snprintf(buf + n, sizeof(buf) - n, "%02x", b->hdr[k]);
        n += snprintf(buf + n, sizeof(buf) - n, "\"}");
    }
    if (n < (int)sizeof(buf)) n += snprintf(buf + n, sizeof(buf) - n, "]}");
    if (n >= (int)sizeof(buf)) return send_error(req, 500, "json_overflow");
    return send_json_response(req, buf);
}

// ---- POST /api/v1/ota/zigbee ----
// Body = raw .gbl for the MG24 NCP. Received into a staging buffer here, then
// a background task drives the XMODEM transfer — the wire time is tens of
// seconds and must not be spent inside the HTTP handler.
static esp_err_t handle_ota_zigbee(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    if (ota_mg24_busy())   return send_error(req, 409, "mg24_ota_in_progress");
    if (ota_cloud_busy())  return send_error(req, 409, "cloud_ota_in_progress");
    if (s_ota_in_progress) return send_error(req, 409, "ota_already_in_progress");

    const int content_len = req->content_len;
    if (content_len <= 0) return send_error(req, 400, "empty_body");

    esp_err_t err = ota_mg24_begin((size_t)content_len, OTA_MG24_SOURCE_WEB);
    if (err == ESP_ERR_INVALID_SIZE) return send_error(req, 400, "file_too_large");
    if (err == ESP_ERR_NO_MEM)       return send_error(req, 500, "alloc_failed");
    if (err != ESP_OK)               return send_error(req, 409, "mg24_ota_busy");

    char *bounce = malloc(4096);
    if (!bounce) {
        ota_mg24_abort();
        return send_error(req, 500, "alloc_failed");
    }

    int received = 0;
    while (received < content_len) {
        int want = content_len - received;
        if (want > 4096) want = 4096;
        int ret = httpd_req_recv(req, bounce, want);
        if (ret <= 0) {
            ESP_LOGE(TAG, "MG24 OTA recv error at %d/%d (err=%d)",
                     received, content_len, ret);
            free(bounce);
            ota_mg24_abort();
            return send_error(req, 400, "recv_failed");
        }
        if (ota_mg24_write((const uint8_t *)bounce, ret) != ESP_OK) {
            free(bounce);
            ota_mg24_abort();
            return send_error(req, 400, "write_failed");
        }
        received += ret;
    }
    free(bounce);

    err = ota_mg24_commit();
    /* not_a_gbl_image / ncp_image_malformed / ncp_signature_invalid /
       ncp_image_unsigned / ncp_image_downgrade / ncp_image_wipe / ncp_image_unreadable
       — all decided before the MG24 was touched. A downgrade is a 409: the file is a
       perfectly good image, it conflicts with what this chip has already run (T9.25). */
    if (err == ESP_ERR_NOT_SUPPORTED) {
        const char *code = ota_mg24_last_reject_code();
        return send_error(req, strcmp(code, "ncp_image_downgrade") == 0 ? 409 : 400, code);
    }
    if (err == ESP_ERR_INVALID_SIZE)  return send_error(req, 400, "truncated_body");
    if (err != ESP_OK)                return send_error(req, 500, "commit_failed");

    /* 202: the image is accepted and the flash is running in the background.
       The caller polls /api/v1/ota/zigbee/status. */
    httpd_resp_set_status(req, "202 Accepted");
    return send_json_response(req,
        "{\"status\":\"accepted\",\"poll\":\"/api/v1/ota/zigbee/status\"}");
}

// ---- GET /api/v1/ota/zigbee/status ----
static esp_err_t handle_ota_zigbee_status(httpd_req_t *req)
{
    AUTH_REQUIRED(req);

    mg24_ota_status_t st;
    ota_mg24_get_status(&st);

    static const char *names[] = {
        "idle", "receiving", "preparing", "transferring",
        "finalizing", "success", "failed", "unconfirmed"   /* 次序 = mg24_ota_state_t */
    };
    const char *name = (st.state < sizeof(names) / sizeof(names[0]))
                     ? names[st.state] : "unknown";

    char detail[sizeof(st.detail)];
    json_sanitize_copy(detail, st.detail, sizeof(detail));

    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\"state\":\"%s\",\"percent\":%u,\"sent_blocks\":%"PRIu32","
        "\"total_blocks\":%"PRIu32",\"image_size\":%"PRIu32","
        "\"error\":%"PRId32",\"detail\":\"%s\"}",
        name, (unsigned)st.percent, st.sent_blocks, st.total_blocks,
        st.image_size, st.err, detail);
    return send_json_response(req, buf);
}

// ---- GET/POST /api/v1/ota/zigbee/unlock ----
// Developer unlock for unsigned NCP images (T1.2 step 3). POST {} arms a 30 s
// window; the physical button held 2-10 s and released inside it confirms and
// unsigned web uploads are then accepted for 10 min. POST {"lock":true}
// cancels. RAM only — a reboot locks. The cloud path ignores all of this.
// Password without the button, or button without the password, does nothing.
static esp_err_t send_unlock_status(httpd_req_t *req, const char *status)
{
    ota_mg24_unlock_status_t u;
    ota_mg24_unlock_get(&u);
    char buf[288];
    int n = snprintf(buf, sizeof(buf),
        "{\"status\":\"%s\",\"armed\":%s,\"arm_remaining_s\":%d,"
        "\"unlocked\":%s,\"unlock_remaining_s\":%d,\"signature_required\":%s,"
        "\"confirm_gesture\":\"hold the button 2-10 s, then release\"}",
        status, u.armed ? "true" : "false", u.arm_remaining_s,
        u.unlocked ? "true" : "false", u.unlock_remaining_s,
        u.signature_required ? "true" : "false");
    if (n < 0 || n >= (int)sizeof(buf)) return send_error(req, 500, "json_overflow");
    return send_json_response(req, buf);
}

static esp_err_t handle_ota_zigbee_unlock_get(httpd_req_t *req)
{
    AUTH_REQUIRED(req);
    return send_unlock_status(req, "ok");
}

static esp_err_t handle_ota_zigbee_unlock_post(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    bool lock = false;
    if (req->content_len > 0) {
        char body[64];
        if (recv_body(req, body, sizeof(body)) > 0) {
            cJSON *root = cJSON_Parse(body);
            if (root) {
                lock = cJSON_IsTrue(cJSON_GetObjectItem(root, "lock"));
                cJSON_Delete(root);
            }
        }
    }

    if (lock) {
        ota_mg24_unlock_lock();
        return send_unlock_status(req, "locked");
    }
    if (ota_mg24_unlock_arm() != ESP_OK) return send_error(req, 503, "mg24_ota_not_ready");
    return send_unlock_status(req, "armed");
}

// ---- POST /api/v1/zigbee/bootloader ----
// Parks the MG24 in the Gecko bootloader. The EZSP bridge stays down until
// the caller posts again with {"exit":true} — there is no auto-recovery, so
// the response says so explicitly.
static esp_err_t handle_zigbee_bootloader(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    bool want_exit = false;
    if (req->content_len > 0) {
        char body[64];
        if (recv_body(req, body, sizeof(body)) > 0) {
            cJSON *root = cJSON_Parse(body);
            if (root) {
                const cJSON *e = cJSON_GetObjectItem(root, "exit");
                want_exit = cJSON_IsTrue(e);
                cJSON_Delete(root);
            }
        }
    }

    esp_err_t err = want_exit ? ota_mg24_exit_bootloader()
                              : ota_mg24_enter_bootloader();
    if (err == ESP_ERR_NOT_SUPPORTED)
        return send_error(req, 501, "no_nboot_pin");
    if (err == ESP_ERR_INVALID_STATE)
        return send_error(req, 409, "bridge_busy");
    if (err == ESP_ERR_INVALID_RESPONSE)
        return send_error(req, 502, "mg24_did_not_enter_bootloader");
    if (err != ESP_OK)
        return send_error(req, 500, "bootloader_switch_failed");

    return send_json_response(req, want_exit
        ? "{\"status\":\"ok\",\"mode\":\"application\"}"
        : "{\"status\":\"ok\",\"mode\":\"bootloader\",\"note\":\"Zigbee is down "
          "until you POST {\\\"exit\\\":true} to this endpoint\"}");
}


// ---- POST /api/v1/diag/uart ----
// Measures the MG24 link in both directions. Runs on its own task: the sweep
// owns the UART for several seconds and would otherwise hold an httpd worker
// (and the bench itself drops the bridge, so the reply must go out first).
static uart_bench_result_t s_bench;
static volatile bool s_bench_running;
static volatile bool s_bench_valid;

static esp_err_t s_bench_err;

static void bench_task(void *arg)
{
    uart_bench_result_t r;
    /* Keep the result either way: a failure carries the note explaining why,
       and silently dropping it was how an earlier run reported "never_run"
       with no clue what had gone wrong. */
    s_bench_err = uart_bench_run(&r);
    s_bench = r;
    s_bench_valid = true;
    s_bench_running = false;
    vTaskDelete(NULL);
}

static int leg_json(char *buf, size_t size, const char *name,
                    const uart_bench_leg_t *l)
{
    return snprintf(buf, size,
        "\"%s\":{\"baud\":%d,\"bytes\":%"PRIu32",\"us\":%"PRIu32
        ",\"bps\":%"PRIu32",\"theory_bps\":%"PRIu32",\"pct\":%u}",
        name, l->baud, l->bytes, l->us, l->bps, l->theory_bps,
        (unsigned)l->efficiency_pct);
}

static esp_err_t handle_diag_uart(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    if (s_bench_running) return send_error(req, 409, "bench_running");
    if (ota_mg24_busy()) return send_error(req, 409, "mg24_ota_in_progress");
    if (ota_cloud_busy()) return send_error(req, 409, "cloud_ota_in_progress");

    s_bench_running = true;
    s_bench_valid = false;
    if (xTaskCreate(bench_task, "uart_bench", 4096, NULL, 5, NULL) != pdPASS) {
        s_bench_running = false;
        return send_error(req, 500, "task_create_failed");
    }
    httpd_resp_set_status(req, "202 Accepted");
    return send_json_response(req,
        "{\"status\":\"running\",\"poll\":\"/api/v1/diag/uart\"}");
}

// ---- GET /api/v1/diag/uart ----
static esp_err_t handle_diag_uart_result(httpd_req_t *req)
{
    AUTH_REQUIRED(req);

    if (s_bench_running)
        return send_json_response(req, "{\"state\":\"running\"}");
    if (!s_bench_valid)
        return send_json_response(req, "{\"state\":\"never_run\"}");

    char note[sizeof(s_bench.note)];
    json_sanitize_copy(note, s_bench.note, sizeof(note));

    char buf[512];
    int n = snprintf(buf, sizeof(buf), "{\"state\":\"done\",\"result\":%"PRId32",",
                     (int32_t)s_bench_err);
    n += leg_json(buf + n, sizeof(buf) - n, "tx", &s_bench.tx);
    n += snprintf(buf + n, sizeof(buf) - n,
        ",\"tx_blocked\":%s,\"ncp_alive\":%s,\"ncp_reset_ready_us\":%"PRIu32
        ",\"uart_rx_errors\":%"PRIu32",\"note\":\"%s\"}",
        s_bench.tx_blocked ? "true" : "false",
        s_bench.ncp_alive ? "true" : "false",
        s_bench.ncp_reset_ready_us, s_bench.uart_rx_errors, note);

    if (n >= (int)sizeof(buf)) return send_error(req, 500, "json_overflow");
    return send_json_response(req, buf);
}

// ---- GET /api/v1/diag/heap ----
/* /api/v1/status 的 free_heap / min_free_heap 是 MALLOC_CAP_DEFAULT 的**求和**
   （esp_system_chip.c: heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT)，
   heap_caps.c:306 逐个 heap 累加）。内部 SRAM 与 PSRAM 都带 DEFAULT 位，于是
   两者被加在一起，单看总数无法判断到底是哪块见底了——而只有内部 SRAM 见底才
   危险（DMA / WiFi / 任务栈只能用它）。这里按 cap 拆开，仅做读取。

   注意 min 的语义：每块 heap 各自记自己的历史最低点，累加的结果**不对应任何
   一个真实时刻**。所以要分开看，不要把总和当成"曾经差点没内存"。 */
static int heap_cap_json(char *buf, size_t size, const char *name, uint32_t caps)
{
    return snprintf(buf, size,
        "\"%s\":{\"free\":%u,\"min_free\":%u,\"largest\":%u,\"total\":%u}",
        name,
        (unsigned)heap_caps_get_free_size(caps),
        (unsigned)heap_caps_get_minimum_free_size(caps),
        (unsigned)heap_caps_get_largest_free_block(caps),
        (unsigned)heap_caps_get_total_size(caps));
}

static esp_err_t handle_diag_heap(httpd_req_t *req)
{
    AUTH_REQUIRED(req);

    char buf[2048];
    int n = snprintf(buf, sizeof(buf), "{");
    n += heap_cap_json(buf + n, sizeof(buf) - n, "default", MALLOC_CAP_DEFAULT);
    n += snprintf(buf + n, sizeof(buf) - n, ",");
    n += heap_cap_json(buf + n, sizeof(buf) - n, "internal",
                       MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT);
    n += snprintf(buf + n, sizeof(buf) - n, ",");
    n += heap_cap_json(buf + n, sizeof(buf) - n, "spiram",
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_DEFAULT);
    n += snprintf(buf + n, sizeof(buf) - n, ",");
    n += heap_cap_json(buf + n, sizeof(buf) - n, "dma", MALLOC_CAP_DMA);

    uint32_t af_count = 0, af_size = 0, af_caps = 0;
    watchdog_get_alloc_failures(&af_count, &af_size, &af_caps);
    n += snprintf(buf + n, sizeof(buf) - n,
        ",\"alloc_failures\":{\"count\":%u,\"last_size\":%u,\"last_caps\":%u}",
        (unsigned)af_count, (unsigned)af_size, (unsigned)af_caps);

    /* 启动轨迹：定位是哪一步吃掉内部 SRAM */
    const heap_mark_t *marks = NULL;
    size_t nm = watchdog_heap_marks(&marks);
    n += snprintf(buf + n, sizeof(buf) - n, ",\"boot_trace\":[");
    for (size_t i = 0; i < nm && n < (int)sizeof(buf); i++) {
        n += snprintf(buf + n, sizeof(buf) - n,
            "%s{\"step\":\"%s\",\"internal\":%u,\"dma\":%u,\"ms\":%u}",
            i ? "," : "", marks[i].step,
            (unsigned)marks[i].internal_free, (unsigned)marks[i].dma_free,
            (unsigned)marks[i].uptime_ms);
    }
    n += snprintf(buf + n, sizeof(buf) - n, "]}");

    if (n >= (int)sizeof(buf)) return send_error(req, 500, "json_overflow");
    return send_json_response(req, buf);
}


// ---- GET /api/v1/diag/ed ----
/* 能量扫描为什么拿不到数据 —— 把推断变成数字。
   用法：先 GET 取基线，再 GET /api/v1/spectrum/scan 跑一次扫描，再 GET 本端点，
   看 last_scan 里的增量。判读见 docs 与 thread_br.h 的注释：
     ed_scan_coex ≈ 16   → 每个信道都被共存仲裁器拒了
     ed_scan_coex ≈ 0 且 ed_abort 有数 → 不是共存，是别的原因中止
     两者都是 0          → ED 压根没启动
   ed_irq_armed=false 说明 ED 类 abort 中断从未使能，ed_failed 回调是死代码，
   扫描属于"静默死亡"。此时可 POST ?arm=1 打开中断位再测一次（仍是纯观测）。 */
static esp_err_t handle_diag_ed(httpd_req_t *req)
{
    AUTH_REQUIRED(req);

    thread_br_ed_diag_t d;
    esp_err_t err = thread_br_ed_diag_read(&d);
    if (err == ESP_ERR_NOT_SUPPORTED) return send_error(req, 501, "openthread_not_built");
    if (err != ESP_OK)                return send_error(req, 503, "thread_not_running");

    thread_br_scan_diag_t s;
    thread_br_scan_diag_read(&s);

    char buf[640];
    int n = snprintf(buf, sizeof(buf),
        "{\"counters\":{\"ed_scan_coex\":%u,\"ed_abort\":%u,"
        "\"rx_abort_coex\":%u,\"cca_fail\":%u},"
        "\"rx_abort_event_en\":\"0x%08x\",\"ed_irq_armed\":%s,"
        "\"ed_failed_cb\":%u",
        (unsigned)d.ed_scan_coex, (unsigned)d.ed_abort,
        (unsigned)d.rx_abort_coex, (unsigned)d.cca_fail,
        (unsigned)d.rx_abort_event_en, d.ed_irq_armed ? "true" : "false",
        (unsigned)d.ed_failed_cb);

    if (s.valid && n > 0 && n < (int)sizeof(buf)) {
        n += snprintf(buf + n, sizeof(buf) - n,
            ",\"last_scan\":{\"valid_count\":%u,\"valid_mask\":\"0x%04x\","
            "\"duration_ms\":%u,\"elapsed_ms\":%u,\"completed_naturally\":%s,"
            "\"ed_scan_coex\":%u,\"ed_abort\":%u,\"rx_abort_coex\":%u,"
            "\"cca_fail\":%u,\"ed_failed_cb\":%u}",
            s.valid_count, s.valid_mask, s.duration_ms, (unsigned)s.elapsed_ms,
            s.completed_naturally ? "true" : "false",
            (unsigned)s.ed_scan_coex, (unsigned)s.ed_abort,
            (unsigned)s.rx_abort_coex, (unsigned)s.cca_fail,
            (unsigned)s.ed_failed_cb);
    }
    if (n > 0 && n < (int)sizeof(buf)) {
        n += snprintf(buf + n, sizeof(buf) - n, "}");
    }

    if (n < 0 || n >= (int)sizeof(buf)) return send_error(req, 500, "json_overflow");
    return send_json_response(req, buf);
}

// ---- POST /api/v1/diag/ed?arm=1 ----
/* 使能/还原 ED_ABORT 与 ED_COEX_REJECT 两个 RX-abort 事件位。
   仍是纯观测：ed_failed 回调只累加计数，不向 OpenThread 转发，扫描结果不变。
   开了之后 ed_failed_cb 若开始有数，就证明"中断没使能"是 ED 静默死亡的直接原因。 */
static esp_err_t handle_diag_ed_arm(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char q[32] = {0};
    bool arm = false;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char v[8] = {0};
        if (httpd_query_key_value(q, "arm", v, sizeof(v)) == ESP_OK) {
            arm = (v[0] == '1' || v[0] == 't');
        }
    }

    esp_err_t err = thread_br_ed_diag_arm_irq(arm);
    if (err == ESP_ERR_NOT_SUPPORTED) return send_error(req, 501, "openthread_not_built");
    if (err != ESP_OK)                return send_error(req, 503, "thread_not_running");

    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ed_irq_armed\":%s}", arm ? "true" : "false");
    return send_json_response(req, buf);
}

#if !CONFIG_BITENGINE_NET_ETH_ONLY   /* 纯以太网剖面没有 Wi-Fi，不编也不注册（docs/15 设计 2.2） */
// ---- GET/POST /api/v1/diag/wifi_probe ----
/* 从 WiFi STA 口持续 ping 一个目标，记录网关自己看到的"洞"（见 wifi_probe.h）。
   POST ?enable=1&target=<IP> 启动，?enable=0 停止。出厂不启动，重启即停。 */
static esp_err_t send_wifi_probe_status(httpd_req_t *req)
{
    wifi_probe_status_t st;
    wifi_probe_get_status(&st);
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "{\"running\":%s,\"target\":\"%s\",\"sent\":%" PRIu32 ",\"replied\":%" PRIu32
        ",\"missed\":%" PRIu32 ",\"holes\":%" PRIu32 ",\"cur_miss_run\":%" PRIu32
        ",\"max_hole_ms\":%" PRIu32 ",\"rssi\":%d,\"recent\":[",
        st.running ? "true" : "false", st.target, st.sent, st.replied, st.missed,
        st.holes, st.cur_miss_run, st.max_hole_ms, st.rssi);
    uint32_t cnt = st.holes < WIFI_PROBE_HOLE_RING ? st.holes : WIFI_PROBE_HOLE_RING;
    for (uint32_t k = 0; k < cnt && n < (int)sizeof(buf) - 48; k++) {
        /* 按时间顺序：最老的在 ring 里的位置是 holes % RING（满了之后） */
        uint32_t i = (st.holes - cnt + k) % WIFI_PROBE_HOLE_RING;
        n += snprintf(buf + n, sizeof(buf) - n, "%s{\"start_ms\":%" PRIu32 ",\"ms\":%" PRIu32 "}",
                      k ? "," : "", st.hole_start_ms[i], st.hole_ms[i]);
    }
    if (n >= (int)sizeof(buf) - 3) return send_error(req, 500, "response_truncated");
    n += snprintf(buf + n, sizeof(buf) - n, "]}");
    return send_json_response(req, buf);
}

static esp_err_t handle_diag_wifi_probe_get(httpd_req_t *req)
{
    AUTH_REQUIRED(req);
    return send_wifi_probe_status(req);
}

static esp_err_t handle_diag_wifi_probe_set(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char q[80] = {0}, v[8] = {0}, target[16] = {0};
    bool enable = false;
    uint32_t interval = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "enable", v, sizeof(v)) == ESP_OK)
            enable = (v[0] == '1' || v[0] == 't');
        httpd_query_key_value(q, "target", target, sizeof(target));
        if (httpd_query_key_value(q, "interval", v, sizeof(v)) == ESP_OK) interval = (uint32_t)atoi(v);
    }
    /* 诊断旋钮（2026-09-14 查 WiFi 空闲洞用）：?coex154=0 告诉共存层 802.15.4 不活动，
       =1 恢复。只改共存仲裁，不碰 OT/射频；重启即复原。别在生产路径上用。 */
    if (httpd_query_key_value(q, "coex154", v, sizeof(v)) == ESP_OK) {
        if (v[0] == '0') esp_coex_ieee802154_status_disable();
        else             esp_coex_ieee802154_status_enable();
        ESP_LOGW(TAG, "diag: coex 802.15.4 status %s", v[0] == '0' ? "DISABLED" : "enabled");
        if (httpd_query_key_value(q, "enable", v, sizeof(v)) != ESP_OK)
            return send_wifi_probe_status(req);   /* 只切共存，不动探针 */
    }
    esp_err_t err = enable ? wifi_probe_start(target, interval) : wifi_probe_stop();
    if (err == ESP_ERR_INVALID_ARG)   return send_error(req, 400, "bad_target");
    if (err == ESP_ERR_INVALID_STATE) return send_error(req, 503, "wifi_not_ready");
    if (err != ESP_OK)                return send_error(req, 500, "probe_failed");
    return send_wifi_probe_status(req);
}

#endif /* !CONFIG_BITENGINE_NET_ETH_ONLY */

// ---- POST /api/v1/thread/readvertise ----
/* 手动把 SRP 代播的 mDNS 服务全部重新宣告一遍（goodbye + 重新宣告）。
   本来是开机后自动做的，开出来是为了能在不重启网关的前提下验证效果——
   否则每测一次都要断电，等子设备归队 4-6 分钟。
   立即返回 202：实际动作在 srp_readv 任务里做，最长 5s 后开始，
   每个服务中间还有一段 goodbye 间隔。做完的结果从 GET 同一路径读。 */
static esp_err_t handle_thread_readvertise(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    esp_err_t err = srp_readv_trigger_all();
    if (err == ESP_ERR_INVALID_STATE) return send_error(req, 503, "thread_not_running");
    if (err != ESP_OK)                return send_error(req, 501, "readvertise_unavailable");

    /* ⚠️ 出厂构型 FORCE_BYE=n 时，readvertise_one() 直接 return ESP_OK —— 只跟踪、
       **不动 mDNS 记录**。此前这里一律回 202 "scheduled"，调用方无从知道
       什么都不会发生（T3.6）。现在把开关状态一并说出来，并且关着时回 200 而不是
       202：202 的语义是"已受理、稍后执行"，而这里根本不会有后续动作。 */
#if CONFIG_BITENGINE_SRP_READV_FORCE_BYE
    httpd_resp_set_status(req, "202 Accepted");
    return send_json_response(req,
        "{\"status\":\"scheduled\",\"force_bye\":true,"
        "\"poll\":\"/api/v1/thread/readvertise\"}");
#else
    return send_json_response(req,
        "{\"status\":\"tracking_only\",\"force_bye\":false,"
        "\"note\":\"FORCE_BYE is off in this build: services are tracked but mDNS "
        "records are left untouched, so nothing will be re-announced. "
        "Needs a purpose-built firmware to exercise.\","
        "\"poll\":\"/api/v1/thread/readvertise\"}");
#endif
}

// ---- GET /api/v1/thread/readvertise ----
static esp_err_t handle_thread_readvertise_stats(httpd_req_t *req)
{
    AUTH_REQUIRED(req);

    uint32_t done = 0, failed = 0, tracked = 0;
    srp_readv_get_stats(&done, &failed, &tracked);
    uint32_t lock_us = 0, lock_max_us = 0, scanned = 0;
    srp_readv_get_scan_stats(&lock_us, &lock_max_us, &scanned);

    char buf[224];
    /* force_bye 一并给出：关着时 readvertised 会一路涨，但那只是"跟踪表标了 done"，
       并没有真的重新宣告过任何记录。不说清楚的话这个计数会被当成功能在工作。 */
    snprintf(buf, sizeof(buf),
             "{\"readvertised\":%u,\"failed\":%u,\"tracked\":%u,"
             "\"scan_lock_us\":%u,\"scan_lock_max_us\":%u,\"scan_services\":%u,\"force_bye\":%s}",
             (unsigned)done, (unsigned)failed, (unsigned)tracked,
             (unsigned)lock_us, (unsigned)lock_max_us, (unsigned)scanned,
#if CONFIG_BITENGINE_SRP_READV_FORCE_BYE
             "true"
#else
             "false"
#endif
             );
    return send_json_response(req, buf);
}

// ---- GET /api/v1/diag/watchdog ----
/* 看门狗 v2 的统一诊断入口（docs/04_watchdog_design_v2.md 第十节）。
   一次请求给出四层的全部结论：任务心跳、子系统探针、资源守卫、恢复引擎，
   外加取证历史。排查时不必再去翻串口日志。 */
static esp_err_t handle_diag_watchdog(httpd_req_t *req)
{
    AUTH_REQUIRED(req);

    wd_resource_t   res;
    wd_probe_view_t pv;
    wd_recovery_view_t rv;
    wd_resource_get(&res);
    wd_probes_get(&pv);
    wd_recovery_get(&rv);

    wd_task_view_t tasks[WD_MAX_TASKS];
    size_t nt = wd_registry_snapshot(tasks, WD_MAX_TASKS);

    wd_forensic_rec_t hist[WD_HISTORY_MAX];
    size_t nh = wd_forensics_history(hist, WD_HISTORY_MAX);

    /* 分块发送：任务表 + 历史最多可到 ~2KB，一个栈缓冲装不下又不想 malloc
       （看门狗诊断在内存紧张时最该可用，这时 malloc 恰恰可能失败）。 */
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "{\"safe_mode\":%s,\"safe_reason\":\"%s\","
        "\"self_heal_count\":%u,\"last_action\":\"%s\",\"last_action_ts\":%u,"
        "\"levels\":{\"task\":%d,\"subsys\":%d,\"resource\":%d,\"periph\":%d},"
        /* 系统里全部 FreeRTOS 任务的个数（不只是下面登记进看门狗的那些）。PSRAM 栈的常驻
           任务（zb_check、mdns_refresh…）不在登记表里，任务泄漏只能靠它看出来：L3 子系统重启 /
           边界路由换口前后应当相等（2026-10-06；此前 zb_check 每次 Thread 重启多建一个）。
           Thread 自 2026-10-07 起没有 L3（T9.18），受控对比改在 TC-W07（httpd L3）做。 */
        "\"task_count\":%u,",
        rv.safe_mode ? "true" : "false", wd_safe_mode_reason(),
        rv.self_heal_count, rv.last_action ? rv.last_action : "none",
        (unsigned)rv.last_action_ts,
        (int)rv.level[WD_FAULT_TASK_STALL], (int)rv.level[WD_FAULT_SUBSYS_DEAD],
        (int)rv.level[WD_FAULT_RESOURCE],   (int)rv.level[WD_FAULT_PERIPH_DEAD],
        (unsigned)uxTaskGetNumberOfTasks());
    httpd_resp_set_type(req, WEBUI_JSON_CONTENT_TYPE);
    httpd_resp_send_chunk(req, buf, n);

    n = snprintf(buf, sizeof(buf),
        "\"resource\":{\"alloc_failures\":%u,\"alloc_fail_window\":%u,"
        "\"last_size\":%u,\"last_caps\":%u,"
        "\"internal_free\":%u,\"internal_min\":%u,"
        "\"dma_free\":%u,\"dma_min\":%u,\"dma_largest\":%u,"
        "\"stack_low_task\":\"%s\",\"stack_low_bytes\":%u},",
        (unsigned)res.alloc_fail_total, (unsigned)res.alloc_fail_window,
        (unsigned)res.alloc_fail_last_size, (unsigned)res.alloc_fail_last_caps,
        (unsigned)res.internal_free, (unsigned)res.internal_min,
        (unsigned)res.dma_free, (unsigned)res.dma_min, (unsigned)res.dma_largest,
        res.stack_low_task ? res.stack_low_task : "",
        (unsigned)res.stack_low_bytes);
    httpd_resp_send_chunk(req, buf, n);

    n = snprintf(buf, sizeof(buf),
        "\"probes\":{\"mg24_ok\":%s,\"mg24_judged\":%s,\"mg24_last_ok_s\":%u,"
        "\"thread_ok\":%s,\"thread_running\":%s,"
        "\"network_ok\":%s,\"httpd_ok\":%s},\"tasks\":[",
        pv.mg24_ok ? "true" : "false", pv.mg24_judged ? "true" : "false",
        (unsigned)pv.mg24_last_ok_s,
        pv.thread_ok ? "true" : "false", pv.thread_running ? "true" : "false",
        pv.network_ok ? "true" : "false", pv.httpd_ok ? "true" : "false");
    httpd_resp_send_chunk(req, buf, n);

    for (size_t i = 0; i < nt; i++) {
        static const char *sev_str[] = { "observe", "recover", "fatal" };
        n = snprintf(buf, sizeof(buf),
            "%s{\"name\":\"%s\",\"deadline_ms\":%u,\"age_ms\":%u,"
            "\"miss\":%u,\"sev\":\"%s\",\"suspended\":%s,"
            "\"stack_left\":%u}",
            i ? "," : "", tasks[i].name ? tasks[i].name : "?",
            (unsigned)tasks[i].deadline_ms, (unsigned)tasks[i].age_ms,
            (unsigned)tasks[i].miss_count,
            sev_str[tasks[i].severity <= WD_SEV_FATAL ? tasks[i].severity : 0],
            tasks[i].suspended ? "true" : "false",
            (unsigned)tasks[i].stack_left);
        httpd_resp_send_chunk(req, buf, n);
    }

    httpd_resp_send_chunk(req, "],\"history\":[", HTTPD_RESP_USE_STRLEN);
    for (size_t i = 0; i < nh; i++) {
        char safe_detail[24];
        json_sanitize_copy(safe_detail, hist[i].detail, sizeof(safe_detail));
        n = snprintf(buf, sizeof(buf),
            "%s{\"uptime_s\":%u,\"class\":%u,\"level\":%u,\"detail\":\"%s\","
            "\"internal_free\":%u,\"dma_free\":%u,\"alloc_fails\":%u}",
            i ? "," : "", (unsigned)hist[i].uptime_s, hist[i].fault_class,
            hist[i].level, safe_detail, (unsigned)hist[i].internal_free,
            (unsigned)hist[i].dma_free, (unsigned)hist[i].alloc_fails);
        httpd_resp_send_chunk(req, buf, n);
    }
    httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ---- POST /api/v1/diag/safemode/exit ----
/* 直连型 + Muse：请求体可带 {"muse_off":true} —— 退出之前先记下"下次开机先关掉 Muse"（docs/15 附录 E33：反复重启的
   正是 Muse 时，安全模式下 hub_store 不起，/muse/config 关不了它，一退出又照开、接着崩）。先判安全模式再写：
   不在安全模式时什么都不记。HA 型不读请求体，与以前一样。 */
static esp_err_t handle_safemode_exit(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    if (!wd_safe_mode()) return send_error(req, 409, "not_in_safe_mode");
#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
    bool muse_off = false;
    if (req->content_len > 0) {
        char body[64];
        if (recv_body(req, body, sizeof(body)) <= 0) return send_error(req, 400, "invalid_body");
        cJSON *root = cJSON_Parse(body);
        if (!cJSON_IsObject(root)) {
            cJSON_Delete(root);
            return send_error(req, 400, "invalid_json");
        }
        const cJSON *m = cJSON_GetObjectItem(root, "muse_off");
        const bool bad = m && !cJSON_IsBool(m);
        muse_off = cJSON_IsTrue(m);
        cJSON_Delete(root);
        if (bad) return send_error(req, 400, "invalid_params");
    }
    /* 没记下就别退出：退出了 Muse 照开，用户以为关了 */
    if (muse_off && muse_hold_request() != ESP_OK) return send_error(req, 500, "muse_hold_failed");
    send_json_response(req, muse_off ? "{\"status\":\"ok\",\"note\":\"rebooting\",\"muse_off\":true}"
                                     : "{\"status\":\"ok\",\"note\":\"rebooting\"}");
#else
    send_json_response(req, "{\"status\":\"ok\",\"note\":\"rebooting\"}");
#endif
    wd_safe_mode_exit();   /* 不返回 */
    return ESP_OK;
}

// ---- GET /api/v1/diag/srp ---- SRP server 端口与已注册计数
// hosts/services 是真实设备数的地面真值（不受 /node/children 那个 2048B 缓冲的
// 无标记截断影响）；port 是从局域网侧压测 SRP 的前提 —— 它在 53535-53554 里随机
// 选一个并持久化，从外部探测不出来（畸形 UPDATE 静默丢弃、关闭端口也不回 ICMPv6）。
static esp_err_t handle_diag_srp(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    uint16_t port = 0; int hosts = 0, services = 0;
    esp_err_t err = thread_br_srp_info(&port, &hosts, &services);
    if (err == ESP_ERR_INVALID_STATE) return send_error(req, 409, "thread_not_running");
    if (err != ESP_OK)                return send_error(req, 503, "ot_lock_timeout");

    /* 过载保护读数（thread_br/ot_cpu_guard.c）：idle_starve_max_ms = 开机以来 IDLE 最长一次没跑到，看它离 5000
       （任务看门狗）多远；守卫失效的表现是重启（uptime 归零），这个数到不了 5000 */
    uint32_t yields = 0, starve_ms = 0;
    thread_br_cpu_guard_stats(&yields, &starve_ms);
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"port\":%u,\"hosts\":%d,\"services\":%d,"
             "\"cpu_guard_yields\":%" PRIu32 ",\"idle_starve_max_ms\":%" PRIu32 "}",
             (unsigned)port, hosts, services, yields, starve_ms);
    return send_json_response(req, buf);
}

#if CONFIG_BITENGINE_ENABLE_MDNS_LOAD
// ---- GET/POST /api/v1/diag/mdns_load ---- 合成 mDNS 负载（扩容实验 C 方案）
// ⚠️ 出厂关。这些记录会真的播到局域网上，HA 会真的看到一堆假 Matter 设备。
static esp_err_t handle_diag_mdns_load_get(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"active\":%d,\"max\":%d,\"fabric\":\"%s\"}",
             mdns_load_count(), MDNS_LOAD_MAX, mdns_load_fabric());
    return send_json_response(req, buf);
}

static esp_err_t handle_diag_mdns_load_set(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[64];
    int len = recv_body(req, body, sizeof(body));
    if (len <= 0) return send_error(req, 400, "empty_body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");
    const cJSON *jn = cJSON_GetObjectItem(root, "n");
    if (!cJSON_IsNumber(jn)) { cJSON_Delete(root); return send_error(req, 400, "missing_n"); }
    int want = jn->valueint;
    /* 可选 fabric：等价性比对时设成真实 fabric，排除 subtype 名字不同的字节差。
       必须在改 n 之前处理 —— set_fabric 只在 active==0 时允许。 */
    const cJSON *jf = cJSON_GetObjectItem(root, "fabric");
    char fabric[20] = {0};
    if (cJSON_IsString(jf) && jf->valuestring) {
        strlcpy(fabric, jf->valuestring, sizeof(fabric));
    }
    cJSON_Delete(root);

    if (fabric[0]) {
        esp_err_t ferr = mdns_load_set_fabric(fabric);
        if (ferr == ESP_ERR_INVALID_STATE)
            return send_error(req, 409, "fabric_locked_while_active");
        if (ferr != ESP_OK)
            return send_error(req, 400, "bad_fabric");
    }

    int active = 0;
    esp_err_t first = ESP_OK;
    if (mdns_load_set(want, &active, &first) != ESP_OK)
        return send_error(req, 400, "n_out_of_range");

    /* ⚠️ 撞到 mDNS 上限**不是**调用错误，是这个实验要找的结果 —— 所以回 200，
       把"停在第几条、被什么挡住"放进 body。回 5xx 会让调用方以为是故障。 */
    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"requested\":%d,\"active\":%d,\"max\":%d,\"fabric\":\"%s\","
             "\"capped\":%s,\"error\":\"%s\"}",
             want, active, MDNS_LOAD_MAX, mdns_load_fabric(),
             (active < want) ? "true" : "false",
             (first == ESP_OK) ? "" : esp_err_to_name(first));
    return send_json_response(req, buf);
}
#endif /* CONFIG_BITENGINE_ENABLE_MDNS_LOAD */

#if CONFIG_BITENGINE_DIAG_THREAD_BACKBONE
// ---- POST /api/v1/diag/thread/backbone {"to":"eth"|"wifi"} ----
/* 测试专用（Kconfig 默认关）：验证"运行中换边界路由 backbone"的机制，见测试 6c。 */
static esp_err_t handle_diag_thread_backbone(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[64];
    int len = recv_body(req, body, sizeof(body));
    if (len <= 0) return send_error(req, 400, "empty_body");
    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");
    char to[8] = "";
    const cJSON *t = cJSON_GetObjectItem(root, "to");
    if (cJSON_IsString(t)) strlcpy(to, t->valuestring, sizeof(to));
    cJSON_Delete(root);
    if (strcmp(to, "eth") && strcmp(to, "wifi")) return send_error(req, 400, "to_must_be_eth_or_wifi");

    thread_br_backbone_switch_t r;
    esp_err_t err = thread_br_switch_backbone(to, 10000, &r);
    if (err == ESP_ERR_NOT_FOUND)     return send_error(req, 404, "netif_not_found");
    if (err == ESP_ERR_INVALID_STATE) return send_error(req, 409, "thread_not_running_or_netif_down");
    if (err == ESP_ERR_NOT_FINISHED)  return send_error(req, 409, "switch_in_progress");
    if (err == ESP_ERR_TIMEOUT)       return send_error(req, 504, "switch_timeout");
    if (err == ESP_ERR_NO_MEM)        return send_error(req, 500, "no_mem");

    char buf[224];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"%s\",\"from\":\"%s\",\"to\":\"%s\",\"deinit\":\"%s\","
             "\"init\":\"%s\",\"ms\":%" PRIu32 ",\"backbone\":\"%s\"}",
             (err == ESP_OK && r.deinit_err == ESP_OK) ? "ok" : "error",
             r.from ? r.from : "", r.to ? r.to : "",
             esp_err_to_name(r.deinit_err), esp_err_to_name(r.init_err), r.ms,
             thread_br_backbone_name());
    return send_json_response(req, buf);
}
#endif /* CONFIG_BITENGINE_DIAG_THREAD_BACKBONE */

#if CONFIG_BITENGINE_DIAG_SVC_ID
// ---- POST /api/v1/diag/svc_id {"s":"eth"|"wifi"} ----
/* 测试专用（Kconfig 默认关）：改服务身份记录、重启后生效 —— 在先插网线装的板子上模拟"先用 WiFi 装"。
   见 docs/test_cases_stability.md 测试 6e；设计 docs/16。 */
static esp_err_t handle_diag_svc_id(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[48];
    int len = recv_body(req, body, sizeof(body));
    if (len <= 0) return send_error(req, 400, "empty_body");
    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");
    char sv[8] = "";
    const cJSON *t = cJSON_GetObjectItem(root, "s");
    if (cJSON_IsString(t)) strlcpy(sv, t->valuestring, sizeof(sv));
    cJSON_Delete(root);
    esp_err_t err = network_mgr_diag_svc_id_set(sv);
    if (err == ESP_ERR_INVALID_ARG)   return send_error(req, 400, "s_must_be_eth_or_wifi");
    if (err == ESP_ERR_INVALID_STATE) return send_error(req, 409, "svc_identity_not_active");
    return send_json_response(req, "{\"status\":\"ok\",\"note\":\"reboot to apply\"}");
}

// ---- GET /api/v1/diag/svc_id ---- 两张卡的 DHCP 状态（长时间续租测试，测试 6e R5）
static esp_err_t handle_diag_svc_id_get(httpd_req_t *req)
{
    AUTH_REQUIRED(req);
    char buf[640];
    if (network_mgr_diag_dhcp_json(buf, sizeof(buf)) < 0) return send_error(req, 500, "dhcp_read_failed");
    return send_json_response(req, buf);
}
#endif /* CONFIG_BITENGINE_DIAG_SVC_ID */

#if CONFIG_BITENGINE_DIAG_HUBDB
// ---- GET/POST /api/v1/diag/hubdb ---- 开发期：上板验 hub_store（docs/15 设计 2.5；Kconfig 默认关）
/* GET：hub_store 的状态 + 探针键 hub/diag_probe 的值（读己之写，防抖中的也读得到）。
   POST {"text":"…"}（≤ 64 字节）写探针、{"erase":true} 删、{"flush":true} 立即落盘。 */
#include "hub_store.h"
#define HUBDB_PROBE_NS   HUB_STORE_NS_HUB
#define HUBDB_PROBE_KEY  "diag_probe"
#define HUBDB_PROBE_VER  1
#define HUBDB_PROBE_MAX  64

static esp_err_t handle_diag_hubdb_get(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    hub_store_status_t st;
    hub_store_get_status(&st);
    char probe[HUBDB_PROBE_MAX + 1] = "";
    size_t len = HUBDB_PROBE_MAX;
    uint8_t ver = 0;
    esp_err_t perr = st.ready ? hub_store_get(HUBDB_PROBE_NS, HUBDB_PROBE_KEY, HUBDB_PROBE_VER, &ver,
                                              probe, &len, 2000)
                              : ESP_ERR_INVALID_STATE;
    if (perr == ESP_OK) probe[len < HUBDB_PROBE_MAX ? len : HUBDB_PROBE_MAX] = '\0';
    char esc[HUBDB_PROBE_MAX * 6 + 1];      /* 最坏每字节转成 \u00XX */
    webui_json_escape(esc, sizeof(esc), probe);
    char buf[640];
    int n = snprintf(buf, sizeof(buf),
        "{\"ready\":%s,\"reset_at_init\":%s,\"flush_on_restart\":%s,\"init_err\":\"%s\",\"pending\":%d,"
        "\"writes\":%" PRIu32 ",\"coalesced\":%" PRIu32 ",\"retries\":%" PRIu32 ",\"failures\":%" PRIu32 ","
        "\"refused_newer\":%" PRIu32 ",\"last_err\":%d,\"probe_err\":\"%s\",\"probe\":%s%s%s}",
        st.ready ? "true" : "false", st.reset_at_init ? "true" : "false", st.flush_on_restart ? "true" : "false",
        esp_err_to_name(st.init_err),
        st.pending, st.writes, st.coalesced, st.retries, st.failures, st.refused_newer, st.last_err,
        esp_err_to_name(perr), perr == ESP_OK ? "\"" : "", perr == ESP_OK ? esc : "null", perr == ESP_OK ? "\"" : "");
    if (n < 0 || n >= (int)sizeof(buf)) return send_error(req, 500, "response_too_large");
    return send_json_response(req, buf);
}

static esp_err_t handle_diag_hubdb_post(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    char body[160];
    int blen = recv_body(req, body, sizeof(body));
    if (blen == -2) return send_error(req, 413, "body_too_large");
    if (blen <= 0) return send_error(req, 400, "empty_body");
    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");
    const cJSON *text  = cJSON_GetObjectItem(root, "text");
    const cJSON *erase = cJSON_GetObjectItem(root, "erase");
    const cJSON *flush = cJSON_GetObjectItem(root, "flush");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    const char *what = "nothing";
    if (cJSON_IsString(text)) {
        size_t tl = strlen(text->valuestring);
        if (tl > HUBDB_PROBE_MAX) {
            cJSON_Delete(root);
            return send_error(req, 400, "text_too_long");
        }
        err = hub_store_put(HUBDB_PROBE_NS, HUBDB_PROBE_KEY, HUBDB_PROBE_VER, text->valuestring, tl);
        what = "queued";
    } else if (cJSON_IsTrue(erase)) {
        err = hub_store_erase(HUBDB_PROBE_NS, HUBDB_PROBE_KEY);
        what = "erase_queued";
    } else if (cJSON_IsTrue(flush)) {
        err = hub_store_flush(5000);
        what = "flushed";
    }
    cJSON_Delete(root);
    if (err == ESP_ERR_INVALID_ARG)   return send_error(req, 400, "need_text_erase_or_flush");
    if (err == ESP_ERR_INVALID_STATE) return send_error(req, 409, "hub_store_not_ready");
    if (err != ESP_OK)                return send_error(req, 500, esp_err_to_name(err));
    char out[64];
    snprintf(out, sizeof(out), "{\"status\":\"ok\",\"result\":\"%s\"}", what);
    return send_json_response(req, out);
}
#endif /* CONFIG_BITENGINE_DIAG_HUBDB */

#if CONFIG_BITENGINE_DIAG_SRP_INJECT
// ---- POST /api/v1/diag/srp/inject ---- 二进制 SRP UPDATE 进、二进制应答（application/dns-message）出
/* 测试专用（Kconfig 默认关）：tools/srp_load.py --inject 用它扮演 N 台 SRP 客户端，走完整真实链路。
   为什么、怎么判：components/thread_br/thread_br_diag.c 下半段、docs/test_cases_scale_baseline.md「SRP 代播满载压测」。
   收不全就报错、不在接收超时上空转（webui 是单任务，卡住 = 整个 Web 界面无响应）。 */
static esp_err_t handle_diag_srp_inject(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    size_t total = req->content_len;
    if (total < 12)                     return send_error(req, 400, "too_short");
    if (total > THREAD_BR_DIAG_SRP_MAX) return send_error(req, 413, "body_too_large");

    /* 请求 + 应答两块放 PSRAM：诊断构建也别吃内部 SRAM */
    uint8_t *buf = heap_caps_malloc(2 * THREAD_BR_DIAG_SRP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return send_error(req, 503, "no_mem");
    uint8_t *rsp = buf + THREAD_BR_DIAG_SRP_MAX;

    size_t got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, (char *)buf + got, total - got);
        if (r <= 0) { free(buf); return send_error(req, 400, "recv_failed"); }
        got += (size_t)r;
    }

    size_t rsp_len = 0;
    esp_err_t err = thread_br_diag_srp_inject(buf, total, rsp, THREAD_BR_DIAG_SRP_MAX, &rsp_len, 3000);
    if (err != ESP_OK) {
        free(buf);
        if (err == ESP_ERR_INVALID_STATE) return send_error(req, 409, "srp_server_not_running_or_burst_busy");
        if (err == ESP_ERR_TIMEOUT)       return send_error(req, 503, "srp_no_response");
        if (err == ESP_ERR_NO_MEM)        return send_error(req, 503, "ot_no_bufs");
        if (err == ESP_ERR_INVALID_ARG)   return send_error(req, 400, "bad_request");
        return send_error(req, 500, esp_err_to_name(err));
    }
    httpd_resp_set_type(req, "application/dns-message");
    esp_err_t e = httpd_resp_send(req, (const char *)rsp, (ssize_t)rsp_len);
    free(buf);
    return e;
}

// ---- POST /api/v1/diag/srp/burst?timeout_ms=N（二进制记录）· GET /api/v1/diag/srp/burst[?detail=1] ----
/* 注册潮：模拟网关重启后设备在 10 s 内成批重注册。记录格式、量什么、为什么在后台任务里跑：
   thread_br_diag.h / thread_br_diag.c 的注册潮那段。POST 收完就返回，GET 轮询进度与结果。
   body 上限 = 条数上限 × (6 + 单条上限)，放 PSRAM。 */
#define SRP_BURST_BODY_MAX ((size_t)THREAD_BR_DIAG_BURST_MAX * (6 + THREAD_BR_DIAG_SRP_MAX))

static esp_err_t handle_diag_srp_burst_post(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    size_t total = req->content_len;
    if (total < 6 + 12)              return send_error(req, 400, "too_short");
    if (total > SRP_BURST_BODY_MAX)  return send_error(req, 413, "body_too_large");
    uint32_t timeout_ms = 30000;
    char q[48], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "timeout_ms", v, sizeof(v)) == ESP_OK) {
        char *end = NULL;
        long t = strtol(v, &end, 10);
        if (!end || *end || t < 1000 || t > 300000) return send_error(req, 400, "bad_timeout_ms");
        timeout_ms = (uint32_t)t;
    }

    uint8_t *buf = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return send_error(req, 503, "no_mem");
    size_t got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, (char *)buf + got, total - got);
        if (r <= 0) { free(buf); return send_error(req, 400, "recv_failed"); }   /* 不在接收超时上空转，理由同上 */
        got += (size_t)r;
    }
    esp_err_t err = thread_br_diag_srp_burst_start(buf, total, timeout_ms);   /* 成功即接管 buf */
    if (err != ESP_OK) {
        free(buf);
        if (err == ESP_ERR_INVALID_STATE) return send_error(req, 409, "busy_or_thread_down");
        if (err == ESP_ERR_INVALID_ARG)   return send_error(req, 400, "bad_records");
        return send_error(req, 503, esp_err_to_name(err));
    }
    thread_br_diag_burst_t st;
    thread_br_diag_srp_burst_status(&st);
    char out[64];
    snprintf(out, sizeof(out), "{\"started\":true,\"n\":%" PRIu32 "}", st.n);
    return send_json_response(req, out);
}

static esp_err_t handle_diag_srp_burst_get(httpd_req_t *req)
{
    AUTH_REQUIRED(req);

    bool detail = false;
    char q[32], v[8];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "detail", v, sizeof(v)) == ESP_OK) detail = (v[0] == '1');

    thread_br_diag_burst_t st;
    thread_br_diag_srp_burst_status(&st);
    char rc[200];
    int rl = snprintf(rc, sizeof(rc), "{");
    for (int i = 0; i < 16 && rl > 0 && rl < (int)sizeof(rc); i++) {
        if (st.rcode[i]) rl += snprintf(rc + rl, sizeof(rc) - rl, "%s\"%d\":%" PRIu32, rl > 1 ? "," : "", i, st.rcode[i]);
    }
    if (rl <= 0 || rl >= (int)sizeof(rc) - 1) return send_error(req, 500, "format");
    rc[rl++] = '}'; rc[rl] = '\0';

    char head[640];
    int n = snprintf(head, sizeof(head),
        "{\"running\":%s,\"n\":%" PRIu32 ",\"sent\":%" PRIu32 ",\"send_fail\":%" PRIu32 ",\"retries\":%" PRIu32
        ",\"responded\":%" PRIu32 ",\"rcode\":%s,\"elapsed_ms\":%" PRIu32
        ",\"lat_ms\":{\"p50\":%" PRIu32 ",\"p95\":%" PRIu32 ",\"max\":%" PRIu32 "}"
        ",\"send_lag_max_ms\":%" PRIu32 ",\"ot_lock_wait_max_ms\":%" PRIu32 ",\"idle_gap_max_ms\":%" PRIu32,
        st.running ? "true" : "false", st.n, st.sent, st.send_fail, st.retries, st.responded, rc, st.elapsed_ms,
        st.lat_p50_ms, st.lat_p95_ms, st.lat_max_ms, st.send_lag_max_ms, st.ot_lock_wait_max_ms, st.idle_gap_max_ms);
    if (n <= 0 || n >= (int)sizeof(head)) return send_error(req, 500, "format");

    httpd_resp_set_type(req, WEBUI_JSON_CONTENT_TYPE);
    if (httpd_resp_sendstr_chunk(req, head) != ESP_OK) return ESP_FAIL;

    /* 代播调的 mdns 写接口：{名字:[次数, 累计 ms, 最长 ms]}（srp_mdns_wrap.c） */
    thread_br_diag_mdns_call_t mc[12];
    size_t nmc = thread_br_diag_mdns_calls(mc, sizeof(mc) / sizeof(mc[0]));
    char item[160];
    if (httpd_resp_sendstr_chunk(req, ",\"mdns_calls\":{") != ESP_OK) return ESP_FAIL;
    for (size_t i = 0; i < nmc; i++) {
        snprintf(item, sizeof(item), "%s\"%s\":[%" PRIu32 ",%" PRIu32 ",%" PRIu32 "]", i ? "," : "",
                 mc[i].name, mc[i].n, mc[i].total_us / 1000, mc[i].max_us / 1000);
        if (httpd_resp_sendstr_chunk(req, item) != ESP_OK) return ESP_FAIL;
    }
    if (httpd_resp_sendstr_chunk(req, "}") != ESP_OK) return ESP_FAIL;

    /* mdns 任务里各类动作：{类型:[次数, 累计 ms, 最长 ms]}，只列发生过的 */
    thread_br_diag_mdns_call_t ma[18];   /* 与 srp_mdns_wrap.c 的 MDNS_ACT_TYPES 一致 */
    size_t nma = thread_br_diag_mdns_actions(ma, sizeof(ma) / sizeof(ma[0]));
    if (httpd_resp_sendstr_chunk(req, ",\"mdns_actions\":{") != ESP_OK) return ESP_FAIL;
    bool first = true;
    for (size_t i = 0; i < nma; i++) {
        if (!ma[i].n) continue;
        snprintf(item, sizeof(item), "%s\"%s\":[%" PRIu32 ",%" PRIu32 ",%" PRIu32 "]", first ? "" : ",",
                 ma[i].name, ma[i].n, ma[i].total_us / 1000, ma[i].max_us / 1000);
        first = false;
        if (httpd_resp_sendstr_chunk(req, item) != ESP_OK) return ESP_FAIL;
    }
    if (httpd_resp_sendstr_chunk(req, "}") != ESP_OK) return ESP_FAIL;

#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    /* 各任务 [名字, 优先级, 累计运行 µs, 栈余量字节]：主机每秒取一次做差分 = 各任务 CPU 占用 */
    UBaseType_t cap = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *ts = heap_caps_malloc(cap * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ts) {
        UBaseType_t nt = uxTaskGetSystemState(ts, cap, NULL);
        esp_err_t e = httpd_resp_sendstr_chunk(req, ",\"tasks\":[");
        for (UBaseType_t i = 0; e == ESP_OK && i < nt; i++) {
            snprintf(item, sizeof(item), "%s[\"%s\",%u,%" PRIu32 ",%u]", i ? "," : "", ts[i].pcTaskName,
                     (unsigned)ts[i].uxCurrentPriority, (uint32_t)ts[i].ulRunTimeCounter,
                     (unsigned)ts[i].usStackHighWaterMark);
            e = httpd_resp_sendstr_chunk(req, item);
        }
        free(ts);
        if (e != ESP_OK || httpd_resp_sendstr_chunk(req, "]") != ESP_OK) return ESP_FAIL;
    }
#endif

    if (!detail) {
        return httpd_resp_sendstr_chunk(req, "}") == ESP_OK ? httpd_resp_sendstr_chunk(req, NULL) : ESP_FAIL;
    }
    /* 逐条：[计划, 实际送出, 应答, RCODE]（ms，-1 = 没有）—— 画"延迟随序号"的曲线用，看 O(N²) 长没长出来 */
    if (httpd_resp_sendstr_chunk(req, ",\"slots\":[") != ESP_OK) return ESP_FAIL;
    /* 拼块缓冲放 PSRAM：这个处理函数本身的栈帧就不小，httpd 栈实测最坏用到一半多（审计 10-07） */
    const int LINE_CAP = 64 * 16;
    char *line = heap_caps_malloc(LINE_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!line) return ESP_FAIL;      /* 头已经发出去了，只能断开 */
    int ll = 0;
    int32_t at, sent, rsp;
    int rcode;
    esp_err_t e = ESP_OK;
    for (uint32_t i = 0; e == ESP_OK && thread_br_diag_srp_burst_slot(i, &at, &sent, &rsp, &rcode); i++) {
        ll += snprintf(line + ll, LINE_CAP - ll, "%s[%" PRId32 ",%" PRId32 ",%" PRId32 ",%d]",
                       i ? "," : "", at, sent, rsp, rcode);
        if (ll > LINE_CAP - 64) {
            e = httpd_resp_send_chunk(req, line, ll);
            ll = 0;
        }
    }
    if (e == ESP_OK && ll) e = httpd_resp_send_chunk(req, line, ll);
    free(line);
    if (e != ESP_OK || httpd_resp_sendstr_chunk(req, "]}") != ESP_OK) return ESP_FAIL;
    return httpd_resp_sendstr_chunk(req, NULL);
}
#endif /* CONFIG_BITENGINE_DIAG_SRP_INJECT */

#if CONFIG_BITENGINE_DIAG_COREDUMP
// ---- GET /api/v1/diag/coredump ---- coredump 分区里那份镜像原样（application/octet-stream）
/* 诊断专用（Kconfig 默认关）。怎么解码、为什么不用 esptool 读、为什么出厂关：见 Kconfig 的 help。
   esp_core_dump_image_get() 回的是分区起点（绝对地址）和镜像长度（含校验，已核过头部），
   镜像从分区偏移 0 开始 —— esp-coredump 的 raw 格式要的就是这段原样字节。
   404 = 分区里没有（从没崩过，或被擦过）。头发出去之后读失败只能断连接：返回非 ESP_OK，httpd 会关 socket，
   客户端看到的是截断，不会把半份当整份。 */
static esp_err_t handle_diag_coredump(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    size_t addr = 0, size = 0;
    esp_err_t err = esp_core_dump_image_get(&addr, &size);
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_SIZE) return send_error(req, 404, "no_coredump");
    if (err != ESP_OK) return send_error(req, 500, esp_err_to_name(err));
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                           ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!part || part->address != addr || size == 0 || size > part->size) {
        return send_error(req, 500, "coredump_partition_mismatch");
    }

    /* 1 KB 内部 RAM：PSRAM 缓冲 esp_flash_read 也会自己去要一块内部临时缓冲，开销一样 */
    const size_t CHUNK = 1024;
    char *chunk = heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!chunk) return send_error(req, 503, "no_mem");
    httpd_resp_set_type(req, "application/octet-stream");
    for (size_t off = 0; off < size; ) {
        size_t n = (size - off < CHUNK) ? size - off : CHUNK;
        err = esp_partition_read(part, off, chunk, n);
        if (err == ESP_OK) err = httpd_resp_send_chunk(req, chunk, (ssize_t)n);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "coredump 下载中断 @%u/%u: %s", (unsigned)off, (unsigned)size, esp_err_to_name(err));
            free(chunk);
            return ESP_FAIL;
        }
        off += n;
    }
    free(chunk);
    return httpd_resp_send_chunk(req, NULL, 0);
}
#endif /* CONFIG_BITENGINE_DIAG_COREDUMP */

#if CONFIG_BITENGINE_DIAG_THREAD_PENDING
// ---- GET /api/v1/diag/thread/pending  ·  POST {"fakes":N} ----
/* 测试专用（Kconfig 默认关）：802.15.4 帧挂起表压测。为什么、怎么判，见 components/thread_br/thread_br_diag.c。
   GET 只读快照（fakes 回 -1：只读时不去表里数）；POST 把假条目数调到正好 N（0..CONFIG_IEEE802154_PENDING_TABLE_SIZE），
   回包的 fakes 是这次确认在表里的个数，status=table_full 表示表里剩下的格子被真子设备占着、没加够。 */
static esp_err_t diag_thread_pending_reply(httpd_req_t *req, int want)
{
    thread_br_diag_pending_t d;
    esp_err_t err = thread_br_diag_pending(want, &d);
    if (err == ESP_ERR_INVALID_STATE) return send_error(req, 409, "thread_not_running");
    if (err == ESP_ERR_TIMEOUT)       return send_error(req, 503, "thread_busy");
    if (err == ESP_ERR_INVALID_ARG)   return send_error(req, 400, "fakes_out_of_range");
    if (err != ESP_OK && err != ESP_ERR_NO_MEM) return send_error(req, 500, esp_err_to_name(err));

    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
        "{\"status\":\"%s\",\"fakes\":%d,\"table_size\":%d,\"scan_miss_cycles\":%" PRIu32 ","
        "\"scan_miss_us\":%.2f,\"pending_mode\":%d,"
        "\"mac\":{\"rx_total\":%" PRIu32 ",\"rx_duplicated\":%" PRIu32 ",\"tx_total\":%" PRIu32 ","
        "\"tx_retry\":%" PRIu32 ",\"tx_err_abort\":%" PRIu32 ",\"tx_direct_max_retry\":%" PRIu32 ","
        "\"tx_indirect_max_retry\":%" PRIu32 "},\"children_total\":%d,\"children\":[",
        err == ESP_OK ? "ok" : "table_full", d.fakes, CONFIG_IEEE802154_PENDING_TABLE_SIZE,
        d.scan_miss_cycles, (double)d.scan_miss_cycles / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, d.pending_mode,
        d.rx_total, d.rx_duplicated, d.tx_total, d.tx_retry, d.tx_err_abort,
        d.tx_direct_max_retry, d.tx_indirect_max_retry, d.children_total);
    for (int i = 0; i < d.n_children && n > 0 && n < (int)sizeof(buf); i++) {
        const thread_br_diag_child_t *c = &d.child[i];
        n += snprintf(buf + n, sizeof(buf) - n,
            "%s{\"rloc16\":\"0x%04X\",\"rx_on_idle\":%s,\"csl_synced\":%s,\"queued\":%u,"
            "\"frame_err\":%u,\"msg_err\":%u,\"age\":%" PRIu32 ",\"timeout\":%" PRIu32 "}",
            i ? "," : "", c->rloc16, c->rx_on_idle ? "true" : "false", c->csl_synced ? "true" : "false",
            c->queued, c->frame_err, c->msg_err, c->age, c->timeout);
    }
    if (n > 0 && n < (int)sizeof(buf))
        n += snprintf(buf + n, sizeof(buf) - n, "]}");
    if (n <= 0 || n >= (int)sizeof(buf)) return send_error(req, 500, "response_truncated");
    return send_json_response(req, buf);
}

static esp_err_t handle_diag_thread_pending_get(httpd_req_t *req)
{
    AUTH_REQUIRED(req);
    return diag_thread_pending_reply(req, -1);
}

static esp_err_t handle_diag_thread_pending_post(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[48];
    int len = recv_body(req, body, sizeof(body));
    if (len == -2) return send_error(req, 413, "body_too_large");
    if (len <= 0)  return send_error(req, 400, "empty_body");
    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");
    const cJSON *f = cJSON_GetObjectItem(root, "fakes");
    int want = cJSON_IsNumber(f) ? f->valueint : -1;
    bool integral = cJSON_IsNumber(f) && f->valuedouble == (double)f->valueint;
    cJSON_Delete(root);
    if (!integral || want < 0 || want > CONFIG_IEEE802154_PENDING_TABLE_SIZE)
        return send_error(req, 400, "fakes_out_of_range");
    return diag_thread_pending_reply(req, want);
}
#endif /* CONFIG_BITENGINE_DIAG_THREAD_PENDING */

#if CONFIG_BITENGINE_DIAG_ZB_SNIFF
// ---- POST /api/v1/diag/zb_sniff {"channel":20,"seconds":30} / GET 取结果 ----
/* 诊断固件专用：在 C5 上录一段 Zigbee 信道的空中帧（components/thread_br/zb_sniff.c 头注）。
   录制在独立任务里跑，POST 立即返回；录制期间 Thread 暂停。GET 在录完后逐帧分块发十六进制，
   主机侧 tools/zb_sniff.py 解码。 */
static esp_err_t handle_diag_zb_sniff_post(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[64];
    int len = recv_body(req, body, sizeof(body));
    if (len == -2) return send_error(req, 413, "body_too_large");
    if (len <= 0)  return send_error(req, 400, "empty_body");
    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");
    const cJSON *c = cJSON_GetObjectItem(root, "channel");
    const cJSON *t = cJSON_GetObjectItem(root, "seconds");
    int ch = cJSON_IsNumber(c) ? c->valueint : -1;
    int sec = cJSON_IsNumber(t) ? t->valueint : -1;
    cJSON_Delete(root);
    if (ch < 11 || ch > 26 || sec < 1 || sec > ZB_SNIFF_MAX_SECONDS) return send_error(req, 400, "channel_or_seconds_out_of_range");

    esp_err_t err = thread_br_zb_sniff_start((uint8_t)ch, (uint16_t)sec);
    if (err == ESP_ERR_NOT_FINISHED)  return send_error(req, 409, "sniff_in_progress");
    if (err == ESP_ERR_INVALID_STATE) return send_error(req, 409, "thread_never_started");
    if (err == ESP_ERR_NO_MEM)        return send_error(req, 500, "no_mem");
    if (err != ESP_OK)                return send_error(req, 400, "invalid_request");
    httpd_resp_set_status(req, "202 Accepted");
    return send_json_response(req, "{\"status\":\"started\"}");
}

static esp_err_t handle_diag_zb_sniff_get(httpd_req_t *req)
{
    AUTH_REQUIRED(req);
    static const char *st_str[] = { "idle", "running", "done", "error" };
    thread_br_zb_sniff_status_t st;
    thread_br_zb_sniff_status(&st);
    char safe_err[48];
    json_sanitize_copy(safe_err, st.err, sizeof(safe_err));

    httpd_resp_set_type(req, WEBUI_JSON_CONTENT_TYPE);
    char buf[352];
    int n = snprintf(buf, sizeof(buf),
        "{\"state\":\"%s\",\"channel\":%u,\"seconds\":%u,\"count\":%u,\"dropped\":%" PRIu32 ","
        "\"tx_seen\":%" PRIu32 ",\"err\":\"%s\",\"frames\":[",
        st_str[st.state <= THREAD_BR_ZB_SNIFF_ERROR ? st.state : 0], st.channel, st.seconds,
        st.count, st.dropped, st.tx_seen, safe_err);
    httpd_resp_send_chunk(req, buf, n);
    for (uint16_t i = 0; st.frames && i < st.count; i++) {
        const zb_sniff_frame_t *f = &st.frames[i];
        n = snprintf(buf, sizeof(buf), "%s{\"t\":%" PRIu32 ",\"rssi\":%d,\"lqi\":%u,\"ch\":%u,\"hex\":\"",
                     i ? "," : "", f->t_ms, f->rssi, f->lqi, f->channel);
        for (uint8_t k = 0; k < f->len && n < (int)sizeof(buf) - 4; k++) {
            n += snprintf(buf + n, sizeof(buf) - n, "%02x", f->psdu[k]);
        }
        n += snprintf(buf + n, sizeof(buf) - n, "\"}");
        httpd_resp_send_chunk(req, buf, n);
    }
    httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);
}
#endif /* CONFIG_BITENGINE_DIAG_ZB_SNIFF */

#if CONFIG_BITENGINE_DIAG_HEAP_TRACE
// ---- GET /api/v1/diag/heaptrace[?all=1] ----
/* 诊断固件专用：读 wd_heaptrace 的结果。默认只列"最低点那一刻还占着"的块（低谷就是
   它们堆出来的），?all=1 列窗口内全部 DMA 分配。逐条分块发，不在内部内存里拼大串 ——
   这个端点本来就是为了量内部内存，不能自己先吃掉一截。 */
static esp_err_t handle_diag_heaptrace_get(httpd_req_t *req)
{
    AUTH_REQUIRED(req);

    bool all = false;
    char q[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char v[4];
        if (httpd_query_key_value(q, "all", v, sizeof(v)) == ESP_OK) all = (v[0] == '1');
    }
    wd_ht_summary_t sm;
    wd_heaptrace_summary(&sm);
    static const char *const STATE[] = { "idle", "armed", "done" };

    char buf[208];
    int n = snprintf(buf, sizeof(buf),
        "{\"state\":\"%s\",\"trigger\":\"%s\",\"window_ms\":%" PRIu32 ","
        "\"dma_free_at_arm\":%" PRIu32 ",\"dma_free_min\":%" PRIu32 ",\"min_at_ms\":%" PRIu32 ","
        "\"allocs\":%" PRIu32 ",\"overflow\":%s,\"skipped\":%" PRIu32 ",\"entries\":[",
        STATE[sm.state <= WD_HT_DONE ? sm.state : 0], sm.trigger ? sm.trigger : "",
        sm.window_ms, sm.dma_free_at_arm, sm.dma_free_min, sm.min_at_ms,
        sm.n_entries, sm.overflow ? "true" : "false", sm.skipped);
    httpd_resp_set_type(req, WEBUI_JSON_CONTENT_TYPE);
    if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) return ESP_FAIL;

    if (sm.state == WD_HT_DONE) {
        bool first = true;
        wd_ht_entry_t e;
        for (uint32_t i = 0; i < sm.n_entries && wd_heaptrace_entry(i, &e); i++) {
            if (!all && !e.alive_at_min) continue;
            n = snprintf(buf, sizeof(buf),
                "%s{\"t\":%" PRIu32 ",\"size\":%" PRIu32 ",\"caps\":\"0x%" PRIx32 "\","
                "\"task\":\"%s\",\"freed\":%" PRId32 ",\"at_min\":%s}",
                first ? "" : ",", e.t_ms, e.size, e.caps, e.task, e.freed_ms,
                e.alive_at_min ? "true" : "false");
            if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) return ESP_FAIL;
            first = false;
        }
    }
    if (httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

// ---- POST /api/v1/diag/heaptrace {"window_s":N} ----
/* N∈[1,60]：立即开窗；N=0：把已抓完的结果置回 idle，让下一次以太网断线重新自动开窗。 */
static esp_err_t handle_diag_heaptrace_arm(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[48];
    int len = recv_body(req, body, sizeof(body));
    if (len <= 0) return send_error(req, 400, "empty_body");
    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");
    const cJSON *w = cJSON_GetObjectItem(root, "window_s");
    bool ok = cJSON_IsNumber(w);
    double ws = ok ? w->valuedouble : -1;
    cJSON_Delete(root);
    if (!ok || ws < 0 || ws > 60 || ws != (double)(int)ws) {
        return send_error(req, 400, "window_s_must_be_integer_0_to_60");
    }
    if (ws == 0) {
        wd_heaptrace_reset();
        return send_json_response(req, "{\"status\":\"reset\"}");
    }
    esp_err_t err = wd_heaptrace_arm("manual", (uint32_t)ws * 1000);
    if (err == ESP_ERR_INVALID_STATE) return send_error(req, 409, "armed_or_not_initialized");
    if (err != ESP_OK)                return send_error(req, 500, esp_err_to_name(err));
    return send_json_response(req, "{\"status\":\"armed\"}");
}
#endif /* CONFIG_BITENGINE_DIAG_HEAP_TRACE */

#if CONFIG_BITENGINE_FAULT_INJECTION
// ---- POST /api/v1/diag/fault ----
static esp_err_t handle_diag_fault(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[192];
    int len = recv_body(req, body, sizeof(body));
    if (len <= 0) return send_error(req, 400, "empty_body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");

    const cJSON *k = cJSON_GetObjectItem(root, "kind");
    const cJSON *t = cJSON_GetObjectItem(root, "target");
    const cJSON *a = cJSON_GetObjectItem(root, "ms");
    if (!a) a = cJSON_GetObjectItem(root, "bytes");

    esp_err_t err;
    if (cJSON_IsString(k) && !strcmp(k->valuestring, "httpd_block")) {
        /* 在 webui 组件内就地处理：往 httpd 自己的工作队列投一个阻塞项，
           复现"被滞留连接堵住"的真实形态。用 vTaskSuspend 挂起 httpd 任务
           是不行的——那样 httpd_stop() 会永远等不到它退出，L3 恢复反而会
           把 wd_action 任务一起卡死。 */
        uint32_t ms = cJSON_IsNumber(a) ? (uint32_t)a->valuedouble : 120000;
        err = webui_inject_httpd_block(ms);
    } else if (cJSON_IsString(k) && !strcmp(k->valuestring, "clear")) {
        wd_fault_clear_all();
        err = ESP_OK;
#if CONFIG_BITENGINE_ENABLE_ZHUB
    } else if (cJSON_IsString(k) && !strcmp(k->valuestring, "zhub_ash_rst")) {
        /* TC-ZH07（docs/15 第十四节）：zhub 背着 ASH 写一帧 RST，模拟 NCP 自己重启；不碰 IO23 */
        err = zhub_fault_ash_rst() == ESP_OK ? ESP_OK : ESP_ERR_INVALID_ARG;
#endif
    } else if (cJSON_IsString(k)) {
        err = wd_fault_inject(k->valuestring,
                              cJSON_IsString(t) ? t->valuestring : NULL,
                              cJSON_IsNumber(a) ? (uint32_t)a->valuedouble : 0);
    } else {
        err = ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(root);

    if (err == ESP_ERR_NOT_FOUND)     return send_error(req, 404, "target_not_found");
    if (err == ESP_ERR_NOT_SUPPORTED) return send_error(req, 400, "unknown_kind");
    if (err != ESP_OK)                return send_error(req, 400, "invalid_request");
    return send_json_response(req, "{\"status\":\"ok\"}");
}
#endif

// ================= PoE 供电检测 =================

// ---- GET /api/v1/poe/status ----
/* 是否由 PoE 供电。判定**只看** no_pull 那一格：1=PoE，0=无 PoE。
 * 2026-08-13 双向实测（纯 PoE / 纯 USB）确认这一格可靠。
 *
 * probe 的另外两格只作原始数据上报，**不参与判定**。
 *
 * ⚠️ 曾经想用它们区分"引脚悬空（分压未上件）"和"正常无 PoE"，那是错的：
 *   真悬空 + 内部上拉 → 3.3V           → 读 1
 *   正常无 PoE + 上拉 → 3.3×100/145 = 2.28V → 读 1（翻转点 ≈VDD/2=1.65V）
 * 两者数字读数相同，分辨不了。当初按最坏情况 VIH(2.48V) 推算才以为能分辨，
 * 而实际翻转点低得多。IO0 又不是 ADC 通道（C5 的 ADC1 只到 GPIO6），读不到
 * 电压。**一个会误报硬件故障的诊断比没有诊断更糟**，所以不再输出 floating。
 *
 * ⚠️ 同理，固件也测不出轨压**上限**：5.0V 与 5.6V 读数完全相同，但后者在
 * 引脚上是 3.33V，超过 VDD、会经 ESD 钳位灌电流。安全性必须用万用表量
 * 5V 轨 ×0.595 核算，见 docs/hardware 待确认清单 C2。
 */
static esp_err_t handle_poe_status(httpd_req_t *req)
{
    AUTH_REQUIRED(req);

    bool np = false, pu = false, pd = false;
    esp_err_t err = poe_probe(&np, &pu, &pd);
    if (err == ESP_ERR_INVALID_STATE) return send_error(req, 503, "poe_not_initialised");
    if (err != ESP_OK)                return send_error(req, 500, "gpio_error");

    char json[360];
    int n = snprintf(json, sizeof(json),
        "{\"poe_present\":%s,\"source\":\"%s\",\"gpio\":%d,"
        "\"probe\":{\"no_pull\":%s,\"pull_up\":%s,\"pull_down\":%s},"
        "\"divider\":\"R96 68K / R97 100K = 0.595\","
        "\"note\":\"digital only: IO0 is not ADC-capable, so rail voltage "
        "(and whether it exceeds VDD) must be checked with a meter — see C2\"}",
        np ? "true" : "false", np ? "poe" : "usb", POE_DETECT_PIN,
        np ? "true" : "false", pu ? "true" : "false", pd ? "true" : "false");
    if (n < 0 || n >= (int)sizeof(json)) return send_error(req, 500, "json_overflow");
    return send_json_response(req, json);
}

// ================= 系统日志 =================

// ---- GET /api/v1/logs?since=<seq>&limit=<n>&level=<E|W|I|D> ----
/* 设备日志的滚动留存，用于现场排障（此前只能挂串口）。
 *
 * 存储见 components/log_store：PSRAM 环形 + RTC_NOINIT 尾巴，**不落 Flash**。
 * 重启前的最后几行会以 tag="prev-boot" 出现在开头 —— 看门狗/panic 重启时
 * 那几行是唯一线索。"为什么重启"本身请看 /api/v1/diag/watchdog（取证记录）。
 *
 * dropped 必须看：环形缓冲溢出丢了多少行。不为 0 说明你拿到的**不是全部**。
 *
 * 不传 since 时返回**最新的** limit 条（tail 语义）—— 没人 curl /logs 是想看
 * 开机那一刻的。传了 since 则按序增量返回，前端就是这么翻页的。
 */
static esp_err_t handle_logs(httpd_req_t *req)
{
    AUTH_REQUIRED(req);

    uint32_t since = 0;
    int limit = 50;
    char level = 0;
    char q[96];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char v[24];
        if (httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK) since = strtoul(v, NULL, 10);
        if (httpd_query_key_value(q, "limit", v, sizeof(v)) == ESP_OK) limit = atoi(v);
        if (httpd_query_key_value(q, "level", v, sizeof(v)) == ESP_OK) level = v[0];
    }
    /* 上限 100：每条最多 ~200 字节 JSON，再多就要分块发，而排障没这个必要 */
    if (limit < 1)   limit = 1;
    if (limit > 100) limit = 100;

    uint32_t next_seq = 0, dropped = 0, capacity = 0, stored = 0;
    log_store_stats(&next_seq, &dropped, &capacity, &stored);

    if (capacity == 0) {
        /* PSRAM 没拿到，功能整个没启用 —— 明说，别让人以为"日志是空的" */
        return send_error(req, 503, "log_store_unavailable");
    }

    /* ⚠️ level=W/E 走**告警环**，不是"取回主环再过滤"。
       主环实测保留窗口只有约 111 秒（2026-08-22 上板量的：丢弃率 90.4%，
       OPENTHREAD 的 INFO 占 88%）。在主环上做过滤等于问一个已经没有答案的
       问题 —— 取回的 100 条里本来就没有告警。告警环 200 槽只收 E/W，
       能装几天份。 */
    bool want_warn = (level == 'W' || level == 'E');
    uint32_t wdropped = 0, wcap = 0, wstored = 0;
    log_store_warn_stats(&wdropped, &wcap, &wstored);
    if (want_warn && wcap == 0) want_warn = false;   /* 告警环没分配上，退回主环 */

    /* 显式走 PSRAM。limit=100 要 17KB，而一次 2MB 的 OTA 之后 internal_free
       只剩一万多字节 —— 而那正是**最需要看日志**的时刻（要查 OTA 为什么失败）。
       这块缓冲只是拷出来发走，完全不需要内部 RAM。
       ⚠️ 订正（2026-08-23）：本配置下 CONFIG_SPIRAM_USE_MALLOC=y 且
          CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024，所以 >1KB 的 malloc **本来
          就会落到 PSRAM**，这一行在当前配置下并不改变行为。我一度以为这里
          发生过 500 out_of_memory —— 那是从"17KB > 剩余内部内存"推出来的，
          **从未真正观察到**，前提也不成立。保留显式写法只是为了不依赖
          ALWAYSINTERNAL 那个阈值：谁把它调大，这里也不会突然改吃内部 RAM。 */
    log_rec_t *recs = heap_caps_malloc(sizeof(log_rec_t) * limit,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!recs) recs = malloc(sizeof(log_rec_t) * limit);
    if (!recs) return send_error(req, 500, "out_of_memory");
    int n = want_warn ? log_store_fetch_warn(since, recs, limit)
                      : log_store_fetch(since, recs, limit);
    if (want_warn) { dropped = wdropped; capacity = wcap; stored = wstored; }

    httpd_resp_set_type(req, WEBUI_JSON_CONTENT_TYPE);
    char head[200];
    /* filtered 与 dropped 必须分开露出来：dropped 是"环满覆盖"（容量不够），
       filtered 是"按 tag 主动挡掉的噪声"（见 log_store.c 的 k_noisy_tags）。
       合成一个数会让人看着数字去加容量，而真正的原因是噪声源。 */
    snprintf(head, sizeof(head),
             "{\"next_seq\":%"PRIu32",\"dropped\":%"PRIu32","
             "\"filtered\":%"PRIu32","
             "\"capacity\":%"PRIu32",\"stored\":%"PRIu32","
             "\"source\":\"%s\",\"entries\":[",
             next_seq, dropped, log_store_filtered(), capacity, stored,
             want_warn ? "warn_ring" : "main_ring");
    httpd_resp_sendstr_chunk(req, head);

    /* 分块发送：50 条 × ~200B 一次性拼会吃掉 10KB 栈/堆，而 httpd 的
       chunked 输出本来就是为这种场景准备的。 */
    bool first = true;
    for (int i = 0; i < n; i++) {
        if (level && recs[i].level != level) continue;
        char tag[LOG_TAG_MAX * 2 + 4], msg[LOG_MSG_MAX * 2 + 8], item[LOG_MSG_MAX * 2 + 160];
        /* 转义且保证合法 UTF-8：日志正文里可能有外部送进来的任意字节（见 json_escape.h） */
        webui_json_escape(tag, sizeof(tag), recs[i].tag);
        webui_json_escape(msg, sizeof(msg), recs[i].msg);
        int w = snprintf(item, sizeof(item),
                 "%s{\"seq\":%"PRIu32",\"ms\":%"PRIu32",\"level\":\"%c\","
                 "\"tag\":\"%s\",\"msg\":\"%s\"}",
                 first ? "" : ",", recs[i].seq, recs[i].ms,
                 recs[i].level ? recs[i].level : '?', tag, msg);
        if (w > 0 && w < (int)sizeof(item)) {
            httpd_resp_sendstr_chunk(req, item);
            first = false;
        }
    }
    free(recs);

    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);      /* 收尾块 */
    return ESP_OK;
}

// ================= NTP / 时区 =================

/* 回包里带**板子自己的当前本地时间**。这是这个端点最重要的一个字段：
   时区默认 UTC+0，用户在别的时区设"22:00-07:00 夜间模式"时，生效的是 UTC 的
   22:00，灯的行为完全对不上直觉。2026-08-13 调试夜间模式就在这上面绕了一圈。
   把 local_time 直接给出来，改完时区立刻能确认对不对，不用去猜。 */
static esp_err_t send_ntp_json(httpd_req_t *req)
{
    const ntp_config_t *n = config_mgr_get_ntp();

    char server[URL_MAX_LEN], tz[TIMEZONE_MAX_LEN];
    json_sanitize_copy(server, n->server,   sizeof(server));
    json_sanitize_copy(tz,     n->timezone, sizeof(tz));

    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    /* scheduler 用同一个判据决定要不要跑夜间模式/定时备份（scheduler.c:23），
       这里如实告诉前端：没同步上就别怪定时任务不动。 */
    bool synced = (t.tm_year > (2024 - 1900));

    char json[URL_MAX_LEN + TIMEZONE_MAX_LEN + 288];
    int len = snprintf(json, sizeof(json),
        "{\"server\":\"%s\",\"timezone\":\"%s\",\"sync_interval\":%"PRIu32","
        "\"tz_declared\":%s,"
        "\"synced\":%s,\"local_time\":\"%04d-%02d-%02d %02d:%02d:%02d\"}",
        server, tz, n->sync_interval,
        /* false = 还是出厂的 UTC+0，前端据此提示用户去设，别拿默认值冒充 */
        n->tz_declared ? "true" : "false",
        synced ? "true" : "false",
        t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
        t.tm_hour, t.tm_min, t.tm_sec);
    if (len < 0 || len >= (int)sizeof(json)) return send_error(req, 500, "json_overflow");
    return send_json_response(req, json);
}

// ---- 射频地区（docs/14 第 7.5 节）：GET/POST /api/v1/radio/region ----
/* NCP 的地区/功率快照 → JSON 对象。/zigbee/status 与 POST /radio/region 共用。
   字段全部来自 ncp_version_get()->txp —— 那是整板上电开机或"应用"时在独占 UART 下读的快照
   （C5 单独重启时沿用上次的，cached=true，T9.31 ②），
   不是实时值（有客户端时不能碰 UART，决策 1），所以带 read_uptime_s 让界面写明"截至第 N 秒"。 */
static int txp_json(char *out, size_t cap)
{
    const ncp_version_t *nv = ncp_version_get();
    const ncp_txp_state_t *t = &nv->txp;
    if (!t->xncp) {
        return snprintf(out, cap, "{\"ncp_xncp\":false,\"read_uptime_s\":%" PRIu32 "}", t->read_uptime_s);
    }
    /* 国家码来自 NCP 的应答，是外部输入：过一遍 json_sanitize_copy 再进 JSON */
    char host[4], gw[4];
    json_sanitize_copy(host, t->host_cc, sizeof(host));
    json_sanitize_copy(gw,   t->gw_cc,   sizeof(gw));
    return snprintf(out, cap,
        "{\"ncp_xncp\":true,\"class\":%u,\"cap_dbm\":%d,\"requested_dbm\":%d,"
        "\"host_class\":%u,\"gw_class\":%u,\"host_region\":\"%s\",\"gw_region\":\"%s\","
        "\"clamped_count\":%u,\"read_uptime_s\":%" PRIu32 ",\"cached\":%s}",
        t->cls, t->cap_dbm, t->requested_dbm, t->host_cls, t->gw_cls, host, gw,
        t->clamped_count, t->read_uptime_s,
        /* true = 这次开机没读 NCP，是上一次开机的快照（C5 单独重启，T9.31 ②） */
        nv->from_cache ? "true" : "false");
}

/* 菜单只在固件里有一份（config_mgr/radio_region.c），前端按这个数组渲染下拉框 */
static int region_menu_json(char *out, size_t cap)
{
    size_t n = 0;
    const radio_region_menu_t *m = radio_region_menu(&n);
    int p = snprintf(out, cap, "[");
    for (size_t i = 0; i < n && p > 0 && p < (int)cap; i++) {
        p += snprintf(out + p, cap - p, "%s\"%s\"", i ? "," : "", m[i].code);
    }
    if (p > 0 && p < (int)cap) p += snprintf(out + p, cap - p, "]");
    return p;
}

/* applied = NULL（GET）| "now" | "deferred" */
static esp_err_t send_region_json(httpd_req_t *req, const char *applied, const char *reason)
{
    const sys_config_t *sys = config_mgr_get_sys();
    char cc[RADIO_REGION_CODE_LEN + 2];
    json_sanitize_copy(cc, sys ? sys->radio_region : "", sizeof(cc));
    char menu[48];
    region_menu_json(menu, sizeof(menu));
    char txp[224];
    txp_json(txp, sizeof(txp));
    int8_t th = thread_br_get_tx_power_dbm();
    char th_buf[8];
    if (th == INT8_MAX) strlcpy(th_buf, "null", sizeof(th_buf));
    else snprintf(th_buf, sizeof(th_buf), "%d", th);
    char app_buf[16];
    if (applied) snprintf(app_buf, sizeof(app_buf), "\"%s\"", applied);
    else strlcpy(app_buf, "null", sizeof(app_buf));

    char json[512];
    int len = snprintf(json, sizeof(json),
        "{\"region\":%s%s%s,\"declared\":%s,\"menu\":%s,"
        "\"applied\":%s,\"reason\":\"%s\","
        "\"zigbee\":%s,\"thread_tx_dbm\":%s}",
        cc[0] ? "\"" : "", cc[0] ? cc : "null", cc[0] ? "\"" : "",
        cc[0] ? "true" : "false", menu, app_buf, reason ? reason : "", txp, th_buf);
    if (len < 0 || len >= (int)sizeof(json)) return send_error(req, 500, "json_overflow");
    return send_json_response(req, json);
}

static esp_err_t handle_radio_region_get(httpd_req_t *req)
{
    AUTH_REQUIRED(req);
    return send_region_json(req, NULL, "");
}

static esp_err_t handle_radio_region_set(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[96];
    int len = recv_body(req, body, sizeof(body));
    if (len == -2) return send_error(req, 413, "body_too_large");
    if (len <= 0)  return send_error(req, 400, "empty_body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");
    cJSON *r = cJSON_GetObjectItem(root, "region");
    char cc[RADIO_REGION_CODE_LEN + 1] = "";
    bool ok = (r != NULL);
    if (ok && !cJSON_IsNull(r)) {
        ok = cJSON_IsString(r) && r->valuestring && strlen(r->valuestring) == RADIO_REGION_CODE_LEN;
        if (ok) strlcpy(cc, r->valuestring, sizeof(cc));
    }
    cJSON_Delete(root);
    if (!ok) return send_error(req, 400, "missing_or_invalid_region");
    radio_region_normalize(cc);
    /* 只收菜单里的码：C5 与 NCP 对同一个码的等级判定才一定一致（radio_region.h） */
    if (!radio_region_code_valid(cc)) return send_error(req, 400, "region_not_in_menu");
    if (config_mgr_set_radio_region(cc) != ESP_OK) return send_error(req, 500, "save_failed");

    /* 立刻应用：独占 UART 把地区发给 NCP（会断开 Z2M/ZHA 约 40 s，MG24 软复位一次）；
       Thread 那颗走 CONFIG_EVENT 立即重设。拿不到独占（MG24 正在升级）就只存下，
       下次开机生效，回包如实说 deferred —— 别装成生效了。 */
    esp_err_t aerr = ezsp_bridge_ncp_resync();
    /* Thread 的重设在事件循环任务里，稍等再读回，否则回包里还是旧值（同 handle_ntp_set） */
    vTaskDelay(pdMS_TO_TICKS(150));
    const char *applied = (aerr == ESP_OK) ? "now" : "deferred";
    const char *reason  = (aerr == ESP_OK) ? ""
                        : (aerr == ESP_ERR_INVALID_STATE) ? "bridge_busy" : esp_err_to_name(aerr);
    /* 合规声明的改动值得一行 WARN，同 extra_host */
    ESP_LOGW(TAG, "radio region set to \"%s\" (%s%s%s)", cc, applied, reason[0] ? ": " : "", reason);
    return send_region_json(req, applied, reason);
}

// ---- GET /api/v1/ntp ----
static esp_err_t handle_ntp_get(httpd_req_t *req)
{
    AUTH_REQUIRED(req);
    return send_ntp_json(req);
}

// ---- POST /api/v1/ntp ----
/* Body（字段可选，缺省保持原值）：
     {"server":"pool.ntp.org","timezone":"CST-8","sync_interval":60..86400}
 *
 * timezone 是 POSIX TZ 串（"UTC+0"、"CST-8"、"PST8PDT,M3.2.0,M11.1.0"），
 * 语法没法在这里完整校验 —— 所以回包直接给出应用后的 local_time，
 * 用户一眼就能看出这串写对没有，比任何校验都直观。
 */
static esp_err_t handle_ntp_set(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[URL_MAX_LEN + TIMEZONE_MAX_LEN + 128];
    int len = recv_body(req, body, sizeof(body));
    if (len == -2) return send_error(req, 413, "body_too_large");
    if (len <= 0)  return send_error(req, 400, "empty_body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");

    ntp_config_t n = *config_mgr_get_ntp();
    int interval = (int)n.sync_interval;

    bool ok = json_opt_str(root, "server",   n.server,   sizeof(n.server))
           && json_opt_str(root, "timezone", n.timezone, sizeof(n.timezone))
           /* 下限 60s：再短就是拿公共 NTP 池当沙包，且对本机没有意义。 */
           && json_opt_int(root, "sync_interval", 60, 86400, &interval);
    /* 只有请求里**真的带了** timezone 才算"用户声明过"。只改 server 或
       sync_interval 不应把时区标成已确认 —— 那就成了拿默认值冒充。 */
    if (ok && cJSON_GetObjectItem(root, "timezone")) n.tz_declared = true;
    cJSON_Delete(root);
    if (!ok) return send_error(req, 400, "invalid_field");

    n.sync_interval = (uint32_t)interval;
    if (n.server[0] == '\0')   return send_error(req, 400, "server_required");
    if (n.timezone[0] == '\0') return send_error(req, 400, "timezone_required");

    /* TZ 串会被 setenv 交给 libc，控制字符没有意义且会污染日志 */
    for (const char *p = n.timezone; *p; p++) {
        if (*p < 0x20 || *p == 0x7F) return send_error(req, 400, "invalid_timezone");
    }

    if (config_mgr_set_ntp(&n) != ESP_OK) return send_error(req, 500, "save_failed");

    /* config_mgr 发 CONFIG_EVENT_NTP_CHANGED，main.c 的处理器重新下发给 SNTP
       并 setenv("TZ")+tzset()。事件是同步投递到 event loop 任务的，这里稍等
       一下再读 local_time，否则回包里给的还是旧时区，看着像没生效。 */
    vTaskDelay(pdMS_TO_TICKS(120));
    return send_ntp_json(req);
}

// ================= TCP 白名单 =================
#if !CONFIG_BITENGINE_ENABLE_ZHUB   /* 只服务 :6638，直连型不编（设计 2.8 A-04；配置项与导出段的去留在 M6 随导入导出一起做） */

/* 白名单只管 EZSP 桥的 6638 端口，管不到 WebUI —— 设错了不会把自己关在门外。
   active 单独给：enabled=true 但一个 IP 都没有时 tcp_server 是**不过滤**的
   （tcp_server.c 的判断是 `enabled && count > 0`），不点明的话用户会以为
   "开了就拦住了"。 */
static esp_err_t send_whitelist_json(httpd_req_t *req)
{
    const tcp_whitelist_config_t *w = config_mgr_get_whitelist();

    char ips[MAX_WHITELIST_IPS * 18 + 4];
    int off = 0;
    ips[0] = '\0';
    for (int i = 0; i < w->count && i < MAX_WHITELIST_IPS; i++) {
        int n = snprintf(ips + off, sizeof(ips) - off, "%s\"%u.%u.%u.%u\"",
                         off ? "," : "",
                         IP_B0(w->allowed_ips[i]), IP_B1(w->allowed_ips[i]),
                         IP_B2(w->allowed_ips[i]), IP_B3(w->allowed_ips[i]));
        if (n < 0 || n >= (int)sizeof(ips) - off) return send_error(req, 500, "json_overflow");
        off += n;
    }

    char json[sizeof(ips) + 160];
    int len = snprintf(json, sizeof(json),
        "{\"enabled\":%s,\"active\":%s,\"count\":%u,\"max\":%d,\"ips\":[%s]}",
        w->enabled ? "true" : "false",
        (w->enabled && w->count > 0) ? "true" : "false",
        w->count, MAX_WHITELIST_IPS, ips);
    if (len < 0 || len >= (int)sizeof(json)) return send_error(req, 500, "json_overflow");
    return send_json_response(req, json);
}

// ---- GET /api/v1/whitelist ----
static esp_err_t handle_whitelist_get(httpd_req_t *req)
{
    AUTH_REQUIRED(req);
    return send_whitelist_json(req);
}

// ---- POST /api/v1/whitelist ----
/* Body：{"enabled":bool, "ips":["192.168.1.10", ...]}
 * ips 给了就是**整表替换**（不是追加）—— 删一个 IP 总得有办法，而部分更新
 * 语义在数组上是含混的。不给 ips 就只改 enabled。
 *
 * 改动只影响**之后**的新连接：tcp_server 在 accept 时才查表，已经建立的
 * Z2M/ZHA 连接不会被踢掉。想立刻生效就重启桥接。
 */
static esp_err_t handle_whitelist_set(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[MAX_WHITELIST_IPS * 24 + 128];
    int len = recv_body(req, body, sizeof(body));
    if (len == -2) return send_error(req, 413, "body_too_large");
    if (len <= 0)  return send_error(req, 400, "empty_body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");

    tcp_whitelist_config_t w = *config_mgr_get_whitelist();
    bool ok = json_opt_bool(root, "enabled", &w.enabled);

    const cJSON *jips = cJSON_GetObjectItem(root, "ips");
    if (ok && jips) {
        if (!cJSON_IsArray(jips) || cJSON_GetArraySize(jips) > MAX_WHITELIST_IPS) {
            ok = false;
        } else {
            uint8_t n = 0;
            const cJSON *it = NULL;
            cJSON_ArrayForEach(it, jips) {
                uint32_t addr = 0;
                if (!parse_ip_field(it, &addr)) { ok = false; break; }
                w.allowed_ips[n++] = addr;
            }
            if (ok) {
                w.count = n;
                for (int i = n; i < MAX_WHITELIST_IPS; i++) w.allowed_ips[i] = 0;
            }
        }
    }
    cJSON_Delete(root);
    if (!ok) return send_error(req, 400, "invalid_field");

    if (config_mgr_set_whitelist(&w) != ESP_OK) return send_error(req, 500, "save_failed");
    return send_whitelist_json(req);
}
#endif /* !CONFIG_BITENGINE_ENABLE_ZHUB */

// ================= MQTT / Webhook 配置 =================

/* MQTT 状态回包。**密码永不回显** —— 只给 password_set 布尔让前端知道该不该
   把输入框显示成"已设置"。config/export 里确实有明文密码，但那是用户主动下载
   备份文件；一个每次开页面都会调的 status 端点不该顺手把凭据摊在响应里。
   broker/username/topic 都来自用户输入并存过 NVS，进 JSON 前一律转义。 */
static esp_err_t send_mqtt_status_json(httpd_req_t *req)
{
    const mqtt_config_t *m = config_mgr_get_mqtt();

    char broker[URL_MAX_LEN], user[PASSWORD_MAX_LEN], prefix[TOPIC_MAX_LEN];
    json_sanitize_copy(broker, m->broker,       sizeof(broker));
    json_sanitize_copy(user,   m->username,     sizeof(user));
    json_sanitize_copy(prefix, m->topic_prefix, sizeof(prefix));

    char json[URL_MAX_LEN + PASSWORD_MAX_LEN + TOPIC_MAX_LEN + 256];
    int n = snprintf(json, sizeof(json),
        "{\"enabled\":%s,\"connected\":%s,\"broker\":\"%s\",\"port\":%u,"
        "\"username\":\"%s\",\"password_set\":%s,\"topic_prefix\":\"%s\","
        "\"publish_interval\":%u,\"tls_enabled\":%s}",
        m->enabled ? "true" : "false",
        mqtt_client_is_connected() ? "true" : "false",
        broker, m->port, user,
        m->password[0] ? "true" : "false",
        prefix, m->publish_interval,
        m->tls_enabled ? "true" : "false");
    if (n < 0 || n >= (int)sizeof(json)) return send_error(req, 500, "json_overflow");
    return send_json_response(req, json);
}

// ---- GET /api/v1/mqtt/status ----
static esp_err_t handle_mqtt_status(httpd_req_t *req)
{
    AUTH_REQUIRED(req);
    return send_mqtt_status_json(req);
}

// ---- POST /api/v1/mqtt/config ----
/* Body（字段全部可选，缺省 = 保持原值）：
     {"enabled":bool, "broker":"host", "port":1-65535, "username":"...",
      "password":"...", "topic_prefix":"...", "publish_interval":10-65535,
      "tls_enabled":bool}
 *
 * password 缺省 = 保持原密码，给空串 "" = 清空。前端回填表单时**不要**把密码
 * 框预填成掩码再原样提交，那会把掩码本身存成密码 —— status 给的是
 * password_set 而不是密码，就是为了让前端只能走"不改就别发"这条路。
 *
 * ⚠️ 这里不调 mqtt_client_restart()：stop/destroy 会阻塞，而我们跑在 httpd
 * task 上（S2）。config_mgr_set_mqtt() 发 CONFIG_EVENT_MQTT_CHANGED，
 * bitengine_mqtt.c:171 收到后只置 s_restart_requested，真正的重连在 mqtt_task
 * 里做。07 模块当年就是在事件回调里直接 restart，每次泄漏一个 task + handler。
 */
static esp_err_t handle_mqtt_config(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[URL_MAX_LEN + PASSWORD_MAX_LEN * 2 + TOPIC_MAX_LEN + 256];
    int len = recv_body(req, body, sizeof(body));
    if (len == -2) return send_error(req, 413, "body_too_large");
    if (len <= 0)  return send_error(req, 400, "empty_body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");

    mqtt_config_t m = *config_mgr_get_mqtt();
    int port = m.port;
    int interval = m.publish_interval;

    bool ok = json_opt_bool(root, "enabled", &m.enabled)
           && json_opt_str(root, "broker",       m.broker,       sizeof(m.broker))
           && json_opt_int(root, "port", 1, 65535, &port)
           && json_opt_str(root, "username",     m.username,     sizeof(m.username))
           && json_opt_str(root, "password",     m.password,     sizeof(m.password))
           && json_opt_str(root, "topic_prefix", m.topic_prefix, sizeof(m.topic_prefix))
           /* 下限 10s：publish_interval 直接决定发包频率，给 0/1 会让 mqtt_task
              变成忙循环。上限是 uint16_t 的天花板。 */
           && json_opt_int(root, "publish_interval", 10, 65535, &interval)
           && json_opt_bool(root, "tls_enabled", &m.tls_enabled);
    cJSON_Delete(root);
    if (!ok) return send_error(req, 400, "invalid_field");

    m.port = (uint16_t)port;
    m.publish_interval = (uint16_t)interval;

    esp_err_t err = config_mgr_set_mqtt(&m);
    if (err == ESP_ERR_INVALID_ARG) return send_error(req, 400, "broker_required");
    if (err != ESP_OK)              return send_error(req, 500, "save_failed");

    return send_mqtt_status_json(req);
}

// ---- POST /api/v1/webhook/config ----
/* Body（字段全部可选，缺省 = 保持原值）：
     {"enabled":bool, "url":"https://...", "backup_interval":0|300-2592000,
      "event_zigbee_crash":bool, "event_network_down":bool,
      "event_interference":bool, "event_ota":bool,
      "event_temp_high":bool, "event_memory_low":bool}
 *
 * webhook 改完立刻生效，不需要重启任何东西：webhook_task 每次发事件前都重新
 * 读 config_mgr_get_webhook()（webhook.c:109）。所以没有 WEBHOOK_CHANGED 的
 * 消费者不是遗漏。
 */
/* webhook 配置的统一回包构造 —— POST 与 GET 共用，两边永远不会漂移。 */
static esp_err_t send_webhook_config_json(httpd_req_t *req)
{
    const webhook_config_t *cur = config_mgr_get_webhook();
    char url_safe[URL_MAX_LEN];
    json_sanitize_copy(url_safe, cur->url, sizeof(url_safe));

    char json[URL_MAX_LEN + 320];
    int n = snprintf(json, sizeof(json),
        "{\"status\":\"ok\",\"enabled\":%s,\"url\":\"%s\",\"backup_interval\":%"PRIu32","
        "\"event_zigbee_crash\":%s,\"event_network_down\":%s,\"event_interference\":%s,"
        "\"event_ota\":%s,\"event_temp_high\":%s,\"event_memory_low\":%s}",
        cur->enabled ? "true" : "false", url_safe, cur->backup_interval,
        cur->event_zigbee_crash ? "true" : "false",
        cur->event_network_down ? "true" : "false",
        cur->event_interference ? "true" : "false",
        cur->event_ota          ? "true" : "false",
        cur->event_temp_high    ? "true" : "false",
        cur->event_memory_low   ? "true" : "false");
    if (n < 0 || n >= (int)sizeof(json)) return send_error(req, 500, "json_overflow");
    return send_json_response(req, json);
}

static esp_err_t handle_webhook_config(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[URL_MAX_LEN + 384];
    int len = recv_body(req, body, sizeof(body));
    if (len == -2) return send_error(req, 413, "body_too_large");
    if (len <= 0)  return send_error(req, 400, "empty_body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");

    webhook_config_t w = *config_mgr_get_webhook();
    int backup = (int)w.backup_interval;

    bool ok = json_opt_bool(root, "enabled", &w.enabled)
           && json_opt_str(root, "url", w.url, sizeof(w.url))
           && json_opt_bool(root, "event_zigbee_crash", &w.event_zigbee_crash)
           && json_opt_bool(root, "event_network_down", &w.event_network_down)
           && json_opt_bool(root, "event_interference", &w.event_interference)
           && json_opt_bool(root, "event_ota",          &w.event_ota)
           && json_opt_bool(root, "event_temp_high",    &w.event_temp_high)
           && json_opt_bool(root, "event_memory_low",   &w.event_memory_low);
    /* backup_interval：0 = 关闭；否则至少 5 分钟（低于这个值等于拿自己的
       webhook 端点做 DoS），上限 30 天。 */
    if (ok) {
        const cJSON *jb = cJSON_GetObjectItem(root, "backup_interval");
        if (jb) {
            ok = json_opt_int(root, "backup_interval", 0, 2592000, &backup)
                 && (backup == 0 || backup >= 300);
        }
    }
    cJSON_Delete(root);
    if (!ok) return send_error(req, 400, "invalid_field");

    w.backup_interval = (uint32_t)backup;

    /* URL 会被原样交给 esp_http_client。不限定 scheme 等于让外部输入决定网关
       主动去请求什么协议 —— S1 要求在入口就拦掉，而不是指望下游拒绝。 */
    if (w.url[0] &&
        strncmp(w.url, "http://", 7) != 0 && strncmp(w.url, "https://", 8) != 0) {
        return send_error(req, 400, "invalid_url_scheme");
    }
    if (w.enabled && w.url[0] == '\0') {
        return send_error(req, 400, "url_required");
    }

    if (config_mgr_set_webhook(&w) != ESP_OK)
        return send_error(req, 500, "save_failed");

    return send_webhook_config_json(req);
}

// ---- GET /api/v1/webhook/config ----
/* 此前只有 POST。想知道当前配置只能 POST 一个空对象（把"读"伪装成"写"，还白白
   触发一次 NVS 写），或者去解析整份 /config/export —— 前端页面要显示现状就卡在这。
   ⚠️ url 原样回显：它可能带令牌（Slack/Discord 那类 webhook URL 就是），但
   ① 端点已在 AUTH_REQUIRED 后面，② POST 的回包一直就在回显它，藏 GET 不藏 POST
   只是自欺。真要藏得两个一起改，且前端就没法做"编辑现有 URL"了。
   MQTT 密码是另一回事 —— 那个永不回显，只给 password_set 布尔。 */
static esp_err_t handle_webhook_config_get(httpd_req_t *req)
{
    AUTH_REQUIRED(req);
    return send_webhook_config_json(req);
}

// ---- 501 stub for unimplemented endpoints ----
// ---- 云端更新 ----
// 形态是「查更新 + 用户点一下装」，不做自动推送。设计见 docs/guide/云端OTA设计.md。
// ⚠️ 验签不在这里也不在 ota_cloud 里 —— esp_https_ota 内部走 esp_ota_end()。

static const char *cloud_state_name(ota_cloud_state_t st)
{
    switch (st) {
        case OTA_CLOUD_CHECKING:         return "checking";
        case OTA_CLOUD_UP_TO_DATE:       return "up_to_date";
        case OTA_CLOUD_UPDATE_AVAILABLE: return "update_available";
        case OTA_CLOUD_DOWNLOADING:      return "downloading";
        case OTA_CLOUD_SUCCESS:          return "success";
        case OTA_CLOUD_FAILED:           return "failed";
        default:                         return "idle";
    }
}

/* 状态回包。check 与 status 共用，免得两处各写一份然后长歪。 */
static esp_err_t send_cloud_status(httpd_req_t *req)
{
    const ota_cloud_manifest_t *m = ota_cloud_get_latest();
    /* version/notes 来自网络，必须转义后才能进我们自己的 JSON —— 与 www_version
       同一条规则（见本文件 :165 的注释）。url 不回显：它对前端没用，而且是
       最长的一个不可信字段。 */
    char safe_ver[48], safe_www[40], safe_ncp[40], safe_notes[160], safe_err[96], safe_ncp_err[64];
    json_sanitize_copy(safe_ver,     m->version,                 sizeof(safe_ver));
    json_sanitize_copy(safe_www,     m->www_version,             sizeof(safe_www));
    json_sanitize_copy(safe_ncp,     m->ncp_version,             sizeof(safe_ncp));
    json_sanitize_copy(safe_notes,   m->notes,                   sizeof(safe_notes));
    json_sanitize_copy(safe_err,     ota_cloud_last_error(),     sizeof(safe_err));
    json_sanitize_copy(safe_ncp_err, ota_cloud_ncp_last_error(), sizeof(safe_ncp_err));

    static const char *ncp_names[] = { "idle", "downloading", "flashing", "failed" };
    ota_cloud_ncp_state_t ns = ota_cloud_ncp_state();
    const char *ncp_state = (ns < sizeof(ncp_names) / sizeof(ncp_names[0])) ? ncp_names[ns] : "unknown";

    const esp_app_desc_t *app = esp_app_get_description();
    char buf[800];
    int n = snprintf(buf, sizeof(buf),
        "{\"state\":\"%s\","
        "\"current_version\":\"%s\","
        "\"latest_version\":\"%s\","
        "\"latest_www_version\":\"%s\","
        "\"app_update\":%s,"
        "\"www_update\":%s,"
        "\"phase\":\"%s\","
        "\"ncp_update\":%s,"
        "\"latest_ncp_version\":\"%s\","
        "\"ncp_state\":\"%s\","
        "\"ncp_error\":\"%s\","
        "\"notes\":\"%s\","
        "\"size\":%u,"
        "\"progress\":%d,"
        "\"error\":\"%s\"}",
        cloud_state_name(ota_cloud_get_state()),
        app ? app->version : "",
        m->valid ? safe_ver : "",
        (m->valid && m->has_www) ? safe_www : "",
        (m->valid && m->app_newer)   ? "true" : "false",
        (m->valid && m->www_differs) ? "true" : "false",
        ota_cloud_phase(),
        (m->valid && m->ncp_differs) ? "true" : "false",
        (m->valid && m->has_ncp) ? safe_ncp : "",
        ncp_state,
        safe_ncp_err,
        m->valid ? safe_notes : "",
        (unsigned)m->size,
        ota_cloud_progress(),
        safe_err);
    if (n >= (int)sizeof(buf)) return send_error(req, 500, "status_json_overflow");
    return send_json_response(req, buf);
}

// ---- GET /api/v1/ota/check ----
// ⚠️ 同步阻塞最长 CONFIG_BITENGINE_OTA_CLOUD_TIMEOUT_MS，期间整个 Web 界面卡住
// （webui 是单 httpd 任务）。这是刻意的取舍：用户主动点"检查更新"时等一下可以
// 接受，换成异步要多一套轮询状态机。别把超时往大调。
static esp_err_t handle_ota_check(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    esp_err_t err = ota_cloud_check();
    if (err == ESP_ERR_NOT_SUPPORTED) return send_error(req, 501, "manifest_url_not_configured");
    if (err == ESP_ERR_INVALID_STATE) return send_error(req, 409, "ota_in_progress");
    /* 其余失败（网络不通、manifest 坏）仍回 200 + state=failed：
       "查不到"是一种正常结局，前端要显示原因而不是弹一个 HTTP 错误。 */
    return send_cloud_status(req);
}

// ---- GET /api/v1/ota/cloud/status ----
static esp_err_t handle_ota_cloud_status(httpd_req_t *req)
{
    AUTH_REQUIRED(req);
    return send_cloud_status(req);
}

// ---- POST /api/v1/ota/cloud/ncp ----
// 装 manifest 里的 Zigbee 芯片固件（.gbl）。与 app/前端分开：刷 NCP 要断 Zigbee 约 15-30 秒，
// 由用户单独点。立即回 202；先轮询 /ota/cloud/status 的 ncp_state，离开 downloading 后
// 改轮询 /ota/zigbee/status（与 Web 上传同一条 XMODEM 路径、同一个进度）。
// body 可带 {"force":true}：指纹相同也刷（产线 / 售后重刷）。
static esp_err_t handle_ota_cloud_ncp(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    if (ota_mg24_busy())   return send_error(req, 409, "mg24_ota_in_progress");
    if (ota_cloud_busy())  return send_error(req, 409, "cloud_ota_in_progress");
    if (s_ota_in_progress) return send_error(req, 409, "ota_already_in_progress");

    bool force = false;
    if (req->content_len > 0 && req->content_len < 128) {
        char body[128];
        int len = httpd_req_recv(req, body, sizeof(body) - 1);
        if (len > 0) {
            body[len] = '\0';
            cJSON *root = cJSON_Parse(body);
            if (root) {
                force = cJSON_IsTrue(cJSON_GetObjectItem(root, "force"));
                cJSON_Delete(root);
            }
        }
    }

    esp_err_t err = ota_cloud_install_ncp(force);
    if (err == ESP_ERR_INVALID_STATE) return send_error(req, 409, "no_ncp_update");
    if (err != ESP_OK)                return send_error(req, 500, "install_start_failed");

    httpd_resp_set_status(req, "202 Accepted");
    return send_json_response(req,
        "{\"status\":\"accepted\",\"poll\":\"/api/v1/ota/cloud/status\","
        "\"then\":\"/api/v1/ota/zigbee/status\"}");
}

// ---- POST /api/v1/ota/cloud ----
// 装 check 查到的那个版本。立即回 202，实际下载在后台任务里。
static esp_err_t handle_ota_cloud_install(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    if (ota_mg24_busy())   return send_error(req, 409, "mg24_ota_in_progress");
    if (s_ota_in_progress) return send_error(req, 409, "ota_already_in_progress");

    esp_err_t err = ota_cloud_install();
    if (err == ESP_ERR_INVALID_VERSION) return send_error(req, 409, "refused_downgrade");
    if (err == ESP_ERR_INVALID_STATE)   return send_error(req, 409, "no_update_available");
    if (err != ESP_OK)                  return send_error(req, 500, "install_start_failed");

    httpd_resp_set_status(req, "202 Accepted");
    return send_json_response(req,
        "{\"status\":\"accepted\",\"poll\":\"/api/v1/ota/cloud/status\"}");
}

static esp_err_t handle_not_implemented(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    return send_error(req, 501, "not_yet_implemented");
}

// ---- Route registration ----


/* ---- GET/POST /api/v1/otbr/auth ----
   OTBR 标准路由（/node、/diagnostics）要不要认证。**默认关**，理由见
   config_mgr.h 的 thread.rest_auth 与 docs/13 决策 1.5：本产品的信任边界是
   局域网（Zigbee 桥的 6638 端口本来就完全无认证），而打开它会逼用户把网关
   管理密码明文填进 HA 的 OTBR 集成 URL。

   ⚠️ 本端点自身**始终**要认证 —— 否则局域网里任何人都能把闸门关掉。 */
static esp_err_t send_otbr_auth_json(httpd_req_t *req)
{
    const thread_config_t *t = config_mgr_get_thread();
    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"enabled\":%s,\"note\":\"When on, HA's OTBR integration needs "
             "http://user:<device password>@host:8081 (ASCII password, no / ? #)\"}",
             (t && t->rest_auth) ? "true" : "false");
    return send_json_response(req, buf);
}

static esp_err_t handle_otbr_auth_get(httpd_req_t *req)
{
    AUTH_REQUIRED(req);
    return send_otbr_auth_json(req);
}

static esp_err_t handle_otbr_auth_set(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[128];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) return send_error(req, 400, "empty_body");
    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");
    cJSON *en = cJSON_GetObjectItem(root, "enabled");
    if (!cJSON_IsBool(en)) { cJSON_Delete(root); return send_error(req, 400, "enabled_must_be_bool"); }

    thread_config_t t = *config_mgr_get_thread();
    t.rest_auth = cJSON_IsTrue(en);
    cJSON_Delete(root);

    /* 开启认证却没有设备密码 = 假保护：check_auth 在无密码时恒真，
       用户会以为锁上了。直接拒绝，把话说明白。 */
    if (t.rest_auth && !config_mgr_has_password()) {
        return send_error(req, 400, "set_device_password_first");
    }

    esp_err_t err = config_mgr_set_thread(&t);
    if (err != ESP_OK) return send_error(req, 500, "save_failed");
    /* 立刻落盘，别走 2s 防抖。这是安全设置：打开后 2 秒内断电，重启回来是
       关着的，而我们已经回了 200 说"已开启"——用户以为锁上了其实没有。
       config_mgr_set_password 出于同样理由也是立即写（config_mgr.c）。 */
    err = config_mgr_save_now();
    if (err != ESP_OK) return send_error(req, 500, "save_failed");

    ESP_LOGI(TAG, "OTBR REST auth -> %s", t.rest_auth ? "on" : "off");
    return send_otbr_auth_json(req);
}

/* ---- 跨站守卫的额外放行 Host（2026-08-23）----
   这一对端点是 421 的**唯一出路**。守卫默认只认 IP 字面量、localhost、以及
   <hostname> 加一组不可公网解析的私有后缀；路由器自定义 local domain、反向代理、
   将来的 DDNS 一律猜不到。没有它，那些场景只能靠恢复出厂脱身。 */
static esp_err_t handle_access_host_get(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    const sys_config_t *sc = config_mgr_get_sys();
    cJSON *root = cJSON_CreateObject();
    if (!root) return send_error(req, 500, "oom");
    cJSON_AddStringToObject(root, "hostname",   sc->hostname);
    cJSON_AddStringToObject(root, "extra_host", sc->extra_host);
    cJSON_AddStringToObject(root, "current",    "");   /* 占位，见下 */

    /* accepted 是给设置页直接渲染的 —— 让用户不必去读固件源码才知道哪些名字能用。
       后缀表来自 webui_server.c 的同一个数组，不在这里再抄一份（抄了必漂移）。 */
    cJSON *acc = cJSON_AddArrayToObject(root, "accepted");
    if (acc) {
        cJSON_AddItemToArray(acc, cJSON_CreateString(sc->hostname));
        for (size_t k = 0; k < webui_host_suffix_count; k++) {
            char n[HOSTNAME_MAX_LEN + 32];
            snprintf(n, sizeof(n), "%s.%s", sc->hostname, webui_host_suffixes[k]);
            cJSON_AddItemToArray(acc, cJSON_CreateString(n));
        }
        cJSON_AddItemToArray(acc, cJSON_CreateString("localhost"));
        cJSON_AddItemToArray(acc, cJSON_CreateString("<任意 IP 地址>"));
    }

    /* 把**本次请求用的** Host 原样回给前端，让设置页能一键把它填进去 ——
       用户能走到这个页面说明当前 Host 已经通过守卫了，但他可能正想加另一个。 */
    char cur[264];
    if (httpd_req_get_hdr_value_str(req, "Host", cur, sizeof(cur)) == ESP_OK) {
        cJSON_ReplaceItemInObject(root, "current", cJSON_CreateString(cur));
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return send_error(req, 500, "oom");
    esp_err_t r = send_json_response(req, out);
    cJSON_free(out);
    return r;
}

static esp_err_t handle_access_host_set(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[192];
    int len = recv_body(req, body, sizeof(body));
    if (len == -2) return send_error(req, 413, "body_too_large");
    if (len <= 0)  return send_error(req, 400, "empty_body");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");
    cJSON *v = cJSON_GetObjectItem(root, "extra_host");
    if (!v || !cJSON_IsString(v)) {
        cJSON_Delete(root);
        return send_error(req, 400, "missing_extra_host");
    }
    /* 空串 = 关掉这条放行，是合法输入，不要当成"没填"。 */
    esp_err_t e = config_mgr_set_extra_host(v->valuestring);
    cJSON_Delete(root);
    if (e == ESP_ERR_INVALID_ARG) return send_error(req, 400, "invalid_host");
    if (e != ESP_OK)              return send_error(req, 500, "save_failed");
    return send_json_response(req, "{\"status\":\"ok\"}");
}

/* ---- OTBR 专用访问 token（2026-08-23）----
   为什么不复用 rest_auth：那个开关同时管着 4 个 AUTH_REQUIRED_UNLESS_OPEN 的只读
   端点（含 /api/v1/status），一打开就把**我们自己集成的零输入发现**也弄没了
   （config_flow 的匿名探测会 401 → 退回问密码）。token 只管 otbr_api.c 那 25 条。 */
/* 可粘贴 URL 里放哪个地址（T9.22，2026-10-08）：优先设备地址（/status 的 svc_ip）。HA 的 OTBR 条目改不了，
   它必须是网关一直保持可达的那一个（T9.37 双向浮动 / 旧规则下的以太网服务 IP），而不是这次打开网页碰巧用的地址 ——
   经 WiFi 自己的地址打开时，那个地址拔插网线后就不在网关上了；经 bitengine.local 打开时 HA 解析不了。
   设备地址还没有（开机早期）才退回本次连接的本地地址。network_mgr_svc_ip 只读一个静态量、不阻塞
   （与 webui_local_addr 不用 network_mgr_get_status 同一个理由：单任务 httpd）。 */
static void otbr_url_host(httpd_req_t *req, char *out, size_t out_len)
{
    uint32_t svc = network_mgr_svc_ip();      /* 网络序 */
    if (svc) {
        snprintf(out, out_len, "%u.%u.%u.%u", IP_B0(svc), IP_B1(svc), IP_B2(svc), IP_B3(svc));
        return;
    }
    webui_local_addr(req, out, out_len);
}

static esp_err_t handle_otbr_token_get(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    const thread_config_t *t = config_mgr_get_thread();
    char ip[48];
    otbr_url_host(req, ip, sizeof(ip));

    /* 直接把**可粘贴的整条 URL** 交出去，而不是让前端自己拼。
       HA 的 otbr 集成没有 reconfigure 步骤，填错一次就得删了重来，
       所以这一步越少人参与越好。端口用 8081（OTBR 标准口）。
       ⚠️ 必须给 IP 不能给主机名：HA 容器里没有 nss-mdns，
          实测 gethostbyname("bitengine.local") 直接 gaierror。 */
    char body[256];
    if (t && t->otbr_token[0] && ip[0]) {
        snprintf(body, sizeof(body),
                 "{\"enabled\":true,\"token\":\"%s\",\"url\":\"http://otbr:%s@%s:8081\"}",
                 t->otbr_token, t->otbr_token, ip);
    } else if (t && t->otbr_token[0]) {
        snprintf(body, sizeof(body),
                 "{\"enabled\":true,\"token\":\"%s\",\"url\":\"\"}", t->otbr_token);
    } else {
        snprintf(body, sizeof(body),
                 "{\"enabled\":false,\"token\":\"\",\"url\":\"\"}");
    }
    return send_json_response(req, body);
}

static esp_err_t handle_otbr_token_set(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[96];
    int len = recv_body(req, body, sizeof(body));
    if (len == -2) return send_error(req, 413, "body_too_large");
    if (len <= 0)  return send_error(req, 400, "empty_body");
    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");
    cJSON *act = cJSON_GetObjectItem(root, "action");
    bool gen   = act && cJSON_IsString(act) && strcmp(act->valuestring, "generate") == 0;
    bool clr   = act && cJSON_IsString(act) && strcmp(act->valuestring, "clear") == 0;
    cJSON_Delete(root);
    if (!gen && !clr) return send_error(req, 400, "invalid_action");

    char tok[33] = "";
    if (gen) {
        /* token **由设备生成**，不接受用户自选：用户选的会短、会重复、会带上
           那两个害死人的字符（非 ASCII → aiohttp UnicodeEncodeError；`/` →
           yarl ValueError，两者都发生在 HA 侧，设备上看不到任何请求）。
           16 字节 = 128 位，在线爆破不成立，所以 auth.c 的比对刻意不做节流
           （做了反而让第三方轮询能把管理员锁死）。 */
        uint8_t raw[16];
        esp_fill_random(raw, sizeof(raw));
        for (int i = 0; i < 16; i++) sprintf(tok + i * 2, "%02x", raw[i]);
    }
    if (config_mgr_set_otbr_token(tok) != ESP_OK)
        return send_error(req, 500, "save_failed");

    /* 回包直接带上新 URL，省掉一次 GET —— 用户点完"生成"就能复制。 */
    char ip[48];
    otbr_url_host(req, ip, sizeof(ip));
    char out[256];
    if (gen && ip[0]) {
        snprintf(out, sizeof(out),
                 "{\"status\":\"ok\",\"enabled\":true,\"token\":\"%s\","
                 "\"url\":\"http://otbr:%s@%s:8081\"}", tok, tok, ip);
    } else if (gen) {
        snprintf(out, sizeof(out),
                 "{\"status\":\"ok\",\"enabled\":true,\"token\":\"%s\",\"url\":\"\"}", tok);
    } else {
        snprintf(out, sizeof(out),
                 "{\"status\":\"ok\",\"enabled\":false,\"token\":\"\",\"url\":\"\"}");
    }
    return send_json_response(req, out);
}

#define URI_ENTRY(path, meth, fn) \
    { .uri = path, .method = meth, .handler = fn }

void register_rest_api_handlers(httpd_handle_t server)
{
    /* 两张表都必须 static：不加 static 就是每次调用在栈上现搭（93 条 × 28 字节 ≈ 2.6KB）。
       开机在主任务的大栈上没事，但看门狗 L3 在 wd_action（3072 栈）里经 webui_httpd_restart()
       走到这里 —— 2026-10-07 coredump：wd_action 用到 2960/3068 时在本函数的 ESP_LOGI 里溢出，
       httpd 卡住的恢复动作变成整机 panic。httpd_register_uri_handler() 会复制条目，表本身不用活着。 */
    static const httpd_uri_t implemented[] = {
        URI_ENTRY("/api/v1/status",          HTTP_GET,  handle_status),
#if !CONFIG_BITENGINE_NET_ETH_ONLY   /* 纯以太网剖面不注册 /wifi 下的端点（设计 2.2） */
        URI_ENTRY("/api/v1/wifi/scan",       HTTP_GET,  handle_wifi_scan),
        URI_ENTRY("/api/v1/wifi/config",     HTTP_POST, handle_wifi_config),
#endif
        URI_ENTRY("/api/v1/ethernet/status", HTTP_GET,  handle_eth_status),
        URI_ENTRY("/api/v1/ethernet/config", HTTP_POST, handle_eth_config),
        URI_ENTRY("/api/v1/system/hostname", HTTP_POST, handle_system_hostname),
        URI_ENTRY("/api/v1/thread/config",   HTTP_GET,  handle_thread_config_get),
        URI_ENTRY("/api/v1/thread/config",   HTTP_POST, handle_thread_config_set),
        URI_ENTRY("/api/v1/zigbee/status",   HTTP_GET,  handle_zigbee_status),
        URI_ENTRY("/api/v1/zigbee/restart",  HTTP_POST, handle_zigbee_restart),
        URI_ENTRY("/api/v1/radio/region",    HTTP_GET,  handle_radio_region_get),
        URI_ENTRY("/api/v1/radio/region",    HTTP_POST, handle_radio_region_set),
        URI_ENTRY("/api/v1/config/export",   HTTP_GET,  handle_config_export),
        URI_ENTRY("/api/v1/config/import",   HTTP_POST, handle_config_import),
        URI_ENTRY("/api/v1/config/factory",  HTTP_POST, handle_factory_reset),
        URI_ENTRY("/api/v1/ota/esp32",       HTTP_POST, handle_ota_esp32),
#if !CONFIG_BITENGINE_ENABLE_ZHUB
        URI_ENTRY("/api/v1/mode",            HTTP_POST, handle_mode),
#endif
        URI_ENTRY("/api/v1/spectrum/scan",     HTTP_GET,  handle_spectrum_scan),
        URI_ENTRY("/api/v1/spectrum/beacon",   HTTP_GET,  handle_spectrum_beacon),
        URI_ENTRY("/api/v1/spectrum/recommend",HTTP_GET,  handle_spectrum_recommend),
        URI_ENTRY("/api/v1/zigbee/channel",    HTTP_POST, handle_zigbee_channel),
        URI_ENTRY("/api/v1/diag/beacon",       HTTP_GET,  handle_diag_beacon),
        URI_ENTRY("/api/v1/thread/readvertise",HTTP_POST, handle_thread_readvertise),
        URI_ENTRY("/api/v1/thread/readvertise",HTTP_GET,  handle_thread_readvertise_stats),
    };

    static const httpd_uri_t stubs[] = {
        URI_ENTRY("/api/v1/zigbee/bootloader", HTTP_POST, handle_zigbee_bootloader),
#if !CONFIG_BITENGINE_ENABLE_ZHUB
        URI_ENTRY("/api/v1/zigbee/clients/disconnect", HTTP_POST, handle_clients_disconnect),
#endif
        URI_ENTRY("/api/v1/diag/ncp_line", HTTP_GET, handle_diag_ncp_line),
        URI_ENTRY("/api/v1/zigbee/stats/reset",  HTTP_POST, handle_stats_reset),
#if !CONFIG_BITENGINE_NET_ETH_ONLY   /* 纯以太网剖面不注册 /network/softap 和 /wifi 下的端点（设计 2.2） */
        URI_ENTRY("/api/v1/network/softap",      HTTP_GET,  handle_softap_get),
        URI_ENTRY("/api/v1/network/softap",      HTTP_POST, handle_softap_set),
        URI_ENTRY("/api/v1/wifi/static",         HTTP_POST, handle_wifi_static),
#endif
        URI_ENTRY("/api/v1/zigbee/lqi",        HTTP_GET,  handle_not_implemented),
        URI_ENTRY("/api/v1/zigbee/backup",     HTTP_GET,  handle_not_implemented),
        URI_ENTRY("/api/v1/zigbee/restore",    HTTP_POST, handle_not_implemented),
        URI_ENTRY("/api/v1/ota/www",           HTTP_POST, handle_ota_www),
        URI_ENTRY("/api/v1/ota/zigbee",        HTTP_POST, handle_ota_zigbee),
        URI_ENTRY("/api/v1/ota/zigbee/status", HTTP_GET,  handle_ota_zigbee_status),
        URI_ENTRY("/api/v1/ota/zigbee/unlock", HTTP_GET,  handle_ota_zigbee_unlock_get),
        URI_ENTRY("/api/v1/ota/zigbee/unlock", HTTP_POST, handle_ota_zigbee_unlock_post),
        URI_ENTRY("/api/v1/diag/uart",         HTTP_POST, handle_diag_uart),
        URI_ENTRY("/api/v1/diag/uart",         HTTP_GET,  handle_diag_uart_result),
        URI_ENTRY("/api/v1/diag/heap",         HTTP_GET,  handle_diag_heap),
        URI_ENTRY("/api/v1/diag/ed",           HTTP_GET,  handle_diag_ed),
        URI_ENTRY("/api/v1/diag/ed",           HTTP_POST, handle_diag_ed_arm),
        URI_ENTRY("/api/v1/diag/watchdog",     HTTP_GET,  handle_diag_watchdog),
        URI_ENTRY("/api/v1/diag/srp",          HTTP_GET,  handle_diag_srp),
#if !CONFIG_BITENGINE_NET_ETH_ONLY
        URI_ENTRY("/api/v1/diag/wifi_probe",   HTTP_GET,  handle_diag_wifi_probe_get),
        URI_ENTRY("/api/v1/diag/wifi_probe",   HTTP_POST, handle_diag_wifi_probe_set),
#endif
        URI_ENTRY("/api/v1/diag/safemode/exit",HTTP_POST, handle_safemode_exit),
#if CONFIG_BITENGINE_FAULT_INJECTION
        URI_ENTRY("/api/v1/diag/fault",        HTTP_POST, handle_diag_fault),
#endif
#if CONFIG_BITENGINE_DIAG_SVC_ID
        URI_ENTRY("/api/v1/diag/svc_id",       HTTP_POST, handle_diag_svc_id),
        URI_ENTRY("/api/v1/diag/svc_id",       HTTP_GET,  handle_diag_svc_id_get),
#endif
#if CONFIG_BITENGINE_DIAG_THREAD_BACKBONE
        URI_ENTRY("/api/v1/diag/thread/backbone", HTTP_POST, handle_diag_thread_backbone),
#endif
#if CONFIG_BITENGINE_DIAG_HUBDB
        URI_ENTRY("/api/v1/diag/hubdb",        HTTP_GET,  handle_diag_hubdb_get),
        URI_ENTRY("/api/v1/diag/hubdb",        HTTP_POST, handle_diag_hubdb_post),
#endif
#if CONFIG_BITENGINE_DIAG_SRP_INJECT
        URI_ENTRY("/api/v1/diag/srp/inject",   HTTP_POST, handle_diag_srp_inject),
        URI_ENTRY("/api/v1/diag/srp/burst",    HTTP_POST, handle_diag_srp_burst_post),
        URI_ENTRY("/api/v1/diag/srp/burst",    HTTP_GET,  handle_diag_srp_burst_get),
#endif
#if CONFIG_BITENGINE_DIAG_COREDUMP
        URI_ENTRY("/api/v1/diag/coredump",     HTTP_GET,  handle_diag_coredump),
#endif
#if CONFIG_BITENGINE_DIAG_ZB_SNIFF
        URI_ENTRY("/api/v1/diag/zb_sniff",     HTTP_GET,  handle_diag_zb_sniff_get),
        URI_ENTRY("/api/v1/diag/zb_sniff",     HTTP_POST, handle_diag_zb_sniff_post),
#endif
#if CONFIG_BITENGINE_DIAG_THREAD_PENDING
        URI_ENTRY("/api/v1/diag/thread/pending", HTTP_GET,  handle_diag_thread_pending_get),
        URI_ENTRY("/api/v1/diag/thread/pending", HTTP_POST, handle_diag_thread_pending_post),
#endif
#if CONFIG_BITENGINE_DIAG_HEAP_TRACE
        URI_ENTRY("/api/v1/diag/heaptrace",    HTTP_GET,  handle_diag_heaptrace_get),
        URI_ENTRY("/api/v1/diag/heaptrace",    HTTP_POST, handle_diag_heaptrace_arm),
#endif
#if CONFIG_BITENGINE_ENABLE_MDNS_LOAD
        URI_ENTRY("/api/v1/diag/mdns_load",    HTTP_GET,  handle_diag_mdns_load_get),
        URI_ENTRY("/api/v1/diag/mdns_load",    HTTP_POST, handle_diag_mdns_load_set),
#endif
        URI_ENTRY("/api/v1/ota/check",         HTTP_GET,  handle_ota_check),
        URI_ENTRY("/api/v1/ota/cloud",         HTTP_POST, handle_ota_cloud_install),
        URI_ENTRY("/api/v1/ota/cloud/status",  HTTP_GET,  handle_ota_cloud_status),
        URI_ENTRY("/api/v1/ota/cloud/ncp",     HTTP_POST, handle_ota_cloud_ncp),
        URI_ENTRY("/api/v1/logs",              HTTP_GET,  handle_logs),
        URI_ENTRY("/api/v1/led/test",          HTTP_POST, handle_led_test),
        URI_ENTRY("/api/v1/led/test",          HTTP_GET,  handle_led_test_info),
        URI_ENTRY("/api/v1/led/timing",        HTTP_GET,  handle_led_timing_get),
        URI_ENTRY("/api/v1/led/timing",        HTTP_POST, handle_led_timing_set),
        URI_ENTRY("/api/v1/led/probe",         HTTP_POST, handle_led_probe),
        URI_ENTRY("/api/v1/led/sweep",         HTTP_POST, handle_led_sweep),
        URI_ENTRY("/api/v1/led/config",        HTTP_GET,  handle_led_config_get),
        URI_ENTRY("/api/v1/led/config",        HTTP_POST, handle_led_config),
        URI_ENTRY("/api/v1/led/nightmode",     HTTP_POST, handle_led_nightmode),
        URI_ENTRY("/api/v1/mqtt/status",       HTTP_GET,  handle_mqtt_status),
        URI_ENTRY("/api/v1/mqtt/config",       HTTP_POST, handle_mqtt_config),
        URI_ENTRY("/api/v1/webhook/config",    HTTP_GET,  handle_webhook_config_get),
        URI_ENTRY("/api/v1/webhook/config",    HTTP_POST, handle_webhook_config),
        URI_ENTRY("/api/v1/otbr/auth",          HTTP_GET,  handle_otbr_auth_get),
        URI_ENTRY("/api/v1/otbr/auth",          HTTP_POST, handle_otbr_auth_set),
        URI_ENTRY("/api/v1/ntp",               HTTP_GET,  handle_ntp_get),
        URI_ENTRY("/api/v1/ntp",               HTTP_POST, handle_ntp_set),
        URI_ENTRY("/api/v1/otbr/token",        HTTP_GET,  handle_otbr_token_get),
        URI_ENTRY("/api/v1/otbr/token",        HTTP_POST, handle_otbr_token_set),
        URI_ENTRY("/api/v1/net/access-host",   HTTP_GET,  handle_access_host_get),
        URI_ENTRY("/api/v1/net/access-host",   HTTP_POST, handle_access_host_set),
#if !CONFIG_BITENGINE_ENABLE_ZHUB
        URI_ENTRY("/api/v1/whitelist",         HTTP_GET,  handle_whitelist_get),
        URI_ENTRY("/api/v1/whitelist",         HTTP_POST, handle_whitelist_set),
#else
        URI_ENTRY("/api/v1/system/reboot",     HTTP_POST, handle_system_reboot),
#endif
        URI_ENTRY("/api/v1/poe/status",        HTTP_GET,  handle_poe_status),
    };

    for (int i = 0; i < sizeof(implemented) / sizeof(implemented[0]); i++)
        httpd_register_uri_handler(server, &implemented[i]);

    int still_stub = 0;
    for (int i = 0; i < sizeof(stubs) / sizeof(stubs[0]); i++) {
        httpd_register_uri_handler(server, &stubs[i]);
        if (stubs[i].handler == handle_not_implemented) still_stub++;
    }

    /* stub 数量数出来而不是写死：这个数组早已是实现与桩混装（ota_*、diag_* 都
       是真 handler），写死的字面量只会随着接线一次次变成假话。 */
    ESP_LOGI(TAG, "REST API: %d + %d handlers registered (%d still 501)",
             (int)(sizeof(implemented)/sizeof(implemented[0])),
             (int)(sizeof(stubs)/sizeof(stubs[0])),
             still_stub);
}
