#pragma once
/* hub_cmds —— 命令处理函数的实现（docs/15_muse_direct_design_v1.md 6.4、附录 A）。只在直连型里编。
 * 命令表（hub_registry）保持纯逻辑、主机测试编它；处理函数要读网络、Thread、内存，按名字在这里绑定。
 * M3 实现：device.health、hub.thread.network、hub.devices.list / hub.device.get（Thread 网络层清单 thread_inventory，第七节）、
 * hub.events.recent（事件环，6.7）。 */
#include "hub_call.h"
#include "hub_registry.h"

/* 这个构建里实现了的处理函数；NULL = 命令在表里但本版还没有实现（调用方回 unsupported） */
hub_cmd_fn hub_cmds_find(const char *name);
