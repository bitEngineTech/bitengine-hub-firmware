#pragma once
/* muse_link_core —— Muse 会话的状态机与退避（docs/15_muse_direct_design_v1.md 4.1、4.2）。纯 C，不依赖 ESP-IDF：
 * test_host/ 原样编它，把"状态 × 事件"整张矩阵跑一遍（CLAUDE.md 编码后第 1 条）。会话任务（M3 muse_session）
 * 把网络 / 内存 / VM API / WebSocket / 注册的结果翻译成事件喂进来，照输出的动作去做。
 *
 * 与 4.2 矩阵的出入（设计 4.2 有指针指回这里；"上游"指 esp32/main，e6fe517）：
 *   - CONNECTING 收到 WS 升级 401/403（或 Noise 握手失败 —— 上游同样按鉴权失败算，noise_control.cpp:2070-2073）：
 *     先退避 60 s 再回 FETCH_VM 取新 VM 凭据（上游 RECONNECT_AUTH_FAILED_MS），不是立刻回 —— 立刻回会和服务端形成热循环；
 *   - FETCH_VM 收到 401/403：本轮还没刷新过令牌 → REFRESHING；刷新过了还被拒 → "新凭据也不认"，**不是撤销**，保留配对，
 *     按 1/2/4/8/16/30 min 退避（上游 app.c:288-323 note_vm_credentials_refused，:976-1009）。只有 REFRESHING 里
 *     刷新令牌本身被拒（被动刷新，上游 allow_stale=false）才清令牌进 UNPAIRED；ONLINE 里的后台定期刷新被拒**保留**
 *     旧令牌（上游 allow_stale=true，app.c:937-941、:2909："the token still works, nothing has rejected it"）；
 *   - 连接退避：1 s 起、每次加倍、封顶 15 s；从注册成功算起在线超过 30 s 再断才回到 1 s。上游是先加倍再等（第一次等 2 s）、
 *     在线时长从会话开始算、连续 3 次短会话才重取 VM；我们每次退避到点都走 FETCH_VM 重取（4.2 矩阵），断网期间
 *     fetch_vms 最多约 240 次 / 小时；
 *   - 注册被拒：60 s 起每次加倍、封顶 30 min（4.2 写的是固定 60 s，服务端一直拒时一天约 1440 次完整连接）；注册发出后
 *     30 s 没有应答 → 关会话走连接退避（上游没有这个超时，会一直挂在"连着没注册"、每个调用回 busy）；
 *   - DISABLED 收到 EN 时网在就直接进 FETCH_VM（矩阵写的是 WAIT_NET，那样会等一个不会再来的 NET_UP）；
 *   - 进 FETCH_VM 之前先看网络、再看内存闸门（4.6）。**约定**：网络与内存的状态由调用方告诉状态机 —— 变化时发
 *     NET_* / MEM_* 事件，喂 MLE_TIMER 之前先 mlc_set_env() 刷新一次（LOW_MEM 的 60 s 复查靠它）；
 *   - 解绑（进 UNPAIRED）清掉退避与各种拒绝计数（上游解绑后整机重启，等于全清）；
 *   - 退避到点之前不重取 VM：进 BACKOFF 时记下 not_before，期间断网又恢复（WAIT_NET / LOW_MEM 回来）也要等到点，
 *     否则网线一抖就绕过 1–30 min 的退避、对 api.muse.ai 形成刷新风暴（上游的刷新下限按时间算，app.c:1112-1119）；
 *   - 在线超过 30 s 后不论因为什么离开 ONLINE（断网、内存、重注册、禁用），连接退避都回到 1 s；
 *   - device.unpair（4.13）：先删凭据并读回确认、**先把 accepted 发出去**，再投 MLE_UNPAIRED_EVT（原因 unpaired_in_app）——
 *     动作顺序是 CLOSE 在先，先投事件会把回包一起关掉；device.set_vm / reset_vm：存好 vm_id、回 accepted，再投 MLE_VM_SWITCH。
 * 矩阵里的"—¹"（该状态下不可能到达的事件）返回 unexpected，由调用方 ESP_LOGW 后丢弃，不 assert。
 * 原因串（last_error）里没有服务端给的文字；注册被拒时服务端给的原因不可信，显示前要过 hub_sanitize 再转义。 */
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MUSE_ST_DISABLED = 0, MUSE_ST_UNPAIRED, MUSE_ST_WAIT_NET, MUSE_ST_LOW_MEM,
    MUSE_ST_FETCH_VM, MUSE_ST_CONNECTING, MUSE_ST_REGISTERING, MUSE_ST_ONLINE,
    MUSE_ST_BACKOFF, MUSE_ST_REFRESHING, MUSE_ST_PROTOCOL_ERROR,
    MUSE_ST_COUNT
} muse_state_t;

typedef enum {
    MLE_ENABLE = 0, MLE_DISABLE,
    MLE_NET_UP, MLE_NET_DOWN, MLE_MEM_LOW, MLE_MEM_OK,
    MLE_PAIRED,                     /* 配对凭据落盘（muse_link_on_provisioned） */
    MLE_VM_OK, MLE_VM_AUTH, MLE_VM_ERR,             /* fetch_vms 的结果 */
    MLE_WS_UP, MLE_WS_AUTH, MLE_WS_ERR,             /* TLS + WS 升级 + Noise；WS_ERR 也表示在线会话断了 */
    MLE_REG_OK, MLE_REG_ERR, MLE_REG_PROTO,         /* link.register 的应答（muse_proto 的判定） */
    MLE_REFRESH_OK, MLE_REFRESH_REJECTED, MLE_REFRESH_ERR,   /* 设备令牌刷新的结果（REFRESHING 里的，或 ONLINE 里后台的） */
    MLE_INVOKE,                     /* 收到 link.invoke */
    MLE_UNPAIRED_EVT,               /* 服务端的 link.unpaired / node.unpaired */
    MLE_LOCAL_UNPAIR,               /* 本地解绑（REST）；device.unpair 删完凭据后投的是 MLE_UNPAIRED_EVT（见文件头） */
    MLE_REREG,                      /* 权限或后端变了，命令表要重发 */
    MLE_TIMER,                      /* 本状态的定时器到点 */
    MLE_TOKEN_DUE,                  /* 令牌到了 30 天的刷新周期（4.2 注 4） */
    MLE_VM_SWITCH,                  /* device.set_vm / reset_vm 改了选中的 VM：关会话、重新取 VM（4.13） */
    MLE_COUNT
} mle_t;

/* 动作位：调用方按位做，顺序为 CLOSE → CLEAR_CREDS → 其余 */
#define MLA_CLOSE        0x0001u   /* 关会话（销毁 ClientSession、清会话密钥） */
#define MLA_CLEAR_CREDS  0x0002u   /* 删 acc_tok、ref_tok、vm_id（sdk_tok 与设置保留，4.10） */
#define MLA_FETCH_VM     0x0004u   /* 调 fetch_vms */
#define MLA_CONNECT      0x0008u   /* TLS + WS + Noise */
#define MLA_REGISTER     0x0010u   /* 发 link.register（命令表按当时的权限与后端生成） */
#define MLA_REFRESH      0x0020u   /* 刷新设备令牌（REFRESHING） */
#define MLA_BG_REFRESH   0x0040u   /* 在线时后台刷新令牌，会话不断 */
#define MLA_REPLY_BUSY   0x0080u   /* 注册还没完成就来了调用：回 busy */
#define MLA_DISPATCH     0x0100u   /* 派发调用 */

#define MLC_TIMER_KEEP   (-1)      /* 定时器不变 */

typedef enum {
    MLR_NONE = 0,
    MLR_TOKEN_REVOKED,             /* 刷新令牌被拒：已清令牌（4.10） */
    MLR_UNPAIRED_BY_APP,           /* "已在 Muse App 中删除" */
    MLR_LOCAL_UNPAIR,
    MLR_PROTOCOL_CHANGED,          /* 注册被拒且带 unsupported / version：提示升级固件 */
    MLR_REG_REJECTED,
    MLR_AUTH_REFUSED,              /* 新令牌也被拒（不是撤销），在退避 */
    MLR_WS_AUTH,                   /* WS 升级被拒，60 s 后取新 VM 凭据 */
    MLR_NET_ERROR,                 /* VM API / WS / 刷新的网络类错误，在退避 */
    MLR_LOW_MEM,
    MLR_COUNT
} mlc_reason_t;

typedef struct {
    muse_state_t st;
    bool     enabled, paired, net_up, mem_ok;
    bool     refreshed;            /* 这一轮 FETCH 已经刷新过一次令牌 */
    bool     rereg_pending;        /* 注册已发出后权限又变了：注册成功就重连重注册 */
    uint8_t  auth_refusals;        /* 新凭据被拒的连续次数 */
    uint8_t  reg_rejects;          /* 注册被拒的连续次数 */
    uint32_t conn_backoff_ms;      /* 下一次连接类退避的时长 */
    uint64_t online_since_ms;
    uint64_t not_before_ms;        /* 这之前不进 FETCH_VM（进 BACKOFF 时记下；0 = 没有） */
    uint64_t now_ms;               /* 当前这一步的时刻（mlc_step 进来时记） */
    uint32_t registrations;        /* link.register 成功次数（4.1 muse_status_t） */
    mlc_reason_t reason;           /* 最近一次进非正常状态的原因（ONLINE 时清掉） */
} mlc_t;

typedef struct {
    uint32_t     act;              /* MLA_* */
    int32_t      timer_ms;         /* > 0 起定时器；0 取消；MLC_TIMER_KEEP 不变 */
    bool         unexpected;       /* 矩阵里的"—¹"：调用方 ESP_LOGW 后丢弃 */
    muse_state_t from, to;
} mlc_out_t;

/* 退避常量（4.2、上游） */
#define MLC_CONN_BACKOFF_MIN_MS   1000u
#define MLC_CONN_BACKOFF_MAX_MS   15000u
#define MLC_STABLE_SESSION_MS     30000u
#define MLC_REG_ERR_BACKOFF_MS    60000u      /* 60 s 起每次加倍，封顶 30 min */
#define MLC_REG_ERR_BACKOFF_MAX_MS 1800000u
#define MLC_REG_TIMEOUT_MS        30000u      /* 注册发出后等应答的时限 */
#define MLC_WS_AUTH_BACKOFF_MS    60000u
#define MLC_AUTH_REFUSED_BASE_MS  60000u      /* 1 / 2 / 4 / 8 / 16 / 30 min */
#define MLC_AUTH_REFUSED_MAX_MS   1800000u
#define MLC_LOW_MEM_RECHECK_MS    60000u
#define MLC_PROTOCOL_RETRY_MS     21600000u   /* 6 h */

/* 初始化并落到初始状态（DISABLED / UNPAIRED / WAIT_NET / LOW_MEM / FETCH_VM），返回进入初始状态要做的动作 */
mlc_out_t mlc_init(mlc_t *c, bool enabled, bool paired, bool net_up, bool mem_ok);
mlc_out_t mlc_step(mlc_t *c, mle_t ev, uint64_t now_ms);
/* 刷新网络 / 内存的状态但不触发转移（喂 MLE_TIMER 之前调） */
void mlc_set_env(mlc_t *c, bool net_up, bool mem_ok);

const char *mlc_state_str(muse_state_t s);
const char *mlc_event_str(mle_t e);
const char *mlc_reason_str(mlc_reason_t r);
/* 第 n 次（从 1 起）新凭据被拒之后的退避 */
uint32_t mlc_auth_refused_backoff_ms(uint8_t refusals);
/* 第 n 次（从 1 起）注册被拒之后的退避 */
uint32_t mlc_reg_rejected_backoff_ms(uint8_t rejects);
