#pragma once
/* blew_core —— ble_window 里不依赖 ESP-IDF 的判定（docs/15 设计 3.1、3.2）。
 * 产品源文件原样编进 test_host/blew_core_test.c 测真值表；不许 include 任何 esp_ / freertos / nimble 头。
 * 常量与 ble_window.h 的枚举一一对应，ble_window.c 里有编译期断言钉着。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 策略 C 的一次性标志：RTC_NOINIT 里两个字，magic 和它的取反（设计 3.2 第 1 步）。上电时 RTC RAM 是随机值，
   两个字恰好互为取反且等于 magic 的概率可以忽略。 */
#define BLEW_PAIR_MAGIC 0x4D555345u   /* "MUSE" */

#define BLEW_CORE_OWNER_MUSE_PAIR 1
#define BLEW_CORE_OWNER_MATTER    2
#define BLEW_CORE_ROLE_PERIPHERAL 0
#define BLEW_CORE_ROLE_CENTRAL    1

#define BLEW_MUSE_MAX_MS   600000u    /* Muse 配对窗口硬上限 600 s（3.1） */
#define BLEW_MATTER_MAX_MS 120000u    /* Matter 配网 120 s（P3） */

/* 这次开机进不进配对剖面：magic 与取反都对、复位原因是软件复位（esp_restart）、而且不在安全模式。
   断电（POWERON）、崩溃（PANIC / 看门狗）都回正常启动，不会卡在配对剖面里循环（TC-MU04）。 */
bool blew_boot_decide(uint32_t magic, uint32_t magic_inv, bool sw_reset, bool safe_mode);

/* 写进 RTC 的两个字（request_isolated 用；测试也用它造合法输入）。 */
void blew_boot_arm(uint32_t *magic, uint32_t *magic_inv);

typedef enum {
    BLEW_REQ_OK = 0,
    BLEW_REQ_BAD_ARG,         /* owner / role 不认识，max_ms 为 0 或超过该 owner 的上限 */
    BLEW_REQ_BUSY,            /* 已经有一个窗口开着（同时只允许一个） */
    BLEW_REQ_NOT_SUPPORTED,   /* 这个组合眼下没有：Matter（P3）、中心角色、或策略 C 下不在配对剖面里开窗口 */
} blew_req_verdict_t;

/* 开窗口的请求能不能接。眼下只有策略 C：窗口只能在配对剖面里开，只给 Muse 配对、只当外设。 */
blew_req_verdict_t blew_req_check(int owner, int role, uint32_t max_ms, bool window_open, bool pair_boot);

/* 内存闸门（3.4）：内部空闲与 DMA 空闲都够才开 BT。 */
bool blew_mem_gate(uint32_t internal_free, uint32_t dma_free, uint32_t need_internal, uint32_t need_dma);

/* ---------------- BLE 窗口之后的 Thread 自检（设计 3.3；策略 C 在重启回正常开机之后做） ---------------- */

typedef enum {
    BLEW_SC_NONE = 0,              /* 没做过（进 flash 的编号，只许追加） */
    BLEW_SC_PASS = 1,
    BLEW_SC_DEGRADED_ACK = 2,      /* ACK 比例掉到门槛以下（#1851：BLE 之后 Thread 收不到 ACK） */
    BLEW_SC_DEGRADED_CHILDREN = 3, /* 子节点 240 s 内没回到 90%（只在 judge_children 时：策略 A / B） */
    BLEW_SC_DEGRADED_BOTH = 4,
    BLEW_SC_INCONCLUSIVE = 5,      /* 没流量也没子节点：判不了 */
    BLEW_SC_NO_THREAD = 6,         /* Thread 没起来 / 用户关了 Thread：不判 */
    BLEW_SC_INTERRUPTED = 7,       /* 自检还没做完就又请求了配对（重启把它打断了） */
    BLEW_SC_MAX
} blew_sc_result_t;

#define BLEW_SC_MIN_FRAMES 20      /* 要求 ACK 的帧少于这个数，比例没意义 */

typedef struct {
    uint32_t base_req, base_acked;     /* 开窗前约 10 分钟的增量 */
    uint8_t  children_before;          /* 开窗前在线的子节点 */
    uint32_t after_req, after_acked;   /* 恢复后第 60–120 s 的增量 */
    uint8_t  children_after;           /* 恢复后观察期内在线子节点的最大值（策略 C 拿到第二次 ACK 读数就收，约 120 s） */
    bool     judge_children;           /* 子节点参不参与判定。策略 C 下 false：配对期间 Thread 整段不在（3.5–11 分钟），超过
                                          child timeout，子设备会改挂别的路由器、回来也不换回 —— 那是 C 自己的停机，不是 BLE 的
                                          残留（#1851 的症状是收不到 ACK）。只记录不判（2026-10-22 审计） */
} blew_sc_input_t;

/* 判定（设计 3.3）：ack_after ≥ max(0.5, 0.8 × ack_before)（基线帧数不够时门槛 0.5；观察窗帧数不够就不判 ACK）；
   judge_children 时子节点恢复到 children_before 的 90% 以上（之前没有子节点就不判）。都判不了 → INCONCLUSIVE。 */
blew_sc_result_t blew_sc_verdict(const blew_sc_input_t *in);
const char *blew_sc_str(blew_sc_result_t r);
/* 比例的千分数；分母为 0 返回 0 */
uint16_t blew_permille(uint32_t num, uint32_t den);

/* muse/blew_res 的 blob（不含 hub_store 的格式版本字节）：设计 4.4（≤ 64 B） */
#define BLEW_RES_BLOB_LEN 14
typedef struct {
    uint8_t  last;                 /* blew_sc_result_t */
    uint8_t  consecutive_degraded; /* 连续几次退化（PASS 清零，INCONCLUSIVE / NO_THREAD / INTERRUPTED 不动） */
    uint16_t total;                /* 一共做了几次 */
    uint16_t before_pm, after_pm;  /* 上一次的 ACK 比例（千分数） */
    uint8_t  children_before, children_after;
    uint32_t epoch;                /* 墙钟（没对时为 0） */
} blew_res_t;
void blew_res_encode(const blew_res_t *r, uint8_t out[BLEW_RES_BLOB_LEN]);
bool blew_res_decode(const uint8_t *in, size_t len, blew_res_t *r);
/* 把这一次的结果并进记录（prev 可为 NULL = 没有旧记录）。判了的（PASS / DEGRADED_*）才更新比例与子节点数；
   判不了的（INCONCLUSIVE / NO_THREAD / INTERRUPTED）只动 last、total，数值保留上一次判过的，别显示成"950‰ → 0‰"。 */
void blew_res_update(blew_res_t *out, const blew_res_t *prev, blew_sc_result_t r, const blew_sc_input_t *in, uint32_t epoch);
