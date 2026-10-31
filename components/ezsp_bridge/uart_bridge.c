#include "uart_bridge.h"
#include "bridge_stats.h"
#include "ezsp_bridge.h"
#include "devkit_pins.h"

#include <string.h>
#include "driver/uart.h"
#include "soc/uart_reg.h"
#include "esp_cpu.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "uart_bridge";

static QueueHandle_t s_uart_event_queue;
static int           s_baud = EZSP_UART_BAUD;

esp_err_t uart_bridge_init(void)
{
    const uart_config_t cfg = {
        .baud_rate           = EZSP_UART_BAUD,
        .data_bits           = UART_DATA_8_BITS,
        .parity              = UART_PARITY_DISABLE,
        .stop_bits           = UART_STOP_BITS_1,
#ifdef CONFIG_BITENGINE_EZSP_HW_FLOWCTRL
        /* Both chips must be flashed as a pair — an MG24 built with flow
           control None never drives our CTS and every TX blocks forever.
           See devkit_pins.h for the pin-direction rationale. */
        .flow_ctrl           = UART_HW_FLOWCTRL_CTS_RTS,
#else
        /* HW flow control OFF (CLAUDE.md decision): the shipped MG24 NCP is
           built with flow control None and does not drive our CTS. With
           CTS_RTS enabled here, a floating CTS blocks all TX. */
        .flow_ctrl           = UART_HW_FLOWCTRL_DISABLE,
#endif
        .rx_flow_ctrl_thresh = 120,
        .source_clk          = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(
        uart_param_config(EZSP_UART_NUM, &cfg),
        TAG, "UART param config failed");

    ESP_RETURN_ON_ERROR(
        uart_set_pin(EZSP_UART_NUM,
                     EZSP_UART_TX_PIN, EZSP_UART_RX_PIN,
#ifdef CONFIG_BITENGINE_EZSP_HW_FLOWCTRL
                     EZSP_UART_RTS_PIN, EZSP_UART_CTS_PIN),
#else
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
#endif
        TAG, "UART set pin failed");

    ESP_RETURN_ON_ERROR(
        uart_driver_install(EZSP_UART_NUM,
                            2048,   // RX buffer
                            0,      // TX buffer (0 = direct FIFO write)
                            8,      // event queue size
                            &s_uart_event_queue,
                            0),
        TAG, "UART driver install failed");

    s_baud = EZSP_UART_BAUD;
#ifdef CONFIG_BITENGINE_EZSP_HW_FLOWCTRL
    ESP_LOGI(TAG, "UART%d: %d baud, 8N1, RTS=IO%d CTS=IO%d",
             EZSP_UART_NUM, EZSP_UART_BAUD,
             EZSP_UART_RTS_PIN, EZSP_UART_CTS_PIN);
#else
    ESP_LOGI(TAG, "UART%d: %d baud, 8N1, no flow control", EZSP_UART_NUM, EZSP_UART_BAUD);
#endif
    return ESP_OK;
}

esp_err_t uart_bridge_set_baud(int baud)
{
    if (baud < 9600 || baud > 4000000) return ESP_ERR_INVALID_ARG;
    /* Guards the flush path below: before uart_bridge_init the queue handle
       is NULL and xQueueReset would fault. */
    if (!s_uart_event_queue) return ESP_ERR_INVALID_STATE;
    if (baud == s_baud) return ESP_OK;

    /* Drain TX first: uart_set_baudrate reprograms the divider immediately,
       so a byte still in the FIFO would finish shifting out at the new rate
       and be read as a framing error on the far side. */
    esp_err_t err = uart_wait_tx_done(EZSP_UART_NUM, pdMS_TO_TICKS(500));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TX not drained before baud switch (%s)", esp_err_to_name(err));
    }

    ESP_RETURN_ON_ERROR(uart_set_baudrate(EZSP_UART_NUM, baud),
                        TAG, "set baudrate %d failed", baud);

    /* Anything already buffered was clocked in at the old rate. */
    uart_flush_input(EZSP_UART_NUM);
    xQueueReset(s_uart_event_queue);

    s_baud = baud;
    ESP_LOGI(TAG, "UART%d baud -> %d", EZSP_UART_NUM, baud);
    return ESP_OK;
}

int uart_bridge_get_baud(void)
{
    return s_baud;
}

void uart_bridge_flush_input(void)
{
    uart_flush_input(EZSP_UART_NUM);
    xQueueReset(s_uart_event_queue);
}

esp_err_t uart_bridge_wait_tx_done(int timeout_ms)
{
    return uart_wait_tx_done(EZSP_UART_NUM, pdMS_TO_TICKS(timeout_ms));
}

int uart_bridge_read(uint8_t *buf, int max_len, int timeout_ms)
{
    // Drain UART error events (non-blocking)
    uart_event_t event;
    while (xQueueReceive(s_uart_event_queue, &event, 0)) {
        switch (event.type) {
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
            bridge_stats_get()->uart_rx_errors++;
            ESP_LOGW(TAG, "UART RX overflow (type=%d)", event.type);
            uart_flush_input(EZSP_UART_NUM);
            xQueueReset(s_uart_event_queue);
            esp_event_post(BRIDGE_EVENT, BRIDGE_EVENT_UART_ERROR,
                           &event.type, sizeof(event.type), 0);
            break;
        case UART_PARITY_ERR:
        case UART_FRAME_ERR:
            bridge_stats_get()->uart_rx_errors++;
            ESP_LOGW(TAG, "UART error: type=%d", event.type);
            esp_event_post(BRIDGE_EVENT, BRIDGE_EVENT_UART_ERROR,
                           &event.type, sizeof(event.type), 0);
            break;
        default:
            break;
        }
    }

    return uart_read_bytes(EZSP_UART_NUM, buf, max_len, pdMS_TO_TICKS(timeout_ms));
}

int uart_bridge_write(const uint8_t *buf, int len, int timeout_ms)
{
    // TX buffer = 0, so uart_write_bytes would block forever if CTS held.
    // Use uart_tx_chars (non-blocking FIFO write) with manual timeout.
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    int total = 0;

    while (total < len) {
        if (xTaskGetTickCount() >= deadline) {
            break;
        }
        int n = uart_tx_chars(EZSP_UART_NUM, (const char *)(buf + total),
                              len - total);
        if (n > 0) {
            total += n;
        } else {
            vTaskDelay(1);
        }
    }
    return total;
}

esp_err_t uart_bridge_get_rx_buffered(size_t *out_len)
{
    return uart_get_buffered_data_len(EZSP_UART_NUM, out_len);
}

/* ---- 线路电平采样（见 uart_bridge.h）---- */
#define LINE_SAMPLE_MAX_MS   5000

void uart_bridge_sample_lines(uint32_t ms, uart_line_sample_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (ms == 0) ms = 1000;
    if (ms > LINE_SAMPLE_MAX_MS) ms = LINE_SAMPLE_MAX_MS;

    int prev = -1;
    uint32_t run = 0;
    for (uint32_t i = 0; i < ms; i++) {
        uint32_t st = REG_READ(UART_STATUS_REG(EZSP_UART_NUM));
        int cts = (st & UART_CTSN) ? 1 : 0;
        int rxd = (st & UART_RXD)  ? 1 : 0;

        out->samples++;
        if (cts) {
            out->cts_deasserted++;
            run++;
            if (run > out->cts_longest_deassert_ms) out->cts_longest_deassert_ms = run;
        } else {
            run = 0;
        }
        if (!rxd) out->rxd_low++;
        if (prev >= 0 && cts != prev) out->cts_transitions++;
        prev = cts;
        out->cts_now = (uint8_t)cts;
        out->rxd_now = (uint8_t)rxd;

        vTaskDelay(1);   /* tick = 1ms（CONFIG_FREERTOS_HZ=1000），约 1kHz */
    }
    out->duration_ms = ms;
}

bool uart_bridge_line_says_absent(uint32_t ms)
{
    uart_line_sample_t s;
    uart_bridge_sample_lines(ms, &s);
    /* 要求**整窗**都解除断言。实测（2026-09-15）：MG24 运行时无论空闲还是带
       ZHA 流量，8 个窗口共 34000 个采样点，ctsn 一次都没抬起来过；而复位那一下
       只持续 63ms。所以窗口取几百毫秒就已经远离正常抖动。 */
    return s.samples > 0 && s.cts_deasserted == s.samples;
}
