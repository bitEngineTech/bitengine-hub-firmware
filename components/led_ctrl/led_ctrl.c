#include "led_ctrl.h"
#include "config_mgr.h"
#include "network_mgr.h"
#include "watchdog.h"
#include "ezsp_bridge.h"
#include "psram_task.h"
// WEBUI_EVENT base + IDs — avoid including webui.h (circular dep risk)
ESP_EVENT_DECLARE_BASE(WEBUI_EVENT);
ESP_EVENT_DECLARE_BASE(BRIDGE_EVENT);
enum { _LED_WEBUI_OTA_STARTED = 0, _LED_WEBUI_OTA_PROGRESS = 1,
       _LED_WEBUI_OTA_COMPLETE = 2, _LED_WEBUI_OTA_FAILED = 3 };

#include <stdbool.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 无条件包含：里面只有 #define，没有硬件依赖。而 WS2812B_LED_COUNT 要供
   led_ctrl_count()/led_ctrl_name() 使用 —— 那两个函数在没有 WS2812B 硬件的
   构建里也必须存在（REST 层要靠它们回答"这块板子有几颗灯"）。 */
#include "devkit_pins.h"

#ifdef CONFIG_BITENGINE_HAS_WS2812B
#include "driver/rmt_tx.h"
#include <string.h>
#endif
#if defined(CONFIG_BITENGINE_HAS_WS2812B) || defined(CONFIG_BITENGINE_HAS_ETHERNET)
#include "driver/gpio.h"
#endif

static int s_wd_slot_led = -1;

static const char *TAG = "led_ctrl";

// LED indices in the WS2812B daisy-chain (per schematic Sheet5)
enum { LED_SYS = 0, LED_ZIGBEE = 1, LED_THREAD = 2, LED_NET = 3 };

/* 灯位名字的**唯一出处**。REST 的 GET /api/v1/led/test 和前端测试面板都从这里
   取，任何地方都不要再抄一份 —— "1 号灯是哪颗"一旦两处对不上，测试面板就从
   排障工具变成误导工具（点 Zigbee 亮的是 Thread，人会照着去查错的子系统）。
   顺序必须与上面的 enum 一致。 */
static const char *const s_led_names[] = { "sys", "zigbee", "thread", "net" };
_Static_assert(sizeof(s_led_names) / sizeof(s_led_names[0]) == WS2812B_LED_COUNT,
               "LED 名字表与 WS2812B_LED_COUNT 不一致");

int led_ctrl_count(void)
{
    return WS2812B_LED_COUNT;
}

const char *led_ctrl_name(int index)
{
    if (index < 0 || index >= WS2812B_LED_COUNT) return NULL;
    return s_led_names[index];
}

/* SYS 灯的显示按**优先级叠加**，不是一个变量轮流被覆盖。四层，高到低：
 *
 *   1  覆盖层    s_test_until   工厂测试 / 按键长按反馈。故意压过夜间与亮度：
 *                               按键反馈是安全 UI（要让人分清"松手是重启还是
 *                               擦库"），夜里、低亮度下都必须看得见。
 *   1½ 配对指示  s_pair_ind     只在直连型（BITENGINE_ENABLE_MUSE_LINK）：Muse 配对窗口 / 等按键 / 结果，
 *                               只管 SYS，压过夜间（等按键时人就在旁边，夜里也要看得见；docs/15 设计第十一节）
 *   2  夜间抑制  s_night        全局熄灯，scheduler 按时间窗设置
 *   3  瞬时事件  s_event        OTA / ERROR，到期自动落回第 4 层
 *   4  网络态    s_net_state    BOOT/SOFTAP/CONNECTING/NORMAL，由事件持续更新
 *
 * 2026-08-15 之前 3 和 4 挤在同一个 s_state 里，于是：
 *   - 报错时要在**每个报错点**记一份 s_error_recover_to（"待会儿恢复成什么"），
 *     4 个点各猜各的；MG24 复位那处甚至要写 `prev == ERROR ? NORMAL : prev`
 *     去猜前一个状态。猜错就是灯长期显示错的东西。
 *   - 夜间也是这个枚举的一个值，任何事件都能把它顶掉（H1，当天已修）。
 * 拆开之后网络态**从不被覆盖**，事件到期直接落回它，不需要任何记账。 */
static volatile led_state_t s_net_state = LED_STATE_BOOT;
#if CONFIG_BITENGINE_ENABLE_ZHUB
static volatile led_zb_hub_t s_zb_hub = LED_ZB_HUB_OFF;   /* 直连型 ZIGBEE 灯：zhub 推（led_ctrl_set_zigbee_hub），渲染循环每帧读；单字写入，不用锁 */
#endif
#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
static volatile led_pair_ind_t s_pair_ind = LED_PAIR_NONE;
#endif

/* 瞬时事件层。s_event_until = 0 表示不过期（OTA 进行中 / 即将重启），
   非 0 表示到期 tick。到期后自动落回 s_net_state。 */
static volatile led_state_t s_event       = LED_STATE_NORMAL;
static volatile bool        s_event_on    = false;
static volatile uint32_t    s_event_until = 0;

/* tick 比较必须用有符号差值：TickType_t 是 32 位，直接 `now >= deadline`
   在回绕点（100Hz 下约 497 天）会瞬间判成"已到期"或"永不到期"。 */
static inline bool tick_reached(uint32_t deadline)
{
    return (int32_t)(xTaskGetTickCount() - deadline) >= 0;
}

/* 当前生效的语义状态：事件优先于网络态。夜间不在这里合并 —— 它是渲染层的
   抑制，不是一种状态（见 s_night）。 */
static inline led_state_t effective_state(void)
{
    return s_event_on ? s_event : s_net_state;
}

/* 进入一个瞬时事件。hold_ms = 0 表示不过期。
   **不需要传"恢复成什么"** —— 到期落回 s_net_state，而它一直是最新的。 */
static void event_enter(led_state_t ev, uint32_t hold_ms)
{
    s_event       = ev;
    s_event_until = hold_ms ? (xTaskGetTickCount() + pdMS_TO_TICKS(hold_ms)) : 0;
    s_event_on    = true;
}

/* 活动指示改为"保持到某个时刻"而不是"亮一帧就清"（2026-09-11 T4.4）。
 *
 * 原来是 bool：置位 → 渲染一帧 → 清掉。渲染是 20fps，所以一次突发只亮
 * **50ms**，人眼勉强看得见，像素回读更抓不稳（40 次采样只碰到 2 次，
 * 换个时候就是 0 次）。一个看不见也测不出的指示灯等于没接。
 *
 * 现在保持 ZB_ACTIVITY_HOLD_MS。配合 uart 侧 400ms 的节流，连续流量下看到的是
 * "绿底上每 400ms 闪一下蓝"，既读得出"有活动"，又保住了绿=正常这个基色；
 * 保持时间若长过节流间隔就会变成常蓝，那就丢掉了基色。 */
#define ZB_ACTIVITY_HOLD_MS   120

/* NET 灯的节流间隔。必须**明显大于** ZB_ACTIVITY_HOLD_MS，否则连续流量下
   下一次置位会接在上一次的保持窗口里，白色永不落回绿色（见
   led_ctrl_notify_network_activity 的说明）。400 与 120 的比例同 zigbee 侧。 */
#define NET_ACTIVITY_THROTTLE_MS  400
static volatile TickType_t  s_zigbee_activity_until = 0;
static volatile TickType_t  s_net_activity_until = 0;
/* Thread 状态，thread_br 每 2s 推一次，render 只读 */
static volatile bool        s_thread_running = false;
static volatile bool        s_thread_radio_stalled = false;
static volatile bool        s_thread_attached = false;   /* role 是 child/router/leader */
/* 默认 true：Thread 没起来时第一档就是"灭"，这个值不参与；起来后 2 s 定时器会写入实值 */
static volatile bool        s_thread_br_ok = true;

/* 夜间模式是**全局显示模式**，不是一种状态 —— 所以它不能待在 s_state 里。
 *
 * 2026-08-15 之前它是 LED_STATE_NIGHT，和 BOOT/NORMAL/ERROR 挤在同一个枚举里。
 * 后果：led_event_handler() 在事件循环任务里写 s_state 时根本不看当前是不是夜间，
 * 夜里网络抖一下（重连 / 网线插拔 / MG24 复位事件 / OTA 结束）就把 NIGHT 顶掉；
 * 而 scheduler 的夜间判定是边沿触发的（in_night && !s_night_active），
 * s_night_active 已经是 true，**永远不会再设回来** —— 灯亮到天亮。
 * 同源的洞在按键那条路上也出现过一次（见 button.c）。
 *
 * 拆开之后：事件照常更新 s_state（底层状态始终正确），夜间只在渲染时抑制输出。
 * 对外仍按 LED_STATE_NIGHT 上报，/api/v1/status 与 tools/test_api.sh 的契约不变。 */
static volatile bool        s_night = false;

/* Zigbee / 网络的稳态，由渲染任务每 2s 采样一次（见 sample_subsystems） */
static volatile bool        s_zb_mode_off = false;
static volatile bool        s_zb_ncp_alive = true;
static volatile uint8_t     s_zb_clients = 0;
static volatile bool        s_net_fault = false;
static volatile bool        s_net_pending = false;

#ifdef CONFIG_BITENGINE_HAS_WS2812B
/* Own minimal WS2812 TX path (replaces led_strip component usage): the
   board's transistor level shifter widens every pulse by **~150-250ns**,
   so the bit timings are pre-compensated via Kconfig. Whether they return
   to 300/900 depends on what AM2 fits (AM2 work order A2, appendix 1 item 1).
 *
 * 展宽量订正（2026-08-13）：此处原写 250-400ns，是用「LED0 红」判据量出来的，
 * 而红看不见字节丢失（见 k_sweep 注释），量偏了。用四颗全白重测，可用窗口是
 * T1H ∈ [400,700]，据此反推 W≈150-250ns。默认值同时从 T1H=850 改为 550
 * （窗口中心）—— 850 落在窗口外，白/橙/品红都会丢字节，菊花链末端整颗熄灭。
 *
 * 运行时可经 POST /api/v1/led/timing 改（仅内存），用于现场复测这块板的窗口。 */
static rmt_channel_handle_t s_led_ch  = NULL;
static rmt_encoder_handle_t s_led_enc = NULL;
static bool                 s_led_ready = false;
static uint8_t              s_px_buf[WS2812B_LED_COUNT * 3];  /* GRB */

// Scale a 0-255 base color by the configured brightness (0-255).
static inline uint8_t scale(uint8_t c, uint8_t bright)
{
    return (uint16_t)c * bright / 255;
}
#endif

// ---- Event handler ----
static void led_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    if (base == NETWORK_EVENT) {
        /* 网络事件只动第 4 层。它从不被覆盖，所以事件到期时不必猜恢复成什么。 */
        if (id == NETWORK_EVENT_ONLINE) {
            s_net_state = LED_STATE_NORMAL;
        } else if (id == NETWORK_EVENT_OFFLINE) {
            s_net_state = LED_STATE_CONNECTING;   /* 掉线后就是在重连 */
            event_enter(LED_STATE_ERROR, 3000);   /* 顺带红闪 3s 提示 */
        } else if (id == NETWORK_EVENT_SOFTAP_STARTED) {
            s_net_state = LED_STATE_SOFTAP;
        } else if (id == NETWORK_EVENT_SOFTAP_STOPPED && s_net_state == LED_STATE_SOFTAP) {
            /* 热点关了却没有随后的 ONLINE（网口在线时手动开、再关热点；或网口在线时配网成功关热点，T9.40）：
               以前不处理，SYS 灯一直黄呼吸（2026-10-08 审计）。按此刻的主状态落回去 —— 无锁读一个枚举。
               WiFi 连上关热点的那条路随后照样有 ONLINE，这里先落一次无害。 */
            net_state_t ns = network_mgr_get_state();
            s_net_state = (ns == NET_STATE_ETH_ONLINE || ns == NET_STATE_WIFI_ONLINE ||
                           ns == NET_STATE_BOTH_ONLINE) ? LED_STATE_NORMAL : LED_STATE_CONNECTING;
        }
    } else if (base == WEBUI_EVENT) {
        if (id == _LED_WEBUI_OTA_STARTED) {
            event_enter(LED_STATE_OTA, 0);        /* 不过期，直到完成或失败 */
        } else if (id == _LED_WEBUI_OTA_COMPLETE) {
            event_enter(LED_STATE_BOOT, 0);       /* 马上就要重启了 */
        } else if (id == _LED_WEBUI_OTA_FAILED) {
            /* 失败不重启，红闪 3s 后自动落回当前网络态 —— 以前这里要写死
               s_error_recover_to = NORMAL，而那时网络可能正好是 SoftAP。 */
            event_enter(LED_STATE_ERROR, 3000);
        }
    } else if (base == WATCHDOG_EVENT) {
        if (id == WATCHDOG_EVENT_MG24_RESET) {
            /* MG24 复位不改变网络状态。以前这里要靠
               `prev == ERROR ? NORMAL : prev` 猜前一个状态，现在不必了。 */
            event_enter(LED_STATE_ERROR, 2000);
        }
    } else if (base == CONFIG_EVENT) {
        if (id == CONFIG_EVENT_LED_CHANGED) {
            ESP_LOGI(TAG, "LED config updated");
        } else if (id == CONFIG_EVENT_NVS_ERROR) {
            /* 配置落盘失败：红闪 5 秒。这是 CONFIG_EVENT_NVS_ERROR 的**第一个**
               订阅者（2026-09-11 T2.6；在此之前它 post 了两年没人接）。
               为什么值得点灯：这个故障对用户完全静默 —— REST setter 在 2s 防抖前
               就回了 200，用户看到"保存成功"，直到重启才发现设置回退了。
               不开浏览器也能看见的信号只有灯。
               比网络掉线(3s)长一点：它更罕见也更严重，值得多看两眼。 */
            ESP_LOGW(TAG, "配置落盘失败，红闪提示");
            event_enter(LED_STATE_ERROR, 5000);
        }
    } else if (base == BRIDGE_EVENT) {
        // Zigbee activity — brief flash in NORMAL state
        if (effective_state() == LED_STATE_NORMAL) {
            s_zigbee_activity_until = xTaskGetTickCount() +
                                      pdMS_TO_TICKS(ZB_ACTIVITY_HOLD_MS);
        }
    }
}

// ---- LED test mode (factory test / per-LED verification via REST) ----
#ifdef CONFIG_BITENGINE_HAS_WS2812B
static volatile TickType_t s_test_until = 0;
static uint8_t s_test_px[WS2812B_LED_COUNT][3];

esp_err_t led_ctrl_test(int index, uint8_t r, uint8_t g, uint8_t b,
                        uint32_t hold_ms)
{
    if (index < -1 || index >= WS2812B_LED_COUNT) return ESP_ERR_INVALID_ARG;
    if (hold_ms == 0 || hold_ms > 60000) return ESP_ERR_INVALID_ARG;

    for (int i = 0; i < WS2812B_LED_COUNT; i++) {
        bool hit = (index == -1) || (i == index);
        s_test_px[i][0] = hit ? r : 0;
        s_test_px[i][1] = hit ? g : 0;
        s_test_px[i][2] = hit ? b : 0;
    }
    /* L1：像素数组不是 volatile，deadline 是 —— 没有屏障的话编译器可以把
       deadline 的写提到像素之前，渲染任务就会拿旧颜色渲一帧。视觉上无害，
       但这是真实的重排窗口，一行屏障的事。 */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s_test_until = xTaskGetTickCount() + pdMS_TO_TICKS(hold_ms);
    return ESP_OK;
}
#else
esp_err_t led_ctrl_test(int index, uint8_t r, uint8_t g, uint8_t b,
                        uint32_t hold_ms)
{
    (void)index; (void)r; (void)g; (void)b; (void)hold_ms;
    return ESP_ERR_NOT_SUPPORTED;   // no WS2812B hardware
}

esp_err_t led_ctrl_set_timing(uint16_t t0h_ns, uint16_t t1h_ns)
{
    (void)t0h_ns; (void)t1h_ns;
    return ESP_ERR_NOT_SUPPORTED;   // no WS2812B hardware
}

void led_ctrl_get_timing(uint16_t *t0h_ns, uint16_t *t1h_ns)
{
    if (t0h_ns) *t0h_ns = 0;
    if (t1h_ns) *t1h_ns = 0;
}
#endif

#ifdef CONFIG_BITENGINE_HAS_WS2812B
// Forward declarations (pixel buffer + own RMT TX path, defined below)
static void      zled_set_pixel(int i, uint8_t r, uint8_t g, uint8_t b);
static void      zled_clear(void);
static esp_err_t zled_refresh(void);
static void      led_strip_create(void);
static void      led_strip_destroy(void);

/* 采样各子系统的稳态。每 2s 调一次（不是每帧）——虽然这三个 getter 都只是读
   内存变量、不阻塞，但没必要 20fps 去问；而且集中在一处便于确认"渲染循环里
   绝不做可能阻塞的事"。Thread 的数据不在这里取：它要持 OT 锁，只能由
   thread_br 侧的定时器推过来。 */
static void sample_subsystems(void)
{
    bridge_mode_t mode = ezsp_bridge_get_mode();
    s_zb_mode_off = (mode == BRIDGE_MODE_DISABLED);

    const bridge_stats_t *bs = ezsp_bridge_get_stats();
    s_zb_clients = bs ? bs->active_clients : 0;

    const health_status_t *hs = watchdog_get_status();
    s_zb_ncp_alive = hs ? (hs->mg24_alive != 0) : true;

    switch (network_mgr_get_state()) {
    case NET_STATE_NO_NETWORK:
        s_net_fault = true;  s_net_pending = false; break;
    case NET_STATE_INIT:
    case NET_STATE_SOFTAP_ONLY:
    case NET_STATE_ETH_CONNECTING:
    case NET_STATE_WIFI_CONNECTING:
    case NET_STATE_WIFI_RECONNECTING:
        s_net_fault = false; s_net_pending = true;  break;
    default:                     /* ETH_ONLINE / WIFI_ONLINE / BOTH_ONLINE */
        s_net_fault = false; s_net_pending = false; break;
    }
}

/* 刷新 + 限流告警。三条出口（覆盖层 / 夜间 / 正常渲染）共用同一条，
   以前只有正常渲染这条检查返回值，夜间和覆盖层是丢弃的 —— 夜里 RMT 挂了
   没有任何痕迹，而"灯全灭"恰好和夜间模式长得一模一样。

   Do NOT fail silently: if RMT transmit errors on every frame the LEDs never
   receive a single bit and sit in their power-up state (clone WS2812 often
   power up lit) — indistinguishable from a hardware fault unless we log it.
   Rate-limit to one log per ~5s (100 frames). */
static void refresh_checked(void)
{
    esp_err_t rerr = zled_refresh();
    if (rerr != ESP_OK) {
        static uint32_t s_err_count = 0;
        if ((s_err_count++ % 100) == 0) {
            ESP_LOGE(TAG, "led_strip_refresh failed: %s (count=%"PRIu32")",
                     esp_err_to_name(rerr), s_err_count);
        }
    }
}

// Render the 4-LED chain for the current state. tick = frame counter (20fps).
// LED layout: 0=SYS, 1=Zigbee, 2=thread, 3=net.
static void led_render(uint32_t tick)
{
    if ((tick % 40) == 0) sample_subsystems();   /* 20fps → 每 2s */

    /* RMT 建不起来时**不能静默**。本函数下面刷新失败那里就写着 "Do NOT fail
       silently"，而创建失败以前只在启动时打一条 ESP_LOGE，之后每帧无声返回——
       现场是四颗灯全黑而 /api/v1/status 的 led_state 一切正常，排查的人拿不到
       任何线索。现在每 30s 吼一次，并顺手重建一次：RMT 通道被别的组件抢占是
       可能自行缓解的（对方释放后我们就能拿到），能自愈就别等重启。 */
    if (!s_led_ready) {
        if ((tick % 600) == 0) {                     /* 20fps → 30s */
            ESP_LOGE(TAG, "WS2812B 未就绪（RMT 创建失败）—— 四颗灯全灭，重试中");
            led_strip_create();
        }
        return;
    }

    /* 第 1 层：覆盖层。故意压过夜间与亮度——按键长按反馈是安全 UI，夜里和
       低亮度下都必须看得见（见文件头的优先级表）。 */
    if (s_test_until != 0) {
        if (!tick_reached(s_test_until)) {          /* L2：wrap-safe */
            __atomic_thread_fence(__ATOMIC_ACQUIRE);   /* L1：与 test() 配对 */
            for (int i = 0; i < WS2812B_LED_COUNT; i++) {
                zled_set_pixel(i,
                                    s_test_px[i][0], s_test_px[i][1], s_test_px[i][2]);
            }
            refresh_checked();                      /* L3：失败也要吼 */
            return;
        }
        s_test_until = 0;
    }

    const led_config_t *lc = config_mgr_get_led();
    uint8_t b = lc ? lc->brightness : 64;
    bool pulse = (tick % 20) < 10;            // ~0.5s on/off for slow blink
    bool fast  = (tick % 6)  < 3;             // ~0.15s for fast/error flash

    /* 夜间模式熄灯 —— 唯一权威是 s_night，由 scheduler 在时间窗内设置。
       这里**不能**看 lc->night_mode_enabled：那个字段只是"功能开关"，一旦为真
       就立刻全灭，scheduler 算好的 22:00-07:00 窗口形同虚设（下午 2 点打开开关
       灯就永远不亮了）。2026-08-12 接 /api/v1/led/nightmode 时发现。 */
#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
    /* 第 1½ 层：Muse 配对指示，只管 SYS。呼吸：2 s 一个来回（20fps 下 40 帧的三角波）。 */
    const led_pair_ind_t pair_ind = s_pair_ind;
    uint8_t pair_px[3] = {0, 0, 0};
    if (pair_ind != LED_PAIR_NONE) {
        const uint32_t ph = tick % 40;
        const uint32_t breath = 30 + (ph < 20 ? ph : 40 - ph) * 11;   /* 30..250 */
        switch (pair_ind) {
        case LED_PAIR_WINDOW:  pair_px[1] = scale(breath, b); pair_px[2] = scale(breath, b); break;
        case LED_PAIR_CONFIRM: pair_px[1] = scale(fast ? 255 : 0, b); pair_px[2] = scale(fast ? 255 : 0, b); break;
        case LED_PAIR_SUCCESS: pair_px[1] = scale(255, b); break;
        case LED_PAIR_FAILED:  pair_px[0] = scale(fast ? 255 : 0, b); break;
        default: break;
        }
    }
#endif

    /* 第 2 层：夜间抑制。 */
    if (s_night) {
        zled_clear();
        led_ctrl_set_eth_led(false);
#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
        if (pair_ind != LED_PAIR_NONE) zled_set_pixel(LED_SYS, pair_px[0], pair_px[1], pair_px[2]);   /* 配对指示压过夜间 */
#endif
        refresh_checked();                          /* L3：失败也要吼 */
        return;
    }

    // SYS LED (index 0) reflects overall state colour.
    switch (effective_state()) {
    case LED_STATE_BOOT:        // yellow
        zled_set_pixel(LED_SYS, scale(255, b), scale(180, b), 0);
        break;
    case LED_STATE_SOFTAP:      // yellow breathing
        zled_set_pixel(LED_SYS,
                            scale(pulse ? 255 : 40, b), scale(pulse ? 180 : 28, b), 0);
        break;
    case LED_STATE_CONNECTING:  // blue blink
        zled_set_pixel(LED_SYS, 0, 0, scale(pulse ? 255 : 40, b));
        break;
    case LED_STATE_NORMAL:      // green solid
        zled_set_pixel(LED_SYS, 0, scale(255, b), 0);
        break;
    case LED_STATE_OTA:         // purple chase
        zled_set_pixel(LED_SYS,
                            scale(fast ? 200 : 40, b), 0, scale(fast ? 200 : 40, b));
        break;
    case LED_STATE_ERROR:       // red fast flash
        zled_set_pixel(LED_SYS, scale(fast ? 255 : 0, b), 0, 0);
        break;
    default:
        zled_set_pixel(LED_SYS, 0, 0, 0);
        break;
    }
#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
    if (pair_ind != LED_PAIR_NONE) zled_set_pixel(LED_SYS, pair_px[0], pair_px[1], pair_px[2]);
#endif

    /* ---- Zigbee / Thread / net：统一色语义 ----
       绿常亮=正常，黄慢闪=注意（进行中/降级/等操作），红慢闪=故障，灭=未启用。
       四颗灯共用这一套，用户只需要学一次。

       为什么不再是"只在有活动时闪"：那样不活动就熄灭，用户分不清"正常但空闲"
       和"坏了"——这正是把人逼去拔电源的根源。现在每颗灯都表达**稳态**。 */

    // Zigbee LED (index 1)
#if CONFIG_BITENGINE_ENABLE_ZHUB
    /* 直连型：没有桥（s_zb_mode_off 恒真），看 zhub 推来的状态（Z54）。黄 / 红的慢闪与下面 HA 型同一组值 */
    switch (s_zb_hub) {
    case LED_ZB_HUB_UP:
        /* 到期就清零：int32 差值在 2^31 tick（约 24.9 天）没消息后会变号，没清的旧截止时刻会让灯一直蓝（审计 ⚠️；HA 型桥的流量不断，那一支不改） */
        if (s_zigbee_activity_until && tick_reached(s_zigbee_activity_until)) s_zigbee_activity_until = 0;
        if (s_zigbee_activity_until) zled_set_pixel(LED_ZIGBEE, 0, 0, scale(255, b));   // 设备发来消息：蓝闪一下
        else zled_set_pixel(LED_ZIGBEE, 0, scale(200, b), 0);                                                                   // 网络在线
        break;
    case LED_ZB_HUB_STARTING:
    case LED_ZB_HUB_JOIN:
        zled_set_pixel(LED_ZIGBEE, scale(pulse ? 255 : 20, b), scale(pulse ? 140 : 11, b), 0);   // 建网中 / 入网窗口开着
        break;
    case LED_ZB_HUB_FAIL:
        zled_set_pixel(LED_ZIGBEE, scale(pulse ? 255 : 0, b), 0, 0);                             // Zigbee 芯片不应答
        break;
    default:
        zled_set_pixel(LED_ZIGBEE, 0, 0, 0);
        break;
    }
#else
    if (s_zb_mode_off) {
        zled_set_pixel(LED_ZIGBEE, 0, 0, 0);                       // 桥接关闭
    } else if (!s_zb_ncp_alive) {
        zled_set_pixel(LED_ZIGBEE, scale(pulse ? 255 : 0, b), 0, 0);  // MG24 无响应
    } else if (s_zb_clients == 0) {
        // MG24 好着，但 Z2M/ZHA 没连上来 —— 最常见的"以为坏了"场景
        zled_set_pixel(LED_ZIGBEE, scale(pulse ? 255 : 20, b), scale(pulse ? 140 : 11, b), 0);
    } else if (s_zigbee_activity_until && !tick_reached(s_zigbee_activity_until)) {
        zled_set_pixel(LED_ZIGBEE, 0, 0, scale(255, b));           // 数据活动：蓝闪一下
    } else {
        zled_set_pixel(LED_ZIGBEE, 0, scale(200, b), 0);           // 有客户端，正常
    }
#endif

    /* thread LED (index 2)：只报**网关自己**的 Thread 状态。
       灭=BR 未运行；红闪=射频停摆；琥珀慢闪=还没入网、掉出网络，或边界路由停了；绿常亮=在网且边界路由在跑。

       ⚠️ 2026-08-15 之前这里看的是"直连子节点里有几个还在恢复"。那个判据是错的：
       它只统计网关的**直连**子节点，而网状网里终端设备会挂到离它更近的路由器下面。
       当天实测——门窗传感器改挂到另一台 FTD（rloc 0x9000）下面，网关子节点表为空、
       HA 一切正常；而网关那条从 NVS 恢复出来、设备再也不会回来的残留条目一直处于
       restoring，灯就黄闪了好几分钟。反过来 role=detached 时只要没有 restoring
       子节点，灯还是绿的——网关掉出网络了却显示正常。两个方向都错。

       现在只报网关确定知道的事。"终端设备能不能用"需要另一个信号，在找到经得起
       验证的判据之前，宁可不显示，也不要显示一个错的。 */
    if (!s_thread_running) {
        zled_set_pixel(LED_THREAD, 0, 0, 0);                       // BR 未运行
    } else if (s_thread_radio_stalled) {
        // 射频停止收发（MAC 计数器长时间不动）——2026-08-09 的能量扫描事故就是这个
        zled_set_pixel(LED_THREAD, scale(pulse ? 255 : 0, b), 0, 0);
    } else if (!s_thread_attached) {
        zled_set_pixel(LED_THREAD,                                  // 还没入网 / 掉出网络
                            scale(pulse ? 255 : 30, b), scale(pulse ? 140 : 16, b), 0);
    } else if (!s_thread_br_ok) {
        /* 在网，但边界路由停了 —— HA 连不到任何 Thread 设备。与"没入网"同一档琥珀慢闪：
           对用户都是"Thread 这边暂时不能用"。典型场景是以太网刚断、还没换到 WiFi 的几秒；
           长时间不灭说明换口失败（测试 6c）。2026-10-05 之前这里直接落到绿灯。 */
        zled_set_pixel(LED_THREAD,
                            scale(pulse ? 255 : 30, b), scale(pulse ? 140 : 16, b), 0);
    } else {
        zled_set_pixel(LED_THREAD, 0, scale(200, b), 0);            // 在网、射频活着
    }
    /* THREAD 灯每次换档记一条，写明是哪条判据。2026-10-05 拔线验收：网关日志说边界路由
       6 s 就恢复，用户却看到琥珀约 40 s —— 灯的显示此前没有任何记录，无从对质。
       只在换档时记，稳态零开销；W 级好与 thread_br 的边界路由日志一起留在告警环里。 */
    static int s_thread_shown = -1;
    int shown = !s_thread_running       ? 0 :
                s_thread_radio_stalled  ? 1 :
                !s_thread_attached      ? 2 :
                !s_thread_br_ok         ? 3 : 4;
    if (shown != s_thread_shown) {
        static const char *const why[] = {
            "灭（Thread 未运行）", "红闪（射频停摆）", "琥珀慢闪（没入网）",
            "琥珀慢闪（边界路由停了）", "绿",
        };
        ESP_LOGW(TAG, "THREAD 灯 → %s", why[shown]);
        s_thread_shown = shown;
    }

    // net LED (index 3)
    if (s_net_fault) {
        zled_set_pixel(LED_NET, scale(pulse ? 255 : 0, b), 0, 0);   // 无网络
    } else if (s_net_pending) {
        zled_set_pixel(LED_NET,                                      // 连接中/重连中/待配网
                            scale(pulse ? 255 : 20, b), scale(pulse ? 140 : 11, b), 0);
    } else if (s_net_activity_until && !tick_reached(s_net_activity_until)) {
        zled_set_pixel(LED_NET, scale(255, b), scale(255, b), scale(255, b));
    } else {
        zled_set_pixel(LED_NET, 0, scale(200, b), 0);                // 在线
    }

    refresh_checked();
}
#endif

#ifdef CONFIG_BITENGINE_HAS_WS2812B
static void zled_set_pixel(int i, uint8_t r, uint8_t g, uint8_t b)
{
    if (i < 0 || i >= WS2812B_LED_COUNT) return;
    s_px_buf[i * 3 + 0] = g;   /* WS2812 wire order: GRB */
    s_px_buf[i * 3 + 1] = r;
    s_px_buf[i * 3 + 2] = b;
}

static void zled_clear(void)
{
    memset(s_px_buf, 0, sizeof(s_px_buf));
}

static esp_err_t zled_refresh(void)
{
    if (!s_led_ready) return ESP_ERR_INVALID_STATE;
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    esp_err_t err = rmt_transmit(s_led_ch, s_led_enc, s_px_buf,
                                 sizeof(s_px_buf), &tx_cfg);
    if (err == ESP_OK) err = rmt_tx_wait_all_done(s_led_ch, 100);
    return err;
}

static void led_strip_destroy(void)
{
    if (!s_led_ready) return;
    rmt_disable(s_led_ch);
    rmt_del_encoder(s_led_enc);
    rmt_del_channel(s_led_ch);
    s_led_ch = NULL; s_led_enc = NULL;
    s_led_ready = false;
}

/* 生效中的位时序。Kconfig 值只是**开机默认**，运行时可经
   POST /api/v1/led/timing 改，用来在网页上现场找这块板的补偿量，不用重烧。
   只存在于 RAM：重启即回默认值。这是有意的 —— 调坏了拔电重来，
   找到好值再写进 Kconfig 固化。 */
static volatile uint16_t s_t0h_ns = CONFIG_BITENGINE_WS2812_T0H_NS;
static volatile uint16_t s_t1h_ns = CONFIG_BITENGINE_WS2812_T1H_NS;
/* 待应用的新时序。**必须**由渲染任务自己应用：重建 encoder 会 del/new，
   而渲染任务每帧都在用它，从 httpd 任务直接重建就是 use-after-free。 */
static volatile bool     s_timing_req = false;

#define WS2812_BIT_PERIOD_NS 1200

static void led_strip_create(void)
{
    rmt_tx_channel_config_t ch_cfg = {
        .gpio_num          = WS2812B_DATA_PIN,
        .clk_src           = RMT_CLK_SRC_XTAL,   /* DFS-immune */
        .resolution_hz     = 20 * 1000 * 1000,   /* 50ns ticks */
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    if (rmt_new_tx_channel(&ch_cfg, &s_led_ch) != ESP_OK) {
        ESP_LOGE(TAG, "WS2812B RMT channel create failed");
        return;
    }
    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .level0 = 1, .duration0 = s_t0h_ns / 50,
                  .level1 = 0, .duration1 = (WS2812_BIT_PERIOD_NS - s_t0h_ns) / 50 },
        .bit1 = { .level0 = 1, .duration0 = s_t1h_ns / 50,
                  .level1 = 0, .duration1 = (WS2812_BIT_PERIOD_NS - s_t1h_ns) / 50 },
        .flags.msb_first = 1,
    };
    if (rmt_new_bytes_encoder(&enc_cfg, &s_led_enc) != ESP_OK ||
        rmt_enable(s_led_ch) != ESP_OK) {
        ESP_LOGE(TAG, "WS2812B encoder/enable failed");
        if (s_led_enc) { rmt_del_encoder(s_led_enc); s_led_enc = NULL; }
        rmt_del_channel(s_led_ch); s_led_ch = NULL;
        return;
    }
    s_led_ready = true;
    memset(s_px_buf, 0, sizeof(s_px_buf));
    zled_refresh();
    ESP_LOGI(TAG, "WS2812B initialized: %d LEDs on GPIO%d "
             "(T0H=%uns/T1H=%uns, 低电平 T0L=%uns/T1L=%uns)",
             WS2812B_LED_COUNT, WS2812B_DATA_PIN,
             (unsigned)s_t0h_ns, (unsigned)s_t1h_ns,
             (unsigned)(WS2812_BIT_PERIOD_NS - s_t0h_ns),
             (unsigned)(WS2812_BIT_PERIOD_NS - s_t1h_ns));
}

/* ---- Timing sweep diagnostic (kept for factory test / X5 regression):
   cycles TX variants rendering **four white LEDs**; the step that renders
   correctly quantifies the level-shifter distortion.
 *
 * ⚠️ 判据必须是白色，不能是单色。2026-08-13 之前这里用的是「LED0 红」，
 * 而红在线序上是 `00 FF 00` —— 第一个非零字节之后本来就没有内容，所以
 * 「1 之后的数据全丢」这个故障对它**完全不可见**。当初正是靠这个瞎了的判据
 * 选出了 T1H=850，结果单色全对、白/橙/品红全错（品红显示成纯红，因为 G=00
 * 跳过、R=FF 生效、B=FF 丢失）。
 * 白色 `FF FF FF` 要求三个字节**全部**存活才能显示对，是唯一能暴露该故障的
 * 最简判据；四颗全亮同时覆盖整条菊花链末端。 */
typedef struct {
    const char *name;
    bool        invert;
    uint16_t    t0h_ns, t0l_ns, t1h_ns, t1l_ns;
} sweep_variant_t;

/* 主扫描变量是 T1H —— 故障机理是「1 bit 之后的低电平被展宽吃掉，连续的 1
   粘成一条长高电平，LED 丢失位时钟」。T1L 必须留够余量：展宽 250-400ns，
   要保证展宽后仍有 ~300ns 以上的低电平，T1L 至少要 600-700ns。
   T0H 保持在 100-150（单色显示正常说明 0 bit 目前是好的，不要一起动，
   否则又变成多变量）。 */
static const sweep_variant_t k_sweep[] = {
    { "1: standard    T0H=300 T1H=900", false, 300, 900,  900, 300 },
    { "2: CURRENT     T0H=150 T1H=850", false, 150, 1050, 850, 350 },
    { "3:             T0H=100 T1H=750", false, 100, 1100, 750, 450 },
    { "4:             T0H=100 T1H=600", false, 100, 1100, 600, 600 },
    { "5:             T0H=100 T1H=500", false, 100, 1100, 500, 700 },
    { "6:             T0H=100 T1H=400", false, 100, 1100, 400, 800 },
    { "7: inverted    T0H=300 T1H=900", true,  300, 900,  900, 300 },
};

static volatile bool s_sweep_req = false;

esp_err_t led_ctrl_sweep(void)
{
    s_sweep_req = true;
    return ESP_OK;
}

esp_err_t led_ctrl_set_timing(uint16_t t0h_ns, uint16_t t1h_ns)
{
    /* 下限 50ns = RMT 一个 tick（20MHz），再小编码器会算出 0 时长。
       上限留 200ns 低电平余量：低电平是 WS2812 用来分隔相邻 bit 的，
       给到 0 就是现在这个故障（连续的 1 粘成一条长高电平）。 */
    if (t0h_ns < 50 || t1h_ns < 50) return ESP_ERR_INVALID_ARG;
    if (t0h_ns > WS2812_BIT_PERIOD_NS - 200) return ESP_ERR_INVALID_ARG;
    if (t1h_ns > WS2812_BIT_PERIOD_NS - 200) return ESP_ERR_INVALID_ARG;
    /* T0H 必须短于 T1H，否则 0 和 1 分不开 */
    if (t0h_ns >= t1h_ns) return ESP_ERR_INVALID_ARG;

    s_t0h_ns = t0h_ns;
    s_t1h_ns = t1h_ns;
    s_timing_req = true;          /* 由渲染任务重建，见 s_timing_req 的注释 */
    return ESP_OK;
}

void led_ctrl_get_timing(uint16_t *t0h_ns, uint16_t *t1h_ns)
{
    if (t0h_ns) *t0h_ns = s_t0h_ns;
    if (t1h_ns) *t1h_ns = s_t1h_ns;
}

static void led_sweep_send(rmt_channel_handle_t ch, rmt_encoder_handle_t enc,
                           const uint8_t *px, size_t len)
{
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    if (rmt_transmit(ch, enc, px, len, &tx_cfg) == ESP_OK) {
        rmt_tx_wait_all_done(ch, 100);
    }
    vTaskDelay(pdMS_TO_TICKS(1));
}

static void led_sweep_run(void)
{
    ESP_LOGW(TAG, "==== LED TX sweep: 找【四颗都是干净白色】的那一步 ====");
    ESP_LOGW(TAG, "     偏绿/偏红/偏黄 = 该步时序仍在丢字节，不合格");
    led_strip_destroy();

    /* 四颗全白。白要求 G/R/B 三个字节全部存活，是能暴露「1 之后丢字节」的
       最简判据；单色（如 `00 FF 00`）看不出来，别改回去。 */
    static const uint8_t white_all[12] = {
        0xFF, 0xFF, 0xFF,  0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF,  0xFF, 0xFF, 0xFF,
    };
    static const uint8_t off[12]  = { 0 };

    for (size_t v = 0; v < sizeof(k_sweep) / sizeof(k_sweep[0]); v++) {
        const sweep_variant_t *sv = &k_sweep[v];
        /* 这两个诊断动作是同步跑在渲染任务里的：sweep 约 11s、probe 最长 60s，
           而本任务的看门狗槽 deadline 只有 5000ms。不在循环里喂狗的话，每做一次
           产线 LED 诊断就留一条假故障记录（severity 是 OBSERVE，只报告不动作，
           所以以前没人注意到，但污染的是排查时最该可信的那份取证）。 */
        wd_kick(s_wd_slot_led);
        ESP_LOGW(TAG, "sweep step %s", sv->name);

        rmt_tx_channel_config_t ch_cfg = {
            .gpio_num          = WS2812B_DATA_PIN,
            .clk_src           = RMT_CLK_SRC_XTAL,
            .resolution_hz     = 20 * 1000 * 1000,
            .mem_block_symbols = 64,
            .trans_queue_depth = 4,
            .flags.invert_out  = sv->invert,
        };
        rmt_channel_handle_t ch = NULL;
        if (rmt_new_tx_channel(&ch_cfg, &ch) != ESP_OK) continue;
        rmt_bytes_encoder_config_t enc_cfg = {
            .bit0 = { .level0 = 1, .duration0 = sv->t0h_ns / 50,
                      .level1 = 0, .duration1 = sv->t0l_ns / 50 },
            .bit1 = { .level0 = 1, .duration0 = sv->t1h_ns / 50,
                      .level1 = 0, .duration1 = sv->t1l_ns / 50 },
            .flags.msb_first = 1,
        };
        rmt_encoder_handle_t enc = NULL;
        if (rmt_new_bytes_encoder(&enc_cfg, &enc) != ESP_OK ||
            rmt_enable(ch) != ESP_OK) {
            if (enc) rmt_del_encoder(enc);
            rmt_del_channel(ch);
            continue;
        }

        led_sweep_send(ch, enc, off, sizeof(off));
        vTaskDelay(pdMS_TO_TICKS(600));
        for (int r = 0; r < 8; r++) {
            led_sweep_send(ch, enc, white_all, sizeof(white_all));
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        rmt_disable(ch);
        rmt_del_encoder(enc);
        rmt_del_channel(ch);
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    ESP_LOGW(TAG, "==== LED TX sweep done ====");
    led_strip_create();
    s_sweep_req = false;
}

/* DIN probe: releases the RMT channel and drives GPIO25 as a 1Hz square
   wave so the whole level-shifter chain can be traced with a multimeter
   (no scope needed). Runs inside led_task (the strip owner). */
static volatile uint32_t s_probe_seconds = 0;

esp_err_t led_ctrl_probe(uint32_t seconds)
{
    if (seconds == 0 || seconds > 60) return ESP_ERR_INVALID_ARG;
    s_probe_seconds = seconds;
    return ESP_OK;
}

static void led_probe_run(void)
{
    uint32_t sec = s_probe_seconds;
    ESP_LOGW(TAG, "DIN probe: 1Hz square wave on GPIO%d for %"PRIu32"s "
             "(multimeter should read ~0V/~3.3V alternating at IO%d, and "
             "~0V/~VCC_IN at LED1 DIN if the shifter passes DC)",
             WS2812B_DATA_PIN, sec, WS2812B_DATA_PIN);
    led_strip_destroy();
    gpio_set_level(WS2812B_DATA_PIN, 0);
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << WS2812B_DATA_PIN,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    for (uint32_t i = 0; i < sec * 2; i++) {
        wd_kick(s_wd_slot_led);       /* 同上：最长 60s，别让它变成假故障 */
        gpio_set_level(WS2812B_DATA_PIN, i & 1);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    led_strip_create();
    s_probe_seconds = 0;
    ESP_LOGW(TAG, "DIN probe done, strip re-initialized");
}
#else
esp_err_t led_ctrl_probe(uint32_t seconds)
{
    (void)seconds;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t led_ctrl_sweep(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
#endif

// ---- Animation task ----
static void led_task(void *arg)
{
    uint32_t tick = 0;
    while (1) {
        wd_kick(s_wd_slot_led);
        /* 瞬时事件到期 → 自动落回网络态。不需要"恢复成什么"的记账：
           s_net_state 一直是最新的（网络事件从不被覆盖）。 */
        if (s_event_on && s_event_until != 0 && tick_reached(s_event_until)) {
            s_event_on = false;
        }

#ifdef CONFIG_BITENGINE_HAS_WS2812B
        if (s_probe_seconds) led_probe_run();
        if (s_sweep_req) led_sweep_run();
        /* 时序重建只能在这里做 —— destroy/create 会 del 掉 encoder 和 channel，
           而 led_render() 每帧都在用它们。放在渲染之前，本帧就用上新时序。 */
        if (s_timing_req) {
            s_timing_req = false;
            led_strip_destroy();
            led_strip_create();
            ESP_LOGW(TAG, "位时序已切换: T0H=%uns T1H=%uns (仅内存，重启回默认)",
                     (unsigned)s_t0h_ns, (unsigned)s_t1h_ns);
        }
        led_render(tick);
#endif
        // (no hardware: state tracked in memory for WebUI/API reporting)

        tick++;
        vTaskDelay(pdMS_TO_TICKS(50));  // 20fps
    }
}

// ---- Public API ----

esp_err_t led_ctrl_init(void)
{
    /* 初值必须在注册 handler **之前**赋。注册完成的下一刻事件循环任务就可能
       送来 NETWORK_EVENT_ONLINE 把 s_state 置为 NORMAL，这行若留在函数末尾
       （2026-08-13 之前如此）会把它盖回 BOOT —— SYS 永远黄常亮。 */
    s_net_state = LED_STATE_BOOT;

    // Subscribe to events
    esp_event_handler_instance_register(NETWORK_EVENT, ESP_EVENT_ANY_ID,
                                        led_event_handler, NULL, NULL);
    esp_event_handler_instance_register(WEBUI_EVENT, ESP_EVENT_ANY_ID,
                                        led_event_handler, NULL, NULL);
    esp_event_handler_instance_register(WATCHDOG_EVENT, ESP_EVENT_ANY_ID,
                                        led_event_handler, NULL, NULL);
    esp_event_handler_instance_register(CONFIG_EVENT, CONFIG_EVENT_NVS_ERROR,
                                        led_event_handler, NULL, NULL);
    esp_event_handler_instance_register(CONFIG_EVENT, CONFIG_EVENT_LED_CHANGED,
                                        led_event_handler, NULL, NULL);

    // Subscribe to bridge events (Zigbee activity indication)
    esp_event_handler_instance_register(BRIDGE_EVENT, ESP_EVENT_ANY_ID,
                                        led_event_handler, NULL, NULL);

    /* ---- 补读一次当前网络状态（必须在注册之后）----
       ESP-IDF 默认事件循环**不粘、不重放**：注册之前派发掉的事件永远看不到，
       而 NETWORK_EVENT_ONLINE 只在上线那一刻发一次。main.c 里 network_mgr_init()
       比这里早了近十个组件（thread_br 起 OpenThread 尤其慢），以太网 1-3s 就
       link-up + DHCP 完毕 —— 事件在本函数注册 handler 之前就发完了。
       后果不是"某一帧不对"，是永久性的：SYS 停在 BOOT 黄常亮，而 zigbee/thread/
       net 三颗不看 s_state，照常绿常亮。整机一切正常，唯独 SYS 像没启动完；
       且 BOOT 态下 led_event_handler() 不置 s_zigbee_activity，Zigbee 灯连蓝色
       活动闪都没有。2026-08-13 现场实测到此现象后加的这段。

       只在仍为 BOOT 时补：注册之后真收到的事件信息更新，不能被这里覆盖。 */
    if (s_net_state == LED_STATE_BOOT) {
        net_state_t ns = network_mgr_get_state();
        switch (ns) {
        case NET_STATE_ETH_ONLINE:
        case NET_STATE_WIFI_ONLINE:
        case NET_STATE_BOTH_ONLINE:
            s_net_state = LED_STATE_NORMAL;
            break;
        case NET_STATE_SOFTAP_ONLY:
            /* 出厂态同样中招：无 WiFi 凭据时 SoftAP 在 network_mgr_init() 里就
               起来了，SOFTAP_STARTED 一并错过 —— 黄呼吸退化成黄常亮，用户看不
               出该去连配网热点。 */
            s_net_state = LED_STATE_SOFTAP;
            break;
        case NET_STATE_NO_NETWORK:
            /* 与 NETWORK_EVENT_OFFLINE 分支保持一致：红闪 3s 后转 CONNECTING */
            s_net_state = LED_STATE_CONNECTING;
            event_enter(LED_STATE_ERROR, 3000);
            break;
        default:
            /* INIT / *_CONNECTING / WIFI_RECONNECTING：还没上线过，BOOT 黄灯就是
               正确显示，稍后的 ONLINE 事件会正常送达，无需补。 */
            break;
        }
        ESP_LOGI(TAG, "initial LED state %d (net state %d)", s_net_state, ns);
    }

#ifdef CONFIG_BITENGINE_HAS_WS2812B
    led_strip_create();
#else
    ESP_LOGI(TAG, "LED control initialized (no hardware — state tracking only)");
#endif

    /* IO27 是 RS706 的 MR（手动复位输入），不是 LED 栅极 —— 这里曾把它配置成
       输出并驱动高。两个问题：gpio_config(OUTPUT) 会先驱动出寄存器默认的 0，
       在 MR 上产生低毛刺（只因该网络有 0.2uF 去抖电容才没每次开机复位）；
       而夜间模式会持续拉低它，直接把板子送进无限重启循环。
       现在完全不碰这个脚，让 R57 的 10K 上拉保持 MR 为高。见 devkit_pins.h。 */

    static const wd_task_spec_t wd_spec = {
        .name = "led_task", .deadline_ms = 5000, .severity = WD_SEV_OBSERVE,
    };
    s_wd_slot_led = wd_register(&wd_spec);
    /* 2560（原 2048）：2026-08-17 逐任务栈余量接出来后，负载下 led_task 是全表
       最紧的一个，stack_left 只剩 692。栈在 PSRAM，加大不占内部 SRAM。
       它爆栈的后果是整机 panic 重启（金丝雀开着），不值得省这 512 字节。 */
    psram_task_create(led_task, "led_task", 2560, NULL, 6, NULL);
    /* 这里**不要**再赋 s_state —— 见函数开头。 */
    return ESP_OK;
}

#if CONFIG_BITENGINE_ENABLE_ZHUB
void led_ctrl_set_zigbee_hub(led_zb_hub_t st)
{
    s_zb_hub = st;          /* 渲染循环每帧读一次；单字写入，不用锁 */
}
#endif

#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
void led_ctrl_set_pair_indication(led_pair_ind_t ind)
{
    s_pair_ind = ind;       /* 渲染循环每帧读一次；单字写入，不用锁 */
}
#endif

void led_ctrl_set_night(bool on)
{
    if (s_night == on) return;
    s_night = on;
    ESP_LOGI(TAG, "夜间模式 %s", on ? "ON（熄灯）" : "OFF");
}

void led_ctrl_set_state(led_state_t state)
{
    /* NIGHT 不是状态，是显示模式。留这条重定向是防御性的：它曾经是枚举值，
       将来若有人照着旧代码写 set_state(LED_STATE_NIGHT)，不该静默失效。 */
    if (state == LED_STATE_NIGHT) {
        ESP_LOGW(TAG, "set_state(NIGHT) 已废弃，请用 led_ctrl_set_night(true)");
        led_ctrl_set_night(true);
        return;
    }
    /* 写的是第 4 层（网络态）。瞬时事件由 event_enter() 走另一条路，两者不互相
       覆盖 —— 这正是拆开的目的。 */
    if (s_net_state == state) return;  /* callers may set repeatedly */
    s_net_state = state;
    ESP_LOGI(TAG, "LED net state → %d", state);
}

/* 对外把 night 合并回来：/api/v1/status 的 led_state 与 tools/test_api.sh 的
   断言都按 "night" 认，拆分是内部实现细节，不该泄漏到接口上。 */
led_state_t led_ctrl_get_state(void)
{
    return s_night ? LED_STATE_NIGHT : effective_state();
}

/* 故意用 switch 而不是查表：漏掉新增的 led_state_t 会被 -Wswitch 当场报出来。
   名字是 /api/v1/status 的 led_state 字段，也是 tools/test_api.sh 的断言对象。 */
const char *led_ctrl_state_name(led_state_t state)
{
    switch (state) {
    case LED_STATE_BOOT:       return "boot";
    case LED_STATE_SOFTAP:     return "softap";
    case LED_STATE_CONNECTING: return "connecting";
    case LED_STATE_NORMAL:     return "normal";
    case LED_STATE_OTA:        return "ota";
    case LED_STATE_ERROR:      return "error";
    case LED_STATE_NIGHT:      return "night";
    }
    return "unknown";
}

void led_ctrl_notify_zigbee_activity(void)
{
    /* 保持一小段而不是"亮一帧"——理由见 ZB_ACTIVITY_HOLD_MS 的说明。
       调用方（uart_bridge/tcp_server）已按 400ms 节流，这里不再节流。 */
    s_zigbee_activity_until = xTaskGetTickCount() + pdMS_TO_TICKS(ZB_ACTIVITY_HOLD_MS);
}

/* NET 灯的"有流量"白闪（2026-09-11 补接）。
 *
 * ⚠️ 这个函数此前**零调用者**：渲染分支（led_render 的 LED_NET 那一档）、
 *    函数体、头文件声明三样都在，就是没人调 —— 于是 NET 灯永远只在绿/黄/红
 *    三个稳态之间走，那一档白闪是死代码。T4.4 的 TODO 标题写的是
 *    「NET / Zigbee 灯活动闪」，实际只接了 Zigbee 那半边，NET 这半边留下的
 *    形状恰恰就是第 4 节要消灭的那种断链。由用户肉眼发现（"net 上没看到闪烁"）。
 *    `VERIFY.sh` 现在盯着这两个 notify 都必须有调用者。
 *
 * ⚠️ **节流放在这里，不在调用方**——这是它和 zigbee 那个的关键区别。
 *    zigbee 只有一个来源（tcp_server 的收发两个方向），可以在调用方节流；
 *    而 NET 的语义是"网口上有数据进出"，来源有四处（桥接 TCP、WebUI 的每个
 *    HTTP 请求、MQTT 发布、webhook 投递）。若各自按 400ms 节流，四个来源叠
 *    起来最快每 100ms 就重新置位一次，而保持窗口是 120ms —— 结果是**常白**，
 *    把"绿=在线"这个基色彻底盖掉，看起来反而像坏了。
 *
 * 竞态：s_last 的读-改-写在多任务下不是原子的（httpd / mqtt / webhook / 桥接
 * 四个任务都会调）。最坏结果是同一个窗口里多置位一次，而置位本身幂等，
 * 视觉上毫无区别 —— 为它加锁会让这个函数变成可能阻塞的，而它在 TX 路径上
 * 被持锁调用，代价方向反了。 */
void led_ctrl_notify_network_activity(void)
{
    static volatile TickType_t s_last = 0;
    TickType_t now = xTaskGetTickCount();
    if (s_last && (now - s_last) < pdMS_TO_TICKS(NET_ACTIVITY_THROTTLE_MS)) return;
    s_last = now;
    s_net_activity_until = now + pdMS_TO_TICKS(ZB_ACTIVITY_HOLD_MS);
}

/* 保留空实现而不是删函数：调用点（scheduler 夜间模式、led_render）本身逻辑
   没错，错的是这块板上没有这盏灯，而它们操作的引脚是 RS706 的复位输入。
   驱动它 = 整板重启，所以这里什么都不做。
   若将来硬件真的加了以太网 LED，改的是引脚号，不是这些调用点。 */
void led_ctrl_set_eth_led(bool on)
{
    (void)on;
}

/* 回读**最后一次真正写进 strip 的**像素（RGB，非线上的 GRB 序）。
 *
 * 加这个是因为在此之前，"灯此刻到底在显示什么"没有任何机器可读的出口：
 * /api/v1/status 的 led_state 报的是语义状态，而语义状态和实际像素中间还隔着
 * 覆盖层、夜间抑制、RMT 是否就绪三道关——2026-08-15 验证"夜间模式不被事件顶掉"
 * 时就卡在这里：只能问人"灯闪了吗"，而人不可能连续盯几分钟、更看不见 50ms 一帧
 * 的闪烁。有了它，夜间必须全零、错误态必须红色这类判据才是可断言的。
 *
 * 返回写入的灯数；out 为 NULL 或无硬件时返回 0。 */
int led_ctrl_get_pixels(uint8_t out[][3], int max_leds)
{
#ifdef CONFIG_BITENGINE_HAS_WS2812B
    if (!out || max_leds <= 0) return 0;
    int n = (max_leds < WS2812B_LED_COUNT) ? max_leds : WS2812B_LED_COUNT;
    for (int i = 0; i < n; i++) {
        out[i][0] = s_px_buf[i * 3 + 1];   /* R —— 线上是 GRB */
        out[i][1] = s_px_buf[i * 3 + 0];   /* G */
        out[i][2] = s_px_buf[i * 3 + 2];   /* B */
    }
    return n;
#else
    (void)out; (void)max_leds;
    return 0;
#endif
}

void led_ctrl_set_thread_border_routing(bool running)
{
    s_thread_br_ok = running;
}

void led_ctrl_set_thread_health(bool running, bool radio_stalled, bool attached)
{
    s_thread_running = running;
    s_thread_radio_stalled = radio_stalled;
    s_thread_attached = attached;
}

