/*
 * Zigbee / Thread 信道共存：挑信道 + 靠太近的判定。
 *
 * 单独一个编译单元，因为这些是**纯计算**，值得被主机侧直接编译验证 ——
 * 规则本身（阈值、候选集、边界是 < 还是 <=）恰恰是最容易改错又最不容易在
 * 板子上看出来的部分。放在 thread_br.c 里就只能靠 grep 检查代码长什么样，
 * 证明不了结果对。测试见 test_host/。
 *
 * 依赖刻意压到最小：只要 thread_br.h / ncp_version.h / esp_log.h。
 */

#include "thread_br.h"
#include "config_mgr.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "thread_ch";


/* WiFi 空隙信道：802.15.4 的 15/20/25 分别落在 2.4G WiFi ch1/6/11 之间。
   本机 WiFi 是 5GHz only，构不成干扰源，但邻居的 2.4G WiFi 是 —— 能量扫描
   量的正是它。ch26 不在集合里：贴频段上边沿，多数射频在那里发射功率降额。 */
static const uint8_t k_wifi_gap_channels[] = { 15, 20, 25 };

#define THREAD_CH_MIN 11
#define THREAD_CH_MAX 25   /* 26 一律排除，理由同上 */

static int ch_distance(uint8_t a, uint8_t b)
{
    return (a > b) ? (int)(a - b) : (int)(b - a);
}

uint8_t thread_br_pick_channel(uint8_t zigbee_channel, bool zigbee_known,
                               uint8_t configured_channel)
{
    /* 没有要躲的东西就别擅自改变出厂行为 */
    if (!zigbee_known || zigbee_channel < 11 || zigbee_channel > 26) {
        return configured_channel;
    }

    /* 一级：WiFi 空隙信道里间隔足够的，取间隔最大者 */
    int best = -1, best_dist = -1;
    for (size_t i = 0; i < sizeof(k_wifi_gap_channels); i++) {
        uint8_t ch = k_wifi_gap_channels[i];
        int d = ch_distance(ch, zigbee_channel);
        if (d > COEX_TOO_CLOSE_MAX && d > best_dist) {
            best = ch;
            best_dist = d;
        }
    }
    if (best >= 0) {
        ESP_LOGI(TAG, "Thread 信道选 %d（Zigbee 在 %u，间隔 %d）",
                 best, zigbee_channel, best_dist);
        return (uint8_t)best;
    }

    /* 二级：放宽到全范围。Zigbee 落在 ch20 时一级无解，这里给出 ch11。 */
    best = -1; best_dist = -1;
    for (uint8_t ch = THREAD_CH_MIN; ch <= THREAD_CH_MAX; ch++) {
        int d = ch_distance(ch, zigbee_channel);
        if (d > best_dist) {
            best = ch;
            best_dist = d;
        }
    }

    ESP_LOGW(TAG, "Zigbee 在 %u，WiFi 空隙信道 {15,20,25} 没有间隔 >%d 的选择；"
                  "改选 ch%d（间隔 %d），代价是可能与 2.4G WiFi 重叠 —— "
                  "与 Zigbee 拉开距离优先",
             zigbee_channel, COEX_TOO_CLOSE_MAX, best, best_dist);
    return (uint8_t)best;
}

/* Zigbee 信道的唯一来源：用户声明值。为什么不是实测值，见 config_mgr.h 里
   zigbee_config_t 的注释（简言之：读它需要先 write_config，等于造 EZSP 主机）。
   未声明就当"不知道"，不知道 ≠ 有问题。 */
static bool zigbee_declared_channel(uint8_t *out)
{
    const zigbee_config_t *zb = config_mgr_get_zigbee();
    if (!zb || !zb->channel_declared) return false;
    if (zb->channel < 11 || zb->channel > 26) return false;
    if (out) *out = zb->channel;
    return true;
}

bool thread_br_channel_too_close(uint8_t thread_channel, uint8_t *zigbee_channel_out)
{
    uint8_t zb = 0;
    if (!zigbee_declared_channel(&zb)) return false;   /* 未知 ≠ 有问题 */

    if (zigbee_channel_out) *zigbee_channel_out = zb;
    return ch_distance(thread_channel, zb) <= COEX_TOO_CLOSE_MAX;
}

uint8_t thread_br_list_safe_zigbee_channels(uint8_t thread_channel,
                                            uint8_t *out, uint8_t max)
{
    if (!out || max == 0) return 0;
    if (thread_channel < 11 || thread_channel > 26) return 0;

    uint8_t n = 0;
    for (uint8_t ch = 11; ch <= 26 && n < max; ch++) {
        if (ch_distance(ch, thread_channel) > COEX_TOO_CLOSE_MAX) out[n++] = ch;
    }
    return n;
}

bool thread_br_should_repick(uint8_t current_thread_channel, bool had_child,
                             uint8_t *new_channel)
{
    /* 接入过设备就一律不动。这一条排在最前面，任何后续条件都不能推翻它。 */
    if (had_child) return false;

    uint8_t zb = 0;
    if (!thread_br_channel_too_close(current_thread_channel, &zb)) return false;

    uint8_t picked = thread_br_pick_channel(zb, true, current_thread_channel);

    /* 挑不出更好的就别动 —— 白搬一次家还不如留在原地。理论上不会走到
       （全表验证过每格都有间隔 ≥6 的解），留着是防止候选集以后被改窄。 */
    if (picked == current_thread_channel) return false;

    if (new_channel) *new_channel = picked;
    return true;
}

void thread_br_get_coex_status(coex_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    uint8_t thread_ch;
    if (thread_br_get_active_channel(&thread_ch) != ESP_OK) return;

    uint8_t zb = 0;
    if (!zigbee_declared_channel(&zb)) return;

    out->known          = true;
    out->thread_channel = thread_ch;
    out->zigbee_channel = zb;
    out->separation     = (uint8_t)ch_distance(thread_ch, zb);
    out->too_close      = out->separation <= COEX_TOO_CLOSE_MAX;
}
