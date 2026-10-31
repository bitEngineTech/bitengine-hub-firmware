#include "thread_br.h"
#include "srp_readvertise.h"
#include "watchdog.h"
#include "config_mgr.h"
#include "radio_region.h"
#include "mdns_svc.h"
#include "network_mgr.h"
#include "led_ctrl.h"
#include "ezsp_bridge.h"    /* ezsp_bridge_get_mode()：USB 桥模式下别给 OPENTHREAD 设级别 */
#include "ot_log_level.h"    /* BITENGINE_OT_ESP_LOG_LEVEL（映射只在那里写一次） */
#include "esp_timer.h"
#include "nvs.h"          /* "接入过设备"粘性标记 */

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "thread_br";


#ifdef CONFIG_OPENTHREAD_ENABLED

#include "esp_openthread.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_lock.h"
#include "openthread/srp_server.h"
#if CONFIG_BITENGINE_ENABLE_OT_CLI
#include "esp_openthread_cli.h"
#endif
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_vfs_eventfd.h"
#include "esp_coexist.h"
#include "esp_ieee802154.h"              /* 射频发射功率旋钮，见 apply_tx_power() */
#include "openthread/dataset.h"
#include "openthread/border_routing.h"
#include "openthread/dataset_ftd.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/logging.h"      /* otLoggingSetLevel —— 见 init 里那段 */
#include "openthread/thread_ftd.h"       /* otChildInfo / otThreadGetChildInfoByIndex */
#include "openthread/platform/radio.h"   /* otPlatRadioEnergyScanDone / OT_RADIO_RSSI_INVALID */
#include "openthread/thread.h"
#include "psram_task.h"   /* 任务栈放 PSRAM，省内部 SRAM */

/* 802.15.4 帧挂起表必须装得下全部子设备（理由在 sdkconfig.defaults）：表满时 ESP 的 OT 端口不报错、
   OT 的兜底失效，网关这边不会再把没进表的休眠子设备补进去。谁只抬了子设备上限、忘了挂起表，这里编译失败。 */
_Static_assert(CONFIG_IEEE802154_PENDING_TABLE_SIZE >= CONFIG_OPENTHREAD_MLE_MAX_CHILDREN,
               "IEEE802154_PENDING_TABLE_SIZE must be >= OPENTHREAD_MLE_MAX_CHILDREN (see sdkconfig.defaults)");

static volatile bool s_running = false;
/* stop 正在让主循环退出。ot_main_task 据此区分"被叫停"与"自己退了"——后者以前完全静默 */
static volatile bool s_stopping = false;
static TaskHandle_t  s_task_handle = NULL;

/* 子节点恢复进度定时器 —— 定义在文件后半段（紧挨 thread_br_get_child_stats），
   但 thread_br_start()/stop() 在前面就要用，这里前向声明。 */
static esp_timer_handle_t s_child_stats_timer;
static bool s_health_running;
static bool s_health_stalled;

static void child_stats_timer_cb(void *arg);
static void zigbee_check_task(void *arg);   /* 定义在文件后半段，随扫描代码 */
static TaskHandle_t s_zb_check_task;          /* 常驻，只建一次（见 zb_check_ensure） */

/* ---- "这个 Thread 网络接入过设备吗" 的粘性记录 ----
 *
 * 用于决定开机时能不能把网络挪到别的信道（thread_br_should_repick）。
 *
 * 为什么不直接读子节点表：一是建 dataset 那段跑在 otThreadSetEnabled() 之前，
 * 那时表里有没有东西取决于 OT 内部的恢复时机，不该赌；二是语义本来就该是
 * "有没有接入过"而不是"现在有几个"——休眠设备可能暂时不在表里，按当前数
 * 判断会把用户已经在用的网络当成空网络搬走。
 *
 * 只置位不清零，唯一的清零点是建了一个全新的 dataset（新网络 = 重新开始）。 */
#define TBR_NVS_NS        "thread_br"
#define TBR_NVS_HAD_CHILD "had_child"

/* 三态：-1 = 还没从 NVS 读过。不能用 bool 初值 false 冒充"未读"——建新 dataset
   的路径先 store(false) 后才有人 load()，用 bool 时那次写会被下面的幂等判断
   短路掉，NVS 里上一个网络残留的 1 就永远清不掉，新建的空网络会被当成
   "接入过设备"，自动重选对它彻底失效。做成三态后与调用顺序无关。 */
static int8_t s_had_child_cache = -1;

static bool had_child_load(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(TBR_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, TBR_NVS_HAD_CHILD, &v);
        nvs_close(h);
    }
    s_had_child_cache = (v != 0) ? 1 : 0;
    return s_had_child_cache != 0;
}

static void had_child_store(bool v)
{
    if (s_had_child_cache < 0) had_child_load();   /* 缓存没对齐过，先读 */
    if (s_had_child_cache == (v ? 1 : 0)) return;  /* 幂等，别磨损 Flash */
    nvs_handle_t h;
    if (nvs_open(TBR_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u8(h, TBR_NVS_HAD_CHILD, v ? 1 : 0) == ESP_OK) nvs_commit(h);
    nvs_close(h);
    s_had_child_cache = v ? 1 : 0;
    ESP_LOGI(TAG, "Thread 网络\"接入过设备\"标记 = %d", (int)v);
}

static esp_netif_t  *s_ot_netif = NULL;
static esp_openthread_platform_config_t s_ot_platform_cfg;

/* backbone netif 在 thread_br_start() 里设定，边界路由在 mainloop task 里初始化，
   两者跨任务，所以用标志位传递结果，并让最终日志如实反映有没有边界路由。 */
static volatile bool s_backbone_ready = false;
static volatile bool s_border_routing = false;
/* 边界路由当前用的 backbone。不能用 esp_openthread_get_backbone_netif()：它在
   border_router_init 完成前返回 NULL（初始化是异步的），会让"现在走哪个口"误判成 none。 */
static esp_netif_t * volatile s_backbone_netif = NULL;
/* 开机时边界路由由 ot_br_init_task 初始化；运行中的自动换口要等它做完，否则两边各
   border_router_init 一次，后一次必然 INVALID_STATE（见 backbone_follow_timer_cb）。 */
static volatile bool s_br_init_done = false;
/* 递归互斥：串行化 start / stop 与运行中换 backbone。
   没有它，stop 可能在换口任务拿到 OT 锁之前把 OT 拆掉 —— esp_openthread_deinit()
   会连 OT 锁本身一起删掉。递归是因为 start 的失败路径会调 stop。 */
static SemaphoreHandle_t s_life_lock;
static esp_err_t stop_impl(void);
static void backbone_follow_kick(void);
static void backbone_follow_timer_cb(void *arg);
static void backbone_follow_arm(void);
/* 上行变化后边界路由跟着换口的防抖定时器（见下方跟随一节）。thread_br_init 建一次，不删。 */
static esp_timer_handle_t s_follow_timer;

/* 上行口候选，按优先级。开机选口（thread_br_start）与运行中跟随（backbone_follow_timer_cb）
   都只用这一张表，两处的优先级永远一致：以太网优先于 WiFi，与 network_mgr 的默认路由策略
   一致（BOTH_ONLINE 时以太网保持默认路由）。
   句柄存在 ≠ 接口可用：wifi_mgr_init() 无条件创建 STA netif，出厂态它非空但从未 up；
   把它设成 backbone，OT 一发包就跳到地址 0 → 无限重启。所以必须逐个查 is_netif_up。 */
static const struct { const char *ifkey; const char *name; } BACKBONE_CANDIDATES[] = {
    { "ETH_DEF",      "Ethernet" },
    { "WIFI_STA_DEF", "WiFi STA" },   /* 名字别改：TC-07 B 的判据 grep 这个串 */
};

/* 句柄缓存：esp_netif_get_handle_from_ifkey() 要经 IPC 同步等 tcpip 线程（IDF
   esp_netif_lwip.c:909），而跟随定时器的回调跑在 esp_timer 任务里 —— 拔线那一刻 tcpip
   线程正忙，每次查都会拖慢全系统的定时器（2026-10-05 审计）。以太网 / STA 网口创建后
   全仓不销毁（唯一的 esp_netif_destroy 是 Thread 自己的 netif），找到一次就记住；
   还没建的（如没有以太网的构型）下次再找。指针写入是原子的，多任务同时填同一个值无害。 */
#define BACKBONE_N_CAND (sizeof(BACKBONE_CANDIDATES) / sizeof(BACKBONE_CANDIDATES[0]))
static esp_netif_t * volatile s_cand_netif[BACKBONE_N_CAND];

static esp_netif_t *pick_backbone(const char **name_out)
{
    for (size_t i = 0; i < BACKBONE_N_CAND; i++) {
        esp_netif_t *n = s_cand_netif[i];
        if (!n) n = s_cand_netif[i] = esp_netif_get_handle_from_ifkey(BACKBONE_CANDIDATES[i].ifkey);
        if (n && esp_netif_is_netif_up(n)) {
            if (name_out) *name_out = BACKBONE_CANDIDATES[i].name;
            return n;
        }
    }
    if (name_out) *name_out = NULL;
    return NULL;
}

/* OT mainloop task — creates netif+glue, then runs forever */
static void ot_main_task(void *arg)
{
    s_running = true;

    /* H1: OT netif + glue must be created in the same task as the mainloop */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_OPENTHREAD();
    s_ot_netif = esp_netif_new(&netif_cfg);
    if (!s_ot_netif) {
        ESP_LOGE(TAG, "Failed to create OT netif");
        s_running = false;
        vTaskDelete(NULL);
        return;
    }
    esp_netif_attach(s_ot_netif, esp_openthread_netif_glue_init(&s_ot_platform_cfg));

    ESP_LOGI(TAG, "OpenThread main loop starting");
    /* ⚠️ ot_main **不能**注册进 wd registry，别再试：登记表是任务自采栈余量
       （wd_kick 内调 uxTaskGetStackHighWaterMark(NULL)），而 launch_mainloop()
       正常运行时不返回，任务内没有任何可以 kick 的点，OT 侧也没有跑在本任务上的周期回调。
       唯一"绕过"办法是存 TaskHandle_t 由别人代采 —— 那正是 2026-08-11 整板重启的
       第二个真因，wd_registry.c:39-51 已经把它写成明令禁止（改成只存值之后，
       悬空句柄这个缺陷类在那里不可表达）。
       存活监控分两半：射频死了、协议栈还在（计数器不动）由 wd_probes.c 的 probe_thread() 盯；
       主循环自己退出由下面这段直接报给看门狗（退出后 s_running=false，探针看到的是"没在跑"、不判）。
       缺的只是栈余量可见性，要补只能给 OT 加一个跑在本任务上的周期回调。 */
    esp_err_t merr = esp_openthread_launch_mainloop();  /* 只在被叫停或 process 出错时返回 */
    if (!s_stopping) {
        /* 没人叫停却退了：Thread 从此停着，thread_br_stop() 见 !s_running 直接返回、不会 deinit，
           没有任何就地救法 —— 报给看门狗。不带 L2 手段，引擎按 Thread 的阶梯直接 L4 整机重启（T9.18 方案 A）。
           2026-10-07 实验 2 的第一种失效（dns_upstream_query 残留）就是这个样子，那时它完全静默。 */
        ESP_LOGE(TAG, "OpenThread 主循环意外退出（%s）—— Thread 已停止，交给看门狗", esp_err_to_name(merr));
        wd_report(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "thread", NULL);
    }

    esp_openthread_netif_glue_deinit();
    esp_netif_destroy(s_ot_netif);
    s_ot_netif = NULL;
    s_running = false;
    vTaskDelete(NULL);
}

/* BR init task — runs after mainloop starts, configures dataset + border routing */
/* ---- 边界路由起来后 5 分钟的 mDNS 刷新（2026-10-05 从 ot_br_init_task 拆出）----
 *
 * 做什么：每 30 s 重新宣告一次**我们自己**的 mDNS 服务（mdns_svc_refresh，2026-10-07 起
 * 不再碰 SRP 代播的 Matter 记录 —— 那会把全部代播服务重新探测一遍，设备一多就触发任务看门狗，
 * 理由见 mdns_svc_refresh 的注释。代播记录逐字节没变，重宣告对端只当 Updated，本来就帮不上 Matter 订阅恢复；
 * 出厂也没有替代机制 —— srp_readvertise.c 的 FORCE_BYE 默认关、只做跟踪，见它的头注）；
 * 顺带把库发布的 _meshcop._udp 的 vn/mn 改成本产品品牌。
 * 库要等 Border Agent listening（实测约 27 s）才发布，所以靠重试等它出现，而不是猜时间；
 * 每轮都重设一次：库在状态变化时会更新 TXT（sb/at/pt/sq 都是实时值），万一它整体重写
 * 就能被下一轮盖回来。
 *
 * ⚠️ 为什么单独一个常驻任务、栈放 PSRAM，而不是像以前那样留在 ot_br_init_task 里：
 * ot_br_init_task 的 4096 栈在**内部** SRAM（它调 border_router_init，库可能写 NVS，
 * 不能用 PSRAM 栈）。以前它做完初始化还要睡 10×30 s 才自删，于是开机 / Thread 重启后
 * 的前 5 分钟，DMA 池一直少 4.2 KB —— 2026-10-05 诊断追踪器（wd_heaptrace）量出来是
 * 拔线 failover 低谷的三块之一。本任务只调 mdns_svc_*（不碰 flash/NVS，psram_task.h
 * 规则 1），常驻不自删（规则 2），内部内存只剩一个 TCB。
 * 每次边界路由初始化完、每次换口成功后被 mdns_refresh_kick() 踢一次；刷新途中又被踢就从头数。 */
#define MDNS_REFRESH_ROUNDS   10
#define MDNS_REFRESH_GAP_MS   30000

static TaskHandle_t s_mdns_refresh_task;

static void mdns_refresh_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* 空闲时无限期等下一次 kick：这是"等活干"，不是等某个必然发生的事件 */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        bool branded = false;
        for (int i = 0; i < MDNS_REFRESH_ROUNDS; i++) {
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(MDNS_REFRESH_GAP_MS)) > 0) {
                i = -1;                 /* 等待中又被踢：Thread 重启过，从头再刷 10 轮 */
                branded = false;
                continue;
            }
            if (!s_running) break;      /* Thread 停了：这一批作罢，等下一次启动再踢 */
            mdns_svc_refresh();
            esp_err_t berr = mdns_svc_brand_meshcop();
            if (berr == ESP_OK && !branded) {
                branded = true;
                ESP_LOGI(TAG, "_meshcop TXT branded (vn/mn)");
            } else if (berr != ESP_OK && i == MDNS_REFRESH_ROUNDS - 1) {
                ESP_LOGW(TAG, "_meshcop 未能打上品牌（服务始终不存在）—— "
                              "HA 的 Thread 面板会显示 OpenThread/BorderRouter");
            }
        }
    }
}

/* "常驻、只建一次"的两个任务（mdns_refresh / zb_check）由开机初始化任务和换口跟随任务
   两处调用；正常时序下两者不会同时走到创建，但跟随只等 s_br_init_done、不等初始化任务
   跑完，所以认领放进临界区：谁先认领谁建，建失败就放回去，下次再试。 */
static portMUX_TYPE s_once_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_mdns_refresh_claimed;
static bool s_zb_check_claimed;

static bool once_claim(bool *claimed)
{
    bool mine = false;
    portENTER_CRITICAL(&s_once_mux);
    if (!*claimed) { *claimed = true; mine = true; }
    portEXIT_CRITICAL(&s_once_mux);
    return mine;
}

/* 每次边界路由初始化完（开机 / Thread 重启）和每次换口成功后调用：首次调用时建任务，
   之后只发通知让它从头刷 10 轮。换口后也刷：OT 在新口上重发 _meshcop 时 TXT 会被重写，
   要盖回品牌；我们自己的服务也在新口上再宣告一遍；上行晚于开机 5 分钟才出现的情形
   （开机那 10 轮已经刷完、_meshcop 还没发布）也靠这里补上品牌（2026-10-06，T9.7②）。 */
static void mdns_refresh_kick(void)
{
    if (!s_mdns_refresh_task) {
        if (!once_claim(&s_mdns_refresh_claimed)) return;   /* 另一处正在建，它会自己发通知 */
        if (psram_task_create(mdns_refresh_task, "mdns_refresh", 4096, NULL, 3,
                              &s_mdns_refresh_task) != pdPASS) {
            s_mdns_refresh_task = NULL;
            s_mdns_refresh_claimed = false;
            ESP_LOGW(TAG, "mdns_refresh 任务创建失败 —— 自有 mDNS 服务不会周期重宣告，"
                          "_meshcop 也不会打上品牌");
            return;
        }
    }
    xTaskNotifyGive(s_mdns_refresh_task);
}

/* zb_check 常驻、只建一次。开机那次边界路由起来了就在 ot_br_init_task 里建；开机时没有
   上行、边界路由后来才由跟随换口建起来的，在换口成功后补建 —— 此前这种情形永远不建
   （2026-10-06，T9.7③）。
   栈放 PSRAM：psram_task.h 四条规则逐条核过——不碰 flash/NVS
   （zigbee_check_once 只读 config_mgr 的 RAM 缓存 + active_scan）、不自删、优先级 3 不敏感。
   省 4096 内部 SRAM。⚠️ 将来若在 zigbee_check_once() 里加"把实测信道落 NVS"这类需求，
   必须改回 xTaskCreate，否则是 cache 关闭期访问 PSRAM 栈的随机崩溃。 */
static void zb_check_ensure(void)
{
    if (s_zb_check_task || !once_claim(&s_zb_check_claimed)) return;
    if (psram_task_create(zigbee_check_task, "zb_check", 4096, NULL, 3,
                          &s_zb_check_task) != pdPASS) {
        s_zb_check_task = NULL;
        s_zb_check_claimed = false;
        ESP_LOGW(TAG, "zb_check 任务创建失败 —— Zigbee 实际信道检测不可用");
    }
}

/* 骨干网卡的 IPv6 链路本地地址可用（DAD 做完、preferred）了再起边界路由（2026-10-22）。
   以太网的链路本地地址在 GOT_IP 时才建（net_events.c），DAD 要 1–2 s，而这里常在那之后几百毫秒就到：
   OT 的 RS、ESP BR 库的组播监听探测都以它为源，地址还是 TENTATIVE 时 lwIP 发不出去 —— 开机偶发的
   "Failed to probe backbone multicast listeners" / "Failed to send RS 1/3" 就是这个竞态。
   有上限：等不到也往下走（OT 的 RS 自己会隔 4 s 重试），只记一条 W。必须在拿 OT 锁之前等（持锁等会卡住主循环）。 */
#define BACKBONE_LL_WAIT_MS 3000

static void backbone_ll_wait(esp_netif_t *nif, const char *who)
{
    if (!nif) return;
    esp_ip6_addr_t ll;
    for (int waited = 0; waited <= BACKBONE_LL_WAIT_MS; waited += 100) {
        if (esp_netif_get_ip6_linklocal(nif, &ll) == ESP_OK) {
            if (waited) ESP_LOGI(TAG, "%s: backbone IPv6 link-local ready after %d ms", who, waited);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGW(TAG, "%s: backbone IPv6 link-local still not preferred after %d ms — starting border routing anyway "
                  "(OT retries RS every 4 s)", who, BACKBONE_LL_WAIT_MS);
}

static void ot_br_init_task(void *arg)
{
    /* Wait for mainloop to be running */
    vTaskDelay(pdMS_TO_TICKS(500));

    if (s_backbone_ready) backbone_ll_wait(s_backbone_netif, "ot_br_init");

    const thread_config_t *cfg = config_mgr_get_thread();

    esp_openthread_lock_acquire(portMAX_DELAY);

    otInstance *instance = esp_openthread_get_instance();

    /* Only create new dataset if none exists (persist across reboots) */
    otOperationalDatasetTlvs existing;
    if (otDatasetGetActiveTlvs(instance, &existing) != OT_ERROR_NONE ||
        existing.mLength == 0) {
        otOperationalDataset dataset;
        otDatasetCreateNewNetwork(instance, &dataset);

        /* 首次建网是我们唯一能自己定信道的时刻 —— dataset 一旦存进 OT 的 NVS
           就归它管，之后只有 HA 推数据集能改。所以这里必须躲开 Zigbee。
           Zigbee 信道用的是**用户声明值**（config_mgr.h 里有为什么读不到实测值
           的完整说明）。用户没声明就退回配置的默认信道，不擅自猜。 */
        const zigbee_config_t *zb = config_mgr_get_zigbee();
        bool zb_known = zb && zb->channel_declared &&
                        zb->channel >= 11 && zb->channel <= 26;
        uint8_t channel = thread_br_pick_channel(zb_known ? zb->channel : 0,
                                                 zb_known, cfg->channel);

        dataset.mChannel = channel;
        dataset.mComponents.mIsChannelPresent = true;
        otDatasetSetActive(instance, &dataset);
        had_child_store(false);     /* 新网络，重新开始计 */
        ESP_LOGI(TAG, "Created new Thread dataset, channel=%d (配置值 %d, "
                      "Zigbee %s)", channel, cfg->channel,
                 zb_known ? "已声明" : "未声明");
    } else {
        /* dataset 已存在。产线首烧那条路径上，它是在"MG24 还没建网、没东西可躲"
           的时候建的，信道只能是配置值；用户之后配 Z2M 才定下 Zigbee 信道，
           可能正好撞上。这里是每次开机唯一能干净读到 NCP 的时刻（TCP server
           还没接客），所以补一次判断。
           只在网络从未接入过设备时才动 —— 迁移空网络代价为零；有设备的网络
           要走 pending dataset，是另一件事。 */
        bool had_child = had_child_load();
        uint8_t cur = otLinkGetChannel(instance);
        uint8_t nch = 0;

        if (thread_br_should_repick(cur, had_child, &nch)) {
            otOperationalDataset ds;
            if (otDatasetGetActive(instance, &ds) == OT_ERROR_NONE) {
                /* 只改信道，network key / PAN ID / ExtPanId 全部保留 ——
                   网络身份不变，HA 那边存过的凭据仍然指向同一个网络。
                   时间戳要递增，否则别处存的旧副本会被认为更新。
                   此刻 Thread 尚未 enable，可以直接设 active dataset。 */
                ds.mChannel = nch;
                ds.mComponents.mIsChannelPresent = true;
                ds.mActiveTimestamp.mSeconds++;
                ds.mActiveTimestamp.mTicks = 0;
                ds.mActiveTimestamp.mAuthoritative = false;
                ds.mComponents.mIsActiveTimestampPresent = true;
                otDatasetSetActive(instance, &ds);
                ESP_LOGW(TAG, "Thread 信道 %u → %u：与 Zigbee 靠得太近，且本网络"
                              "从未接入过设备，直接改信道（网络身份不变）",
                         cur, nch);
            } else {
                ESP_LOGW(TAG, "想把 Thread 从 %u 挪开却读不到 active dataset，维持原样", cur);
            }
        } else {
            ESP_LOGI(TAG, "Using existing Thread dataset from NVS (ch%u%s)",
                     cur, had_child ? "，已接入过设备，不自动改信道" : "");
        }
    }

    /* Enable IPv6 + Thread */
    otIp6SetEnabled(instance, true);
    otThreadSetEnabled(instance, true);

    /* Init border routing — 只在 backbone 真的设上了才有意义。没设就别调，
       否则拿到的必然是 ESP_ERR_INVALID_STATE("Backbone netif not set")，
       徒增一条 E 级日志误导排查。 */
    if (s_backbone_ready) {
        esp_err_t err = esp_openthread_border_router_init();
        s_border_routing = (err == ESP_OK);
        if (s_border_routing) {
            ESP_LOGI(TAG, "Border routing initialized");
        } else {
            ESP_LOGE(TAG, "Border router init failed: %s", esp_err_to_name(err));
        }
    } else {
        s_border_routing = false;
        ESP_LOGW(TAG, "Skipping border routing init — no backbone netif");
    }
    s_br_init_done = true;   /* 之后的 backbone 变化归跟随定时器管 */
    /* 从开机选口到这里，上行可能已经变了（那期间的事件被跟随定时器放弃了），补看一次。
       通常是同一个口，定时器回调直接返回，不建任务。 */
    backbone_follow_kick();

    /* Tune MAC frame retries for WiFi coexistence:
       - Direct retries: 15→3. Default 15 means a CCA-failed frame blocks the queue
         for 16 attempts (~5s). With 3, failed frames are dropped in ~1s, preventing
         "dir queue full" buildup that kills Thread communication.
       - Indirect retries: 0→3. Default 0 means messages to sleeping children are
         never retried. With 3, the BR has 4 chances per frame. */
    otLinkSetMaxFrameRetriesDirect(instance, 3);
    otLinkSetMaxFrameRetriesIndirect(instance, 3);
    ESP_LOGI(TAG, "MAC retries: direct=3, indirect=3");

    esp_openthread_lock_release();

    /* _meshcop._udp 整条记录（发布、端口、xa/nn/xp 等 TXT）都归 BR 库（esp_openthread_publish_meshcop_mdns），
       入网后它才发布，之后数据集 / 网络名 / 扩展 PAN ID 一变就重发；我们只在它发布后覆盖 vn/mn 品牌
       （mdns_refresh_kick → mdns_svc_brand_meshcop）。这里以前还自己写一遍 xa/nn/xp：此刻服务还没发布，
       必然失败，每次开机三条 E 级 "Service doesn't exist"（2026-10-22 删，见 mdns_svc.h）。 */

    /* 别谎报成功：边界路由没起来时 Thread 网络本身能跑，但它不是 border router，
       外部网络到 Thread 设备的路由是不通的。以前这里无条件打
       "Thread BR fully started"，把上面那条 E 级失败盖过去了，监控和自测都会被骗。 */
    if (s_border_routing) {
        zb_check_ensure();   /* 常驻、只建一次（Thread 重启不再多建），见函数注释 */

    ESP_LOGI(TAG, "Thread BR fully started (border routing active)");
    } else {
        ESP_LOGW(TAG, "Thread network started WITHOUT border routing — "
                      "Thread devices are not reachable from the uplink");
    }

    /* 开机后 5 分钟的 mDNS 刷新交给常驻的 mdns_refresh 任务（栈在 PSRAM），本任务
       现在就退出 —— 别把那个循环搬回来，理由见 mdns_refresh_task 上方的注释。 */
    mdns_refresh_kick();

    vTaskDelete(NULL);
}

/* 802.15.4 发射功率（T1.4，2026-09-11）。
 *
 * 此前这颗射频的功率**从来没人设过，也没人看过**：IDF 的 PIB 初始化把 16 个信道
 * 全填成芯片功率表的末项（esp_ieee802154_pib.c:70 的 IEEE802154_TXPOWER_VALUE_MAX），
 * 而 OpenThread 在正常路径上根本不调 otPlatRadioSetTransmitPower —— 全 OT 源码里
 * 只有 factory_diags.cpp 调它。也就是说 Thread 一直在满功率发射，是默认，不是选择。
 *
 * 认证按"设备能发的最大功率"测，所以这是 CE/FCC 唯一要动的那个旋钮；
 * Zigbee 那颗不在这里（MG24 的功率由 Z2M/ZHA 经 EZSP 写，网关读不到也管不了）。
 *
 * 无条件打这行日志：关着开关时它打印的就是本平台功率表的上限，而那个数字在别处
 * 没有任何出口（get_txpower 回显的是 PIB 里的请求值，覆盖之后就再也问不出来了）。 */
/* 2026-09-15（docs/14 第 7.5 节）：功率现在按网关声明的地区取，编译期开关只剩"实验室强制覆盖"。
   这四个数是 Thread 这颗射频的口径，Zigbee 那颗的表在 MG24 固件里（mg24/ncp/tx_power_table.h），
   两边同一等级的数可以不同（Zigbee FCC 19 是 PA 档位，这里 20 是 C5 功率表末项）。 */
#define THREAD_TXP_CE_DBM       10
#define THREAD_TXP_FCC_DBM      20
#define THREAD_TXP_UNKNOWN_DBM   8
#define THREAD_TXP_UNSET_DBM    10   /* 没设地区 = 欧盟口径，哪里都合法（docs/14 Q5） */

static int8_t s_tx_power_dbm = INT8_MAX;   /* 最近一次成功请求的值；INT8_MAX = 还没设过 */

int8_t thread_br_region_tx_dbm(const char *cc)
{
    switch (radio_region_class_of(cc)) {
    case RADIO_REGION_CLASS_CE:      return THREAD_TXP_CE_DBM;
    case RADIO_REGION_CLASS_FCC:     return THREAD_TXP_FCC_DBM;
    case RADIO_REGION_CLASS_UNKNOWN: return THREAD_TXP_UNKNOWN_DBM;
    case RADIO_REGION_CLASS_NONE:
    default:                         return THREAD_TXP_UNSET_DBM;
    }
}

int8_t thread_br_get_tx_power_dbm(void)
{
    return s_tx_power_dbm;
}

static void apply_tx_power(void)
{
    ESP_LOGI(TAG, "802.15.4 TX power: %d dBm (radio default = chip power-table max)",
             esp_ieee802154_get_txpower());

#ifdef CONFIG_BITENGINE_THREAD_TX_POWER_OVERRIDE
    int8_t want = (int8_t)CONFIG_BITENGINE_THREAD_TX_POWER_DBM;
    const char *why = "Kconfig override";
#else
    const sys_config_t *sys = config_mgr_get_sys();
    const char *cc = (sys && sys->radio_region[0]) ? sys->radio_region : "";
    int8_t want = thread_br_region_tx_dbm(cc);
    const char *why = cc[0] ? cc : "region not set";
#endif
    esp_err_t perr = esp_ieee802154_set_txpower(want);
    if (perr != ESP_OK) {
        /* 非致命：Thread 照常工作，只是功率还是默认的满功率。但认证口径会不成立，
           所以要吼出来，别让它混在 INFO 里。 */
        ESP_LOGE(TAG, "802.15.4 TX power clamp to %d dBm FAILED (%s) — "
                      "radio still at maximum", want, esp_err_to_name(perr));
    } else {
        s_tx_power_dbm = want;
        /* 驱动向下吸附到功率表最近一档，回读拿不到吸附后的值，所以这里只能说
           "请求了什么"，不能说"实际发多少"。别把这行当成实测证据。 */
        ESP_LOGW(TAG, "802.15.4 TX power clamped: requested %d dBm [%s] "
                      "(driver snaps down to the nearest table step)", want, why);
    }
}

static esp_err_t start_impl(void)
{
    if (s_running) return ESP_OK;

    const thread_config_t *cfg = config_mgr_get_thread();

    /* 1. Register eventfd VFS (required by OpenThread for task queue + discovery delegate) */
    esp_vfs_eventfd_config_t eventfd_cfg = { .max_fds = 5 };
    esp_err_t err = esp_vfs_eventfd_register(&eventfd_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "eventfd register failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 2. Set backbone netif BEFORE esp_openthread_init (API requirement)

       挑"当前真正在用的上行"，优先级必须和 network_mgr 的默认路由策略一致：
       ETH 优先于 WiFi（见 network_mgr.c 的 BOTH_ONLINE 分支，WiFi 起来时
       ETH 仍保持默认路由）。曾经只找 WIFI_STA_DEF，于是有线部署下 backbone
       永远设不上，esp_openthread_border_router_init() 报 "Backbone netif not set"，
       等于 PoE 有线网关根本没有 Thread 边界路由——而有线是本产品主部署形态。

       句柄存在 ≠ 接口可用。wifi_mgr_init() 无条件创建 STA netif，所以出厂态
       （无凭据 → 纯 SoftAP，wifi_mgr_start_sta() 从不调用）下这个句柄非空，
       但接口从未 up、lwIP 的 netif->output_ip6 还是 NULL。把它设成 backbone，
       OpenThread 一发布 DNS/SRP 就往上面发 IPv6 包：
           ip6_output_if_src (lwip/core/ipv6/ip6.c:1296) → netif->output_ip6 == NULL
           → 跳转到地址 0 → Instruction access fault → 复位 → 无限重启
       实测出厂固件每 ~10s 崩一次，开箱即砖。所以每个候选都必须查接口是否真的 up。 */
    const char *backbone_name = NULL;
    esp_netif_t *backbone = pick_backbone(&backbone_name);

    s_backbone_ready = (backbone != NULL);
    s_backbone_netif = backbone;
    if (backbone) {
        esp_openthread_set_backbone_netif(backbone);
        ESP_LOGI(TAG, "Backbone netif set to %s", backbone_name);
    } else {
        /* 无上游时不设 backbone：此时边界路由本来也无意义。上游后来出现时，
           ot_br_init_task 做完会踢一次跟随定时器，NETWORK_EVENT_ONLINE 也会踢，
           由 backbone_follow_timer_cb 在运行中设 backbone 并 border_router_init
           （2026-10-05 起；此前确实只能等下次重启）。
           IDF 头文件说 set_backbone_netif 要在 esp_openthread_init 之前 —— 测试 6c 证明运行中换口可行。
           实际开机走不到这里（T9.9，2026-10-08 两次实测）：有 WiFi 凭据但连不上时 main 先等上线才
           thread_br_init；没凭据开热点时本文件把启动延后到上线（"deferred until the gateway is online"）。
           两次都是上行出现后按正常开机建边界路由。只剩"刚上线又掉线"的竞态会进来，没专门测。 */
        ESP_LOGW(TAG, "No usable uplink netif (ETH_DEF / WIFI_STA_DEF both absent "
                      "or down) — Thread border routing deferred until an uplink comes up");
    }

    /* 3. Platform config */
    s_ot_platform_cfg = (esp_openthread_platform_config_t){
        .radio_config = { .radio_mode = RADIO_MODE_NATIVE },
        .host_config = { .host_connection_mode = HOST_CONNECTION_MODE_NONE },
        .port_config = {
            .storage_partition_name = "nvs",
            /* 48（原 20）：lwIP→OT 方向的发送队列。满了在
               esp_openthread_netif_glue.c 的 xQueueSend(...,0) 直接丢包，
               要开 OT_PLAT 标签才看得到 WARN —— 默认日志级别下是静默的。
               20 是按个位数设备定的，50 台批量归队/组播风暴会打满。
               每槽 80 字节，涨到 48 只多 ~2.2KB。 */
            .netif_queue_size = 48,
            .task_queue_size = 20,
        },
    };

    /* 4. Init OpenThread platform */
    err = esp_openthread_init(&s_ot_platform_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OpenThread init failed: %s (Thread BR degraded)", esp_err_to_name(err));
        return ESP_OK;  /* don't block other modules */
    }

    apply_tx_power();

#if CONFIG_OPENTHREAD_LOG_LEVEL_DYNAMIC
    /* OT 的运行时日志级别。IDF 自己的启动封装在 esp_openthread_init() 之后会设一次
       （esp_openthread.cpp ot_task_worker：otLoggingSetLevel(CONFIG_LOG_DEFAULT_LEVEL)）；我们是自己调
       esp_openthread_init 的，以前漏了这一步，OT 就停在编译期上限 DEBG：DEBG 级先在 OT 里 vsnprintf 一遍
       再被 esp_log 丢掉，INFO 级以 115200 波特忙等写进本板没引出的 UART0。
       2026-10-07 实测：每条 SRP 注册多打 22 行 INFO —— 网关重启后设备 10 s 内成批重注册，128 台就把 IDLE 饿 5 s、
       任务看门狗 panic（数据在 docs/test_cases_scale_baseline.md「SRP 代播满载压测」第 2 件）。这些行本来也进不了
       /api/v1/logs（log_store 只留 OPENTHREAD 的 W/E），只在 USB 串口监视里看得到；要看就单独编一版把
       CONFIG_BITENGINE_OT_LOG_LEVEL 调到 4（INFO）。此时主循环还没起，不用持 OT 锁（同下面 CLI 那段的 H1 第 1 条）。
     *
     * ESP 的 "OPENTHREAD" 标签要一起压：那 22 行里有 6 行是 IDF 的代播库（libopenthread_br.a 的
       esp_openthread_srp_server.c）用这个标签直接 ESP_LOGI 的（update host / Add a new host / Service / Add new
       service / Update subtype / Host update），不受上面 OT 级别管；OT 的 NOTE 也是经这个标签按 INFO 输出的。
       所以两个级别跟着同一个 Kconfig 走（映射见 ot_log_level.h）。 */
    otError lerr = otLoggingSetLevel((otLogLevel)CONFIG_BITENGINE_OT_LOG_LEVEL);
    if (lerr != OT_ERROR_NONE) ESP_LOGW(TAG, "otLoggingSetLevel(%d) failed: %d", CONFIG_BITENGINE_OT_LOG_LEVEL, lerr);
    /* ⚠️ USB 桥模式下不设：那时副控制台承载 EZSP 字节流，USB 桥把 "*" 设成了 NONE；按标签设的级别优先于
       默认值，这里一设，OT 的告警就混进 EZSP 流打坏 ASH 帧。退出 USB 模式时由 usb_bridge_stop() 设回去。
       （与运行中切到 USB 模式之间有个窄窗口不互斥：切模式是人工动作，接受。） */
    if (ezsp_bridge_get_mode() != BRIDGE_MODE_USB) {
        esp_log_level_set("OPENTHREAD", BITENGINE_OT_ESP_LOG_LEVEL);
    }
#endif

#if CONFIG_BITENGINE_ENABLE_OT_CLI
    /* 射频实验室用：把 OT CLI 接到 USB 控制台，才够得着 `diag` 那组命令
       （CONFIG_OPENTHREAD_DIAG 一直是 y，但 CLI 从未被初始化，所以没有输入口）。
       出厂 default n，理由见 Kconfig.projbuild 的那段 help（泄密 / 4KB 内部
       SRAM / 抢 stdin，三条）。

       H1 —— IDF v6.0.2 的 esp_openthread_cli.c 逐行核过，四件事写在这里，
       别凭记忆改：
         1. esp_openthread_cli_init() 就是 otCliInit(esp_openthread_get_instance(),
            ...)，所以**必须在 esp_openthread_init() 之后**；而下面那句
            xTaskCreate(ot_main_task) 才会 launch_mainloop()，此刻协议栈还没跑起来，
            **因此不需要持 esp_openthread_lock**。放到建任务之后就是竞态。
         2. esp_openthread_cli_create_task() 内部是 xTaskCreate(ot_cli_loop,
            "ot_cli", 4096, ...) —— 栈大小写死在 IDF 里，落内部 SRAM，我们既
            不能传参也不能迁 PSRAM。
         3. **IDF 没有提供 deinit / delete**（esp_openthread_cli.h 里只有 init /
            create_task / input / console_command_[un]register）。而本函数在 stop 之后
            可能被再次调用（失败路径、配网结束后的延迟启动）—— 所以下面这个一次性
            守卫是必需的，不是保险：少了它，Thread 每重启一次就多一个 ot_cli
            任务，两个 linenoise 抢同一个 stdin。
         4. ot_cli_loop 会 esp_console_init() + linenoise 接管标准输入。 */
    static bool s_cli_started = false;
    if (!s_cli_started) {
        esp_openthread_cli_init();
        esp_openthread_cli_create_task();
        s_cli_started = true;
        ESP_LOGW(TAG, "OpenThread CLI attached to console (diag build — never ship this)");
    }
#endif

    /* Enable WiFi + 802.15.4 coexistence per ESP-IDF ot_br example.
       Doc says WiFi STA Connected + Thread Router = Y (stable).
       Requires: PMF off + PS_NONE + BLE proxy off for stability. */
    /* 纯以太网剖面没有 Wi-Fi，不开共存（docs/15 设计 2.2）：开了就会把 Wi-Fi 库链接进来 */
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE && CONFIG_SOC_IEEE802154_SUPPORTED && !CONFIG_BITENGINE_NET_ETH_ONLY
    err = esp_coex_wifi_i154_enable();
    ESP_LOGI(TAG, "WiFi+802.15.4 coex: %s", err == ESP_OK ? "enabled" : esp_err_to_name(err));
#endif

    /* 6. Start mainloop task (creates OT netif + glue internally) */
    s_stopping = false;
    if (xTaskCreate(ot_main_task, "ot_main", 8192, NULL, 16, &s_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "OT mainloop task create failed");
        esp_openthread_deinit();
        return ESP_ERR_NO_MEM;
    }

    /* 7. Start BR init task (dataset + enable + border routing) */
    if (xTaskCreate(ot_br_init_task, "ot_br_init", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "OT BR init task create failed");
        stop_impl();
        return ESP_ERR_NO_MEM;
    }

    /* 8. 子节点恢复进度定时器（喂 LED 与 WebUI 的"重连中 x/y"） */
    if (!s_child_stats_timer) {
        const esp_timer_create_args_t targs = {
            .callback = child_stats_timer_cb,
            .name     = "child_stats",
        };
        if (esp_timer_create(&targs, &s_child_stats_timer) != ESP_OK) {
            /* 非致命：只是没有进度指示，Thread 本身照常工作 */
            ESP_LOGW(TAG, "child stats timer create failed — no recovery indication");
            s_child_stats_timer = NULL;
        }
    }
    if (s_child_stats_timer && !esp_timer_is_active(s_child_stats_timer)) {
        esp_timer_start_periodic(s_child_stats_timer, 2000 * 1000);  /* 2s */
    }

    /* 9. SRP 代播服务重新宣告（Matter 订阅恢复，见 srp_readvertise.c） */
    srp_readv_start();

    ESP_LOGI(TAG, "Thread BR starting, channel=%d", cfg->channel);
    return ESP_OK;
}

/* ⚠️ 拆边界路由一律走这里，别直接调 esp_openthread_border_router_deinit()。
 *
 * IDF v6.0.2 的主循环工作流表注销不掉名字 ≥16 字节的条目：注册时按 15 字节截断存
 * （esp_openthread_platform.cpp register 用 strnlen(name, WORKFLOW_MAX_NAMELEN - 1)），
 * 注销时却按 16 字节比（unregister 用 strnlen(name, WORKFLOW_MAX_NAMELEN)），永远对不上。
 * 全 OT 栈只有一个这么长的：边界路由库（预编译 libopenthread_br.a）里 DNS 上游转发的
 * "dns_upstream_query"，表里存成 "dns_upstream_qu"。它的 deinit 释放 g_resolver、注销落空，
 * 表里留下一个 process 恒返回 ESP_ERR_INVALID_STATE 的条目（反汇编核过）；而
 * esp_openthread_launch_mainloop() 遇到 process 失败就 break 退出。
 *
 * 2026-10-07 实验 2（T9.18）实测后果：看门狗 L3 的 stop → start 之后，新主循环第一圈就撞上
 * 这个条目退出 —— 那时新一轮的 border_router_init 还没跑，g_resolver 是空的。Thread 从此停着，
 * start 却返回 ESP_OK、日志照打 "fully started"。08-09 那次"往返失败"很可能是同一个根因
 * （主循环先静默退出，再 start 就是 "platform already initialized"）。
 * 运行中换口（do_backbone_switch）在同一把锁里 deinit + init，主循环看不到中间态，所以 6c 没撞上；
 * 但 init 一失败，主循环下一圈同样会退出。
 *
 * 修法：deinit 之后用表里**存下来的截断名**再注销一次，能对上。必须持 OT 锁（主循环在锁内遍历
 * 这张表）。下一次 border_router_init 会按原名重新注册一条新的。IDF master（2026-03 快照）同样未修。 */
#define OT_DNS_UPSTREAM_WORKFLOW_STORED_NAME "dns_upstream_qu"
/* 声明在 IDF openthread 组件的 private_include/esp_openthread_platform.h（extern "C"），不对外导出 */
extern void esp_openthread_platform_workflow_unregister(const char *name);

static esp_err_t br_deinit_locked(void)
{
    esp_err_t err = esp_openthread_border_router_deinit();
    esp_openthread_platform_workflow_unregister(OT_DNS_UPSTREAM_WORKFLOW_STORED_NAME);
    return err;
}

static esp_err_t stop_impl(void)
{
    /* 先收回重新宣告任务：它同样会取 OT 锁（会等到它离开 OT 区间才返回） */
    srp_readv_stop();

    /* 先停进度定时器：它会取 OT 锁，而下面正要把 OT 拆掉 */
    if (s_child_stats_timer && esp_timer_is_active(s_child_stats_timer)) {
        esp_timer_stop(s_child_stats_timer);
    }
    /* 跟随定时器回调看到 !s_running 本来也会直接返回；停掉只是不让它白跑一次 */
    if (s_follow_timer) esp_timer_stop(s_follow_timer);
    led_ctrl_set_thread_health(false, false, false);   /* BR 要停了，灯灭 */

    if (!s_running) return ESP_OK;

    s_stopping = true;   /* 从这里起主循环的退出都是我们要的，ot_main_task 别报"意外退出" */
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *instance = esp_openthread_get_instance();
    if (instance) {
        otThreadSetEnabled(instance, false);
        otIp6SetEnabled(instance, false);
    }
    br_deinit_locked();
    esp_openthread_lock_release();

    esp_openthread_mainloop_exit();
    for (int i = 0; i < 50 && s_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* ot_main_task deletes ITSELF after the mainloop exits (it sets
       s_running=false right before vTaskDelete). Calling vTaskDelete on its
       handle here would act on a freed TCB. Only force-delete if the task
       provably never exited the mainloop. */
    if (s_running && s_task_handle) {
        ESP_LOGE(TAG, "OT mainloop did not exit within 5s — force deleting");
        vTaskDelete(s_task_handle);
        s_running = false;
    }
    s_task_handle = NULL;
    s_backbone_netif = NULL;
    s_br_init_done = false;

    esp_openthread_deinit();
    ESP_LOGI(TAG, "Thread BR stopped");
    return ESP_OK;
}

/* 锁没建（thread_br_init 之前）就不加锁：那时也不会有别的任务来抢。30 s 足够覆盖 stop
   里最长 5 s 的 mainloop 退出等待，拿不到就失败返回，不无限等。 */
static bool life_take(void)
{
    return !s_life_lock || xSemaphoreTakeRecursive(s_life_lock, pdMS_TO_TICKS(30000)) == pdTRUE;
}

static void life_give(void)
{
    if (s_life_lock) xSemaphoreGiveRecursive(s_life_lock);
}

esp_err_t thread_br_start(void)
{
    if (!life_take()) {
        ESP_LOGE(TAG, "thread_br_start: 等生命周期锁超时（30 s）");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = start_impl();
    life_give();
    return err;
}

esp_err_t thread_br_stop(void)
{
    if (!life_take()) {
        ESP_LOGE(TAG, "thread_br_stop: 等生命周期锁超时（30 s）");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = stop_impl();
    life_give();
    return err;
}

/* ---- SoftAP 配网 ↔ 802.15.4 射频仲裁 ------------------------------------
 *
 * IDF `docs/en/api-guides/coexist.rst` 的共存矩阵把
 * `Wi-Fi SOFTAP / Connecting` × `Thread Router` 标为 **X（不支持）**。
 * 实测印证（2026-08-08，每组 10 次连接，判据为设备侧 `wifi:station: … join`）：
 *
 *     官方 softAP 例程 @5G ch36 ............ 10/10   AP 信号 95
 *     本固件，Thread BR 运行中 ............. 0/10    AP 信号 67
 *     本固件，跳过 thread_br_init() ........ 10/10   AP 信号 95（DHCP 也 10/10）
 *
 * 即：出厂设备只要 Thread 起来，SoftAP 配网就一次都成不了 —— 客户端 auth
 * 三次重试全超时，AP 侧 15s 后 GC 回收。
 *
 * 所以 SoftAP 在跑的时候不碰 802.15.4：开机若已处于配网态就推迟启动；运行中
 * 掉线回落到 SoftAP 就暂停射频；SoftAP 一停（意味着已有上行）再启动或恢复。
 * 无上行的边界路由本来也没有意义，推迟不损失功能。
 *
 * 附带修好一处旧限制：推迟到上行就绪后才 esp_openthread_init()，此时
 * backbone netif 真的是 up 的，边界路由不再"要重启一次才生效"（见 start()
 * 第 2 步注释）。 */

static volatile bool s_deferred_start_pending = false;

static void ot_deferred_start_task(void *arg)
{
    ESP_LOGI(TAG, "Provisioning finished — starting Thread BR now");
    thread_br_start();
    /* 启动窗口里上行可能又断了、SoftAP 重新拉起 —— 那次 SOFTAP_STARTED 事件
       看到的 s_running 还是 false，暂停不了。这里补一次检查，否则会留下
       "Thread 与 SoftAP 同时在跑" 这个正好配不上网的状态。 */
    if (s_running && network_mgr_softap_active()) {
        ESP_LOGW(TAG, "SoftAP came back during Thread start — pausing radio");
        thread_br_pause();
    }
    s_deferred_start_pending = false;
    vTaskDelete(NULL);
}

/* 运行在默认事件循环任务里，必须快速返回：thread_br_start() 会做
   esp_openthread_init() 并建任务，耗时不可控，扔到一次性任务里做。 */
static void net_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    /* 上行变了：边界路由的 backbone 原来只在开机时选一次，以太网一断就停在死口上
       （测试 6c）。这里只重置跟随定时器，判断在定时器回调里、切换在它建的任务里。 */
    if (id == NETWORK_EVENT_FAILOVER || id == NETWORK_EVENT_RECOVERED ||
        id == NETWORK_EVENT_ONLINE) {
        if (s_running) backbone_follow_kick();
        return;
    }
    if (id == NETWORK_EVENT_SOFTAP_STARTED) {
        if (s_running) {
            ESP_LOGW(TAG, "SoftAP provisioning started — pausing Thread radio "
                          "(SoftAP + 802.15.4 cannot coexist)");
            thread_br_pause();
        }
    } else if (id == NETWORK_EVENT_SOFTAP_STOPPED) {
        if (s_running) {
            thread_br_resume();
            backbone_follow_kick();   /* 配网期间上行可能换了口 */
        } else if (!s_deferred_start_pending) {
            s_deferred_start_pending = true;
            if (xTaskCreate(ot_deferred_start_task, "ot_defer_start",
                            4096, NULL, 5, NULL) != pdPASS) {
                s_deferred_start_pending = false;
                ESP_LOGE(TAG, "Deferred Thread BR start task create failed — "
                              "Thread stays down until reboot");
            }
        }
    }
}

/* 地区改了就立刻重设功率：esp_ieee802154_set_txpower() 只写 PIB 表（IDF esp_ieee802154_pib.c），
   任何任务里调都行，不用重启 Thread。射频没起来（s_running=false）时不动，起来时 start 会设。 */
static void cfg_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == CONFIG_EVENT_RADIO_REGION_CHANGED && s_running) apply_tx_power();
}

esp_err_t thread_br_init(void)
{
    thread_br_cpu_guard_init();     /* OT 任务过载时让出 CPU（ot_cpu_guard.c），越早装越好、只装一次 */
    if (!s_life_lock && !(s_life_lock = xSemaphoreCreateRecursiveMutex())) {
        /* 不致命：life_take() 在锁不存在时直接放行，只是失去与运行中换口的互斥 */
        ESP_LOGE(TAG, "生命周期锁创建失败 —— start/stop 与换口不再互斥");
    }
    if (!s_follow_timer) {
        const esp_timer_create_args_t fargs = {
            .callback = backbone_follow_timer_cb,
            .name     = "ot_bb_follow",
        };
        if (esp_timer_create(&fargs, &s_follow_timer) != ESP_OK) {
            /* 不致命：Thread 照常工作，只是上行换口后边界路由不会自动跟过去（测试 6c 的老问题） */
            ESP_LOGE(TAG, "边界路由跟随定时器创建失败 —— 拔网线后 Matter 设备可能一直不通，直到重启");
            s_follow_timer = NULL;
        }
    }

    /* 不登记 L3 恢复入口：Thread 的阶梯是 L2（射频重启）→ L4（整机重启），没有 L3。
       整栈重启（stop → start）正常态实测不可用，见 wd_recovery.c 的 s_subsys_override（T9.18）。 */

    const thread_config_t *cfg = config_mgr_get_thread();
    if (!cfg->enabled) {
        ESP_LOGI(TAG, "Thread BR disabled by config");
        return ESP_OK;
    }

    if (esp_event_handler_register(CONFIG_EVENT, CONFIG_EVENT_RADIO_REGION_CHANGED,
                                   cfg_event_handler, NULL) != ESP_OK) {
        /* 非致命：地区改动要重启才作用到 Thread。Zigbee 那颗不经这里。 */
        ESP_LOGW(TAG, "CONFIG_EVENT subscribe failed — region change applies to Thread after reboot");
    }

    esp_err_t err = esp_event_handler_register(NETWORK_EVENT, ESP_EVENT_ANY_ID,
                                               net_event_handler, NULL);
    if (err != ESP_OK) {
        /* 没有事件订阅就没法在配网结束后自动起来，宁可立刻启动也不要静默失联 */
        ESP_LOGE(TAG, "Network event subscribe failed: %s — starting Thread now",
                 esp_err_to_name(err));
        return thread_br_start();
    }

    if (network_mgr_softap_active()) {
        ESP_LOGW(TAG, "SoftAP provisioning active — Thread BR deferred until "
                      "the gateway is online");
        return ESP_OK;
    }

    return thread_br_start();
}

/* Declared in thread_radio_mgr.c */
extern bool thread_br_radio_is_paused(void);

bool thread_br_is_running(void) { return s_running && !thread_br_radio_is_paused(); }

esp_err_t thread_br_srp_info(uint16_t *port, int *hosts, int *services)
{
    if (!s_running) return ESP_ERR_INVALID_STATE;
    /* 200ms 与本文件其他读路径一致：拿不到锁宁可回失败也不拖住 httpd
       （webui 是单任务，堵住就是整个 Web 界面无响应）。 */
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(200))) return ESP_ERR_TIMEOUT;

    otInstance *inst = esp_openthread_get_instance();
    if (!inst) { esp_openthread_lock_release(); return ESP_ERR_INVALID_STATE; }

    if (port) *port = otSrpServerGetPort(inst);

    int h = 0, sv = 0;
    const otSrpServerHost *host = NULL;
    while ((host = otSrpServerGetNextHost(inst, host)) != NULL) {
        if (otSrpServerHostIsDeleted(host)) continue;   /* 墓碑条目不算 */
        h++;
        const otSrpServerService *svc = NULL;
        while ((svc = otSrpServerHostGetNextService(host, svc)) != NULL) {
            if (!otSrpServerServiceIsDeleted(svc)) sv++;
        }
    }
    esp_openthread_lock_release();

    if (hosts) *hosts = h;
    if (services) *services = sv;
    return ESP_OK;
}

void thread_br_lower_priority(void)
{
    if (s_task_handle) {
        vTaskPrioritySet(s_task_handle, 2);
        ESP_LOGI(TAG, "OT mainloop priority lowered for OTA");
    }
}

uint8_t thread_br_get_configured_channel(void)
{
    return config_mgr_get_thread()->channel;
}

esp_err_t thread_br_get_active_channel(uint8_t *out_channel)
{
    if (!out_channel) return ESP_ERR_INVALID_ARG;
    if (!thread_br_is_running()) return ESP_ERR_INVALID_STATE;

    /* 带超时取锁，绝不用 portMAX_DELAY：调用方里有单任务 httpd，
       OT 忙时永久阻塞会把整个 WebUI 拖死。 */
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(200))) {
        return ESP_ERR_TIMEOUT;
    }
    otInstance *inst = esp_openthread_get_instance();
    if (!inst) {
        esp_openthread_lock_release();
        return ESP_ERR_INVALID_STATE;
    }
    *out_channel = otLinkGetChannel(inst);
    esp_openthread_lock_release();
    return ESP_OK;
}

/* ---- 能量扫描诊断（P0，纯观测） ----
 *
 * 计数器语义见 thread_br.h 的 thread_br_ed_diag_t 注释。
 * 这里只做三件事：累加 ed_failed 回调次数、读硬件计数器、在扫描前后取快照。
 * **不向 OpenThread 转发任何东西**，所以扫描结果与加这段代码之前逐位一致。
 *
 * H1 verified（hal/ieee802154_common_ll.h）:
 *   :482 ieee802154_ll_clear_debug_cnt(bits) —— 写一次即清，无需回写 0
 *        （驱动自己就是这么用的，esp_ieee802154_debug.c:350-362）
 *   :497/:517/:532 三个 getter 直读 debug_* 寄存器，自由运行，
 *        **不依赖 CONFIG_IEEE802154_DEBUG**（本工程该选项为 off）
 *   :92/:94 IEEE802154_RX_ABORT_BY_ED_ABORT=24 / _ED_COEX_REJECT=26，
 *        使能掩码用 BIT(reason - 1)，与 esp_ieee802154_dev.c:906 的写法一致
 * 清计数器是全局副作用，但本工程 CONFIG_IEEE802154_TXRX_STATISTIC 未开，
 * 驱动侧的 ieee802154_rx_nums_update() 不参与编译，无人与我们争这几个寄存器。
 */
#include "hal/ieee802154_common_ll.h"   /* 计数器 getter / rx_abort 事件位 */
#include "soc/ieee802154_reg.h"         /* *_CNT_CLEAR 位定义 */

#define ED_IRQ_BITS  (BIT(IEEE802154_RX_ABORT_BY_ED_ABORT - 1) | \
                      BIT(IEEE802154_RX_ABORT_BY_ED_COEX_REJECT - 1))
#define ED_CNT_BITS  (IEEE802154_ED_SCAN_COEX_CNT_CLEAR | \
                      IEEE802154_ED_ABORT_CNT_CLEAR     | \
                      IEEE802154_RX_ABORT_COEX_CNT_CLEAR | \
                      IEEE802154_CCA_FAIL_CNT_CLEAR)

static volatile uint32_t s_ed_failed_cb;          /* ISR 里累加 */
static volatile bool     s_ed_hook_armed;         /* 仅在扫描窗口内为 true */
static thread_br_scan_diag_t s_scan_diag;
static portMUX_TYPE s_diag_mux = portMUX_INITIALIZER_UNLOCKED;

/* IDF 的私有头 esp_ieee802154_dev.h:240 才有这个声明，应用侧拿不到，故自行声明。
   实现是 OT port 的强符号（esp_openthread_radio.c:798，IRAM_ATTR）：
   只做 s_ed_power = power; set_event(EVENT_ENERGY_DETECT_DONE)，
   随后 OT mainloop 在 radio.c:228-231 调 otPlatRadioEnergyScanDone。 */
extern void esp_ieee802154_energy_detect_done(int8_t power);

/* 覆盖 IDF 的 weak 空桩（esp_ieee802154.c:486）。调用点在 802.15.4 中断里
 * （esp_ieee802154_dev.c:612-615，ED_ABORT / ED_COEX_REJECT 两种中止）。
 *
 * S2 上下文：ISR。只自增一个 volatile，无锁无阻塞。不加 IRAM_ATTR 是有意的——
 * 802.15.4 中断以 flags=0 注册（dev.c:923，无 ESP_INTR_FLAG_IRAM），cache 关闭
 * 期间该中断被屏蔽而不是崩；而本板内部 SRAM 已经很紧（见 /api/v1/diag/heap），
 * 不值得为三条指令占 IRAM。
 *
 * ⚠️ 默认情况下这个函数一次都不会被调用：ED 类 abort 事件位在驱动里从未被使能
 * （dev.c:906 只使能了 TX_ACK_TIMEOUT | TX_ACK_COEX_BREAK 两位），所以扫描期间
 * 必须由 ed_irq_arm() 临时把它们打开，否则 ED 被拒是"静默死亡"。
 * 2026-08-11 实测：不 arm 时本函数 0 次触发，arm 后精确 16/16 触发。
 *
 * 转发 OT_RADIO_RSSI_INVALID(127) 而不是某个能量值：OT 在
 * Mac::ReportEnergyScanResult (mac.cpp:318) 会把 127 丢弃、不上报该信道，
 * 然后照常推进到下一个信道。于是"被拒的信道"= 没有回调 = valid 位不置位，
 * 既不会伪造数据，也不会卡住 MAC。
 *
 * ⚠️ s_ed_hook_armed 这个门是必须的：不在扫描窗口内时若还往 OT 灌
 * EnergyScanDone，会经 Mac::FinishOperation() 把 mOperation 无条件置成 Idle，
 * 把 OT 正在进行的 TX/RX 踢掉 —— 那比现在的故障更隐蔽。 */
void esp_ieee802154_ed_failed(uint16_t error)
{
    (void)error;
    s_ed_failed_cb++;
    if (s_ed_hook_armed) {
        esp_ieee802154_energy_detect_done(OT_RADIO_RSSI_INVALID);
    }
}

/* 扫描窗口内临时使能 ED 类 abort 中断；返回原值供 ed_irq_restore() 还原。
   不是无脑关掉，因为 /api/v1/diag/ed?arm=1 可能已经手工打开了。 */
static uint32_t ed_irq_arm(void)
{
    portENTER_CRITICAL(&s_diag_mux);
    uint32_t prev = IEEE802154.rx_abort_event_en.rx_abort_en;
    ieee802154_ll_enable_rx_abort_events(ED_IRQ_BITS);
    portEXIT_CRITICAL(&s_diag_mux);
    return prev;
}

static void ed_irq_restore(uint32_t prev)
{
    portENTER_CRITICAL(&s_diag_mux);
    /* 只还原我们动过的两位，别把期间别人使能的位一起抹掉 */
    ieee802154_ll_disable_rx_abort_events(ED_IRQ_BITS & ~prev);
    portEXIT_CRITICAL(&s_diag_mux);
}

esp_err_t thread_br_ed_diag_read(thread_br_ed_diag_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    /* 射频没起来时外设寄存器读数没有意义，别给出一组会被当真的 0。 */
    if (!thread_br_is_running()) return ESP_ERR_INVALID_STATE;

    out->ed_scan_coex      = ieee802154_ll_get_ed_scan_coex_cnt();
    out->ed_abort          = ieee802154_ll_get_ed_abort_cnt();
    out->rx_abort_coex     = ieee802154_ll_get_rx_abort_coex_cnt();
    out->cca_fail          = ieee802154_ll_get_cca_fail_cnt();
    out->rx_abort_event_en = IEEE802154.rx_abort_event_en.rx_abort_en;
    out->ed_irq_armed      = (out->rx_abort_event_en & ED_IRQ_BITS) == ED_IRQ_BITS;
    out->ed_failed_cb      = s_ed_failed_cb;
    return ESP_OK;
}

void thread_br_scan_diag_read(thread_br_scan_diag_t *out)
{
    if (!out) return;
    /* 快照由跑扫描的任务（httpd 或 spectrum monitor）整体写入，这里整体读出，
       避免读到写了一半的结构。C5 单核，临界区即关中断，几十条指令。 */
    portENTER_CRITICAL(&s_diag_mux);
    *out = s_scan_diag;
    portEXIT_CRITICAL(&s_diag_mux);
}

esp_err_t thread_br_ed_diag_arm_irq(bool arm)
{
    if (!thread_br_is_running()) return ESP_ERR_INVALID_STATE;

    /* S2：rx_abort_event_en 是读-改-写，而 next_operation() 在 ISR 里也会动它
       （dev.c:474-478，仅 receive_at 路径；本板 rx-on-when-idle 不走那条）。
       驱动的自旋锁是私有的，用本地临界区——C5 单核下等价于关中断，足以排他。 */
    portENTER_CRITICAL(&s_diag_mux);
    if (arm) ieee802154_ll_enable_rx_abort_events(ED_IRQ_BITS);
    else     ieee802154_ll_disable_rx_abort_events(ED_IRQ_BITS);
    portEXIT_CRITICAL(&s_diag_mux);

    ESP_LOGW(TAG, "ED abort IRQ %s（诊断用，不改变扫描行为）",
             arm ? "已使能" : "已还原");
    return ESP_OK;
}

/* ---- Energy scan via OpenThread ----
 * H1 contract (openthread/link.h): otLinkEnergyScan(instance, channel_mask,
 * duration_ms_per_channel, callback, ctx). Callback runs in the OT task,
 * once per channel with the result, then once with aResult == NULL on
 * completion. otEnergyScanResult = { mChannel, mMaxRssi }. */
static struct {
    volatile bool done;
    int8_t *energy;          /* 16 entries, ch11..26 */
    volatile uint16_t valid; /* bit i = ch(11+i) 真的回调上来了 */
} s_escan;

static void energy_scan_cb(otEnergyScanResult *result, void *ctx)
{
    if (!result) {           /* NULL = scan complete */
        s_escan.done = true;
        return;
    }
    int idx = result->mChannel - 11;
    if (idx >= 0 && idx < 16 && s_escan.energy) {
        /* max-hold：多轮短采样里取最大值。单次 1ms 窗口只有 62 symbol，
           容易错过突发流量；跨轮取最大能把"偶尔很吵"的信道显出来，
           信息量接近一次长窗口，而每次采样都短到能赢下共存仲裁。 */
        if (!(s_escan.valid & (1u << idx)) || result->mMaxRssi > s_escan.energy[idx]) {
            s_escan.energy[idx] = result->mMaxRssi;
        }
        /* OT 在 Mac::ReportEnergyScanResult (mac.cpp:318) 已经把 kInvalidRssi(127)
           过滤掉了，所以"回调来过"就等于"这个信道真测到了"。 */
        s_escan.valid |= (uint16_t)(1u << idx);
    }
}

/*
 * 扫描没在超时内完成时，把它强行走完。
 *
 * 为什么必须有这个：ESP-IDF v6.0.2 的 802.15.4 驱动在 ED 被中止时走
 *   esp_ieee802154_dev.c:612  case IEEE802154_RX_ABORT_BY_ED_ABORT:
 *   esp_ieee802154_dev.c:613  case IEEE802154_RX_ABORT_BY_ED_COEX_REJECT:
 *                                 esp_ieee802154_ed_failed(rx_status);
 * 而 esp_ieee802154_ed_failed() 是个 weak 空桩——**IDF 的 OpenThread port 没有实现它**
 * （只实现了成功路径 esp_ieee802154_energy_detect_done，见 esp_openthread_radio.c:798）。
 * 于是 otPlatRadioEnergyScanDone() 永远不来，OpenThread 的 Mac 永久停在
 * kOperationEnergyScan：所有报文堆在它后面，tx/rx 计数器冻结，子节点超时被摘，
 * 发送队列撑满刷 "MeshForwarder: Dropping (dir queue full)"，**整个 Thread 网络死亡，
 * 只有整板重启能恢复**。本板 CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y 且 WiFi 常驻，
 * 共存仲裁器拒绝 ED 是常态，所以点开一次 WebUI 频谱页就足以打死 Thread。
 * 2026-08-09 实测确认，详见 docs/ 与提交记录。
 *
 * 收尾方式：otPlatRadioEnergyScanDone() 每调一次只推进一个信道
 * （Mac::HandleEnergyScanDone → ReportEnergyScanResult → PerformEnergyScan），
 * 所以要循环把剩余信道走完，最后一个信道结束时 OT 才会用 NULL 回调通知完成。
 * 上限取 16 信道 + 4 余量；每轮之间留一个信道的时间，让真正能跑通的 ED 正常完成。
 *
 * 上下文：本函数在调用者任务里执行（非 OT 任务），因此每次调用都持 OT 锁——
 * 这正是 esp_openthread_lock 的用途。ISR 里不能这么做，所以没有去覆盖那个 weak 符号。
 */
static void force_finish_energy_scan(uint16_t duration_per_ch_ms)
{
    ESP_LOGW(TAG, "Energy scan did not complete (IDF ED-failure callback missing) "
                  "— forcing completion to avoid wedging the MAC");

    for (int i = 0; i < 20 && !s_escan.done; i++) {
        esp_openthread_lock_acquire(portMAX_DELAY);
        otInstance *inst = esp_openthread_get_instance();
        bool in_progress = inst && otLinkIsEnergyScanInProgress(inst);
        if (in_progress) {
            otPlatRadioEnergyScanDone(inst, OT_RADIO_RSSI_INVALID);
        }
        esp_openthread_lock_release();

        if (!in_progress) break;   /* 已经收干净了 */
        vTaskDelay(pdMS_TO_TICKS(duration_per_ch_ms + 50));
    }

    if (s_escan.done) {
        ESP_LOGI(TAG, "Energy scan force-completed, MAC released");
    } else {
        ESP_LOGE(TAG, "Energy scan still stuck after forced completion — "
                      "Thread TX may be blocked until reboot");
    }
}

/* 持 OT 锁调用 */
static void count_children_locked(otInstance *inst, uint8_t *total, uint8_t *online)
{
    uint16_t max_children = otThreadGetMaxAllowedChildren(inst);
    uint8_t t = 0, o = 0;
    for (uint16_t i = 0; i < max_children; i++) {
        otChildInfo ci;
        if (otThreadGetChildInfoByIndex(inst, i, &ci) != OT_ERROR_NONE) continue;
        t++;
        /* mIsStateRestoring：条目是从 NVS 恢复出来的、设备还没真正回来。
           H1 verified: openthread/thread_ftd.h:81 */
        if (!ci.mIsStateRestoring) o++;
    }
    if (total)  *total  = t;
    if (online) *online = o;
}

/* 子设备数缓存：child_stats_timer_cb 每 2 s 采一次（只试不等，拿到锁才更新）。
   thread_br_get_child_stats() 只读缓存 —— 它的调用方是 /status，跑在唯一的 httpd 任务里；以前在那里等 OT 锁
   最长 5 s，OT 一忙（注册潮，ot_cpu_guard.c 还会持着锁让出 CPU）整个 Web 界面跟着卡（2026-10-07 审计）。
   代价：读数最多晚 2 s。8 位对齐读写是原子的。 */
static volatile uint8_t s_child_total, s_child_online;

#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
/* 直连型 + Muse：BLE 窗口之后的 Thread 自检（docs/15 设计 3.3）要的 ACK 计数。2 s 采样顺手缓存累计值，每 60 s 存一格进
   10 格环形缓冲 —— "过去约 10 分钟"的增量就是"现在 - 最老那一格"。只在采到锁的那一拍更新（同 s_child_*）。 */
#define ACK_RING_N 10
static volatile uint32_t s_ack_req, s_acked;
static volatile bool     s_ack_valid;
static struct { uint32_t req, acked; int64_t t_us; } s_ack_ring[ACK_RING_N];
static volatile uint8_t  s_ack_ring_n, s_ack_ring_head;
static uint32_t          s_ack_ring_ticks;
#endif

void thread_br_get_child_stats(uint8_t *total, uint8_t *online)
{
    bool run = thread_br_is_running();
    if (total)  *total  = run ? s_child_total : 0;
    if (online) *online = run ? s_child_online : 0;
}

/* 采样子节点统计（2s 一次）。
   S2：LED 渲染跑在 20fps 的任务里，在那里取 OT 锁会和 OT mainloop 抢锁，
   所以由这里低频采样，渲染侧只读缓存。

   ⚠️ 2026-08-15 起这两个数**不再喂给 LED**。它们只统计网关的**直连**子节点，
   而网状网里终端设备会挂到离它更近的路由器下面——当天实测，门窗传感器改挂到
   另一台 FTD（rloc 0x9000）下面之后，网关子节点表为空、HA 一切正常，而一条
   从 NVS 恢复出来、设备再也不会回来的 restoring 残留条目让灯黄闪了好几分钟。
   仍然保留是因为 had_child 和 /api/v1/status 要用。 */
static void child_stats_timer_cb(void *arg)
{
    (void)arg;
    /* ⚠️ 这里跑在 esp_timer 任务里（优先级 22）：只**试一次** OT 锁，拿不到就当这一拍没采到，绝不等。
       2026-10-07 前这里先后等过三次锁（取子设备数 5 s、MAC 计数器 2 s、角色 0.5 s）：OT 任务一忙
       （网关重启后设备成批重注册），esp_timer 任务就被卡住好几秒 —— 全系统别的定时器（mdns 的
       100 ms 发送节拍等）跟着停；而且它一等锁，持锁的 ot_main（16）就被优先级继承抬到 22，压过
       EZSP 桥（20/21）和 lwIP（18）。coredump 里 ot_main 是 22/16，就是这么来的。
       没采到的这一拍：子设备数不更新（had_child 是只升不降的标记，不急）；停摆检测按"没动"计一拍、但不覆盖
       上一次读到的计数器 —— OT 一直不放锁时照样 180 s 判出停摆；"射频死了、协议栈还在忙"时间歇抢不到锁，
       也不会把连续不动的计数清零（若把没读到的 0 当读数写进去，0≠N、N≠0 会把它清零两次）；
       灯的状态沿用上一次（下面那段）。语义同 docs/04_watchdog_design_v2.md §6.2。 */
    bool running = thread_br_is_running();
    bool sampled = false;               /* 这一拍真拿到了锁 */
    uint8_t total = 0, online = 0;
    uint32_t tx = 0, rx = 0;
#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
    uint32_t ack_req = 0, acked = 0;
#endif
    /* attached：role 是 child/router/leader 才算真的在网里。旧判据完全不看这个，
       于是网关掉出 Thread 网络（detached）时，只要没有 restoring 子节点，灯还是
       绿的——那正是最该报警的时候。 */
    bool attached = false;
    /* br_ok：边界路由真的在跑。Thread 网内部好好的（leader）而边界路由停了的时候，
       HA 已经连不到任何 Thread 设备 —— 2026-10-05 拔网线实测灯却一直是绿的（测试 6c）。 */
    bool br_ok = false;
    otBorderRoutingState br_state = OT_BORDER_ROUTING_STATE_UNINITIALIZED;
    if (running && esp_openthread_lock_acquire(0)) {
        otInstance *inst = esp_openthread_get_instance();
        if (inst) {
            count_children_locked(inst, &total, &online);
            const otMacCounters *mc = otLinkGetCounters(inst);
            tx = mc->mTxTotal; rx = mc->mRxTotal;
#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
            ack_req = mc->mTxAckRequested; acked = mc->mTxAcked;
#endif
            otDeviceRole r = otThreadGetDeviceRole(inst);
            attached = (r == OT_DEVICE_ROLE_CHILD || r == OT_DEVICE_ROLE_ROUTER ||
                        r == OT_DEVICE_ROLE_LEADER);
            br_state = otBorderRoutingGetState(inst);
            br_ok = (br_state == OT_BORDER_ROUTING_STATE_RUNNING);
            sampled = true;
        }
        esp_openthread_lock_release();
    }

    if (sampled) { s_child_total = total; s_child_online = online; }
    else if (!running) { s_child_total = 0; s_child_online = 0; }
#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
    if (sampled) {
        s_ack_req = ack_req; s_acked = acked; s_ack_valid = true;
        if (s_ack_ring_ticks++ % 30 == 0) {      /* 2 s × 30 = 60 s 一格；Thread 重新跑起来的第一拍就写一格 */
            s_ack_ring[s_ack_ring_head].req   = ack_req;
            s_ack_ring[s_ack_ring_head].acked = acked;
            s_ack_ring[s_ack_ring_head].t_us  = esp_timer_get_time();
            s_ack_ring_head = (uint8_t)((s_ack_ring_head + 1) % ACK_RING_N);
            if (s_ack_ring_n < ACK_RING_N) s_ack_ring_n++;
        }
    } else if (!running) {
        /* 整个环一起清（含写入位置与节拍）：不然恢复后"最老一格"会取到暂停前的旧值，窗口横跨两次运行 */
        s_ack_valid = false; s_ack_ring_n = 0; s_ack_ring_head = 0; s_ack_ring_ticks = 0;
    }
#endif

    /* 记下"这个网络接入过设备"。一旦置位就不再自动改信道 —— 用 total 而不是
       online：从 NVS 恢复出来、设备还没回来的条目同样算数，那也是用户的设备。
       had_child_store() 自身幂等，稳态下不会每 2s 写一次 NVS。 */
    if (total > 0) had_child_store(true);

    /* 射频停摆检测：MAC 计数器长时间不动 = 收发已停（2026-08-09 能量扫描事故
       的特征）。孤立单节点网络也会周期性发 MLE advertisement，所以 180s 不动
       一定不正常。这颗灯是用户唯一能看到的诊断入口，必须报出来。 */
    static uint32_t s_last_tx, s_last_rx;
    static uint32_t s_still_ticks;
    bool stalled = false;
    /* 用户在网页上停用了 Thread（PUT /node/state disable：otThreadSetEnabled(false)，并把 thread.enabled=false
       落盘）：协议栈停了、s_running 却还是 true，MAC 进 sleep、计数器不动 —— 那是用户要的，不是停摆。不判，
       否则看门狗约 4 分钟后把网关整机重启（T9.18 方案 A 之后 Thread 停摆走 L4，2026-10-07 审计）。
       配置开着却停在 disabled（SoftAP 暂停后 resume 失败，thread_radio_mgr.c）照常判：那是真故障，重启才救得回来。 */
    if (running && config_mgr_get_thread()->enabled) {
        if (!sampled || (tx == s_last_tx && rx == s_last_rx)) s_still_ticks++;   /* 没读到 = 没看到它动 */
        else s_still_ticks = 0;
        if (sampled) { s_last_tx = tx; s_last_rx = rx; }
        stalled = (s_still_ticks >= 90);      /* 2s * 90 = 180s */
    } else {
        s_still_ticks = 0;
    }
    bool br_sampled = sampled;          /* 真读到了状态（拿不到锁那一拍不算） */
    /* 拿不到 OT 锁 ≠ 没入网 / 边界路由停了。原来这一拍会把 attached、br_ok 都当成 false，
       灯就无声地琥珀 —— 无声是因为下面的日志只在真读到状态时才记。2026-10-05 拔线验收：
       日志说边界路由 6 s 就恢复，用户却看到琥珀约 40 s。没读到就沿用上一次读到的值；
       OT 真卡死时，上面的 MAC 计数器停摆检测（180 s → 红）照样会报出来。 */
    static bool     s_prev_attached = false, s_prev_br_ok = false;
    static uint32_t s_lock_miss = 0;
    /* 只试不等之后，负载下偶尔一拍抢不到锁是常态：连续 LOCK_MISS_WARN 拍（6 s）才记一对告警，
       否则每次单拍失手都是"拿不到 / 恢复"两条 W，按 log_store.c 头注的预算会把告警环刷掉 */
    enum { LOCK_MISS_WARN = 3 };
    if (running) {
        if (br_sampled) {
            if (s_lock_miss >= LOCK_MISS_WARN) {
                ESP_LOGW(TAG, "健康采样恢复：此前连续 %" PRIu32 " 次（约 %" PRIu32 " s）拿不到 OT 锁",
                         s_lock_miss, s_lock_miss * 2);
            }
            s_lock_miss = 0;
            s_prev_attached = attached;
            s_prev_br_ok    = br_ok;
        } else {
            if (++s_lock_miss == LOCK_MISS_WARN) {
                ESP_LOGW(TAG, "健康采样连续 %d 次拿不到 OT 锁（esp_timer 里只试不等）—— THREAD 灯沿用上一次的状态",
                         LOCK_MISS_WARN);
            }
            attached = s_prev_attached;
            br_ok    = s_prev_br_ok;
        }
    } else {
        s_lock_miss = 0;
    }
    led_ctrl_set_thread_health(running, stalled, attached);
    led_ctrl_set_thread_border_routing(br_ok);

    /* 边界路由状态一变就记一条，THREAD 灯的琥珀/绿就是跟着它走的。两条都用 W：成对留在
       告警环里（主环几十分钟就被冲掉），事后能对上"灯琥珀了多久、当时走哪个口"。
       2026-10-05 拔网线验收时用户看到琥珀"超过十秒"，而换口日志在拔线后 6 s —— 没有这两条
       就无从判断灯到底几时变回绿（拔线后主机有 26 s 连不上网关，外部采不到）。 */
    static int s_last_br = -1;          /* -1 未知 / 0 停 / 1 在跑 */
    if (!running) {
        s_last_br = -1;
    } else if (br_sampled && (br_ok ? 1 : 0) != s_last_br) {
        const char *st = br_state == OT_BORDER_ROUTING_STATE_DISABLED ? "disabled"
                       : br_state == OT_BORDER_ROUTING_STATE_STOPPED  ? "stopped"
                       : br_state == OT_BORDER_ROUTING_STATE_RUNNING  ? "running" : "uninitialized";
        if (br_ok) {
            ESP_LOGW(TAG, "边界路由恢复运行（backbone=%s）—— THREAD 灯回绿", thread_br_backbone_name());
        } else if (s_last_br != -1 || attached) {
            ESP_LOGW(TAG, "边界路由没在运行（state=%s，backbone=%s）—— THREAD 灯琥珀慢闪，"
                          "HA 暂时连不到 Thread 设备", st, thread_br_backbone_name());
        }
        s_last_br = br_ok ? 1 : 0;
    }

    /* 看门狗层 C 的探针读这份缓存。这里已经持锁采过样了，探针再取一次
       OT 锁会和 mainloop 抢锁、拖慢射频，所以只暴露结论。 */
    s_health_running = running;
    s_health_stalled = stalled;
}

#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
bool thread_br_get_ack_now(uint32_t *ack_req, uint32_t *acked)
{
    if (!thread_br_is_running() || !s_ack_valid) return false;
    if (ack_req) *ack_req = s_ack_req;
    if (acked)   *acked   = s_acked;
    return true;
}

bool thread_br_get_ack_window(uint32_t *req_delta, uint32_t *acked_delta, uint32_t *span_s)
{
    if (!thread_br_is_running() || !s_ack_valid || s_ack_ring_n == 0) return false;
    /* 最老的那一格 = 写入位置往回数 n 格（满了时正好就是 head） */
    const uint8_t oldest = (uint8_t)((s_ack_ring_head + ACK_RING_N - s_ack_ring_n) % ACK_RING_N);
    const uint32_t r0 = s_ack_ring[oldest].req, a0 = s_ack_ring[oldest].acked;
    const int64_t t0 = s_ack_ring[oldest].t_us;
    if (req_delta)   *req_delta   = s_ack_req - r0;      /* 无符号减法：计数回绕也对 */
    if (acked_delta) *acked_delta = s_acked - a0;
    if (span_s)      *span_s      = (uint32_t)((esp_timer_get_time() - t0) / 1000000);
    return true;
}
#endif

/* 强符号，覆盖 wd_probes.c 里的 weak 默认实现（watchdog 不能反向依赖
   thread_br：thread_br → led_ctrl → watchdog 已经成链）。 */
bool thread_br_get_health(bool *running, bool *stalled)
{
    if (running) *running = s_health_running;
    if (stalled) *stalled = s_health_stalled;
    return true;
}

/* L2 定向恢复：只重启 802.15.4 射频，不动 OT 实例与数据集。
   2026-08-09 的两次事故（射频静默死亡、能量扫描后 MAC 永久卡死）都属于
   "协议栈状态正常但收发已停"，射频层重来一次即可，代价远小于重建 BR。 */
esp_err_t thread_br_radio_recover(void)
{
    if (!thread_br_is_running()) return ESP_ERR_INVALID_STATE;

    ESP_LOGW(TAG, "射频恢复：disable → enable（看门狗 L2）");
    esp_err_t ret = ESP_FAIL;
    if (esp_openthread_lock_acquire(pdMS_TO_TICKS(2000))) {
        otInstance *inst = esp_openthread_get_instance();
        if (inst) {
            /* H1: otPlatRadioSleep 要求射频处于 Receive/Sleep 态；先 Sleep
               再 Enable 是 openthread/platform/radio.h 允许的状态迁移。 */
            otPlatRadioSleep(inst);
            ret = (otPlatRadioEnable(inst) == OT_ERROR_NONE) ? ESP_OK : ESP_FAIL;
        }
        esp_openthread_lock_release();
    }
    return ret;
}

/* ---- 运行中换 backbone（见 thread_br.h 的 thread_br_switch_backbone 说明）---- */

static const char *netif_label(esp_netif_t *n)
{
    if (!n) return "none";
    if (n == esp_netif_get_handle_from_ifkey("ETH_DEF"))      return "eth";
    if (n == esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) return "wifi";
    return "other";
}

const char *thread_br_backbone_name(void)
{
    return s_border_routing ? netif_label(s_backbone_netif) : "none";
}

/* 单个切换任务的输入/输出放静态区：调用方等超时返回后任务仍在跑，放调用方栈上就是
   用后释放。s_bb_busy 保证同一时刻只有一个任务在用它们。 */
static SemaphoreHandle_t                s_bb_done;
static esp_netif_t                     *s_bb_to;
static thread_br_backbone_switch_t      s_bb_res;
static volatile bool                    s_bb_busy = false;
static portMUX_TYPE                     s_bb_mux = portMUX_INITIALIZER_UNLOCKED;

/* 真正拆/建边界路由的地方。调用方必须已持 s_bb_busy，且跑在**内部 SRAM 栈、≥4096**
   的任务里（border_router_init 在预编译库里，可能写 NVS；开机的 ot_br_init_task 也是
   4096 调它）。自动跟随与测试接口各建一个 4096 的任务调它，同一时刻只有一个
   （s_bb_busy）；自动跟随为什么只在安静下来之后才建任务，见下方跟随一节的 ⚠️。 */
static void do_backbone_switch(esp_netif_t *to, thread_br_backbone_switch_t *out)
{
    thread_br_backbone_switch_t r = {
        .deinit_err = ESP_OK, .init_err = ESP_FAIL,
        .from = thread_br_backbone_name(), .to = netif_label(to),
    };
    /* 与 ot_examples_br.c 一致：set_backbone + border_router_init 都在 OT 锁内；
       deinit 同样持锁（示例的 stop 就是这么做的）。锁等 5 s，拿不到就放弃，不硬等。 */
    bool life = life_take();
    if (!life) {
        r.init_err = ESP_ERR_TIMEOUT;
    } else if (!s_running || thread_br_radio_is_paused() || !s_br_init_done ||
               !esp_netif_is_netif_up(to)) {
        /* 等锁期间 Thread 被停了 / 暂停了 / 正在重启（新一轮 ot_br_init_task 还没做完，
           这时再 init 一次两边必有一个 INVALID_STATE），或目标口已经断了 */
        r.init_err = ESP_ERR_INVALID_STATE;
    } else if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(5000))) {
        r.init_err = ESP_ERR_TIMEOUT;
    } else {
        int64_t t0 = esp_timer_get_time();
        if (s_border_routing) {
            r.deinit_err = br_deinit_locked();
            s_border_routing = false;
        }
        esp_openthread_set_backbone_netif(to);
        s_backbone_netif = to;
        s_backbone_ready = true;
        r.init_err = esp_openthread_border_router_init();
        s_border_routing = (r.init_err == ESP_OK);
        r.ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        esp_openthread_lock_release();
    }
    if (life) life_give();
    /* 栈余量一并记下：库内栈用量不可知，4096 是照开机任务定的，有了实测才好再收紧 */
    unsigned hwm = (unsigned)uxTaskGetStackHighWaterMark(NULL);
    if (r.init_err == ESP_OK && r.deinit_err == ESP_OK) {
        ESP_LOGW(TAG, "边界路由 backbone：%s → %s（%" PRIu32 " ms，栈余量 %u 字节）",
                 r.from, r.to, r.ms, hwm);
    } else {
        ESP_LOGE(TAG, "边界路由 backbone %s → %s 失败：deinit=%s init=%s（%" PRIu32 " ms，栈余量 %u 字节）",
                 r.from, r.to, esp_err_to_name(r.deinit_err), esp_err_to_name(r.init_err), r.ms, hwm);
    }
    if (out) *out = r;
}

/* 测试接口（thread_br_switch_backbone）走这里：调用方是 httpd 任务，栈不够也不该让它
   去持 OT 锁，所以单起一个任务。自动跟随不走这里。 */
static void backbone_switch_task(void *arg)
{
    thread_br_backbone_switch_t r;
    do_backbone_switch(s_bb_to, &r);
    s_bb_res = r;
    xSemaphoreGive(s_bb_done);
    s_bb_busy = false;
    vTaskDelete(NULL);
}

esp_err_t thread_br_switch_backbone(const char *to, uint32_t timeout_ms,
                                    thread_br_backbone_switch_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!to) return ESP_ERR_INVALID_ARG;
    const char *key = !strcmp(to, "eth") ? "ETH_DEF" : !strcmp(to, "wifi") ? "WIFI_STA_DEF" : NULL;
    esp_netif_t *n = key ? esp_netif_get_handle_from_ifkey(key) : NULL;
    if (!n) return ESP_ERR_NOT_FOUND;
    /* 句柄存在 ≠ 接口可用：出厂态 STA 句柄非空但从未 up，设成 backbone 会让 OT 往
       output_ip6 == NULL 的 netif 发包 → 跳到地址 0（见 thread_br_start 第 2 步）。 */
    if (!s_running || thread_br_radio_is_paused() || !esp_netif_is_netif_up(n)) {
        return ESP_ERR_INVALID_STATE;
    }

    bool mine = false;
    portENTER_CRITICAL(&s_bb_mux);
    if (!s_bb_busy) { s_bb_busy = true; mine = true; }
    portEXIT_CRITICAL(&s_bb_mux);
    if (!mine) return ESP_ERR_NOT_FINISHED;

    if (!s_bb_done && !(s_bb_done = xSemaphoreCreateBinary())) {
        s_bb_busy = false;
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_bb_done, 0);      /* 清掉上一轮超时后才到的 give */
    s_bb_to = n;
    /* 内部 SRAM 栈：border_router_init 在预编译库里，可能写 NVS（OMR 前缀等存 settings），
       PSRAM 栈在 flash 写期间 cache 关闭会随机崩（psram_task.h 规则 1）。4096 同开机的
       ot_br_init_task（它也调 border_router_init）；官方示例用 6144，但每多 1 KB 都是内部内存。 */
    if (xTaskCreate(backbone_switch_task, "ot_bb_switch", 4096, NULL, 5, NULL) != pdPASS) {
        s_bb_busy = false;
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_bb_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (out) *out = s_bb_res;
    return s_bb_res.init_err;
}

/* ---- 上行切换时边界路由跟着换口（2026-10-05，docs/test_cases_stability.md 测试 6c）----
 *
 * 实测：以太网一断、网关退到 WiFi，边界路由停在 stopped，Matter 设备 15 分钟不恢复；
 * 运行中把 backbone 换到 WiFi（thread_br_switch_backbone）后 5 s 内恢复并稳定 21 分钟。
 *
 * 等 BACKBONE_SETTLE_MS 再看：网线抖动时 FAILOVER/RECOVERED 会成串来，每次换口 Matter
 * 断几秒，不值得跟着每一下抖。每来一个事件就把定时器从头计时，安静满 5 s 才看一次，
 * 不会漏掉最后状态。规则只有一条：边界路由要在 pick_backbone() 选出的口上 —— 与开机
 * 选口完全相同，所以网线回来时也会换回以太网。
 *
 * ⚠️ 防抖必须用定时器，别改回"建任务后睡 5 s"：拔线后那几秒是全系统内部/DMA 内存
 * 最紧的时刻 —— ZHA 重连（一次 ~1.3 KB）、webhook 走 TLS、MQTT 重连都挤在 +1~4 s。
 * 旧做法一收到事件就建 4096 栈的任务再睡，这 4 KB 正好压在谷底上：拆成 3072+6144 两个
 * 任务时 dma.min_free 测到 2531，合并成一份 4096 后反而 1579（重连高峰上多压了 1 KB），
 * 都低于硬停判据 6 的 3072。现在任务只在定时器到点、确认真要换口时才建，换完即删，
 * 不需要换口的事件（开机的 ONLINE、同口的 RECOVERED）一个任务都不建。 */
#define BACKBONE_SETTLE_MS  5000

/* 内部 SRAM 栈、4096：border_router_init 在预编译库里，可能写 NVS，不能用 PSRAM 栈
   （任务还自删，psram_task.h 也不允许）。arg 是目标口；s_bb_busy 已由定时器回调置上。 */
/* 换口失败后自动重试的上限（2026-10-05 审计）。失败最可能出在拔线那一刻 —— 正是内部
   内存最紧、建 4096 栈任务最容易失败的时候；而网络稳在 WiFi 上之后不会再有事件，不重试
   就是测试 6c 的原症状（边界路由停在死口上）。有上限是为了确定性失败时别每 5 s 刷一条。
   三个任务（事件循环 / esp_timer / 跟随任务）都会读写它，竞争最多多试或少试一次，无害。 */
#define BACKBONE_FOLLOW_MAX_RETRY  3
static volatile uint8_t s_follow_retries;

/* 只重新计时，不动重试计数。
   ⚠️ 别用 esp_timer_restart()：IDF v6.0.2 里它在锁**外**判断"在不在跑"（esp_timer.c:143）
   再进锁摘链表，与到期处理交错时会对同一个定时器摘两次，可能弄坏整条定时器链表；
   esp_timer_stop / esp_timer_start_once 都是进锁后才判断（源码注释"to avoid a data race"）。 */
static void backbone_follow_arm(void)
{
    if (!s_follow_timer) return;             /* 创建失败已在 thread_br_init 报过 */
    esp_timer_stop(s_follow_timer);          /* 没在计时会回 INVALID_STATE，无妨 */
    /* 两个调用方同时走到这里时后一个会拿到 INVALID_STATE，但定时器已经在跑了，不算失败 */
    if (esp_timer_start_once(s_follow_timer, (uint64_t)BACKBONE_SETTLE_MS * 1000) != ESP_OK &&
        !esp_timer_is_active(s_follow_timer)) {
        ESP_LOGE(TAG, "边界路由跟随定时器启动失败 —— 本次上行切换后边界路由可能停在旧口上");
    }
}

/* 换口失败：有上限地再试一次。 */
static void backbone_follow_retry(const char *why)
{
    if (s_follow_retries >= BACKBONE_FOLLOW_MAX_RETRY) {
        ESP_LOGE(TAG, "边界路由跟随连续失败 %d 次（最后一次：%s），放弃 —— 等下一次网络事件；"
                      "在那之前 HA 到 Thread 设备可能不通、THREAD 灯保持琥珀，重启网关可恢复",
                 BACKBONE_FOLLOW_MAX_RETRY + 1, why);
        return;
    }
    s_follow_retries++;
    ESP_LOGW(TAG, "边界路由跟随失败（%s），%d s 后第 %u 次重试",
             why, BACKBONE_SETTLE_MS / 1000, (unsigned)s_follow_retries);
    backbone_follow_arm();
}

static void backbone_follow_task(void *arg)
{
    thread_br_backbone_switch_t r;
    do_backbone_switch((esp_netif_t *)arg, &r);   /* 结果日志在里面 */
    s_bb_busy = false;
    if (r.init_err == ESP_OK) {
        s_follow_retries = 0;
        /* 开机时没有上行、边界路由这才第一次起来的情形，开机路径里这两件都没做成
           （T9.7②③）；平时换口也值得重新宣告一遍 mDNS。两者都幂等。 */
        zb_check_ensure();
        mdns_refresh_kick();
    } else if (r.init_err == ESP_ERR_INVALID_STATE) {
        /* Thread 停了 / 暂停了 / 正在重启，或目标口断了：状态已经变了，相应的启动
           或网络事件会再踢跟随，这里重试只会撞同一堵墙 */
    } else {
        backbone_follow_retry(esp_err_to_name(r.init_err));
    }
    vTaskDelete(NULL);
}

/* 跑在 esp_timer 任务里：只做不长时间阻塞的判断（网口句柄已缓存，见 pick_backbone），
   真要换口才建任务。 */
static void backbone_follow_timer_cb(void *arg)
{
    if (!s_running || thread_br_radio_is_paused()) return;
    /* 开机的 border_router_init 由 ot_br_init_task 做，没做完就别插手 —— 否则两边各
       init 一次，后一次必然 INVALID_STATE。它做完会自己再踢一次，这里直接放弃即可。 */
    if (!s_br_init_done) return;

    esp_netif_t *want = pick_backbone(NULL);
    if (!want) return;                       /* 没有可用上行：边界路由本来也没意义，等下一次事件 */
    if (s_border_routing && want == s_backbone_netif) return;   /* 已经在对的口上 */

    /* 与测试接口互斥：同一时刻只允许一个切换在跑。忙就过 5 s 再看，不丢这次变化。 */
    bool mine = false;
    portENTER_CRITICAL(&s_bb_mux);
    if (!s_bb_busy) { s_bb_busy = true; mine = true; }
    portEXIT_CRITICAL(&s_bb_mux);
    if (!mine) {
        backbone_follow_arm();               /* 不算失败，不占重试次数 */
        return;
    }
    if (xTaskCreate(backbone_follow_task, "ot_bb_follow", 4096, want, 4, NULL) != pdPASS) {
        s_bb_busy = false;
        backbone_follow_retry("建跟随任务失败（内部内存不足）");
    }
}

/* 网络事件 / 开机初始化做完时调用：新的一轮，重试计数清零，从头计时 5 s。立刻返回。 */
static void backbone_follow_kick(void)
{
    s_follow_retries = 0;
    backbone_follow_arm();
}

/* 这里原来是 thread_br_restart()（看门狗 L3：stop → start 整栈重启）。2026-10-07 实验 2 证明
   正常态也不可用（一处已绕过，还会 panic，记录在 components/webui/otbr_api.c 末尾），按 T9.18
   方案 A 删掉：Thread 的阶梯改成 L2 → L4。别加回来，除非先把那几件 IDF 侧缺陷都解决。 */

/* 扫描结束时把这一轮的增量存成快照。计数器在扫描开始时已清零，所以直接读到的
   就是增量。整体写在临界区里，读者（/api/v1/diag/ed）才不会看到写了一半的结构。 */
static void record_scan_diag(uint32_t cb_base, int64_t t0_us,
                             uint16_t duration_ms, bool completed_naturally)
{
    thread_br_scan_diag_t d = {
        .valid         = true,
        .ed_scan_coex  = ieee802154_ll_get_ed_scan_coex_cnt(),
        .ed_abort      = ieee802154_ll_get_ed_abort_cnt(),
        .rx_abort_coex = ieee802154_ll_get_rx_abort_coex_cnt(),
        .cca_fail      = ieee802154_ll_get_cca_fail_cnt(),
        .ed_failed_cb  = s_ed_failed_cb - cb_base,
        .valid_mask    = s_escan.valid,
        .valid_count   = (uint8_t)__builtin_popcount(s_escan.valid),
        .duration_ms   = duration_ms,
        .elapsed_ms    = (uint32_t)((esp_timer_get_time() - t0_us) / 1000),
        .completed_naturally = completed_naturally,
    };

    portENTER_CRITICAL(&s_diag_mux);
    s_scan_diag = d;
    portEXIT_CRITICAL(&s_diag_mux);

    ESP_LOGI(TAG, "扫描诊断：有效信道 %u/16，ed_scan_coex=%u ed_abort=%u "
                  "cca_fail=%u ed_failed_cb=%u 耗时 %ums",
             d.valid_count, (unsigned)d.ed_scan_coex, (unsigned)d.ed_abort,
             (unsigned)d.cca_fail, (unsigned)d.ed_failed_cb,
             (unsigned)d.elapsed_ms);
}

/* ---- 主动扫描（beacon）：纯观测，见 thread_br.h 的说明 ---- */
static struct {
    thread_br_beacon_scan_t *out;
    volatile bool done;
} s_ascan;

/* PCAP 原始帧回调。在 OT 任务上下文，只做定长拷贝，不阻塞。
   目的：绕过 OT 扫描结果解析器"源地址必须是扩展地址"的过滤（见 thread_br.h），
   把 Zigbee 那种短地址 beacon 也抓下来。 */
static void pcap_cb(const otRadioFrame *f, bool is_tx, void *ctx)
{
    (void)ctx;
    thread_br_beacon_scan_t *o = s_ascan.out;
    if (!o || !f || !f->mPsdu || f->mLength < 1) return;
    if (is_tx) { o->pcap_tx_total++; return; }

    const uint8_t *p = f->mPsdu;
    o->pcap_rx_total++;
    switch (p[0] & 0x07) {
    case 0: o->pcap_rx_beacon++; break;
    case 1: o->pcap_rx_data++;   break;
    case 2: o->pcap_rx_ack++;    break;
    case 3: o->pcap_rx_cmd++;    break;
    default: break;
    }
    if ((p[0] & 0x07) != 0x00) return;               /* 下面只解析 beacon */
    if (f->mLength < 5) return;

    uint8_t dst_mode = (p[1] >> 2) & 0x03;
    uint8_t src_mode = (p[1] >> 6) & 0x03;
    if (src_mode != 0x02 && src_mode != 0x03) return; /* 没有源地址就没意义 */

    /* FCF(2) + seq(1)，随后按寻址模式跳过目的字段。beacon 通常 dst_mode=0。 */
    int off = 3;
    if (dst_mode != 0) {
        off += 2;                                     /* dst PAN */
        off += (dst_mode == 0x03) ? 8 : 2;
    }
    if (off + 2 > f->mLength) return;
    uint16_t pan = (uint16_t)(p[off] | (p[off + 1] << 8));
    off += 2;

    int alen = (src_mode == 0x03) ? 8 : 2;
    if (off + alen > f->mLength) return;

    if (o->raw_count >= THREAD_BR_BEACON_MAX) { o->raw_dropped++; return; }
    thread_br_raw_beacon_t *r = &o->raw[o->raw_count++];
    memset(r, 0, sizeof(*r));
    r->channel      = f->mChannel;
    r->rssi         = f->mInfo.mRxInfo.mRssi;
    r->lqi          = f->mInfo.mRxInfo.mLqi;
    r->pan_id       = pan;
    r->src_extended = (src_mode == 0x03);
    memcpy(r->src_addr, p + off, (size_t)alen);
    r->hdr_len = (uint8_t)(f->mLength < sizeof(r->hdr) ? f->mLength : sizeof(r->hdr));
    memcpy(r->hdr, p, r->hdr_len);
}

/* 在 OT 任务里跑。只搬运，不阻塞。aResult == NULL 表示扫描结束。 */
static void active_scan_cb(otActiveScanResult *r, void *ctx)
{
    (void)ctx;
    if (!r) { s_ascan.done = true; return; }
    thread_br_beacon_scan_t *o = s_ascan.out;
    if (!o || o->count >= THREAD_BR_BEACON_MAX) return;

    thread_br_beacon_t *n = &o->nets[o->count++];
    n->pan_id  = r->mPanId;
    n->channel = r->mChannel;
    n->rssi    = r->mRssi;
    n->lqi     = r->mLqi;
    memcpy(n->ext_addr, r->mExtAddress.m8, sizeof(n->ext_addr));
}

esp_err_t thread_br_active_scan(uint16_t dwell_ms, uint32_t channel_mask,
                                thread_br_beacon_scan_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!thread_br_is_running()) return ESP_ERR_INVALID_STATE;

    if (dwell_ms == 0)   dwell_ms = 200;      /* beacon 要等应答，比 ED 长得多 */
    if (dwell_ms > 1000) dwell_ms = 1000;
    if (channel_mask == 0) channel_mask = 0x07FFF800;   /* ch11..26 */
    out->dwell_ms = dwell_ms;

    s_ascan.out  = out;
    s_ascan.done = false;

    /* 有超时而不是 portMAX_DELAY：调用方是单任务 httpd。 */
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(2000))) {
        s_ascan.out = NULL;
        ESP_LOGW(TAG, "取 OT 锁超时，放弃主动扫描");
        return ESP_ERR_TIMEOUT;
    }
    otInstance *inst = esp_openthread_get_instance();
    if (inst) otLinkSetPcapCallback(inst, pcap_cb, NULL);   /* 先挂，再扫 */
    otError oe = inst ? otLinkActiveScan(inst, channel_mask, dwell_ms,
                                          active_scan_cb, NULL)
                      : OT_ERROR_INVALID_STATE;
    esp_openthread_lock_release();

    if (oe != OT_ERROR_NONE) {
        s_ascan.out = NULL;
        ESP_LOGW(TAG, "otLinkActiveScan failed: %d", oe);
        return (oe == OT_ERROR_BUSY) ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    }

    /* 预算 = 每信道驻留 × 信道数 + 调度余量，并硬钳上限：单任务 httpd 等在
       这里，超时会让整个 WebUI 失去响应。 */
    uint32_t n_ch = (uint32_t)__builtin_popcount(channel_mask);
    uint32_t budget = n_ch * dwell_ms + 1500;
    if (budget > THREAD_BR_SCAN_MAX_TOTAL_MS) budget = THREAD_BR_SCAN_MAX_TOTAL_MS;

    int64_t t0 = esp_timer_get_time();
    uint32_t waited = 0;
    while (!s_ascan.done && waited < budget) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
    out->completed  = s_ascan.done;
    out->elapsed_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    /* 摘钩子要在清 s_ascan.out 之前：回调只认 out 指针，顺序反了会有一个
       极短的窗口让回调写进已经被调用方拿走的结构体。 */
    if (esp_openthread_lock_acquire(pdMS_TO_TICKS(1000))) {
        otInstance *i2 = esp_openthread_get_instance();
        if (i2) otLinkSetPcapCallback(i2, NULL, NULL);
        esp_openthread_lock_release();
    }
    s_ascan.out = NULL;

    ESP_LOGI(TAG, "主动扫描：OT 报 %u 个 / 原始抓到 %u 个 beacon"
                  "（%u 信道 × %ums，%s，耗时 %ums）",
             out->count, out->raw_count, (unsigned)n_ch, dwell_ms,
             out->completed ? "自然结束" : "预算耗尽", (unsigned)out->elapsed_ms);

    return out->completed ? ESP_OK : ESP_ERR_TIMEOUT;
}


/* ---- 自家协调器 PAN 探测 + 声明值过期检测（见 thread_br.h） ---- */

/* 800ms：实测 300ms 抓不到、800ms 稳定抓到。Zigbee 协调器的 beacon 要等它的
   jitter 窗口（SL_ZIGBEE_EZSP_CONFIG_BEACON_JITTER_DURATION）。 */
#define ZB_PROBE_DWELL_MS   800
/* 自家协调器在同板几厘米外，实测 -20/-21 dBm；邻居实测 -79 / -81 / -97。
   门限取 -50：宁可报"识别不出自己的网络"，也不能把邻居当成自己 ——
   2026-08-12 实测踩过：不设门限时，一次抓到 -97 的邻居就被当成自家 PAN 存下，
   之后所有过期检测都会盯着别人家的网络。 */
#define ZB_OWN_MIN_RSSI     (-50)
/* 自家 beacon 不是每轮都抓得到（实测同样 800ms，有时有有时没有），多扫几轮 */
#define ZB_PROBE_ROUNDS     3
/* 全频段那一趟必须让 16×dwell 落在 THREAD_BR_SCAN_MAX_TOTAL_MS 之内 */
#define ZB_SWEEP_DWELL_MS   450
#define ZB_CHECK_INTERVAL_S (6 * 3600)
/* 连续多少轮一个 beacon 都没有，才下 not_found 的结论。单轮落空不是信息 ——
   C5 的 802.15.4 与 WiFi 共享射频并由共存仲裁器调度，驻留窗口被抢占时漏掉
   beacon 很容易：同一个 PAN 曾以 -22 dBm 被扫到，之后在设备全都正常上报时
   照样漏（docs/guide/Zigbee全掉线排查.md 8.1）。
   落空期间按 ZB_RETRY_INTERVAL_S 快速重试而不是干等一个 6 小时周期，所以
   "协调器真的停了"仍然在十几分钟内报出来，只是不再由单次落空说了算。 */
#define ZB_MISS_STREAK       3
#define ZB_RETRY_INTERVAL_S  300
/* not_found 成立之后的复查节奏。不能退回 6 小时：结论一旦挂上，就算是误报也要
   挂满六小时。Zigbee 页有「重新测一次」按钮可以手动催（zigbee.js _renderCheck），
   但不能指望用户盯着看。30 分钟一轮 ≈ 10s/1800s = 0.55% 的射频占用，换"最多
   半小时自愈"。真的关了 Zigbee 就一直是这个节奏，所以别再往下调。 */
#define ZB_NOTFOUND_INTERVAL_S (30 * 60)

static zigbee_channel_check_t s_zb_check;
/* 连续落空轮数。达到 ZB_MISS_STREAK 之前不覆盖 s_zb_check —— 没测到与
   "测到了，没有"是两回事，前者不该把上一轮的结论抹掉。 */
static uint8_t s_zb_miss_streak;

esp_err_t thread_br_probe_pan(uint8_t channel, uint16_t *pan_out, int8_t *rssi_out)
{
    if (channel < 11 || channel > 26) return ESP_ERR_INVALID_ARG;

    uint16_t best_pan = 0;
    int8_t   best_rssi = -128;
    uint8_t  seen = 0;

    for (int round = 0; round < ZB_PROBE_ROUNDS; round++) {
        thread_br_beacon_scan_t r;
        thread_br_active_scan(ZB_PROBE_DWELL_MS, 1u << channel, &r);
        seen += r.raw_count;
        for (uint8_t i = 0; i < r.raw_count; i++) {
            if (r.raw[i].rssi > best_rssi) {
                best_rssi = r.raw[i].rssi;
                best_pan  = r.raw[i].pan_id;
            }
        }
        /* 已经强到只可能是同板那颗，不必再扫 */
        if (best_rssi >= ZB_OWN_MIN_RSSI) break;
    }

    if (best_rssi == -128) {
        ESP_LOGI(TAG, "ch%u 上 %d 轮都没抓到 beacon", channel, ZB_PROBE_ROUNDS);
        return ESP_ERR_NOT_FOUND;
    }
    if (best_rssi < ZB_OWN_MIN_RSSI) {
        /* 抓到了，但弱得不可能是同板那颗 —— 是邻居。不能拿它当自家 PAN。 */
        ESP_LOGW(TAG, "ch%u 最强 beacon 只有 %d dBm（PAN 0x%04X），弱于门限 %d，"
                      "判定为邻居，不作为自家网络", channel, best_rssi, best_pan,
                 ZB_OWN_MIN_RSSI);
        return ESP_ERR_NOT_FOUND;
    }

    if (pan_out)  *pan_out  = best_pan;
    if (rssi_out) *rssi_out = best_rssi;
    ESP_LOGI(TAG, "ch%u 认定自家网络：PAN 0x%04X，%d dBm（%d 轮共见 %u 个 beacon）",
             channel, best_pan, best_rssi, ZB_PROBE_ROUNDS, seen);
    return ESP_OK;
}

esp_err_t thread_br_find_strongest(uint8_t *ch_out, uint16_t *pan_out,
                                   int8_t *rssi_out)
{
    thread_br_beacon_scan_t r;
    thread_br_active_scan(ZB_SWEEP_DWELL_MS, 0, &r);

    int best = -1;
    for (uint8_t i = 0; i < r.raw_count; i++) {
        if (best < 0 || r.raw[i].rssi > r.raw[best].rssi) best = i;
    }
    /* 同样要门限：提示"最强的网络在 CH20"如果指的是邻居，只会把用户带偏。 */
    if (best < 0 || r.raw[best].rssi < ZB_OWN_MIN_RSSI) return ESP_ERR_NOT_FOUND;

    if (ch_out)   *ch_out   = r.raw[best].channel;
    if (pan_out)  *pan_out  = r.raw[best].pan_id;
    if (rssi_out) *rssi_out = r.raw[best].rssi;
    return ESP_OK;
}

void thread_br_get_zigbee_check(zigbee_channel_check_t *out)
{
    if (out) *out = s_zb_check;
}

void thread_br_zigbee_check_seed(uint8_t declared_channel, bool pan_known,
                                 uint16_t pan_id, int8_t rssi)
{
    if (!pan_known) {
        /* 声明变了但没探到 PAN：旧结论针对的是旧声明，留着只会指着错的网络。
           作废让 /api/v1/status 如实回 null（"还没测过"），而不是一个陈旧的
           ok 或 not_found。 */
        memset(&s_zb_check, 0, sizeof(s_zb_check));
        s_zb_miss_streak = 0;
        return;
    }
    zigbee_channel_check_t c = {0};
    c.valid            = true;
    c.declared_channel = declared_channel;
    c.pan_id           = pan_id;
    c.rssi             = rssi;
    c.checked_at       = (uint32_t)(esp_timer_get_time() / 1000000);
    s_zb_check      = c;
    s_zb_miss_streak = 0;
    ESP_LOGI(TAG, "过期检测结论由实测直接刷新：ch%u 上 PAN 0x%04X（%d dBm）",
             declared_channel, pan_id, rssi);
}

/* 在一次扫描结果里找本 PAN。返回信道，找不到返回 0。 */
static uint8_t find_pan(const thread_br_beacon_scan_t *r, uint16_t pan, int8_t *rssi)
{
    int best = -1;
    for (uint8_t i = 0; i < r->raw_count; i++) {
        if (r->raw[i].pan_id != pan) continue;
        if (best < 0 || r->raw[i].rssi > r->raw[best].rssi) best = i;
    }
    if (best < 0) return 0;
    if (rssi) *rssi = r->raw[best].rssi;
    return r->raw[best].channel;
}

/* 窄扫：在声明的信道上多轮找本 PAN，命中返回信道，全落空返回 0。
   ⚠️ 必须和 thread_br_probe_pan() 一样多轮 —— 自家 beacon 要等协调器的 jitter
   窗口，同样 800ms 有时有有时没有（见 ZB_PROBE_ROUNDS 处的实测注释）。
   2026-09-15 之前这里只扫一轮，灵敏度**严格低于**声明信道时用的那次探测，
   是 not_found 误报的最大来源。命中就立刻收手，稳态开销仍是一轮。 */
static uint8_t narrow_find_pan(uint16_t pan, uint8_t channel, int8_t *rssi)
{
    for (int round = 0; round < ZB_PROBE_ROUNDS; round++) {
        thread_br_beacon_scan_t r;
        thread_br_active_scan(ZB_PROBE_DWELL_MS, 1u << channel, &r);
        uint8_t found = find_pan(&r, pan, rssi);
        if (found != 0) return found;
    }
    return 0;
}

/* 返回下一轮该等多久（秒）—— 三种结局的复查节奏完全不同，见各个 INTERVAL 常量。 */
static uint32_t zigbee_check_once(void)
{
    const zigbee_config_t *zb = config_mgr_get_zigbee();
    if (!zb || !zb->channel_declared || !zb->pan_known) return ZB_CHECK_INTERVAL_S;
    if (!thread_br_is_running()) return ZB_CHECK_INTERVAL_S;


    zigbee_channel_check_t c = {0};
    c.declared_channel = zb->channel;
    c.pan_id           = zb->pan_id;
    c.checked_at       = (uint32_t)(esp_timer_get_time() / 1000000);

    /* 先窄：只扫声明的信道。没搬家时这就是全部开销（约 1s）。 */
    if (narrow_find_pan(zb->pan_id, zb->channel, &c.rssi) != 0) {
        c.valid = true;                    /* 还在原地，一切正常 */
        s_zb_check = c;
        s_zb_miss_streak = 0;
        return ZB_CHECK_INTERVAL_S;
    }

    /* 再宽：全频段找它搬到哪儿了。罕见路径。 */
    ESP_LOGI(TAG, "PAN 0x%04X 不在声明的 ch%u 上，全频段查找…",
             zb->pan_id, zb->channel);
    thread_br_beacon_scan_t r;
    thread_br_active_scan(ZB_SWEEP_DWELL_MS, 0, &r);
    uint8_t found = find_pan(&r, zb->pan_id, &c.rssi);

    if (found != 0) {
        c.valid = true;
        if (found != zb->channel) {
            c.mismatch = true;
            c.detected_channel = found;
            ESP_LOGW(TAG, "Zigbee 信道声明值已过期：声明 ch%u，实测 PAN 0x%04X "
                          "在 ch%u（%d dBm）", zb->channel, zb->pan_id, found, c.rssi);
        }
        s_zb_check = c;
        s_zb_miss_streak = 0;
        return ZB_CHECK_INTERVAL_S;
    }

    /* 哪儿都没有。**先不下结论**：单轮落空更可能是射频被 WiFi 抢占没听见。
       不覆盖 s_zb_check —— 抹掉上一轮的 ok 换成一个没有信息量的值是净损失。 */
    if (++s_zb_miss_streak < ZB_MISS_STREAK) {
        ESP_LOGI(TAG, "PAN 0x%04X 这轮没扫到（连续 %u/%u），%ds 后重试，暂不下结论",
                 zb->pan_id, s_zb_miss_streak, ZB_MISS_STREAK, ZB_RETRY_INTERVAL_S);
        return ZB_RETRY_INTERVAL_S;
    }

    c.valid = true;
    c.not_found = true;
    ESP_LOGW(TAG, "连续 %u 轮全频段都没找到 PAN 0x%04X —— Zigbee 网络关了、"
                  "改过 PAN、或信号太弱", s_zb_miss_streak, zb->pan_id);
    s_zb_check = c;
    return ZB_NOTFOUND_INTERVAL_S;
}

/* T2.20（2026-09-14）：不能写 pdMS_TO_TICKS(ZB_CHECK_INTERVAL_S * 1000)。
   那个宏先乘 configTICK_RATE_HZ 再除 1000，21,600,000 × 1000 在 32 位里溢出，
   算出来是 125 s —— "6 小时一次"的检查整晚每 126 s 跑一趟，PAN 找不到时还
   带一次 7.5 s 的全频段扫，每两分钟占 802.15.4 射频 8.4 s。
   1000 Hz 下宏的安全上限是 4,294 s，所以按秒切片睡。 */
static void zb_sleep_seconds(uint32_t secs)
{
    for (uint32_t i = 0; i < secs; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* 独立任务而不是挂在已有定时器上：一次检测要占用射频约 1s（搬家时约 8s），
   放进 2s 的 child_stats 定时器或看门狗任务里都会撞它们的期限。 */
static void zigbee_check_task(void *arg)
{
    (void)arg;
    /* 开机噪声（WiFi 关联、Thread 起栈、主机重连）正压在射频上，这时候扫大概率
       落空。落空不再直接变成结论，而是走 ZB_RETRY_INTERVAL_S 重试，所以这个
       起步延时保持 60s 就够，不必为了躲噪声把首次检测推得很晚。 */
    vTaskDelay(pdMS_TO_TICKS(60 * 1000));
    while (1) {
        zb_sleep_seconds(zigbee_check_once());
    }
}

/* 跑一轮 16 信道扫描。返回 false = 这一轮没能自然走完（已强行收尾）。 */
static bool run_one_scan_pass(uint16_t dwell_ms)
{
    s_escan.done = false;

    /* 每轮都重新确认 Thread 还活着：扫描中途 Thread 可能被停掉（SoftAP 配网暂停、stop），
       OT 实例若正在析构，继续往里捅就是访问已释放对象。 */
    if (!thread_br_is_running()) return false;

    /* 有超时而不是 portMAX_DELAY：这个函数被单任务 httpd 调用，
       OT 锁若被长时间占住（如 BR 正在拆除），不能把整个 WebUI 拖死。 */
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(2000))) {
        ESP_LOGW(TAG, "取 OT 锁超时，放弃本轮扫描");
        return false;
    }
    otInstance *inst = esp_openthread_get_instance();
    otError oe = inst ? otLinkEnergyScan(inst, 0x07FFF800 /* ch11..26 */,
                                         dwell_ms, energy_scan_cb, NULL)
                      : OT_ERROR_INVALID_STATE;
    esp_openthread_lock_release();

    if (oe != OT_ERROR_NONE) {
        /* OT_ERROR_BUSY = 上一轮没收干净（正常不该出现，因为我们等到 done 才返回）。
           不强行收尾：此时 OT 的扫描不是我们发起的，乱推进会踢掉别人的操作。 */
        ESP_LOGW(TAG, "otLinkEnergyScan failed: %d", oe);
        return false;
    }

    /* 有了 ed_failed 钩子，被拒的信道是**即时**完成的（微秒级），不再靠超时；
       所以预算只需覆盖真正测到的信道 + OT 调度开销。1ms×16 实测约 50ms。 */
    uint32_t timeout_ms = 16u * dwell_ms + 1500;
    uint32_t waited = 0;
    while (!s_escan.done && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    if (s_escan.done) return true;

    /* 兜底：钩子没起作用（比如中断位被别处改掉）时仍要把 MAC 放出来。
       ⚠️ 先摘钩子再强行收尾：否则在途的 ED 中断和 force_finish 会各推进一次，
       把 OT 的扫描状态机推过头。 */
    s_ed_hook_armed = false;
    force_finish_energy_scan(dwell_ms);
    return false;
}

/* 多轮短驻留采样，逐信道 max-hold。
 *
 * 为什么是这个形状（2026-08-11 在板实测，见 /api/v1/diag/ed）：
 * 共存仲裁器按**连续独占时长**决定放不放行，与优先级无关 ——
 *   驻留 100ms/50ms → 16/16 全被拒，一个读数都拿不到
 *   驻留  20ms      → 4/16
 *   驻留   1ms      → 15~16/16
 * CCA（128µs 的 ED）从来没被拒过，这是同一块板上的对照。
 * 所以正确做法不是"重试"（同长度的重试必然同样被拒，拒绝是确定性的），
 * 而是把窗口切短、多采几次取最大值：信息量接近一次长窗口，
 * 每次采样却短到能赢下仲裁。
 */
esp_err_t thread_br_energy_scan_ex(uint16_t dwell_ms, uint8_t passes,
                                   int8_t energy[16], uint16_t *valid_mask)
{
    if (!energy) return ESP_ERR_INVALID_ARG;
    /* thread_br_is_running() 而不是 s_running：射频暂停时（SoftAP 配网中）
       OT 实例还在，但 802.15.4 已 disable，扫描只会等到超时拿一片 -100。
       配网态本来就该拒绝，别让调用方以为扫到了"干净的频谱"。 */
    if (!thread_br_is_running()) return ESP_ERR_INVALID_STATE;

    /* 上界 1000：otPlatRadioEnergyScan (esp_openthread_radio.c:429) 把 ms 换算成
       symbol 后塞进 uint16_t 的 ieee802154_ll_set_ed_duration()，>1048ms 静默截断。
       下界 1：dwell=0 会走 OT 的同步 GetRssi 路径，那是最近一次收帧的锁存值。 */
    if (dwell_ms == 0)    dwell_ms = THREAD_BR_SCAN_DEFAULT_DWELL_MS;
    if (dwell_ms > 1000)  dwell_ms = 1000;
    if (passes == 0)      passes = THREAD_BR_SCAN_DEFAULT_PASSES;
    if (passes > THREAD_BR_SCAN_MAX_PASSES) passes = THREAD_BR_SCAN_MAX_PASSES;

    /* ⚠️ 光钳两个参数不够，要钳**乘积**。dwell 与 passes 各自都合法的
       1000ms × 16 轮 = 16 信道 × 16 轮 × 1s ≈ 256s，而本函数是被
       **单任务 httpd** 同步调用的（rest_api.c 的 handle_spectrum_scan），
       整个 WebUI 会失去响应几分钟，还会撞穿 httpd 自己的看门狗。
       所以按总预算反推轮数，宁可少扫几轮也不能超时。 */
    uint32_t per_pass_ms = 16u * dwell_ms + THREAD_BR_SCAN_PASS_GAP_MS;
    uint32_t max_passes  = THREAD_BR_SCAN_MAX_TOTAL_MS / (per_pass_ms ? per_pass_ms : 1);
    if (max_passes == 0) max_passes = 1;          /* 至少跑一轮 */
    if (passes > max_passes) {
        ESP_LOGW(TAG, "扫描轮数 %u → %u（总时长预算 %ums，每轮约 %ums）",
                 passes, (unsigned)max_passes,
                 (unsigned)THREAD_BR_SCAN_MAX_TOTAL_MS, (unsigned)per_pass_ms);
        passes = (uint8_t)max_passes;
    }

    for (int i = 0; i < 16; i++) energy[i] = -100;
    s_escan.energy = energy;
    s_escan.valid  = 0;

    uint32_t cb_base = s_ed_failed_cb;
    int64_t  t0_us   = esp_timer_get_time();
    ieee802154_ll_clear_debug_cnt(ED_CNT_BITS);

    /* 扫描窗口：使能 ED 失败中断 + 打开转发钩子。两者缺一不可 ——
       没有中断则回调不触发，没有钩子则回调不转发。 */
    uint32_t irq_prev = ed_irq_arm();
    s_ed_hook_armed = true;

    uint8_t done_passes = 0;
    for (uint8_t p = 0; p < passes; p++) {
        if (!run_one_scan_pass(dwell_ms)) break;   /* 出错就别再往下捅 */
        done_passes++;
        /* 即使第一轮就 16/16 也继续跑完：多轮的意义在 max-hold 抓突发流量，
           不只是补齐被拒的信道。 */
        /* 让出射频，别把 Thread 的正常收发压死。 */
        vTaskDelay(pdMS_TO_TICKS(THREAD_BR_SCAN_PASS_GAP_MS));
    }

    s_ed_hook_armed = false;
    ed_irq_restore(irq_prev);

    uint16_t valid = s_escan.valid;
    s_escan.energy = NULL;
    if (valid_mask) *valid_mask = valid;

    record_scan_diag(cb_base, t0_us, dwell_ms, done_passes == passes);
    ESP_LOGI(TAG, "能量扫描：%u/16 信道有效（%u×%ums，完成 %u 轮）",
             __builtin_popcount(valid), passes, dwell_ms, done_passes);

    /* 一个信道都没测到才算失败。部分结果是有价值的，交给上层用 valid_mask
       区分，不再像以前那样整批丢掉。 */
    return valid ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* 旧接口：保留给不关心 valid_mask 的调用方。参数语义变了 ——
   历史上它是"每信道驻留 N ms 扫一轮"，现在 N 只作为总时长预算的参考，
   实际用短驻留多轮去逼近，因为长驻留在本平台上必然被共存拒绝。 */
esp_err_t thread_br_energy_scan(uint16_t duration_per_ch_ms, int8_t energy[16])
{
    uint16_t valid = 0;
    esp_err_t err = thread_br_energy_scan_ex(THREAD_BR_SCAN_DEFAULT_DWELL_MS,
                                             THREAD_BR_SCAN_DEFAULT_PASSES,
                                             energy, &valid);
    (void)duration_per_ch_ms;
    /* 老调用方不认识 valid_mask，会把 -100 当测量值。只有全部信道都测到时
       才对它们报成功，否则宁可报错也不给出半真半假的一片数据。 */
    if (err == ESP_OK && valid != 0xFFFF) return ESP_ERR_TIMEOUT;
    return err;
}

#else /* CONFIG_OPENTHREAD_ENABLED not set */

/* docs/14：Thread 编译掉时 REST 仍要能回 thread_tx_dbm，给 null（INT8_MAX） */
int8_t thread_br_region_tx_dbm(const char *cc) { (void)cc; return INT8_MAX; }
int8_t thread_br_get_tx_power_dbm(void) { return INT8_MAX; }

esp_err_t thread_br_init(void)
{
    const thread_config_t *cfg = config_mgr_get_thread();
    if (cfg->enabled) {
        ESP_LOGW(TAG, "Thread BR requested but OpenThread not compiled in");
    } else {
        ESP_LOGI(TAG, "Thread BR disabled");
    }
    return ESP_OK;
}

esp_err_t thread_br_start(void)  { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t thread_br_stop(void)   { return ESP_OK; }
esp_err_t thread_br_switch_backbone(const char *to, uint32_t timeout_ms,
                                    thread_br_backbone_switch_t *out)
{
    (void)to; (void)timeout_ms;
    if (out) memset(out, 0, sizeof(*out));
    return ESP_ERR_NOT_SUPPORTED;
}
const char *thread_br_backbone_name(void) { return "none"; }
bool      thread_br_is_running(void) { return false; }
#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
bool      thread_br_get_ack_now(uint32_t *ack_req, uint32_t *acked) { (void)ack_req; (void)acked; return false; }
bool      thread_br_get_ack_window(uint32_t *r, uint32_t *a, uint32_t *s) { (void)r; (void)a; (void)s; return false; }
#endif
void      thread_br_lower_priority(void) { }
uint8_t   thread_br_get_configured_channel(void) { return config_mgr_get_thread()->channel; }
/* OpenThread 没编进来就没有 otLink 可读。诚实地说"没有实际信道"，
   而不是把配置值伪装成实测值 —— 那正是这次要修掉的病。 */
esp_err_t thread_br_get_active_channel(uint8_t *out) { (void)out; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t thread_br_energy_scan(uint16_t duration_per_ch_ms, int8_t energy[16])
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t thread_br_energy_scan_ex(uint16_t dwell_ms, uint8_t passes,
                                   int8_t energy[16], uint16_t *valid_mask)
{
    if (valid_mask) *valid_mask = 0;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t thread_br_active_scan(uint16_t dwell_ms, uint32_t channel_mask,
                                thread_br_beacon_scan_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t thread_br_probe_pan(uint8_t channel, uint16_t *pan_out, int8_t *rssi_out)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t thread_br_find_strongest(uint8_t *ch_out, uint16_t *pan_out,
                                   int8_t *rssi_out)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void thread_br_get_zigbee_check(zigbee_channel_check_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
}

void thread_br_zigbee_check_seed(uint8_t declared_channel, bool pan_known,
                                 uint16_t pan_id, int8_t rssi)
{
    (void)declared_channel; (void)pan_known; (void)pan_id; (void)rssi;
}

/* 诊断桩：非 OpenThread 构建里 802.15.4 由 spectrum 直接驱动，这套计数器
   与 ED 中断的分析不成立，一律报未就绪而不是给出会被当真的 0。 */
esp_err_t thread_br_ed_diag_read(thread_br_ed_diag_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    return ESP_ERR_NOT_SUPPORTED;
}

void thread_br_scan_diag_read(thread_br_scan_diag_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
}

esp_err_t thread_br_ed_diag_arm_irq(bool arm)
{
    (void)arm;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
