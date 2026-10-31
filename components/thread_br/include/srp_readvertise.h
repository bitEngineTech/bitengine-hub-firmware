#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* SRP 代播服务的强制重新宣告。
 *
 * 解决的问题：网关一重启，Thread 子设备的 Matter 会话就失效，但 matter-server
 * 仍握着已死的订阅对象，最长要等 600s（电池设备的 CHIP 存活超时上限）才恢复上报。
 *
 * 能在网关侧解决，是因为**这些设备的 mDNS 记录本来就是网关在替它们喊**：
 * 子设备用 SRP 注册到网关，libopenthread_br 的 advertising proxy 再通过
 * ESP-IDF mdns 组件以 delegated host 的形式播到局域网。实测抓包（2026-08-10）：
 * `_matter._tcp` 的唯一应答源是网关，SRV target 指向的 AAAA 是 Thread 网内的
 * OMR 地址，设备在局域网上没有任何独立存在感。
 *
 * 做法见 srp_readvertise.c 顶部。对外只有四个接口。 */

/* 启动后台任务（thread_br_start() 成功后调用）。重复调用是幂等的。 */
esp_err_t srp_readv_start(void);

/* 停止后台任务（thread_br_stop() 里、拆 OpenThread 之前调用）。 */
esp_err_t srp_readv_stop(void);

/* 手动把当前所有 SRP 代播服务重新宣告一遍（调试 / REST 触发用）。
 * 只置标志立即返回，实际动作在后台任务里做，最长 POLL_MS 后开始。 */
esp_err_t srp_readv_trigger_all(void);

/* 统计：成功重新宣告次数 / 失败次数 / 当前跟踪的服务数。参数可传 NULL。 */
void srp_readv_get_stats(uint32_t *readvertised, uint32_t *failed, uint32_t *tracked);

/* 扫描开销：最近一轮与开机以来最长一轮**持 OT 锁**的微秒数、最近一轮见到的 SRP 服务数。参数可传 NULL。
 * 代播条数到几百条时用来判断这一轮会不会卡住 OT 任务（docs/test_cases_scale_baseline.md「SRP 代播满载压测」）。 */
void srp_readv_get_scan_stats(uint32_t *lock_us, uint32_t *lock_max_us, uint32_t *services);

/* 重启前把所有代播服务发一次 TTL=0 的 goodbye（只撤不加）。
 *
 * srp_readv_start() 会把它注册成 esp_restart() 的 shutdown handler，所以**正常
 * 情况下不需要手工调用**——按键、WebUI、OTA、恢复出厂、看门狗自愈全都覆盖到。
 * 单独暴露出来是为了能在测试里直接触发。
 *
 * 为什么需要：开机后的 bye+重新宣告只解决"控制器收不到 Added"，解决不了"控制器
 * 压根没察觉网关重启过"。2026-08-15 实测，网关重启后 +49s 就重新宣告了，但
 * matter-server 全程没把节点标 offline，握着死订阅等到 6 分钟后设备被人按醒才恢复。
 * 先撤记录，控制器当场知道不可达；重启后的宣告才是真正的 Added。 */
void srp_readv_bye_all(void);


