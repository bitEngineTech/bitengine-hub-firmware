#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* PoE 供电检测。
 *
 * 硬件：TMI7321 的 5V 输出经 R96(68K)/R97(100K) 分压进 POE_DETECT(IO0)。
 * 分压比 100/(68+100) = 0.595，即 5.0V 轨 → 引脚约 2.98V，高于 C5 的
 * VIH(≈0.75×3.3=2.48V)，读作高电平。
 *
 * ⚠️ IO0 **不是 ADC 通道**（C5 的 ADC1 只覆盖 GPIO1-6），读不到电压，
 * 只能读数字电平。轨压是否在安全范围（>3.3V 会经 ESD 钳位灌电流）必须
 * 用万用表量 5V 轨再乘 0.595 核算，固件无法自证。见待确认清单 C2。
 *
 * ⚠️ 永远只配成**输入**。这个脚外部接着来自 5V 轨的分压，配成输出等于
 * 让内部驱动器和外部分压对顶。
 */
esp_err_t poe_init(void);

/* 当前是否由 PoE 供电。未初始化或无硬件时返回 false。
 *
 * 就是一次 gpio_get_level()，不阻塞、无副作用，任何任务都能调。
 *
 * ⚠️ 与 poe_probe() 有一个**几十毫秒的竞态窗口**（2026-09-11 起真实存在：
 *    T2.5 之前本函数零调用者，现在 /api/v1/status 与 WS 推送都在调它）。
 *    poe_probe() 会依次把引脚配成悬空/上拉/下拉各读一次，期间本函数读到的是
 *    被拉住的电平，可能与真实供电状态不符。probe 结束必定恢复悬空，所以影响
 *    只有那一瞬 —— 最坏是一帧 WS 推送里 poe_present 报错，下一帧自愈。
 *    **没有为此加锁**：加锁会让本函数变成可能阻塞的，而调用它的是 WS 推送任务；
 *    为一个自愈的瞬时显示误差去阻塞推送，代价方向反了。
 *    probe 只在用户手动打开诊断时发生，不是周期性的。 */
bool poe_is_present(void);

/* 诊断用：分别在无上下拉 / 内部上拉 / 内部下拉三种配置下读电平。
 *
 * **判定 PoE 有无只看 no_pull**，另外两格仅作原始数据。2026-08-13 双向实测：
 *
 *   纯 PoE 供电   no_pull=1  pull_up=1  pull_down=0
 *   纯 USB 供电   no_pull=0  pull_up=1  pull_down=0
 *
 * ⚠️ 别指望用 pull_up 区分"引脚悬空（分压未上件）"和"正常无 PoE"——分辨不了：
 *      真悬空 + 上拉 → 3.3V              → 读 1
 *      无 PoE + 上拉 → 3.3×100/145=2.28V → 读 1
 *   两者数字读数相同（GPIO 翻转点 ≈VDD/2=1.65V，不是最坏情况 VIH 2.48V）。
 *   最初按 VIH 推算以为能分辨，实测才发现不行 —— 一个会误报硬件故障的诊断
 *   比没有诊断更糟，所以判定逻辑已简化为只看 no_pull。 */
esp_err_t poe_probe(bool *no_pull, bool *pull_up, bool *pull_down);

#ifdef __cplusplus
}
#endif
