/* ble_window 的策略 C 开机部分（docs/15 设计 3.2、2.6）：RTC 一次性标志、请求隔离重启、正常开机释放 BT 内存。
 *
 * 一次性标志是 RTC_NOINIT 里两个字（magic + 取反），**自己的变量**：与 wd_forensics 的 RTC 记录（s_rtc）是两个符号，
 * 链接器不会让它们重叠（设计 3.2 末段）。开机最早处取走并无条件清掉，所以断电、崩溃、配对剖面里再重启都回正常开机。
 */
#include "ble_window.h"
#include "blew_core.h"
#include "blew_priv.h"

#include "esp_attr.h"
#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "watchdog.h"

static const char *TAG = "ble_window";

static RTC_NOINIT_ATTR uint32_t s_pair_magic[2];
static bool s_pair_boot;          /* 定案：这次开机是配对剖面（main.c 与安全模式相与之后 set） */
static bool s_bt_mem_released;

bool ble_window_take_pair_boot(void)
{
    /* 原始复位原因：wd_boot_reason 锁存的是"补重启之前的真原因"，那是给告警看的；这里要的是"这次是不是 esp_restart
       进来的"。安全模式在 watchdog_init() 之后才知道，由 main.c 再相与（blew_boot_decide 的 safe_mode 传 false）。 */
    const bool sw_reset = (esp_reset_reason() == ESP_RST_SW);
    const bool armed = blew_boot_decide(s_pair_magic[0], s_pair_magic[1], sw_reset, false);
    s_pair_magic[0] = 0;   /* 一次性：不管判成什么都清掉 */
    s_pair_magic[1] = 0;
    if (armed) {
        ESP_LOGW(TAG, "RTC 标志在、软件复位 —— 这次开机进 Muse 配对剖面（设计 3.2）");
    }
    return armed;
}

void ble_window_set_pair_boot(bool active)
{
    s_pair_boot = active;
}

bool ble_window_pair_boot_active(void)
{
    return s_pair_boot;
}

bool blew_bt_mem_released(void)
{
    return s_bt_mem_released;
}

esp_err_t ble_window_request_isolated(blew_owner_t owner)
{
    if (owner != BLEW_OWNER_MUSE_PAIR) return ESP_ERR_INVALID_ARG;   /* 策略 C 只给 Muse 配对（设计 3.2） */
    if (s_pair_boot) return ESP_ERR_INVALID_STATE;                  /* 已经在配对剖面里 */

    blew_selfcheck_capture_baseline();      /* 设计 3.3：开窗前的基线（策略 C 下"开窗前" = 重启进配对剖面之前） */
    blew_boot_arm(&s_pair_magic[0], &s_pair_magic[1]);
    /* 计划内重启要记账（交叉清单 A-26）：进门这条按 L2 只记 RTC（一次配对只让出门那条进 NVS 历史），不算自愈、不进安全模式计数 */
    wd_forensics_record_planned("muse_pair_enter", false);   /* 只记 RTC；出门那条进历史（muse_pair.c） */
    ESP_LOGW(TAG, "重启进 Muse 配对剖面（策略 C：这次开机不起 Thread，配完再重启回来）");
    esp_restart();          /* CPU 复位，不重采 strap（不撞 IO28）；hub_store / config 的 shutdown handler 先落盘 */
    return ESP_FAIL;        /* 不会到这里 */
}

void ble_window_release_bt_mem(void)
{
    if (s_pair_boot || s_bt_mem_released) return;
    const size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    /* 要求控制器还在 IDLE（没人 init 过）：正常开机里 BT 只经 ble_window，而 ble_window 只在配对剖面开窗口 */
    esp_err_t err = esp_bt_mem_release(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_bt_mem_release 失败: %s —— BT 的静态内存这次开机还占着", esp_err_to_name(err));
        return;
    }
    s_bt_mem_released = true;
    ESP_LOGI(TAG, "BT 静态内存还给了堆：内部空闲 %u → %u（策略 C，正常开机不用 BT）",
             (unsigned)before, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
