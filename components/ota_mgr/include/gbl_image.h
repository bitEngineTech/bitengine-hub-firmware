#pragma once

/*
 * GBL container helpers shared by every path that feeds a .gbl to the MG24
 * (web upload and cloud OTA both end in ota_mg24_commit()). Pure C — no
 * ESP-IDF, no FreeRTOS — so the very same file is compiled and exercised on
 * the host by test_host/run.sh.
 *
 * Format facts, each verified 2026-09-14 against the SDK source in
 * ~/src/mg24-fw/sdk/platform/bootloader, commander 1v23p1b1971 output and
 * openssl (write-up: docs/产品化TODO_2026-09-09.md T1.2):
 *
 *  - A GBL is a chain of { uint32 tagId; uint32 length; payload } records,
 *    little-endian. It opens with the V3 header tag (btl_gbl_format.h:49) and
 *    closes with the END tag whose 4-byte payload is the CRC-32 of every byte
 *    before it (btl_gbl_format.h:71). Nothing follows the END tag.
 *  - The ECDSA-P256 signature tag (btl_gbl_format.h:87, id 0xF70A0AF7) holds
 *    64 bytes, raw r||s, over SHA-256 of every byte that PRECEDES the tag
 *    (btl_gbl_parser.c:329 "do hashing ourselves to avoid hashing the
 *    signature/end tag"). The parser accepts no more data after it except
 *    the END tag ("accept no more data hereafter").
 *  - commander sets bit 0x100 of the header's `type` word on signed files
 *    (GBL_TYPE_SIGNATURE_ECDSA, btl_gbl_format.h:99). The bootloader reads it
 *    only when it enforces signatures (btl_gbl_parser.c:1087) — ours does not.
 *
 * Why the gateway strips the signature again before XMODEM: the deployed,
 * non-enforcing bootloader still *verifies* any signature tag it sees
 * (btl_gbl_parser.c parser_checkSignature runs regardless of the enforce
 * switch), against a public key that was never provisioned on the chip. A
 * signed container would therefore be rejected — after the old application
 * had already been erased, because the standalone bootloader writes as it
 * parses. So the gateway verifies the signature itself, then hands the
 * bootloader the exact unsigned container it has always accepted:
 * strip(signed) is byte-identical to `commander gbl create --app` without
 * `--sign` (checked on the shipped NCP image, sha256 b1f31c86…).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GBL_TAG_ID_HEADER_V3            0x03A617EBUL   /* btl_gbl_format.h:49 */
#define GBL_TAG_ID_END                  0xFC0404FCUL   /* btl_gbl_format.h:71 */
#define GBL_TAG_ID_SIGNATURE_ECDSA_P256 0xF70A0AF7UL   /* btl_gbl_format.h:87 */
#define GBL_TYPE_SIGNATURE_ECDSA        0x00000100UL   /* btl_gbl_format.h:99 */

#define GBL_SIGNATURE_LEN               64             /* r(32) || s(32) */
#define GBL_TAG_HDR_LEN                 8              /* id + length */
#define GBL_HEADER_TAG_LEN              (GBL_TAG_HDR_LEN + 8)   /* version + type */
#define GBL_END_TAG_LEN                 (GBL_TAG_HDR_LEN + 4)   /* crc32 */
#define GBL_MIN_LEN                     (GBL_HEADER_TAG_LEN + GBL_END_TAG_LEN)

/* btl_crc32.h:9,11 — seed, and the residue a valid file streams down to */
#define GBL_CRC32_START                 0xFFFFFFFFUL
#define GBL_CRC32_END                   0xDEBB20E3UL

typedef struct {
    bool   has_signature;   /* false: plain container, nothing to verify/strip */
    size_t sig_off;         /* offset of the signature tag *header* */
    size_t signed_len;      /* bytes covered by the signature (== sig_off) */
} gbl_sig_info_t;

/* CRC-32 with the bootloader's convention: reflected, poly 0xEDB88320,
   seeded 0xFFFFFFFF and NOT inverted at the end (btl_crc32Stream() reads the
   GPCRC DATA register raw). The value stored in the END tag is the standard
   CRC-32, i.e. this register value XOR 0xFFFFFFFF. */
uint32_t gbl_crc32(const uint8_t *data, size_t len, uint32_t crc);
bool     gbl_crc32_selftest(void);

/*
 * Container check, done before a single byte reaches the MG24: magic, END
 * tag position, whole-file CRC residue, and a full tag walk that must land
 * exactly on the END tag. `why` receives a static string on failure.
 */
bool gbl_validate(const uint8_t *img, size_t len, const char **why);

/*
 * Locate the ECDSA-P256 signature tag in an image that passed gbl_validate().
 * Returns true with out->has_signature=false when there is none. Returns
 * false (with `why`) when the container is malformed with respect to signing:
 * more than one signature tag, a payload that is not 64 bytes, or any tag
 * between the signature and the END tag.
 */
bool gbl_locate_signature(const uint8_t *img, size_t len,
                          gbl_sig_info_t *out, const char **why);

/*
 * Remove the signature tag in place, clear GBL_TYPE_SIGNATURE_ECDSA in the
 * header and recompute the END tag CRC. Returns the new length, or 0 when
 * `sig` does not describe a signature located in this exact image.
 */
size_t gbl_strip_signature(uint8_t *img, size_t len, const gbl_sig_info_t *sig);

/* ---- NCP downgrade guard (T9.25, 2026-10-08) ---------------------------
 *
 * mg24/build_ncp.sh embeds a plain-text build tag in the application,
 * "bitEngine-xncp/<ver> <git describe> <date> nvm3c=<n>[ WIPE]". The NVM3
 * cache a chip has used can only grow: an NCP built with a smaller cache
 * than the chip has already used crashes at boot, every boot, and the
 * gateway would report the upload as a success (mg24/README.md, "直连子设备
 * 64：根因实测"). Every unit ships having used the 400-entry cache, and
 * every older image is properly signed and still sits in git history, so
 * the signature check alone does not stop it. Same rules as
 * mg24/ncp_image_check.py, which gates publishing and the factory kit. */

#define GBL_NVM3C_LEGACY   200   /* images without the tag / without nvm3c= */
#define GBL_BUILD_TAG_MAX  127   /* longest tag text kept after the prefix */

typedef struct {
    bool    has_build_tag;  /* found "bitEngine-xncp/" in the program data */
    bool    wipe;           /* " WIPE" token: erases NVM3 on every boot */
    bool    compressed;     /* LZ4 / LZMA program tags: cannot be read */
    int32_t nvm3_cache;     /* nvm3c= value, -1 when the tag has none */
    char    tag[GBL_BUILD_TAG_MAX + 1];   /* text after the prefix, NUL-terminated */
} gbl_build_info_t;

/*
 * Read the build tag out of an image that passed gbl_validate(). The program
 * data is split across PROG / ERASEPROG tags (4-byte flash address + bytes);
 * the text is matched over their concatenation, so a tag that straddles two
 * of them is still found. No allocation. Returns false only on bad arguments.
 */
bool gbl_build_info(const uint8_t *img, size_t len, gbl_build_info_t *out);

/*
 * Decide whether an image may be flashed onto a chip whose NVM3 cache floor is
 * @p floor entries. Returns NULL when it may, otherwise the REST error code:
 *   "ncp_image_unreadable"  compressed program data, cannot check
 *   "ncp_image_wipe"        erase-on-every-boot build
 *   "ncp_image_downgrade"   NVM3 cache below the floor (no tag = legacy 200)
 * @p bypass (developer unlock confirmed by the button, web uploads only)
 * accepts all three. @p eff_out, when non-NULL, receives the image's effective
 * cache size (GBL_NVM3C_LEGACY when it has no nvm3c=, -1 when unreadable).
 */
const char *gbl_ncp_guard(const gbl_build_info_t *bi, int32_t floor, bool bypass,
                          int32_t *eff_out);

/* ---- provided by the platform layer -----------------------------------
 * gbl_crypto_psa.c on the target (PSA Crypto API), host_crypto.c in
 * test_host (OpenSSL). Same contract, so the container logic above is
 * tested against real ECDSA on the host. */

/** SHA-256 of @p data. Returns false on failure. */
bool gbl_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/**
 * ECDSA-P256 / SHA-256 verification of @p sig (raw r||s) over @p msg with
 * the uncompressed public key point @p pub_xy (X||Y, 64 bytes).
 * Returns 0 when the signature verifies, 1 when it does not, -1 on an
 * internal error (bad key, crypto init failure).
 */
int gbl_sig_verify(const uint8_t *msg, size_t msg_len,
                   const uint8_t sig[GBL_SIGNATURE_LEN],
                   const uint8_t pub_xy[64]);

/* The official MG24 image signing public key, X||Y. Generated at build time
   by tools/gen_mg24_pubkey.py from mg24/release/mg24_gbl_signing_key.pub —
   the PEM file is the only copy that is edited; the array is derived. */
extern const uint8_t mg24_gbl_pubkey_xy[64];

#ifdef __cplusplus
}
#endif
