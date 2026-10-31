/* ble_window 的 Thread 自检（docs/15_muse_direct_design_v1.md 3.3；TC-MU05 / TC-BW05）。
 *
 * 策略 C 的前提是"BLE 和 Thread 从不同时出现在一次开机里，CPU 复位之后共存登记就干净了"（#1851）。这里验证它：
 *  - 正常开机请求进配对剖面时（ble_window_request_isolated，重启之前）记基线：过去约 10 分钟的 ACK 增量、在线子节点数，
 *    写进**自己的** RTC_NOINIT 记录（与配对标志、wd_forensics 是不同符号）；配对剖面那次开机不碰它；
 *  - 配完重启回正常开机，main.c 调 ble_window_selfcheck_start()：取走基线（一次性），起任务等 Thread 起来，
 *    第 60–120 s 看 ACK 比例；子节点只记录（策略 C 不判，见 blew_core.h 的 judge_children），判定见 blew_core.c；
 *  - H1（OT 的 otMacCounters 注释）：mTxAckRequested = 要求 ACK 的单播帧**发送请求**数（重传不另计），mTxAcked = 最终（含重传）
 *    收到 ACK 的帧数。所以只有整次传输连重传都收不到 ACK，比例才会掉 —— 正是 #1851 的症状；没有单播流量时两者都不动（判 inconclusive）；
 *  - 结果并进 hubdb muse/blew_res（总次数、连续退化次数），/api/v1/muse/status 的 blew.selfcheck 显示。
 * 策略 C 下判退化**不自动重启**：再做一次 CPU 复位解决不了"复位后共存状态还在"（那正是 C 的前提失败），只会循环；
 * 记下来、日志打 E，留给 G0-4 (e) 与 WebUI 提示（连续两次，设计 3.3）。断电回来 RTC 是随机值、magic 对不上，不做。
 */
#include <time.h>

#include "sdkconfig.h"          /* 先于 thread_br.h：它的 ACK 接口在 CONFIG_BITENGINE_ENABLE_MUSE_LINK 里，头文件自己不包含 sdkconfig */
#include "ble_window.h"
#include "blew_core.h"
#include "blew_priv.h"

#include "config_mgr.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hub_store.h"
#include "thread_br.h"

static const char *TAG = "blew_sc";

#define SC_MAGIC          0x53434B31u    /* "SCK1" */
#define SC_TASK_STACK     3072           /* 内部栈：只有 hub_store 的同步调用和日志 */
#define SC_TASK_PRIO      3
#define SC_WAIT_THREAD_S  300            /* Thread 5 分钟还没起来：不判 */
#define SC_OBS_START_S    60             /* 设计 3.3：恢复后第 60 s 起 */
#define SC_OBS_END_S      120            /* 观察 60 s */
#define SC_CHILD_LIMIT_S  240            /* 取不到第二次 ACK 读数时的总上限（策略 A / B 判子节点时也按它：child timeout 默认 240 s） */
#define SC_POLL_S         5

typedef struct {
    uint32_t magic, inv;
    uint32_t base_req, base_acked;
    uint8_t  children_before;
    uint8_t  have_window;                /* 基线时 Thread 在跑、拿到了 10 分钟窗口 */
    uint8_t  bt_used;                    /* 配对剖面里 BLE 窗口真的开起来过（blew_do_open 成功时置）；没开过就不做自检 */
} sc_rtc_t;

static RTC_NOINIT_ATTR sc_rtc_t s_sc_rtc;

/* 对外快照（get_status 读，单字段写入，不加锁） */
static volatile blew_sc_result_t s_sc_last = BLEW_SC_NONE;
static volatile bool             s_sc_pending;
static blew_res_t                s_sc_rec;          /* 最近一次存下的记录（开机读一次 hubdb，自检完更新） */
static sc_rtc_t                  s_task_base;       /* 这次开机正在做的那一轮的基线（自检中又请求配对时沿用它） */

static void store(blew_sc_result_t r, const blew_sc_input_t *in);

static uint32_t wall_epoch(void)
{
    const time_t now = time(NULL);
    return now > 1700000000 ? (uint32_t)now : 0;
}

void blew_selfcheck_capture_baseline(void)
{
    if (s_sc_pending) {
        /* 刚从配对剖面回来、自检还没做完就又请求配对：这几分钟的计数是"刚开机"的瞬态（若 #1851 残留真在，它本身就是退化的），
           拿它当基线会把退化掩盖掉。沿用正在做的那一轮的基线，并把被打断的这一轮记一笔（2026-10-22 审计） */
        s_sc_rtc = s_task_base;
        s_sc_rtc.bt_used = 0;
        s_sc_rtc.inv = ~SC_MAGIC;
        s_sc_rtc.magic = SC_MAGIC;
        store(BLEW_SC_INTERRUPTED, NULL);
        ESP_LOGW(TAG, "自检还没做完又请求配对：沿用上一轮的基线，被打断的那一轮记 interrupted");
        return;
    }
    uint32_t r = 0, a = 0, span = 0;
    uint8_t total = 0, online = 0;
    const bool have = thread_br_get_ack_window(&r, &a, &span);
    thread_br_get_child_stats(&total, &online);
    s_sc_rtc.base_req = have ? r : 0;
    s_sc_rtc.base_acked = have ? a : 0;
    s_sc_rtc.children_before = online;
    s_sc_rtc.have_window = have ? 1 : 0;
    s_sc_rtc.bt_used = 0;
    s_sc_rtc.inv = ~SC_MAGIC;
    s_sc_rtc.magic = SC_MAGIC;               /* 最后写 magic：前面写到一半断了也不会被当成合法记录 */
    ESP_LOGI(TAG, "自检基线：ACK %u/%u（过去 %u s）、在线子节点 %u%s", (unsigned)a, (unsigned)r, (unsigned)span,
             (unsigned)online, have ? "" : "（Thread 没在跑，没有 ACK 基线）");
}

void blew_selfcheck_note_bt_used(void)
{
    if (s_sc_rtc.magic == SC_MAGIC && s_sc_rtc.inv == (uint32_t)~SC_MAGIC) s_sc_rtc.bt_used = 1;
}

static void store(blew_sc_result_t r, const blew_sc_input_t *in)
{
    blew_res_t prev = {0}, next;
    uint8_t blob[BLEW_RES_BLOB_LEN + 8], ver = 0;
    size_t len = sizeof(blob);
    const esp_err_t gerr = hub_store_get(HUB_STORE_NS_MUSE, "blew_res", 1, &ver, blob, &len, 2000);
    if (gerr != ESP_OK && gerr != ESP_ERR_NOT_FOUND && gerr != ESP_ERR_INVALID_VERSION) {
        /* 读不到（超时 / hub_store 没起）≠ 没有：覆盖成 total=1 会把连续退化次数悄悄清掉。这次不写 */
        ESP_LOGE(TAG, "读不到上次的自检记录（%s）—— 这次结果（%s）不落盘", esp_err_to_name(gerr), blew_sc_str(r));
        return;
    }
    const bool have_prev = gerr == ESP_OK && blew_res_decode(blob, len, &prev);
    blew_res_update(&next, have_prev ? &prev : NULL, r, in, wall_epoch());
    blew_res_encode(&next, blob);
    if (hub_store_put(HUB_STORE_NS_MUSE, "blew_res", 1, blob, BLEW_RES_BLOB_LEN) != ESP_OK) {
        ESP_LOGE(TAG, "自检结果没存下来（%s）", blew_sc_str(r));
    }
    s_sc_rec = next;
}

static void sc_task(void *arg)
{
    blew_sc_input_t in = {
        .base_req = s_task_base.base_req, .base_acked = s_task_base.base_acked,
        .children_before = s_task_base.children_before,
        .judge_children = false,         /* 策略 C：配对期间 Thread 整段不在，子节点改挂是 C 自己的停机造成的 —— 只记录不判 */
    };
    (void)arg;
    blew_sc_result_t r = BLEW_SC_NO_THREAD;

    const thread_config_t *tc = config_mgr_get_thread();
    for (int i = 0; tc && tc->enabled && i < SC_WAIT_THREAD_S && !thread_br_is_running(); i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));   /* 用户关了 Thread 就不空等 */
    }
    if (!tc || !tc->enabled || !thread_br_is_running()) {
        ESP_LOGW(TAG, "Thread 没起来 / 用户关了 Thread —— 不做自检");
        goto done;
    }

    {
        const int64_t t0 = esp_timer_get_time();
        int64_t t_got0 = 0;
        uint32_t r0 = 0, a0 = 0, r1 = 0, a1 = 0;
        bool got0 = false, got1 = false;
        uint8_t max_online = 0;
        for (;;) {
            const int64_t now = esp_timer_get_time();
            const uint32_t el = (uint32_t)((now - t0) / 1000000);
            uint8_t total = 0, online = 0;
            thread_br_get_child_stats(&total, &online);
            if (online > max_online) max_online = online;
            if (!got0 && el >= SC_OBS_START_S && (got0 = thread_br_get_ack_now(&r0, &a0))) t_got0 = now;
            /* 观察窗相对第一次取样算满 60 s：第一次取晚了（中途 Thread 暂停过），窗口也不缩水 */
            if (got0 && !got1 && now - t_got0 >= (int64_t)(SC_OBS_END_S - SC_OBS_START_S) * 1000000) {
                got1 = thread_br_get_ack_now(&r1, &a1);
            }
            if (got1 || el >= SC_CHILD_LIMIT_S) break;   /* 子节点只记录（策略 C 不判），拿到 ACK 就收 */
            vTaskDelay(pdMS_TO_TICKS(SC_POLL_S * 1000));
        }
        in.after_req = got1 ? r1 - r0 : 0;        /* 无符号减法：计数回绕也对 */
        in.after_acked = got1 ? a1 - a0 : 0;
        in.children_after = max_online;
        r = blew_sc_verdict(&in);
    }

done:
    store(r, &in);
    s_sc_last = r;
    s_sc_pending = false;
    if (r == BLEW_SC_DEGRADED_ACK || r == BLEW_SC_DEGRADED_CHILDREN || r == BLEW_SC_DEGRADED_BOTH) {
        ESP_LOGE(TAG, "BLE 窗口之后 Thread 退化（%s）：ACK %u‰ → %u‰，子节点 %u → %u（连续 %u 次）—— 策略 C 的前提可能不成立"
                      "（#1851，G0-4 (e)），不自动重启（再复位一次也救不回来）",
                 blew_sc_str(r), (unsigned)s_sc_rec.before_pm, (unsigned)s_sc_rec.after_pm,
                 (unsigned)in.children_before, (unsigned)in.children_after, (unsigned)s_sc_rec.consecutive_degraded);
    } else {
        /* 打这一轮的帧数，不打 s_sc_rec 里的比例：判不了时那是上一轮判过的旧数字 */
        ESP_LOGI(TAG, "BLE 窗口之后的 Thread 自检：%s（这一轮：基线 %u/%u 帧，恢复后 %u/%u 帧，在线子节点 %u → %u）",
                 blew_sc_str(r), (unsigned)in.base_acked, (unsigned)in.base_req, (unsigned)in.after_acked,
                 (unsigned)in.after_req, (unsigned)in.children_before, (unsigned)in.children_after);
    }
    ESP_LOGI(TAG, "自检任务栈最紧时还剩 %u 字节", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

esp_err_t ble_window_selfcheck_start(void)
{
    if (ble_window_pair_boot_active()) return ESP_ERR_INVALID_STATE;   /* 配对剖面里不碰基线（下一次正常开机才用） */
    const bool armed = s_sc_rtc.magic == SC_MAGIC && s_sc_rtc.inv == (uint32_t)~SC_MAGIC;
    s_task_base = s_sc_rtc;
    s_sc_rtc.magic = 0;                                                 /* 一次性 */
    s_sc_rtc.inv = 0;
    /* 先把上次存的结果读出来给状态页（自检进行中那几分钟也显示历史次数） */
    uint8_t blob[BLEW_RES_BLOB_LEN + 8], ver = 0;
    size_t len = sizeof(blob);
    blew_res_t rec;
    if (hub_store_get(HUB_STORE_NS_MUSE, "blew_res", 1, &ver, blob, &len, 1000) == ESP_OK
        && blew_res_decode(blob, len, &rec)) {
        s_sc_rec = rec;
        s_sc_last = (blew_sc_result_t)rec.last;
    }
    if (!armed) return ESP_ERR_NOT_FOUND;                               /* 这次开机没经过配对 */
    if (!s_task_base.bt_used) {
        ESP_LOGI(TAG, "配对剖面里 BLE 窗口没开起来过（没 token / 没网 / 预检失败 / 取消）—— 不做自检");
        return ESP_ERR_NOT_FOUND;
    }
    s_sc_pending = true;
    if (xTaskCreate(sc_task, "blew_sc", SC_TASK_STACK, NULL, SC_TASK_PRIO, NULL) != pdPASS) {
        s_sc_pending = false;
        ESP_LOGE(TAG, "自检任务起不来");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "刚从配对剖面回来：%u s 后开始 Thread 自检", SC_OBS_START_S);
    return ESP_OK;
}

void blew_selfcheck_status(const char **state, blew_res_t *rec)
{
    if (state) *state = s_sc_pending ? "pending" : blew_sc_str(s_sc_last);
    if (rec) *rec = s_sc_rec;
}
