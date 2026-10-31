#include "button.h"
#include "led_ctrl.h"
#include "config_mgr.h"
#include "watchdog.h"

#include "esp_log.h"
#include "esp_check.h"
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "button";

/* 在 ifdef 之外：无按键构型也要能链接订阅者（ota_mgr 总是订阅） */
ESP_EVENT_DEFINE_BASE(BUTTON_EVENT);

#ifdef CONFIG_BITENGINE_HAS_BUTTON

#include "driver/gpio.h"
#include "devkit_pins.h"

/* 只做重启和恢复出厂——这两个动作网络不通时也得能用，所以必须有物理入口；
   其余（切桥模式、清密码）走 WebUI。
     <100ms      蹭碰，丢弃
     100ms-2s    重启
     2-10s       无动作（已过重启区间，红灯提示"继续按住就是恢复出厂"）
     >10s        恢复出厂
     >20s        卡键，不执行 */
#define POLL_MS           20
#define DEBOUNCE_MS       50
#define PRESS_MIN_MS     100
#define PRESS_MAX_MS    2000
#define HOLD_FACTORY_MS 10000
#define HOLD_STUCK_MS   20000

static bool btn_down(void)
{
    return gpio_get_level(BUTTON_PIN) == 0;   /* 外部 10K 上拉，按下接地 */
}

/* 留痕（见 button.h）。渲染成结果的只有产测接口，所以简单的 volatile 就够：
   写在按键任务、读在 httpd 任务，都是单字对齐，撕裂了最多显示一次旧值。 */
static volatile uint32_t s_press_count;
static volatile uint32_t s_last_ms;
static const char *volatile s_last_action = "none";
static volatile bool s_pressed_now;

void button_get_stats(button_stats_t *out)
{
    if (!out) return;
    out->press_count      = s_press_count;
    out->last_duration_ms = s_last_ms;
    out->last_action      = s_last_action ? s_last_action : "none";
    out->pressed_now      = s_pressed_now;
}

static void button_task(void *arg)
{
    while (1) {
        /* 等按下，50ms 后仍按下才算 */
        if (!btn_down()) { vTaskDelay(pdMS_TO_TICKS(POLL_MS)); continue; }
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
        if (!btn_down()) continue;

        /* 按住计时。重启那一档太短，给不了预警；红灯从 2s 起亮的意义是"重启区间已经过了，继续按住就是恢复出厂"，10s 转红白快闪 = 松手即擦除。
           走 led_ctrl_test 的覆盖层而不是 led_ctrl_set_state：覆盖层 200ms 自动失效，松手即回到真实状态；set_state 会顶掉 s_state，而夜间模式是边沿触发的，被顶掉后灯要亮一整夜。 */
        s_pressed_now = true;
        uint32_t held = DEBOUNCE_MS;
        while (btn_down() && held < HOLD_STUCK_MS) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            held += POLL_MS;
            if (held >= HOLD_FACTORY_MS) {
                uint8_t w = ((held / 150) & 1) ? 255 : 0;
                led_ctrl_test(-1, 255, w, w, 200);        /* 红白交替快闪 */
            } else if (held >= PRESS_MAX_MS) {
                led_ctrl_test(-1, 255, 0, 0, 200);        /* 红常亮 */
            }
        }

        /* 松开也要去抖：只对按下去抖的话，一次抖动就等于按当时已累计的时长
           触发一次动作，超过 10s 那次就是擦掉全部配置。 */
        do {
            while (btn_down()) vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
        } while (btn_down());
        s_pressed_now = false;

        /* 留痕先于执行：重启和恢复出厂都不返回，写在后面就永远记不上。
           产测正是靠这几个字段确认"这一按被完整看见了"。 */
        s_press_count++;
        s_last_ms = held;
        s_last_action = (held >= HOLD_STUCK_MS)   ? "stuck"
                      : (held >= HOLD_FACTORY_MS) ? "factory_reset"
                      : (held >= PRESS_MIN_MS && held < PRESS_MAX_MS) ? "restart"
                      : "none";

        /* 20s 上界是必要的：没有它，被外壳压住一分钟的按键在弹起那一刻就是一次
           恢复出厂。100ms 下界挡蹭碰——重启会让 ZHA 卡死到要重启 HA，不能让
           碰一下就发生。2-10s 落空是有意的：那是恢复出厂的蓄力区间。 */
        if (held >= HOLD_STUCK_MS) {
            ESP_LOGW(TAG, "Held >%ds — stuck button, ignoring", HOLD_STUCK_MS / 1000);
        } else if (held >= HOLD_FACTORY_MS) {
            /* 按键一律保留 Thread 网络：按这个键的人多半是网配错了或密码忘了，
               不该赔上整个 Thread 网络。要连 Thread 一起清得去 WebUI 勾。 */
            ESP_LOGW(TAG, "Hold >10s — factory reset (keeping Thread network)");
            config_mgr_factory_reset(FACTORY_RESET_KEEP_THREAD);   /* 不返回 */
        /* 保护带：本身无动作。只把"有人按住了 2-10 s 又松开"广播出去，给已经
           在网页发起了解锁的人当物理确认（ota_mgr 订阅，见 button.h）；没人
           发起 = 没人理会。等待 0：事件队列满了也绝不让按键任务卡住。 */
        } else if (held >= PRESS_MAX_MS && held < HOLD_FACTORY_MS) {
            esp_event_post(BUTTON_EVENT, BUTTON_EVENT_HOLD_CONFIRM, &held,
                           sizeof(held), 0);
        } else if (held >= PRESS_MIN_MS && held < PRESS_MAX_MS) {
#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
            if (button_restart_inhibited()) { ESP_LOGW(TAG, "Short press (%"PRIu32"ms) ignored — Muse pairing: hold 2-10 s to confirm", held); s_last_action = "ignored"; continue; }
#endif
            ESP_LOGW(TAG, "Short press (%"PRIu32"ms) — restart", held);
            watchdog_restart_esp32();        /* 不返回 */
        }
    }
}

esp_err_t button_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config(BUTTON) failed");

    /* 3072 而非 docs/01 表里的 2048：恢复出厂在这个任务栈上跑一串 NVS 操作，
       栈溢出的表现恰好是"按满 10s 就重启"——看着像成功了，实际没擦干净。
       但也不能再大：本板内部 RAM 的运行时低水位只有几百字节（/diag/heap），
       每 1KB 都是从那里扣的。真实需要量由 config_mgr_factory_reset() 里那条
       "Stack headroom on reset path" 日志给出，实测后按它定。 */
    if (xTaskCreate(button_task, "button", 3072, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create button task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Button initialized on GPIO%d", BUTTON_PIN);
    return ESP_OK;
}

#else /* No button hardware */

esp_err_t button_init(void)
{
    ESP_LOGI(TAG, "Button disabled (no hardware)");
    return ESP_OK;
}

void button_get_stats(button_stats_t *out)
{
    if (!out) return;
    *out = (button_stats_t){ .last_action = "no_hardware" };
}

#endif

#if CONFIG_BITENGINE_ENABLE_MUSE_LINK
/* 直连型：Muse 配对剖面里短按不重启（2026-10-22 上板：App 提示"按一下按钮"，用户短按 = 重启，配对被打断）。确认要按住 2–10 s；
   配对剖面结束时它自己会重启回正常开机，所以这里只管设、不用清。>10 s 恢复出厂照旧 */
static volatile bool s_restart_inhibit;
void button_set_restart_inhibit(bool on) { s_restart_inhibit = on; }
bool button_restart_inhibited(void) { return s_restart_inhibit; }
#endif
