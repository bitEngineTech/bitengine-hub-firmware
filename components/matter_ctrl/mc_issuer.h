/* mc_issuer —— 网关 fabric 的凭据签发器（docs/15 设计 9.3；M7 第二步）。只在 CHIP 任务里用。
 *
 * 换掉 esp-matter 默认的 example_credentials_issuer（CHIP 的 ExampleOperationalCredentialsIssuer）：
 *  - 它发给设备的 IPK 是 CHIP 写死的公开测试值（源码自带 TODO #13825），设备一旦配进来就带着；
 *  - 证书有效期 2000-01-01 → 2010-01-01（mNow = 0、10 年）：CHIP 的默认策略在不知道准确时间时不查过期才配得上，
 *    对过时的设备会判过期拒绝；
 *  - 根 / 中间 CA 私钥放在 CHIP 的 KVS（matter 分区），分区一坏设备全成孤儿。
 * 这里：自签根 CA、不用中间 CA（根直接签 NOC）、随机 IPK、notBefore 固定在 2025-01-01、notAfter 用"无到期日"
 * （kNullCertTime，X.509 的 99991231235959Z）；身份（mc_ident_t）是 hubdb 里的正本，matter 分区只放 CHIP 自己的状态。 */
#pragma once
#include "mc_ident_core.h"

#include <esp_matter_controller_credentials_issuer.h>
#include <credentials/CHIPCert.h>

namespace mc {

/* 新建一份身份（CHIP 任务里调：要 CHIP 的密码学 PAL）：给定的 fabric ID（启动任务先随机选好，matter 分区按它绑定）、
   随机根 CA ID / IPK、P-256 根密钥对、自签根证书 */
CHIP_ERROR ident_generate(uint64_t fabric_id, mc_ident_t &out);

class issuer : public esp_matter::controller::credentials_issuer, public chip::Controller::OperationalCredentialsDelegate {
public:
    /* 装进身份（拷一份密钥对进 CHIP 的 P256Keypair、取根证书的主题 DN）；之后 id 可以清掉 */
    CHIP_ERROR load(const mc_ident_t &id);
    const uint8_t *ipk() const { return m_ipk; }

    /* esp_matter::controller::credentials_issuer */
    esp_err_t initialize_credentials_issuer(chip::PersistentStorageDelegate &storage) override;
    chip::Controller::OperationalCredentialsDelegate *get_delegate() override { return this; }
    esp_err_t generate_controller_noc_chain(chip::NodeId node_id, chip::FabricId fabric, chip::Crypto::P256Keypair &keypair,
                                            chip::MutableByteSpan &rcac, chip::MutableByteSpan &icac,
                                            chip::MutableByteSpan &noc) override;

    /* chip::Controller::OperationalCredentialsDelegate（commissioner 在要 NOC 之前先告诉节点号与 fabric ID，CHIPDeviceController.cpp） */
    CHIP_ERROR GenerateNOCChain(const chip::ByteSpan &csrElements, const chip::ByteSpan &csrNonce,
                                const chip::ByteSpan &attestationSignature, const chip::ByteSpan &attestationChallenge,
                                const chip::ByteSpan &DAC, const chip::ByteSpan &PAI,
                                chip::Callback::Callback<chip::Controller::OnNOCChainGeneration> *onCompletion) override;
    void SetNodeIdForNextNOCRequest(chip::NodeId nodeId) override
    {
        m_next_node = nodeId;
        m_node_requested = true;
    }
    void SetFabricIdForNextNOCRequest(chip::FabricId fabricId) override { m_next_fabric = fabricId; }

private:
    CHIP_ERROR issue_noc(chip::NodeId node, chip::FabricId fabric, const chip::Crypto::P256PublicKey &pub, chip::MutableByteSpan &noc);

    bool m_ready = false;
    chip::Crypto::P256Keypair m_root;
    chip::Credentials::ChipDN m_root_dn;
    chip::FabricId m_fabric = chip::kUndefinedFabricId;
    uint8_t m_ipk[MC_IDENT_IPK_LEN] = {};
    uint8_t m_rcac[MC_IDENT_RCAC_MAX] = {};
    uint16_t m_rcac_len = 0;
    chip::NodeId m_next_node = chip::kUndefinedNodeId;
    bool m_node_requested = false;
    chip::FabricId m_next_fabric = chip::kUndefinedFabricId;
};

} // namespace mc
