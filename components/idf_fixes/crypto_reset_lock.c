/* components/idf_fixes/crypto_reset_lock.c — ECDSA 锁连带 SHA/AES 与 MPI 锁（T9.47，2026-10-23）
 *
 * 现象：10-22 直连型 OTA 后首启，Muse 会话的 TLS 握手在验服务器证书链的 RSA 签名时 panic：
 *   assert failed: mpi_mult_mpi_failover_mod_mult esp_bignum.c:666 (z_words - mpi_words(Z) <= (size_t)1)
 *   即硬件大数乘法读回的结果位数不对 —— 外设算错了。1/5 次复现。
 *
 * 根因（IDF v6.0.2 在 C5 上的缺陷，上游 09-08 修复并回合到 release/v6.0，v6.0.3 不含）：
 *   C5 的复位是连着的：ECDSA 的复位位置上时 RSA（mpi_ll_reset_register 的注释："otherwise RSA is held in
 *   reset"）和 SHA（sha_ll_reset_register）也被按在复位里，SHA 又与 AES 共用 DMA。v6.0.2 的硬件 ECDSA 驱动
 *   每次拿硬件都 esp_crypto_ecdsa_enable_periph_clk(true) → ecdsa_ll_reset_register() 打一个复位脉冲，
 *   而 esp_crypto_ecdsa_lock_acquire() 在 C5 上只连带 ECC 锁（SOC_ECDSA_USES_MPI 没定义）。
 *   于是别的任务正拿着 MPI 锁做 RSA（或拿着 SHA/AES 锁做哈希 / 加解密）时，一次硬件 ECDSA 验签就把那次运算
 *   复位掉，读回来的是垃圾。
 *   板上谁在并发：Thread 的 SRP 服务端每条设备注册都在 ot_main（优先级 16）里做一次硬件 ECDSA 验签
 *   （CONFIG_MBEDTLS_HARDWARE_ECDSA_VERIFY=y）；TLS（Muse / 云 OTA / webhook / mqtts，优先级都更低）做 RSA。
 *   开机后 Thread 设备成批重注册、Muse 同时首次握手 —— 正是"OTA 后首启"。coredump 里 ot_main 停在
 *   Srp::Server::ProcessDnsUpdate → HandleUpdate，也就是刚验完一条签名（证据在
 *   ~/bitengine-backups/coredumps/20261022-muse-tls-bignum/）。
 *   两种配置都有这个问题：HA 型同样有 Thread BR 与 TLS。
 *
 * 修法：照搬上游 release/v6.0 的 ba13c2852a + 48a864dd75 —— ECDSA 锁按 ecdsa → ecc → sha_aes → mpi 的顺序
 *   连带拿锁（与 esp_crypto_ds_lock_acquire 的 sha_aes < mpi 一致，不引入新的锁序）。v6.0.2 里调这把锁的只有
 *   硬件 ECDSA 驱动（psa_crypto_driver_esp_ecdsa.c 的 esp_ecdsa_acquire/release_hardware），两次调用之间只做
 *   ecdsa_hal_* 寄存器操作、不碰 SHA / MPI，所以多拿的两把锁不会自锁。代价：一次硬件验签（实测约 18 ms，
 *   thread_br/ot_cpu_guard.c 头注）期间 RSA / SHA / AES 的使用者要等；拿锁是互斥量，有优先级继承。
 *   链接期用 --wrap 截住（CMakeLists.txt），不改 IDF 源码：~/esp-idf-v6.0.2 是共用的 worktree。
 *   上游另外几处（Key Manager 不复位、HMAC / DS 的复位次序）本固件用不到：不开 flash 加密、不用 HMAC / DS。
 *   ⚠️ 将来若开硬件 ECDSA 签名（CONFIG_MBEDTLS_HARDWARE_ECDSA_SIGN，或 muse_pair 的 EFUSE_AUTH）：驱动的签名 / 导出公钥
 *   在锁内有不设上限的重试循环（do…while(process_again)），硬件一直失败时会连 SHA/AES 与 MPI 一起永久占住 ——
 *   上游同样如此；开之前先看那两处（审计 ⚠️，2026-10-23）。
 *
 * 删除条件：IDF 升到带这两个提交的版本（v6.0.4 起，或 v6.1）就删掉整个 components/idf_fixes 的这一处。
 *   不删也编不过，三道绊线：IDF 高于 v6.0.3 先停下来核对（版本钉）；上游第二个提交给 C5 定义了
 *   SOC_ECDSA_USES_MPI（#error）；第一个提交让 ECDSA 锁连带 SHA/AES 锁（check_idf_lock.cmake 读 IDF 源码查）。两道都是为了不重复加锁 —— 这几把锁不可重入，
 *   重复拿 = 第一次验签就锁死。
 *
 * 诊断构建（CONFIG_BITENGINE_DIAG_CRYPTO_RACE）多一个运行时旁路，压测用它在同一块固件里对照修复前后
 *   （crypto_race_diag.c）。出厂固件没有这个开关（VERIFY.sh 钉着）。 */
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>
#include "soc/soc_caps.h"
#include "esp_crypto_lock.h"
#include "idf_fixes.h"

#if !SOC_ECDSA_SUPPORTED || !SOC_MPI_SUPPORTED || !(SOC_SHA_SUPPORTED || SOC_AES_SUPPORTED)
#error "idf_fixes: 这个回合只针对有 ECDSA / MPI / SHA 外设的芯片（ESP32-C5）"
#endif

#include "esp_idf_version.h"
#if ESP_IDF_VERSION > ESP_IDF_VERSION_VAL(6, 0, 3)
#error "idf_fixes: IDF 高于 v6.0.3（已核：v6.0.2 / v6.0.3 不含上游修复），先确认这版是否已带 ba13c2852a + 48a864dd75：带了就删掉这处 wrap，没带就把这里的版本号往上挪（T9.47）"
#endif

#if SOC_ECDSA_USES_MPI
#error "idf_fixes: 这版 IDF 已带上游 ECDSA 锁修复（C5 定义了 SOC_ECDSA_USES_MPI），删掉这处 wrap，否则重复加锁会锁死（T9.47）"
#endif

void __real_esp_crypto_ecdsa_lock_acquire(void);
void __real_esp_crypto_ecdsa_lock_release(void);

#if CONFIG_BITENGINE_DIAG_CRYPTO_RACE
static volatile bool s_bypass;      /* 压测切换；OT 的 SRP 验签随时在跑，切换可能落在一次验签中间 ——
                                       所以每次拿锁时记下这次多拿了没有，放锁照记录放，不再读 s_bypass */
static bool s_extra_held;           /* 本次是否多拿了两把锁：在 ECDSA 锁内写、在 ECDSA 锁内读 */
static volatile uint32_t s_locks;   /* 走过这里几次 —— 证明硬件 ECDSA 真的经过这把锁 */

void idf_fixes_diag_set_bypass(bool bypass) { s_bypass = bypass; }
uint32_t idf_fixes_diag_ecdsa_locks(void) { return s_locks; }
#endif

void __wrap_esp_crypto_ecdsa_lock_acquire(void)
{
    __real_esp_crypto_ecdsa_lock_acquire();   /* ecdsa → ecc */
#if CONFIG_BITENGINE_DIAG_CRYPTO_RACE
    s_locks++;
    s_extra_held = !s_bypass;
    if (!s_extra_held) return;
#endif
    esp_crypto_sha_aes_lock_acquire();        /* ECDSA 复位按住 SHA，SHA 与 AES 共用 DMA */
    esp_crypto_mpi_lock_acquire();            /* ECDSA 复位按住 RSA */
}

void __wrap_esp_crypto_ecdsa_lock_release(void)
{
#if CONFIG_BITENGINE_DIAG_CRYPTO_RACE
    if (!s_extra_held) {
        __real_esp_crypto_ecdsa_lock_release();
        return;
    }
#endif
    esp_crypto_mpi_lock_release();
    esp_crypto_sha_aes_lock_release();
    __real_esp_crypto_ecdsa_lock_release();
}
