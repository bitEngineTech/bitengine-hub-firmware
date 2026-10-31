#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_event.h"
#include "esp_system.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------- Constants ----------------

#define WDT_FEED_INTERVAL_MS        500
#define HEALTH_CHECK_INTERVAL_MS    10000
#define MG24_HEARTBEAT_TIMEOUT_S    180  // 3 min (Zigbee idle + ASH keepalive can be infrequent)
#define MG24_EZSP_QUERY_TIMEOUT_MS  2000
#define MG24_FAIL_THRESHOLD         3
#define MG24_RESET_PULSE_MS         100
#define MG24_RESET_COOLDOWN_S       300
/* v1 的 HEAP_WARN_THRESHOLD / HEAP_CRITICAL_THRESHOLD / HEAP_LEAK_SAMPLES
   已删除：它们比的是 esp_get_free_heap_size()，即含 8MB PSRAM 的聚合值，
   四条判定路径全部不可能触发（2026-08-10 实测）。
   取而代之的是 wd_resource.c 的分堆水位 + 分配失败计数，阈值定义在那里。 */
#define TEMP_WARN_THRESHOLD         75.0f
#define TEMP_CRITICAL_THRESHOLD     85.0f
#define WIFI_RSSI_WARN_THRESHOLD    (-80)
#define UART_ERROR_RATE_WARN_PCT    5

// ---------------- Health Status ----------------

typedef struct {
    uint8_t  mg24_alive;
    uint8_t  mg24_fail_count;       /* legacy：v2 起由 wd_probes 判定，恒为 0 */
    uint32_t mg24_last_reset_time;
    uint32_t mg24_total_resets;
    /* ⚠️ legacy：含 8MB PSRAM 的聚合值，仅为前端兼容保留，不作任何判定依据。
       要看内存健康用 /api/v1/diag/watchdog 的 resource 段。 */
    uint32_t free_heap;
    uint32_t min_free_heap;
    float    temperature;
    bool     heap_leak_suspected;   /* legacy：v2 起不再维护，恒为 false */
    uint32_t uptime_seconds;
    uint8_t  last_reset_reason;
    int8_t   wifi_rssi;
    uint8_t  uart_error_rate_pct;
} health_status_t;

// ---------------- Events ----------------

ESP_EVENT_DECLARE_BASE(WATCHDOG_EVENT);

typedef enum {
    WATCHDOG_EVENT_MG24_RESET,
    WATCHDOG_EVENT_HEAP_LOW,
    WATCHDOG_EVENT_HEAP_CRITICAL,
    WATCHDOG_EVENT_HEAP_LEAK_SUSPECTED,
    WATCHDOG_EVENT_TEMP_HIGH,
    WATCHDOG_EVENT_TEMP_CRITICAL,
    WATCHDOG_EVENT_ABNORMAL_BOOT,
    WATCHDOG_EVENT_WIFI_RSSI_LOW,
    WATCHDOG_EVENT_UART_ERROR_RATE_HIGH,
} watchdog_event_id_t;

// ---------------- Public API ----------------
/* WATCHDOG_EVENT_ABNORMAL_BOOT 的事件数据（T9.27 / T9.30 ④）。两处会发：
   health_monitor 在开机后补发（上一次是异常复位，reset_reason 为真原因），
   wd_recovery 在看门狗整机重启前抢发（reset_reason 为 0）。webhook / MQTT 把 cause 原样带出去。 */
#define WD_ABNORMAL_CAUSE_LEN 40
typedef struct {
    uint8_t reset_reason;                  /* esp_reset_reason_t（wd_boot_reason_get() 的值） */
    char    cause[WD_ABNORMAL_CAUSE_LEN];  /* 机器可读："panic" / "task_wdt" / "watchdog:subsys/thread" …
                                              只含 [a-z0-9_:/.-]，可直接拼进 JSON */
} wd_abnormal_boot_t;


esp_err_t              watchdog_init(void);
const health_status_t *watchdog_get_status(void);

/* 启动阶段的内部 SRAM 水位轨迹。串口在复位时会随 USB-CDC 重枚举丢行，靠日志
   抓不全，所以把标记留在 RAM 里由 /api/v1/diag/heap 读出。step 必须是字符串
   字面量（只存指针，不拷贝）。满了以后的标记直接丢掉，所以上限要按配置给够：
   直连型多了 bt_mem / hub_store / hub_svc / muse_link 这几步，正常开机已经恰好打满 24 个，M4 的 zhub 还要加
   （docs/15 设计第十五节 A-24、components/zhub/CHECKLIST.md Z44）。 */
#if CONFIG_BITENGINE_PROFILE_DIRECT
#define HEAP_MARK_MAX 32
#else
#define HEAP_MARK_MAX 24
#endif
typedef struct {
    const char *step;
    uint32_t    internal_free;   // MALLOC_CAP_INTERNAL|MALLOC_CAP_DEFAULT
    uint32_t    dma_free;        // MALLOC_CAP_DMA
    uint32_t    uptime_ms;
} heap_mark_t;

void   watchdog_heap_mark(const char *step);

/* malloc 失败次数（IDF heap 失败回调统计）。0 = 目前还没有分配真的失败过。 */
void   watchdog_get_alloc_failures(uint32_t *count, uint32_t *last_size,
                                   uint32_t *last_caps);
size_t watchdog_heap_marks(const heap_mark_t **out);
esp_err_t              watchdog_reset_mg24(void);
void                   watchdog_restart_esp32(void);

/* ================================================================
 * v2 架构（docs/04_watchdog_design_v2.md）
 * ================================================================ */

// ---------------- 故障分类（F1-F5，见设计文档第二节）----------------

typedef enum {
    WD_FAULT_NONE = 0,
    WD_FAULT_TASK_STALL,      /* F2 任务停转 */
    WD_FAULT_SUBSYS_DEAD,     /* F3 子系统假死 */
    WD_FAULT_RESOURCE,        /* F4 资源耗尽 */
    WD_FAULT_PERIPH_DEAD,     /* F5 外设失联 */
    WD_FAULT_CLASS_MAX,
#if CONFIG_BITENGINE_PROFILE_DIRECT
    /* 计划内重启（docs/15 设计 3.2，交叉清单 A-26）：不是故障。放在 CLASS_MAX 之后，所以不进恢复策略表、
       wd_report() 不认它；只经 wd_forensics_record_planned() 记（进门 L2 只记 RTC、出门 L3 进 NVS 历史）—— 都 < L4，不加自愈计数、
       不会因为配对重启几次就进安全模式，下次开机也不算异常开机。 */
    WD_FAULT_PLANNED = 0x10,
#endif
} wd_fault_class_t;

/* 恢复阶梯。数值即级别，便于比较与升级。 */
typedef enum {
    WD_LEVEL_NONE    = 0,
    WD_LEVEL_LOG     = 1,   /* L1 记录 + 事件 */
    WD_LEVEL_TARGET  = 2,   /* L2 定向修复 */
    WD_LEVEL_SUBSYS  = 3,   /* L3 子系统重启 */
    WD_LEVEL_REBOOT  = 4,   /* L4 整机重启 */
    /* L5（IO27→MR 硬件复位）等 AM2，本版本不实现 */
} wd_level_t;

// ---------------- 层 B：任务心跳注册表 ----------------

/* 直连型多 muse_sess / hub_store / hub_svc / zh_ash / zh_main / ble_window / matter_ctrl 的槽，合计超过 16（docs/15 设计第十五节 A-24、8.2）。
   wd_probes_tick 与 /diag/watchdog 把整张表放在栈上（每项 24 B），多 4 项 = 多 96 B。 */
#if CONFIG_BITENGINE_PROFILE_DIRECT
#define WD_MAX_TASKS 20
#else
#define WD_MAX_TASKS 16
#endif

typedef enum {
    WD_SEV_OBSERVE,   /* 只记录，不恢复 */
    WD_SEV_RECOVER,   /* 触发定向恢复，失败则升级 */
    WD_SEV_FATAL,     /* 停转即视为整机不可用 */
} wd_severity_t;

typedef struct {
    const char   *name;            /* 字符串字面量，只存指针 */
    uint32_t      deadline_ms;     /* 超过此时长未 kick 判定停转，最小 3000 */
    wd_severity_t severity;
    esp_err_t   (*recover)(void);  /* NULL = 无定向恢复手段 */
} wd_task_spec_t;

/* 注册一个需要被监视的任务。返回 slot（≥0），失败返回 -1。
   必须在被监视任务开始循环之前调用。注册表是静态数组，不分配内存。 */
int  wd_register(const wd_task_spec_t *spec);

/* 任务循环体内调用，声明"我又转了一圈"。O(1)、无锁、可在任何上下文调用。 */
void wd_kick(int slot);

/* 已知的长操作（MG24 OTA、UART 基准测试）临时豁免，避免误判。 */
void wd_suspend(int slot, uint32_t grace_ms);

/* 槽位当前无主：拥有它的任务已**按计划**结束，扫描应跳过，别当成停转。
   与 wd_suspend() 的区别是无上界 —— 用于停留时长由外部决定的情形
   （如 EZSP 桥停在 MG24 bootloader，何时退出由用户决定）。
   不需要配对的"上线"调用：下一次 wd_kick() 或 wd_register() 会自动清除。 */
void wd_slot_offline(int slot);

/* 供层 F 的扫描使用：全部 RECOVER/FATAL 任务是否都在按时 kick。 */
bool wd_registry_scan(void);

typedef struct {
    const char   *name;
    uint32_t      deadline_ms;
    uint32_t      age_ms;        /* 距上次 kick */
    uint16_t      miss_count;
    wd_severity_t severity;
    bool          suspended;
    /* 该任务自采的栈余量（字节，高水位口径）；0 = 还没被 kick 过所以没采到。
       此前只有全局最小值对外可见，于是「谁的栈开太大 / 谁快爆了」都看不出来。
       2026-08-17 就因为看不到这个，按静态估算把 ws_push 从 3072 裁到 1536，
       上板才发现实际只剩 16 字节余量。裁栈必须看这个数，不能看 objdump。 */
    uint32_t      stack_left;
} wd_task_view_t;

size_t wd_registry_snapshot(wd_task_view_t *out, size_t max);

/* 层 D 用：已注册任务中栈余量最小的那个（首次 kick 后才有句柄）。 */
void wd_registry_stack_low(const char **name, uint32_t *bytes);

// ---------------- 层 D：资源守卫 ----------------

typedef struct {
    uint32_t alloc_fail_total;
    uint32_t alloc_fail_window;    /* 当前 60s 窗口内的失败数 */
    uint32_t alloc_fail_last_size;
    uint32_t alloc_fail_last_caps;
    uint32_t internal_free;
    uint32_t internal_min;
    uint32_t dma_free;
    uint32_t dma_min;
    uint32_t dma_largest;
    const char *stack_low_task;    /* 余量最小的任务名，NULL = 未知 */
    uint32_t stack_low_bytes;
} wd_resource_t;

void wd_resource_init(void);
void wd_resource_get(wd_resource_t *out);
void wd_resource_tick(void);       /* 由 health_task 每 10s 调用 */

// ---------------- 层 C：子系统探针 ----------------

typedef struct {
    bool     mg24_ok;
    /* 当前结论是否成立。false = **不判定** —— 没有客户端（或 OTA 占着 UART）。唯一的探测
       手段（ASH RST）会把 NCP 的 Zigbee 协议栈打掉，所以没有客户端时不探（T9.31）。
       ⚠️ 不判定 ≠ 活着，也 ≠ 死了。别把它当布尔的 mg24_ok 用（伪造证据是
       本模块 v1 的原罪）。这期间线路电平判据仍在跑，见 wd_probes.c。 */
    bool     mg24_judged;
    uint32_t mg24_last_ok_s;       /* 上次拿到证据的 uptime 秒 */
    bool     thread_ok;
    bool     thread_running;
    bool     network_ok;
    bool     httpd_ok;
} wd_probe_view_t;

void wd_probes_tick(void);         /* 由 health_task 每 10s 调用 */
void wd_probes_get(wd_probe_view_t *out);

/* MG24 空闲探测窗口：探测期间 tcp_server 延迟 accept，避免 ASH RST 打断
   正在建立的 Z2M 会话。由 ezsp_bridge/tcp_server.c 查询。 */
bool wd_mg24_probe_active(void);

// ---------------- 层 E：恢复引擎 ----------------

/* 上报一个故障。可从任何任务调用；实际恢复动作在 wd_action 任务里执行，
   调用方不会被阻塞。detail 必须是字符串字面量。 */
void wd_report(wd_fault_class_t cls, wd_severity_t sev, const char *detail,
               esp_err_t (*recover)(void));

/* 故障消失时调用，让该故障类的升级级别回落。 */
void wd_report_recovered(wd_fault_class_t cls, const char *detail);

typedef struct {
    wd_level_t  level[WD_FAULT_CLASS_MAX];
    const char *last_action;
    uint32_t    last_action_ts;
    uint8_t     self_heal_count;
    bool        safe_mode;
} wd_recovery_view_t;

void wd_recovery_get(wd_recovery_view_t *out);
void wd_recovery_init(void);

/* 各子系统在自己的 init 里登记 L3 恢复入口，避免 watchdog 反向依赖它们
   （webui/led_ctrl 等已经 REQUIRES watchdog，反向依赖会形成 CMake 循环）。
   key 以子串方式匹配上报时的 detail；key 与 fn 都必须是常驻的。 */
esp_err_t wd_register_recovery(const char *key, esp_err_t (*fn)(void));

/* 恢复引擎自身的心跳槽与应急路径。恢复动作是任意回调、可能阻塞任意久，
   引擎卡死时后续故障会全部堆在队列里无人处理，而外部看不出来——所以它
   自己也要被监视，且停转时必须走一条不经过自己队列的路。 */
int  wd_recovery_action_slot(void);
void wd_recovery_emergency(const char *detail);   /* 不返回 */

/* 安全模式：自愈重启过于频繁时进入，只起网络 + WebUI。
   main.c 在初始化各子系统前查询。 */
bool wd_safe_mode(void);
const char *wd_safe_mode_reason(void);
esp_err_t wd_safe_mode_exit(void);   /* 用户显式退出（WebUI），清记录并重启 */

// ---------------- 真复位原因（T9.27，wd_boot_reason.c）----------------

/* main.c 非干净复位后补的那次 esp_restart() 会把复位原因变成 SW。补重启前调 note 留下真原因，
   开机早期调 latch 取回；之后一律用 get，别再直接调 esp_reset_reason()。 */
void               wd_boot_reason_note(esp_reset_reason_t original);
void               wd_boot_reason_latch(void);
esp_reset_reason_t wd_boot_reason_get(void);

/* 整机重启（L4）前问一下有没有升级在写 flash（T9.30 ①）。watchdog 不能反向依赖 OTA 组件，
   由 main.c 注册一个把 C5 网页上传 / 云端 / MG24 三条路合起来的查询。没注册 = 不等。 */
void               wd_recovery_set_ota_busy_query(bool (*busy)(void));

/* 故障类的短名（"task" / "subsys" / "resource" / "periph"），越界返回 "unknown" */
const char        *wd_fault_class_name(wd_fault_class_t cls);
/* 填 wd_abnormal_boot_t.cause：看门狗自愈 → "watchdog:<类>/<detail>"；其余按复位原因 → "panic" 等 */
void               wd_abnormal_cause_watchdog(wd_abnormal_boot_t *ev, wd_fault_class_t cls,
                                              const char *detail);
void               wd_abnormal_cause_reset(wd_abnormal_boot_t *ev, esp_reset_reason_t r);

// ---------------- 层 G：取证 ----------------

#define WD_HISTORY_MAX 8

typedef struct {
    uint32_t uptime_s;             /* 事发时的 uptime */
    uint8_t  fault_class;
    uint8_t  level;
    char     detail[24];
    uint32_t internal_free;
    uint32_t dma_free;
    uint32_t alloc_fails;
} wd_forensic_rec_t;

void   wd_forensics_init(void);
void   wd_forensics_record(wd_fault_class_t cls, wd_level_t lvl,
                           const char *detail);
size_t wd_forensics_history(wd_forensic_rec_t *out, size_t max);
/* 上一次重启是否由看门狗自愈触发；out_rec 可为 NULL。 */
bool   wd_forensics_last_was_self_heal(wd_forensic_rec_t *out_rec);
#if CONFIG_BITENGINE_PROFILE_DIRECT
/* 记一笔计划内重启（detail ≤ 23 字符，如 muse_pair_enter / muse_pair_exit），然后由调用方 esp_restart()。
   to_history = true 按 L3 记，进 8 条的 NVS 历史；false 按 L2 只记 RTC（下次开机读得到"上次是怎么重启的"）。
   一次配对两次重启，只让出门那条进历史 —— 不然试几次就把真实故障的历史冲掉（2026-10-22 审计）。两种都 < L4：不加自愈计数。
   ⚠️ to_history 时会写 flash：调用方必须在内部栈的任务里。与 wd_action 任务的取证写入没有互斥（窗口极小，
   最坏是那一条历史记录写花），原有的取证路径一行没改（HA 型不变）。 */
void   wd_forensics_record_planned(const char *detail, bool to_history);
#endif

// ---------------- 层 H：故障注入（CONFIG_BITENGINE_FAULT_INJECTION）----------------

/* wd_recovery.c 使用；不属于对外 API */
bool        wd_forensics_safe_mode(void);
uint8_t     wd_forensics_self_heal_count(void);
const char *wd_forensics_last_detail(void);
void        wd_forensics_clear_safe_mode(void);

#if CONFIG_BITENGINE_FAULT_INJECTION
/* kind: task_hang / heap_drain / mg24_mute / thread_radio_freeze / httpd_block */
esp_err_t wd_fault_inject(const char *kind, const char *target, uint32_t arg);
void      wd_fault_clear_all(void);
bool      wd_fault_active(const char *kind);
#endif

// ---------------- Internal (cross-file) ----------------

void      wdt_feed_start(void);
void      health_monitor_start(void);
esp_err_t mg24_reset_init(void);
esp_err_t mg24_do_reset(void);

/**
 * True when resetting the MG24 takes the whole gateway down with it.
 *
 * On AM1 boards IO23 is tied into the RS706 supervisor's RESET net through
 * R113 and reaches C5's own EN through R58 — pulling it low reboots both
 * chips (`docs/hardware/硬件缺陷档案.md` X4). The reset is still offered
 * as a last resort for a wedged MG24, but every caller that shows a warning
 * must ask here rather than hardcode one: the WebUI used to promise "Zigbee
 * may briefly disconnect" while the entire gateway restarted.
 *
 * Becomes false once A1 (R113 → diode + 10K pull-up) is populated and
 * CONFIG_BITENGINE_MG24_RESET_ISOLATED is set.
 */
bool mg24_reset_takes_board_down(void);

/**
 * Drive the MG24 nBOOT line (active low: 0 = request bootloader entry).
 *
 * The Gecko bootloader samples this pin on every reset, outside the
 * reset-cause check (btl_main.c:557), so holding it low across a *software*
 * reset is enough — which is the only option here, because pulsing
 * MG24_RESET_PIN would take the whole board down with it (see mg24_do_reset).
 *
 * Leaving it low also traps the MG24 in the bootloader across subsequent
 * resets, so every caller must restore it to 1.
 *
 * @return ESP_ERR_NOT_SUPPORTED when built without
 *         CONFIG_BITENGINE_HAS_MG24_RESET_PIN (pin never configured).
 */
esp_err_t mg24_set_nboot(int level);

/* 放在文件末尾（HA 型里预处理后不存在，放中间会让后面的行号整体移动） */
#if CONFIG_BITENGINE_PROFILE_DIRECT
/* 恢复引擎此刻在做什么："wd_action <idle|msg|rec|L1|L2|L3|L4|ota> <上报者>"（应急重启写进历史的就是它；截到 n − 1） */
void wd_recovery_doing(char *out, size_t n);
#endif

#ifdef __cplusplus
}
#endif
