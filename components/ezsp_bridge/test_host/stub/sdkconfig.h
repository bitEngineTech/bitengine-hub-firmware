#pragma once
/* 主机测试的 sdkconfig 桩：什么都不定义 = HA 型的默认配置（CONFIG_BITENGINE_ENABLE_ZHUB 未定义 → 头文件声明
   完整桥的 ezsp_bridge_init）。ezsp_bridge.h 自 2026-10-22 起显式包含 sdkconfig.h（直连型不带桥，架构 5.8 R-2）。 */
