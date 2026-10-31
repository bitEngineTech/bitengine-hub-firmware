/* components/thread_br/ot_cpu_guard.c — OT 任务过载时让出 CPU（2026-10-07）
 *
 * 为什么要它：网关重启后，Thread 设备会在 10 s 抖动窗口里成批重新做 SRP 注册（OT SRP client 的
 * kMaxTxJitterOnServerRestart）。每条注册在 OT 任务（优先级 16）里要 ~35-40 ms CPU，其中硬件 ECDSA
 * 验签就占 ~18 ms。252 台挤在 10 s 里（每秒 ~25 条）OT 任务会被连续占满 —— IDLE 一连 5 s 跑不到，
 * 任务看门狗 panic（2026-10-07 实测，coredump 里占着 CPU 的是 ot_main）。而重启又会让设备再次成批重注册：
 * 不加防护就是重启循环。过载时应当"处理变慢"，不该"整机重启"。
 *
 * 怎么做：每条 SRP 注册都要在 OT 任务里验一次签（otPlatCryptoEcdsaVerify），链接期用 --wrap 截住它
 * （thread_br/CMakeLists.txt）；验完看 IDLE 上次跑到是多久以前（idle hook 记的时刻），超过
 * GUARD_STARVE_MS 就 vTaskDelay(GUARD_YIELD_MS)，让低优先级任务和 IDLE 喘口气。
 *   - 平时（IDLE 每毫秒都跑得到）开销是一次时间比较；
 *   - 过载时每 ~1 s 多睡 20 ms：OT 的处理变慢、应答变晚，SRP client 超时会自己重试 —— 这是想要的背压；
 *   - 睡的时候持着 OT 锁：等这把锁的多是 httpd 的 OTBR 接口、srp_readv 这类低优先级任务，晚一点无妨；
 *     默认事件循环（优先级 20）在 thread_br_pause/resume 里也会等它（最多 2 s），那时 ot_main 会被优先级继承
 *     抬到 20 睡完这 20 ms —— 只多 20 ms，等待方超时后继承即撤销。esp_timer 回调（child_stats_timer_cb）只试不等。
 * 不在这里兜的（盲区）：
 *   - 不经过验签的 OT 过载（例如大量租约同时到期、成批删除代播服务）—— 守卫只在验签之后检查；
 *   - mdns 任务（优先级 1）自己的长动作 —— 一次函数调用、持着 mdns 服务锁，没法中途让；服务上限 256 时
 *     实测最长 ~0.7 s（docs/test_cases_scale_baseline.md「SRP 代播满载压测」）。
 *
 * 读数：GET /api/v1/diag/srp 的 cpu_guard_yields（让过几次）与 idle_starve_max_ms —— 后者是 idle hook 自己
 * 量的**开机以来 IDLE 最长一次没跑到**（任何原因，不只是 OT），出厂固件就能读。看它离 5000（任务看门狗）多远；
 * 它不会真的到 5000：一到看门狗就 panic、计数随重启清零 —— 守卫失效的表现是重启（uptime 归零），不是这个数变大。 */
#include "sdkconfig.h"

#if CONFIG_OPENTHREAD_ENABLED
#include <stdint.h>
#include "esp_freertos_hooks.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "openthread/platform/crypto.h"
#include "thread_br.h"

#define GUARD_STARVE_MS 1000    /* 任务看门狗 5 s，留足余量 */
#define GUARD_YIELD_MS  20      /* 一个节拍不够：同优先级以上的就绪任务会先把它用掉，IDLE 未必轮得到 */

static const char *TAG = "ot_cpu_guard";

/* 32 位微秒：对齐的 32 位读写在 RISC-V 上是原子的（64 位不是），71 分钟回绕靠无符号减法照样对。
   两个量都只由 IDLE 任务写（idle hook），别处只读。 */
static volatile uint32_t s_idle_last_us;
static volatile uint32_t s_idle_gap_max_us;   /* 开机以来 IDLE 两次运行之间的最长间隔 = 最长一次被饿 */
static volatile uint32_t s_yields;
static bool s_hooked;

static bool idle_hook(void)
{
    uint32_t now = (uint32_t)esp_timer_get_time();
    uint32_t gap = now - s_idle_last_us;      /* 空闲时 WFI 每个节拍醒一次，间隔 ≈ 1 ms */
    if (gap > s_idle_gap_max_us) s_idle_gap_max_us = gap;
    s_idle_last_us = now;
    return true;                /* 照常 WFI */
}

void thread_br_cpu_guard_init(void)
{
    if (s_hooked) return;
    s_idle_last_us = (uint32_t)esp_timer_get_time();
    if (esp_register_freertos_idle_hook_for_cpu(idle_hook, 0) == ESP_OK) {
        s_hooked = true;
    } else {
        /* 不致命：只是没有过载保护（idle hook 槽满了才会这样，IDF 有 8 个） */
        ESP_LOGE(TAG, "idle hook 注册失败 —— OT 任务过载时不会主动让出 CPU");
    }
}

void thread_br_cpu_guard_stats(uint32_t *yields, uint32_t *starve_max_ms)
{
    if (yields) *yields = s_yields;
    if (starve_max_ms) *starve_max_ms = s_idle_gap_max_us / 1000;
}

/* mdns 组件的打过补丁的任务循环用它判断"让出去之后 IDLE 真的跑过没有"（tools/patch_mdns.py SCALE ⑧，那边有弱定义兜底）。
   hook 没挂上时返回 0 = 不知道，对方退化成只睡一拍 */
uint32_t bitengine_idle_last_us(void)
{
    return s_hooked ? s_idle_last_us : 0;
}

#if CONFIG_BITENGINE_DIAG_SRP_INJECT
void thread_br_diag_account_ecdsa(int64_t t0);   /* srp_mdns_wrap.c：诊断构建里给验签计时 */
#endif

otError __real_otPlatCryptoEcdsaVerify(const otPlatCryptoEcdsaPublicKey *aPublicKey, const otPlatCryptoSha256Hash *aHash,
                                       const otPlatCryptoEcdsaSignature *aSignature);
otError __wrap_otPlatCryptoEcdsaVerify(const otPlatCryptoEcdsaPublicKey *aPublicKey, const otPlatCryptoSha256Hash *aHash,
                                       const otPlatCryptoEcdsaSignature *aSignature)
{
#if CONFIG_BITENGINE_DIAG_SRP_INJECT
    int64_t t0 = esp_timer_get_time();
#endif
    otError r = __real_otPlatCryptoEcdsaVerify(aPublicKey, aHash, aSignature);
#if CONFIG_BITENGINE_DIAG_SRP_INJECT
    thread_br_diag_account_ecdsa(t0);
#endif
    if (s_hooked) {
        uint32_t starve = (uint32_t)esp_timer_get_time() - s_idle_last_us;
        if (starve > GUARD_STARVE_MS * 1000u) {
            s_yields++;
            vTaskDelay(pdMS_TO_TICKS(GUARD_YIELD_MS));
        }
    }
    return r;
}
#else /* !CONFIG_OPENTHREAD_ENABLED：没有 OT 任务也就没有要护的；REST 照样能读（全 0） */
#include <stddef.h>
#include <stdint.h>
void thread_br_cpu_guard_init(void) {}
void thread_br_cpu_guard_stats(uint32_t *yields, uint32_t *starve_max_ms)
{
    if (yields) *yields = 0;
    if (starve_max_ms) *starve_max_ms = 0;
}
uint32_t bitengine_idle_last_us(void) { return 0; }
#endif /* CONFIG_OPENTHREAD_ENABLED */
