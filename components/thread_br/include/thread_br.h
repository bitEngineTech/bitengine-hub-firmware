#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t thread_br_init(void);
esp_err_t thread_br_start(void);
esp_err_t thread_br_stop(void);
bool      thread_br_is_running(void);
esp_err_t thread_br_pause(void);
esp_err_t thread_br_resume(void);
void      thread_br_lower_priority(void);   /* OTA: reduce CPU contention */
/* 802.15.4 发射功率按地区（docs/14 第 7.5 节）：cc 是 radio_region.h 菜单里的码或 ""。
   region_tx_dbm 是纯映射（REST 回包用）；get_tx_power_dbm 是最近一次成功请求的值，
   INT8_MAX = 还没设过。都不是射频实测（回读不是证据，见 docs/hardware/射频功率与认证.md 规矩 3）。 */
int8_t    thread_br_region_tx_dbm(const char *cc);
int8_t    thread_br_get_tx_power_dbm(void);

/* ---- 信道：配置值与实际值是两回事 ----
 *
 * configured = NVS 里的 thread.channel，**只在首次建网时**作为默认值参考。
 * active     = 射频当前真正工作的信道，来自 otLinkGetChannel()。
 *
 * 两者会分叉：dataset 一旦建立就存在 OT 自己的 NVS 里，HA / 其他 BR 推来的
 * 数据集（含信道迁移）会改掉 active 而完全不碰 NVS 配置。以前只有一个
 * thread_br_get_channel() 返回配置值，却被 /node/diagnostics 当"实际运行信道"
 * 报出去 —— 迁移过的板子会一直报旧值。
 *
 * get_active 未运行返回 ESP_ERR_INVALID_STATE，抢不到 OT 锁返回 ESP_ERR_TIMEOUT；
 * 拿不到时**不要**拿 configured 顶替后当实际值报，那正是要修掉的毛病。
 */
uint8_t   thread_br_get_configured_channel(void);
esp_err_t thread_br_get_active_channel(uint8_t *out_channel);

/* ---- Zigbee / Thread 共存间隔 ----
 *
 * 板上是两颗独立射频（Zigbee 在 MG24，Thread 在 C5 自带的 802.15.4），天线在
 * 同一块 PCB 上。危害不是"环境干扰"而是自家两个发射机互相压：邻道抑制只有
 * 几十 dB，而近端发射最高 +20 dBm、远端传感器回波可能只有 -85 dBm，差 100 dB
 * 以上；另外对方一发射就抬高本方测到的信道能量，CCA 判忙、退避、发送队列堆积
 * （thread_br.c 里把 MAC 直接重传从 15 降到 3 就是在收拾这个后果）。
 *
 * 间隔 ≤ 此值视为"太近"：既用于建网时挑信道，也用于运行中告警，只有这一个数。
 * 与 spectrum 的 W_THREAD 惩罚带宽（≤3）是两码事 —— 那个是分档扣分的软权重，
 * 这个是硬阈值，各管各的，别互相对齐。
 */
#define COEX_TOO_CLOSE_MAX  5

/*
 * 建 Thread 网络时挑一个远离 Zigbee 的信道。纯函数，无副作用，便于单测。
 *
 * @p zigbee_known 为 false（NCP 没建网或读不到）时直接返回配置值 —— 没有要
 * 躲的东西，不该擅自改变出厂行为。
 *
 * 两级规则：
 *  1. 先在 WiFi 空隙信道 {15,20,25} 里找间隔 > COEX_TOO_CLOSE_MAX 的，取间隔最大者；
 *  2. 一个都不合格时放宽到 11–25 全范围取间隔最大者，并记一条 W 日志。
 *     ch26 一律排除（贴频段上边沿，多数射频在那里发射功率降额）。
 *
 * 逐个 Zigbee 信道推演（间隔 = |ch - zigbee|）：
 *   zigbee 11 → 15:4✗  20:9✓  25:14✓ → 25
 *   zigbee 15 → 15:0✗  20:5✗  25:10✓ → 25   （与现行默认值一致，出货行为不变）
 *   zigbee 20 → 15:5✗  20:0✗  25:5✗  → 二级：11（间隔 9）
 *   zigbee 25 → 15:10✓ 20:5✗  25:0✗  → 15
 *   zigbee 26 → 15:11✓ 20:6✓  25:1✗  → 15
 * ch20 那一格是唯一走二级的：ch11 压在 2.4G WiFi ch1 上，但邻居 WiFi 看现场，
 * 板上那颗 Zigbee 射频是必然存在的，取舍上间隔优先。
 */
uint8_t   thread_br_pick_channel(uint8_t zigbee_channel, bool zigbee_known,
                                 uint8_t configured_channel);

/* 共存判定的唯一实现。规则只写在这里，调用方不许自己再算一份 —— 否则两处
   阈值迟早各自漂。两边都必须是**实测值**才给结论：拿配置值凑数正是这次要
   修掉的病。Zigbee 侧的"实测值"是用户声明值 —— 实测拿不到，理由见
   config_mgr.h 里 zigbee_config_t 的注释。 */
typedef struct {
    bool    known;           /* 两边都读到了，下面几项才有意义 */
    bool    too_close;       /* separation <= COEX_TOO_CLOSE_MAX */
    uint8_t thread_channel;
    uint8_t zigbee_channel;
    uint8_t separation;
} coex_status_t;

void thread_br_get_coex_status(coex_status_t *out);

/* 判某个**待定**的 Thread 信道是否离 Zigbee 太近（用于 HA 推数据集时的告警）。
   Zigbee 信道未知时返回 false —— 未知不等于有问题。
   @p zigbee_channel_out 可为 NULL。 */
bool thread_br_channel_too_close(uint8_t thread_channel, uint8_t *zigbee_channel_out);

/*
 * 开机时判断要不要把已存在的 Thread 网络挪到别的信道。
 *
 * 为什么需要：产线首烧时 MG24 是空的，没有 Zigbee 网络可躲，
 * thread_br_pick_channel() 只能退回配置值 25；用户之后配 Z2M，信道由他那边
 * 决定，可能正好落在 25（正面撞上）或 20（间隔 5）。挑信道那套逻辑在产线
 * 首烧这条最常见的路径上其实一次都没起作用。
 *
 * @p had_child 是**"这个网络有没有接入过设备"**的粘性记录，不是"当前有几个"：
 * 休眠设备可能暂时不在子节点表里，按当前数判断会误伤。有过设备就一律不动 ——
 * 迁移一个空网络代价为零，迁移一个有设备的网络要走 pending dataset 且会有
 * 中断窗口，那是另一件事（默认关的开关），不在这里做。
 *
 * 返回 true 时 @p new_channel 填新信道。@p new_channel 可为 NULL。
 */
bool thread_br_should_repick(uint8_t current_thread_channel, bool had_child,
                             uint8_t *new_channel);

/*
 * 列出与 @p thread_channel 间隔足够、可以安全给 Zigbee 用的信道（11-26）。
 *
 * 存在的理由是"规则只许有一处"。这份列表要显示在 WebUI 上告诉用户把 Z2M 设成
 * 哪个信道 —— 如果让前端自己写 `Math.abs(c - th) > limit`，比较符就成了第二份
 * 规则，固件哪天把边界改了前端不会跟着变，两边给出的可用信道会差一格。
 * 所以算在这里，前端只负责显示。
 *
 * 这里不排除 ch26：排除它是 Thread 选信道时的取舍（贴频段上边沿要降额），
 * Zigbee 信道归 Z2M/ZHA 决定，我们只负责说"离 Thread 远一点"，无权替用户
 * 缩小合法范围。
 *
 * @p out 至少 16 个元素。返回写入个数（thread_channel 越界时返回 0）。
 */
uint8_t thread_br_list_safe_zigbee_channels(uint8_t thread_channel,
                                            uint8_t *out, uint8_t max);

/* ---- 802.15.4 主动扫描（beacon）—— 纯观测诊断 ----
 *
 * 目的：回答"能不能不靠 EZSP、直接用 C5 的射频测出 Zigbee 网络在哪个信道"。
 * 走 otLinkActiveScan：发 beacon request、收 beacon。Zigbee 协调器会应答，
 * mPanId / mChannel / mRssi 来自 802.15.4 MAC 头，对 Zigbee beacon 同样有效
 * （mNetworkName / mExtendedPanId 是 Thread beacon payload 专有，对 Zigbee
 * 无意义，所以这里不收）。
 *
 * 为什么不用 spectrum 的 beacon_scan：那个直接挂 esp_ieee802154_receive_done，
 * 编入 OpenThread 时会和 OT port 抢 ISR，所以在本构型里被禁用（501）。
 * 走 OT 自己的扫描没有这个冲突。
 *
 * ⚠️ 这是**诊断端点，不参与任何判断**。即使扫到了，"哪个 beacon 是本机
 * MG24 的"也只能靠 RSSI 推断（同板相距几厘米，通常强几十 dB）—— 那是启发式，
 * 不是身份证明。要用它自动填信道，必须让用户确认。
 *
 * ⚠️ 共存可能直接挡掉它。能量扫描实测过：WiFi 共存仲裁器按**连续独占时长**
 * 放行，100ms 驻留时 0/16 信道拿得到数据，1ms 才 15~16/16（见下面那张表）。
 * 主动扫描要等 beacon 回来，驻留短不了，很可能撞同一堵墙 —— 这正是要实测的
 * 问题，所以先做成纯观测。
 */
#define THREAD_BR_BEACON_MAX  16

typedef struct {
    uint16_t pan_id;
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  lqi;
    uint8_t  ext_addr[8];
} thread_br_beacon_t;

/* 从 PCAP 原始帧自己解析出来的 beacon。
 *
 * 为什么需要它：OT 的扫描结果解析器有一条硬过滤 —— scan_result.cpp:50
 *   VerifyOrExit(address.IsExtended(), error = kErrorParse);
 * 源地址不是 64 位扩展地址的 beacon 一律丢弃。而 **Zigbee 协调器用 16 位短地址
 * (0x0000) 发 beacon**，所以 otLinkActiveScan 结构性地看不见 Zigbee 网络。
 * 挂 otLinkSetPcapCallback 拿原始帧自己解析就能绕过这条过滤。
 *
 * 2026-08-12：先前误以为扫不到自家协调器是 permit join 关着 —— 错的，
 * permit join 只是 beacon 里的一个标志位（sl_zigbee_types.h:1205），
 * 不控制发不发。 */
typedef struct {
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  lqi;
    uint16_t pan_id;
    bool     src_extended;    /* false = 16 位短地址（Zigbee 的典型形态） */
    uint8_t  src_addr[8];     /* 短地址时只有前 2 字节有效 */
    uint8_t  hdr_len;         /* 存了多少字节原始帧头，便于人工核对 */
    uint8_t  hdr[16];
} thread_br_raw_beacon_t;

typedef struct {
    uint8_t  count;            /* OT 报上来的（只含扩展地址源） */
    bool     completed;        /* 扫描自然结束，不是被预算强行收尾 */
    uint16_t dwell_ms;
    uint32_t elapsed_ms;
    thread_br_beacon_t nets[THREAD_BR_BEACON_MAX];

    /* 同一次扫描里从 PCAP 抓到的原始 beacon。raw_count > count 就说明
       OT 确实丢弃了一部分（短地址的那些）。 */
    uint8_t  raw_count;
    uint8_t  raw_dropped;      /* 超出数组容量而没记下的 */
    /* PCAP 看到的全部帧数，按类型分。用来把"钩子没触发/射频没听到"和
       "确实没有 beacon"区分开 —— 只看 beacon 数为 0 是有歧义的。 */
    uint32_t pcap_rx_total;
    uint32_t pcap_rx_beacon;   /* 帧类型 0 */
    uint32_t pcap_rx_data;     /* 帧类型 1 */
    uint32_t pcap_rx_ack;      /* 帧类型 2 */
    uint32_t pcap_rx_cmd;      /* 帧类型 3（含 beacon request） */
    uint32_t pcap_tx_total;
    thread_br_raw_beacon_t raw[THREAD_BR_BEACON_MAX];
} thread_br_beacon_scan_t;


/* dwell_ms=0 取默认；channel_mask=0 取 ch11-26 全扫。
   ESP_ERR_INVALID_STATE = Thread BR 没跑；ESP_ERR_TIMEOUT = 扫描没能自然收尾。
   注意：count==0 且 completed==true 是**有效结果**（附近没有 Zigbee 网络，
   或者协调器没应答），不是失败。 */
esp_err_t thread_br_active_scan(uint16_t dwell_ms, uint32_t channel_mask,
                                thread_br_beacon_scan_t *out);

/* 在指定信道上扫一遍，返回信号最强的那个 beacon 的 PAN ID。
   用于"用户声明信道时顺便把自家协调器的 PAN 记下来"——同板相距几厘米，
   自家 beacon 通常比邻居强几十 dB（实测 -20 vs -81），最强者就是它。
   找不到任何 beacon 返回 ESP_ERR_NOT_FOUND。 */
esp_err_t thread_br_probe_pan(uint8_t channel, uint16_t *pan_out, int8_t *rssi_out);

/* 全频段找信号最强的 beacon。用于用户声明的信道上扫不到东西时给出提示 ——
   "你说 CH15，但我在 CH20 上看到一个 -21 dBm 的网络，确定吗？"
   这是"一上来就填错"这条路径唯一的拦截点：填错就学不到 PAN，学不到 PAN
   就没法做过期检测，缺口正好在这里闭合。 */
esp_err_t thread_br_find_strongest(uint8_t *ch_out, uint16_t *pan_out,
                                   int8_t *rssi_out);

/* ---- 声明值过期检测 ----
 *
 * 声明值最大的弱点是会悄悄过期：用户在 Z2M/ZHA 改了信道却忘了回来同步，
 * 之后所有共存判断都建立在一个错的数上。有了 PAN 就能发现它 —— 定期扫描，
 * 本 PAN 出现在别的信道 = 用户改过了。
 *
 * 检测顺序是先窄后宽：先只扫声明的那个信道（约 1s），本 PAN 还在就收工；
 * 不在才全频段扫（约 7s）去找它搬到哪儿了。搬家是罕见事件，所以稳态开销
 * 就是每个周期 1 秒。 */
typedef struct {
    bool     valid;            /* 至少完整跑过一次检测 */
    bool     mismatch;         /* 本 PAN 出现在与声明值不同的信道上 */
    bool     not_found;        /* 全频段都没扫到本 PAN（网络关了？搬远了？） */
    uint8_t  declared_channel;
    uint8_t  detected_channel; /* mismatch 时有效 */
    uint16_t pan_id;
    int8_t   rssi;
    uint32_t checked_at;       /* 秒，同 esp_timer 时基 */
} zigbee_channel_check_t;

void thread_br_get_zigbee_check(zigbee_channel_check_t *out);

/* 用一次外部实测结果直接刷新结论，不再等下一个检测周期。
 * 调用方：POST /api/v1/zigbee/channel —— 它声明信道时本来就在该信道上探过一次
 * PAN，那与周期检测的窄扫阶段是同一件事，白白让横幅挂着旧结论干等 6 小时没有
 * 道理（2026-09-15）。
 * pan_known=false（探不到，或用户撤销了声明）→ 作废结论，status 回 null。
 * 注意这里只写 s_zb_check，不占射频。 */
void thread_br_zigbee_check_seed(uint8_t declared_channel, bool pan_known,
                                 uint16_t pan_id, int8_t rssi);

/* ---- 802.15.4 能量扫描 ----
 *
 * 走 otLinkEnergyScan：OT port 占着裸驱动的回调，绕过它去直接驱射频会踩
 * OT 的断言。填 energy[16]（ch11..26，dBm）。
 *
 * 参数为什么长这样 —— 2026-08-11 在板实测（`GET /api/v1/diag/ed` 的计数器）：
 * WiFi 共存仲裁器按**连续独占时长**决定放不放行，跟优先级无关。
 *
 *     每信道驻留   拿到读数的信道数
 *       100 ms        0/16      ← 旧实现，功能等于废掉
 *        50 ms        0/16
 *        20 ms        4/16
 *         5 ms       9~11/16
 *         1 ms      15~16/16    ← 现在的默认值
 *
 * 对照组：CCA 就是一次 128µs 的 ED，它每次发包都在跑，从来没被拒过。
 * 所以"多重试几次"是无效的（同长度的拒绝是确定性的，实测两轮都恰好 16/16 全拒），
 * 唯一有效的变量是把窗口切短；再用多轮 max-hold 把单次短窗口易漏突发流量的
 * 缺点补回来。
 */
#define THREAD_BR_SCAN_DEFAULT_DWELL_MS  1    /* 每信道驻留，实测的甜点 */
#define THREAD_BR_SCAN_DEFAULT_PASSES    16   /* 轮数，约 1.1s 扫完 */
#define THREAD_BR_SCAN_MAX_PASSES        64
#define THREAD_BR_SCAN_PASS_GAP_MS       20   /* 轮间让出射频给 Thread 正常收发 */
/* 总时长硬上限：本函数被单任务 httpd 同步调用，超时会让整个 WebUI 失去响应。
   dwell 与 passes 各自合法也可能乘出几分钟，所以必须钳乘积。 */
#define THREAD_BR_SCAN_MAX_TOTAL_MS      8000

/* dwell_ms=0 / passes=0 取上面的默认值；dwell_ms 钳到 [1,1000]
   （>1048ms 会在 IDF 里静默截断，0 会走 OT 的同步 GetRssi 假数据路径）。
   valid_mask 可为 NULL；bit i = ch(11+i) 拿到了真实读数。
   返回 ESP_OK 表示**至少一个**信道有效 —— 调用方必须看 valid_mask，
   energy[] 里 valid 位为 0 的项是 -100 哨兵，不是测量值。
   ESP_ERR_TIMEOUT = 一个信道都没测到；ESP_ERR_INVALID_STATE = Thread BR 没跑。 */
esp_err_t thread_br_energy_scan_ex(uint16_t dwell_ms, uint8_t passes,
                                   int8_t energy[16], uint16_t *valid_mask);

/* 旧接口，保留兼容。duration_per_ch_ms 已被忽略（长驻留在本平台必被拒绝），
   内部走默认的短驻留多轮；且只有 16 个信道全部测到才返回 ESP_OK，
   因为老调用方不认识 valid_mask，会把 -100 当成真实能量。 */
esp_err_t thread_br_energy_scan(uint16_t duration_per_ch_ms, int8_t energy[16]);

/* 子节点恢复进度。
 *
 * 网关重启后，OpenThread 会从 NVS 恢复出重启前的子节点列表，但这些条目要等各自
 * 的设备真正重新入网才"活"过来——休眠设备实测要 4-6 分钟。这段时间里网关看着
 * 一切正常而设备全部离线，用户很容易以为网关坏了，所以必须把进度显示出来。
 *
 * total  = 子节点表里的条目数（含尚未回来的）
 * online = 已真正重新入网的数量（otChildInfo.mIsStateRestoring == false）
 * online < total 即"恢复中"。Thread BR 未运行时两者均为 0。读的是 2 s 定时器的缓存（不拿 OT 锁），最多晚 2 s。
 */
void thread_br_get_child_stats(uint8_t *total, uint8_t *online);

/* ---- 能量扫描诊断（P0，纯观测，不改变扫描行为） ----
 *
 * 为什么需要它：扫描拿不到数据时，"ED 被 WiFi 共存拒了"一直只是推断。
 * 802.15.4 外设里有一组**自由运行的硬件计数器**能把它变成数字，且不依赖
 * CONFIG_IEEE802154_DEBUG、也不受中断使能影响（读/清见
 * hal/ieee802154_common_ll.h:482-535，清除位见 soc/ieee802154_reg.h:468-498）。
 *
 * 最关键的一项是 rx_abort_event_en：驱动只在初始化时使能了
 * TX_ACK_TIMEOUT 与 TX_ACK_COEX_BREAK 两位（esp_ieee802154_dev.c:906），
 * ED_ABORT(24) / ED_COEX_REJECT(26) 的位**从未被使能**。若实测确认如此，
 * 说明 ED 被拒时连中断都不产生 —— 是"静默死亡"而不是"回调缺失"，
 * esp_ieee802154_ed_failed() 在本板上是死代码。这一条决定后续走哪条路，
 * 而该寄存器的复位值源码里查不到，只能实测。
 */
typedef struct {
    /* ---- 硬件计数器（自由运行） ---- */
    uint32_t ed_scan_coex;      /* ED 被共存仲裁器打断的次数（本议题主角） */
    uint32_t ed_abort;          /* ED 因其他原因中止的次数 */
    uint32_t rx_abort_coex;     /* 对照组：普通 RX 被共存打断（RX 靠驱动自动重试，用户无感） */
    uint32_t cca_fail;          /* 对照组：CCA 失败。CCA 就是一次 128µs 的 ED，
                                   它每次发包都在跑 —— 与 ED 的差别只有连续独占时长 */
    /* ---- 中断使能现状 ---- */
    uint32_t rx_abort_event_en; /* IEEE802154_RX_ABORT_INTR_CTRL_REG (base+0x68) 原值 */
    bool     ed_irq_armed;      /* ED_ABORT 与 ED_COEX_REJECT 两位是否都已使能 */
    /* ---- 软件计数 ---- */
    uint32_t ed_failed_cb;      /* esp_ieee802154_ed_failed() 实际被调用的次数 */
} thread_br_ed_diag_t;

/* 最近一次能量扫描期间的增量快照 */
typedef struct {
    bool     valid;             /* 是否已经跑过至少一次扫描 */
    uint32_t ed_scan_coex;      /* 以下四项均为"扫描期间的增量" */
    uint32_t ed_abort;
    uint32_t rx_abort_coex;
    uint32_t cca_fail;
    uint32_t ed_failed_cb;
    uint16_t valid_mask;        /* bit i = ch(11+i) 真的回调上来了（OT 会丢弃 127，
                                   所以"回调来过"就等于"这个信道真测到了"） */
    uint8_t  valid_count;       /* popcount(valid_mask)，0 = 一个信道都没测到 */
    uint16_t duration_ms;       /* 本次扫描的每信道驻留时长 */
    uint32_t elapsed_ms;        /* 整次扫描实际耗时 */
    bool     completed_naturally; /* false = 被 force_finish_energy_scan() 强行收尾 */
} thread_br_scan_diag_t;

/* 读硬件计数器。射频未起来时返回 ESP_ERR_INVALID_STATE（此时寄存器读数无意义）。 */
esp_err_t thread_br_ed_diag_read(thread_br_ed_diag_t *out);
void      thread_br_scan_diag_read(thread_br_scan_diag_t *out);

/* 把 ED_ABORT / ED_COEX_REJECT 两个 RX-abort 事件位打开（或还原）。
 * 仍然是纯观测：本文件的 esp_ieee802154_ed_failed() 只累加计数，不向
 * OpenThread 转发任何东西，扫描结果与不开时完全一致。开了之后
 * ed_failed_cb 若开始有数，就证明"中断没使能"是 ED 静默死亡的直接原因。
 * 注意：这两位不会被普通收发流程清掉 —— 只有 receive_at（定时 RX，本板
 * rx-on-when-idle 不走）路径会在 next_operation() 里重置它们
 * （esp_ieee802154_dev.c:474-478）。 */
esp_err_t thread_br_ed_diag_arm_irq(bool arm);

/* 看门狗层 C：读 2s 定时器算好的射频健康结论（不取 OT 锁）。返回 false
   表示 thread_br 未初始化。 */
bool      thread_br_get_health(bool *running, bool *stalled);

#if defined(CONFIG_BITENGINE_ENABLE_MUSE_LINK) && CONFIG_BITENGINE_ENABLE_MUSE_LINK
/* ⚠️ 本头文件不包含 sdkconfig.h（主机测试 test_host/ 没有它）：用这两个函数的 .c 要先包含 sdkconfig.h。
   直连型 + Muse：BLE 窗口之后的 Thread 自检（docs/15 设计 3.3）。读的是 2 s 采样的缓存，不拿 OT 锁，任何任务可调。
   now：MAC 计数器 mTxAckRequested / mTxAcked 的累计值。window：过去约 10 分钟（60 s 一格的 10 格环）的增量与跨度。
   Thread 没在跑或还没采到过返回 false。 */
bool      thread_br_get_ack_now(uint32_t *ack_req, uint32_t *acked);
bool      thread_br_get_ack_window(uint32_t *req_delta, uint32_t *acked_delta, uint32_t *span_s);
#endif
/* 看门狗 L2：只重启 802.15.4 射频。Thread 没有 L3（整栈重启正常态不可用，T9.18），
   L2 救不回就由看门狗整机重启。 */
esp_err_t thread_br_radio_recover(void);

/* 运行中把边界路由的 backbone 换到另一个上行口（to = "eth" / "wifi"）。
 *
 * 为什么需要：backbone 原来只在 thread_br_start() 里按"以太网优先"选一次。以太网一断、
 * 网关退到 WiFi，OpenThread 的路由管理器自己停掉（/node/border-routing = stopped），
 * HA 到 Thread 网络的路由一直指向以太网口的链路本地地址 —— Matter 设备全不通，
 * 直到网线插回或整板重启（2026-10-05 实测，docs/test_cases_stability.md 测试 6c）。
 *
 * 做法：只拆/建边界路由这一层（border_router_deinit → set_backbone_netif →
 * border_router_init），Thread 网络本身不停、设备不用重新入网。IDF v6.0.2 官方示例
 * examples/openthread/ot_common_components/ot_examples_br/ot_examples_br.c 就是在
 * OT 协议栈起来之后、持 OT 锁做 set_backbone + border_router_init，并配有只拆这一层的
 * stop —— 头文件里"set_backbone 必须在 esp_openthread_init 之前"的注释与官方示例矛盾。
 * ⚠️ 边界路由层是预编译库（libopenthread_br.a），"拆了再建"能否清理干净无源码可证，
 *    以实测为准。
 *
 * 在一次性任务里执行（库内栈用量不可知、还可能写 NVS，既不能放 httpd 任务也不能用
 * PSRAM 栈），本函数最多等 timeout_ms。同一时刻只允许一个切换在跑。
 * 返回：init 的结果；ESP_ERR_NOT_FOUND 未知网口名或网口不存在；ESP_ERR_INVALID_STATE
 * Thread 没在跑 / 射频暂停中 / 目标网口没 up；ESP_ERR_NOT_FINISHED 已有切换在跑；
 * ESP_ERR_TIMEOUT 等结果超时（切换仍会在后台完成）。out 可为 NULL。 */
typedef struct {
    esp_err_t   deinit_err;   /* 原来没有边界路由时为 ESP_OK（没拆） */
    esp_err_t   init_err;
    uint32_t    ms;           /* 持 OT 锁拆+建的耗时 */
    const char *from;         /* "eth" / "wifi" / "none" */
    const char *to;
} thread_br_backbone_switch_t;
esp_err_t   thread_br_switch_backbone(const char *to, uint32_t timeout_ms,
                                      thread_br_backbone_switch_t *out);
/* 当前边界路由用的上行口："eth" / "wifi" / "none"（没有边界路由）。不取 OT 锁。 */
const char *thread_br_backbone_name(void);

/* SRP server 的实时状态：监听端口 + 已注册的 host/service 计数。
 * 为什么需要：
 *  · `hosts`/`services` 是**真实设备数的地面真值** —— 不受 /node/children 那个
 *    2048 字节 JSON 缓冲的无标记截断影响（约 10 条就截断，见 docs/扩到50设备 约束 3）。
 *  · `port` 是从局域网侧做 SRP 压测的前提：OT 的 SRP server 在 53535-53554 里
 *    随机选一个并持久化，**从外部探测不出来**（畸形 UPDATE 被静默丢弃，
 *    关闭端口也不回 ICMPv6 unreachable —— 2026-09-15 两种探法都试过）。
 * 取 OT 锁；未运行时返回 ESP_ERR_INVALID_STATE。 */
esp_err_t thread_br_srp_info(uint16_t *port, int *hosts, int *services);

/* OT 任务过载保护（ot_cpu_guard.c）：thread_br_init 里装一次 idle hook；读数给 /api/v1/diag/srp。
   yields = 因为 IDLE 久未运行而主动让出过几次；starve_max_ms = 开机以来 IDLE 最长一次没跑到（idle hook 量的，任何原因）。 */
void thread_br_cpu_guard_init(void);
void thread_br_cpu_guard_stats(uint32_t *yields, uint32_t *starve_max_ms);
/* IDLE 最近一次运行的时刻（esp_timer µs 低 32 位；idle hook 没挂上时为 0）。给打过补丁的 mdns 任务循环用（SCALE ⑧） */
uint32_t bitengine_idle_last_us(void);
