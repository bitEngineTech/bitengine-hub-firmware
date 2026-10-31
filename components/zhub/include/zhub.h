#pragma once
/* zhub —— 直连型的 Zigbee 主机（docs/15_muse_direct_design_v1.md 第八节）。只在 CONFIG_BITENGINE_ENABLE_ZHUB 下编（HA 型没有这个任务）。
 *
 * 一个任务（"zhub"，CHECKLIST Z60：设计 8.2 原来的 zh_ash 与 zh_main 合并）：持有 MG24 的 UART，跑编排层 zh_host_core（ASH + EZSP 会话 +
 * 复位 / 握手 / networkInit / 退避，规则与数值见 zh_host_core.h）。别的任务只经下面这几个函数影响它，全部是"投递请求 / 读快照"，不碰它的状态。
 *
 * UART 归属（8.1、K9 K10 K11）：zhub 运行时桥的状态是 BRIDGE_STATE_ZHUB。MG24 升级、UART 基准测试、ncp_resync、bootloader 进出都照旧调
 * ezsp_bridge_ota_claim()；直连型里它先经登记的钩子调 zhub_suspend()（zhub 收摊、归还 UART、不再自己拿回去），ezsp_bridge_ota_release()
 * 之后调 zhub_resume()（zhub 重新接管、从 ASH RST 走一遍）。所以这些调用方一行都不用改。 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "zh_dev.h"

typedef struct {
    bool        started;          /* 任务在跑 */
    bool        holding_uart;     /* 现在持有 UART（BRIDGE_STATE_ZHUB） */
    bool        suspended;        /* 有人要 UART：zhub 已让出 / 正在让出 */
    const char *state;            /* zh_host_state_name()：idle / resetting / handshake / net_init / running / backoff */
    const char *net;              /* zh_host_net_name()：unknown / up / down / none */
    const char *last_fail;        /* zh_host_fail_name() */
    uint32_t    last_fail_code;
    uint32_t    fails;            /* 连续失败次数（退避表的下标） */
    bool        failing;          /* 连续失败到 ZH_HOST_FAILING_AT 次 */
    uint32_t    last_net_status;  /* 最近一次 networkInit / stackStatusHandler 的 sl_status_t */
    bool        have_version;
    uint8_t     stack_type;
    uint16_t    stack_version;
    bool        have_params;      /* getNetworkParameters 读到过（这一轮链路里） */
    uint8_t     channel, node_type;
    int8_t      tx_power;
    uint16_t    pan_id;
    uint8_t     ext_pan_id[8];
    bool        have_eui64;
    uint8_t     eui64[8];         /* 协调器自己的 EUI64（getEui64 读回，小端 = 线上字节序） */
    bool        concentrator;     /* 这次 UP 之后集中器（setConcentrator + 发现模式）设好了 */
    uint32_t    up_bad;           /* UP 之后的序列没成的次数（各步合计；尽力而为、会重试） */
    uint32_t    init_opt_rejects; /* networkInit 之前的可选配置被 NCP 拒的次数（0 = MAC 间接发送超时等都收下了） */
    uint32_t    incoming;         /* 收到的 incomingMessageHandler（设备发给网关的 APS 消息：上报、应答、ZDO） */
    unsigned    devices;          /* 设备表里的条数 */
    zh_dev_stats_t dev_st;        /* 设备层计数（被动学习、表满…） */
    uint8_t     disc_state;       /* 主动发现（M6 第一步）：zh_disc_state_t */
    uint8_t     disc_q_n, disc_q_i;   /* 排进来的路由器 / 爬到第几台 */
    uint32_t    incoming_bad;     /* 解不开的 incomingMessageHandler */
    int8_t      bad_err;          /* 最近一条解不开的：解码错误码（zh_ezsp_err_t） */
    uint8_t     bad_fc, bad_fc_hi;
    uint8_t     bad_n;            /* 参数部分的字节数 */
    uint8_t     bad_head_n;
    uint8_t     bad_head[31];     /* 参数的前 31 字节：按 SDK 布局是消息头（类型 / APS / 接收信息 / 长度），不到载荷 */
    bool        have_last_rx;     /* 最近一条消息的来源（板上验"设备上报到了"用；载荷不留） */
    uint16_t    last_rx_node, last_rx_profile, last_rx_cluster;
    uint8_t     last_rx_ep, last_rx_lqi;
    int8_t      last_rx_rssi;
    uint32_t    last_rx_age_ms;
    /* 入网窗口（zh_host_core.h 文件头"入网窗口"一段） */
    const char *win_state;        /* zh_host_win_name()：closed / opening / open / closing */
    uint32_t    win_left_s;       /* 开着时还剩几秒 */
    uint8_t     win_s;            /* 这次开的时长 */
    uint32_t    win_quota_left_s; /* 滚动一小时里还能开多少秒（总时长上限） */
    bool        ncp_open;         /* NCP 报过 NETWORK_OPENED（真开了） */
    uint32_t    win_opens, win_open_fails, win_closes, win_expired, win_close_retries, win_skipped, win_last_err;
    /* 入网观察（trustCenterPostJoinHandler 与 ZDO Device_annce；记录 / 访谈随第七步） */
    uint32_t    joined, rejoined, left, denied, annces, tc_bad;
    bool        have_last_join;
    uint16_t    last_join_node, last_join_parent;
    uint8_t     last_join_eui64[8];   /* 小端 = 线上字节序 */
    uint8_t     last_join_status, last_join_decision;
    uint32_t    last_join_age_ms;
    bool        have_evidence;    /* 这次接管 UART 以来有过正向证据（配上的应答 / 合法回调） */
    uint32_t    evidence_age_ms;  /* 距最近一次正向证据；没有证据时距接管 UART */
    uint32_t    resets, readies, net_inits, callbacks, restarts;
    uint32_t    ash_tx_frames, ash_rx_frames, ash_retx, ash_crc, ash_ack_timeouts, ash_ncp_resets, ash_ncp_errors;
    uint32_t    ses_timeouts, ses_echoes, ses_rejected, ses_overflow, ses_lost_callbacks, ses_keepalives;
    uint32_t    uart_tx_short;    /* UART 写不全（流控卡住超过写超时） */
} zhub_status_t;

/* 建任务并登记 UART 让出钩子（只调一次；安全模式不调）。任务起来后自己认领 UART、复位 NCP、networkInit。 */
esp_err_t zhub_start(void);
/* 让 zhub 交出 UART（计数：每次成功的 suspend 配一次 resume）。同步：等到 zhub 已归还才返回，最多 timeout_ms；
   超时回 ESP_ERR_TIMEOUT 并撤回这次请求（不用再 resume）。zhub 没起时立即回 ESP_OK（照样计数）。不许在 zhub 任务里调。 */
esp_err_t zhub_suspend(uint32_t timeout_ms);
/* 撤回一次 suspend；计数回到 0 时 zhub 重新接管 UART（从 ASH RST 走一遍）。 */
void      zhub_resume(void);
/* 立即复位链路（跳过退避、不计失败；/zigbee/restart、看门狗的定向恢复）。没起 / 没持有 UART 时回 ESP_ERR_INVALID_STATE；
   离上一次被接受的不到 ZHUB_RESTART_MIN_MS（zhub.c）回 ESP_ERR_NOT_ALLOWED。只投递请求，不等结果。 */
esp_err_t zhub_request_restart(void);
/* 读快照（任务每圈更新一次，加锁拷贝）。 */
void      zhub_get_status(zhub_status_t *out);
/* 只要"任务在跑"这一位（started）：给不想在栈上拷整个 zhub_status_t（约 420 B）的调用方，如 Muse 会话任务（PSRAM 栈 12 KB；C17 审计）。
   started 只会 false → true 一次（任务起来后第一次快照；软重启 / 让出 UART / 挂起都不改它） */
bool      zhub_started(void);

/* 入网窗口：seconds 0 = 关，30..254 由调用方校验（附录 A；这里只拒 > 254）。投给 zhub 任务拿受理结果：拿锁、等结果各最多 ZHUB_PJ_WAIT_MS
   （zhub.c，500 ms），最坏约 1 s（httpd / Muse 会话任务里调，不能久等）。返回 ESP_OK 时 out 有效；zhub 没起 / 没持有 UART 回 ESP_ERR_INVALID_STATE；等不到回 ESP_ERR_TIMEOUT。
   不许在 zhub 任务里调。 */
typedef struct {
    int      result;              /* zh_pj_result_t */
    const char *win_state;        /* 处理完之后的窗口状态 */
    uint32_t left_s;              /* 开着时还剩几秒 */
    uint32_t seconds;             /* 这次开的时长（可能被总时长上限截短） */
    uint32_t retry_s;             /* 回 ZH_PJ_QUOTA 时：多少秒后能再开 */
} zhub_pj_t;
esp_err_t zhub_permit_join(uint32_t seconds, zhub_pj_t *out);

/* 控制命令（设计 8.11；M5 第七步；规则见 zh_dev.h 的控制一节）。act = zh_ctl_act_t，level_pct 只给 ZH_CTL_LEVEL。
   同步：投给 zhub 任务、等它有结论（最多约 ZH_CTL_DEADLINE_MS + 2 s，httpd / Muse 会话任务里调）。返回 ESP_OK 时 out 有效：
   err != ZH_CTL_E_OK = 没发出去（原因）；否则 result = zh_ctl_result_t。zhub 没起 / 没持有 UART 回 ESP_ERR_INVALID_STATE；
   抢不到（上一条还在等）回 ESP_ERR_TIMEOUT —— 没发，可以再试；已经交给 zhub 任务却等不到结论回 ESP_ERR_NOT_FINISHED —— 可能发出去了，别重发 */
typedef struct {
    uint32_t id;                  /* 请求号（内部配对用） */
    int      err;                 /* zh_ctl_err_t */
    uint8_t  result;              /* zh_ctl_result_t */
    uint8_t  zcl_status;          /* FAILED 且设备回了错误状态时 */
    uint32_t status;              /* FAILED 且 NCP 拒了 / 没送达时 */
    uint32_t ms;                  /* 发出到有结论 */
} zhub_ctl_t;
esp_err_t zhub_control(const uint8_t ieee[8], uint8_t act, uint8_t level_pct, zhub_ctl_t *out);

/* ---- 设备层（M4 第七步 a；规则见 zh_dev.h） ----
   存取与事件经钩子：hub_core 在 zhub_start 之前登记（zhub 不依赖 hub_core，两个组件不互相依赖）。钩子在 zhub 任务里调（PSRAM 栈：
   存取要经 hub_store 的队列，不许直接碰 flash）；load 只在开机加载时调，save 只在结构变化时调（调用方防抖），event 不许阻塞太久（≤ 100 ms）。 */
typedef struct {
    zh_dev_rec_t rec;             /* 记录本身（IEEE 小端 = 线上字节序；manuf / model 是设备自报的原样字节，输出前要按 6.8 净化） */
    uint8_t  iv;                  /* 访谈进度 zh_iv_step_t（ZH_IV_NONE 且 rec.flags 带 INTERVIEWED = 已完成） */
    bool     iv_started;          /* 访谈的请求发出去过（列表里区分"等着"与"进行中"） */
    bool     seen;                /* 这次开机以来收到过它的消息 */
    bool     offline;             /* 可用性（8.10）：判过离线、还没再收到它的消息（M5 第十一步） */
    uint32_t age_ms;              /* 距最近一次收到（没收到过：距开机加载）；zhub_dev_list 在拷的那一刻算，最多 ZH_DEV_AGE_CAP_MS */
    uint32_t t_seen;              /* zhub 内部用（快照里存时刻、读的时候才算年龄：没有新消息时快照不重发，年龄也得往前走） */
    zh_state_t st;                /* 读数（M5 第一步）；st.t[] 是 zhub 的时刻，用下面的年龄 */
    uint32_t st_age_ms[ZH_ST_N];  /* 每个读数的年龄：zhub_dev_list 拷的那一刻算（同 age_ms） */
    uint8_t  st_ev;               /* ZH_DEV_EV_STATE 时发生了什么（ZH_STEV_*；只在事件钩子里有意义） */
    int8_t   rssi;
    uint8_t  lqi;
    uint32_t rx;
} zhub_dev_view_t;
typedef enum {
    ZHUB_LOAD_EMPTY = 0,          /* 这一格没有记录（或存储整个没起来：反正也写不进去，不必保留） */
    ZHUB_LOAD_OK,                 /* 读到了，*out 有效 */
    ZHUB_LOAD_ERR,                /* 有东西但读不出来（超时 / 读错 / 格式比固件新）：这次开机保留这一格，不分配给新设备 */
} zhub_load_t;
typedef struct {
    zhub_load_t (*load)(unsigned idx, zh_dev_rec_t *out);                  /* 开机加载第 idx 条 */
    void (*save)(unsigned idx, const zh_dev_rec_t *rec);
    void (*event)(zh_dev_ev_t ev, zh_dev_src_t src, const zhub_dev_view_t *dev);
} zhub_hooks_t;
void zhub_set_hooks(const zhub_hooks_t *hooks);
/* 拷一份设备表（最多 max 条，按表下标顺序）；返回条数，zhub 没起返回 0，锁 100 ms 没拿到返回 -1（调用方回"忙"，别当成没有设备）。
   快照最多旧 ZHUB_DEV_PUB_MS（zhub.c）；age_ms 在拷的那一刻算 */
int zhub_dev_list(zhub_dev_view_t *out, size_t max);

/* 看门狗的 MG24 探针用（wd_probes.c 里是弱符号的默认实现：zhub 没编进来就"不判定"，Z47）。 */
typedef struct {
    bool     active;              /* zhub 正持有 UART 在跑（挂起 / 没起时 false：取不到证据） */
    bool     evidence;            /* 这次接管以来有过正向证据 */
    uint32_t age_ms;              /* 距最近一次正向证据；没有证据时距接管 UART（判死计时从这里起算） */
} zhub_mg24_view_t;
bool      zhub_mg24_evidence(zhub_mg24_view_t *out);
/* 看门狗 PERIPH_DEAD 的定向恢复（L2）：请 zhub 立即复位链路。只投递请求，不阻塞。 */
esp_err_t zhub_wd_recover(void);
/* NCP 侧诊断（只读，/api/v1/zigbee/ncp_diag）：NCP 自己的计数器（readCounters：NCP 上电以来累计，16 位封顶不回绕）与子设备表。
   定位"入网放行了、之后一帧都收不到"这类断在 NCP 里的问题（2026-10-23 小米重新配对）。zhub_ncp_diag_request 投递一次重读
   （计数器 + 安全状态 + 扫 ZHUB_NCP_CHILD_SCAN 格子设备表，一条在途、约 1 s 读完）；没持有 UART 回 ESP_ERR_INVALID_STATE。zhub_ncp_diag_get 拷最近读完的那份。 */
#define ZHUB_NCP_CTR_N      42u   /* = ZH_EZSP_COUNTER_N（zhub.c 静态断言） */
#define ZHUB_NCP_CHILD_SCAN 64u   /* 扫子设备表的格数（NCP 直连子设备上限 64，mg24/ 的 NCP 配置） */
#define ZHUB_NCP_CHILD_MAX  16u   /* 最多报这么多台（再多记 child_more） */
typedef struct {
    uint8_t  eui64[8];
    uint16_t id;
    uint8_t  type, timeout;       /* type：2 终端 / 3 休眠终端；timeout：终端超时档 */
    uint32_t remaining;           /* 离超时还剩（NCP 报的原值） */
} zhub_ncp_child_t;
typedef struct {
    bool     valid;               /* 读完过一份 */
    bool     busy;                /* 正在读 */
    uint32_t age_ms;              /* 距那份读完 */
    uint32_t reads, fails;        /* 读完 / 中途作废的次数 */
    uint16_t ctr[ZHUB_NCP_CTR_N];
    uint16_t sec_bitmask;         /* getCurrentSecurityState 的 bitmask（sl_zigbee_types.h:2027-2060；0x0084 = 信任中心用散列链路密钥） */
    uint8_t  n_child;
    bool     child_more;
    zhub_ncp_child_t child[ZHUB_NCP_CHILD_MAX];
} zhub_ncp_diag_t;
esp_err_t   zhub_ncp_diag_request(void);
void        zhub_ncp_diag_get(zhub_ncp_diag_t *out);
const char *zhub_ncp_counter_name(unsigned i);   /* 计数器的短名（SDK sl_zigbee_types.h 的 SL_ZIGBEE_COUNTER_* 顺序）；越界回 NULL */
/* 故障注入（TC-ZH07，只在 CONFIG_BITENGINE_FAULT_INJECTION 构建里有定义）：下一圈绕过 ASH 状态机往 UART 写一帧 RST，
   模拟 NCP 自己重启 —— NCP 回 RSTACK，连着的 ASH 按"NCP 复位"处理（ZH_HOST_FAIL_NCP_RESET）、走正常恢复。
   不碰 IO23。没持有 UART 时回 ESP_ERR_INVALID_STATE；只投递，不等结果。 */
esp_err_t zhub_fault_ash_rst(void);
