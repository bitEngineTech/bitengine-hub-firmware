/* noise_interop_harness —— 本仓库自加（不是上游文件）：固件的 noise_core 发起方对一个独立实现的响应方走完 Noise XX。
 *
 * 为什么要它：上游 test_noise_core*.py 只测了 AES-GCM / X25519 原语与 Transport 自己跟自己收发，从没拿 InitiatorHandshake
 * 和别的实现握过手。2026-10-22 变异：把协议名 "Noise_XX_25519_AESGCM_SHA256" 改掉一个字符，上游三条测试照样全绿。
 * 对端是 Linux SDK 的 NoiseXXResponder（Python + cryptography，与 C++ 代码无共享），由 test_noise_interop.py 驱动。
 *
 * 协议：标准输入输出，一行一条 "<标签> <十六进制>"。
 *   → M1 <msg1>
 *   ← M2 <msg2>                         ReadMessage2 失败时 → ERR m2，退出码 3
 *   → PX <对端在 msg2 里带的载荷>
 *   → M3 <msg3>（载荷固定为 kMsg3Payload）
 *   → HH <握手哈希>（Finalize 会把它清零 —— 上游 DestroySecrets —— 所以在 Finalize 之前打）
 *   → RS <对端静态公钥>
 *   → C2S <密文> × kMessages            计数 0..kMessages-1
 *   ← S2C <密文> × kMessages            → PT <明文>，打不开 → ERR open，退出码 4
 *   → C2S <密文>                         发送计数先设成 kHighNonce（只在 NOISE_BUILDING_TESTS 下有）：钉 nonce 高 4 字节的字节序
 *   ← S2C <密文>                         接收计数同样设成 kHighNonce → PT <明文>
 */
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <xplat/noise/core/InitiatorHandshake.h>
#include <xplat/noise/core/PsaCryptoBackend.h>
#include <xplat/noise/core/Transport.h>

namespace tn = musegadgets::noise::core;

namespace {

constexpr char kMsg3Payload[] = "bitengine-msg3-payload";
constexpr int kMessages = 4;
constexpr uint64_t kHighNonce = 0x0001020304050607ull;   /* < Python 侧 MAX_SAFE_NONCE (2^53 - 1) */

void PutHex(const char* tag, const uint8_t* p, size_t n) {
  std::printf("%s ", tag);
  for (size_t i = 0; i < n; ++i) std::printf("%02x", p[i]);
  std::printf("\n");
  std::fflush(stdout);
}

bool GetHex(const char* tag, std::vector<uint8_t>& out) {
  char line[140000];
  if (!std::fgets(line, sizeof(line), stdin)) return false;
  const size_t tl = std::strlen(tag);
  if (std::strncmp(line, tag, tl) != 0 || line[tl] != ' ') return false;
  out.clear();
  for (const char* q = line + tl + 1; q[0] && q[0] != '\n' && q[1]; q += 2) {
    unsigned v = 0;
    if (std::sscanf(q, "%2x", &v) != 1) return false;
    out.push_back(static_cast<uint8_t>(v));
  }
  return true;
}

int Fail(const char* what, int code) {
  std::printf("ERR %s\n", what);
  std::fflush(stdout);
  return code;
}

std::vector<uint8_t> C2sPlaintext(int i) {
  std::string s = "c2s-" + std::to_string(i) + "-";
  s.append(static_cast<size_t>(i) * 37u, static_cast<char>('a' + i));
  return std::vector<uint8_t>(s.begin(), s.end());
}

bool SealAndPut(tn::Transport& t, const std::vector<uint8_t>& pt) {
  std::vector<uint8_t> ct(pt.size() + tn::Transport::kTagSize);
  const tn::StatusWithSize r = t.Seal(tn::ConstByteSpan(pt.data(), pt.size()), tn::ByteSpan(ct.data(), ct.size()));
  if (!r.ok() || r.size() != ct.size()) return false;
  PutHex("C2S", ct.data(), ct.size());
  return true;
}

bool GetAndOpen(tn::Transport& t) {
  std::vector<uint8_t> ct;
  if (!GetHex("S2C", ct) || ct.size() < tn::Transport::kTagSize) return false;
  std::vector<uint8_t> pt(ct.size() - tn::Transport::kTagSize);
  const tn::StatusWithSize r = t.Open(tn::ConstByteSpan(ct.data(), ct.size()), tn::ByteSpan(pt.data(), pt.size()));
  if (!r.ok() || r.size() != pt.size()) return false;
  PutHex("PT", pt.data(), pt.size());
  return true;
}

}  // namespace

int main() {
  tn::PsaCryptoBackend backend;
  tn::InitiatorHandshake hs(backend);

  std::array<uint8_t, tn::InitiatorHandshake::kMessage1Size> m1{};
  const tn::StatusWithSize w1 = hs.WriteMessage1(tn::ByteSpan(m1));
  if (!w1.ok()) return Fail("m1", 2);
  PutHex("M1", m1.data(), w1.size());

  std::vector<uint8_t> m2;
  if (!GetHex("M2", m2)) return Fail("m2-input", 2);
  std::vector<uint8_t> extra(m2.size());
  size_t extraLen = 0;
  if (!hs.ReadMessage2(tn::ConstByteSpan(m2.data(), m2.size()), tn::ByteSpan(extra.data(), extra.size()), extraLen).ok()) {
    return Fail("m2", 3);
  }
  PutHex("PX", extra.data(), extraLen);

  const size_t payloadLen = sizeof(kMsg3Payload) - 1;
  std::vector<uint8_t> m3(tn::InitiatorHandshake::kMinMessage3Size + payloadLen);
  const tn::StatusWithSize w3 = hs.WriteMessage3(
      tn::ConstByteSpan(reinterpret_cast<const uint8_t*>(kMsg3Payload), payloadLen), tn::ByteSpan(m3.data(), m3.size()));
  if (!w3.ok()) return Fail("m3", 2);
  PutHex("M3", m3.data(), w3.size());
  PutHex("HH", hs.handshakeHash().data(), hs.handshakeHash().size());

  std::array<uint8_t, tn::Transport::kKeySize> sendKey{};
  std::array<uint8_t, tn::Transport::kKeySize> recvKey{};
  if (!hs.Finalize(tn::ByteSpan(sendKey), tn::ByteSpan(recvKey)).ok()) return Fail("finalize", 2);
  PutHex("RS", hs.peerStaticPublicKey().data(), hs.peerStaticPublicKey().size());

  tn::Transport t(backend, tn::ConstByteSpan(sendKey), tn::ConstByteSpan(recvKey));
  if (!t.status().ok()) return Fail("transport", 2);
  for (int i = 0; i < kMessages; ++i) {
    if (!SealAndPut(t, C2sPlaintext(i))) return Fail("seal", 2);
  }
  for (int i = 0; i < kMessages; ++i) {
    if (!GetAndOpen(t)) return Fail("open", 4);
  }
  t.setSendNonceForTesting(kHighNonce);
  t.setRecvNonceForTesting(kHighNonce);
  if (!SealAndPut(t, C2sPlaintext(kMessages))) return Fail("seal", 2);
  if (!GetAndOpen(t)) return Fail("open", 4);
  return 0;
}
