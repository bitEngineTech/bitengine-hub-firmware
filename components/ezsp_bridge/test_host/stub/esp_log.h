#pragma once
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...) printf("I %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("E %s: " fmt "\n", tag, ##__VA_ARGS__)
/* ESP_LOGD 在主机侧默认静默：真机上它也要 CONFIG_LOG_MAXIMUM_LEVEL=DEBUG 才出来，
   让主机测试打印 debug 只会淹掉断言输出。2026-08-17 补 —— read_ncp_config()
   用它记录"单个配置项不受支持"这种非致命情况。 */
#define ESP_LOGD(tag, fmt, ...) ((void)0)
