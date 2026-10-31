/*
 * Platform half of gbl_image.h on the ESP32-C5: SHA-256 and ECDSA-P256
 * verification through the PSA Crypto API.
 *
 * Why PSA and not mbedtls_ecdsa_verify(): ESP-IDF v6.0.2 ships Mbed TLS
 * 4.1.0, where the legacy low-level crypto headers (ecdsa.h, sha256.h) are
 * private to TF-PSA-Crypto; PSA is the supported interface. H1 check of the
 * API semantics (2026-09-14, tf-psa-crypto/include/psa/crypto.h and IDF's own
 * bootloader_support/src/secure_boot_v2/secure_boot_ecdsa_signature.c):
 *   - psa_import_key() for PSA_KEY_TYPE_ECC_PUBLIC_KEY(SECP_R1) takes the
 *     uncompressed point 0x04 || X || Y (65 bytes) and rejects a point that
 *     is not on the curve — so a corrupt embedded key fails closed here.
 *   - psa_verify_hash() with PSA_ALG_ECDSA(PSA_ALG_SHA_256) takes the
 *     32-byte digest and the signature in PSA's raw format r || s, each
 *     zero-padded to the curve size: 64 bytes — exactly what the GBL tag
 *     carries, no DER conversion.
 *   - psa_crypto_init() is idempotent; IDF already runs it from
 *     ESP_SYSTEM_INIT_FN (mbedtls/port/esp_psa_crypto_init.c), calling it
 *     again is the documented way to be sure.
 * Memory: with CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y the key-store and MPI
 * allocations land in PSRAM, so this does not touch the scarce internal SRAM.
 */

#include "gbl_image.h"

#include "psa/crypto.h"

#include <string.h>

bool gbl_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    if (!data || !out) return false;
    if (psa_crypto_init() != PSA_SUCCESS) return false;

    size_t hash_len = 0;
    psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256, data, len, out, 32, &hash_len);
    return st == PSA_SUCCESS && hash_len == 32;
}

int gbl_sig_verify(const uint8_t *msg, size_t msg_len,
                   const uint8_t sig[GBL_SIGNATURE_LEN],
                   const uint8_t pub_xy[64])
{
    if (!msg || !sig || !pub_xy) return -1;

    uint8_t hash[32];
    if (!gbl_sha256(msg, msg_len, hash)) return -1;

    uint8_t point[65];
    point[0] = 0x04;                       /* SEC1 uncompressed */
    memcpy(point + 1, pub_xy, 64);

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

    psa_key_id_t key = 0;
    psa_status_t st = psa_import_key(&attr, point, sizeof(point), &key);
    psa_reset_key_attributes(&attr);
    if (st != PSA_SUCCESS) {
        return -1;                         /* key not on the curve, no memory… */
    }

    st = psa_verify_hash(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256),
                         hash, sizeof(hash), sig, GBL_SIGNATURE_LEN);
    psa_destroy_key(key);                  /* every path past import */

    if (st == PSA_SUCCESS) return 0;
    if (st == PSA_ERROR_INVALID_SIGNATURE) return 1;
    return -1;
}
