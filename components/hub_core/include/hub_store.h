#pragma once
/* hub_store —— hubdb 分区的唯一写入者（docs/15_muse_direct_design_v1.md 2.5；只在直连型里编）。
 *
 * hubdb 存直连型自己的数据：muse（Muse 凭据与设置）、hub（设备名字 / 房间 / 推送开关）、zhub（Zigbee 设备库）、
 * mctrl（Matter fabric）。主 nvs 是配置与 OpenThread 的，这里永远不碰它。
 *
 * 用法：所有函数都可以从任何任务调（包括 PSRAM 栈的任务 —— 它们自己不能碰 flash，经队列交给 hub_store 任务做）；
 * 不能在 ISR 里调，**也不能在 esp_timer 回调里调**：put/erase 在队列满时最多阻塞 1 s，get/flush 要等答复（红线 10）。
 *  - put：2 s 防抖（同一个键连写只落最后一次），立即返回；
 *  - erase：不防抖，尽快执行；要"先删后回"（device.unpair）就接着调 flush；
 *  - get：同步，读己之写（还在防抖的值也读得到）；
 *  - flush：把在等的全部写掉再返回。
 * 每个 blob 第一个字节是格式版本（ver，1..255）：读到比 max_ver 新的当作没有（ESP_ERR_INVALID_VERSION），
 * 写的时候已存的更新就不覆盖（两种情况都打 W 级日志）。esp_restart() 之前会把防抖中的写落盘（shutdown handler）。写失败共试 3 次仍不行 → SYS 灯 5 s 红闪（CONFIG_EVENT_NVS_ERROR）+ E 级日志。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define HUB_STORE_NS_MUSE   "muse"
#define HUB_STORE_NS_HUB    "hub"
#define HUB_STORE_NS_ZHUB   "zhub"
#define HUB_STORE_NS_MCTRL  "mctrl"

typedef struct {
    bool      ready;           /* 分区初始化成功、任务在跑 */
    bool      reset_at_init;   /* 开机时 hubdb 坏了 / 版本不认识，被整区擦掉重建过（规则 2） */
    bool      flush_on_restart;/* 重启前落盘的钩子登记上了（shutdown handler 槽位只有 5 个，没登记上就会丢防抖中的写） */
    esp_err_t init_err;        /* 初始化失败的原因（ready = false 时看它） */
    int       pending;         /* 还在等写的条数 */
    uint32_t  writes, coalesced, retries, failures, refused_newer;
    int       last_err;        /* 最近一次后端错误（hub_store_core.h 的 HSC_*） */
} hub_store_status_t;

/* 初始化 hubdb 分区并起 hub_store 任务。幂等：重复调用直接返回上一次的结果。 */
esp_err_t hub_store_start(void);

/* 排一条写（2 s 防抖）。ns / key ≤ 15 个 [A-Za-z0-9_.-] 字符，ver ≥ 1，len ≤ 4000。数据当场拷走。 */
esp_err_t hub_store_put(const char *ns, const char *key, uint8_t ver, const void *data, size_t len);
/* 同上，队列满时最多等 wait_ms（hub_store_put 等 1000 ms）。等不起的调用方用 0：zhub 任务里还跑着 ASH，卡 1 s 会误判链路（第七步 a 审计）；
   回 ESP_ERR_TIMEOUT = 队列满、这条没排上 */
esp_err_t hub_store_put_wait(const char *ns, const char *key, uint8_t ver, const void *data, size_t len, uint32_t wait_ms);

/* 排一条删除（不防抖）。 */
esp_err_t hub_store_erase(const char *ns, const char *key);

/* 同步读。*len_inout 进来是 buf 容量、出去是负载长度；*ver 为格式版本。
   ESP_ERR_NOT_FOUND = 没有；ESP_ERR_INVALID_VERSION = 已存版本比 max_ver 新（当作没有）；
   ESP_ERR_INVALID_SIZE = buf 装不下（*len_inout 写入需要的长度）；ESP_ERR_TIMEOUT = 任务没在 timeout_ms 内答复。 */
esp_err_t hub_store_get(const char *ns, const char *key, uint8_t max_ver, uint8_t *ver,
                        void *buf, size_t *len_inout, uint32_t timeout_ms);

/* 把在等的全部执行掉再返回。返回 ESP_FAIL 表示其中有条目重试用尽、最终没写进去。 */
esp_err_t hub_store_flush(uint32_t timeout_ms);

/* 同步擦掉整个命名空间（恢复出厂用）。 */
esp_err_t hub_store_erase_ns(const char *ns, uint32_t timeout_ms);
/* 恢复出厂（设计第十二节）：两种都清 muse，wipe_all 再清 hub，zhub 保留。hub_store 没起来（安全模式）会先起。 */
esp_err_t hub_store_factory_reset(bool wipe_all);

void      hub_store_get_status(hub_store_status_t *out);
