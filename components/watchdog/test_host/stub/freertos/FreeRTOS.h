#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE       1
#define pdFALSE      0
#define pdPASS       1
#define pdFAIL       0
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) (ms)
