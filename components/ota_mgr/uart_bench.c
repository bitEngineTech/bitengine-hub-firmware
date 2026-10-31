/*
 * EZSP UART link check.
 *
 * Answers one narrow question safely: is the line moving bytes, and is the
 * far end answering. It is the acceptance test for hardware flow control —
 * a CTS that is enabled on one side only leaves TX permanently blocked, and
 * this reports that as a bounded failure instead of a gateway that has
 * quietly stopped passing Zigbee traffic.
 *
 * It deliberately does NOT try to measure line rate by blasting bulk data.
 * That worked while flow control was off, but with CTS honoured the far end
 * throttles: the receiver is the NCP's ASH layer (or the bootloader menu,
 * which prints ~90 bytes for every byte it consumes), neither of which drains
 * at line rate. What that measured was the receiver's drain rate, and worse,
 * the top-up loop could block indefinitely once RTS went away — which is
 * exactly how an earlier version left the MG24 stranded in its bootloader.
 *
 * For real throughput, time an MG24 update (POST /api/v1/ota/zigbee): that is
 * a receiver which actually consumes, so the number means something.
 * Measured: 243 KB in 15 s at 460800, 31 s at 115200.
 *
 * Does not reuse uart_bridge_write(): it tops the FIFO up with vTaskDelay(1),
 * which at 128-byte FIFO granularity would be reported as line time.
 */

#include "uart_bench.h"

#include "ezsp_bridge.h"
#include "uart_bridge.h"
#include "ota_mg24.h"
#include "devkit_pins.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "uart_bench";

/* 0x55 = 01010101: maximum transition density, so a marginal line shows up as
   framing errors rather than passing by luck. Must also be a byte ASH ignores:
   reserved are 0x7E/0x7D/0x11/0x13/0x18/0x1A and — easy to miss — 0xFF, which
   is ASH_WAKE, not filler (ash-protocol.h:39-48). 0x55 is none of them. */
#define BENCH_FILL_BYTE     0x55

/* One FIFO-full, deliberately. The MG24 can absorb ~160 bytes before it needs
   to drain (128-byte UART FIFO + 32-byte iostream buffer), so a burst this
   size never provokes RTS back-pressure and what is timed is the shift
   register alone. A 256-byte burst measured 69% of line rate — that was the
   NCP throttling us, not a link fault, and it made the figure useless as a
   health threshold. End-to-end throughput belongs to the OTA timing instead. */
#define BENCH_TX_BYTES      128
/* A hard ceiling, not a typical duration: 256 bytes is 5.6 ms at 460800 and
   22 ms at 115200. Anything approaching this means CTS is holding us off. */
#define BENCH_TX_DEADLINE_MS 2000
/* How long the FIFO top-up may busy-yield before backing off to sleeping.
   Sized to cover a healthy burst at the slowest rate we run (22 ms at 115200)
   with margin. Sleeping instead would quantise the result to the 1 ms tick:
   a 5.6 ms burst needing two top-ups measured 7.8 ms, i.e. 70% of line rate,
   which is scheduler granularity and not a link fault — useless as a
   threshold. Yielding for tens of milliseconds at priority 5 is harmless;
   wdt_feed runs at 22 and still preempts. */
#define BENCH_TX_SPIN_MS     40

/* ASH RST -> RSTACK. Generous: this is a full NCP reboot, ~1.2 s measured. */
#define BENCH_RST_TIMEOUT_MS 3000

/*
 * Push `len` bytes, returning microseconds on the wire, or 0 if the deadline
 * expired first. The deadline is the whole point: with flow control enabled a
 * far end that stops asserting CTS blocks TX forever, and an unbounded top-up
 * loop would spin until the board was power-cycled.
 */
static uint32_t tx_burst_us(const uint8_t *buf, size_t len, bool *blocked)
{
    uart_wait_tx_done(EZSP_UART_NUM, pdMS_TO_TICKS(500));

    const int64_t t0 = esp_timer_get_time();
    const int64_t deadline = t0 + (int64_t)BENCH_TX_DEADLINE_MS * 1000;
    size_t sent = 0;

    while (sent < len) {
        int n = uart_tx_chars(EZSP_UART_NUM, (const char *)buf + sent,
                              (uint32_t)(len - sent));
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        const int64_t now = esp_timer_get_time();
        if (now > deadline) {
            *blocked = true;
            ESP_LOGE(TAG, "TX blocked after %u/%u bytes — CTS never asserted? "
                     "Check that the MG24 firmware also has flow control on.",
                     (unsigned)sent, (unsigned)len);
            return 0;
        }
        if (now - t0 < (int64_t)BENCH_TX_SPIN_MS * 1000) {
            taskYIELD();          /* healthy case: keeps the timing honest */
        } else {
            vTaskDelay(1);        /* held off — stop burning the CPU */
        }
    }

    if (uart_wait_tx_done(EZSP_UART_NUM, pdMS_TO_TICKS(BENCH_TX_DEADLINE_MS)) != ESP_OK) {
        *blocked = true;
        ESP_LOGE(TAG, "TX FIFO never drained — line held off by CTS");
        return 0;
    }
    return (uint32_t)(esp_timer_get_time() - t0);
}

/* bytes/sec -> bits/sec on an 8N1 line (10 bits per byte incl. start+stop) */
static uint32_t bps_from(uint32_t bytes, uint32_t us)
{
    if (!us) return 0;
    return (uint32_t)(((uint64_t)bytes * 10ULL * 1000000ULL) / us);
}

esp_err_t uart_bench_run(uart_bench_result_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (ota_mg24_busy()) return ESP_ERR_INVALID_STATE;

    memset(out, 0, sizeof(*out));

    const bridge_mode_t prev_mode = ezsp_bridge_get_mode();
    if (prev_mode != BRIDGE_MODE_DISABLED) {
        esp_err_t err = ezsp_bridge_switch_mode(BRIDGE_MODE_DISABLED);
        if (err != ESP_OK) {
            strlcpy(out->note, "could not disable bridge", sizeof(out->note));
            return err;
        }
    }
    esp_err_t err = ezsp_bridge_ota_claim();
    if (err != ESP_OK) {
        strlcpy(out->note, "bridge busy", sizeof(out->note));
        ezsp_bridge_switch_mode(prev_mode);
        return err;
    }

    /* ---- TX: does the line move, and at what rate ---- */
    uint8_t chunk[BENCH_TX_BYTES];
    memset(chunk, BENCH_FILL_BYTE, sizeof(chunk));

    uart_bridge_flush_input();
    out->tx.baud       = uart_bridge_get_baud();
    out->tx.bytes      = BENCH_TX_BYTES;
    out->tx.theory_bps = (uint32_t)out->tx.baud;
    out->tx.us         = tx_burst_us(chunk, sizeof(chunk), &out->tx_blocked);
    out->tx.bps        = bps_from(out->tx.bytes, out->tx.us);
    out->tx.efficiency_pct = out->tx.theory_bps
        ? (uint8_t)((uint64_t)out->tx.bps * 100 / out->tx.theory_bps) : 0;

    ESP_LOGI(TAG, "TX %u bytes @%d: %s%u us -> %u bps (%u%% of line)",
             (unsigned)out->tx.bytes, out->tx.baud,
             out->tx_blocked ? "BLOCKED after " : "",
             (unsigned)out->tx.us, (unsigned)out->tx.bps,
             (unsigned)out->tx.efficiency_pct);

    /* ---- RX: the NCP just took 256 bytes of junk into a frame that never got
       a flag byte. ASH RST both resynchronises it and proves the return path
       carries data. Timing it is a full NCP reboot, not a link latency. ---- */
    static const uint8_t ash_rst[] = {0x1A, 0xC0, 0x38, 0xBC, 0x7E};
    uart_bridge_flush_input();
    const int64_t r0 = esp_timer_get_time();
    uart_bridge_write(ash_rst, sizeof(ash_rst), 500);

    uint8_t rb[64];
    TickType_t dl = xTaskGetTickCount() + pdMS_TO_TICKS(BENCH_RST_TIMEOUT_MS);
    while (xTaskGetTickCount() < dl && !out->ncp_alive) {
        int n = uart_bridge_read(rb, sizeof(rb), 50);
        for (int i = 0; i < n; i++) {
            if (rb[i] == 0x7E) {
                out->ncp_reset_ready_us = (uint32_t)(esp_timer_get_time() - r0);
                out->ncp_alive = true;
                break;
            }
        }
    }
    ESP_LOGI(TAG, "ASH RST -> RSTACK (full NCP reboot): %s (%u us)",
             out->ncp_alive ? "ok" : "NO RESPONSE",
             (unsigned)out->ncp_reset_ready_us);

    const bridge_stats_t *st = ezsp_bridge_get_stats();
    out->uart_rx_errors = st ? st->uart_rx_errors : 0;

    if (out->tx_blocked) {
        strlcpy(out->note, "TX blocked — CTS not asserted by the MG24; "
                "is its firmware built with flow control?", sizeof(out->note));
    } else if (!out->ncp_alive) {
        strlcpy(out->note, "TX fine but no RSTACK — MG24 not answering",
                sizeof(out->note));
    }

    ezsp_bridge_ota_release();
    if (ezsp_bridge_get_mode() != prev_mode) {
        ezsp_bridge_switch_mode(prev_mode);
    }
    return ESP_OK;
}
