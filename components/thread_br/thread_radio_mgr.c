#include "thread_br.h"

#include "esp_log.h"
#include "esp_coex_i154.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef CONFIG_OPENTHREAD_ENABLED
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/thread.h"
#endif

static const char *TAG = "radio_mgr";

static SemaphoreHandle_t s_radio_mutex = NULL;
static volatile bool s_paused = false;

// Expose paused state to thread_br.c
bool thread_br_radio_is_paused(void) { return s_paused; }

__attribute__((constructor))
static void radio_mgr_init(void)
{
    s_radio_mutex = xSemaphoreCreateMutex();
}

esp_err_t thread_br_pause(void)
{
    if (!thread_br_is_running()) return ESP_OK;
    if (!s_radio_mutex) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_radio_mutex, portMAX_DELAY);
    if (s_paused) {
        xSemaphoreGive(s_radio_mutex);
        return ESP_OK;
    }

    /* 只走 OpenThread 自己的 API，**不要**碰 esp_ieee802154_*。
       RADIO_MODE_NATIVE 下驱动归 OT 端口所有，两个方向都验证过：
         - 只 esp_ieee802154_disable()：OT 下一次 MAC 操作又把它打开，暂停期间
           照样 Send Advertisement / Send Announce，SoftAP 关联 8/10 失败；
         - OT 停了之后再 esp_ieee802154_disable()：暂停有效（10/10），但 resume
           时 otThreadSetEnabled(true) 起不来，Role 停在 disabled 再也不回来。
       otThreadSetEnabled(false) 本身就会让 MAC 进 sleep，射频该停已经停了。 */
#ifdef CONFIG_OPENTHREAD_ENABLED
    /* 有界等锁：pause 由 NETWORK_EVENT 处理函数调用，跑在默认事件循环任务里。
       portMAX_DELAY 万一遇上 OT mainloop 卡住持锁，整个事件循环就停摆，网络
       状态机再也收不到事件。正常路径实测 36ms 拿到锁。 */
    if (esp_openthread_lock_acquire(pdMS_TO_TICKS(2000))) {
        otInstance *inst = esp_openthread_get_instance();
        if (inst) {
            otError e1 = otThreadSetEnabled(inst, false);
            otError e2 = otIp6SetEnabled(inst, false);
            if (e1 != OT_ERROR_NONE || e2 != OT_ERROR_NONE) {
                ESP_LOGE(TAG, "OT stop failed (thread=%d ip6=%d) — 802.15.4 still "
                              "on air, SoftAP provisioning will likely fail", e1, e2);
            }
        }
        esp_openthread_lock_release();
    } else {
        ESP_LOGE(TAG, "OT lock timeout — Thread not stopped, SoftAP provisioning "
                      "will likely fail");
    }
#endif

    esp_coex_ieee802154_status_disable();  /* tell coex layer: 802.15.4 inactive */
    s_paused = true;
    ESP_LOGI(TAG, "Thread radio paused (OT stopped, coex released)");

    xSemaphoreGive(s_radio_mutex);
    return ESP_OK;
}

esp_err_t thread_br_resume(void)
{
    if (!s_radio_mutex) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_radio_mutex, portMAX_DELAY);
    if (!s_paused) {
        xSemaphoreGive(s_radio_mutex);
        return ESP_OK;
    }

    /* Pause disabled coex arbitration for 802.15.4 — without re-enabling it
       here, WiFi traffic tramples Thread for the rest of the uptime. */
    esp_coex_ieee802154_status_enable();

    /* 与 pause 对称：让 OT 用存量 dataset 重新入网（ip6 先于 thread）。 */
#ifdef CONFIG_OPENTHREAD_ENABLED
    if (esp_openthread_lock_acquire(pdMS_TO_TICKS(2000))) {
        otInstance *inst = esp_openthread_get_instance();
        if (inst) {
            otError e1 = otIp6SetEnabled(inst, true);
            otError e2 = otThreadSetEnabled(inst, true);
            if (e1 != OT_ERROR_NONE || e2 != OT_ERROR_NONE) {
                /* 不检查返回值就是上一版栽的地方：Role 停在 disabled、Thread
                   网络再也没回来，日志里却一片安静。 */
                ESP_LOGE(TAG, "OT restart failed (ip6=%d thread=%d) — Thread network "
                              "will not come back without a reboot", e1, e2);
            }
        }
        esp_openthread_lock_release();
    } else {
        ESP_LOGE(TAG, "OT lock timeout — Thread stays down until next resume");
    }
#endif

    s_paused = false;
    ESP_LOGI(TAG, "Thread radio resumed (OT restarted, coex restored)");

    xSemaphoreGive(s_radio_mutex);
    return ESP_OK;
}
