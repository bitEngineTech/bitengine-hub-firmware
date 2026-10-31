#pragma once
/* ble_window —— 直连型里 BLE 的唯一入口（docs/15_muse_direct_design_v1.md 第三节；只在 BITENGINE_ENABLE_MUSE_LINK 下编）。
 *
 * 眼下只有策略 C（隔离重启，设计 3.2）：
 *   正常开机 ──/muse/pair──▶ ble_window_request_isolated()：写 RTC 一次性标志、记一笔计划内重启、esp_restart()
 *   下次开机 app_main 最早处 ble_window_take_pair_boot() 取走标志 → 配对剖面（不起 Thread / 频谱 / MQTT / 调度…）
 *   配对剖面里 muse_pair 调 ble_window_open() 开外设窗口，结束后 ble_window_close()，再 esp_restart() 回正常开机。
 * 正常开机永远不初始化 BT，并且一开机就 ble_window_release_bt_mem() 把 BT 的静态 .bss/.data（约 1 KB）还给堆。
 * ⚠️ 不是"窗口外零占用"：控制器的 IRAM 代码（libble_app + libbt 约 25 KB）链接进来就常驻（2026-10-22 审计量的）。
 * A（并存）/ B（暂停 Thread）、中心角色（Matter 扫描）等 G0-4 与 P3，相应 API 现在回 ESP_ERR_NOT_SUPPORTED。
 *
 * 线程：open / close 把活交给 ble_window 自己的任务串行做（内部栈 4 KB、优先级 5），调用方阻塞等结果（有上限）；
 * 不能在 ISR、esp_timer 回调、NimBLE host 任务里调 open / close（会等自己）。窗口超时由 esp_timer 只投递一条消息。
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum { BLEW_OWNER_MUSE_PAIR = 1, BLEW_OWNER_MATTER = 2 } blew_owner_t;
typedef enum { BLEW_POLICY_CONCURRENT, BLEW_POLICY_PAUSE_THREAD, BLEW_POLICY_ISOLATED } blew_policy_t;
typedef enum { BLEW_ROLE_PERIPHERAL, BLEW_ROLE_CENTRAL } blew_role_t;

typedef enum {
    BLEW_CLOSE_TIMEOUT = 1,    /* max_ms 到了 */
    BLEW_CLOSE_BT_ERROR,       /* NimBLE host 复位等，窗口已不可用 */
} blew_close_reason_t;

/* owner 收到 on_expire 后多久还没 ble_window_close()，ble_window 就强制关（拆 BT）。要大于 owner 收尾的最坏时长：
   muse_pair 等 TX 锁（≤ 15 s）+ 等在途的 commit（落盘 + 读回，≤ 约 20 s），不然强制关的那一刻 owner 可能还在调 NimBLE */
#define BLEW_OWNER_GRACE_MS 40000

struct ble_gatt_svc_def;

/* 设计 3.1 的 blew_req_t 只有 owner / role / max_ms。GATT 服务必须在 nimble_port_init() 与 host 任务启动之间注册，
   而那一段在 ble_window 的任务里跑，所以 owner 的服务表与 host 回调随请求一起交进来（2026-10-22 实现时加）。 */
typedef struct {
    blew_owner_t owner;
    blew_role_t  role;
    uint32_t     max_ms;                        /* 硬上限：Muse ≤ 600 s；Matter ≤ 120 s */
    const struct ble_gatt_svc_def *svcs;        /* 外设角色必填；窗口开着期间必须一直有效 */
    const char  *device_name;                   /* GAP 名；窗口开着期间必须一直有效 */
    void (*on_sync)(void);                      /* host 同步好了（NimBLE host 任务里）：owner 在这里开广播 */
    void (*on_reset)(int reason);               /* host 复位（host 任务里）：窗口随后按 BT_ERROR 关掉 */
    /* 窗口到时 / BT 出错（ble_window 任务里回调）：此刻 BT **还没拆**。owner 只该投递给自己的任务，由它先停掉自己的 GATT
       收发（不再碰 NimBLE），再调 ble_window_close()。BLEW_OWNER_GRACE_MS 内不关，ble_window 强制关 —— 先拆 BT 再通知
       会让 owner 在途的 NimBLE 调用打到已经拆掉的栈上（2026-10-22 审计）。 */
    void (*on_expire)(blew_close_reason_t why);
    /* 宽限期到了、要强制拆 BT 之前（ble_window 任务里**同步**调，可阻塞 ≤ 15 s）：owner 必须保证此后自己不再碰 NimBLE
       （muse_pair 用它调 pair_ble_mark_dead）。拆完之后 NimBLE 的状态指针是 NULL（BT_NIMBLE_STATIC_TO_DYNAMIC），再碰就 panic。 */
    void (*on_force_close)(void);
} blew_req_t;

typedef struct {
    bool     ok;
    bool     thread_paused;      /* 策略 C 不暂停 Thread（这次开机根本没起 Thread） */
    uint32_t thread_paused_ms;
    int      selfcheck;          /* 0 = 通过，>0 = 退化（3.3），-1 = 没做（策略 C 的自检在重启回正常开机后由 ble_window_selfcheck_start 做） */
} blew_result_t;

typedef struct {
    const char   *state;                 /* "idle" / "bt_init" / "open" / "bt_deinit" / "bt_stuck"（host 停不下来，等重启） */
    blew_policy_t policy;
    bool          pair_boot;             /* 这次开机是配对剖面 */
    bool          bt_mem_released;       /* 正常开机已把 BT 的静态内存还给堆 */
    uint32_t      opened_ms;             /* 当前窗口开了多久（没开为 0） */
    uint32_t      internal_free_at_open; /* 开窗前的内部 / DMA 空闲（TC-BW04 看窗口吃了多少） */
    uint32_t      dma_free_at_open;
    uint32_t      internal_min_open;     /* 窗口开着期间内部空闲的最低点 */
    int           last_err;              /* 最近一次 open / close 失败的 esp_err_t（0 = 没有） */
    uint32_t      host_stack_free;       /* 上一个窗口里 NimBLE host 任务栈最紧时还剩多少（字节；0 = 还没量到） */
    const char   *selfcheck;             /* BLE 窗口之后的 Thread 自检：none / pending / pass / degraded_* / inconclusive / no_thread / interrupted */
    uint16_t      sc_before_pm, sc_after_pm;          /* 上一次自检的 ACK 比例（千分数） */
    uint8_t       sc_children_before, sc_children_after;
    uint8_t       sc_consecutive_degraded;
    uint16_t      sc_total;
} blew_status_t;

/* 开一个窗口。只允许一个；忙时 ESP_ERR_INVALID_STATE；参数不对 ESP_ERR_INVALID_ARG；内存闸门不过 ESP_ERR_NO_MEM；
   眼下不支持的组合（见文件头）ESP_ERR_NOT_SUPPORTED；BT 起不来回那次失败的错误码。 */
esp_err_t ble_window_open(const blew_req_t *req);
esp_err_t ble_window_scan_begin(void);               /* 中心角色（P3）：ESP_ERR_NOT_SUPPORTED */
esp_err_t ble_window_scan_end(void);                 /* 同上 */
esp_err_t ble_window_resume_thread_early(void);      /* Matter 时序（9.4，P3）：ESP_ERR_NOT_SUPPORTED */
/* 关窗口（含 BT 反初始化）。没开着时返回 ESP_OK、out->ok = true。out 可为 NULL。 */
esp_err_t ble_window_close(blew_result_t *out);
blew_policy_t ble_window_effective_policy(void);
void ble_window_get_status(blew_status_t *out);

/* ---- 策略 C ---- */

/* app_main 最先调用（在"非干净复位补重启"之前）：读 RTC 一次性标志并**无条件清掉**，返回这次开机该不该进配对剖面
   （标志合法 + 原始复位原因是 ESP_RST_SW）。安全模式要等 watchdog_init() 之后才知道，所以由 main.c 再与
   !wd_safe_mode() 相与；之后调 ble_window_set_pair_boot() 定案。 */
bool ble_window_take_pair_boot(void);
void ble_window_set_pair_boot(bool active);
bool ble_window_pair_boot_active(void);

/* 正常开机请求进配对剖面：只接受 BLEW_OWNER_MUSE_PAIR。写标志、记一笔计划内重启（muse_pair_enter，只记 RTC）、esp_restart()。
   成功不返回；参数不对 ESP_ERR_INVALID_ARG，已经在配对剖面里 ESP_ERR_INVALID_STATE。调用方要先把 HTTP 回包送出去
   （esp_restart 前 shutdown handler 会落盘 hubdb / config）。 */
esp_err_t ble_window_request_isolated(blew_owner_t owner);

/* 正常开机（不是配对剖面）里调一次：把 BT 的静态内存还给堆。必须在任何东西碰 BT 之前。 */
void ble_window_release_bt_mem(void);

/* 正常开机里、hub_store 起来之后调一次（main.c）：刚从配对剖面回来就起 Thread 自检任务（设计 3.3，TC-MU05），
   否则只把上次存的结果读出来给状态页（返回 ESP_ERR_NOT_FOUND）。配对剖面里返回 ESP_ERR_INVALID_STATE。 */
esp_err_t ble_window_selfcheck_start(void);
