#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_TAG_MAX   16
#define LOG_MSG_MAX   144

/* 一条日志。定长槽位：环形缓冲的加锁与遍历都变成常数时间的 memcpy，
   不用管变长记录的边界。浪费的那点空间在 8MB PSRAM 面前无所谓。 */
typedef struct {
    uint32_t seq;                 /* 单调递增，0 表示空槽 */
    uint32_t ms;                  /* 设备运行毫秒数（取自日志行里的时间戳） */
    char     level;               /* E W I D V；解析失败为 '?' */
    char     tag[LOG_TAG_MAX];
    char     msg[LOG_MSG_MAX];    /* 已剥离 ANSI；超长会截断并加 "…" */
} log_rec_t;

/* 安装 esp_log_set_vprintf 钩子并分配环形缓冲。
 *
 * 要尽早调用（NVS 之后、其它组件之前），否则启动早期的日志抓不到。
 * 分配失败不是致命错误：返回非 OK，串口输出照常，只是没有历史可查。
 */
esp_err_t log_store_init(void);

/* 取出 seq > since_seq 的记录，最多 max 条，按时间升序写入 out。
   返回实际条数。缓冲由调用方提供 —— 持锁期间只做定长 memcpy，
   格式化（snprintf/JSON）留到锁外做。 */
int  log_store_fetch(uint32_t since_seq, log_rec_t *out, int max);

/* next_seq  下一条将要分配的序号，客户端下次带 since=next_seq-1 增量拉取
   dropped   环形缓冲溢出而丢弃的行数（累计）——必须上报，否则用户会
             以为拿到的是全部日志
   capacity  槽位总数
   stored    当前有效记录数 */
/* 被 tag 过滤器主动挡掉的行数（见 log_store.c 的 k_noisy_tags）。
   与 log_store_stats 的 dropped 是**两回事**：那个是环满覆盖，这个是有意不收。 */
uint32_t log_store_filtered(void);

void log_store_stats(uint32_t *next_seq, uint32_t *dropped,
                     uint32_t *capacity, uint32_t *stored);

/* 只取 E/W —— 数据源是独立的告警环，不会被 INFO 洪水挤掉。
   主环实测保留窗口只有约 111 秒（OPENTHREAD 的 INFO 占 88%），
   所以「刚才有没有报错」这个问题必须问这个接口，不能拿主环去 level 过滤。 */
int  log_store_fetch_warn(uint32_t since_seq, log_rec_t *out, int max);

/* 告警环的统计。capacity==0 表示告警环没分配上（PSRAM 不够）。 */
void log_store_warn_stats(uint32_t *dropped, uint32_t *capacity, uint32_t *stored);

#ifdef __cplusplus
}
#endif
