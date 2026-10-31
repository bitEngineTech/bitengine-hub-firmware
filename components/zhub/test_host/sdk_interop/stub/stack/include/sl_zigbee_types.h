#pragma once
#include "sdk_plat.h"
/* 旧式缓冲区管理的最小替身：一个缓冲 = 池里的一项（连续存放，"链接缓冲"按偏移直接取） */
typedef uint16_t sli_buffer_manager_buffer_t;
#define SL_ZIGBEE_NULL_MESSAGE_BUFFER ((sli_buffer_manager_buffer_t)0xFFFF)
typedef struct { sli_buffer_manager_buffer_t tail; } sli_zigbee_message_buffer_queue_t;
#define PACKET_BUFFER_SIZE 32
