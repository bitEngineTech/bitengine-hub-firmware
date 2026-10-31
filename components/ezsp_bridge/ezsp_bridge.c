#include "ezsp_bridge.h"
#include "ncp_version.h"
#include "ncp_cache.h"
#include "uart_bridge.h"
#if !CONFIG_BITENGINE_ENABLE_ZHUB
#include "tcp_server.h"         /* 直连型不编这两个（CMakeLists，架构 5.8 R-2） */
#include "usb_bridge.h"
#endif
#include "bridge_stats.h"
#include "mdns_svc.h"
#include "config_mgr.h"        /* 启动时读回 sys.bridge_mode */

#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <string.h>
#include "esp_system.h"
#include <stdatomic.h>
#include "psram_task.h"

static const char *TAG = "ezsp_bridge";

ESP_EVENT_DEFINE_BASE(BRIDGE_EVENT);

static atomic_int    s_state = BRIDGE_STATE_INIT;
#if CONFIG_BITENGINE_ENABLE_ZHUB
static bridge_mode_t s_mode  = BRIDGE_MODE_DISABLED;   /* 直连型没有桥；init 失败提前返回时也不能停在 LAN */
#else
static bridge_mode_t s_mode  = BRIDGE_MODE_LAN;
#endif

static esp_err_t bridge_mode_start(bridge_mode_t mode);

#if !CONFIG_BITENGINE_ENABLE_ZHUB   /* 下面两个只为 :6638 的客户端服务；直连型没有桥 */
/* 记下"这台网关曾经有过协调器"，粘性、写一次。
 *
 * 用来区分两件完全不同的事：**从没配过 Zigbee**（只用 Thread/Matter，或还没装
 * ZHA）与**以前好好的、现在连不上了**。只有后者是故障。没有这个区分，纯 Thread
 * 用户会一直挂着一条"没有协调器"的红色横幅，而设备完全正常 —— 报一个用户无法
 * 消除、也不该消除的警，比不报更糟。
 *
 * 跑在事件循环任务里而不是 tcp_server 的 accept 路径上：那里持着 s_client_mutex，
 * 在锁内调 config_mgr（自己有锁 + 可能落 NVS）是自找死锁。
 *
 * 已经置位就直接返回，所以整个生命周期只写一次 NVS，不存在磨损问题。
 */
static void on_client_connected(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    const zigbee_config_t *cur = config_mgr_get_zigbee();
    if (!cur || cur->host_ever_seen) return;

    zigbee_config_t zb = *cur;
    zb.host_ever_seen = true;
    if (config_mgr_set_zigbee(&zb) == ESP_OK) {
        ESP_LOGI(TAG, "首次记录：本网关已有 Zigbee 协调器接入过");
    }
}

/* Post-boot client watchdog: if no ZHA/Z2M client connects within 2 minutes,
   refresh mDNS to help HA rediscover the coordinator. Runs once after boot. */
static void client_watchdog_task(void *arg)
{
    /* Wait 2 minutes for a client to connect */
    for (int i = 0; i < 24; i++) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (bridge_stats_get()->active_clients > 0) {
            ESP_LOGI(TAG, "ZHA/Z2M client connected within %ds of boot", (i + 1) * 5);
            vTaskDelete(NULL);
            return;
        }
    }

    /* No client after 2 minutes — refresh mDNS */
    ESP_LOGW(TAG, "No TCP client after 120s, refreshing mDNS services");
    mdns_svc_refresh();

    /* Keep checking for another 3 minutes with periodic refresh */
    for (int i = 0; i < 6; i++) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        if (bridge_stats_get()->active_clients > 0) {
            ESP_LOGI(TAG, "Client connected after mDNS refresh");
            vTaskDelete(NULL);
            return;
        }
        mdns_svc_refresh();
    }

    ESP_LOGW(TAG, "No client after 5 minutes — ZHA/Z2M may need manual reload");
    vTaskDelete(NULL);
}
#endif /* !CONFIG_BITENGINE_ENABLE_ZHUB */

// ---- Periodic stats logging ----
static void stats_log_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        const bridge_stats_t *st = bridge_stats_get();
#if CONFIG_BITENGINE_ENABLE_ZHUB
        ESP_LOGI(TAG, "Stats: UART rx=%"PRIu32" tx=%"PRIu32" err=%"PRIu32" | up=%"PRIu32"s",
                 st->uart_rx_bytes, st->uart_tx_bytes, st->uart_rx_errors, st->uptime_seconds);
#else
        ESP_LOGI(TAG, "Stats: UART rx=%"PRIu32" tx=%"PRIu32" err=%"PRIu32
                 " | TCP rx=%"PRIu32" tx=%"PRIu32" clients=%d"
                 " | up=%"PRIu32"s",
                 st->uart_rx_bytes, st->uart_tx_bytes, st->uart_rx_errors,
                 st->tcp_rx_bytes, st->tcp_tx_bytes, st->active_clients,
                 st->uptime_seconds);
#endif
    }
}

#if CONFIG_BITENGINE_ENABLE_ZHUB
esp_err_t ezsp_bridge_init_uart_only(void)
#else
esp_err_t ezsp_bridge_init(void)
#endif
{
    bridge_stats_init();

    esp_err_t err = uart_bridge_init();
    if (err != ESP_OK) {
        atomic_store(&s_state, BRIDGE_STATE_ERROR);
        return err;
    }

    /* Read the NCP firmware version while we still provably own the UART: the
       TCP server is not listening yet, so no client can be mid-handshake when
       the query's ASH RST resets the NCP. This is the only race-free window in
       the whole runtime — afterwards a client may attach at any moment, and
       health_monitor deliberately avoids RST in LAN mode for that reason
       (health_monitor.c:72-83). Costs up to ~2.5s only when the MG24 is dead. */
    /* 顺带把网关声明的地区发给 NCP（docs/14 第 7.5 节）：每次开机都发，没设就发空，
       这样 C5 恢复出厂之后 NCP 里不会残留旧的网关声明。 */
    /* ⚠️ 只在整板上电时读（T9.31 ②，2026-10-08）：读要先发 ASH RST，会把 MG24 的协议栈打回
       未初始化、网关自己拉不起来。整板上电时 MG24 也刚上电，没有损失；C5 单独重启（OTA、看门狗
       自愈、网页重启 —— 复位原因都是 SW，非干净复位也会被 main.c 补成 SW）时 MG24 多半还在带网，
       一发就停网，没有协调器回来的话几小时后有设备丢网（10-07 Sengled）。那种开机沿用上次的读数。
       例外三条，照旧读：没有缓存；NCP 不认 XNCP（缓存说明不了地区下发成没成）；缓存里 NCP 回报的
       网关地区与现在的配置不同（恢复出厂、或改地区时没来得及下发）。
       用 esp_reset_reason() 而不是 wd_boot_reason_get()：这里只关心"是不是整板上电"，补重启不影响它。 */
    ncp_version_set_persist_hook(ncp_cache_save);
    const sys_config_t *sys0 = config_mgr_get_sys();
    const char *region = (sys0 && sys0->radio_region[0]) ? sys0->radio_region : "";
    char region_cc[3] = { region[0], region[0] ? region[1] : '\0', '\0' };
    esp_reset_reason_t rst = esp_reset_reason();
    ncp_version_t cached;
    if (rst != ESP_RST_POWERON && ncp_cache_load(&cached) && cached.txp.xncp &&
        strcmp(cached.txp.gw_cc, region_cc) == 0) {
        ncp_version_adopt_cached(&cached);
        ESP_LOGI(TAG, "C5 单独重启（reset=%d）：MG24 可能还在带网，不发 ASH RST，沿用上次的 NCP 读数",
                 (int)rst);
    } else if (ncp_version_query(region) != ESP_OK) {
        ESP_LOGW(TAG, "NCP version unavailable — bridge continues regardless");
    }

#if CONFIG_BITENGINE_ENABLE_ZHUB
    /* 直连型到此为止：UART 起来了、NCP 读数有了，数据面恒为 DISABLED（没有 :6638 / USB 桥，NVS 里 HA 型留下的
       bridge_mode 不认）。DISABLED 下没人读 UART —— MG24 的 NCP 不被问就不说话；健康探测、MG24 升级、
       ncp_resync 都按 DISABLED 设计，照常工作。 */
    s_mode = BRIDGE_MODE_DISABLED;
    atomic_store(&s_state, BRIDGE_STATE_RUNNING);
    if (psram_task_create(stats_log_task, "stats_log", 2048, NULL, 5, NULL) != pdPASS) {
        ESP_LOGW(TAG, "stats task create failed (UART still functional)");
    }
    ESP_LOGI(TAG, "MG24 UART up (direct profile: no :6638 bridge, data plane disabled until zhub)");
    return ESP_OK;
#else
    /* 桥接模式取自 NVS。此前这里恒为 LAN，写入端也从没人调
       config_mgr_set_bridge_mode() —— 用户在 WebUI 切到 USB/DISABLED 只活在 RAM 里，
       任何一次重启（含看门狗自愈重启、按键短按重启）都悄悄回到 LAN。
       写入端在 rest_api 的 handle_mode 里，**不**在 switch_mode 里：OTA / uart_bench
       都会临时切 DISABLED 再恢复，落在那一层会把临时态写进 NVS。 */
    bridge_mode_t want = BRIDGE_MODE_LAN;
    const sys_config_t *sys = config_mgr_get_sys();
    if (sys && sys->bridge_mode <= (uint8_t)BRIDGE_MODE_DISABLED) {
        want = (bridge_mode_t)sys->bridge_mode;
    }

    err = bridge_mode_start(want);
    if (err != ESP_OK && want != BRIDGE_MODE_LAN) {
        /* 记住的模式起不来（例如 USB 没接主机）不该让网关整个失去桥接能力。
           退回 LAN，但**不**改写 NVS —— 用户的选择留着，下次重启再试一次。 */
        ESP_LOGW(TAG, "restoring bridge mode %d failed (%s) — falling back to LAN",
                 want, esp_err_to_name(err));
        want = BRIDGE_MODE_LAN;
        err = bridge_mode_start(want);
    }
    if (err != ESP_OK) {
        atomic_store(&s_state, BRIDGE_STATE_ERROR);
        return err;
    }

    s_mode  = want;
    atomic_store(&s_state, BRIDGE_STATE_RUNNING);

    /* 只注册一次：bridge_mode_start 会被反复调用（模式切换、OTA 前后），
       重复注册会让同一个回调被调用多次。 */
    static bool s_client_evt_registered;
    if (!s_client_evt_registered) {
        if (esp_event_handler_register(BRIDGE_EVENT, BRIDGE_EVENT_CLIENT_CONNECTED,
                                       on_client_connected, NULL) == ESP_OK) {
            s_client_evt_registered = true;
        }
    }

    if (psram_task_create(stats_log_task, "stats_log", 2048, NULL, 5, NULL) != pdPASS ||
        xTaskCreate(client_watchdog_task, "client_wd", 2048, NULL, 5, NULL) != pdPASS) {
        ESP_LOGW(TAG, "aux task create failed (bridge still functional)");
    }

    /* 日志要报**实际**模式：以前恒印 "LAN mode"，现在它能是恢复出来的 USB/DISABLED，
       印错会让"重启后 Z2M 连不上"这类现场问题第一时间指错方向。 */
    if (s_mode == BRIDGE_MODE_LAN) {
        ESP_LOGI(TAG, "EZSP bridge running (LAN mode, port %d)", CONFIG_BITENGINE_TCP_PORT);
    } else {
        ESP_LOGI(TAG, "EZSP bridge running (%s mode, restored from NVS)",
                 s_mode == BRIDGE_MODE_USB ? "USB" : "DISABLED");
    }
    return ESP_OK;
#endif /* CONFIG_BITENGINE_ENABLE_ZHUB */
}

static esp_err_t bridge_mode_start(bridge_mode_t mode)
{
#if CONFIG_BITENGINE_ENABLE_ZHUB
    return mode == BRIDGE_MODE_DISABLED ? ESP_OK : ESP_ERR_NOT_SUPPORTED;   /* 直连型没有桥 */
#else
    if (mode == BRIDGE_MODE_LAN) return tcp_server_start();
    if (mode == BRIDGE_MODE_USB) return usb_bridge_start();
    return ESP_OK;  /* DISABLED: nothing to start */
#endif
}

static void bridge_mode_stop(bridge_mode_t mode)
{
#if CONFIG_BITENGINE_ENABLE_ZHUB
    (void)mode;                     /* 直连型只有 DISABLED，没什么可停 */
#else
    if (mode == BRIDGE_MODE_LAN) tcp_server_stop();
    else if (mode == BRIDGE_MODE_USB) usb_bridge_stop();
#endif
}

esp_err_t ezsp_bridge_switch_mode(bridge_mode_t mode)
{
    if (mode != BRIDGE_MODE_LAN && mode != BRIDGE_MODE_USB &&
        mode != BRIDGE_MODE_DISABLED) {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_BITENGINE_ENABLE_ZHUB
    if (mode != BRIDGE_MODE_DISABLED) return ESP_ERR_NOT_SUPPORTED;    /* 直连型没有 LAN / USB 桥 */
#endif
    if (mode == s_mode) return ESP_OK;

    /* Serialize concurrent switch requests (WebUI + button) */
    static atomic_flag busy = ATOMIC_FLAG_INIT;
    if (atomic_flag_test_and_set(&busy)) return ESP_ERR_INVALID_STATE;

    atomic_store(&s_state, BRIDGE_STATE_DRAINING);

    /* Wait for UART RX buffer to drain (max 3s). The active bridge keeps
       forwarding during DRAINING; with no consumer (DISABLED mode or no
       clients), discard so the new mode starts with a clean stream. */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
    while (xTaskGetTickCount() < deadline) {
        size_t buffered = 0;
        uart_bridge_get_rx_buffered(&buffered);
        if (buffered == 0) break;
        const bridge_stats_t *st = bridge_stats_get();
        if (s_mode == BRIDGE_MODE_DISABLED ||
            (s_mode == BRIDGE_MODE_LAN && st && st->active_clients == 0)) {
            uint8_t scratch[64];
            uart_bridge_read(scratch, sizeof(scratch), 10);
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    atomic_store(&s_state, BRIDGE_STATE_SWITCHING);

    bridge_mode_stop(s_mode);

    esp_err_t err = bridge_mode_start(mode);
    if (err != ESP_OK) {
        /* Try to restore the previous mode rather than leaving the bridge
           dead in ERROR state with no recovery path. */
        ESP_LOGE(TAG, "Mode %d start failed (%s), rolling back to %d",
                 mode, esp_err_to_name(err), s_mode);
        if (bridge_mode_start(s_mode) == ESP_OK) {
            atomic_store(&s_state, BRIDGE_STATE_RUNNING);
        } else {
            ESP_LOGE(TAG, "Rollback failed — bridge in ERROR state");
            atomic_store(&s_state, BRIDGE_STATE_ERROR);
        }
        atomic_flag_clear(&busy);
        return err;
    }

    s_mode = mode;
    atomic_store(&s_state, BRIDGE_STATE_RUNNING);
    atomic_flag_clear(&busy);

    esp_event_post(BRIDGE_EVENT, BRIDGE_EVENT_MODE_CHANGED,
                   &mode, sizeof(mode), 0);
    ESP_LOGI(TAG, "Mode switched to %d", mode);
    return ESP_OK;
}

#if CONFIG_BITENGINE_ENABLE_ZHUB
static esp_err_t (*s_zhub_suspend)(uint32_t timeout_ms);
static void      (*s_zhub_resume)(void);

void ezsp_bridge_set_zhub_hooks(esp_err_t (*suspend)(uint32_t timeout_ms), void (*resume)(void))
{
    s_zhub_suspend = suspend;
    s_zhub_resume = resume;
}

esp_err_t ezsp_bridge_zhub_claim(void)
{
    int expected = BRIDGE_STATE_RUNNING;
    return atomic_compare_exchange_strong(&s_state, &expected, BRIDGE_STATE_ZHUB) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void ezsp_bridge_zhub_release(void)
{
    int expected = BRIDGE_STATE_ZHUB;
    if (!atomic_compare_exchange_strong(&s_state, &expected, BRIDGE_STATE_RUNNING)) {
        ESP_LOGW(TAG, "zhub release ignored (bridge state %d)", expected);
    }
}

void ezsp_bridge_note_uart(uint32_t rx, uint32_t tx, bool tx_short)
{
    bridge_stats_t *st = bridge_stats_get();
    if (!st) return;
    st->uart_rx_bytes += rx;
    st->uart_tx_bytes += tx;
    if (tx_short) st->uart_tx_timeouts++;
}
#endif

esp_err_t ezsp_bridge_ota_claim(void)
{
#if CONFIG_BITENGINE_ENABLE_ZHUB
    /* 直连型：UART 平时在 zhub 手里（BRIDGE_STATE_ZHUB），先请它让出来 —— 同步：它收摊、归还、在 resume 之前不再自己拿回去。
       超时（zhub 卡住）时它已撤回这次请求，这里直接按"忙"返回；调用方的处置与 OTA 正在进行时相同。 */
    if (s_zhub_suspend) {
        const esp_err_t ze = s_zhub_suspend(EZSP_ZHUB_YIELD_MS);
        if (ze != ESP_OK) {
            ESP_LOGW(TAG, "OTA claim refused: zhub did not yield the UART (%s)", esp_err_to_name(ze));
            return ESP_ERR_INVALID_STATE;
        }
    }
#endif
    /* Only a settled bridge may be claimed: DRAINING/SWITCHING means another
       task is mid-transition and would fight us for the UART, and a second
       claim from a concurrent request must not silently succeed. */
    int expected = BRIDGE_STATE_RUNNING;
    if (!atomic_compare_exchange_strong(&s_state, &expected, BRIDGE_STATE_OTA)) {
        ESP_LOGW(TAG, "OTA claim refused (bridge state %d)", expected);
#if CONFIG_BITENGINE_ENABLE_ZHUB
        if (s_zhub_resume) s_zhub_resume();         /* 上面那次 suspend 成功了：配一次 resume，否则 zhub 永远停着 */
#endif
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "UART claimed for MG24 OTA");
    return ESP_OK;
}

esp_err_t ezsp_bridge_ncp_resync(void)
{
    /* 与 uart_bench_run() 同一套独占序列：切 DISABLED（拆 TCP 服务器、断客户端）→ claim →
       独占会话 → release → 切回。代价与 MG24 升级同级：Zigbee 断开约 40 s，MG24 软复位一次。 */
    const sys_config_t *sys = config_mgr_get_sys();
    const bridge_mode_t prev = s_mode;
    if (prev != BRIDGE_MODE_DISABLED) {
        esp_err_t derr = ezsp_bridge_switch_mode(BRIDGE_MODE_DISABLED);
        if (derr != ESP_OK) {
            ESP_LOGW(TAG, "NCP resync: could not disable bridge (%s)", esp_err_to_name(derr));
            return derr;
        }
    }
    esp_err_t err = ezsp_bridge_ota_claim();
    if (err != ESP_OK) {
        if (prev != BRIDGE_MODE_DISABLED) ezsp_bridge_switch_mode(prev);
        return err;                       /* OTA 正在进行：调用方按"下次开机生效"处理 */
    }
    err = ncp_version_query(sys ? sys->radio_region : NULL);
    ezsp_bridge_ota_release();
    if (prev != BRIDGE_MODE_DISABLED) {
        esp_err_t merr = ezsp_bridge_switch_mode(prev);
        if (merr != ESP_OK) {
            ESP_LOGE(TAG, "NCP resync: restoring mode %d failed (%s)", prev, esp_err_to_name(merr));
            return merr;
        }
    }
    return err;
}

void ezsp_bridge_ota_release(void)
{
    int expected = BRIDGE_STATE_OTA;
    if (!atomic_compare_exchange_strong(&s_state, &expected, BRIDGE_STATE_RUNNING)) {
        /* Not ours to release — leave whatever state the bridge reached
           rather than stomping an ERROR or an in-flight switch. */
        ESP_LOGW(TAG, "OTA release ignored (bridge state %d)", expected);
        return;
    }
    ESP_LOGI(TAG, "UART released after MG24 OTA");
#if CONFIG_BITENGINE_ENABLE_ZHUB
    if (s_zhub_resume) s_zhub_resume();             /* 配 ota_claim 里那次 suspend：zhub 重新接管（从 ASH RST 走一遍） */
#endif
}

bridge_mode_t  ezsp_bridge_get_mode(void)  { return s_mode; }
bridge_state_t ezsp_bridge_get_state(void) { return atomic_load(&s_state); }

const bridge_stats_t *ezsp_bridge_get_stats(void)
{
    return bridge_stats_get();
}

void ezsp_bridge_reset_stats(void)
{
    bridge_stats_reset();
}

void ezsp_bridge_get_clients(tcp_client_info_t *out, int *out_count)
{
#if CONFIG_BITENGINE_ENABLE_ZHUB
    (void)out;
    if (out_count) *out_count = 0;          /* 直连型没有 TCP 客户端 */
#else
    tcp_server_get_clients(out, out_count);
#endif
}

esp_err_t ezsp_bridge_disconnect_client(int index)
{
#if CONFIG_BITENGINE_ENABLE_ZHUB
    (void)index;
    return ESP_ERR_NOT_SUPPORTED;
#else
    return tcp_server_disconnect_client(index);
#endif
}


esp_err_t ezsp_bridge_disconnect_client_by_ip(const char *ip, int *out_closed)
{
#if CONFIG_BITENGINE_ENABLE_ZHUB
    (void)ip;
    if (out_closed) *out_closed = 0;
    return ESP_ERR_NOT_SUPPORTED;
#else
    return tcp_server_disconnect_by_ip(ip, out_closed);
#endif
}

bool ezsp_bridge_health_probe(int timeout_ms)
{
#if CONFIG_BITENGINE_ENABLE_ZHUB
    /* 直连型一律不发：UART 是 zhub 的，RST 会打断它的 ASH 会话（CHECKLIST K5）。直连型的 MG24 探针本来就不走这里
       （wd_probes.c 的 probe_mg24_zhub），这一道防的是将来有人绕过仲裁直接调它。不按"状态是不是 RUNNING"判：zhub 让出之后、
       别人认领之前那一下也是 RUNNING，查和用之间有竞态（第一轮审计） */
    (void)timeout_ms;
    return false;
#else
    // Only valid when no TCP clients hold the ASH session
    const bridge_stats_t *st = bridge_stats_get();
    if (st && st->active_clients > 0) {
        return false;
    }

    // Flush any stale data in UART RX buffer before sending RST.
    // Bounded: a babbling NCP must not pin this task forever.
    uint8_t flush[64];
    for (int i = 0; i < 32 && uart_bridge_read(flush, sizeof(flush), 10) > 0; i++) { }

    // Send ASH RST frame
    static const uint8_t ash_rst[] = {0x1A, 0xC0, 0x38, 0xBC, 0x7E};
    uart_bridge_write(ash_rst, sizeof(ash_rst), 500);

    // Read response, looking for 0x7E (ASH frame terminator = RSTACK)
    uint8_t buf[64];
    int elapsed = 0;
    const int chunk_ms = 50;

    while (elapsed < timeout_ms) {
        int n = uart_bridge_read(buf, sizeof(buf), chunk_ms);
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                if (buf[i] == 0x7E) {
                    // Flush any remaining data
                    uart_bridge_read(buf, sizeof(buf), 10);
                    return true;
                }
            }
        }
        elapsed += chunk_ms;
    }

    return false;
#endif
}
