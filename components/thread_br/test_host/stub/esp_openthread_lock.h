#pragma once
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
static inline bool esp_openthread_lock_acquire(TickType_t t) { (void)t; return true; }
static inline void esp_openthread_lock_release(void) { }
