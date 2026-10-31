/**
 * Beacon Scan — active Zigbee network discovery via Beacon Request frames
 *
 * Radio ownership (H1):
 *   - OpenThread builds (the shipping configuration): the OT port owns
 *     esp_ieee802154_transmit_done / receive_done (strong symbols). A raw
 *     esp_ieee802154_transmit() makes the OT port's ISR callbacks fire for a
 *     frame they did not originate, which trips ETS_ASSERT in ISR context →
 *     device hang. So beacon scan goes through OpenThread's active scan
 *     (thread_br_active_scan) instead.
 *   - otLinkActiveScan alone is NOT enough: OT's result parser drops every
 *     beacon whose source is not a 64-bit extended address
 *     (scan_result.cpp:50), and Zigbee coordinators beacon with the 16-bit
 *     short address 0x0000. thread_br_active_scan() therefore also hooks
 *     otLinkSetPcapCallback and hands back the raw beacons (raw[]). We merge
 *     both views here. Verified on the bench 2026-08-12: OT reported 0 while
 *     PCAP saw 2 (docs/test_cases_channel_coex.md, TC-C11).
 *   - Non-OT builds: raw driver, callbacks defined here (unchanged).
 *
 * Until 2026-09-13 the OT branch was a stub returning ESP_ERR_NOT_SUPPORTED,
 * so /api/v1/spectrum/beacon answered 501 on every shipped unit (T4.8).
 *
 * H1 API contracts (non-OT path):
 *   - esp_ieee802154_receive_done(frame, info): ISR callback, must call
 *     receive_handle_done to release the RX buffer
 *   - esp_ieee802154_transmit(frame, cca): frame[0] = PHR (total bytes incl FCS)
 *   - esp_ieee802154_receive(): enter receive mode
 */

#include "spectrum.h"
#include "thread_br.h"
#include "config_mgr.h"

#include <string.h>
#include "esp_log.h"
#include "esp_ieee802154.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "beacon_scan";

/* ---- 最近一次结果（两种构型共用）----
   只在持有 spectrum 锁时读写。锁不可重入（xSemaphoreCreateMutex），所以
   spectrum_beacon_scan_cached() 先在锁内拷贝、放锁，再决定要不要扫。 */
static beacon_scan_result_t s_last;
static bool                 s_last_valid = false;

static uint32_t now_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

esp_err_t spectrum_beacon_scan(beacon_scan_result_t *result)
{
    return spectrum_beacon_scan_ex(0, result);
}

esp_err_t spectrum_beacon_scan_cached(beacon_scan_result_t *result, uint32_t max_age_s)
{
    if (!result) return ESP_ERR_INVALID_ARG;
    if (!spectrum_is_initialized()) return ESP_ERR_INVALID_STATE;
    if (!spectrum_scan_lock_take(30000)) return ESP_ERR_TIMEOUT;
    bool fresh = false;
    if (s_last_valid && (now_s() - s_last.timestamp) <= max_age_s) {
        *result = s_last;
        fresh = true;
    }
    spectrum_scan_lock_give();
    if (fresh) return ESP_OK;
    return spectrum_beacon_scan_ex(0, result);
}

#if !CONFIG_OPENTHREAD_ENABLED

static beacon_scan_result_t *s_current_result = NULL;

// ---- ISR callback — only when the OT port is NOT managing the radio ----
void esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *info)
{
    if (!s_current_result || !frame || !info) {
        if (frame) esp_ieee802154_receive_handle_done(frame);
        return;
    }

    if (frame[0] == 0) {
        esp_ieee802154_receive_handle_done(frame);
        return;
    }
    uint8_t frame_type = frame[1] & 0x07;
    if (frame_type != 0x00) {
        esp_ieee802154_receive_handle_done(frame);
        return;
    }

    if (s_current_result->count >= MAX_ZIGBEE_NEIGHBORS) {
        esp_ieee802154_receive_handle_done(frame);
        return;
    }

    uint8_t idx = s_current_result->count;
    s_current_result->networks[idx].channel = esp_ieee802154_get_channel();
    s_current_result->networks[idx].rssi = info->rssi;
    s_current_result->networks[idx].lqi = info->lqi;
    s_current_result->networks[idx].self = false;   /* 非 OT 构型不判自家 */

    if (frame[0] >= 6) {
        s_current_result->networks[idx].pan_id = frame[4] | (frame[5] << 8);
    }

    s_current_result->count++;

    // H1: must release RX buffer (API contract)
    esp_ieee802154_receive_handle_done(frame);

    esp_ieee802154_receive();
}

// ---- Beacon Scan (raw radio) ----
esp_err_t spectrum_beacon_scan_ex(uint16_t dwell_ms, beacon_scan_result_t *result)
{
    (void)dwell_ms;   /* 裸驱动路径固定 100ms/信道，与原行为一致 */
    if (!result) return ESP_ERR_INVALID_ARG;
    if (!spectrum_is_initialized()) return ESP_ERR_INVALID_STATE;

    if (!spectrum_scan_lock_take(30000)) return ESP_ERR_TIMEOUT;

    memset(result, 0, sizeof(*result));
    result->timestamp = now_s();
    result->dwell_ms  = 100;

    s_current_result = result;

    // H1: PHR = total bytes after PHR including 2-byte FCS (HW-appended)
    // FCF(2) + Seq(1) + DstPAN(2) + DstAddr(2) + CmdID(1) + FCS(2) = 10
    static const uint8_t beacon_req[] = {
        0x0A, 0x03, 0x08, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x07,
    };

    for (int i = 0; i < SPECTRUM_NUM_CHANNELS; i++) {
        uint8_t ch = SPECTRUM_FIRST_CHANNEL + i;
        if (esp_ieee802154_set_channel(ch) != ESP_OK) continue;
        vTaskDelay(pdMS_TO_TICKS(5));

        esp_ieee802154_receive();
        if (esp_ieee802154_transmit(beacon_req, false) != ESP_OK) {
            ESP_LOGW(TAG, "beacon request TX failed on ch%d", ch);
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    s_current_result = NULL;
    result->completed = true;
    s_last = *result; s_last_valid = true;
    spectrum_scan_lock_give();

    ESP_LOGI(TAG, "Beacon scan complete: %d neighbors found", result->count);
    return ESP_OK;
}

#else /* CONFIG_OPENTHREAD_ENABLED */

/* 每信道默认驻留。thread_br.c 实测：300ms 抓不到自家协调器、800ms 稳定抓到，
   全频段巡检用 450ms。这里取 400：16 × 400 + 1500 余量 = 7900ms，刚好落在
   THREAD_BR_SCAN_MAX_TOTAL_MS(8000) 之内，能自然收尾而不是被预算掐断。 */
#define BEACON_DWELL_MS_DEFAULT   400
#define BEACON_DWELL_MS_MIN        20
#define BEACON_DWELL_MS_MAX      1000
/* 自家协调器在同板几厘米外，实测 -20/-21 dBm；邻居实测 -79 / -81 / -97
   （thread_br.c 的 ZB 探测门限同为 -50，两处必须一致）。 */
#define BEACON_SELF_RSSI_MIN      (-50)

/* 是不是本网关自己的 Zigbee 协调器。
   ① 声明信道时学到过 PAN → 精确匹配。故意**不比信道**：用户改了 Z2M/ZHA 的信道
      没回来同步时，自家网络就出现在别的信道上，那正是 zigbee_check 要抓的情形，
      这里仍然得认它是自家，否则推荐算法会把自家的新信道当邻居扣分。
   ② 没学到 → 只对短地址源（Zigbee 协调器的形态）用 RSSI 门限。Thread 网络的
      beacon 源是扩展地址，走不到这条，也就永远不会被误标成自家。 */
static bool is_self(uint16_t pan, int8_t rssi, bool short_src)
{
    const zigbee_config_t *zc = config_mgr_get_zigbee();
    if (zc && zc->pan_known && zc->pan_id == pan) return true;
    return short_src && rssi >= BEACON_SELF_RSSI_MIN;
}

/* 按 (PAN, 信道) 去重，保留最强的一次；self 只要有一路判定就算。 */
static void add_or_merge(beacon_scan_result_t *r, uint16_t pan, uint8_t ch,
                         int8_t rssi, uint8_t lqi, bool self)
{
    for (int i = 0; i < r->count; i++) {
        zigbee_neighbor_t *n = &r->networks[i];
        if (n->pan_id == pan && n->channel == ch) {
            if (rssi > n->rssi) { n->rssi = rssi; n->lqi = lqi; }
            n->self = n->self || self;
            return;
        }
    }
    if (r->count >= MAX_ZIGBEE_NEIGHBORS) return;
    zigbee_neighbor_t *n = &r->networks[r->count++];
    n->pan_id = pan; n->channel = ch; n->rssi = rssi; n->lqi = lqi;
    n->ext_pan_id = 0; n->self = self;
}

esp_err_t spectrum_beacon_scan_ex(uint16_t dwell_ms, beacon_scan_result_t *result)
{
    if (!result) return ESP_ERR_INVALID_ARG;
    if (!spectrum_is_initialized()) return ESP_ERR_INVALID_STATE;
    if (dwell_ms == 0) dwell_ms = BEACON_DWELL_MS_DEFAULT;
    if (dwell_ms < BEACON_DWELL_MS_MIN) dwell_ms = BEACON_DWELL_MS_MIN;
    if (dwell_ms > BEACON_DWELL_MS_MAX) dwell_ms = BEACON_DWELL_MS_MAX;

    memset(result, 0, sizeof(*result));
    result->timestamp = now_s();
    result->dwell_ms  = dwell_ms;

    /* 与能量扫描串行：两者都要独占 OT 的射频，同时发起会拿到 OT_ERROR_BUSY。
       thread_br 自己的周期性 zigbee_check 不拿这把锁，撞上时表现为
       ESP_ERR_INVALID_STATE（REST 报 thread_not_running_or_busy），重试即可。 */
    if (!spectrum_scan_lock_take(30000)) return ESP_ERR_TIMEOUT;

    thread_br_beacon_scan_t scan;   /* ~750B，与 handle_diag_beacon 同在 httpd 栈上 */
    esp_err_t err = thread_br_active_scan(dwell_ms, 0 /* ch11-26 */, &scan);
    /* ESP_ERR_TIMEOUT = 预算耗尽，但已收到的 beacon 都在 scan 里：如实标
       completed=false 继续往下；其余错误（Thread 没跑 / OT 忙）直接返回。 */
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        spectrum_scan_lock_give();
        return err;
    }

    /* 先并原始帧 —— 这是唯一能看见 Zigbee 协调器的视角。 */
    for (uint8_t i = 0; i < scan.raw_count; i++) {
        const thread_br_raw_beacon_t *b = &scan.raw[i];
        add_or_merge(result, b->pan_id, b->channel, b->rssi, b->lqi,
                     is_self(b->pan_id, b->rssi, !b->src_extended));
    }
    /* 再并 OT 解析出的（扩展地址源：Thread 网络、带扩展地址的 Zigbee 设备）。 */
    for (uint8_t i = 0; i < scan.count; i++) {
        const thread_br_beacon_t *b = &scan.nets[i];
        add_or_merge(result, b->pan_id, b->channel, b->rssi, b->lqi,
                     is_self(b->pan_id, b->rssi, false));
    }
    result->completed = scan.completed;

    s_last = *result; s_last_valid = true;
    spectrum_scan_lock_give();

    ESP_LOGI(TAG, "邻居扫描：%u 个网络（原始 %u / OT %u，%ums/信道，%s，%ums）",
             result->count, scan.raw_count, scan.count, dwell_ms,
             scan.completed ? "自然结束" : "预算耗尽", (unsigned)scan.elapsed_ms);
    return ESP_OK;
}

#endif /* CONFIG_OPENTHREAD_ENABLED */
