#pragma once

/*
 * MG24 NCP firmware update over the existing EZSP UART, via the Gecko
 * Bootloader's XMODEM menu.
 *
 * Production flow: J-Link writes bootloader + NCP app once on the line.
 * Every update after that goes through here — no probe, no disassembly.
 *
 * ── How the MG24 is put into the bootloader ────────────────────────────────
 * NOT by pulsing MG24_RESET_PIN (IO23). On RS706-populated boards IO23 is tied
 * into the supervisor's RESET net through R113, which drags C5's own EN down
 * through R58: driving it low resets the WHOLE BOARD, C5 included, and the
 * update dies with it (see components/watchdog/mg24_reset.c:80-84).
 *
 * Instead: hold nBOOT (IO24) low, then ask the running NCP to reset itself
 * with an ASH RST frame. The Gecko bootloader samples the nBOOT pin on every
 * reset, unconditionally — btl_main.c:557 calls gpio_enterBootloader() outside
 * the reset-cause check — so a software reset lands in the bootloader just as
 * a power cycle with the button held would.
 *
 * Consequence worth knowing: this needs a *live* NCP to accept the ASH RST.
 * If the MG24 app is already dead the only recovery is J-Link, because C5 has
 * no way to reset the MG24 without resetting itself.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The bootloader's UART rate is a build-time property of the *MG24* image, so
 * it lives in Kconfig (CONFIG_BITENGINE_MG24_BOOTLOADER_BAUD) and is read at
 * every use site — the two sides are reflashed as a pair and a stale constant
 * here would silently talk at the wrong rate. Currently 460800, matching the
 * NCP application (mg24/config/btl_uart_driver_cfg.h); the code still handles
 * the two differing, which is what the SDK default (115200) would give.
 */

/* A real NCP .gbl is ~240 KB; 1 MB is a generous ceiling that still bounds
 * the PSRAM staging allocation against a malicious or truncated upload. */
#define MG24_OTA_MAX_SIZE       (1024 * 1024)
#define MG24_OTA_MIN_SIZE       128

typedef enum {
    MG24_OTA_IDLE = 0,
    MG24_OTA_RECEIVING,      /* HTTP body streaming into the staging buffer */
    MG24_OTA_PREPARING,      /* bridge drained, entering the bootloader */
    MG24_OTA_TRANSFERRING,   /* XMODEM blocks in flight */
    MG24_OTA_FINALIZING,     /* releasing nBOOT, booting the new app */
    MG24_OTA_SUCCESS,        /* written, and the new firmware answered the version query */
    MG24_OTA_FAILED,
    MG24_OTA_UNCONFIRMED,    /* written, but the new firmware did not answer and the line looked steady
                                (T9.19 ⑨) — neither success nor a proven failure; fingerprint not recorded */
} mg24_ota_state_t;

typedef struct {
    mg24_ota_state_t state;
    uint32_t image_size;
    uint32_t total_blocks;
    uint32_t sent_blocks;
    uint8_t  percent;
    int32_t  err;            /* esp_err_t of the failure, ESP_OK otherwise */
    char     detail[80];     /* human-readable stage or bootloader reply */
} mg24_ota_status_t;

/**
 * One-time init. Runs the CRC self-test and creates the status lock.
 * Safe to call twice.
 */
esp_err_t ota_mg24_init(void);

/* Who is staging the image. Decides the unsigned-image policy in
 * ota_mg24_commit(): the cloud path never accepts an image without a
 * signature; the web path follows CONFIG_BITENGINE_MG24_REQUIRE_SIGNED_GBL
 * and the button-confirmed developer unlock (ota_mg24_unlock_arm). */
typedef enum {
    OTA_MG24_SOURCE_WEB = 0,     /* POST /api/v1/ota/zigbee */
    OTA_MG24_SOURCE_CLOUD,       /* ota_cloud: manifest-driven install */
} ota_mg24_source_t;

/**
 * Reserve a staging buffer for an image of @p expected_size bytes.
 * Rejects sizes outside [MG24_OTA_MIN_SIZE, MG24_OTA_MAX_SIZE] and any call
 * made while another update is in flight.
 */
esp_err_t ota_mg24_begin(size_t expected_size, ota_mg24_source_t source);

/**
 * REST error code for the last ota_mg24_commit() that returned
 * ESP_ERR_NOT_SUPPORTED, one of:
 *   "not_a_gbl_image"        magic / END tag / CRC / tag chain broken
 *   "ncp_image_malformed"    signature tag present but mis-shaped
 *   "ncp_signature_invalid"  signature does not verify with the official key
 *   "ncp_image_unsigned"     no signature and the policy requires one
 *   "ncp_image_downgrade"    NVM3 cache below what this chip has used (T9.25)
 *   "ncp_image_wipe"         erase-NVM3-on-every-boot build (T9.25)
 *   "ncp_image_unreadable"   compressed program data, cache size unknown (T9.25)
 * The three T9.25 codes are lifted by the developer unlock on web uploads;
 * the cloud path never accepts them.
 * Valid on the same task, immediately after that return.
 */
const char *ota_mg24_last_reject_code(void);

/* ---- developer unlock for unsigned images (T1.2 step 3) -------------------
 * Unsigned .gbl uploads are refused by default
 * (CONFIG_BITENGINE_MG24_REQUIRE_SIGNED_GBL). Somebody standing at the device
 * can lift that for NCP_UNLOCK_WINDOW_S seconds: ota_mg24_unlock_arm() opens a
 * NCP_UNLOCK_ARM_S confirmation window, the button held 2-10 s and released
 * inside it confirms (BUTTON_EVENT_HOLD_CONFIRM). Password without the button,
 * or button without the password, achieves nothing. Web uploads only — the
 * cloud path never accepts unsigned images. RAM only: a reboot locks.
 * Rules and their host test: ncp_unlock.h, test_host/unlock_test.c. */
typedef struct {
    bool armed;                 /* confirmation window open */
    int  arm_remaining_s;
    bool unlocked;              /* unsigned web uploads currently accepted */
    int  unlock_remaining_s;
    bool signature_required;    /* build policy (Kconfig); false = developer build */
} ota_mg24_unlock_status_t;

/** Open the confirmation window (re-opens it if already open). */
esp_err_t ota_mg24_unlock_arm(void);
/** Cancel the window and any active unlock. */
void      ota_mg24_unlock_lock(void);
/** Snapshot. @p out must be non-NULL. */
void      ota_mg24_unlock_get(ota_mg24_unlock_status_t *out);

/** Append received body bytes. Fails if it would overrun @c expected_size. */
esp_err_t ota_mg24_write(const uint8_t *data, size_t len);

/**
 * Validate the staged image and hand it to the flash task.
 * Checks, in order and all before the MG24 is touched: GBL container
 * (magic, END tag, CRC, tag chain), signature tag shape, ECDSA-P256 signature
 * against the embedded official key when a tag is present, and the
 * unsigned-image policy when it is not. A verified signature tag is stripped
 * again before the XMODEM upload (see gbl_image.h for why).
 * ESP_ERR_NOT_SUPPORTED = rejected; ask ota_mg24_last_reject_code() which.
 * Returns as soon as the task is running — the caller (an HTTP handler) must
 * not sit through ~40 s of XMODEM. Poll ota_mg24_get_status() for progress.
 */
esp_err_t ota_mg24_commit(void);

/**
 * Same checks, but the flash runs on the CALLER's stack and this blocks for
 * the whole ~40 s session; the outcome is in ota_mg24_get_status(), the return
 * value only reports the staging checks (identical to ota_mg24_commit()).
 *
 * For callers that already own a task big enough and are about to exit anyway
 * — the cloud installer. There it is not an optimisation: its own task plus
 * the HTTPS download fragment the internal heap badly enough that spawning
 * a second 4 KB flash task fails intermittently (measured 2026-09-14; see the
 * comment in commit_staged() for the numbers as they were then).
 * T2.25 later shrank the caller's stack, which lifted the floor — but this
 * path stays inline precisely because not creating that second task is what
 * made the shrink worth doing. Do not "restore" ota_mg24_commit() here.
 * Never call this from an httpd worker.
 */
esp_err_t ota_mg24_commit_blocking(void);

/** Release the staging buffer and return to IDLE. No-op when idle. */
void ota_mg24_abort(void);

/** Snapshot of current progress. @p out must be non-NULL. */
void ota_mg24_get_status(mg24_ota_status_t *out);

/** True while an upload or flash session owns the UART. */
bool ota_mg24_busy(void);

/**
 * Put the MG24 into the bootloader and leave it there, with the bridge
 * disabled. For diagnostics and for a host-driven upload; the caller is
 * responsible for ota_mg24_exit_bootloader() afterwards.
 */
esp_err_t ota_mg24_enter_bootloader(void);

/** Leave the bootloader, boot the NCP app, restore the bridge. */
esp_err_t ota_mg24_exit_bootloader(void);

/* 2026-09-11(T4.6)：删掉了 ota_mg24_enter/exit_bootloader_locked()。
 * 这里原先写着"Used by the link benchmark"——**那是假的**，全仓零调用者。
 * 链路测试（uart_bench.c）走的是普通版本。一句谎称的注释比没有注释更糟：
 * 它让人以为这条路径被覆盖过。真需要"一次 claim 内多次进出 bootloader"时再加。
 */

#ifdef __cplusplus
}
#endif
