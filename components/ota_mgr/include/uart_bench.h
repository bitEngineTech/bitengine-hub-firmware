#pragma once

/*
 * Measured throughput of the C5 <-> MG24 UART, per direction.
 *
 * Run it to confirm a configuration change actually landed: after switching
 * the link to 460800, or after enabling hardware flow control, TX must still
 * complete at ~line rate and the NCP must still answer. CTS enabled on one
 * side only shows up here as tx_blocked rather than as a gateway that
 * mysteriously stops passing Zigbee traffic.
 *
 * This is a health check, not a throughput benchmark — for real throughput
 * time an MG24 update, whose receiver actually consumes the data.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      baud;            /* line rate this leg ran at */
    uint32_t bytes;           /* payload moved */
    uint32_t us;              /* time on the wire */
    uint32_t bps;             /* measured bits/sec (8N1: 10 bits per byte) */
    uint32_t theory_bps;      /* == baud */
    uint8_t  efficiency_pct;  /* bps as a percentage of theory_bps */
} uart_bench_leg_t;

typedef struct {
    uart_bench_leg_t tx;       /* C5 -> MG24 at the current line rate */

    /* TX could not complete inside the deadline: the far end is not asserting
       CTS. The usual cause is flow control enabled on one side only. */
    bool     tx_blocked;
    bool     ncp_alive;        /* ASH RST answered, i.e. the return path works */
    /* RST -> RSTACK. NOT link latency: an ASH RST makes the NCP perform a
       full software reset, so this is chip-reset + bootloader + application
       start + ASH ready. Measured ~1.16 s. Useful as the cost of a watchdog
       MG24 reset; useless as a measure of per-command turnaround. */
    uint32_t ncp_reset_ready_us;
    uint32_t uart_rx_errors;   /* cumulative framing/overrun count afterwards */
    char     note[80];         /* why a leg was skipped, when one was */
} uart_bench_result_t;

/**
 * Run the check. Takes the UART briefly: the bridge is dropped to
 * BRIDGE_MODE_DISABLED (TCP clients get a clean FIN) and restored on exit,
 * and the NCP is reset, so Zigbee is down for a couple of seconds.
 * Blocking, ~2 s. The REST layer runs it on its own task.
 *
 * Never enters the MG24 bootloader — an earlier version did, to measure the
 * return path in bulk, and a blocked TX left the chip stranded there.
 */
esp_err_t uart_bench_run(uart_bench_result_t *out);

#ifdef __cplusplus
}
#endif
