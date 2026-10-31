#pragma once
#include <stdio.h>
#ifdef SRP_TEST_VERBOSE
#define ESP_LOG_AT(lvl, tag, ...) do { printf("    [%s/%s] ", lvl, tag); printf(__VA_ARGS__); printf("\n"); } while (0)
#else
#define ESP_LOG_AT(lvl, tag, ...) do { (void)sizeof(tag); } while (0)
#endif
#define ESP_LOGE(tag, ...) ESP_LOG_AT("E", tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) ESP_LOG_AT("W", tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) ESP_LOG_AT("I", tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) ESP_LOG_AT("D", tag, __VA_ARGS__)
