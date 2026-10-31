#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------- Constants ----------------
#define SPECTRUM_NUM_CHANNELS    16   // ch11-ch26
#define SPECTRUM_FIRST_CHANNEL   11
#define MAX_ZIGBEE_NEIGHBORS     32

// ---------------- Data Structures ----------------

typedef struct {
    int8_t   energy[SPECTRUM_NUM_CHANNELS];  // ch11-ch26, dBm
    uint32_t timestamp;
    uint16_t scan_duration_ms;
    /* bit i = ch(11+i) 拿到了真实读数。为 0 的信道 energy[i] 是 -100 哨兵，
       不是测量值 —— 本平台上 WiFi 共存会拒掉一部分 ED，部分缺失是常态。
       ⚠️ 任何消费 energy[] 的代码都必须先测这一位：把 -100 当能量算，
       会算出"最干净的信道"这种正好相反的结论。 */
    uint16_t valid_mask;
    /* 这组读数背后的观测量：passes 轮 × scan_duration_ms 的驻留，逐信道取最大值。
       没有它就无法解释数值 —— 1 轮 1ms 读到的是瞬时底噪，16 轮取最大读到的是
       "这一秒里最吵能到多少"，两者相差可以有 40dB，不能混着比。 */
    uint8_t  passes;
} spectrum_scan_result_t;

/* 某个信道是否有真实读数 */
static inline bool spectrum_ch_valid(const spectrum_scan_result_t *r, int idx)
{
    return r && idx >= 0 && idx < SPECTRUM_NUM_CHANNELS &&
           (r->valid_mask & (1u << idx));
}

typedef struct {
    uint16_t pan_id;
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  lqi;
    uint64_t ext_pan_id;    /* OT 构型下拿不到（beacon 载荷不解析），恒 0 */
    /* true = 本网关自己的 Zigbee 协调器（同板那颗 MG24）。判据见 beacon_scan.c
       is_self()：学到过 PAN 就精确匹配，否则短地址源 + RSSI ≥ -50 dBm。
       为什么必须标：recommend 会按邻居扣分，不标就把自己网络所在的信道当成
       "被别人占了"，推荐结果永远指向别处。 */
    bool     self;
} zigbee_neighbor_t;

typedef struct {
    zigbee_neighbor_t networks[MAX_ZIGBEE_NEIGHBORS];
    uint8_t           count;
    uint32_t          timestamp;
    /* false = 扫描被时间预算掐断（THREAD_BR_SCAN_MAX_TOTAL_MS），networks[] 只是
       部分信道的结果。必须如实往外报：不然"ch26 上没邻居"和"ch26 没扫到"分不开。 */
    bool              completed;
    uint16_t          dwell_ms;   /* 实际每信道驻留 */
} beacon_scan_result_t;

// ---------------- Public API ----------------

esp_err_t spectrum_init(void);

/* duration_per_ch_ms = 每信道驻留。传 0 用默认值（1ms —— 见 thread_br.h 里
   实测出来的驻留-成功率曲线，100ms 在本平台上是 0/16）。
   返回 ESP_OK 表示至少一个信道有效，**必须**再看 result->valid_mask。 */
esp_err_t spectrum_energy_scan(uint16_t duration_per_ch_ms,
                               spectrum_scan_result_t *result);

/* passes = 采样轮数（0 用默认值）。轮数的语义差别很大，选错会得到没意义的数：
   - 多轮（默认 16）逐信道取**最大值**：回答"这个信道最坏能有多吵"，
     适合给人看的频谱图；但同一信道两次扫描可能差 40dB（取决于那 1 秒内有没有
     突发流量），**不能用来做差值比较**。
   - 单轮取**瞬时值**：接近底噪，跨次稳定，适合趋势/尖峰检测。 */
esp_err_t spectrum_energy_scan_ex(uint16_t duration_per_ch_ms, uint8_t passes,
                                  spectrum_scan_result_t *result);

/* 推荐信道的可用性判据：有效信道太少时算出来的推荐没有意义 */
#define SPECTRUM_MIN_VALID_FOR_RECOMMEND  8
/* spectrum_recommend_channel() 的"给不出推荐"返回值。
   以前这里返回魔数 15，与"真的推荐 ch15"无法区分。 */
#define SPECTRUM_NO_RECOMMENDATION        (-1)

/* 邻居网络发现（beacon request → beacon）。
 *
 * OT 构型（出货构型）：走 thread_br_active_scan()。裸 esp_ieee802154_transmit()
 * 会撞 OT 端口的 ISR 断言；而 otLinkActiveScan 又结构性看不见 Zigbee 协调器
 * （短地址源被丢弃，见 thread_br.h 的 thread_br_raw_beacon_t），所以结果是
 * OT 报的 + PCAP 原始帧合并、按 (PAN, 信道) 去重取最强。**需要 Thread BR 在跑**，
 * 否则 ESP_ERR_INVALID_STATE —— OT 构型下 Thread 停着时没有别的路能发帧。
 * 阻塞约 16 × dwell_ms（默认 400 → ≈7s），调用方是单任务 httpd，与 /spectrum/scan 同量级。
 *
 * dwell_ms=0 取默认；钳 [20,1000]。ESP_ERR_TIMEOUT = 没拿到 spectrum 锁（另一次扫描在跑）。
 * 扫描被预算掐断不算失败：返回 ESP_OK，result->completed=false，已收到的照样给。 */
esp_err_t spectrum_beacon_scan_ex(uint16_t dwell_ms, beacon_scan_result_t *result);
esp_err_t spectrum_beacon_scan(beacon_scan_result_t *result);   /* = _ex(0, result) */
/* 最近一次结果若不老于 max_age_s 就直接给，否则扫一次。给 recommend 用：
   前端点一次"扫描"会先后打 /beacon 和 /recommend，没有它同一秒内要扫两遍、
   单任务 httpd 再多卡 7s。 */
esp_err_t spectrum_beacon_scan_cached(beacon_scan_result_t *result, uint32_t max_age_s);

int8_t    spectrum_recommend_channel(const spectrum_scan_result_t *energy,
                                     const beacon_scan_result_t *beacons,
                                     uint8_t thread_channel);

void      spectrum_background_check(void);

/* 2026-09-11(T4.1)：删掉了 spectrum_get_history() 与 spectrum_history_append()
 * 以及整个 spectrum_history.c。
 *
 * 那是一份**等硬件的空壳**：四处实现全在 #ifdef CONFIG_BITENGINE_HAS_EXT_FLASH
 * 里等一颗 W25Q32 外部 Flash，而那颗芯片 **BOM 已确认永久不上件**
 *（PCB 待确认清单 B4："封装位彻底删除，省面积防误贴"），GPIO8 已改作 PHY_RESET。
 * 前端也从没调过 /api/v1/spectrum/history。
 *
 * 留着的代价不是几十行代码，而是"看起来快做好了"——下一个人会以为只差接线。
 * 真要做频谱历史，走主 Flash 的 NVS 环形缓冲即可：一条记录 24 字节、
 * 一天两次扫描约 1.15KB，根本用不着外部 Flash。别再等硬件。 */

// Internal (cross-file): serialize concurrent spectrum operations
bool      spectrum_scan_lock_take(uint32_t timeout_ms);
void      spectrum_scan_lock_give(void);
bool      spectrum_is_initialized(void);

#ifdef __cplusplus
}
#endif
