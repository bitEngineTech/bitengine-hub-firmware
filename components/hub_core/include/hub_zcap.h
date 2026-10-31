#pragma once
/* hub_zcap —— Zigbee 设备的能力与设备类（docs/15_muse_direct_design_v1.md 8.8；M5 第一步：只按访谈记下的簇与 ZoneType 推）。
 * 纯逻辑：不依赖 ESP-IDF，test_host/hub_zcap_test 原样编它。只在直连型、开了 zhub 时被 hub_zigbee 调。
 *
 * 规则（8.8 的表，取舍写在 hub_zcap.c 各行旁）：
 *  - 按端点的输入簇定能力；IAS Zone 按 ZoneType 定（人体 / 门磁 / 漏水 / 烟雾 / 振动；燃气、CO 在能力表里没有对应项，只定设备类，不硬套成 smoke）；
 *  - 涂鸦私有簇的设备（M5 第八步）：按社区设备库导出的表（zh_tuya）里它报的字段加能力，报警类按表推出的 ZoneType（zh_dev_zone_type）同上；
 *  - 小米 / Aqara（M5 第十步）：型号在小米表（zh_lumi）里的同样按表加能力；**没访谈完也算**（休眠设备几乎不应访谈，型号从主动上报学），那时只用表；
 *    表改写了的标准簇（门磁的 OnOff 是开合、灯的 Occupancy 丢掉）不按通用规则定能力；
 *  - 控制路径现在真能驱动的另算（hub_zcap_ctl_caps）：ZCL 的开关 / 亮度输入簇，加上涂鸦表里有写法的（M5 第九步写数据点）；
 *  - 按钮：输出簇里有 OnOff / Level / Scenes、输入簇里没有 OnOff；
 *  - 电池：有 Power Configuration 输入簇、且不是路由器（市电的路由器也常带这个簇）；
 *  - 颜色（0x0300）要读 ColorCapabilities 才知道是色温还是 XY，这一步不定能力，只影响设备类（color_light）；
 *  - 没访谈完的设备：没有能力，设备类 "unknown"。 */
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"      /* CONFIG_BITENGINE_ENABLE_ZHUB；主机测试由 test_host/run.sh 用 -D 给 */
#endif

/* zhub 只属于直连型（MD-13；tools/check_scope_claims.py 查 components/zhub/ 以外的每处引用都在这个 #if 里） */
#if CONFIG_BITENGINE_ENABLE_ZHUB
#include "zh_dev.h"

/* 能力位（HUB_CAP_*，hub_model.h） */
uint32_t    hub_zcap_caps(const zh_dev_rec_t *r);
/* 控制路径（zhub_control，8.11）现在真能做的能力：HUB_CAP_ONOFF / HUB_CAP_LEVEL —— ZCL 输入簇，或涂鸦表里有写法 */
uint32_t    hub_zcap_ctl_caps(const zh_dev_rec_t *r);
/* 设备类（附录 A 的 kind：motion_sensor / contact_sensor / dimmable_light / …），静态字符串 */
const char *hub_zcap_kind(const zh_dev_rec_t *r, uint32_t caps);
#endif
