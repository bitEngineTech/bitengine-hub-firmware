/* CHIP 的限额（sdkconfig.defaults.direct 的 CONFIG_CHIP_PROJECT_CONFIG 指向本文件；docs/15 设计 9.2 的表）。
 * 2026-10-24 用户定 Matter 设备 32 → 128（CONFIG_BITENGINE_MATTER_MAX_NODES）：每台一条订阅、各占一个安全会话（CASE），
 * 再给配网（PASE + 新设备的 CASE）、访谈、控制留 16 个 —— 原来的 16 个，第 17 台起订阅就占不到会话。会话表在 SessionManager 里
 * （controller 工厂用 chip::Platform::New 建，本配置走 PSRAM），每个约 0.5 KB，144 个约 70 KB PSRAM、不占内部 SRAM。
 * 同时建 CASE 的客户端 4 → 8：一批设备一起掉线又回来时，订阅重建（同时最多 4 条在建，mc_sub.cpp）+ 控制 + 访谈不互相挤。 */
#pragma once
#include <sdkconfig.h>

#define CHIP_CONFIG_SECURE_SESSION_POOL_SIZE (CONFIG_BITENGINE_MATTER_MAX_NODES + 16)
#define CHIP_CONFIG_CONTROLLER_MAX_ACTIVE_CASE_CLIENTS 8
/* 同时配网的设备数：commissioner 里的静态池，默认 64（约 140 KB；静态池在 PSRAM，linker.lf） */
#define CHIP_CONFIG_CONTROLLER_MAX_ACTIVE_DEVICES 2
