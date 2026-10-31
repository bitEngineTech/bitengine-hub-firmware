/* mc_paa —— 把内置的 PAA 快照（paa_snapshot.c）装成 CHIP 的设备认证信任库（docs/15 设计 9.7；TODO T9.53）。CHIP 任务里、
 * setup_commissioner 之前调（它在里面 get_attestation_trust_store() 一次，交给默认的 DAC 校验器）。
 *
 * H1（esp_matter 1.6.0~1）：CONFIG_CUSTOM_ATTESTATION_TRUST_STORE 时 get_attestation_trust_store() 返回
 * set_custom_attestation_trust_store() 给的那个（esp_matter_attestation_trust_store.cpp）；CHIP 的 ArrayAttestationTrustStore
 * 按 SKID 在 DER 数组里找（DeviceAttestationVerifier.h），不拷贝 —— span 与数组都要活到关机（这里是静态 / 只读段）。
 * 吊销检查没开（CONFIG_DCL_REVOCATION_POINTS_REVOKED_DAC_CHAIN_CHECK=n）：9.7 记为已知限制。 */
#include "mc_paa.h"

#include <atomic>

#include <esp_log.h>

#include <credentials/attestation_verifier/DefaultDeviceAttestationVerifier.h>
#include <credentials/attestation_verifier/DeviceAttestationVerifier.h>
#include <esp_matter_attestation_trust_store.h>
#include <lib/support/CHIPMem.h>

static const char *TAG = "mc_paa";

/* "允许测试设备"（设计 9.7、10.1；CHECKLIST MC22，2026-10-24 用户定：做、默认关、只在本地开）。开着时多认两样，签名照验：
 *  ① CHIP 自带的测试 PAA（GetTestAttestationTrustStore：Matter Test PAA FFF1 / FFF2 / 无厂商号那几张）—— 开发板、DIY 的 DAC 链到它们；
 *  ② 用 CSA 测试密钥签的 CD（DefaultDACVerifier::EnableCdTestKeySupport）—— CHIP 只对"开发 / 测试"类型（kEnableCdTestKeysForProvisionalCds
 *     为 true，所以还有"临时"类型）的 CD 放行测试密钥，
 *     CD 里的厂商号 / 产品号与 DAC 对不上照样拒（DefaultDeviceAttestationVerifier.cpp 的 ValidateCertificateDeclarationPayload）。
 * H1（connectedhomeip 随 esp_matter 1.6.0~1）：ArrayAttestationTrustStore 找不到返回 CHIP_ERROR_CA_CERT_NOT_FOUND、不碰输出缓冲；
 * esp-matter 在 setup_commissioner 里建一次默认校验器、固定信任库指针，并按 CONFIG_ESP_MATTER_COMMISSIONER_SUPPORT_TEST_CD=n
 * 关掉测试 CD 密钥（esp_matter_controller_client.cpp）—— 所以信任库包一层按开关查，CD 密钥在 setup 之后、切换时各设一次。 */
static std::atomic<bool> s_allow_test{false};

class mc_trust_store final : public chip::Credentials::AttestationTrustStore {
public:
    mc_trust_store(const chip::ByteSpan *spans, size_t n) : m_prod(spans, n) {}
    CHIP_ERROR GetProductAttestationAuthorityCert(const chip::ByteSpan &skid, chip::MutableByteSpan &out) const override
    {
        const CHIP_ERROR e = m_prod.GetProductAttestationAuthorityCert(skid, out);
        if (e != CHIP_ERROR_CA_CERT_NOT_FOUND || !s_allow_test.load()) return e;
        const chip::Credentials::AttestationTrustStore *t = chip::Credentials::GetTestAttestationTrustStore();
        return t ? t->GetProductAttestationAuthorityCert(skid, out) : e;
    }

private:
    chip::Credentials::ArrayAttestationTrustStore m_prod;
};

bool mc_paa_install(void)
{
    static mc_trust_store *s_store;
    if (s_store) return true;
    if (mc_paa_count == 0) return false;
    auto *spans = static_cast<chip::ByteSpan *>(chip::Platform::MemoryCalloc(mc_paa_count, sizeof(chip::ByteSpan)));
    if (!spans) return false;
    for (unsigned i = 0; i < mc_paa_count; i++) new (&spans[i]) chip::ByteSpan(mc_paa_certs[i].der, mc_paa_certs[i].len);
    s_store = chip::Platform::New<mc_trust_store>(spans, (size_t)mc_paa_count);
    if (!s_store) return false;
    chip::Credentials::set_custom_attestation_trust_store(s_store);
    ESP_LOGI(TAG, "设备认证用内置的 PAA 快照：%u 张（%s 取自 CSA DCL）", mc_paa_count, mc_paa_snapshot_date);
    return true;
}

void mc_paa_set_allow_test(bool on) { s_allow_test.store(on); }

bool mc_paa_allow_test(void) { return s_allow_test.load(); }

void mc_paa_apply_test(void)
{
    chip::Credentials::DeviceAttestationVerifier *v = chip::Credentials::GetDeviceAttestationVerifier();
    const bool on = s_allow_test.load();
    if (v) v->EnableCdTestKeySupport(on);
    ESP_LOGW(TAG, "测试设备（测试证书 / 测试 CD）%s", on ? "允许配网" : "拒绝（默认）");
}
