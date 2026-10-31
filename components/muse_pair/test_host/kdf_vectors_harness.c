/*
 * 固件侧密钥派生 × 与 App 共享的 v5 测试向量（本仓库自加，不是上游文件）。
 *
 * 上游的覆盖缺口：test_link_pairing_transcript 只用 Python 复算向量里的密钥；
 * test_link_pairing_handshake 的"手机端"直接拿固件算出的 s_rx_key 加密，是自洽的。
 * 所以固件的 HKDF 盐长、标签、会话 ID、AAD/nonce 格式改错了，两边一起错，上游测试全绿，
 * 只有真手机配对时才会红（变异测试实测：盐长少 1 字节上游不报）。
 *
 * 这里把向量里的设备私钥、手机公钥、两个 nonce、transcript 灌进固件状态，调固件自己的
 * derive_session_keys()，再用固件自己的 aes_gcm_decrypt() 解向量里 App 发的 client_finished。
 *
 * 用法：kdf_vectors <device_priv_hex> <mobile_pub_b64> <mobile_nonce_b64> <device_nonce_b64> <transcript>
 *                   <client_finished_ciphertext_b64> <client_finished_tag_b64>
 * 输出：rx=<hex> tx=<hex> sid=<b64> plain=<client_finished 明文>（解不开时 plain= 后为空、退出码 3）
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "psa/crypto.h"
#define CONFIG_HOMEHUB_PAIRING_AUTH_EPOCH 1
int test_fuse_state, test_efuse_reads;
int64_t test_now = 1;
#include "link_pairing.c"

static void put_hex(const char *name, const uint8_t *b, size_t n) {
    printf("%s=", name);
    for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
    printf(" ");
}

int main(int argc, char **argv) {
    if (argc != 8) {
        fprintf(stderr, "usage: %s priv_hex mobile_pub nonce_m nonce_d transcript ct tag\n", argv[0]);
        return 2;
    }
    link_pairing_init("node-test", "device-test", "00:11:22:33:44:55", "0.2.1", NULL);

    size_t n = 0;
    assert(mbedtls_mpi_read_string(&s_device_priv, 16, argv[1]) == 0);
    assert(base64url_decode(argv[2], s_mobile_pub, sizeof(s_mobile_pub), &n) && n == sizeof(s_mobile_pub));
    assert(base64url_decode(argv[3], s_mobile_nonce, sizeof(s_mobile_nonce), &n) && n == sizeof(s_mobile_nonce));
    assert(base64url_decode(argv[4], s_device_nonce, sizeof(s_device_nonce), &n) && n == sizeof(s_device_nonce));
    assert(sha256_bytes((const uint8_t *)argv[5], strlen(argv[5]), s_transcript_hash));
    assert(derive_session_keys());

    put_hex("rx", s_rx_key, sizeof(s_rx_key));
    put_hex("tx", s_tx_key, sizeof(s_tx_key));
    printf("sid=%s ", s_session_id_b64);

    uint8_t cipher[512], tag[GCM_TAG_BYTES], plain[513];
    size_t cipher_len = 0, tag_len = 0;
    assert(base64url_decode(argv[6], cipher, sizeof(cipher), &cipher_len));
    assert(base64url_decode(argv[7], tag, sizeof(tag), &tag_len) && tag_len == GCM_TAG_BYTES);
    // App 发的第一帧：方向 m2d(0)、计数 0 —— 与固件收包路径 link_pairing_decrypt_command 的调用一致
    if (!aes_gcm_decrypt(s_rx_key, cipher, cipher_len, tag, 0, 0, plain)) {
        printf("plain=\n");
        return 3;
    }
    plain[cipher_len] = '\0';
    printf("plain=%s\n", (const char *)plain);
    return 0;
}
