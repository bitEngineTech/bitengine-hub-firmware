#pragma once
/* 探针测试（probes_test.c）用的最小桩：只给 wd_probes.c 编译所需的那几个名字。
   测试里桥恒为"未运行"，probe_mg24 走"取不到证据"的早退分支，不参与断言。 */
#include <stdbool.h>
#include <stdint.h>

typedef enum { BRIDGE_STATE_STOPPED = 0, BRIDGE_STATE_RUNNING } bridge_state_t;
typedef struct { int active_clients; uint32_t uart_rx_bytes; } bridge_stats_t;

bridge_state_t        ezsp_bridge_get_state(void);
const bridge_stats_t *ezsp_bridge_get_stats(void);
bool                  ezsp_bridge_health_probe(int timeout_ms);
bool                  uart_bridge_line_says_absent(int window_ms);
