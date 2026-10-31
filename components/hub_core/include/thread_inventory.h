#pragma once
/* thread_inventory —— Thread 网络层清单（docs/15_muse_direct_design_v1.md 第七节；只在直连型里编）。
 *
 * 与设计的出入：放在 hub_core，不在 thread_br —— thread_br 是 HA 型也编的共用组件，放进去要给它加按配置变的源文件；放 hub_core
 * 不动它一个字节（架构 5.8）。符号前缀仍是 thread_inventory_，check_profiles.py 的 B-08（HA 型 ELF 里不许有）照样管用。
 *
 * 刷新：hub_svc（常驻任务，见 hub_svc.h）每 60 s 调一次 thread_inventory_refresh()、其余 10 s 一轮调 watch（7.4 写的是 esp_timer 投作业到 hub_jobs，作业层还没有；
 * 回调里不碰 OT 这条照样守住）。刷新在 OT 锁里拷数据（目标 < 20 ms，实测值见 thread_inventory_stats），放锁后关联、比较，
 * 新出现的记一条 LIFECYCLE 事件（只进环），离开的（连续两轮、且距上次看到满 110 s）记一条 AVAILABILITY 事件（推送，6.7）。
 * Thread 关着：清单清空、基线重来（2.8 表"用户关掉 Thread"）；BR 掉网期间不比较（thread_down 事件已经报了），回来后重建基线；
 * SRP 服务没在跑时只列子节点 / 路由器、不比较。建基线后 5 分钟里新出现的不报"新来"（TI_QUIET_MS）。
 *
 * 线程：快照由一把锁护着；refresh 与 thread_inventory_with 都可以从任何任务调（不能在 ISR / esp_timer 回调里：要等 OT 锁）。
 * 回调在锁里执行，只做拷贝 / 写 JSON，别在里面阻塞。 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "hub_ti_core.h"

typedef struct {
    uint32_t refreshes, ot_busy, last_lock_us, max_lock_us;
    uint32_t last_refresh_us, max_refresh_us;   /* 整次刷新（含关联、比较；持清单锁的时长） */
    uint16_t entries, hosts, children, routers;
    bool     hosts_truncated;
    uint32_t joined, left;
} thread_inventory_stats_t;

/* 分配快照（PSRAM，约 161 KB）。幂等（失败时已分配的放掉，可以再调）。 */
esp_err_t thread_inventory_init(void);
/* 刷新一次。ESP_ERR_TIMEOUT = OT 锁忙（这轮不算）；ESP_ERR_INVALID_STATE = 没 init。Thread 关着 / 掉网返回 ESP_OK（清单相应清空或保留）。 */
esp_err_t thread_inventory_refresh(void);
/* 在锁里把快照交给 fn。max_age_ms > 0 且快照比它旧（或从没刷新过）就先刷新一次。锁 500 ms 拿不到返回 ESP_ERR_TIMEOUT。 */
esp_err_t thread_inventory_with(uint32_t max_age_ms, void (*fn)(const ti_raw_t *raw, const ti_list_t *list, void *ctx), void *ctx);
void      thread_inventory_get_stats(thread_inventory_stats_t *out);
/* 两次刷新之间的轻量采样（hub_svc 每 10 s 调；OT 锁只等 50 ms）：网络掉了或 SRP 服务停过，下次刷新重建基线 ——
   SRP 在两次 60 s 刷新之间停了又起（网线闪断时路由管理器自动停 SRP）刷新本身看不到，重新注册慢的设备会被报"离开再新来" */
void      thread_inventory_watch(void);
