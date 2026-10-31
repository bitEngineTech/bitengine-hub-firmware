#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_sntp.h"
#include "log_store.h"
#include "device_identity.h"
#include "poe.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "devkit_pins.h"

#include "watchdog.h"
#include "wd_heaptrace.h"
#include "idf_fixes.h"      /* 诊断构建的 ECDSA 复位对照压测（T9.47）；出厂只在链接期生效，没有调用 */
#include "psram_selftest.h"
#include "config_mgr.h"
#include "network_mgr.h"
#include "mdns_svc.h"
#include "ezsp_bridge.h"
#include "ota_mg24.h"
#include "ota_cloud.h"
#include "webui.h"
#include "bitengine_mqtt.h"
#include "webhook.h"
#include "spectrum.h"
#include "thread_br.h"
#include "ble_proxy.h"
#include "matter_bridge.h"
#include "led_ctrl.h"
#include "button.h"
#include "scheduler.h"
#if CONFIG_BITENGINE_PROFILE_DIRECT
#include "hub_store.h"      /* 直连型（docs/15 设计 2.5）：hubdb 的唯一写入者 */
#include "hub_svc.h"        /* 直连型设备层的常驻服务：事件管线、Thread 清单、审计环（设计第十七节"M4 之前"） */
#endif
#if CONFIG_BITENGINE_ENABLE_ZHUB
#include "zhub.h"           /* 直连型的 Zigbee 主机（docs/15 第八节） */
#endif
#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
#include "ble_window.h"     /* 策略 C 的配对剖面（docs/15 设计 3.2） */
#include "muse_link.h"      /* Muse 会话（docs/15 设计第四节） */
#include "muse_hold.h"      /* 安全模式退出时"先关掉 Muse"的留言（附录 E33） */
#include "muse_pair.h"
#endif
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
#include "matter_ctrl.h"      /* 直连型的 Matter 控制器（docs/15 第九节） */
#endif
static const char *TAG = "main";
static EventGroupHandle_t s_net_events;
#define NET_ONLINE_BIT  BIT0
#define BOOT_SUBSCRIBERS_READY_BIT  BIT1   /* 直连型：webhook 已订阅，可以发 OTA_VERIFIED 了（见 direct_verify_task） */

static RTC_NOINIT_ATTR uint32_t s_boot_attempts;   /* Boot retry tracking — survives SW reset, cleared on power-on/EN reset */
#define BOOT_ATTEMPTS_MAGIC 0xC5CA0000  /* upper 16 bits = magic, lower 16 = count */

#if CONFIG_BITENGINE_PROFILE_DIRECT
/* 直连型的等网与 mark valid（docs/15 设计 2.6，交叉清单 A-19）。HA 型有配网热点兜底，几乎总能等到网，
   所以"等 10 分钟、等不到就重启"；直连型没有兜底，照搬就是网线不在时每 10 分钟重启一次。 */
#define DIRECT_NET_WAIT_MS         (30 * 1000)        /* 最多等这么久，之后不管有没有网都往下启动 */
#define DIRECT_VERIFY_DEADLINE_MS  (10 * 60 * 1000)   /* OTA 后首次启动，拿到以太网地址的期限（从开机算） */

/* OTA 后首次启动、等网 30 s 还没网时由它接着等。mark valid 仍然要等以太网拿到 IP：以太网是直连型唯一的
   出入口，一个弄坏了以太网的固件若被标成有效，就再也 OTA 不回来（红线 1）。期限内拿不到就 esp_restart()
   （C5 上是 CPU 复位、不重采 strap，不撞 IO28），bootloader 见 PENDING_VERIFY 就回滚到上一版。
   内部栈（xTaskCreate）：mark valid 写 otadata，PSRAM 栈的任务不能碰 flash（psram_task.h 规则 1）。 */
static void direct_verify_task(void *arg)
{
    (void)arg;
    EventBits_t bits = xEventGroupWaitBits(s_net_events, NET_ONLINE_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(DIRECT_VERIFY_DEADLINE_MS - DIRECT_NET_WAIT_MS));
    if (bits & NET_ONLINE_BIT) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "以太网已拿到地址，新固件首次启动已确认，回滚窗口关闭");
            /* 通知要等 webhook 订阅之后才发：网可能在开机流程走到 webhook_init() 之前就到了，esp_event 不回放，
               先 post 后订阅等于没发（T2.7 栽过一次）。安全模式在 webhook_init() 之前 return，等不到就不发。 */
            if (xEventGroupWaitBits(s_net_events, BOOT_SUBSCRIBERS_READY_BIT, pdFALSE, pdTRUE,
                                    pdMS_TO_TICKS(5 * 60 * 1000)) & BOOT_SUBSCRIBERS_READY_BIT) {
                esp_event_post(WEBUI_EVENT, WEBUI_EVENT_OTA_VERIFIED, NULL, 0, 0);
            } else {
                ESP_LOGW(TAG, "开机流程没走到 webhook_init()（安全模式？）—— 不发升级完成通知");
            }
        } else {
            ESP_LOGE(TAG, "mark valid 失败（%s）—— 下次重启会回滚", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "OTA 后首次启动 %d 分钟内没拿到以太网地址 —— 重启，由 bootloader 回滚到上一版（docs/15 设计 2.6）",
                 DIRECT_VERIFY_DEADLINE_MS / 60000);
        vTaskDelay(pdMS_TO_TICKS(200));     /* 让这条日志落进日志环 */
        esp_restart();
    }
    vTaskDelete(NULL);
}
#endif

/* 看门狗整机重启前要问的"有没有升级在写 flash"：三条路合起来（T9.30 ①） */
static bool any_ota_busy(void)
{
    return webui_ota_busy() || ota_cloud_busy() || ota_mg24_busy();
}

static void network_event_handler(void *arg, esp_event_base_t base,
                                  int32_t id, void *data)
{
    if (id == NETWORK_EVENT_ONLINE) {
        net_interface_t iface = *(net_interface_t *)data;
        ESP_LOGI(TAG, "Network online via %s",
                 iface == NET_IF_ETH ? "ETH" : "WiFi");
        /* Network is up — boot succeeded, clear the retry counter so the
           next WiFi failure starts a fresh retry budget. */
        s_boot_attempts = 0;
        xEventGroupSetBits(s_net_events, NET_ONLINE_BIT);
#if CONFIG_BITENGINE_PROFILE_DIRECT
        /* 直连型开机不等网（docs/15 设计 2.6），SNTP 可能在没网时就起了：lwIP 失败后按指数退避，
           网到了还要再等一两分钟才重试（2026-10-21 上板：开机不插线、后插线，网通后 133 s 才对上；
           墙钟不对，要验证书的 TLS 都连不上）。网一到就重启一次客户端，立刻按新网络去对时。
           sntp_restart() 只投一个 tcpip_callback，不阻塞；还没 init 过时什么都不做（开机 30 s 内就来网的照旧）。 */
        esp_sntp_restart();
#endif
    } else if (id == NETWORK_EVENT_OFFLINE) {
        ESP_LOGW(TAG, "Network offline");
        xEventGroupClearBits(s_net_events, NET_ONLINE_BIT);
    } else if (id == NETWORK_EVENT_SOFTAP_STARTED) {
        /* WiFi STA failed → SoftAP fallback. Do NOT restart-retry here:
           a restart can't conjure an absent AP (reason=201), the one case
           it did fix (5GHz radio wedged after a non-clean reset) is already
           handled by the reset-reason check at boot, and network_mgr keeps
           retrying STA in the background from SOFTAP_ONLY anyway. Restart
           cycles only tear down the provisioning AP under the user. */
        ESP_LOGI(TAG, "SoftAP up, STA retries continue in background — "
                 "unblocking startup");
        xEventGroupSetBits(s_net_events, NET_ONLINE_BIT);
    }
}

/* 把 NTP 配置下发给 SNTP 客户端和 libc。init 和"配置改了"两条路径都走这里，
   否则 POST /api/v1/ntp 就只是把值存进 NVS 而不生效 —— 那正是本项目一贯拒绝的
   假功能。时区尤其要紧：它决定 scheduler 的夜间模式时间窗，配置默认是 UTC+0，
   用户不改的话"22:00 关灯"关的是 UTC 的 22:00。 */
static void sntp_apply_config(void)
{
    const ntp_config_t *ntp = config_mgr_get_ntp();

    /* 注意：lwIP 的 sntp_setservername 存的是**指针**不是拷贝。这里传的是
       g_config.ntp.server 的地址，生命周期与程序等长且内容原地更新，安全。 */
    esp_sntp_setservername(0, ntp->server);
    esp_sntp_set_sync_interval(ntp->sync_interval * 1000);

    setenv("TZ", ntp->timezone, 1);
    tzset();

    ESP_LOGI(TAG, "SNTP: server=%s tz=%s interval=%us",
             ntp->server, ntp->timezone, (unsigned)ntp->sync_interval);
}

static void ntp_config_changed(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    sntp_apply_config();
    /* 改了服务器/周期要重启客户端才会立刻按新参数走，否则要等到下一个
       同步周期（默认 1 小时）才生效。 */
    esp_sntp_restart();
    ESP_LOGI(TAG, "NTP 配置已重新下发");
}

/* SNTP 对上时 → 告诉 network_mgr（T9.20 开机接管要知道"墙钟这次开机对过"）。
   ⚠️ 回调槽全系统只有一个（lwIP 的 sntp.c 用一个静态指针，后注册的静默顶掉前一个），所以只在
   SNTP 的所有者这里注册一次；别处要"对时完成"的信号，从 network_mgr 拿，别再注册第二个。
   也别轮询 esp_sntp_get_sync_status() —— 它读到 COMPLETED 就复位，几处一起轮询会互相偷走信号。
   回调跑在 lwIP 线程里：network_mgr_notify_time_synced() 不阻塞、不拿锁。 */
static void on_sntp_synced(struct timeval *tv)
{
    (void)tv;
    network_mgr_notify_time_synced();
}

static void sntp_sync_init(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_apply_config();
    sntp_set_time_sync_notification_cb(on_sntp_synced);
    esp_sntp_init();

    esp_event_handler_instance_register(CONFIG_EVENT, CONFIG_EVENT_NTP_CHANGED,
                                        ntp_config_changed, NULL, NULL);
}

/*
 * RTS reset from USB-UART (after idf.py flash) doesn't fully reset the
 * 5GHz WiFi radio on ESP32-C5 DevKit. Detect this and do a clean SW restart.
 * RTC_NOINIT memory survives SW reset but is cleared on power-on/EN reset.
 */
void app_main(void)
{
#ifdef CONFIG_BITENGINE_HAS_HW_WATCHDOG
    /* RS706 counts from power-on if WDI is not truly floating; its 1.6s (typ)
       timeout can fire before watchdog_init() runs. Kick WDI first thing —
       the edge restarts the RS706 timer, buying time until wdt_feed_task. */
    gpio_config_t wdi_io = {
        .pin_bit_mask = 1ULL << WDT_WDI_PIN,
        .mode         = GPIO_MODE_OUTPUT,
    };
    /* Latch out-reg before direction switch (glitch-free init; a WDI low
       glitch is only an extra watchdog feed, but keep the pattern uniform). */
    gpio_set_level(WDT_WDI_PIN, 1);
    gpio_config(&wdi_io);
#endif

#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
    /* 策略 C 的一次性标志（docs/15 设计 3.2）：必须在下面"非干净复位补重启"之前取走。否则"写了标志之后 panic"
       会被补重启变成"软件复位 + 合法标志"，误进配对剖面。取走即清，判定只认原始复位原因 ESP_RST_SW；
       安全模式要等 watchdog_init() 之后才知道，在那里再相与。 */
    const bool pair_boot = ble_window_take_pair_boot();
#endif

    /* Track boot attempts across software resets (RTC_NOINIT survives SW reset,
       cleared on power-on/EN reset). Used to restart at most once after a
       non-clean reset (RTS from flashing leaves the 5GHz radio wedged). */
    esp_reset_reason_t rst = esp_reset_reason();
    ESP_LOGI(TAG, "Reset reason: %d (1=poweron 2=ext 3=sw 4=panic 9=brownout 14=pwr_glitch)", rst);
    uint32_t attempts = 0;

    if ((s_boot_attempts & 0xFFFF0000) == BOOT_ATTEMPTS_MAGIC) {
        attempts = s_boot_attempts & 0xFFFF;
    }

    if (rst != ESP_RST_POWERON && rst != ESP_RST_SW && attempts == 0) {
        /* First non-clean reset (RTS from flash) — do SW restart */
        s_boot_attempts = BOOT_ATTEMPTS_MAGIC | 1;
        ESP_LOGW("main", "Non-clean reset (reason=%d), restarting for WiFi radio", rst);
        /* 下一次开机的复位原因会是 ESP_RST_SW，但这次的墙钟带着 RC 慢时钟的漂移 —— 告诉 network_mgr
           别把它当成继承来的可信墙钟（T9.20 开机接管判租期用，见 network_mgr.h） */
        network_mgr_note_unclean_reset();
        /* 补重启会把复位原因变成 SW —— 先把真原因留在 RTC，否则 /status 与异常开机告警都看不到它（T9.27） */
        wd_boot_reason_note(rst);
        esp_restart();
    }
    wd_boot_reason_latch();
    /* Normalize garbage RTC memory (first power-on) to a valid zero counter.
       Do NOT clear a valid counter here: the SoftAP-fallback handler must see
       the accumulated retry count, otherwise WiFi failure restarts forever and
       SoftAP provisioning is never reached. The counter is cleared in
       network_event_handler once the network actually comes online. */
    if ((s_boot_attempts & 0xFFFF0000) != BOOT_ATTEMPTS_MAGIC) {
        s_boot_attempts = 0;
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  bitEngine Hub Firmware v1.0");
    ESP_LOGI(TAG, "========================================");
    watchdog_heap_mark("boot");

    // NVS init
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 日志留存尽早装：装得越晚，启动早期的日志就抓不到，而"开机时发生了什么"
       恰恰是最需要远程查看的场景。放在 NVS 之后是因为要先有堆和 PSRAM。
       失败不致命 —— 串口输出不受影响，只是 /api/v1/logs 会回 503。 */
    log_store_init();

    /* Event loop must exist before any module that posts events
       (watchdog posts ABNORMAL_BOOT, config_mgr posts CONFIG_EVENT). */
    s_net_events = xEventGroupCreate();
    if (!s_net_events) {
        ESP_LOGE(TAG, "Event group alloc failed, restarting");
        esp_restart();
    }
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        NETWORK_EVENT, ESP_EVENT_ANY_ID, network_event_handler, NULL, NULL));

    /* 出厂身份：只读一次 fctry 分区然后就把它还回去。无副作用、不依赖任何人，
       但 webui / 产线自检都要用，所以放在它们前面。没写身份的板子（开发板、
       产线上还没走到写身份那步的板子）照常启动 —— 见 device_identity.h。 */
    device_identity_init();

    /* PoE 检测：只配一个输入脚，无副作用，放这儿保证后面谁都能查供电方式。
       IO0 的 strapping 风险已于 2026-08-13 用 5 次带 PoE 冷启动排除。 */
    poe_init();

    // Watchdog — start feeding ASAP
    watchdog_init();
    wd_recovery_set_ota_busy_query(any_ota_busy);
    watchdog_heap_mark("watchdog");

#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
    /* 安全模式优先（设计 3.2）：有安全模式标志就不进配对剖面，走下面的安全模式出口。
       正常开机（策略 C）永远不用 BT：趁谁都还没碰 BT，把它的静态 .bss/.data（约 1 KB）还给堆。⚠️ 不是"窗口外零占用"：
       控制器的 IRAM 代码（约 25 KB）链接进来就常驻，要省得看 CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY（G0-4 (d) 评估）。 */
    ble_window_set_pair_boot(pair_boot && !wd_safe_mode());
    if (pair_boot && wd_safe_mode()) {
        ESP_LOGW(TAG, "安全模式优先 —— 这次不进 Muse 配对剖面");
    }
    if (!ble_window_pair_boot_active()) {
        ble_window_release_bt_mem();
    }
    watchdog_heap_mark("bt_mem");
#endif

#if CONFIG_BITENGINE_PSRAM_SELFTEST
    /* Replaces CONFIG_SPIRAM_MEMTEST; must stay between watchdog_init()
       (wdt_feed_task keeps the RS706 fed during the ~1s test) and
       network_mgr_init() (the test briefly claims all free PSRAM). */
    if (psram_selftest_run() == ESP_FAIL) {
        ESP_LOGE(TAG, "PSRAM self-test FAILED — hardware fault, expect instability");
    }
#endif
    watchdog_heap_mark("psram_test");
#if CONFIG_BITENGINE_DIAG_HEAP_TRACE
    /* 诊断固件专用（出厂 n）：要在网络起来之前订阅以太网断线，断线时才能最先开窗。
       失败已在内部报 E，诊断功能缺失不影响启动。 */
    (void)wd_heaptrace_init();
#endif

    // Config manager — load NVS into memory
    config_mgr_init();
    watchdog_heap_mark("config");

    /* LED 必须在 network_mgr_init() **之前**起来，两个理由：
       ① 用户可见性：下面那段 xEventGroupWaitBits() 会阻塞等联网（最长 10 分钟），
          此前 led_ctrl 排在它后面，整个等待期四颗灯无人驱动、全黑 —— 开机后要
          "过好一会儿" SYS 才亮，用户分不清是在启动还是板子死了。现在开机 1s 内
          就是 SYS 黄常亮 + net/zigbee 琥珀慢闪，一路亮到联网成功再转绿。
       ② 事件竞态：NETWORK_EVENT_ONLINE 是边沿触发，ESP-IDF 默认事件循环不重放。
          在这里注册意味着 network_mgr 还没启动，任何网络事件都不可能被错过；
          led_ctrl_init() 里那段"补读当前网络状态"从此降级为纯保险。

       依赖已满足：watchdog_init()(wd_register) / PSRAM 自检(psram_task_create) /
       config_mgr_init()(亮度) 都在上面。渲染循环用到的 network_mgr / ezsp_bridge
       两个 getter 在各自 init 之前调用是安全的（纯静态变量读取），此时分别报
       NET_STATE_INIT 和 active_clients=0 —— 正好就是"启动中"的诚实显示。 */
    led_ctrl_init();
    watchdog_heap_mark("led");

    // Network manager
    ret = network_mgr_init();
    if (ret != ESP_OK) {
        /* Without networking nothing else can function — a half-dead device
           is worse than a rebooting one. Delay to avoid a tight reset loop. */
        ESP_LOGE(TAG, "Network manager init failed: %s — restarting in 10s",
                 esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }

    watchdog_heap_mark("net_init");
    ESP_LOGI(TAG, "Waiting for network...");
#if CONFIG_BITENGINE_PROFILE_DIRECT
    /* 直连型：最多等 30 s，之后不管有没有网都往下走（设计 2.6）。要网的组件自己等 NETWORK_EVENT_ONLINE。 */
    EventBits_t bits = xEventGroupWaitBits(s_net_events, NET_ONLINE_BIT,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(DIRECT_NET_WAIT_MS));
    const bool net_online = (bits & NET_ONLINE_BIT) != 0;
    if (!net_online) {
        ESP_LOGW(TAG, "No network after %d s — starting the rest without it (direct profile, docs/15 design 2.6)",
                 DIRECT_NET_WAIT_MS / 1000);
    }
#else
    /* Bounded wait: if neither STA nor the SoftAP fallback ever comes up
       (e.g. SoftAP start itself failed), the device is unreachable — restart
       rather than hang here forever. 10 min covers the 3 WiFi retries. */
    EventBits_t bits = xEventGroupWaitBits(s_net_events, NET_ONLINE_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(10 * 60 * 1000));
    if (!(bits & NET_ONLINE_BIT)) {
        ESP_LOGE(TAG, "No network and no SoftAP after 10 min — restarting");
        esp_restart();
    }
#endif

    /* Network reachable (or SoftAP fallback up) — accept this image.
       With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, an OTA image that
       crashes before this point rolls back to the previous slot. */
    watchdog_heap_mark("net_online");

    /* ── T2.7：把"这次是 OTA 后第一次启动"和"上次回滚过"这两件事说出来 ──
     *
     * 判据是**必须在 mark_valid 之前取**的：PENDING_VERIFY 正是被下面那行清掉的。
     * 普通开机读到的是 VALID，只有刚 OTA 完的第一次启动才是 PENDING_VERIFY。
     *
     * 回滚以前完全不可见：机制是对的（bootloader 会切回旧槽），但
     * esp_ota_get_last_invalid_partition() 全仓零命中 —— 设备默默回到旧版本，
     * 用户只觉得"升级没生效"，而日志里一个字都没有。 */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t   img_state;
    bool first_boot_after_ota =
        (running && esp_ota_get_state_partition(running, &img_state) == ESP_OK
         && img_state == ESP_OTA_IMG_PENDING_VERIFY);

    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    if (bad) {
        ESP_LOGW(TAG, "上一次 OTA 被回滚了：%s 启动失败，现在跑的是 %s",
                 bad->label, running ? running->label : "?");
    }

#if CONFIG_BITENGINE_PROFILE_DIRECT
    if (!net_online && first_boot_after_ota) {
        /* 还没网就不确认：交给 direct_verify_task 等以太网地址（期限内拿不到就重启回滚）。
           "升级完成"通知也由它在确认之后发（等到下面 webhook_init() 之后），下面那处就不发了。
           任务在这里就建、不挪到 webhook_init() 之后：安全模式在那之前 return，挪过去回滚期限就没人管了。 */
        if (xTaskCreate(direct_verify_task, "ota_verify", 3072, NULL, 4, NULL) != pdPASS) {
            ESP_LOGE(TAG, "ota_verify 任务建不了 —— 不确认这版固件，下次重启会回滚");
        }
        first_boot_after_ota = false;
    } else if (net_online) {
        esp_ota_mark_app_valid_cancel_rollback();
    }
    /* 已经确认过的固件没网：什么都不做，也不因为没网而重启（设计 2.6） */
#else
    esp_ota_mark_app_valid_cancel_rollback();
#endif

    if (first_boot_after_ota) {
        ESP_LOGI(TAG, "新固件首次启动已确认（分区 %s），回滚窗口关闭",
                 running->label);
        /* ⚠️ "升级完成"的 WEBUI_EVENT_OTA_VERIFIED **不在这里发**，在下面
           webhook_init() 之后发（见 "OTA_VERIFIED" 处）。2026-09-11 T2.7 曾把它
           放在这里：这时 webhook 的事件订阅还没注册（webhook_init 在 1 秒多之后），
           默认事件循环当场分发、无人接收 —— 通知照样一次都没送出去过，
           2026-09-13 真发一版云端 OTA 才发现（ota_available 收到了、ota_complete 没有）。
           mark-valid 必须留在这里（网络一就绪就关回滚窗口），发通知可以晚。 */
    }

    // SNTP time sync
    sntp_sync_init();
    watchdog_heap_mark("sntp");

    // mDNS service registration (replaces old mdns_init_service)
    mdns_svc_init();
    watchdog_heap_mark("mdns");

#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
    /* ---- Muse 配对剖面（策略 C，设计 3.2）----
       只起：网络（上面）、config、LED（上面）、hubdb、最小 WebUI、按键、muse_pair（BLE 窗口在它里面开）。
       不起：MG24 UART / NCP 升级、云端更新、thread_br、spectrum、mqtt、webhook、scheduler、matter —— 这次开机射频
       只给 BLE（#1851：BLE 与 Thread 不同时出现在一次开机里）。配完（成功 / 失败 / 超时 / 取消）muse_pair 自己
       记一笔计划内重启再 esp_restart() 回正常开机。安全模式已在上面排除。 */
    if (ble_window_pair_boot_active()) {
        if (hub_store_start() != ESP_OK) {
            ESP_LOGE(TAG, "hub_store 没起来 —— 配对剖面读不到 SDK token、也存不下令牌，muse_pair 会判失败重启");
        }
        webui_init();                 /* 配对剖面里只注册登录、Muse 端点、精简状态和一张配对进度页（webui_server.c） */
        button_init();
        if (muse_pair_boot_run() != ESP_OK) {
            ESP_LOGE(TAG, "muse_pair 起不来 —— 10 s 后重启回正常开机");
            vTaskDelay(pdMS_TO_TICKS(10000));
            esp_restart();
        }
        watchdog_heap_mark("muse_pair");
        ESP_LOGI(TAG, "  Ready (Muse pairing profile)  HTTP:80");
        return;
    }
#endif

    // EZSP Bridge
#if CONFIG_BITENGINE_ENABLE_ZHUB
    /* 直连型不带桥（docs/15 设计 2.6 第 6 步、架构 5.8 R-2）：只建 MG24 的 UART、开机读 NCP 版本；
       没有 :6638，UART 留给 MG24 升级与 M4 的 zhub。 */
    ret = ezsp_bridge_init_uart_only();
#else
    ret = ezsp_bridge_init();
#endif
    if (ret != ESP_OK) {
        /* Bridge down but keep booting: WebUI/diagnostics stay reachable so
           the user can see the error and recover (reboot/OTA) remotely. */
        ESP_LOGE(TAG, "EZSP bridge init failed: %s — continuing without bridge",
                 esp_err_to_name(ret));
    }
    watchdog_heap_mark("ezsp");

    /* MG24 NCP updater. Must come before webui_init(): the REST layer exposes
       /api/v1/ota/zigbee immediately, and those handlers refuse to run until
       this has verified the XMODEM CRC and created its lock. Failure only
       costs the MG24 update path, so keep booting. */
    ret = ota_mg24_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MG24 OTA init failed: %s — MG24 updates unavailable",
                 esp_err_to_name(ret));
    }
    watchdog_heap_mark("ota_mg24");

    /* 云端更新。同样必须在 webui_init() 之前：/api/v1/ota/check 与 /ota/cloud
       一注册就可达，而两者都要求这里已经建好互斥量（未初始化时 install() 会
       返回 INVALID_STATE，等于功能静默消失）。只建锁不联网，失败不影响开机。 */
    ret = ota_cloud_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "云端更新初始化失败: %s —— 检查更新功能不可用",
                 esp_err_to_name(ret));
    }
    watchdog_heap_mark("ota_cloud");

    // WebUI
    webui_init();
    watchdog_heap_mark("webui");

    /* ---- 安全模式（看门狗层 E）----
       1 小时窗口内自愈重启 ≥3 次就停在这里：只保留网络与 WebUI，让用户能登
       进来看诊断，而不是继续无限重启（重启解决不了的问题，再重启只会让设备时好时坏）。
       自愈重启走 esp_restart，是 CPU 复位、不重采 strap，不会因 IO28 卡进下载模式（X6）。 */
    if (wd_safe_mode()) {
        ESP_LOGE(TAG, "========================================");
        ESP_LOGE(TAG, "  安全模式：%s", wd_safe_mode_reason());
        ESP_LOGE(TAG, "  已跳过 Thread/Matter/MQTT/Webhook/频谱/调度");
#if CONFIG_BITENGINE_PROFILE_DIRECT
        ESP_LOGE(TAG, "  直连型另外跳过 hubdb/zhub/Muse（反复重启的是 Muse 时：网页上\"退出并先关掉 Muse\"，附录 E33）");
#endif
        ESP_LOGE(TAG, "  WebUI 可用，请从那里查看诊断并显式退出");
        ESP_LOGE(TAG, "========================================");
        watchdog_heap_mark("safe_mode");
        /* led_ctrl_init() 不在这里 —— 它已在 network_mgr_init() 之前跑过了。
           再调一次会重复注册 handler 并多起一个 led_task。 */
        button_init();
        ESP_LOGI(TAG, "  Ready (SAFE MODE)  HTTP:80");
        return;
    }

    // Phase 3: Spectrum engine + Thread BR
    spectrum_init();
    watchdog_heap_mark("spectrum");
    thread_br_init();
    watchdog_heap_mark("thread_br");

    // MQTT client (if enabled in config)
    mqtt_client_init();
    watchdog_heap_mark("mqtt");

    // Webhook (event notification)
    webhook_init();
    watchdog_heap_mark("webhook");

    /* OTA_VERIFIED：新固件首次启动的"升级完成"通知。必须在 webhook_init()（订阅者）
       **之后**才 post —— esp_event 不回放，先 post 后订阅等于没发。
       first_boot_after_ota 在上面网络就绪处算好；mark-valid 已经做过了。 */
    if (first_boot_after_ota) {
        esp_event_post(WEBUI_EVENT, WEBUI_EVENT_OTA_VERIFIED, NULL, 0, 0);
    }
#if CONFIG_BITENGINE_PROFILE_DIRECT
    xEventGroupSetBits(s_net_events, BOOT_SUBSCRIBERS_READY_BIT);   /* 放行 direct_verify_task 的通知 */
#endif

#if CONFIG_BITENGINE_PROFILE_DIRECT
    /* 直连型启动序列第 11 步（docs/15 设计 2.6）：hubdb + hub_store。排在安全模式出口之后（安全模式一律不起），
       在 zhub / matter_ctrl / muse_link 之前（它们都经它落盘）。起不来只记日志、状态里报，不拦后面的组件。 */
    if (hub_store_start() != ESP_OK) {
        ESP_LOGE(TAG, "hub_store 没起来 —— 直连型自己的数据（hubdb）这次开机读写不了");
    }
    watchdog_heap_mark("hub_store");
    /* 事件管线、Thread 网络层清单、审计环与它们的周期轮询（设计第十七节"M4 之前"）：不挂在 Muse 会话任务上 ——
       编译关掉 Muse、会话任务卡在 TLS 里，事件与清单照常；zhub / matter_ctrl 之后也往这里产事件。在 muse_link 之前起（它只认领推送） */
    if (hub_svc_start() != ESP_OK) {
        ESP_LOGE(TAG, "hub_svc 没起来 —— 这次开机事件与 Thread 清单不轮询");
    }
    watchdog_heap_mark("hub_svc");
#if CONFIG_BITENGINE_ENABLE_ZHUB
    /* 启动序列第 12 步（设计 2.6）：zhub —— 网关自己当 Zigbee 主机（第八节）。在 hub_store / hub_svc 之后（设备层以后经它们落盘、
       产事件）、muse_link 之前；安全模式出口之后（安全模式不起，CHECKLIST Z42 / C14）。它一起来就认领 UART、ASH 复位 NCP、networkInit；
       开机读 NCP 版本（ezsp_bridge_init_uart_only）早已做完。起不来只记日志：MG24 的 UART 留着给升级用，网页照常 */
    if (zhub_start() != ESP_OK) {
        ESP_LOGE(TAG, "zhub 没起来 —— 这次开机网关不接管 Zigbee 网络");
    }
    watchdog_heap_mark("zhub");
#endif
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
    /* 启动序列第 13 步（设计 2.6）：matter_ctrl —— 在 thread_br（设备经它的 SRP / 路由可达）、hub_store / hub_svc 之后，
       安全模式出口之后（安全模式不起）。自己晚起 30 s、带防崩溃循环（红线 1），这里只排上；起不来只记日志，网关其他功能照常 */
    if (matter_ctrl_start() != ESP_OK) {
        ESP_LOGE(TAG, "matter_ctrl 这次开机不起（防崩溃循环挡住了或建任务失败，见 /api/v1/matter/status）");
    }
    watchdog_heap_mark("matter_ctrl");
#endif
#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
    /* 刚从配对剖面回来就做 BLE 窗口之后的 Thread 自检（设计 3.3，TC-MU05）；否则只读出上次的结果 */
    (void)ble_window_selfcheck_start();
    /* 启动序列第 14 步（设计 2.6）：muse_link 在 hub_store 之后（凭据经它读写）、安全模式出口之后。
       起不来只记日志：Muse 的失败只影响 Muse 自己（N7） */
    /* 用户在安全模式里点了"退出并先关掉 Muse"（附录 E33）：在 muse_link_init() 读开关之前按留言把 Muse 关掉；
       关不掉（hub_store 没起来、写不进去）就这次不起 muse_link —— 宁可没有 Muse，也不让它照开、接着把网关崩回安全模式 */
    if (muse_hold_apply() != ESP_OK) {
        ESP_LOGE(TAG, "安全模式退出时要求关掉的 Muse 这次没能关上 —— 不起 muse_link，下次开机再试");
    } else if (muse_link_init() != ESP_OK || muse_link_start() != ESP_OK) {
        ESP_LOGE(TAG, "muse_link 没起来 —— 这次开机 Muse 连不上，网关其他功能照常");
    }
    watchdog_heap_mark("muse_link");
#endif
#endif

    /* BLE Proxy（被动扫描 → MQTT，给 HA 的蓝牙集成用）
       出厂关：BLE 扫描会破坏 802.15.4 的 CCA。此前这里是一行注释掉的调用，
       2026-08-17 改成 Kconfig 开关 —— 注释挡不住谁顺手删掉再启用，
       也不会在任何 VERIFY 里露面。开之前先解决射频共存，理由见 Kconfig 的 help。
       ⚠️ 关着它不省内存：A/B 全量构建逐段字节相同（ble_proxy_init 不被调用，
       链接器把整个归档丢掉），别为省内存去动 BT 配置。 */
#if CONFIG_BITENGINE_ENABLE_BLE_PROXY
    ble_proxy_init();
    watchdog_heap_mark("ble_proxy");
#endif

#if CONFIG_BITENGINE_DIAG_CRYPTO_RACE
    /* 诊断固件专用（出厂 n）：延时 90 s 后自己跑完、自删；结果在 /api/v1/logs（components/idf_fixes/crypto_race_diag.c） */
    crypto_race_diag_start();
#endif

    // Matter Bridge (framework, not yet active)
    matter_bridge_init();
    watchdog_heap_mark("matter");

    // LED control 已在 network_mgr_init() 之前初始化（见上方注释）

    // Button
    button_init();
    watchdog_heap_mark("button");

    // Scheduler (night mode, auto backup)
    scheduler_init();
    watchdog_heap_mark("scheduler");

    /* 启动完成时的分块堆快照。min 是每块 heap 各自的历史最低点，累加值
       （即 /api/v1/status 的 min_free_heap）不对应任何真实时刻，排查时看这里。 */
    ESP_LOGI(TAG, "heap: internal free=%u min=%u | spiram free=%u min=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_DEFAULT));

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Ready!  TCP:%d  HTTP:80", CONFIG_BITENGINE_TCP_PORT);
    ESP_LOGI(TAG, "========================================");
}
