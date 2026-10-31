#include "spectrum.h"
#include "webhook.h"

#include <string.h>
#include <stdio.h>
#include <math.h>
#include "esp_log.h"

static const char *TAG = "spectrum_mon";

#define INTERFERENCE_THRESHOLD_DB  10

static spectrum_scan_result_t s_prev_result;
static bool s_has_prev = false;
/* 每信道"上一轮已超阈值"标记 —— 连续两轮才算真干扰 */
static bool s_spike_pending[SPECTRUM_NUM_CHANNELS];

void spectrum_background_check(void)
{
    spectrum_scan_result_t current;

    /* 驻留用默认值（1ms）：50ms 在本平台上会被 WiFi 共存 100% 拒绝，
       后台扫描等于永远失败。见 thread_br.h 的驻留-成功率实测曲线。
       ⚠️ 这里必须显式要**单轮**，不能用给 UI 的多轮 max-hold：
       峰值保持测的是"这一秒里最吵能到多少"，实测同一信道连续三次
       可以读到 -51 / -95 / -52，波动远大于 10dB 的尖峰阈值，
       拿它做差值比较会不停地刷假的干扰告警 webhook。
       单轮瞬时值接近底噪、跨次稳定，才是趋势比较该用的统计量。 */
    esp_err_t err = spectrum_energy_scan_ex(0, 1, &current);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Background scan failed: %s", esp_err_to_name(err));
        return;
    }

    // Compare with previous scan for interference spike detection
    if (s_has_prev) {
        for (int i = 0; i < SPECTRUM_NUM_CHANNELS; i++) {
            /* 只比两次都真实测到的信道。没测到的那些 energy[] 里留的是 -100
               哨兵，拿它做差会造出 -100 → -60 这种凭空的"干扰尖峰"告警。
               判据用 valid_mask 而不是 <= -100：真实底噪实测能到 -108，
               用数值当哨兵会把最干净的信道误判成失败。 */
            if (!spectrum_ch_valid(&current, i) ||
                !spectrum_ch_valid(&s_prev_result, i)) {
                continue;
            }
            int delta = current.energy[i] - s_prev_result.energy[i];
            /* 连续两轮都超阈值才报。改造前这条后台检测因为用 50ms 驻留而
               **从来没真正跑起来过**（扫描必然失败直接 return），现在它第一次
               会出数，所以要防住"偶然一次突发被当成持续干扰"刷 webhook。 */
            if (delta > INTERFERENCE_THRESHOLD_DB) {
                if (!s_spike_pending[i]) { s_spike_pending[i] = true; continue; }
            } else {
                s_spike_pending[i] = false;
                continue;
            }
            {
                uint8_t ch = SPECTRUM_FIRST_CHANNEL + i;
                ESP_LOGW(TAG, "Interference spike on ch%d: %d dB increase (%d → %d dBm)",
                         ch, delta, s_prev_result.energy[i], current.energy[i]);

                char data[96];
                snprintf(data, sizeof(data),
                         "{\"channel\":%d,\"delta\":%d,\"level\":%d}",
                         ch, delta, current.energy[i]);
                webhook_post_event(WEBHOOK_EVENT_INTERFERENCE, data);
            }
        }
    }

    // S3: update previous for next comparison
    memcpy(&s_prev_result, &current, sizeof(s_prev_result));
    s_has_prev = true;
}
