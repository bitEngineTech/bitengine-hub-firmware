#include "ota_mg24.h"
#include "gbl_image.h"
#include "mg24_bl_entry.h"
#include "ncp_unlock.h"
#include "mg24_image_record.h"
#include "xmodem.h"

#include "ezsp_bridge.h"
#include "ncp_version.h"
#include "ncp_cache.h"            /* 进 bootloader 前作废 NCP 读数缓存 */
#include "config_mgr.h"          /* 升级后重读版本时顺带下发地区（docs/14） */
#include "uart_bridge.h"
#include "watchdog.h"
#include "devkit_pins.h"

#include "button.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <string.h>

static const char *TAG = "ota_mg24";

/* GBL container constants, validation, signature verification and stripping
   live in gbl_image.h / gbl_image.c (pure C, exercised on the host by
   test_host/run.sh against real ECDSA). This file only sequences them. */

/* Bootloader menu strings we key off, from
   sdk/platform/bootloader/communication/xmodem-uart/btl_comm_xmodem_common.c */
#define BL_BEGIN_UPLOAD         "begin upload"
#define BL_UPLOAD_COMPLETE      "Serial upload complete"
#define BL_UPLOAD_ABORTED       "Serial upload aborted"

/* Timings. The bootloader itself never times out (BTL_XMODEM_IDLE_TIMEOUT 0),
   so every bound here is ours and exists to stop a wedged session from
   holding the UART forever. */
#define BL_ENTRY_TIMEOUT_MS     3000   /* reset -> menu banner */
#define BL_BEGIN_TIMEOUT_MS     2000   /* '1' -> "begin upload" */
#define BL_START_C_TIMEOUT_MS   5000   /* -> first 'C' (it retries ~1/s) */
#define BL_BLOCK_ACK_TIMEOUT_MS 3000   /* block -> ACK (includes flash write) */
#define BL_EOT_ACK_TIMEOUT_MS   8000   /* EOT -> ACK (final image verify) */
#define BL_BLOCK_RETRIES        5
#define MG24_BOOT_SETTLE_MS     1500   /* app start after leaving bootloader */

/* Padding for the final short block. Content is irrelevant — parser_finalize
   returns as soon as the GBL end tag is consumed (btl_gbl_parser.c:845), so
   the filler is never parsed — but 0xFF matches erased flash and what the
   Silabs host tools emit. */
#define XMODEM_PAD_BYTE         0xFF

/* The flash session's execution context. Two callers, two ways of supplying it:
   the web upload spawns a task (an httpd worker must not sit through ~40 s of
   XMODEM), while the cloud installer lends its own stack — see the note in
   commit_staged() for why that is not a style choice. */
#define MG24_OTA_TASK_STACK     4096
#define MG24_OTA_TASK_PRIO      5

/* ---- module state ---- */
static SemaphoreHandle_t s_lock;                 /* guards s_status + buffer */
static mg24_ota_status_t s_status;
static uint8_t          *s_image;                /* staging buffer (PSRAM) */
static size_t            s_image_cap;            /* bytes reserved */
static size_t            s_image_len;            /* bytes received so far */
static volatile bool     s_task_running;
static ota_mg24_source_t s_source;               /* who staged the image */
static uint8_t           s_upload_sha[32];        /* sha256 of the bytes as uploaded */
static size_t            s_upload_len;
static const char       *s_reject;                /* REST error code of the last commit rejection */
static ncp_unlock_t      s_unlock;                /* developer unlock state, guarded by s_lock; RAM only */
static int32_t           s_image_nvm3c;           /* T9.25: nvm3c= of the staged image, 0 = none / not trusted */
static char              s_guard_why[112];        /* detail text for a downgrade-guard rejection */

static int64_t now_s(void)
{
    return esp_timer_get_time() / 1000000;
}

/*
 * Policy for an image that carries NO signature tag. A signature that is
 * present is always verified regardless of this — see ota_mg24_commit().
 * The cloud never ships unsigned images, so that path is strict even in a
 * developer build and even while unlocked; the Kconfig switch and the
 * button-confirmed unlock only govern the web upload. Caller holds s_lock.
 */
static bool signature_required(void)
{
    if (s_source == OTA_MG24_SOURCE_CLOUD) return true;
#if CONFIG_BITENGINE_MG24_REQUIRE_SIGNED_GBL
    return !ncp_unlock_active(&s_unlock, now_s());
#else
    return false;
#endif
}

/* BUTTON_EVENT_HOLD_CONFIRM: runs on the default event loop task. Only ever
   confirms — arming is the authenticated REST call's job, so a stray press
   with nothing pending changes nothing. */
static void on_button_hold_confirm(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    if (!s_lock) return;
    const uint32_t held = data ? *(const uint32_t *)data : 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool confirmed = ncp_unlock_confirm(&s_unlock, now_s());
    xSemaphoreGive(s_lock);

    if (confirmed) {
        ESP_LOGW(TAG, "NCP developer unlock confirmed by button (%" PRIu32 " ms hold): "
                      "unsigned images accepted for %d s", held, NCP_UNLOCK_WINDOW_S);
    } else {
        ESP_LOGI(TAG, "button hold ignored: no unlock request pending");
    }
}

/* ---------------------------------------------------------------- helpers */

static void status_set(mg24_ota_state_t state, const char *detail, esp_err_t err)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = state;
    s_status.err   = err;
    if (detail) {
        strlcpy(s_status.detail, detail, sizeof(s_status.detail));
    }
    xSemaphoreGive(s_lock);
}

static void status_progress(uint32_t sent, uint32_t total)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.sent_blocks  = sent;
    s_status.total_blocks = total;
    s_status.percent      = total ? (uint8_t)((sent * 100U) / total) : 0;
    xSemaphoreGive(s_lock);
}

/*
 * Read whatever is already buffered, else block for a single byte.
 *
 * uart_read_bytes() only returns early once it has `max_len` bytes — asking
 * for 64 when the peer sends a one-byte ACK burns the whole timeout every
 * time. Measured: that turned a 12 ms/block transfer into 66 ms/block, ~2 min
 * for a 240 KB image. Draining the buffered count first, and otherwise
 * waiting for exactly one byte, keeps the ACK latency at the wire latency.
 */
static int read_some(uint8_t *buf, int max_len, int timeout_ms)
{
    size_t avail = 0;
    if (uart_bridge_get_rx_buffered(&avail) == ESP_OK && avail > 0) {
        int want = (avail > (size_t)max_len) ? max_len : (int)avail;
        return uart_bridge_read(buf, want, 0);
    }
    return uart_bridge_read(buf, 1, timeout_ms);
}

/* Read UART until `needle` appears or the deadline passes.
   Returns true on match. Any text seen is copied into `sink` (if given) so a
   failure can report what the bootloader actually said instead of "timeout". */
static bool wait_for_text(const char *needle, int timeout_ms,
                          char *sink, size_t sink_size)
{
    const size_t nlen = strlen(needle);
    char window[96];
    size_t wlen = 0;
    size_t slen = 0;

    memset(window, 0, sizeof(window));
    if (sink && sink_size) sink[0] = '\0';

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        uint8_t buf[64];
        int n = read_some(buf, sizeof(buf), 50);
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];

            if (sink && sink_size && slen + 1 < sink_size) {
                /* keep it printable so it can go straight into JSON */
                sink[slen++] = (c >= 0x20 && c < 0x7F) ? c : ' ';
                sink[slen]   = '\0';
            }

            /* sliding window, big enough for any needle we use */
            if (wlen + 1 >= sizeof(window)) {
                memmove(window, window + 1, wlen - 1);
                wlen--;
            }
            window[wlen++] = c;
            window[wlen]   = '\0';

            if (wlen >= nlen && memcmp(window + wlen - nlen, needle, nlen) == 0) {
                return true;
            }
        }
    }
    return false;
}

/* Wait for one of the XMODEM control bytes in `accept`. Returns the byte, or
   0 on timeout. Bytes not in `accept` are discarded — the bootloader emits
   banner text between phases. */
static uint8_t wait_for_ctrl(const char *accept, int timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        uint8_t buf[64];
        int n = read_some(buf, sizeof(buf), 50);
        for (int i = 0; i < n; i++) {
            /* buf[i] != 0 matters: strchr matches the terminator, so a NUL on
               the wire would otherwise look like a control byte and be
               returned as 0 — indistinguishable from "timed out". */
            if (buf[i] && strchr(accept, (char)buf[i])) return buf[i];
        }
    }
    return 0;
}

static bool send_all(const uint8_t *data, int len, int timeout_ms)
{
    return uart_bridge_write(data, len, timeout_ms) == len;
}

/* ------------------------------------------------- bootloader entry / exit */

/* Adapters that give mg24_bl_entry.c (host-tested, no ESP-IDF) the UART. */
static bool bl_io_send(void *ctx, const uint8_t *data, int len)
{
    (void)ctx;
    if (!send_all(data, len, 500)) return false;
    uart_bridge_wait_tx_done(200);
    return true;
}

static bool bl_io_wait_text(void *ctx, const char *needle, int timeout_ms,
                            char *sink, size_t sink_size)
{
    (void)ctx;
    return wait_for_text(needle, timeout_ms, sink, sink_size);
}

static int  bl_io_get_baud(void *ctx) { (void)ctx; return uart_bridge_get_baud(); }
static bool bl_io_set_baud(void *ctx, int baud) { (void)ctx; return uart_bridge_set_baud(baud) == ESP_OK; }
static void bl_io_flush(void *ctx) { (void)ctx; uart_bridge_flush_input(); }

static const mg24_bl_io_t s_bl_io = {
    .ctx         = NULL,
    .send        = bl_io_send,
    .wait_text   = bl_io_wait_text,
    .get_baud    = bl_io_get_baud,
    .set_baud    = bl_io_set_baud,
    .flush_input = bl_io_flush,
};

/*
 * Enter the Gecko bootloader.
 *
 * ⚠️ Deliberately never touches MG24_RESET_PIN (IO23). That line is tied into
 * the RS706 supervisor's RESET net via R113 and reaches the C5's own EN via
 * R58 — driving it low is a whole-board reset that would kill this task
 * mid-update (mg24_reset.c:80-84). We hold nBOOT low and make the running
 * image reset itself instead; the bootloader samples nBOOT on every reset
 * regardless of cause.
 *
 * What to say on the UART lives in mg24_bl_entry.c: ASH RST for the NCP, a
 * nudge for a bootloader that is already parked, and a CLI `reset` for a
 * RAILtest image (docs/test_cases_rf_bench.md) — without that last step the
 * only way back from RAILtest was J-Link. A dead MG24 still needs J-Link, and
 * this reports ESP_ERR_INVALID_RESPONSE rather than hanging.
 */
static esp_err_t enter_bootloader(char *detail, size_t detail_size)
{
    ESP_RETURN_ON_FALSE(mg24_set_nboot(0) == ESP_OK, ESP_ERR_NOT_SUPPORTED,
                        TAG, "cannot drive nBOOT");

    /* Let the level settle before the reset latches it. The bootloader's own
       sampler charges the net for ~100 loop iterations, then reads it. */
    vTaskDelay(pdMS_TO_TICKS(20));

    switch (mg24_bl_entry_run(&s_bl_io, EZSP_UART_BAUD,
                              CONFIG_BITENGINE_MG24_BOOTLOADER_BAUD,
                              BL_ENTRY_TIMEOUT_MS, detail, detail_size)) {
    case MG24_BL_VIA_ASH_RST:
        return ESP_OK;
    case MG24_BL_VIA_NUDGE:
        ESP_LOGW(TAG, "MG24 was already in the bootloader");
        return ESP_OK;
    case MG24_BL_VIA_CLI_RESET:
        ESP_LOGW(TAG, "MG24 ignored ASH RST but reset on a CLI `reset` (RAILtest image?)");
        return ESP_OK;
    case MG24_BL_IO_ERROR:
        return ESP_FAIL;
    case MG24_BL_NO_PROMPT:
    default:
        return ESP_ERR_INVALID_RESPONSE;
    }
}

/* Release nBOOT and boot the application.
 *
 * Order matters: nBOOT must go high *before* the '2' (run) command. '2' makes
 * the bootloader reset with reason GO, and the reset path re-samples nBOOT —
 * still low would drop straight back into the bootloader.
 */
static esp_err_t exit_bootloader(void)
{
    esp_err_t err = mg24_set_nboot(1);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Force the bootloader's rate before speaking to it rather than assuming
       we are still where enter_bootloader() left us. Recovery matters here:
       if the C5 rebooted while the MG24 sat parked in the bootloader (it has
       no idle timeout, so it waits indefinitely), our UART is back at
       EZSP_UART_BAUD and a '2' sent at that rate is noise — the MG24 would be
       stranded with no way out but J-Link. */
    const int bl_baud = CONFIG_BITENGINE_MG24_BOOTLOADER_BAUD;
    if (uart_bridge_get_baud() != bl_baud) {
        err = uart_bridge_set_baud(bl_baud);
        if (err != ESP_OK) return err;
    }

    const uint8_t run = '2';
    send_all(&run, 1, 500);
    uart_bridge_wait_tx_done(200);

    if (uart_bridge_get_baud() != EZSP_UART_BAUD) {
        err = uart_bridge_set_baud(EZSP_UART_BAUD);
        if (err != ESP_OK) return err;
    }

    vTaskDelay(pdMS_TO_TICKS(MG24_BOOT_SETTLE_MS));
    uart_bridge_flush_input();
    return ESP_OK;
}

/* ------------------------------------------------------- XMODEM transfer */

static esp_err_t xmodem_send_image(const uint8_t *img, size_t len,
                                   char *detail, size_t detail_size)
{
    /* ceil() — and never one block more. Feeding a whole extra block after
       the GBL end tag makes the parser return PARSER_EOF and the bootloader
       cancels the transfer (btl_gbl_parser.c:848). A short final block padded
       inside itself is fine because parser_finalize returns immediately. */
    const uint32_t total = (uint32_t)((len + XMODEM_DATA_SIZE - 1) / XMODEM_DATA_SIZE);

    const uint8_t start = '1';
    if (!send_all(&start, 1, 500)) {
        strlcpy(detail, "UART write failed sending '1'", detail_size);
        return ESP_FAIL;
    }
    if (!wait_for_text(BL_BEGIN_UPLOAD, BL_BEGIN_TIMEOUT_MS, detail, detail_size)) {
        strlcpy(detail, "bootloader did not acknowledge upload command", detail_size);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* The bootloader now polls with 'C' once a second, up to 60 times. */
    uint8_t c = wait_for_ctrl((const char[]){XMODEM_C, XMODEM_CAN, 0},
                              BL_START_C_TIMEOUT_MS);
    if (c != XMODEM_C) {
        strlcpy(detail, c == XMODEM_CAN ? "bootloader cancelled before transfer"
                                        : "no 'C' handshake from bootloader",
                detail_size);
        return ESP_ERR_INVALID_RESPONSE;
    }

    status_progress(0, total);

    uint8_t packet[XMODEM_PACKET_SIZE];
    for (uint32_t blk = 0; blk < total; blk++) {
        const size_t off  = (size_t)blk * XMODEM_DATA_SIZE;
        const size_t take = (len - off) < XMODEM_DATA_SIZE
                          ? (len - off) : XMODEM_DATA_SIZE;

        /* XMODEM numbers the first data block 1 and wraps 255 -> 0; uint8_t
           arithmetic does the wrap for free. */
        if (!xmodem_build_packet(packet, (uint8_t)(blk + 1),
                                 img + off, take, XMODEM_PAD_BYTE)) {
            strlcpy(detail, "internal: packet build failed", detail_size);
            return ESP_FAIL;
        }

        bool acked = false;
        for (int attempt = 0; attempt < BL_BLOCK_RETRIES && !acked; attempt++) {
            if (!send_all(packet, XMODEM_PACKET_SIZE, 2000)) {
                strlcpy(detail, "UART write failed mid-transfer", detail_size);
                return ESP_FAIL;
            }

            uint8_t r = wait_for_ctrl((const char[]){XMODEM_ACK, XMODEM_NAK,
                                                     XMODEM_CAN, 0},
                                      BL_BLOCK_ACK_TIMEOUT_MS);
            if (r == XMODEM_ACK) {
                acked = true;
            } else if (r == XMODEM_CAN) {
                snprintf(detail, detail_size,
                         "bootloader cancelled at block %u/%u",
                         (unsigned)(blk + 1), (unsigned)total);
                return ESP_ERR_INVALID_RESPONSE;
            } else {
                /* NAK or timeout — resend the same block. */
                ESP_LOGW(TAG, "block %u/%u %s (attempt %d)",
                         (unsigned)(blk + 1), (unsigned)total,
                         r == XMODEM_NAK ? "NAK" : "timeout", attempt + 1);
            }
        }

        if (!acked) {
            snprintf(detail, detail_size, "block %u/%u not acknowledged after %d tries",
                     (unsigned)(blk + 1), (unsigned)total, BL_BLOCK_RETRIES);
            return ESP_ERR_TIMEOUT;
        }

        status_progress(blk + 1, total);

        /* Yield periodically: 1900+ iterations of a tight UART loop would
           otherwise starve lower-priority tasks for the whole transfer. */
        if ((blk & 0x0F) == 0) vTaskDelay(1);
    }

    /* EOT closes the file; the bootloader answers ACK only if the parsed
       image verified, CAN otherwise. */
    const uint8_t eot = XMODEM_EOT;
    if (!send_all(&eot, 1, 500)) {
        strlcpy(detail, "UART write failed sending EOT", detail_size);
        return ESP_FAIL;
    }

    uint8_t r = wait_for_ctrl((const char[]){XMODEM_ACK, XMODEM_CAN, 0},
                              BL_EOT_ACK_TIMEOUT_MS);
    if (r != XMODEM_ACK) {
        strlcpy(detail, r == XMODEM_CAN ? "image rejected by bootloader (verify failed)"
                                        : "no response to EOT",
                detail_size);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Confirm with the human-readable result too — CAN vs ACK above already
       decided, but this distinguishes "wrote it" from "aborted" in the log. */
    char tail[80];
    if (wait_for_text(BL_UPLOAD_COMPLETE, 3000, tail, sizeof(tail))) {
        strlcpy(detail, "upload complete", detail_size);
        return ESP_OK;
    }
    if (strstr(tail, BL_UPLOAD_ABORTED)) {
        strlcpy(detail, "bootloader reported upload aborted", detail_size);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* ACKed but no banner within the window: the write did happen. Say so
       rather than failing an update that actually succeeded. */
    strlcpy(detail, "upload ACKed (no completion banner)", detail_size);
    return ESP_OK;
}

/* ----------------------------------------------------------- flash task */

/* Undo everything enter_bootloader/claim did, in reverse. Called on every
   exit path, success or failure, so a half-finished session can never leave
   nBOOT asserted (which would trap the MG24 in the bootloader across reboots)
   or the bridge stuck in OTA state. */
/* 刷完之后新固件答没答话（T9.19 ⑨）。以前只看 XMODEM 写完就报成功、记指纹 —— 2026-10-06 子设备 64
   那版启动即崩，照样报成了成功，云端也就不再提示更新。 */
typedef enum {
    NCP_CONFIRMED,       /* 读到了版本 */
    NCP_UNCONFIRMED,     /* 没读到，但线路平稳 —— 可能只是这一次没答上 */
    NCP_RESTARTING,      /* 没读到，且线路显示它在反复复位 / 根本没跑起来 */
    NCP_IN_BOOTLOADER,   /* 退出 bootloader 失败：芯片多半还停在里面（第二轮审计 A4） */
} ncp_confirm_t;

/* 读版本失败后采 3 s 线路电平：健康的 NCP CTS 恒断言（34000 个采样点零抬起，2026-09-15 实测），
   复位那一下抬起约 63 ms。所以窗口里出现过抬起（≥2 次跳变 = 至少一次完整的抬起-落下）或整窗都没驱动，
   就是"新固件起不来"；一次都没抬起只能说"没确认"。崩溃循环的周期没实测过，长于 3 s 的会落进"未确认"
   —— 仍然不是"成功"。 */
#define NCP_CONFIRM_SAMPLE_MS 3000

static ncp_confirm_t confirm_after_flash(void)
{
    const sys_config_t *sysc = config_mgr_get_sys();
    if (ncp_version_query(sysc ? sysc->radio_region : NULL) == ESP_OK &&
        ncp_version_get()->valid) {
        return NCP_CONFIRMED;
    }
    uart_line_sample_t ls;
    uart_bridge_sample_lines(NCP_CONFIRM_SAMPLE_MS, &ls);
    ESP_LOGW(TAG, "could not read NCP version after update — line: transitions=%" PRIu32
                  " deasserted=%" PRIu32 "/%" PRIu32 " longest=%" PRIu32 " ms",
             ls.cts_transitions, ls.cts_deasserted, ls.samples, ls.cts_longest_deassert_ms);
    if (ls.samples && (ls.cts_transitions >= 2 || ls.cts_deasserted == ls.samples)) {
        return NCP_RESTARTING;
    }
    return NCP_UNCONFIRMED;
}

static void cleanup_session(bridge_mode_t prev_mode, bool bootloader_entered,
                            bool flashed_ok, ncp_confirm_t *confirm)
{
    *confirm = NCP_UNCONFIRMED;
    if (bootloader_entered) {
        esp_err_t err = exit_bootloader();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "exit_bootloader failed: %s", esp_err_to_name(err));
            *confirm = NCP_IN_BOOTLOADER;
        } else if (flashed_ok) {
            /* Re-read the version here and nowhere else. We still hold the OTA
               claim and the bridge is DISABLED, so the UART is provably ours;
               once ezsp_bridge_ota_release() runs below, a client may attach
               at any moment and the query's ASH RST would break its handshake.
               exit_bootloader() already waited MG24_BOOT_SETTLE_MS for the new
               application to come up. */
            *confirm = confirm_after_flash();
        }
    } else {
        /* Never got in; make sure the line is not left armed anyway. */
        mg24_set_nboot(1);
        if (uart_bridge_get_baud() != EZSP_UART_BAUD) {
            uart_bridge_set_baud(EZSP_UART_BAUD);
        }
    }

    ezsp_bridge_ota_release();

    if (ezsp_bridge_get_mode() != prev_mode) {
        esp_err_t err = ezsp_bridge_switch_mode(prev_mode);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "could not restore bridge mode %d: %s",
                     prev_mode, esp_err_to_name(err));
        }
    }
}

static void free_image(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_image) {
        free(s_image);
        s_image = NULL;
    }
    s_image_cap = 0;
    s_image_len = 0;
    xSemaphoreGive(s_lock);
}

/* The whole flash session, start to finish, on whatever stack calls it.
   Returns normally; the outcome is in the status snapshot, not the return
   value, because both callers report progress through ota_mg24_get_status(). */
static void ota_run_session(void)
{
    char detail[80] = "";
    bridge_mode_t prev_mode = ezsp_bridge_get_mode();
    bool entered = false;
    esp_err_t err = ESP_OK;

    status_set(MG24_OTA_PREPARING, "draining bridge", ESP_OK);

    /* Steps 1-3 of docs/01 §5.3: closing the TCP server sends a FIN to every
       client, so Z2M/ZHA see a clean close and reconnect on their own once
       the bridge comes back. */
    if (prev_mode != BRIDGE_MODE_DISABLED) {
        err = ezsp_bridge_switch_mode(BRIDGE_MODE_DISABLED);
        if (err != ESP_OK) {
            status_set(MG24_OTA_FAILED, "could not disable bridge", err);
            goto done_no_claim;
        }
    }

    err = ezsp_bridge_ota_claim();
    if (err != ESP_OK) {
        status_set(MG24_OTA_FAILED, "bridge busy", err);
        goto done_no_claim;
    }

    status_set(MG24_OTA_PREPARING, "entering bootloader", ESP_OK);
    err = enter_bootloader(detail, sizeof(detail));
    if (err != ESP_OK) {
        status_set(MG24_OTA_FAILED, detail, err);
        goto done;
    }
    entered = true;
    /* 从这里起 MG24 上的固件随时可能被擦掉，"芯片里是什么"就不知道了：
       · 作废 NCP 读数缓存 —— 否则刷写失败或新固件起不来时用户软重启，开机会把旧读数当成现状（T9.31 ②）；
       · 作废旧指纹 —— 否则云端以为"已是最新"，藏起重装官方版的那条路（第二轮审计 A1）。
       只有新固件答了话（NCP_CONFIRMED）才记新指纹、重新落盘读数。 */
    ncp_cache_invalidate();
    (void)mg24_image_record_forget();

    status_set(MG24_OTA_TRANSFERRING, "uploading", ESP_OK);
    err = xmodem_send_image(s_image, s_image_len, detail, sizeof(detail));
    if (err != ESP_OK) {
        status_set(MG24_OTA_FAILED, detail, err);
        goto done;
    }

    status_set(MG24_OTA_FINALIZING, "booting new firmware", ESP_OK);

done:
    /* The NVM3 floor rises as soon as the image is written, confirmed or not: the
       chip may already have run it, and the floor exists to stop a downgrade below
       whatever the chip's NVM3 has seen (T9.25). */
    if (err == ESP_OK && s_image_nvm3c > 0) (void)mg24_image_record_note_nvm3(s_image_nvm3c);

    ncp_confirm_t confirm;
    cleanup_session(prev_mode, entered, err == ESP_OK, &confirm);

    if (err == ESP_OK) {
        /* The fingerprint is recorded only when the new firmware answered (T9.19 ⑨). Otherwise the
           old record stays: the cloud then still sees "differs from the latest" and offers the
           update again, which is the retry path the user needs. The hash is the only field that
           identifies *our* image — the EZSP version reports the EmberZNet release, identical
           across our builds. s_upload_sha survives cleanup (only the image buffer is freed). */
        switch (confirm) {
        case NCP_CONFIRMED:
            mg24_image_record_save_digest(s_upload_sha, s_upload_len);
            status_set(MG24_OTA_SUCCESS, "MG24 firmware updated", ESP_OK);
            ESP_LOGI(TAG, "MG24 OTA succeeded (%u bytes)", (unsigned)s_image_len);
            break;
        case NCP_RESTARTING:
            status_set(MG24_OTA_FAILED, "written, but the new firmware keeps restarting", ESP_FAIL);
            ESP_LOGE(TAG, "MG24 OTA: image written but the new firmware does not come up");
            break;
        case NCP_IN_BOOTLOADER:
            status_set(MG24_OTA_FAILED, "written, but could not leave the bootloader", ESP_ERR_INVALID_STATE);
            ESP_LOGE(TAG, "MG24 OTA: image written but the chip did not leave the bootloader "
                          "(POST /api/v1/zigbee/bootloader {\"exit\":true})");
            break;
        case NCP_UNCONFIRMED:
        default:
            status_set(MG24_OTA_UNCONFIRMED, "written, but the new firmware did not answer", ESP_OK);
            ESP_LOGW(TAG, "MG24 OTA: image written, new firmware did not answer — unconfirmed");
            break;
        }
    } else {
        ESP_LOGE(TAG, "MG24 OTA failed: %s (%s)", detail, esp_err_to_name(err));
    }

done_no_claim:
    free_image();
    s_task_running = false;
}

static void ota_task(void *arg)
{
    (void)arg;
    ota_run_session();
    /* 实测口（第一轮审计 #18）：T9.31 ② / T9.19 ⑦⑨ 之后，刷完重读那一段多了构建串应答缓冲、NCP 读数落盘与
       3 s 线路采样，这块 4 KB 栈此前没量过。单位是字节（IDF 的 xTaskCreate / 水位都按字节）。
       W 级：默认级别下进 /api/v1/logs，一次升级只打一行。失败路径的水位偏浅，别拿来定尺寸。 */
    UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGW(TAG, "mg24_ota 栈水位：分配 %d，最小剩余 %u（字节）", MG24_OTA_TASK_STACK, (unsigned)hw);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------- public API */

esp_err_t ota_mg24_init(void)
{
    if (s_lock) return ESP_OK;

    if (!gbl_crc32_selftest()) {
        ESP_LOGE(TAG, "GBL CRC32 self-test failed — MG24 OTA disabled");
        return ESP_ERR_INVALID_STATE;
    }

    /* The embedded signing key must import as a valid P-256 point; a corrupt
       key would otherwise turn every official image into "invalid signature"
       with no clue pointing at the key. An all-zero signature can never
       verify, so 1 is the healthy answer here and -1 means the key itself
       was refused by the crypto layer. */
    static const uint8_t zero_sig[GBL_SIGNATURE_LEN] = { 0 };
    if (gbl_sig_verify((const uint8_t *)"selftest", 8, zero_sig, mg24_gbl_pubkey_xy) == -1) {
        ESP_LOGE(TAG, "embedded MG24 signing key unusable — MG24 OTA disabled");
        return ESP_ERR_INVALID_STATE;
    }

    if (!xmodem_crc16_selftest()) {
        /* A broken CRC would NAK every block with no usable diagnostic, so
           refuse to offer the feature at all. */
        ESP_LOGE(TAG, "XMODEM CRC self-test failed — MG24 OTA disabled");
        return ESP_ERR_INVALID_STATE;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    memset(&s_status, 0, sizeof(s_status));
    s_status.state = MG24_OTA_IDLE;
    ncp_unlock_lock(&s_unlock);              /* boots locked, always */

    /* Physical confirmation for the developer unlock. The default loop exists
       (main.c creates it before us) and presses only happen later, so there
       is no "posted before subscribed" hole here (docs/交接 二·1). Failure is
       not fatal: signed updates keep working, only the unlock is unavailable. */
    esp_err_t sub = esp_event_handler_register(BUTTON_EVENT, BUTTON_EVENT_HOLD_CONFIRM,
                                               on_button_hold_confirm, NULL);
    if (sub != ESP_OK) {
        ESP_LOGE(TAG, "button event subscription failed (%s) — developer unlock unavailable",
                 esp_err_to_name(sub));
    }

    /* Warm the image record while we are still single-threaded — the REST
       handlers that read it run concurrently once httpd is up. */
    mg24_image_record_get();

    ESP_LOGI(TAG, "ready (bootloader baud %d, app baud %d)",
             CONFIG_BITENGINE_MG24_BOOTLOADER_BAUD, EZSP_UART_BAUD);
    return ESP_OK;
}

esp_err_t ota_mg24_begin(size_t expected_size, ota_mg24_source_t source)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (source != OTA_MG24_SOURCE_WEB && source != OTA_MG24_SOURCE_CLOUD) {
        return ESP_ERR_INVALID_ARG;
    }
    if (expected_size < MG24_OTA_MIN_SIZE || expected_size > MG24_OTA_MAX_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (ota_mg24_busy()) return ESP_ERR_INVALID_STATE;

    /* Prefer PSRAM: a ~240 KB image would take a third of internal DRAM. */
    uint8_t *buf = heap_caps_malloc(expected_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = heap_caps_malloc(expected_size, MALLOC_CAP_8BIT);
    }
    if (!buf) {
        ESP_LOGE(TAG, "cannot allocate %u bytes for staging", (unsigned)expected_size);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_image      = buf;
    s_image_cap  = expected_size;
    s_image_len  = 0;
    s_source     = source;
    s_reject     = NULL;
    s_image_nvm3c = 0;
    s_upload_len = 0;
    memset(&s_status, 0, sizeof(s_status));
    s_status.state      = MG24_OTA_RECEIVING;
    s_status.image_size = (uint32_t)expected_size;
    strlcpy(s_status.detail, "receiving image", sizeof(s_status.detail));
    xSemaphoreGive(s_lock);

    return ESP_OK;
}

esp_err_t ota_mg24_write(const uint8_t *data, size_t len)
{
    if (!s_lock || !data) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (!s_image || s_status.state != MG24_OTA_RECEIVING) {
        err = ESP_ERR_INVALID_STATE;
    } else if (s_image_len + len > s_image_cap) {
        err = ESP_ERR_INVALID_SIZE;   /* body longer than Content-Length */
    } else {
        memcpy(s_image + s_image_len, data, len);
        s_image_len += len;
    }
    xSemaphoreGive(s_lock);
    return err;
}

static esp_err_t commit_staged(bool run_inline)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;

    const char    *why    = "";
    const char    *reject = NULL;
    bool           sha_ok = true;
    gbl_sig_info_t sig    = { 0 };

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool   ready = s_image && s_status.state == MG24_OTA_RECEIVING;
    const size_t len   = s_image_len;
    const size_t cap   = s_image_cap;
    if (ready && len == cap) {
        /* Everything below happens before a single byte reaches the MG24.
           The XMODEM bootloader erases the old application as it writes, so
           a rejection here costs nothing and a rejection there costs the NCP
           (docs/产品化TODO_2026-09-09.md T1.2). Held under the lock so an
           abort() from another task cannot free the buffer mid-hash. */
        if (!gbl_validate(s_image, len, &why)) {
            reject = "not_a_gbl_image";
        } else if (!gbl_locate_signature(s_image, len, &sig, &why)) {
            reject = "ncp_image_malformed";
        } else if (sig.has_signature) {
            /* A signature that is present is always checked, whatever the
               policy says: a wrong one means tampering or the wrong key. */
            const uint8_t *r_s = s_image + sig.sig_off + GBL_TAG_HDR_LEN;
            const int v = gbl_sig_verify(s_image, sig.signed_len, r_s, mg24_gbl_pubkey_xy);
            if (v != 0) {
                reject = "ncp_signature_invalid";
                why    = (v == 1) ? "signature does not verify with the official key"
                                  : "signature check failed internally";
            }
        } else if (signature_required()) {
            reject = "ncp_image_unsigned";
            why    = "image carries no signature tag";
        } else if (!sig.has_signature) {
            ESP_LOGW(TAG, "unsigned NCP image accepted (%s)",
                     ncp_unlock_active(&s_unlock, now_s()) ? "developer unlock confirmed by button"
                                                            : "this build does not require signatures");
        }

        if (!reject) {
            /* T9.25 downgrade guard, last check before anything irreversible:
               an NCP whose NVM3 cache is smaller than the one this chip has
               already used crashes at every boot — and the bootloader would
               report the upload as a success. Signed old images exist, so
               the signature above does not stop them. Same rules as
               mg24/ncp_image_check.py. Web uploads may override it under the
               button-confirmed developer unlock; the cloud never may. */
            gbl_build_info_t bi;
            (void)gbl_build_info(s_image, len, &bi);
            const int32_t floor = mg24_image_record_nvm3_floor();
            int32_t       eff   = -1;
            const char   *g     = gbl_ncp_guard(&bi, floor, false, &eff);
            const bool    bypass = (s_source == OTA_MG24_SOURCE_WEB) && ncp_unlock_active(&s_unlock, now_s());
            s_image_nvm3c = (bi.has_build_tag && bi.nvm3_cache > 0 && !bi.wipe) ? bi.nvm3_cache : 0;
            if (g) {
                if (!strcmp(g, "ncp_image_downgrade")) {
                    snprintf(s_guard_why, sizeof(s_guard_why), "NVM3 cache %d < %d used on this chip: the NCP would crash at every boot",
                             (int)eff, (int)floor);
                } else if (!strcmp(g, "ncp_image_wipe")) {
                    snprintf(s_guard_why, sizeof(s_guard_why), "erase-on-every-boot build (WIPE): the Zigbee network would be lost at every boot");
                } else {
                    snprintf(s_guard_why, sizeof(s_guard_why), "compressed program data: cannot read the NVM3 cache size");
                }
                if (bypass) {
                    ESP_LOGW(TAG, "NCP image accepted under developer unlock despite %s (%s)", g, s_guard_why);
                } else {
                    reject = g;
                    why    = s_guard_why;
                }
            }
        }

        if (!reject) {
            /* Fingerprint of the bytes as uploaded — the hash the cloud
               manifest and SHA256SUMS carry — taken BEFORE the tag is
               stripped, so /zigbee/status.image_sha256 matches the release. */
            sha_ok       = gbl_sha256(s_image, len, s_upload_sha);
            s_upload_len = len;
            if (sha_ok && sig.has_signature) {
                /* The deployed bootloader would verify the tag against a key
                   it never got and reject the whole file after erasing the
                   app; hand it the plain container it has always accepted. */
                const size_t nl = gbl_strip_signature(s_image, len, &sig);
                if (nl == 0) {
                    reject = "ncp_image_malformed";
                    why    = "could not strip the signature tag";
                } else {
                    s_image_len = nl;
                }
            }
        }
    }
    const size_t send_len = s_image_len;
    xSemaphoreGive(s_lock);

    if (!ready)     return ESP_ERR_INVALID_STATE;
    if (len != cap) { free_image(); return ESP_ERR_INVALID_SIZE; }   /* truncated body */
    if (reject) {
        ESP_LOGE(TAG, "rejected image: %s (%s)", reject, why);
        s_reject = reject;
        status_set(MG24_OTA_FAILED, why, ESP_ERR_NOT_SUPPORTED);
        free_image();
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!sha_ok) {
        ESP_LOGE(TAG, "sha256 of the staged image failed");
        status_set(MG24_OTA_FAILED, "sha256 failed", ESP_FAIL);
        free_image();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "staged %u bytes (%s), starting MG24 update", (unsigned)send_len,
             sig.has_signature ? "signature verified, tag stripped" : "unsigned");

    s_task_running = true;

    if (run_inline) {
        /*
         * Borrow the caller's stack instead of allocating a second one.
         *
         * Not a style choice — measured on the board 2026-09-14. As it stood
         * then, the cloud installer's own task held 8 KB of *internal* stack
         * and the HTTPS download left the internal heap fragmented: largest
         * free block dipped to ~3.4 KB right where this code runs. Asking for
         * another 4 KB internal block there is a coin flip, and the first cloud
         * NCP install of the evening lost it: xTaskCreate failed, commit
         * returned ESP_ERR_NO_MEM, the user saw "commit_failed", and the only
         * trace was one 4096 / caps=0x804 entry in /api/v1/diag/heap. An
         * immediate retry succeeded, which is the worst kind of bug to leave
         * in a release.
         *
         * Those two numbers are the state on 2026-09-14 *before* T2.25; that
         * task's stack has since been measured and shrunk (current value and
         * data: NCP_TASK_STACK in ota_cloud.c) and the floor is no longer that
         * tight. Staying inline is still right, and is what made the shrink
         * possible — reintroducing a second task would spend the gain.
         *
         * Priority is matched to the task version for the duration: the cloud
         * downloader deliberately runs below EZSP, but the XMODEM loop has a
         * 3 s per-block ACK budget and should not be starved by web traffic.
         * Restored afterwards so the caller's own contract is unchanged.
         */
        const UBaseType_t prev_prio = uxTaskPriorityGet(NULL);
        if (prev_prio != MG24_OTA_TASK_PRIO) vTaskPrioritySet(NULL, MG24_OTA_TASK_PRIO);
        ota_run_session();
        if (prev_prio != MG24_OTA_TASK_PRIO) vTaskPrioritySet(NULL, prev_prio);
        return ESP_OK;
    }

    if (xTaskCreate(ota_task, "mg24_ota", MG24_OTA_TASK_STACK, NULL,
                    MG24_OTA_TASK_PRIO, NULL) != pdPASS) {
        s_task_running = false;
        free_image();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ota_mg24_commit(void)
{
    return commit_staged(false);
}

esp_err_t ota_mg24_commit_blocking(void)
{
    return commit_staged(true);
}

const char *ota_mg24_last_reject_code(void)
{
    /* Written by ota_mg24_commit() in the caller's own task right before it
       returns ESP_ERR_NOT_SUPPORTED; only one upload session exists at a
       time, so no lock is needed to read it back on that same path. */
    return s_reject ? s_reject : "not_a_gbl_image";
}

/* ------------------------------------------------------ developer unlock */

esp_err_t ota_mg24_unlock_arm(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ncp_unlock_arm(&s_unlock, now_s());
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "NCP developer unlock requested: hold the button 2-10 s and release "
                  "within %d s to accept unsigned images", NCP_UNLOCK_ARM_S);
    return ESP_OK;
}

void ota_mg24_unlock_lock(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool was_open = ncp_unlock_armed(&s_unlock, now_s()) || ncp_unlock_active(&s_unlock, now_s());
    ncp_unlock_lock(&s_unlock);
    xSemaphoreGive(s_lock);
    if (was_open) {
        ESP_LOGW(TAG, "NCP developer unlock cancelled: signed images only");
    }
}

void ota_mg24_unlock_get(ota_mg24_unlock_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
#if CONFIG_BITENGINE_MG24_REQUIRE_SIGNED_GBL
    out->signature_required = true;
#endif
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int64_t now = now_s();
    out->armed              = ncp_unlock_armed(&s_unlock, now);
    out->arm_remaining_s    = ncp_unlock_arm_left(&s_unlock, now);
    out->unlocked           = ncp_unlock_active(&s_unlock, now);
    out->unlock_remaining_s = ncp_unlock_left(&s_unlock, now);
    xSemaphoreGive(s_lock);
}

void ota_mg24_abort(void)
{
    if (!s_lock) return;
    /* Only the upload phase can be abandoned from outside; once the flash
       task owns the UART it must run to its own cleanup. */
    if (s_task_running) {
        ESP_LOGW(TAG, "abort ignored — flash already in progress");
        return;
    }
    free_image();
    status_set(MG24_OTA_IDLE, "aborted", ESP_OK);
}

void ota_mg24_get_status(mg24_ota_status_t *out)
{
    if (!out) return;
    if (!s_lock) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}

bool ota_mg24_busy(void)
{
    if (!s_lock) return false;
    if (s_task_running) return true;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool busy = (s_status.state == MG24_OTA_RECEIVING);
    xSemaphoreGive(s_lock);
    return busy;
}

esp_err_t ota_mg24_enter_bootloader(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (ota_mg24_busy()) return ESP_ERR_INVALID_STATE;

    bridge_mode_t prev = ezsp_bridge_get_mode();
    if (prev != BRIDGE_MODE_DISABLED) {
        ESP_RETURN_ON_ERROR(ezsp_bridge_switch_mode(BRIDGE_MODE_DISABLED),
                            TAG, "cannot disable bridge");
    }
    ESP_RETURN_ON_ERROR(ezsp_bridge_ota_claim(), TAG, "bridge busy");

    char detail[80];
    esp_err_t err = enter_bootloader(detail, sizeof(detail));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enter bootloader failed: %s", detail);
        mg24_set_nboot(1);
        if (uart_bridge_get_baud() != EZSP_UART_BAUD) {
            uart_bridge_set_baud(EZSP_UART_BAUD);
        }
        ezsp_bridge_ota_release();
        ezsp_bridge_switch_mode(prev);
        return err;
    }

    status_set(MG24_OTA_PREPARING, "MG24 held in bootloader", ESP_OK);
    ESP_LOGW(TAG, "MG24 parked in bootloader — bridge stays down until "
                  "ota_mg24_exit_bootloader()");
    return ESP_OK;
}

esp_err_t ota_mg24_exit_bootloader(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (s_task_running) return ESP_ERR_INVALID_STATE;

    /* Take the UART properly instead of assuming we still hold it from
       ota_mg24_enter_bootloader(). This entry point doubles as the recovery
       path after a C5 reboot, where the bridge is back up in LAN mode and its
       tasks would otherwise be reading the line underneath us. */
    const bool already_ours = (ezsp_bridge_get_state() == BRIDGE_STATE_OTA);
    if (!already_ours) {
        if (ezsp_bridge_get_mode() != BRIDGE_MODE_DISABLED) {
            ESP_RETURN_ON_ERROR(ezsp_bridge_switch_mode(BRIDGE_MODE_DISABLED),
                                TAG, "cannot disable bridge");
        }
        ESP_RETURN_ON_ERROR(ezsp_bridge_ota_claim(), TAG, "bridge busy");
    }

    esp_err_t err = exit_bootloader();
    ezsp_bridge_ota_release();

#if !CONFIG_BITENGINE_ENABLE_ZHUB   /* 直连型没有桥，数据面恒为 DISABLED（设计 2.6 第 6 步）：不切，否则 NOT_SUPPORTED 被当成失败 */
    esp_err_t mode_err = ezsp_bridge_switch_mode(BRIDGE_MODE_LAN);
    if (mode_err != ESP_OK) {
        ESP_LOGE(TAG, "bridge did not return to LAN: %s", esp_err_to_name(mode_err));
        if (err == ESP_OK) err = mode_err;
    }
#endif

    status_set(MG24_OTA_IDLE, "bootloader exited", err);
    return err;
}
