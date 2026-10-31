#!/bin/bash
# 主机侧测试：配置导入 / 导出的 UTF-8 规则（T9.33）—— 不需要板子、不需要 ESP-IDF 环境，只要 gcc。
#
#   bash components/config_mgr/test_host/run.sh
#
# 编的是**产品源文件本身**（../config_export.c、../radio_region.c、../../utf8/utf8.c），
# 外加仓库里钉死版本的 cJSON（managed_components/espressif__cjson，与固件同一份）。
# 做法同 components/webui/test_host：造一组最小的 ESP-IDF 头文件放进 shim 目录，
# config_export.c 一个字都不用为测试改。
set -e
cd "$(dirname "$0")"
CC=${CC:-gcc}
ROOT=../../..
CJSON_DIR=$ROOT/managed_components/espressif__cjson/cJSON
if [ ! -f "$CJSON_DIR/cJSON.c" ]; then
    echo "❌ 找不到 $CJSON_DIR/cJSON.c —— 先在仓库根跑一次 idf.py build 让组件管理器把它拉下来"
    exit 1
fi
OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT
mkdir -p "$OUT/shim/freertos"

cat > "$OUT/shim/esp_err.h" <<'H'
#pragma once
typedef int esp_err_t;
#define ESP_OK               0
#define ESP_FAIL            -1
#define ESP_ERR_NO_MEM       0x101
#define ESP_ERR_INVALID_ARG  0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND    0x105
H
cat > "$OUT/shim/esp_event.h" <<'H'
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef const char *esp_event_base_t;
#define ESP_EVENT_DECLARE_BASE(id) extern esp_event_base_t const id
#define ESP_EVENT_DEFINE_BASE(id)  esp_event_base_t const id = #id
esp_err_t esp_event_post(esp_event_base_t base, int32_t id, const void *data, size_t size, uint32_t ticks);
typedef void *SemaphoreHandle_t;
#define portMAX_DELAY 0xffffffffu
#define xSemaphoreTake(m, t) ((void)(m), (void)(t))
#define xSemaphoreGive(m)    ((void)(m))
H
cat > "$OUT/shim/esp_netif_types.h" <<'H'
#pragma once
#include <stdint.h>
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
H
cat > "$OUT/shim/esp_log.h" <<'H'
#pragma once
#include <stdio.h>
#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "    [E %s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "    [W %s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) do { (void)(tag); } while (0)
#define ESP_LOGD(tag, fmt, ...) do { (void)(tag); } while (0)
H

$CC -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -I"$OUT/shim" -I../include -I../../utf8/include -I"$CJSON_DIR" \
    -o "$OUT/t" export_import_test.c ../config_export.c ../radio_region.c \
    ../../utf8/utf8.c "$CJSON_DIR/cJSON.c"
"$OUT/t" 2>"$OUT/log" || { echo ""; echo "── 被测代码的日志 ──"; cat "$OUT/log"; exit 1; }
