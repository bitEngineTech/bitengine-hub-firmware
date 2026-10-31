#pragma once
/* wifi_probe —— 诊断用：从 WiFi STA 网口持续 ping 一个目标，记录"洞"。
   2026-09-14 为查「WiFi 地址每 60s 失联 3-24s」而加：从主机侧只能看到"没回音"，
   分不清是网关自己发不出/收不到，还是 AP 那边掉的；从网关侧盯自己的 WiFi 口，
   洞的起止就有了网关 uptime 时间戳，能和日志里其它事情对时。
   只在 /api/v1/diag/wifi_probe 下使用，出厂不启动。 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define WIFI_PROBE_HOLE_RING 8

typedef struct {
    bool     running;
    char     target[16];
    uint32_t sent, replied, missed;
    uint32_t holes;                 /* 完成的洞（有开始有结束）个数 */
    uint32_t cur_miss_run;          /* 正在进行中的洞已连续丢几个 */
    uint32_t max_hole_ms;
    int8_t   rssi;                  /* 最近一次洞开始时读到的 RSSI */
    /* 最近 8 个洞：开始时刻（uptime ms，按发包时刻估算）与时长 */
    uint32_t hole_start_ms[WIFI_PROBE_HOLE_RING];
    uint32_t hole_ms[WIFI_PROBE_HOLE_RING];
} wifi_probe_status_t;

esp_err_t wifi_probe_start(const char *target_ip, uint32_t interval_ms);   /* 已在跑则先停；interval 0 = 1000 */
esp_err_t wifi_probe_stop(void);
void      wifi_probe_get_status(wifi_probe_status_t *out);
