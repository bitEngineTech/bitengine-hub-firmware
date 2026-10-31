#pragma once

#include <stdbool.h>
#include "radio_region.h"
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------- String Length Constants ----------------

#define HOSTNAME_MAX_LEN    32
#define SSID_MAX_LEN        33
#define PASSWORD_MAX_LEN    64
#define URL_MAX_LEN         256
#define IP_STR_MAX_LEN      16
#define TOPIC_MAX_LEN       64
#define TIMEZONE_MAX_LEN    32
#define COLOR_HEX_LEN       8
#define PWD_HASH_LEN        65  // SHA256 hex (64) + null
#define MAX_WHITELIST_IPS   8

#define CONFIG_VERSION_CURRENT  1

/* 出厂默认时区。config_mgr.c 靠"值 != 这个"来推断用户是否设过时区（见
   ntp_config_t.tz_declared），所以两处必须用同一个常量，不能各写各的字面量。 */
#define NTP_TIMEZONE_DEFAULT    "UTC+0"

// ---------------- Sub-Config Structs ----------------

/* Thread 出厂/兜底信道。选 25 是因为要离 Zigbee 的常见默认信道足够远：
   Z2M 默认 ch11、ZHA 默认 ch15，25 对两者的间隔都 > COEX_TOO_CLOSE_MAX(5)。

   ⚠️ 这个值有**两个**使用点：出厂默认（config_defaults.c）与配置导入的越界兜底
   （config_export.c）。2026-09-11 之前后者写死 15 —— 正好压在 ZHA 的默认信道上，
   导一份信道越界的备份进去就会把两张射频网撞到一起，而这恰恰是默认值 25 要避开的。
   所以两处必须引同一个常量，别再各写一个数。 */
#define THREAD_CHANNEL_DEFAULT  25

/* NVS 落盘失败的累计次数与最近一次的运行秒数。
 *
 * 为什么要有它：REST 的 setter 多数在 2s 防抖**之前**就回 200，落盘失败只剩一条
 * ESP_LOGE。用户看到的是"保存成功，重启后设置回退了"—— 一个没有任何线索的故障。
 * 现在 /api/v1/status 会报 nvs_write_failures，前端在非 0 时出横幅。
 *
 * count 只增不减（除非重启）。last_uptime 是最近一次失败时的 uptime 秒数，
 * 0 表示从没失败过 —— 用 uptime 而不是墙钟时间，因为失败很可能发生在 NTP
 * 同步之前，那时墙钟是 1970。 */
typedef struct {
    uint32_t count;
    uint32_t last_uptime;
} config_nvs_health_t;

void config_mgr_get_nvs_health(config_nvs_health_t *out);

typedef struct {
    char     hostname[HOSTNAME_MAX_LEN];
    /* 额外允许的 HTTP Host。跨站守卫（webui_server.c 的 webui_cross_origin_check）
       默认只认 IP 字面量、localhost、以及 <hostname> 加一组**不可公网解析**的私有
       后缀。设备永远猜不到路由器的自定义 local domain（DHCP 给不到 option 15）、
       反向代理名、将来的 DDNS 域名 —— 没有这个字段，那些场景只能靠恢复出厂脱身。
       ⚠️ 写它要求已认证且已通过守卫，但它一旦被写成攻击者控制的公网域名，就是
       一张**永久的 DNS rebinding 通行证**。所以改动一律打 ESP_LOGW，且它在
       /api/v1/config/export 里可见（不当秘密处理，就是要让人看见）。 */
    char     extra_host[64];
    uint8_t  bridge_mode;
    /* 射频地区（docs/14）：ISO 3166-1 alpha-2 大写，"" = 未设置。只收 radio_region.h 菜单里的码。
       C5 每次开机把它发给 MG24（XNCP 0x7001），Thread 那颗射频也按它取功率。
       是"用户声明"，不校验真伪 —— WiFi 国家码和 HA 自家棒子都是这个模型。 */
    char     radio_region[RADIO_REGION_CODE_LEN + 1];
    char     password_hash[PWD_HASH_LEN];
} sys_config_t;

typedef struct {
    char     ssid[SSID_MAX_LEN];
    char     password[PASSWORD_MAX_LEN];
    bool     has_credentials;
    uint32_t static_ip;
    uint32_t static_gateway;
    uint32_t static_netmask;
    uint32_t static_dns;
} wifi_config_data_t;

typedef struct {
    bool     dhcp;
    uint32_t static_ip;
    uint32_t static_gateway;
    uint32_t static_netmask;
    uint32_t static_dns;
} eth_config_t;

/*
 * ⚠️ channel 是**用户声明值**，不是实测值，固件也从不把它下发给 MG24。
 *
 * 为什么只能是声明值：Zigbee 信道由 Z2M/ZHA 在 formNetwork 时决定，桥是纯字节
 * 流透传（docs/02 决策 1），插不进那个决定。想反过来"读回来"也不行 —— 读参数
 * 前必须 ASH RST，而复位后协议栈未初始化，必须先 write_config（给协议栈分配
 * 地址表/密钥表等一整套 setConfigurationValue）才能 networkInit，否则
 * getNetworkParameters 恒回 NOT_JOINED。那一整套等于在网关里再造一个 EZSP
 * 主机，正是决策 1 不做的事。2026-08-12 上板证实：我们读到 NOT_JOINED 的同时
 * ZHA 的设备正常收发，网络就在 ch20。
 *
 * 所以这里改成让用户如实告知"你在 Z2M/ZHA 里设的是几"，共存判断建立在这个
 * 诚实输入上，而不是伪装成实测。channel_declared=false 时对外一律报 unknown，
 * 绝不拿默认值冒充。
 */
typedef struct {
    uint8_t  channel;           // 11-26，仅在 channel_declared 时有意义
    bool     channel_declared;  // 用户是否真的声明过；false = 未知
    /* 声明时实测到的自家协调器 PAN ID。有了它，以后判断"某个 beacon 是不是
       我们自己的"就是精确匹配，不用靠 RSSI 猜。也正是靠它，声明值过期能被
       发现 —— 定期扫一遍，本 PAN 出现在别的信道上就说明用户改过 Z2M/ZHA
       却忘了回来同步。 */
    uint16_t pan_id;
    bool     pan_known;
    /* 没有 tx_power / turbo_mode 字段（2026-09-10 T1.5 删除）：发射功率由
       Z2M/ZHA 每次启动写进 MG24（zigpy _get_effective_tx_power → bellows
       setRadioPower），网关既不产生、也读不到、也下发不了这个值。曾经的
       NVS 字段只被一个"Turbo"开关渲染成假的 20 dBm。别再加回来。 */
    /* 这台网关**曾经**有过 Zigbee 协调器连上来（粘性，写一次就不再动，
       恢复出厂才清）。用来区分两件完全不同的事：
         · 从没配过 Zigbee（只用 Thread/Matter，或还没装 ZHA）—— 正常，别报警
         · 以前用得好好的，现在连不上了 —— 这才是故障
       没有它的话，纯 Thread 用户会一直挂着一条红色横幅，而设备完全正常。 */
    bool     host_ever_seen;
} zigbee_config_t;

typedef struct {
    bool     enabled;
    uint8_t  channel;       // 11-26
    /* OTBR REST（/node、/diagnostics）要不要认证。**默认 false**。
       为什么默认关：本产品的信任边界是局域网，Zigbee 桥的 6638 端口本来就
       完全无认证（设计决策 1，白名单默认也是关的），单锁 Thread 这一半并不
       真的提升一个档次。而打开它的代价是实打实的：HA 的 OTBR 集成不支持
       凭据参数，只能把密码塞进 URL，于是**网关管理密码明文躺进 HA 的
       .storage**——为了保护 NetworkKey 反而扩散了权限更高的东西。
       想锁的人自己打开，见 docs/13 决策 1.5。 */
    bool     rest_auth;

    /* OTBR 专用访问 token（32 位小写十六进制，空 = 未启用）。2026-08-23 新增。
       解决的正是上面那段话里的死结：想保护 NetworkKey，就得给 HA 一个凭据；
       而 HA 的 otbr 集成**不支持任何认证参数**（python-otbr-api 把 URL 原样
       拼接，凭据只能靠 aiohttp 的 userinfo 走 Basic），于是唯一的凭据只能是
       网关管理密码 —— 为了保护一把钥匙而扩散一把权限更大的钥匙。

       token 把这件事拆开：它**只在 otbr_api.c 那 25 条路由上有效**，打
       /api/v1 下的任何端点 一律无效（判据在 tools/test_api.sh 的 TC-T 组）。泄露的后果
       是"能读写 Thread 数据集"——正是这把钥匙本该有的权限。

       ⚠️ 设了 token 之后，OTBR 路由上的 Basic **只认 token，不再认管理密码**。
          这不是疏漏：HA 若拿着过期 token 反复轮询，每次失败都会撞进管理密码的
          比对路径，把登录节流的计数器顶上去，**最终把管理员自己锁在门外**。
          WebUI 走会话 cookie，不受影响。
       ⚠️ 十六进制是刻意的：aiohttp 按 latin1 编 Basic，非 ASCII 直接
          UnicodeEncodeError；`/` 会让 yarl 抛 ValueError。十六进制两个坑都躲开。 */
    char     otbr_token[33];
} thread_config_t;

typedef struct {
    bool     enabled;
    char     broker[URL_MAX_LEN];
    uint16_t port;
    char     username[PASSWORD_MAX_LEN];
    char     password[PASSWORD_MAX_LEN];
    char     topic_prefix[TOPIC_MAX_LEN];
    uint16_t publish_interval;  // seconds
    bool     tls_enabled;
} mqtt_config_t;

typedef struct {
    bool     enabled;
    char     url[URL_MAX_LEN];
    uint32_t backup_interval;   // seconds, 0=disabled
    bool     event_zigbee_crash;
    bool     event_network_down;
    bool     event_interference;
    bool     event_ota;
    bool     event_temp_high;
    bool     event_memory_low;
} webhook_config_t;

typedef struct {
    uint8_t  brightness;        // 0-255
    bool     night_mode_enabled;
    uint8_t  night_start_hour;
    uint8_t  night_start_min;
    uint8_t  night_end_hour;
    uint8_t  night_end_min;
    uint32_t color_booting;     // 0xRRGGBB
    uint32_t color_normal;
    uint32_t color_zigbee;
    uint32_t color_error;
    uint32_t color_ota;
} led_config_t;

typedef struct {
    bool     enabled;
    uint8_t  count;             // 0-8
    uint32_t allowed_ips[MAX_WHITELIST_IPS];
} tcp_whitelist_config_t;

/*
 * ⚠️ timezone 是 POSIX TZ 串，**符号与直觉相反**：`PST8PDT` = UTC-8，
 * `CST-8` = UTC+8。写反了会差出两倍时差，而且看着仍像个合理时间。
 *
 * tz_declared 区分"用户真的设过"和"用的是出厂默认 UTC+0"——同 zigbee_config_t
 * 的 channel_declared 一个套路。为什么必须区分：时区决定 scheduler 的夜间模式
 * 时间窗，默认 UTC 下用户设的"22:00 关灯"关的是 UTC 的 22:00，行为完全对不上
 * 直觉，而且**这个错误不会自己暴露**（灯照亮，只是时间不对）。有了这个标志，
 * 前端才能在没设过时主动提示，而不是拿默认值冒充有效配置。
 *
 * 老备份没有这个字段 → 导入后为 false（未声明），不能当成已声明。
 */
typedef struct {
    char     server[URL_MAX_LEN];
    char     timezone[TIMEZONE_MAX_LEN];
    uint32_t sync_interval;     // seconds
    bool     tz_declared;       // 用户是否显式设过时区
} ntp_config_t;

// ---------------- Top-Level Config ----------------

typedef struct {
    uint8_t              version;
    sys_config_t         sys;
    wifi_config_data_t   wifi;
    eth_config_t         eth;
    zigbee_config_t      zigbee;
    thread_config_t      thread;
    mqtt_config_t        mqtt;
    webhook_config_t     webhook;
    led_config_t         led;
    tcp_whitelist_config_t whitelist;
    ntp_config_t         ntp;
} gateway_config_t;

// ---------------- Events ----------------

ESP_EVENT_DECLARE_BASE(CONFIG_EVENT);

typedef enum {
    CONFIG_EVENT_WIFI_CHANGED,
    CONFIG_EVENT_ETH_CHANGED,
    CONFIG_EVENT_MQTT_CHANGED,
    CONFIG_EVENT_WEBHOOK_CHANGED,
    CONFIG_EVENT_LED_CHANGED,
    CONFIG_EVENT_ZIGBEE_CHANGED,
    CONFIG_EVENT_THREAD_CHANGED,
    CONFIG_EVENT_NTP_CHANGED,
    CONFIG_EVENT_HOSTNAME_CHANGED,
    CONFIG_EVENT_WHITELIST_CHANGED,
    CONFIG_EVENT_BRIDGE_MODE_CHANGED,
    CONFIG_EVENT_RADIO_REGION_CHANGED,   /* sys.radio_region 变了；thread_br 据此重设功率 */
    CONFIG_EVENT_IMPORTED,
    CONFIG_EVENT_FACTORY_RESET,
    CONFIG_EVENT_NVS_ERROR,
} config_event_id_t;

// ---------------- Public API: Init ----------------

esp_err_t config_mgr_init(void);

// ---------------- Public API: Read (no lock, zero latency) ----------------

const gateway_config_t       *config_mgr_get_all(void);
const wifi_config_data_t     *config_mgr_get_wifi(void);
const eth_config_t           *config_mgr_get_eth(void);
const zigbee_config_t        *config_mgr_get_zigbee(void);
const thread_config_t        *config_mgr_get_thread(void);
const mqtt_config_t          *config_mgr_get_mqtt(void);
const webhook_config_t       *config_mgr_get_webhook(void);
const led_config_t           *config_mgr_get_led(void);
const ntp_config_t           *config_mgr_get_ntp(void);
const sys_config_t           *config_mgr_get_sys(void);
const tcp_whitelist_config_t *config_mgr_get_whitelist(void);

// ---------------- Public API: Write (2s debounce) ----------------

esp_err_t config_mgr_set_wifi(const char *ssid, const char *password);
esp_err_t config_mgr_set_wifi_static_ip(const esp_netif_ip_info_t *ip, uint32_t dns);
esp_err_t config_mgr_set_eth(const eth_config_t *eth);
esp_err_t config_mgr_set_zigbee(const zigbee_config_t *zb);
esp_err_t config_mgr_set_thread(const thread_config_t *thread);
esp_err_t config_mgr_set_mqtt(const mqtt_config_t *mqtt);
esp_err_t config_mgr_set_webhook(const webhook_config_t *wh);
esp_err_t config_mgr_set_led(const led_config_t *led);
esp_err_t config_mgr_set_ntp(const ntp_config_t *ntp);
esp_err_t config_mgr_set_hostname(const char *hostname);
/* 射频地区："" 清除，否则必须是 radio_region.h 菜单里的码（大小写不敏感，存大写）。
   ⚠️ 只存 + 发事件。把它下发给 MG24 是 ezsp_bridge_ncp_resync() 的事（要独占 UART），
   调用方自己决定何时做；开机路径无条件同步一次。 */
esp_err_t config_mgr_set_radio_region(const char *cc);
/* ⚠️ 与 set_hostname 不同，这个是**真的会被调用**的（/api/v1/net/access-host）。
   set_hostname 至今零调用方，本次刻意不接通它 —— 接通等于让"把 hostname 改成
   evil.com"变成可能，而 hostname 是守卫白名单的基准。 */
esp_err_t config_mgr_set_extra_host(const char *h);
/* t = 32 位小写十六进制，或空串（关闭）。其它一律 ESP_ERR_INVALID_ARG。 */
esp_err_t config_mgr_set_otbr_token(const char *t);
esp_err_t config_mgr_set_bridge_mode(uint8_t mode);
esp_err_t config_mgr_set_password(const char *new_password);
esp_err_t config_mgr_set_whitelist(const tcp_whitelist_config_t *wl);
/* 2026-09-11(T4.3)：删掉了 sys.firmware_channel 与
 * config_mgr_set_firmware_channel()。
 *
 * 它存得好好的 —— NVS 读写、导出、导入、校验全有 —— 而**消费者为零**。
 * 全仓唯一提到它的地方是 smlight_compat.c 里一句注释，内容恰恰是
 *「别用它，它是死字段」。
 *
 * 本来打算随云端 OTA 接上（manifest 的 channels 映射），但云端 OTA 已于
 * 2026-09-10 上线，manifest 格式定型且**没有** channels 键（实测线上只有
 * version/url/sha256/size/notes/www/ncp）。现在补等于改一个已经在跑的格式
 * 加发布脚本，为一个没人点的功能。真要做多通道，那时连 manifest 一起设计。
 *
 * ⚠️ 旧备份里仍有 "firmware_channel" 键：导入按未知键忽略，不报错。 */

// ---------------- Public API: Persist / Export / Reset ----------------

esp_err_t config_mgr_save_now(void);
/* 重启前调：还在 2 s 防抖里没落盘的改动现在写进去（2026-10-08：设置页"重启"0.5 s 后就 esp_restart，
   改完设置马上点重启，改动悄悄丢了）。拿锁最多等 timeout_ms —— 发起重启的一方可能正持着配置锁、或持锁的任务卡死了
   （看门狗 L4 就是为它重启），那就放弃，返回 ESP_ERR_TIMEOUT，别卡住重启。没有待写的、或恢复出厂途中，直接 ESP_OK。
   不用关机钩子：IDF 的 5 个槽已被 esp_timer / WiFi / SRP 告别 / TCP 断开 / 网口 PHY 占满。 */
esp_err_t config_mgr_flush(uint32_t timeout_ms);
char     *config_mgr_export_json(void);             // caller must free()
esp_err_t config_mgr_import_json(const char *json);

/* 恢复出厂的清除范围。OpenThread 的网络数据集（网络密钥、PAN ID、子节点表）
   和网关配置存在同一个 nvs 分区里，所以"要不要连 Thread 网络一起清"必须由
   调用方明说：
     KEEP_THREAD —— 默认，Thread 网络原样保留，设备重启后子设备自动回来。
                    按键走这条：用户按它多半是网配错了或密码忘了，不该赔上
                    整个 Thread 网络。
     WIPE_ALL    —— 连 Thread 一起清。所有 Thread / Matter 设备变孤儿，必须
                    逐个重新配网。设备转手、或怀疑 OT 数据本身有问题时才用。
   枚举值 0 是安全的那个：漏传/零初始化不会误擦 Thread。 */
typedef enum {
    FACTORY_RESET_KEEP_THREAD = 0,
    FACTORY_RESET_WIPE_ALL,
} factory_reset_scope_t;

esp_err_t config_mgr_factory_reset(factory_reset_scope_t scope);
#if CONFIG_BITENGINE_PROFILE_DIRECT
/* 直连型：恢复出厂时清 hubdb 里归它管的那些（docs/15 设计第十二节：两种都清 muse，全清再清 hub，zhub 保留）。
   config_mgr.c 里是弱定义（只打 E、不假装清过），hub_core 的 hub_store.c 给强定义 —— config_mgr 不能依赖 hub_core
   （hub_core 依赖 config_mgr，成环）。返回 ESP_OK = 清好了。 */
esp_err_t config_mgr_hubdb_factory_reset(bool wipe_all);
#endif

/* 2026-09-11(T4.1/T4.2)：删掉了 config_mgr_backup_to_flash() /
 * config_mgr_restore_from_flash() 与整个 config_backup.c。
 *
 * 和 spectrum_history 同一个根因：四处实现全在
 * #ifdef CONFIG_BITENGINE_HAS_EXT_FLASH 里等一颗 W25Q32 外部 Flash，而那颗芯片
 * **BOM 已确认永久不上件**，GPIO8 早已改作 DM9051 的 PHY_RESET。
 * 更糟的是 backup_to_flash() 挂在**每次配置落盘**的路径上，每次都白调一次
 * 只为返回 NOT_SUPPORTED。
 *
 * 不做替代实现：NVS 坏掉时的出路是恢复出厂 + 用户自己的
 * GET /api/v1/config/export 备份，那条路已经有了、验过、也写进手册了。
 * 再在同一块主 Flash 上存一份"备份"，介质坏了两份一起坏，并不增加安全性。 */

// ---------------- Public API: Password ----------------

bool config_mgr_verify_password(const char *password);
bool config_mgr_has_password(void);
// 没有 clear_password —— 清密码于 2026-08-14 撤销，忘记密码只能恢复出厂。
// 别再加回来：它是唯一能清空 password_hash 又不触发恢复出厂的入口。

// ---------------- Internal (cross-file) ----------------

void      config_set_defaults(gateway_config_t *cfg);
void      config_run_migrations(uint8_t stored_version);

#ifdef __cplusplus
}
#endif
