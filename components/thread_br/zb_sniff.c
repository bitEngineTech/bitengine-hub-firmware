/* components/thread_br/zb_sniff.c — 诊断构建专用（CONFIG_BITENGINE_DIAG_ZB_SNIFF，出厂 n，出厂编出来是空的）
 *
 * 用 C5 的 802.15.4 射频临时去听 Zigbee 信道，把空中帧原样录下来，主机侧 tools/zb_sniff.py 解码 + 用网络密钥解密。
 * 为什么有它（2026-10-07）：Sengled E11-G13 掉线后通电，协调器每 2 s 在 MAC 层收一个单播、回一个广播，
 * 却从不上报入网事件 —— 灯发的是什么、MG24 回的是什么，EZSP 计数器分不出来，只能看空口。
 *
 * 做法与 OpenThread CLI 抓包一样：停 Thread（thread_br_pause，SoftAP 配网同一条路）→ 开混杂 →
 * 平台层 otPlatRadioReceive(inst, ch) 只把射频调到目标信道 → SubMac 的 pcap 回调在过滤之前拿到每一帧 →
 * 到时关混杂、恢复 Thread。录制期间 Thread 中断（设备会在恢复后重新挂回来）。
 *
 * H1 —— OT 源码逐行核过（IDF v6.0.2 自带的 openthread）：
 *  1. ⚠️ 绝不用 otLinkSetChannel()：MLE 停着时它改 PAN 信道的同时清掉 Active / Pending 数据集
 *     （api/link_api.cpp otLinkSetChannel → ActiveDatasetManager().Clear()），等于丢掉 Thread 网络身份。
 *     平台层 otPlatRadioReceive 只动射频，不碰 PAN 信道与数据集。VERIFY 全仓钉着这一条。
 *  2. otLinkSetPromiscuous 要求 Thread 接口已关（link_api.cpp：ThreadNetif::IsUp() 为假才行），所以必须先 pause。
 *     开混杂会让 Mac 空闲时也收（mac.cpp SetPromiscuous → SetRxOnWhenIdle → UpdateIdleMode → Receive(PAN 信道)），
 *     所以 otPlatRadioReceive 必须在它**之后**调，否则又被拉回 PAN 信道。
 *  3. pcap 回调在 SubMac::HandleReceiveDone 里、地址过滤之前调（mac/sub_mac.cpp），只给收成功的帧；
 *     跑在 OT 主循环任务里、持 OT 锁 —— 回调里只拷贝。发送的帧也回调（is_tx），Thread 停着时应当为 0。
 *  4. otRadioFrame.mLength 是 PSDU 长度、含 2 字节 FCS（ESP 端口把 PHR 原样给 mLength），解码时去掉末 2 字节。
 *  5. 关混杂（SetPromiscuous(false)）后 RxOnWhenIdle 回到 false，Mac 让射频睡；resume 时 Thread 按数据集信道起来。
 *
 * 本板 C5 的 2.4G 收发与 5G WiFi 分时共用射频（本仓库 thread_radio_mgr.c 的共存说明），pause 会告诉共存层
 * "802.15.4 不活动"。录制期间临时再告诉它"活动"，否则 WiFi 占着时整段收不到；恢复前再关回去，让 resume 对称。
 */
#include "sdkconfig.h"
#include "thread_br.h"
#include "thread_br_diag.h"

#if CONFIG_BITENGINE_DIAG_ZB_SNIFF
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_coex_i154.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "openthread/link.h"
#include "openthread/platform/radio.h"

static const char *TAG = "zb_sniff";

extern bool thread_br_radio_is_paused(void);   /* thread_radio_mgr.c */

static struct {
    volatile int        state;        /* thread_br_zb_sniff_state_t */
    uint8_t             channel;
    uint16_t            seconds;
    int64_t             t0_us;
    zb_sniff_frame_t   *buf;          /* PSRAM，首次用时分配，常驻 */
    volatile uint16_t   count;        /* 只有 OT 任务写；读者只在 state=DONE 之后读 */
    volatile uint32_t   dropped;
    volatile uint32_t   tx_seen;
    char                err[40];
} s_sn;

static portMUX_TYPE s_sn_mux = portMUX_INITIALIZER_UNLOCKED;

static void sniff_pcap_cb(const otRadioFrame *f, bool is_tx, void *ctx)
{
    (void)ctx;
    if (is_tx) { s_sn.tx_seen++; return; }
    if (!f || !s_sn.buf) return;
    if (s_sn.count >= ZB_SNIFF_MAX) { s_sn.dropped++; return; }
    zb_sniff_frame_t *o = &s_sn.buf[s_sn.count];
    o->t_ms    = (uint32_t)((esp_timer_get_time() - s_sn.t0_us) / 1000);
    o->rssi    = f->mInfo.mRxInfo.mRssi;
    o->lqi     = f->mInfo.mRxInfo.mLqi;
    o->channel = f->mChannel;
    o->len     = f->mLength > sizeof(o->psdu) ? sizeof(o->psdu) : (uint8_t)f->mLength;
    memcpy(o->psdu, f->mPsdu, o->len);
    s_sn.count++;
}

static void set_err(const char *e)
{
    strlcpy(s_sn.err, e, sizeof(s_sn.err));
    ESP_LOGE(TAG, "%s", e);
}

static void sniff_task(void *arg)
{
    (void)arg;
    bool we_paused = false, promisc = false;
    otInstance *inst = NULL;

    if (thread_br_is_running()) {
        if (thread_br_pause() != ESP_OK || !thread_br_radio_is_paused()) {
            set_err("pause_failed");
            goto out;
        }
        we_paused = true;
    }
    esp_coex_ieee802154_status_enable();      /* 录制期间让共存层给 802.15.4 留时间，见头注 */

    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(5000))) { set_err("ot_lock_timeout"); goto restore; }
    inst = esp_openthread_get_instance();
    otError oe = inst ? otLinkSetPromiscuous(inst, true) : OT_ERROR_INVALID_STATE;
    if (oe == OT_ERROR_NONE) {
        promisc = true;
        s_sn.t0_us = esp_timer_get_time();
        otLinkSetPcapCallback(inst, sniff_pcap_cb, NULL);
        oe = otPlatRadioReceive(inst, s_sn.channel);           /* H1 第 2 条：必须在开混杂之后 */
    }
    esp_openthread_lock_release();
    if (oe != OT_ERROR_NONE) { set_err(promisc ? "radio_receive_failed" : "promiscuous_failed"); goto restore; }

    ESP_LOGW(TAG, "开始录制：信道 %u，%u s（Thread 暂停中）", s_sn.channel, s_sn.seconds);
    vTaskDelay(pdMS_TO_TICKS((uint32_t)s_sn.seconds * 1000));

restore:
    /* 关混杂与回调必须做：拿不到锁也要一直等，否则射频留在别的信道、Thread 恢复不了 */
    if (promisc) {
        while (!esp_openthread_lock_acquire(pdMS_TO_TICKS(5000))) {
            ESP_LOGE(TAG, "关混杂取 OT 锁超时，重试");
        }
        otLinkSetPcapCallback(inst, NULL, NULL);
        otLinkSetPromiscuous(inst, false);                     /* H1 第 5 条：射频回到睡眠 */
        esp_openthread_lock_release();
    }
    esp_coex_ieee802154_status_disable();     /* 与 pause 的状态对称，resume 会再打开 */
    if (we_paused) thread_br_resume();
out:
    ESP_LOGW(TAG, "录制结束：%u 帧（丢 %"PRIu32"，发送回调 %"PRIu32"）%s%s",
             s_sn.count, s_sn.dropped, s_sn.tx_seen, s_sn.err[0] ? "，错误 " : "", s_sn.err);
    s_sn.state = s_sn.err[0] ? THREAD_BR_ZB_SNIFF_ERROR : THREAD_BR_ZB_SNIFF_DONE;
    vTaskDelete(NULL);
}

esp_err_t thread_br_zb_sniff_start(uint8_t channel, uint16_t seconds)
{
    if (channel < 11 || channel > 26 || seconds < 1 || seconds > ZB_SNIFF_MAX_SECONDS) return ESP_ERR_INVALID_ARG;
    /* OT 实例只在 Thread 起来过之后存在：正在跑，或被 SoftAP 暂停着 */
    if (!thread_br_is_running() && !thread_br_radio_is_paused()) return ESP_ERR_INVALID_STATE;

    portENTER_CRITICAL(&s_sn_mux);
    bool busy = (s_sn.state == THREAD_BR_ZB_SNIFF_RUNNING);
    if (!busy) s_sn.state = THREAD_BR_ZB_SNIFF_RUNNING;
    portEXIT_CRITICAL(&s_sn_mux);
    if (busy) return ESP_ERR_NOT_FINISHED;

    if (!s_sn.buf) {
        s_sn.buf = heap_caps_malloc(sizeof(zb_sniff_frame_t) * ZB_SNIFF_MAX, MALLOC_CAP_SPIRAM);
        if (!s_sn.buf) { s_sn.state = THREAD_BR_ZB_SNIFF_IDLE; return ESP_ERR_NO_MEM; }
    }
    s_sn.channel = channel; s_sn.seconds = seconds;
    s_sn.count = 0; s_sn.dropped = 0; s_sn.tx_seen = 0; s_sn.err[0] = '\0';

    /* 内部栈：pause / resume 里 OT 会写 settings（NVS），栈不能在 PSRAM（psram_task.h 规则 1） */
    if (xTaskCreate(sniff_task, "zb_sniff", 4096, NULL, 5, NULL) != pdPASS) {
        s_sn.state = THREAD_BR_ZB_SNIFF_IDLE;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void thread_br_zb_sniff_status(thread_br_zb_sniff_status_t *out)
{
    if (!out) return;
    out->state   = (thread_br_zb_sniff_state_t)s_sn.state;
    out->channel = s_sn.channel;
    out->seconds = s_sn.seconds;
    out->dropped = s_sn.dropped;
    out->tx_seen = s_sn.tx_seen;
    strlcpy(out->err, s_sn.err, sizeof(out->err));
    /* 录制中只报计数，不给帧（写者还在写） */
    out->count   = s_sn.count;
    out->frames  = (s_sn.state == THREAD_BR_ZB_SNIFF_DONE || s_sn.state == THREAD_BR_ZB_SNIFF_ERROR) ? s_sn.buf : NULL;
}
#endif /* CONFIG_BITENGINE_DIAG_ZB_SNIFF */
