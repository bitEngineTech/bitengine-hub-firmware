#pragma once
/*
 * 合成 mDNS 负载 —— 扩容实验用（docs/扩到50设备_2026-08-17.md 的 C 方案）。
 *
 * 为什么存在：这台设备的容量墙是一串**可数的计数/长度上限**，不是内存
 * （Zigbee 侧固件根本不知道接了几个设备；Thread 侧 OT 的表全是编译期预分配）。
 * 所以验证容量不需要 N 台真实设备，只需要**造出 N 条与真实设备字节等价的记录**。
 *
 * 本模块按真实 Matter 设备的线上格式连续注册假服务：
 *   instance  <16hex fabric>-<16hex node>._matter._tcp
 *   host      <16hex>.local，端口 5540，AAAA 一条
 *   TXT       SII= / SAI= / SAT=
 *   subtype   _I<16hex fabric>（同一 fabric 的设备共享，与真实行为一致）
 *
 * ⚠️ 不落 NVS、不碰 SRP server、不影响真实设备；调用 set(0) 即全部撤回。
 * ⚠️ **出厂必须关**（CONFIG_BITENGINE_ENABLE_MDNS_LOAD，default n）：
 *    这些记录会真的播到局域网上，HA / matter-server 会真的看到一堆假 Matter 设备。
 */

#include <stddef.h>
#include "sdkconfig.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 上限：**从 MDNS_MAX_SERVICES 推导**，永远比墙高 16。
 *  撞墙本身就是实验要观测的现象，工具卡在墙之前就没用了；而写死一个数
 *  （原来是 80，按当时 MDNS_MAX_SERVICES=64 选的）在墙被抬高后就够不着了
 *  —— 2026-09-15 把墙抬到 128 做实验时正好踩到。 */
#define MDNS_LOAD_MAX (CONFIG_MDNS_MAX_SERVICES + 16)

/**
 * 把合成服务数量设成 n（0 = 全部撤回）。幂等：多退少补。
 *
 * @param n          目标条数，0..MDNS_LOAD_MAX
 * @param out_active 实际生效条数（撞到 mDNS 上限时会小于 n）
 * @param out_err    第一条失败的 esp_err_t（全成功则 ESP_OK）
 * @return ESP_ERR_INVALID_ARG 参数越界；ESP_OK 其余情况（**部分失败也返回 OK**，
 *         失败信息在 out_active / out_err 里 —— 撞墙是预期结果不是调用错误）
 */
esp_err_t mdns_load_set(int n, int *out_active, esp_err_t *out_err);

/**
 * 设置合成记录用的 fabric id（16 位十六进制）。
 * ⚠️ **只能在 count()==0 时调用** —— 名字是由 fabric 推出来的，有条目活着时改
 * 会导致旧记录撤不掉。用途：做"合成 ≡ 真实"等价性比对时设成真实 fabric，
 * 排除 subtype 名字不同带来的字节差。
 * @return ESP_ERR_INVALID_STATE 有条目活着；ESP_ERR_INVALID_ARG 不是 16 位十六进制
 */
esp_err_t mdns_load_set_fabric(const char *hex16);

/** 当前 fabric id。 */
const char *mdns_load_fabric(void);

/** 当前生效的合成服务条数。 */
int mdns_load_count(void);

#ifdef __cplusplus
}
#endif
