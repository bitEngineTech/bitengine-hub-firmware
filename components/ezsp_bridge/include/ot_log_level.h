/* components/ezsp_bridge/include/ot_log_level.h — "OPENTHREAD" 这个日志标签的 ESP 级别，映射只在这里写一次。
 *
 * 两处共用：thread_br 启动 OT 时设（不在 USB 桥模式时）；usb_bridge_stop() 退出 USB 模式时恢复。
 * 为什么放在 ezsp_bridge：USB 桥用 esp_log_level_set("*", …) 静音 / 恢复全部日志，而 IDF 设 "*" 会清掉所有
 * 按标签设的级别（log/src/log_level/tag_log_level/tag_log_level.c）；退出时得有人把它设回去，ezsp_bridge 又不能
 * 反向依赖 thread_br。单独一个头而不是放进 ezsp_bridge.h：后者要在主机上编译（test_host/），不能依赖 sdkconfig.h。
 * 级别的取舍与理由见 Kconfig BITENGINE_OT_LOG_LEVEL 的 help（≥3 才放 INFO：OT 的 NOTE 也是按 INFO 打的）。 */
#pragma once
#include "esp_log.h"
#include "sdkconfig.h"

#ifdef CONFIG_BITENGINE_OT_LOG_LEVEL
#define BITENGINE_OT_ESP_LOG_LEVEL \
    (CONFIG_BITENGINE_OT_LOG_LEVEL >= 3 ? ESP_LOG_INFO : CONFIG_BITENGINE_OT_LOG_LEVEL == 2 ? ESP_LOG_WARN : ESP_LOG_ERROR)
#endif
