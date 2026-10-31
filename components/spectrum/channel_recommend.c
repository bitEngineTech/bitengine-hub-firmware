#include "spectrum.h"
#include "thread_br.h"   /* COEX_TOO_CLOSE_MAX — the one coexistence rule */
#include <math.h>
#include <stdlib.h>

#define W_ENERGY    0.6f
#define W_NEIGHBOR  15.0f
#define W_THREAD    10.0f

/* 返回推荐信道，或 SPECTRUM_NO_RECOMMENDATION(-1) 表示数据不足以给出推荐。
 *
 * ⚠️ 这里曾经有一个把结论完全推反的缺陷：能量归一化写的是 energy[i] + 100，
 * 于是没测到的信道（哨兵 -100）归一化成 0 = "零能量" = 得分最高 = **最干净**，
 * 会被优先推荐出去。在本平台上 WiFi 共存经常拒掉一部分 ED，所以这不是边角
 * 情况而是常态：越是测不到的信道越会被推荐。现在一律先看 valid_mask，
 * 没有真实读数的信道直接排除出候选。
 */
int8_t spectrum_recommend_channel(const spectrum_scan_result_t *energy,
                                   const beacon_scan_result_t *beacons,
                                   uint8_t thread_channel)
{
    if (!energy) return SPECTRUM_NO_RECOMMENDATION;

    /* 有效信道太少时，"最好的那个"只是矮子里拔将军，给出去会误导用户。
       宁可明说无法推荐 —— 上层会据此提示先解决共存问题。 */
    int valid_count = __builtin_popcount(energy->valid_mask);
    if (valid_count < SPECTRUM_MIN_VALID_FOR_RECOMMEND) {
        return SPECTRUM_NO_RECOMMENDATION;
    }

    float scores[SPECTRUM_NUM_CHANNELS];
    bool  usable[SPECTRUM_NUM_CHANNELS];

    for (int i = 0; i < SPECTRUM_NUM_CHANNELS; i++) {
        usable[i] = spectrum_ch_valid(energy, i);
        if (!usable[i]) {
            scores[i] = 0.0f;   /* 不参与比较，赋值只为避免读未初始化内存 */
            continue;
        }
        // Normalize energy: map from dBm range [-100, 0] to [0, 100]
        float normalized = (float)(energy->energy[i] + 100);  // -100→0, 0→100
        if (normalized < 0) normalized = 0;
        if (normalized > 100) normalized = 100;
        scores[i] = 100.0f - W_ENERGY * normalized;
    }

    // Deduct for neighbor Zigbee networks on same channel
    if (beacons) {
        for (int n = 0; n < beacons->count; n++) {
            /* 自家协调器不是"邻居"：它所在的信道正是用户网络现在的信道，
               扣分等于告诉用户"你现在的信道被别人占了"。 */
            if (beacons->networks[n].self) continue;
            uint8_t ch = beacons->networks[n].channel;
            if (ch >= SPECTRUM_FIRST_CHANNEL &&
                ch < SPECTRUM_FIRST_CHANNEL + SPECTRUM_NUM_CHANNELS) {
                scores[ch - SPECTRUM_FIRST_CHANNEL] -= W_NEIGHBOR;
            }
        }
    }

    /* Thread coexistence. 2026-09-05 之前这里只对 ±3 信道扣 10 分，而共存告警、
       zigbee/status 的 channel_advice 用的都是 COEX_TOO_CLOSE_MAX(=5) 的**硬排除**。
       结果频谱页推荐 CH15/16（Thread 在 11），Dashboard 转头就把它标成"太近"——
       两处规则不一致，用户照推荐改完立刻收到告警。现在与 thread_br_list_safe_
       zigbee_channels() 同一条线：距离 ≤ COEX_TOO_CLOSE_MAX 的信道先整体排除；
       只有排除后一个候选都不剩（Thread 信道居中且其余信道全没测到）才退回扣分制，
       保证"给不出推荐"仍只由 valid_count 决定。 */
    if (thread_channel >= SPECTRUM_FIRST_CHANNEL &&
        thread_channel < SPECTRUM_FIRST_CHANNEL + SPECTRUM_NUM_CHANNELS) {
        bool excl[SPECTRUM_NUM_CHANNELS];
        int remaining = 0;
        for (int i = 0; i < SPECTRUM_NUM_CHANNELS; i++) {
            uint8_t ch = SPECTRUM_FIRST_CHANNEL + i;
            int distance = abs((int)ch - (int)thread_channel);
            excl[i] = (distance <= COEX_TOO_CLOSE_MAX);
            if (usable[i] && !excl[i]) remaining++;
        }
        for (int i = 0; i < SPECTRUM_NUM_CHANNELS; i++) {
            if (!usable[i]) continue;
            if (remaining > 0) {
                if (excl[i]) usable[i] = false;             /* the rule, same as channel_advice */
            } else {
                /* fallback: nothing safe was measured — rank by distance instead */
                int distance = abs((int)(SPECTRUM_FIRST_CHANNEL + i) - (int)thread_channel);
                scores[i] -= (distance == 0) ? W_THREAD * 2 : W_THREAD;
            }
        }
    }

    // Find best channel — 只在有真实读数的信道里挑
    int best = -1;
    for (int i = 0; i < SPECTRUM_NUM_CHANNELS; i++) {
        if (!usable[i]) continue;
        if (best < 0 || scores[i] > scores[best]) {
            best = i;
        }
    }

    if (best < 0) return SPECTRUM_NO_RECOMMENDATION;   /* 上面的 count 检查已保证不会走到 */
    return (int8_t)(SPECTRUM_FIRST_CHANNEL + best);
}
