#pragma once

/*
 * Provenance for the MG24 image, kept in NVS.
 *
 * The EZSP version query (ncp_version.h) reports the EmberZNet release the NCP
 * is running — 8.2.2.0 build 532. That identifies the *stack*, not our image:
 * every .gbl we ship from the same SDK reports the same numbers, so it cannot
 * answer "did this unit get the March build or the June one".
 *
 * This does. It records the sha256 of the image the gateway itself flashed,
 * which is the same hash that appears in mg24/release/SHA256SUMS — so the
 * field value can be matched against a release directly.
 *
 * Blind spot worth stating: units that were only ever flashed on the
 * production line (J-Link, mg24/flash_mg24.sh) have no record here, because
 * the C5 was not involved. Those report "factory" and are traced through
 * mg24_flash_log.csv instead.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     valid;
    char     sha256[65];   /* full hex digest — matches SHA256SUMS verbatim */
    uint32_t size;
    int64_t  flashed_at;   /* unix seconds, 0 when the clock was not yet synced */
} mg24_image_record_t;

/**
 * Persist the record. Called after a successful MG24 OTA with the sha256 of
 * the image *as it was uploaded* — computed by ota_mg24_commit() before a
 * signature tag is stripped, so the value keeps matching SHA256SUMS and the
 * cloud manifest, which describe the signed file, not what went down the UART.
 */
esp_err_t mg24_image_record_save_digest(const uint8_t digest[32], size_t len);

/**
 * Drop the fingerprint (sha/size/time; the NVM3 floor stays). Called when an MG24 flash
 * enters the bootloader: from then on the chip may hold anything, and an old fingerprint
 * would tell the cloud "already on the latest" and hide the reinstall path when the new
 * image fails (T9.19 ⑨, audit round 2 A1). A confirmed flash records the new one.
 */
esp_err_t mg24_image_record_forget(void);

/** Cached record, loading it from NVS on first use. Never NULL. */
const mg24_image_record_t *mg24_image_record_get(void);

/*
 * NVM3 cache floor for the NCP downgrade guard (T9.25, gbl_ncp_guard()).
 * The cache a chip has used only ever grows, so the floor is the largest
 * nvm3c this gateway has ever flashed, and never below the factory value:
 * units are flashed on the production line with the 400-entry NCP (decided
 * 2026-10-07: production ships only that one), which the C5 never sees.
 * Stored in the "mg24" NVS namespace — not erased by a factory reset, because
 * it describes the chip, not the user's settings.
 */
#define MG24_NVM3C_FLOOR_DEFAULT 400
int32_t   mg24_image_record_nvm3_floor(void);
/** Raise the floor to @p nvm3c after a successful flash (no-op when lower). */
esp_err_t mg24_image_record_note_nvm3(int32_t nvm3c);

#ifdef __cplusplus
}
#endif
