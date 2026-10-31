/* mc_issuer —— 网关 fabric 的凭据签发器（为什么不用 esp-matter 默认的，见 mc_issuer.h 头注）。只在 CHIP 任务里用。
 *
 * H1（connectedhomeip 随 esp_matter 1.6.0~1，2026-10-23 读源码核过）：
 *  - X509CertRequestParams 的时间是 Matter 纪元秒（2000-01-01 起）；ValidityEnd = kNullCertTime（0）编成"无到期日"
 *    （GenerateChipX509Cert.cpp：ValidityEnd == kNullCertTime || ≥ ValidityStart）。
 *  - commissioner 在 GenerateNOCChain 之前调 SetNodeIdForNextNOCRequest / SetFabricIdForNextNOCRequest（CHIPDeviceController.cpp），
 *    节点号由我们在 PairDevice 时给出、事先落盘（第三步）—— 这里没收到就拒签，不自己分配。
 *  - csrElements 是 TLV 结构，上下文标签 1 是 DER 的 CSR；VerifyCertificateSigningRequest 验 CSR 自签名并取出公钥
 *    （照 ExampleOperationalCredentialsIssuer::GenerateNOCChain）。CSR 的 nonce 与设备认证签名 commissioner 在调我们之前已经验过。
 *  - 回调 OnNOCChainGeneration(ctx, err, noc, icac, rcac, ipk, adminSubject)：icac 给空 span = 根直接签 NOC（AutoCommissioner 认空的）。
 *  - 控制器这一侧的 IPK 由 matter_ctrl 在 setup_commissioner 之后写进 GroupDataProvider（esp-matter 原本写的测试值由构建期补丁去掉）。 */
#include "mc_issuer.h"

#include <string.h>

#include <esp_log.h>
#include <esp_random.h>

#include <credentials/CHIPCert.h>
#include <crypto/CHIPCryptoPAL.h>
#include <lib/core/TLV.h>
#include <lib/support/ScopedMemoryBuffer.h>

static const char *TAG = "mc_issuer";

using namespace chip;
using namespace chip::Credentials;
using namespace chip::Crypto;

static_assert(MC_IDENT_KP_LEN == kP256_PublicKey_Length + kP256_PrivateKey_Length, "身份格式里的密钥对长度要等于 CHIP 的 P256SerializedKeypair");
static_assert(MC_IDENT_IPK_LEN == CHIP_CRYPTO_SYMMETRIC_KEY_LENGTH_BYTES, "IPK 长度");
static_assert(MC_IDENT_RCAC_MAX == kMaxDERCertLength, "根证书上限");

namespace mc {

/* 2025-01-01T00:00:00Z 的 Matter 纪元秒（Unix 1735689600 − 946684800）。固定在过去：对过时的设备不会"尚未生效"，
   没有时间源的设备会把 Last Known Good Time 至少抬到这里，无害 */
static constexpr uint32_t kCertNotBefore = 789004800;

static uint64_t rand_nonzero_u64(void)
{
    uint64_t v = 0;
    do {
        esp_fill_random(&v, sizeof(v));
    } while (v == 0);
    return v;
}

/* X.509 序列号：正的 63 位随机数（证书之间不重复即可） */
static int64_t rand_serial(void) { return (int64_t)(rand_nonzero_u64() >> 1) | 1; }

CHIP_ERROR ident_generate(uint64_t fabric_id, mc_ident_t &out)
{
    memset(&out, 0, sizeof(out));
    VerifyOrReturnError(fabric_id != kUndefinedFabricId, CHIP_ERROR_INVALID_ARGUMENT);
    out.fabric_id = fabric_id;
    out.rcac_id = rand_nonzero_u64();
    esp_fill_random(out.ipk, sizeof(out.ipk));

    P256Keypair root;
    ReturnErrorOnFailure(root.Initialize(ECPKeyTarget::ECDSA));
    {
        P256SerializedKeypair ser;
        ReturnErrorOnFailure(root.Serialize(ser));
        VerifyOrReturnError(ser.Length() == MC_IDENT_KP_LEN, CHIP_ERROR_INTERNAL);
        memcpy(out.kp, ser.ConstBytes(), MC_IDENT_KP_LEN);
        mc_ident_wipe(ser.Bytes(), ser.Capacity());
    }

    ChipDN dn;
    ReturnErrorOnFailure(dn.AddAttribute_MatterRCACId(out.rcac_id));
    X509CertRequestParams req = { rand_serial(), kCertNotBefore, kNullCertTime, dn, dn };
    MutableByteSpan rcac(out.rcac, sizeof(out.rcac));
    ReturnErrorOnFailure(NewRootX509Cert(req, root, rcac));
    VerifyOrReturnError(rcac.size() > 0 && rcac.size() <= MC_IDENT_RCAC_MAX, CHIP_ERROR_INTERNAL);
    out.rcac_len = (uint16_t)rcac.size();
    return CHIP_NO_ERROR;
}

CHIP_ERROR issuer::load(const mc_ident_t &id)
{
    m_ready = false;
    VerifyOrReturnError(id.fabric_id != kUndefinedFabricId && id.rcac_len > 0 && id.rcac_len <= MC_IDENT_RCAC_MAX, CHIP_ERROR_INVALID_ARGUMENT);
    {
        P256SerializedKeypair ser;
        memcpy(ser.Bytes(), id.kp, MC_IDENT_KP_LEN);
        ReturnErrorOnFailure(ser.SetLength(MC_IDENT_KP_LEN));
        const CHIP_ERROR e = m_root.Deserialize(ser);
        mc_ident_wipe(ser.Bytes(), ser.Capacity());
        ReturnErrorOnFailure(e);
    }
    memcpy(m_rcac, id.rcac, id.rcac_len);
    m_rcac_len = id.rcac_len;
    m_root_dn = ChipDN{};
    ReturnErrorOnFailure(ExtractSubjectDNFromX509Cert(ByteSpan(m_rcac, m_rcac_len), m_root_dn));
    /* 根证书里的公钥必须就是这把密钥：hubdb 里存着的两样要是同一次生成的（坏了宁可起不来，也不签一张设备不认的证书） */
    P256PublicKey cert_pub;
    ReturnErrorOnFailure(ExtractPubkeyFromX509Cert(ByteSpan(m_rcac, m_rcac_len), cert_pub));
    VerifyOrReturnError(cert_pub.Matches(m_root.Pubkey()), CHIP_ERROR_INVALID_PUBLIC_KEY);
    memcpy(m_ipk, id.ipk, sizeof(m_ipk));
    m_fabric = id.fabric_id;
    m_ready = true;
    return CHIP_NO_ERROR;
}

esp_err_t issuer::initialize_credentials_issuer(chip::PersistentStorageDelegate &storage)
{
    (void)storage;                                    /* 身份不在 CHIP 的 KVS 里（hubdb 是正本）：load() 已经装好 */
    return m_ready ? ESP_OK : ESP_ERR_INVALID_STATE;
}

CHIP_ERROR issuer::issue_noc(NodeId node, FabricId fabric, const P256PublicKey &pub, MutableByteSpan &noc)
{
    VerifyOrReturnError(m_ready, CHIP_ERROR_UNINITIALIZED);
    VerifyOrReturnError(fabric == m_fabric, CHIP_ERROR_INVALID_ARGUMENT);   /* 只签自己的 fabric */
    VerifyOrReturnError(IsOperationalNodeId(node), CHIP_ERROR_INVALID_ARGUMENT);
    ChipDN dn;
    ReturnErrorOnFailure(dn.AddAttribute_MatterFabricId(fabric));
    ReturnErrorOnFailure(dn.AddAttribute_MatterNodeId(node));
    X509CertRequestParams req = { rand_serial(), kCertNotBefore, kNullCertTime, dn, m_root_dn };
    return NewNodeOperationalX509Cert(req, pub, m_root, noc);
}

esp_err_t issuer::generate_controller_noc_chain(NodeId node_id, FabricId fabric, P256Keypair &keypair, MutableByteSpan &rcac,
                                                MutableByteSpan &icac, MutableByteSpan &noc)
{
    if (!m_ready || rcac.size() < m_rcac_len) return ESP_ERR_INVALID_STATE;
    const CHIP_ERROR e = issue_noc(node_id, fabric, keypair.Pubkey(), noc);
    if (e != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "控制器自己的 NOC 没签出来：%s", e.AsString());
        return ESP_FAIL;
    }
    memcpy(rcac.data(), m_rcac, m_rcac_len);
    rcac.reduce_size(m_rcac_len);
    icac.reduce_size(0);                              /* 不用中间 CA */
    return ESP_OK;
}

CHIP_ERROR issuer::GenerateNOCChain(const ByteSpan &csrElements, const ByteSpan &csrNonce, const ByteSpan &attestationSignature,
                                    const ByteSpan &attestationChallenge, const ByteSpan &DAC, const ByteSpan &PAI,
                                    Callback::Callback<Controller::OnNOCChainGeneration> *onCompletion)
{
    (void)csrNonce; (void)attestationSignature; (void)attestationChallenge; (void)DAC; (void)PAI;
    VerifyOrReturnError(m_ready, CHIP_ERROR_UNINITIALIZED);
    VerifyOrReturnError(onCompletion != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    /* 节点号必须是 commissioner 这一次给的（我们在 PairDevice 时给、事先落盘）：没给就拒，绝不自己编一个 */
    VerifyOrReturnError(m_node_requested, CHIP_ERROR_INCORRECT_STATE);
    const NodeId node = m_next_node;
    m_node_requested = false;

    TLV::TLVReader reader;
    reader.Init(csrElements);
    if (reader.GetType() == TLV::kTLVType_NotSpecified) ReturnErrorOnFailure(reader.Next());
    ReturnErrorOnFailure(reader.Expect(TLV::kTLVType_Structure, TLV::AnonymousTag()));
    TLV::TLVType outer;
    ReturnErrorOnFailure(reader.EnterContainer(outer));
    ReturnErrorOnFailure(reader.Next(TLV::kTLVType_ByteString, TLV::ContextTag(1)));
    ByteSpan csr(reader.GetReadPoint(), reader.GetLength());
    ReturnErrorOnFailure(reader.ExitContainer(outer));

    P256PublicKey pub;
    ReturnErrorOnFailure(VerifyCertificateSigningRequest(csr.data(), csr.size(), pub));

    Platform::ScopedMemoryBuffer<uint8_t> noc;
    VerifyOrReturnError(noc.Alloc(kMaxDERCertLength), CHIP_ERROR_NO_MEMORY);
    MutableByteSpan noc_span(noc.Get(), kMaxDERCertLength);
    ReturnErrorOnFailure(issue_noc(node, m_next_fabric, pub, noc_span));

    uint8_t ipk[CHIP_CRYPTO_SYMMETRIC_KEY_LENGTH_BYTES];
    memcpy(ipk, m_ipk, sizeof(ipk));
    IdentityProtectionKeySpan ipk_span(ipk);
    ESP_LOGI(TAG, "给节点 %" PRIu64 " 签了 NOC（根直接签，无中间 CA）", (uint64_t)node);
    onCompletion->mCall(onCompletion->mContext, CHIP_NO_ERROR, noc_span, ByteSpan(), ByteSpan(m_rcac, m_rcac_len), MakeOptional(ipk_span),
                        Optional<NodeId>());
    mc_ident_wipe(ipk, sizeof(ipk));
    return CHIP_NO_ERROR;
}

} // namespace mc
