/* matter_ctrl —— 直连型的 Matter 控制器（docs/15_muse_direct_design_v1.md 第九节；M7）。
 * esp-matter 的 client-only 控制器 + commissioner，用网关自己的 fabric；不带 server、不带 BLE、不接管 OpenThread（9.2）。
 * M7 第一步：起控制器、状态；第二步：自己的凭据签发器（mc_issuer.h）与 fabric 身份（hubdb 正本）。配网 / 访谈 / 订阅 / 命令 / 移除在后面几步。
 * HA 型里本组件是空的：调用方一律放在 #if CONFIG_BITENGINE_ENABLE_MATTER_CTRL 里。 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL

typedef enum {
    MATTER_CTRL_OFF = 0,      /* 还没调 matter_ctrl_start() */
    MATTER_CTRL_WAITING,      /* 开机晚起：等 SRP 重注册潮过去 */
    MATTER_CTRL_STARTING,     /* 正在起 CHIP / commissioner */
    MATTER_CTRL_RUNNING,
    MATTER_CTRL_FAILED,       /* 起到一半失败（原因见 fail_step / fail_err），这次开机不再试 */
    MATTER_CTRL_HELD,         /* 防崩溃循环：前几次开机起了 Matter 都没撑过稳定期，这次不起（断电重上电清零） */
} matter_ctrl_state_t;

typedef struct {
    matter_ctrl_state_t state;
    const char *fail_step;    /* FAILED 时是哪一步（静态字符串）；否则 NULL */
    int32_t fail_err;         /* 那一步的错误码（esp_err_t 或 CHIP 错误的整数值） */
    uint32_t tries;           /* 防崩溃循环的计数（连续多少次开机起了 Matter 没撑过稳定期） */
    bool ident_new;           /* 这次开机新建了 fabric 身份（hubdb 里没有）—— 之前配进来的设备（若有）都成了孤儿 */
    bool matter_rebound;      /* matter 分区与身份对不上（新身份 / 分区坏了 / 装过别的固件），这次开机擦掉重建了 CHIP 的状态；身份没变时设备不受影响 */
    uint64_t fabric_id;       /* 我们 fabric 的 ID（9.3：随机 64 位）；还没建时 0 */
    uint64_t compressed_fabric_id;  /* 运行态记录 _matter._tcp 实例名的前半段（<cfid>-<nodeid>）；RUNNING 之前 0 */
    uint64_t node_id;         /* 控制器自己的节点号 */
    uint16_t vendor_id;       /* 写进 fabric 的管理方厂商号（CONFIG_ESP_MATTER_CONTROLLER_VENDOR_ID） */
    uint32_t started_ms;      /* 进入 RUNNING 时的开机毫秒数；没进过 0 */
    uint32_t heap_before;     /* 起 CHIP 前 / 后的内部 SRAM 空闲（看 CHIP 的常驻开销） */
    uint32_t heap_after;
    bool     nodes_ok;        /* 节点表开机读回了（或确实没有）；false 时这次开机不配网、不移除 */
    uint8_t  nodes;           /* 节点表里的节点数（含配网中的） */
    bool     busy;            /* 正在配网或移除一台设备（同一时刻只做一件） */
    uint8_t  subs;            /* 该订阅的设备数（访谈完了的） */
    uint8_t  subs_up;         /* 其中订阅现在建好着的 */
    bool     allow_test_devices;   /* "允许测试设备"开着（9.7、10.1；只在本地改） */
} matter_ctrl_status_t;

/* ---- 控制（M7 第六步；设计 9.5「命令」）：开 / 关 / 切换 / 亮度（动作号 MATTER_CTRL_ACT_*，在 matter_ctrl_live.h） ---- */
typedef enum {
    MATTER_CTRL_CTL_CONFIRMED = 1,  /* 设备回了成功，3 s 内订阅报回了目标值（或本来就是目标） */
    MATTER_CTRL_CTL_DELIVERED,      /* 设备回了成功，没等到报告 */
    MATTER_CTRL_CTL_FAILED,         /* 见 err */
} matter_ctrl_ctl_result_t;
typedef enum {
    MATTER_CTRL_CTL_E_OK = 0,
    MATTER_CTRL_CTL_E_NO_NODE,      /* 没有这台 / 还没访谈完（不知道往哪个端点发） */
    MATTER_CTRL_CTL_E_NO_CLUSTER,   /* 没有对应的簇（亮度 0 % 而没有开关簇也归这里） */
    MATTER_CTRL_CTL_E_UNREACHABLE,  /* 连不上 / 没发出去（含连接还没建好就到了兜底时间）：一定没执行 */
    MATTER_CTRL_CTL_E_REFUSED,      /* 设备回了错误状态（im_status） */
    MATTER_CTRL_CTL_E_NO_ANSWER,    /* 发出去了、没等到回应：可能已经执行（别让人重发，切换会切两次） */
} matter_ctrl_ctl_err_t;
typedef struct {
    uint8_t  result;                /* matter_ctrl_ctl_result_t */
    uint8_t  err;                   /* matter_ctrl_ctl_err_t */
    uint8_t  im_status;             /* REFUSED 时设备回的 IM 状态码 */
    uint32_t ms;                    /* 从发起到结论（confirmed 是到报告） */
} matter_ctrl_ctl_t;
/* 同步：在调用方任务里等结论，最多约 13.5 s（连接 + 回应 9 s 兜底、CHIP 任务排队最多 1.5 s，再等 3 s 报告）。不许在 CHIP 任务 / esp_timer 回调里调。
   返回 ESP_OK（结论在 out）/ ESP_ERR_INVALID_STATE（控制器没在跑）/ ESP_ERR_TIMEOUT（另一条控制正在做：busy）/ ESP_ERR_INVALID_ARG */
esp_err_t matter_ctrl_control(uint64_t node, int act, uint8_t level_pct, matter_ctrl_ctl_t *out);

/* ---- 配网与移除（M7 第三步；设计 9.4 B、10.1 remove） ---- */

/* 一次配网 / 移除的结果。err_code 与 msg 面向调用方：msg 是一句用户能照做的英文（Muse 原样转述，CHECKLIST MC23） */
typedef struct {
    bool        ok;
    uint64_t    node_id;      /* 配网成功：新节点号；移除：被移除的节点号 */
    uint16_t    vid, pid;     /* 配网码里带的（长码 / QR），没有是 0 */
    const char *err_code;     /* 失败：not_found / code_rejected / attestation_failed / timeout / unreachable / storage_error / internal */
    char        msg[200];
} matter_ctrl_result_t;
/* 完成回调：恰好调一次，在 CHIP 任务里调 —— 只许拷贝 / 投递，别阻塞、别调回 matter_ctrl */
typedef void (*matter_ctrl_done_cb_t)(void *ctx, const matter_ctrl_result_t *r);

/* 在网配网（设备已经在别的生态里，对方 App 开了分享窗口）。异步：受理返回 ESP_OK，之后 cb 恰好调一次。
 * 不受理（cb 不会调）时 why 写一句英文原因：
 *   ESP_ERR_INVALID_ARG    码的形状 / 校验和不对
 *   ESP_ERR_NOT_SUPPORTED  只能经蓝牙配的新设备码（P3 不做 BLE 配网）
 *   ESP_ERR_INVALID_STATE  控制器没在跑，或正在配 / 删另一台
 *   ESP_ERR_NO_MEM         节点表满了（BITENGINE_MATTER_MAX_NODES）
 * 任何任务里可调（解析码是纯计算；节点号的分配与落盘在 CHIP 任务里做）。 */
esp_err_t matter_ctrl_commission(const char *code, matter_ctrl_done_cb_t cb, void *ctx, char *why, size_t why_len);
/* 移除一台（只在本地，设计 10.1）：先给设备发 RemoveFabric，设备确认了才删节点表。force = 设备不答话也从网关这边删掉
 * （设备上还留着网关的 fabric，要用户在它自己的 App 里重置）。异步，语义同上；节点号不存在回 ESP_ERR_NOT_FOUND */
esp_err_t matter_ctrl_remove(uint64_t node_id, bool force, matter_ctrl_done_cb_t cb, void *ctx, char *why, size_t why_len);

typedef struct {
    uint64_t node_id;
    uint16_t vid, pid;
    bool     commissioned;    /* false = 正在配网 */
} matter_ctrl_node_t;
/* 节点表快照（按表顺序，最多 max 条）；返回条数，没起来 0 */
int matter_ctrl_nodes(matter_ctrl_node_t *out, int max);

/* 一台的访谈结果（拷一份；类型在 matter_ctrl_iv.h）。没有这台返回 false；访谈没完成时 flags 没有 MC_IV_F_DONE */
struct mc_iv_s;
bool matter_ctrl_node_iv(uint64_t node, struct mc_iv_s *out);
bool matter_ctrl_node_iv_busy(uint64_t node);     /* 正在访谈它 */

/* fabric 身份（根 CA 密钥、fabric ID、IPK、根证书；格式 mc_ident_core.h）的存取。hub_core 用 hub_store 实现（hubdb/mctrl），
 * 在 matter_ctrl_start 之前登记（hub_svc_start）—— matter_ctrl 不依赖 hub_core（同 zhub_set_hooks）。
 * 在启动任务 / CHIP 任务里调，可以阻塞（同步等 hub_store），两者都是内部栈。⚠️ buf 里是秘密：实现不许打日志、不许留拷贝。 */
typedef enum {
    MATTER_CTRL_LOAD_OK = 0,
    MATTER_CTRL_LOAD_EMPTY,   /* 确实没有（新网关、恢复出厂全清过）。matter 分区里还记着以前绑过身份时，matter_ctrl 不新建、停在 ident_lost */
    MATTER_CTRL_LOAD_ERR,     /* 有但读不出来（hub_store 没起来 / 超时 / 格式比固件新）：不许当成"没有"去新建 */
} matter_ctrl_load_t;
typedef struct {
    matter_ctrl_load_t (*ident_load)(uint8_t max_ver, void *buf, size_t *len_inout);
    esp_err_t (*ident_save)(uint8_t ver, const void *buf, size_t len);   /* 落盘并读回比对一致才回 ESP_OK */
    /* 节点表（hubdb mctrl/nodes；格式 mc_nodes_core.h）：同上的语义。开机在启动任务里读一次，之后只在 CHIP 任务里写 */
    matter_ctrl_load_t (*nodes_load)(uint8_t max_ver, void *buf, size_t *len_inout);
    esp_err_t (*nodes_save)(uint8_t ver, const void *buf, size_t len);
    /* 每台的访谈结果（hubdb mctrl/iv<十进制节点号>；格式 mc_iv_core.h，M7 第四步）。save 不必读回确认（丢了下次开机再访谈） */
    matter_ctrl_load_t (*iv_load)(uint64_t node, uint8_t max_ver, void *buf, size_t *len_inout);
    esp_err_t (*iv_save)(uint64_t node, uint8_t ver, const void *buf, size_t len);
    void (*iv_erase)(uint64_t node);
    /* 设备层事件（CHIP 任务里调：只许投递 / 拷贝，别阻塞） */
    void (*event)(uint64_t node, int ev, unsigned bits);   /* ev：MATTER_CTRL_EV_*；bits 只对 EV_STATE 有意义（MC_SEV_*） */
    /* "允许测试设备"存的值（启动任务里、起 CHIP 之前读一次）；没有 / 读不出来回 false（安全的一边） */
    bool (*allow_test_load)(void);
} matter_ctrl_hooks_t;
enum {
    MATTER_CTRL_EV_ADDED = 1,      /* 配网刚成功时排的那次访谈做完了（有了厂家 / 型号 / 能力）：hub_core 推"加好了"。访谈里只此一种会推 */
    MATTER_CTRL_EV_INTERVIEWED,    /* 其余的访谈做完（开机补访谈、记录读不回来重做、以后的重新访谈）：只进事件环 */
    MATTER_CTRL_EV_ONLINE,         /* 离线之后订阅又建好了（开机头一次连上不算） */
    MATTER_CTRL_EV_OFFLINE,        /* 订阅断了、超过 2 × 最大间隔没听到它（9.5） */
    MATTER_CTRL_EV_STATE,          /* 读数翻转：bits = MC_SEV_*（门磁开合、人体、漏水、烟雾、电量低；mc_sub_core.h），新值看 matter_ctrl_node_live */
};
void matter_ctrl_set_hooks(const matter_ctrl_hooks_t *hooks);

/* 起控制器（异步：开机晚起、CHIP 任务里建 commissioner，结果看 matter_ctrl_get_status）。
 * 重复调用只起一次。返回 ESP_ERR_INVALID_STATE = 防崩溃循环挡住了（HELD）。 */
esp_err_t matter_ctrl_start(void);
/* 恢复出厂全清（设计第十二节）用：删掉 matter 分区里的绑定记号。先删它、再删 hubdb 的身份（hub_store.c 的 config_mgr_hubdb_factory_reset）——
   中途断电最坏是"身份还在、记号没了"，下次开机按身份重绑、设备照旧；反过来会停在 ident_lost。任何任务里可调（只是一次 NVS 删键），不碰 CHIP */
esp_err_t matter_ctrl_forget_binding(void);
/* "允许测试设备"（9.7、10.1；只给本地接口用，不给 Muse）：立即生效（信任库多认 CHIP 的测试 PAA、校验器接受 CSA 测试密钥签的 CD）；
   存不存由调用方（hub_matter）管。任何任务里可调 */
esp_err_t matter_ctrl_set_allow_test_devices(bool on);
void matter_ctrl_get_status(matter_ctrl_status_t *out);
const char *matter_ctrl_state_str(matter_ctrl_state_t st);

#endif /* CONFIG_BITENGINE_ENABLE_MATTER_CTRL */

#ifdef __cplusplus
}
#endif
