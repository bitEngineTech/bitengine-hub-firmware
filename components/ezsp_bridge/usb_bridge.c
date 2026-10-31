#include "usb_bridge.h"
#include "ot_log_level.h"   /* BITENGINE_OT_ESP_LOG_LEVEL：退出 USB 模式时把 OPENTHREAD 设回去 */
#include "uart_bridge.h"
#include "bridge_stats.h"
#include "ezsp_bridge.h"

#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "usb_bridge";
static volatile bool s_running = false;
static SemaphoreHandle_t s_exit_sem;       /* counting, created once */
static int               s_tasks_started = 0;

static void usb_to_uart_task(void *arg)
{
    uint8_t buf[256];
    while (s_running) {
        if (ezsp_bridge_get_state() != BRIDGE_STATE_RUNNING) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        int len = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (len > 0) {
            int written = uart_bridge_write(buf, len, 1000);
            bridge_stats_t *st = bridge_stats_get();
            st->usb_rx_bytes += len;
            st->uart_tx_bytes += (written > 0 ? written : 0);
            if (written < len) {
                st->uart_tx_timeouts++;
            }
        }
    }
    xSemaphoreGive(s_exit_sem);
    vTaskDelete(NULL);
}

static void uart_to_usb_task(void *arg)
{
    uint8_t buf[256];
    while (s_running) {
        if (ezsp_bridge_get_state() != BRIDGE_STATE_RUNNING) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        int len = uart_bridge_read(buf, sizeof(buf), 50);
        if (len > 0) {
            /* write_bytes can return short on a full host-side buffer —
               count only what actually went out */
            int sent = usb_serial_jtag_write_bytes((const char *)buf, len,
                                                   pdMS_TO_TICKS(100));
            bridge_stats_t *st = bridge_stats_get();
            st->uart_rx_bytes += len;
            st->usb_tx_bytes  += (sent > 0 ? sent : 0);
        }
    }
    xSemaphoreGive(s_exit_sem);
    vTaskDelete(NULL);
}

esp_err_t usb_bridge_start(void)
{
    if (!s_exit_sem) {
        s_exit_sem = xSemaphoreCreateCounting(2, 0);
        if (!s_exit_sem) return ESP_ERR_NO_MEM;
    }
    if (s_tasks_started > 0) return ESP_ERR_INVALID_STATE;

    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = 1024,
        .tx_buffer_size = 1024,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB Serial/JTAG driver install failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    /* The secondary console (CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG)
       shares this port: any log line would interleave with the EZSP byte
       stream and corrupt it. Silence logging while USB mode is active. */
    ESP_LOGW(TAG, "USB CDC bridge starting — logging suspended until USB mode exits");
    esp_log_level_set("*", ESP_LOG_NONE);

    s_running = true;
    if (xTaskCreate(usb_to_uart_task, "usb_to_uart", 3072, NULL, 20, NULL) != pdPASS) {
        usb_bridge_stop();
        return ESP_ERR_NO_MEM;
    }
    s_tasks_started++;
    if (xTaskCreate(uart_to_usb_task, "uart_to_usb", 3072, NULL, 21, NULL) != pdPASS) {
        usb_bridge_stop();
        return ESP_ERR_NO_MEM;
    }
    s_tasks_started++;
    return ESP_OK;
}

void usb_bridge_stop(void)
{
    s_running = false;

    /* Join bridge tasks before uninstalling the driver they're using —
       a fixed delay would leave a use-after-free window. */
    for (int i = 0; i < s_tasks_started; i++) {
        if (xSemaphoreTake(s_exit_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
            esp_log_level_set("*", CONFIG_LOG_DEFAULT_LEVEL);
            ESP_LOGE(TAG, "USB bridge task did not exit within 2s");
            esp_log_level_set("*", ESP_LOG_NONE);
        }
    }
    s_tasks_started = 0;

    usb_serial_jtag_driver_uninstall();

    esp_log_level_set("*", CONFIG_LOG_DEFAULT_LEVEL);
#ifdef BITENGINE_OT_ESP_LOG_LEVEL
    /* 设 "*" 清掉了按标签的级别：OPENTHREAD 不设回去就是 INFO，代播库每条 SRP 注册的 INFO 又会忙等写串口
       （ot_log_level.h 的注释） */
    esp_log_level_set("OPENTHREAD", BITENGINE_OT_ESP_LOG_LEVEL);
#endif
    ESP_LOGI(TAG, "USB CDC bridge stopped — logging restored");
}
