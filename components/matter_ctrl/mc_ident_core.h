/* mc_ident_core —— 网关 fabric 身份的存储格式（docs/15 设计 9.3；M7 第二步）。纯逻辑，不依赖 IDF / CHIP（test_host/ 原样编）。
 *
 * 身份 = fabric ID + 根 CA 的 ID 与密钥对 + IPK + 根证书（DER）。存 hubdb 的 mctrl/ident（经 hub_core 的钩子、hub_store 落盘），
 * 是 fabric 的正本：matter 分区（CHIP 的 KVS）坏了可以拿它重建控制器、设备不丢；它丢了，配进来的设备就全成了孤儿。
 * ⚠️ 密钥对与 IPK 是秘密：不进日志、REST、导出、Muse（设计第十三节）；用完的缓冲 mc_ident_wipe()。
 *
 * blob v1（小端）：fabric_id u64 | rcac_id u64 | kp_len u8 (= 97) | kp[97]（P-256 公钥 65 + 私钥 32，CHIP 的 P256SerializedKeypair）|
 *                  ipk[16] | rcac_len u16 | rcac[rcac_len]（1..600）。长度必须正好对上，多一个字节都不认。
 * 格式版本不在 blob 里：hub_store 的 ver 字节就是它（MC_IDENT_VER），读到更新的版本 hub_store 回 INVALID_VERSION。 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MC_IDENT_VER       1
#define MC_IDENT_KP_LEN    97     /* kP256_PublicKey_Length + kP256_PrivateKey_Length（mc_issuer.cpp 里 static_assert 对上 CHIP） */
#define MC_IDENT_IPK_LEN   16     /* CHIP_CRYPTO_SYMMETRIC_KEY_LENGTH_BYTES */
#define MC_IDENT_RCAC_MAX  600    /* kMaxDERCertLength */
#define MC_IDENT_BLOB_MAX  (8 + 8 + 1 + MC_IDENT_KP_LEN + MC_IDENT_IPK_LEN + 2 + MC_IDENT_RCAC_MAX)

typedef struct {
    uint64_t fabric_id;                 /* 非 0（0 是 kUndefinedFabricId） */
    uint64_t rcac_id;                   /* 根证书主题里的 MatterRCACId，非 0 */
    uint8_t  kp[MC_IDENT_KP_LEN];
    uint8_t  ipk[MC_IDENT_IPK_LEN];
    uint16_t rcac_len;                  /* 1..MC_IDENT_RCAC_MAX */
    uint8_t  rcac[MC_IDENT_RCAC_MAX];
} mc_ident_t;

typedef enum {
    MC_IDENT_OK = 0,
    MC_IDENT_EARG,        /* 空指针 / 缓冲太小 */
    MC_IDENT_ELEN,        /* blob 长度对不上（截断、多出字节、rcac_len 越界） */
    MC_IDENT_EKP,         /* 密钥对长度不是 97 */
    MC_IDENT_EZERO,       /* fabric_id 或 rcac_id 是 0 */
} mc_ident_err_t;

/* 编码：成功返回写入的字节数（> 0），失败返回 -mc_ident_err_t */
int mc_ident_encode(const mc_ident_t *id, uint8_t *buf, size_t cap);
/* 解码：失败时 *out 被清零（不留半截密钥） */
mc_ident_err_t mc_ident_decode(const uint8_t *buf, size_t len, mc_ident_t *out);
/* 清掉一块装过秘密的内存（编译器不许省掉） */
void mc_ident_wipe(void *p, size_t n);

#ifdef __cplusplus
}
#endif
