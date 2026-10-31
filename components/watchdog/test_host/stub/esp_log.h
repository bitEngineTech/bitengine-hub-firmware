#pragma once
#include <stdio.h>
/* 默认静默：测试要断言行为，不要被日志淹没。WD_TEST_VERBOSE=1 时打开。 */
#ifdef WD_TEST_VERBOSE
#define WD_LOG(tag, ...) do { printf("    [%s] ", tag); printf(__VA_ARGS__); printf("\n"); } while (0)
#else
#define WD_LOG(tag, ...) do { (void)(tag); } while (0)
#endif
#define ESP_LOGI(tag, ...) WD_LOG(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) WD_LOG(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) WD_LOG(tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) WD_LOG(tag, __VA_ARGS__)
