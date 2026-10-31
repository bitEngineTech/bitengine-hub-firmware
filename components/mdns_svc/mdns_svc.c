#include "mdns_svc.h"
#include "config_mgr.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_ota_ops.h"
#include "esp_mac.h"
#include "mdns.h"

static const char *TAG = "mdns_svc";

/* WebUI 的 _http._tcp 端口。init 注册与 refresh 重宣告共用：refresh 靠"把端口设一遍"触发宣告，两处要是不一致，
   它会每 30 s 把对外宣告的端口悄悄改掉（VERIFY.sh 比对两边的（类型，端口）） */
#define MDNS_SVC_HTTP_PORT 80

static void hostname_event_handler(void *arg, esp_event_base_t base,
                                   int32_t id, void *data)
{
    if (id == CONFIG_EVENT_HOSTNAME_CHANGED) {
        const char *new_name = config_mgr_get_sys()->hostname;
        mdns_hostname_set(new_name);
        ESP_LOGI(TAG, "mDNS hostname updated: %s", new_name);
    }
}

esp_err_t mdns_svc_init(void)
{
    const sys_config_t *sys = config_mgr_get_sys();
#if !CONFIG_BITENGINE_ENABLE_ZHUB
    const esp_app_desc_t *app = esp_app_get_description();   /* 只给下面两个 Zigbee 服务的 TXT 用 */
#endif

    /* Not ESP_ERROR_CHECK: an mDNS failure must not abort() the whole
       gateway — the bridge works fine without discovery. */
    esp_err_t init_err = mdns_init();
    if (init_err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s — discovery disabled",
                 esp_err_to_name(init_err));
        return init_err;
    }
    mdns_hostname_set(sys->hostname);
    mdns_instance_name_set("bitEngine Zigbee/Thread Hub");

#if !CONFIG_BITENGINE_ENABLE_ZHUB
    // Service 1: Zigbee bridge (Z2M / ZHA)
    /* mac 进 TXT：HA 集成靠 zeroconf 发现时，能在**建立连接之前**就拿到
       unique_id，从而正确识别"这台我已经加过了"，避免重复添加。 */
    uint8_t mac_b[6] = {0};
    esp_read_mac(mac_b, ESP_MAC_WIFI_STA);
    char mac_txt[18];
    snprintf(mac_txt, sizeof(mac_txt), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac_b[0], mac_b[1], mac_b[2], mac_b[3], mac_b[4], mac_b[5]);

    mdns_txt_item_t zb_txt[] = {
        { "mac",        mac_txt },
        { "model",      "bitEngine-Hub-C5" },
        { "fw_version", app->version },
        { "zb_chip",    "EFR32MG24" },
        { "api_port",   "80" },
    };
    esp_err_t err = mdns_service_add("bitEngine Bridge", "_zigbee-bridge", "_tcp",
                                     CONFIG_BITENGINE_TCP_PORT, zb_txt, 5);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS _zigbee-bridge service add failed: %s", esp_err_to_name(err));
    }

    /* ZHA 的自动发现入口。契约来自 HA 的 zha/config_flow.py（v2026.3）：
         服务类型 _zigbee-coordinator._tcp   （:97 ZEROCONF_SERVICE_TYPE）
         TXT 必须有 radio_type + serial_number（:98 ZEROCONF_PROPERTIES_SCHEMA）
         radio_type 只接受 ezsp/znp/deconz/zigate/xbee，我们是 **ezsp**
       匹配规则是 {"name":"*"}，即该类型下**任何**实例都会被发现。

       ⚠️ 以前只广播 _zigbee-bridge._tcp —— 一字之差，与 ZHA 擦肩而过，
       用户只能手打 socket:// 地址。

       ⚠️ ZHA 拿到发现后填的是 `socket://<IP>:<port>`（config_flow.c:1051 用的是
       discovery_info.host，即**解析出来的 IP**，不是主机名），已建的条目之后也不随
       发现改。所以添加那一刻宣告的是哪个地址，用户就一直用哪个 —— 2026-10-07 起
       本机 A 记录只给以太网服务 IP（T9.34，行为正本是 Kconfig
       BITENGINE_MDNS_SVC_IP_ONLY 的 help），它会跟着浮到 WiFi 上，填它就不用再改。
       （手工填主机名不是出路：HA 容器里解析不了 .local。） */
    mdns_txt_item_t zc_txt[] = {
        { "radio_type",    "ezsp" },
        { "serial_number", mac_txt },   /* 用 MAC 当序列号：唯一且稳定 */
        { "name",          "bitEngine Hub" },
    };
    err = mdns_service_add("bitEngine Hub", "_zigbee-coordinator", "_tcp",
                           CONFIG_BITENGINE_TCP_PORT, zc_txt, 3);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS _zigbee-coordinator 注册失败: %s —— ZHA 将无法自动发现",
                 esp_err_to_name(err));
    }
#else
    /* 直连型不带桥（docs/15 架构 5.8 R-2、设计 2.6 第 5 步）：两个 Zigbee 服务都不宣告 —— 没有 :6638，
       宣告了就是招 ZHA / Z2M 来连一个不存在的端口。只剩 _http。 */
    esp_err_t err;
#endif

    err = mdns_service_add("bitEngine WebUI", "_http", "_tcp", MDNS_SVC_HTTP_PORT, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS _http service add failed: %s", esp_err_to_name(err));
    }

    /* ⚠️ Service 3（`_slzb-06._tcp` / SMLIGHT 兼容）**已于 2026-08-22 停止广播**。
     * 不要再加回来。`/api2/ 下的` 那些端点**保留**（smlight_compat.c），
     * 用户仍可在 HA 里手工添加 SMLIGHT 集成填 IP —— 去掉的只是自动发现。
     *
     * 【为什么当初有它】97fe85d（2026-03-30）加的。那时它是唯一能让 HA 自动
     * 发现我们 Zigbee 协调器的办法 —— 靠冒充一台 SMLIGHT 设备。当时我们既没有
     * 自己的 HA 集成（2026-08-20 才有），也没广播 ZHA 官方的发现服务。
     *
     * 【为什么现在不要】
     *  1. **它对 ZHA 从来就没起过作用。** ZHA 的匹配表里那条是 `slzb-06*`，
     *     按**实例名前缀**匹配，而我们的实例名是 "bitEngine Hub" —— 五个月里
     *     一次都没匹配上。13439b7 的提交说明已经写明这一点。
     *  2. **正牌替代已经就位**：`_zigbee-coordinator._tcp`（13439b7，2026-08-21，
     *     契约照 HA 源码逐条核对并实测过）负责 ZHA 发现；
     *     `_zigbee-bridge._tcp` 负责我们自己的集成。原目的已被完全覆盖。
     *  3. **它实际产生的效果是负的**：HA 的 SMLIGHT 集成按**服务类型**匹配
     *     （不看实例名），于是用户会看到一张写着**竞争对手品牌**的卡片指向我们的
     *     设备；点进去还要密码（`/api2/ 下的` 全带 AUTH_REQUIRED），而它保护的
     *     型号/MAC/固件版本/主机名，mDNS 的 TXT 本来就在匿名广播。
     *     2026-08-21 现场：用户在新环境里就是被这张卡片挡住的。
     *  4. 同一台设备本来就已经冒出三张发现卡片（ZHA / SMLIGHT / Thread），
     *     少一张是净收益。
     */

    /* Service 4（_meshcop._udp / Thread Border Router）**故意不在这里发布**。
     *
     * 2026-08-11 定位：ESP 的 BR 库自己会发布这条记录
     * （libopenthread_br.a 的 esp_openthread_publish_meshcop_mdns），用的是
     * Border Agent 的真实 UDP 端口和标准 TXT。我们以前抢先手工注册了一条
     * 端口写死 80 的，mdns 组件拒绝重复注册，于是库的发布**每次开机失败两次**：
     *     E OPENTHREAD: esp_openthread_publish_meshcop_mdns(169):
     *       Failed to publish meshcop mdns service
     * 网上就只剩我们那条错的。后果是任何按 Thread 规范做 DTLS 的消费者
     * （matterjs-server 的 Thread 诊断）连到 80 端口后 30s 超时，拓扑图空白。
     *
     * 当初写死 80 的理由是"HA 靠 mDNS 端口拼 REST URL"，该前提已查证不成立：
     * HA 的 otbr 集成 config_flow 只有 user/hassio 两步、没有 zeroconf 发现，
     * URL 是手工填的；thread 集成的 ThreadRouterDiscoveryData 里也没有 port 字段。
     *
     * 之所以长期没人发现：_meshcop._udp 声明的是 **UDP** 80，而 WebUI 是 TCP 80，
     * 两个消费者按不同传输层解读同一个数字，互不干扰。
     *
     * 所以这里什么都不做，让库去发布。BA 要等 Thread 起来约 15s 后才 listening
     * （实测 26.9s 打出 "Border Agent start listening on port 49154"），库会在
     * 那时发布，比我们在启动早期硬编码一个端口正确得多。 */

    // Subscribe to hostname changes
    esp_event_handler_instance_register(CONFIG_EVENT, CONFIG_EVENT_HOSTNAME_CHANGED,
                                        hostname_event_handler, NULL, NULL);

#if CONFIG_BITENGINE_ENABLE_ZHUB
    ESP_LOGI(TAG, "mDNS: %s.local, _http._tcp:%d (direct profile: no Zigbee bridge adverts)",
             sys->hostname, MDNS_SVC_HTTP_PORT);
#else
    ESP_LOGI(TAG, "mDNS: %s.local, _zigbee-bridge._tcp:%d, _http._tcp:80",
             sys->hostname, CONFIG_BITENGINE_TCP_PORT);
#endif
    return ESP_OK;
}

esp_err_t mdns_svc_brand_meshcop(void)
{
    /* _meshcop._udp 由 BR 库发布，它填的是 vn=OpenThread / mn=BorderRouter。
       HA 的 Thread 面板会把这两项当厂商/型号显示，所以覆盖成我们的品牌。

       只能在库发布之后做——BA 要等 Thread 起来约 27s 才 listening。服务不存在时
       mdns_service_txt_item_set 返回 ESP_ERR_NOT_FOUND，调用方据此重试即可，
       不要在这里猜时间。 */
    esp_err_t err = mdns_service_txt_item_set("_meshcop", "_udp", "vn", "bitEngine");
    if (err != ESP_OK) return err;          /* 多半是还没发布，调用方会再来 */
    mdns_service_txt_item_set("_meshcop", "_udp", "mn", "bitEngine-Hub-C5");
    return ESP_OK;
}

/* 2026-09-13 删掉了 mdns_svc_update_hostname()：全仓零调用者。主机名变更由本文件的
   事件处理器（CONFIG_EVENT_HOSTNAME_CHANGED → mdns_hostname_set）自己处理，一个公开
   却没人调的 API 只会让下一个人以为"改主机名要记得调它"。 */

esp_err_t mdns_svc_refresh(void)
{
    /* 只重新宣告**我们自己**的三个服务（直连型只有 _http；与 mdns_svc_init 里的 mdns_service_add 一一对应，
       VERIFY.sh 盯着）。端口不变的 mdns_service_port_set() 会让 mdns 对这一个服务发一轮宣告、
       带上主机地址；PCB 正常运行时开销与代播了多少 Matter 设备无关。
     *
     * ⚠️ 以前这里是 mdns_hostname_set("") 再设回原名，**别改回去**：
     *  1. mdns 把空串当非法参数直接拒绝，所以从来不是"先下线再上线"，只是把同一个名字再设一次；
     *     而我们的服务都有显式实例名，主机名重设本来就不给它们发 goodbye。
     *  2. 主机名重设 = mdns_priv_restart_all_pcbs()：把**全部**服务（含 SRP 代播的每一条 Matter
     *     记录）重新探测、宣告。2026-10-07 压测在 199 条代播服务时，开机后 5 分钟内的这个周期刷新
     *     让 mdns 任务连续占满 CPU，任务看门狗 panic（coredump 实证）。真实场景就是网关重启后
     *     设备成批重注册的那几分钟 —— 设备一多就会重启循环。
     *  3. 对代播记录也没有用：记录逐字节相同，对端只当 Updated（见 srp_readvertise.c 头注）。 */
    static const struct { const char *type; uint16_t port; } own[] = {
#if !CONFIG_BITENGINE_ENABLE_ZHUB      /* 直连型不宣告这两个（init 里同样的保护） */
        { "_zigbee-bridge",      CONFIG_BITENGINE_TCP_PORT },
        { "_zigbee-coordinator", CONFIG_BITENGINE_TCP_PORT },
#endif
        { "_http",               MDNS_SVC_HTTP_PORT },
    };
    esp_err_t first_err = ESP_OK;
    for (size_t i = 0; i < sizeof(own) / sizeof(own[0]); i++) {
        esp_err_t err = mdns_service_port_set(own[i].type, "_tcp", own[i].port);
        if (err != ESP_OK) {
            /* 多半是开机时这条就没注册成功（init 里已报过 W） */
            ESP_LOGD(TAG, "re-announce %s._tcp: %s", own[i].type, esp_err_to_name(err));
            if (first_err == ESP_OK) first_err = err;
        }
    }
    ESP_LOGI(TAG, "mDNS own services re-announced");
    return first_err;
}
