/*
 * Host implementation of the platform half of gbl_image.h, on OpenSSL 3.
 * Same contract as gbl_crypto_psa.c so gbl_image.c is exercised against
 * real ECDSA-P256 here, not against a stub.
 */

#include "gbl_image.h"

#include <openssl/evp.h>
#include <openssl/x509.h>

#include <string.h>

bool gbl_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    if (!data || !out) return false;
    unsigned int hl = 0;
    return EVP_Digest(data, len, out, &hl, EVP_sha256(), NULL) == 1 && hl == 32;
}

/* SubjectPublicKeyInfo prefix for an uncompressed secp256r1 point — the
   same 27 constant bytes tools/gen_mg24_pubkey.py insists on. */
static const uint8_t SPKI_P256_PREFIX[27] = {
    0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01,
    0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04
};

/* DER INTEGER from a 32-byte big-endian unsigned value. */
static size_t der_int(const uint8_t *v, uint8_t *out)
{
    size_t start = 0;
    while (start < 31 && v[start] == 0) start++;
    const bool pad = (v[start] & 0x80) != 0;
    const size_t n = 32 - start + (pad ? 1 : 0);
    size_t o = 0;
    out[o++] = 0x02;
    out[o++] = (uint8_t)n;
    if (pad) out[o++] = 0x00;
    memcpy(out + o, v + start, 32 - start);
    return o + 32 - start;
}

int gbl_sig_verify(const uint8_t *msg, size_t msg_len,
                   const uint8_t sig[GBL_SIGNATURE_LEN],
                   const uint8_t pub_xy[64])
{
    if (!msg || !sig || !pub_xy) return -1;

    uint8_t hash[32];
    if (!gbl_sha256(msg, msg_len, hash)) return -1;

    uint8_t spki[sizeof(SPKI_P256_PREFIX) + 64];
    memcpy(spki, SPKI_P256_PREFIX, sizeof(SPKI_P256_PREFIX));
    memcpy(spki + sizeof(SPKI_P256_PREFIX), pub_xy, 64);
    const unsigned char *p = spki;
    EVP_PKEY *pk = d2i_PUBKEY(NULL, &p, sizeof(spki));
    if (!pk) return -1;

    uint8_t der[2 + 2 * 35];
    size_t body = der_int(sig, der + 2);
    body += der_int(sig + 32, der + 2 + body);
    der[0] = 0x30;
    der[1] = (uint8_t)body;

    int result = -1;
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pk, NULL);
    if (ctx && EVP_PKEY_verify_init(ctx) == 1
        && EVP_PKEY_CTX_set_signature_md(ctx, EVP_sha256()) == 1) {
        int rc = EVP_PKEY_verify(ctx, der, body + 2, hash, sizeof(hash));
        result = (rc == 1) ? 0 : (rc == 0 ? 1 : -1);
    }
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pk);
    return result;
}
