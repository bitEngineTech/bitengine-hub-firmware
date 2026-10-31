#pragma once

#include "ezsp_bridge.h"

void            bridge_stats_init(void);
bridge_stats_t *bridge_stats_get(void);
void            bridge_stats_reset(void);
