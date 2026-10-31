/* components/idf_fixes/crypto_race_diag.c — ECDSA 复位打坏并发 RSA / SHA 的对照压测（T9.47，诊断构建专用）
 *
 * 问的问题：一次硬件 ECDSA 验签，会不会把别的任务正在做的硬件大数运算 / SHA 打坏？修复（crypto_reset_lock.c）
 * 之后还会不会？
 *
 * 做法：开机 START_DELAY_S 后跑一轮，分五段、每段 PHASE_MS，结果每段一行 W 级日志（GET /api/v1/logs 读）：
 *   A  只有工作者（基线：没人打复位，必须全对 —— 不全对说明压测本身有问题）
 *   B  工作者 + ECDSA 验签任务，ECDSA 锁退回 v6.0.2 原样（旁路）
 *   C  同 B，修复生效
 *   B、C 再各来一遍，看可重复性
 * 工作者（本任务，优先级低于 ECDSA 任务）轮流做三样，每次都和 A 段之前算好的基准比对：
 *   mul  1024 × 1024 位硬件乘法（走普通乘法单元，算错不会断言，只计数）
 *   exp  2048 位模、e = 65537 的硬件模幂（RR 预先缓存，不走崩溃现场那条会断言的 failover 乘法）；
 *        复位吞掉完成中断时 IDF 等 2 s 返回错误，单独计 err
 *   sha  4 KB 的 SHA-256（PSA → 硬件 SHA）
 * ECDSA 任务用 P-256 公钥反复验同一个签名（与 OT 的 SRP 验签同一条 PSA 路径：psa_verify_hash →
 *   esp_ecdsa_transparent_verify_hash），验完睡 ECDSA_GAP_MS；hw_locks 是这一段里硬件 ECDSA 拿锁的次数，
 *   为 0 说明验签没走硬件驱动、这段对照无效。
 *
 * 判读：A 全对；B 有错（证明根因）；C 全对（证明修复）。
 * 节奏：工作者忙 WORK_BURST_US 后睡 WORK_SLEEP_MS，ECDSA 一次约 18 ms 后睡 ECDSA_GAP_MS —— 两个都让出 CPU，
 *   IDLE 跑得到（任务看门狗 5 s）。ECDSA 任务在节拍上醒，工作者的忙段跨几个节拍，所以常常是在工作者
 *   运算中途被抢占，正是线上的形状（ot_main 抢占 TLS 任务）。
 * ⚠️ B 段是真把缺陷放出来：这期间网关自己的 TLS / Thread 加密也可能被打坏（重连、丢帧，最坏是和 10-22
 *   一样的断言 panic）。只在诊断构建里跑，测完 OTA 刷回正常固件。 */
#include "sdkconfig.h"

#if CONFIG_BITENGINE_DIAG_CRYPTO_RACE
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "mbedtls/bignum.h"
#include "psa/crypto.h"
#include "idf_fixes.h"

static const char *TAG = "crypto_race";

#define START_DELAY_S   90       /* 等开机那一波 SRP 重注册、Muse 首次握手过去 */
#define PHASE_MS        20000
#define WORK_BURST_US   3000
#define WORK_SLEEP_MS   2
#define ECDSA_GAP_MS    15
#define WORKER_PRIO     3        /* 低于 ECDSA 任务；低于 muse_sess(4)、远低于 ot_main(16) */
#define ECDSA_PRIO      6
#define SHA_LEN         4096

typedef struct {
    uint32_t n, bad, err;
} op_stat_t;

static volatile bool s_ecdsa_run;
static TaskHandle_t s_ctl;
static psa_key_id_t s_pub;
static uint8_t s_hash[32], s_sig[64];
static size_t s_sig_len;
static volatile uint32_t s_ecdsa_ok, s_ecdsa_fail;

static void ecdsa_task(void *arg)
{
    (void)arg;
    while (s_ecdsa_run) {
        psa_status_t st = psa_verify_hash(s_pub, PSA_ALG_DETERMINISTIC_ECDSA(PSA_ALG_SHA_256),
                                          s_hash, sizeof(s_hash), s_sig, s_sig_len);
        if (st == PSA_SUCCESS) s_ecdsa_ok++; else s_ecdsa_fail++;
        vTaskDelay(pdMS_TO_TICKS(ECDSA_GAP_MS));
    }
    xTaskNotifyGive(s_ctl);
    vTaskDelete(NULL);
}

/* 随机的 bits 位正整数，最高位置 1（位数固定，硬件路径固定） */
static int rand_mpi(mbedtls_mpi *x, size_t bits, bool odd)
{
    uint8_t buf[256];
    size_t len = bits / 8;
    if (len == 0 || len > sizeof(buf)) return -1;
    esp_fill_random(buf, len);
    buf[0] |= 0x80;
    if (odd) buf[len - 1] |= 0x01;
    return mbedtls_mpi_read_binary(x, buf, len);
}

static bool make_ecdsa_key(void)
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t pair = 0;
    uint8_t pub[65];
    size_t pub_len = 0;
    bool ok = false;

    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_HASH);
    psa_set_key_algorithm(&a, PSA_ALG_DETERMINISTIC_ECDSA(PSA_ALG_SHA_256));
    if (psa_generate_key(&a, &pair) != PSA_SUCCESS) goto out;
    esp_fill_random(s_hash, sizeof(s_hash));
    if (psa_sign_hash(pair, PSA_ALG_DETERMINISTIC_ECDSA(PSA_ALG_SHA_256), s_hash, sizeof(s_hash),
                      s_sig, sizeof(s_sig), &s_sig_len) != PSA_SUCCESS) goto out;
    if (psa_export_public_key(pair, pub, sizeof(pub), &pub_len) != PSA_SUCCESS) goto out;

    /* 与 OT 的 otPlatCryptoEcdsaVerify 一样：导入公钥再验 */
    psa_reset_key_attributes(&a);
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&a, PSA_ALG_DETERMINISTIC_ECDSA(PSA_ALG_SHA_256));
    ok = psa_import_key(&a, pub, pub_len, &s_pub) == PSA_SUCCESS;
out:
    if (pair) psa_destroy_key(pair);
    psa_reset_key_attributes(&a);
    return ok;
}

static void ctl_task(void *arg)
{
    (void)arg;
    mbedtls_mpi X, Y, Zmul, A, E, M, RR, Zexp, Z;
    mbedtls_mpi_init(&X); mbedtls_mpi_init(&Y); mbedtls_mpi_init(&Zmul);
    mbedtls_mpi_init(&A); mbedtls_mpi_init(&E); mbedtls_mpi_init(&M);
    mbedtls_mpi_init(&RR); mbedtls_mpi_init(&Zexp); mbedtls_mpi_init(&Z);
    uint8_t *buf = heap_caps_malloc(SHA_LEN, MALLOC_CAP_8BIT);
    uint8_t sha_ref[32], sha[32];
    size_t olen;
    bool stuck = false;     /* ECDSA 任务没按时退出：它可能还在用 s_pub，别销毁 */

    vTaskDelay(pdMS_TO_TICKS(START_DELAY_S * 1000));
    s_ctl = xTaskGetCurrentTaskHandle();

    /* ── 准备：基准值在没有 ECDSA 并发时算 ── */
    if (!buf || psa_crypto_init() != PSA_SUCCESS || !make_ecdsa_key()) {
        ESP_LOGE(TAG, "准备失败（buf=%p）", buf);
        goto done;
    }
    esp_fill_random(buf, SHA_LEN);
    if (rand_mpi(&X, 1024, false) || rand_mpi(&Y, 1024, false) || mbedtls_mpi_mul_mpi(&Zmul, &X, &Y) ||
        rand_mpi(&M, 2048, true) || rand_mpi(&A, 2040, false) || mbedtls_mpi_lset(&E, 65537) ||
        mbedtls_mpi_exp_mod(&Zexp, &A, &E, &M, &RR) ||       /* 顺带把 RR 缓存进 RR */
        psa_hash_compute(PSA_ALG_SHA_256, buf, SHA_LEN, sha_ref, sizeof(sha_ref), &olen) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "基准计算失败");
        goto done;
    }

    static const char phases[] = "ABCBC";
    for (int pi = 0; phases[pi]; pi++) {
        char ph = phases[pi];
        op_stat_t mul = {0}, ex = {0}, sh = {0};
        idf_fixes_diag_set_bypass(ph == 'B');
        s_ecdsa_ok = s_ecdsa_fail = 0;
        uint32_t locks0 = idf_fixes_diag_ecdsa_locks();
        bool ecdsa = ph != 'A';
        if (ecdsa) {
            s_ecdsa_run = true;
            if (xTaskCreate(ecdsa_task, "cr_ecdsa", 4096, NULL, ECDSA_PRIO, NULL) != pdPASS) {
                ESP_LOGE(TAG, "ECDSA 任务建不起来");
                s_ecdsa_run = false;
                break;
            }
        }

        int64_t end = esp_timer_get_time() + (int64_t)PHASE_MS * 1000;
        unsigned k = 0;
        while (esp_timer_get_time() < end) {
            int64_t burst_end = esp_timer_get_time() + WORK_BURST_US;
            while (esp_timer_get_time() < burst_end) {
                switch (k++ % 3) {
                case 0:
                    mul.n++;
                    if (mbedtls_mpi_mul_mpi(&Z, &X, &Y)) mul.err++;
                    else if (mbedtls_mpi_cmp_mpi(&Z, &Zmul)) mul.bad++;
                    break;
                case 1:
                    ex.n++;
                    if (mbedtls_mpi_exp_mod(&Z, &A, &E, &M, &RR)) ex.err++;
                    else if (mbedtls_mpi_cmp_mpi(&Z, &Zexp)) ex.bad++;
                    break;
                default:
                    sh.n++;
                    if (psa_hash_compute(PSA_ALG_SHA_256, buf, SHA_LEN, sha, sizeof(sha), &olen) != PSA_SUCCESS) sh.err++;
                    else if (memcmp(sha, sha_ref, sizeof(sha))) sh.bad++;
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(WORK_SLEEP_MS));
        }

        if (ecdsa) {
            s_ecdsa_run = false;
            if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000))) {
                ESP_LOGE(TAG, "ECDSA 任务 5 s 没退出，压测中止");
                stuck = true;
                break;
            }
        }
        ESP_LOGW(TAG, "相 %c%s: ecdsa ok=%lu fail=%lu hw_locks=%lu | mul %lu/%lu 错 (err %lu) | exp %lu/%lu 错 (err %lu) | sha %lu/%lu 错 (err %lu)",
                 ph, ph == 'A' ? "（基线）" : ph == 'B' ? "（旁路=v6.0.2）" : "（修复）",
                 (unsigned long)s_ecdsa_ok, (unsigned long)s_ecdsa_fail,
                 (unsigned long)(idf_fixes_diag_ecdsa_locks() - locks0),
                 (unsigned long)mul.bad, (unsigned long)mul.n, (unsigned long)mul.err,
                 (unsigned long)ex.bad, (unsigned long)ex.n, (unsigned long)ex.err,
                 (unsigned long)sh.bad, (unsigned long)sh.n, (unsigned long)sh.err);
    }
    idf_fixes_diag_set_bypass(false);
    if (stuck) ESP_LOGE(TAG, "压测中止（修复已恢复生效）");
    else ESP_LOGW(TAG, "压测结束（修复已恢复生效）");

done:
    if (s_pub && !stuck) {
        psa_destroy_key(s_pub);
        s_pub = 0;
    }
    mbedtls_mpi_free(&X); mbedtls_mpi_free(&Y); mbedtls_mpi_free(&Zmul);
    mbedtls_mpi_free(&A); mbedtls_mpi_free(&E); mbedtls_mpi_free(&M);
    mbedtls_mpi_free(&RR); mbedtls_mpi_free(&Zexp); mbedtls_mpi_free(&Z);
    heap_caps_free(buf);
    /* ECDSA 任务还活着时它退出前会 xTaskNotifyGive(s_ctl)：本任务删了就是写已释放的 TCB，所以留着不删 */
    while (stuck) vTaskDelay(portMAX_DELAY);
    vTaskDelete(NULL);
}

void crypto_race_diag_start(void)
{
    if (xTaskCreate(ctl_task, "cr_ctl", 6144, NULL, WORKER_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "压测任务建不起来");
        return;
    }
    ESP_LOGW(TAG, "诊断构建：%d s 后跑 ECDSA 复位对照压测（A/B/C/B/C 各 %d s）", START_DELAY_S, PHASE_MS / 1000);
}
#endif /* CONFIG_BITENGINE_DIAG_CRYPTO_RACE */
