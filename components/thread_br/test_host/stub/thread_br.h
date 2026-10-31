#pragma once
#include <stdbool.h>
extern int g_fake_br_running;
static inline bool thread_br_is_running(void) { return g_fake_br_running != 0; }
