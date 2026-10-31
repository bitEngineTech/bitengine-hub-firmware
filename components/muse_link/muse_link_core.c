/* muse_link_core —— Muse 会话的状态机与退避（见 muse_link_core.h）。纯 C，不依赖 ESP-IDF。 */
#include "muse_link_core.h"

#include <string.h>

static bool has_session(muse_state_t s)
{
    return s == MUSE_ST_CONNECTING || s == MUSE_ST_REGISTERING || s == MUSE_ST_ONLINE;
}

/* 每个状态进入时的定时器（4.2 的 TIMER 列）；其余状态不要定时器 */
static mlc_out_t go(mlc_t *c, mlc_out_t o, muse_state_t to, int32_t timer_ms)
{
    o.to = to;
    o.timer_ms = timer_ms;
    if (to != c->st && has_session(c->st) && !has_session(to)) o.act |= MLA_CLOSE;   /* 离开有会话的状态一律先关 */
    /* 在线超过 30 s 后离开 ONLINE：连接退避回到 1 s（进 BACKOFF 的那条路由 conn_backoff 自己处理） */
    if (c->st == MUSE_ST_ONLINE && to != MUSE_ST_ONLINE && to != MUSE_ST_BACKOFF && c->now_ms >= c->online_since_ms
        && c->now_ms - c->online_since_ms > MLC_STABLE_SESSION_MS) {
        c->conn_backoff_ms = MLC_CONN_BACKOFF_MIN_MS;
    }
    if (to == MUSE_ST_BACKOFF && timer_ms > 0) c->not_before_ms = c->now_ms + (uint64_t)timer_ms;
    if (to == MUSE_ST_ONLINE) c->reason = MLR_NONE;
    c->st = to;
    return o;
}

/* 进 FETCH_VM 之前先看网络、再看内存闸门（4.6） */
static mlc_out_t enter_fetch(mlc_t *c, mlc_out_t o)
{
    if (!c->net_up) return go(c, o, MUSE_ST_WAIT_NET, 0);
    if (!c->mem_ok) {
        c->reason = MLR_LOW_MEM;
        return go(c, o, MUSE_ST_LOW_MEM, (int32_t)MLC_LOW_MEM_RECHECK_MS);
    }
    /* 退避还没到点（期间断网又恢复）：回 BACKOFF 等剩下的时间；差 50 ms 以内算到点（定时器按 tick 取整会早一点） */
    if (c->not_before_ms > c->now_ms + 50) {
        const uint64_t left = c->not_before_ms - c->now_ms;
        return go(c, o, MUSE_ST_BACKOFF, (int32_t)(left > MLC_PROTOCOL_RETRY_MS ? MLC_PROTOCOL_RETRY_MS : left));
    }
    c->not_before_ms = 0;
    c->refreshed = false;
    o.act |= MLA_FETCH_VM;
    return go(c, o, MUSE_ST_FETCH_VM, 0);
}

/* 启用且已配对时从哪里开始 */
static mlc_out_t enter_active(mlc_t *c, mlc_out_t o)
{
    if (!c->enabled) return go(c, o, MUSE_ST_DISABLED, 0);
    if (!c->paired) return go(c, o, MUSE_ST_UNPAIRED, 0);
    return enter_fetch(c, o);
}

/* 连接类失败的退避：会话在线超过 30 s 才回到 1 s，否则沿用并加倍（封顶 15 s） */
static mlc_out_t conn_backoff(mlc_t *c, mlc_out_t o, uint64_t now_ms, mlc_reason_t why)
{
    if (c->st == MUSE_ST_ONLINE && now_ms >= c->online_since_ms && now_ms - c->online_since_ms > MLC_STABLE_SESSION_MS) {
        c->conn_backoff_ms = MLC_CONN_BACKOFF_MIN_MS;
    }
    const uint32_t d = c->conn_backoff_ms;
    c->conn_backoff_ms = (d >= MLC_CONN_BACKOFF_MAX_MS / 2) ? MLC_CONN_BACKOFF_MAX_MS : d * 2;
    c->reason = why;
    return go(c, o, MUSE_ST_BACKOFF, (int32_t)d);
}

static mlc_out_t to_unpaired(mlc_t *c, mlc_out_t o, mlc_reason_t why, bool clear)
{
    if (clear) o.act |= MLA_CLEAR_CREDS;
    c->paired = false;
    c->rereg_pending = false;
    c->auth_refusals = 0;                                             /* 重新配对后不该一上来就退避 30 min */
    c->reg_rejects = 0;
    c->conn_backoff_ms = MLC_CONN_BACKOFF_MIN_MS;
    c->not_before_ms = 0;
    c->reason = why;
    return go(c, o, MUSE_ST_UNPAIRED, 0);
}

uint32_t mlc_reg_rejected_backoff_ms(uint8_t rejects)
{
    unsigned doublings = rejects > 0 ? (unsigned)rejects - 1u : 0u;
    if (doublings > 5u) doublings = 5u;
    uint32_t d = MLC_REG_ERR_BACKOFF_MS << doublings;
    return d > MLC_REG_ERR_BACKOFF_MAX_MS ? MLC_REG_ERR_BACKOFF_MAX_MS : d;
}

uint32_t mlc_auth_refused_backoff_ms(uint8_t refusals)
{
    unsigned doublings = refusals > 0 ? (unsigned)refusals - 1u : 0u;
    if (doublings > 5u) doublings = 5u;
    uint32_t d = MLC_AUTH_REFUSED_BASE_MS << doublings;
    return d > MLC_AUTH_REFUSED_MAX_MS ? MLC_AUTH_REFUSED_MAX_MS : d;
}

mlc_out_t mlc_init(mlc_t *c, bool enabled, bool paired, bool net_up, bool mem_ok)
{
    mlc_out_t o = { 0, 0, false, MUSE_ST_DISABLED, MUSE_ST_DISABLED };
    if (!c) return o;
    memset(c, 0, sizeof(*c));
    c->enabled = enabled;
    c->paired = paired;
    c->net_up = net_up;
    c->mem_ok = mem_ok;
    c->conn_backoff_ms = MLC_CONN_BACKOFF_MIN_MS;
    c->st = MUSE_ST_DISABLED;
    return enter_active(c, o);
}

mlc_out_t mlc_step(mlc_t *c, mle_t ev, uint64_t now_ms)
{
    mlc_out_t o = { 0, MLC_TIMER_KEEP, false, MUSE_ST_DISABLED, MUSE_ST_DISABLED };
    if (!c) {
        o.unexpected = true;
        return o;
    }
    const muse_state_t s = c->st;
    o.from = o.to = s;
    c->now_ms = now_ms;

    /* 先更新环境标志：它们在任何状态下都要记着，状态转移再按需看 */
    switch (ev) {
    case MLE_NET_UP:   c->net_up = true;  break;
    case MLE_NET_DOWN: c->net_up = false; break;
    case MLE_MEM_LOW:  c->mem_ok = false; break;
    case MLE_MEM_OK:   c->mem_ok = true;  break;
    default: break;
    }

    switch (ev) {
    /* ---------------- 开关 ---------------- */
    case MLE_ENABLE:
        if (s != MUSE_ST_DISABLED) return o;                         /* 已经开着 */
        c->enabled = true;
        c->conn_backoff_ms = MLC_CONN_BACKOFF_MIN_MS;
        c->auth_refusals = 0;
        c->reg_rejects = 0;
        c->not_before_ms = 0;
        c->reason = MLR_NONE;
        return enter_active(c, o);
    case MLE_DISABLE:
        c->enabled = false;
        c->rereg_pending = false;
        c->reason = MLR_NONE;                                         /* 关着就是关着，别显示上一次的网络错误 */
        if (s == MUSE_ST_DISABLED) return o;
        return go(c, o, MUSE_ST_DISABLED, 0);

    /* ---------------- 环境 ---------------- */
    case MLE_NET_UP:
        if (s == MUSE_ST_WAIT_NET) return enter_fetch(c, o);
        return o;
    case MLE_NET_DOWN:
        switch (s) {
        case MUSE_ST_LOW_MEM: case MUSE_ST_FETCH_VM: case MUSE_ST_CONNECTING: case MUSE_ST_REGISTERING:
        case MUSE_ST_ONLINE: case MUSE_ST_BACKOFF: case MUSE_ST_REFRESHING:
            return go(c, o, MUSE_ST_WAIT_NET, 0);
        default:                                                      /* DISABLED / UNPAIRED / WAIT_NET / PROTOCOL_ERROR */
            return o;
        }
    case MLE_MEM_LOW:
        if (s == MUSE_ST_FETCH_VM || has_session(s)) {
            c->reason = MLR_LOW_MEM;
            return go(c, o, MUSE_ST_LOW_MEM, (int32_t)MLC_LOW_MEM_RECHECK_MS);
        }
        return o;
    case MLE_MEM_OK:
        if (s == MUSE_ST_LOW_MEM) return enter_fetch(c, o);
        return o;

    /* ---------------- 配对 / 解绑 ---------------- */
    case MLE_PAIRED:
        c->paired = true;
        if (s == MUSE_ST_UNPAIRED) return enter_active(c, o);
        return o;
    case MLE_LOCAL_UNPAIR:
        if (s == MUSE_ST_DISABLED) {                                  /* 清凭据，仍关着 */
            o.act |= MLA_CLEAR_CREDS;
            c->paired = false;
            c->reason = MLR_LOCAL_UNPAIR;
            return o;
        }
        if (s == MUSE_ST_UNPAIRED) return o;                          /* 忽略 */
        return to_unpaired(c, o, MLR_LOCAL_UNPAIR, true);
    case MLE_UNPAIRED_EVT:
        if (s == MUSE_ST_REGISTERING || s == MUSE_ST_ONLINE) return to_unpaired(c, o, MLR_UNPAIRED_BY_APP, true);
        o.unexpected = true;
        return o;

    /* ---------------- VM API ---------------- */
    case MLE_VM_OK:
        if (s != MUSE_ST_FETCH_VM) break;
        c->auth_refusals = 0;                                         /* 上游 note_vm_credentials_accepted */
        c->refreshed = false;
        c->rereg_pending = false;                                     /* 注册会按此刻的权限生成命令表 */
        o.act |= MLA_CONNECT;
        return go(c, o, MUSE_ST_CONNECTING, 0);
    case MLE_VM_AUTH:
        if (s != MUSE_ST_FETCH_VM) break;
        if (!c->refreshed) {
            o.act |= MLA_REFRESH;
            return go(c, o, MUSE_ST_REFRESHING, 0);
        }
        /* 刚刷新过还被拒：新凭据不认，不是撤销（上游 app.c:986-1006）。保留配对，拉长退避 */
        if (c->auth_refusals < UINT8_MAX) c->auth_refusals++;
        c->reason = MLR_AUTH_REFUSED;
        return go(c, o, MUSE_ST_BACKOFF, (int32_t)mlc_auth_refused_backoff_ms(c->auth_refusals));
    case MLE_VM_ERR:
        if (s != MUSE_ST_FETCH_VM) break;
        return conn_backoff(c, o, now_ms, MLR_NET_ERROR);

    /* ---------------- 令牌刷新 ---------------- */
    case MLE_REFRESH_OK:
        if (s == MUSE_ST_REFRESHING) {
            o = enter_fetch(c, o);
            c->refreshed = (c->st == MUSE_ST_FETCH_VM);               /* 这一轮已经刷新过：再 401 就进新凭据被拒的退避 */
            return o;
        }
        if (s == MUSE_ST_ONLINE) return o;                            /* 后台刷新成功，会话照常 */
        break;
    case MLE_REFRESH_REJECTED:
        if (s == MUSE_ST_REFRESHING) return to_unpaired(c, o, MLR_TOKEN_REVOKED, true);   /* 被动刷新被拒 = 撤销 */
        if (s == MUSE_ST_ONLINE) return o;   /* 后台定期刷新被拒：保留旧令牌，会话照常（上游 allow_stale=true） */
        break;
    case MLE_REFRESH_ERR:
        if (s == MUSE_ST_REFRESHING) return conn_backoff(c, o, now_ms, MLR_NET_ERROR);
        if (s == MUSE_ST_ONLINE) return o;                            /* 后台刷新失败：会话照常，下个周期再试 */
        break;

    /* ---------------- 连接与注册 ---------------- */
    case MLE_WS_UP:
        if (s != MUSE_ST_CONNECTING) break;
        c->rereg_pending = false;                                     /* 这次注册就用最新的命令表 */
        o.act |= MLA_REGISTER;
        return go(c, o, MUSE_ST_REGISTERING, (int32_t)MLC_REG_TIMEOUT_MS);   /* 30 s 没应答 → 连接退避 */
    case MLE_WS_AUTH:
        if (s != MUSE_ST_CONNECTING) break;
        c->reason = MLR_WS_AUTH;                                      /* 60 s 后取新 VM 凭据，不拿同一个 bearer 重试 */
        return go(c, o, MUSE_ST_BACKOFF, (int32_t)MLC_WS_AUTH_BACKOFF_MS);
    case MLE_WS_ERR:
        if (!has_session(s)) break;
        return conn_backoff(c, o, now_ms, MLR_NET_ERROR);
    case MLE_REG_OK:
        if (s != MUSE_ST_REGISTERING) break;
        c->registrations++;
        c->reg_rejects = 0;
        c->not_before_ms = 0;
        c->online_since_ms = now_ms;
        if (c->rereg_pending) {                                       /* 注册在路上时权限变了：马上重连重注册 */
            c->rereg_pending = false;
            o.act |= MLA_CLOSE | MLA_CONNECT;
            return go(c, o, MUSE_ST_CONNECTING, 0);
        }
        return go(c, o, MUSE_ST_ONLINE, 0);
    case MLE_REG_ERR:
        if (s != MUSE_ST_REGISTERING) break;
        c->reason = MLR_REG_REJECTED;
        if (c->reg_rejects < UINT8_MAX) c->reg_rejects++;
        return go(c, o, MUSE_ST_BACKOFF, (int32_t)mlc_reg_rejected_backoff_ms(c->reg_rejects));
    case MLE_REG_PROTO:
        if (s != MUSE_ST_REGISTERING) break;
        c->reason = MLR_PROTOCOL_CHANGED;
        return go(c, o, MUSE_ST_PROTOCOL_ERROR, (int32_t)MLC_PROTOCOL_RETRY_MS);
    case MLE_REREG:
        if (s == MUSE_ST_ONLINE) {
            o.act |= MLA_CLOSE | MLA_CONNECT;                         /* 关 → CONNECTING；VM bearer 还在内存里 */
            return go(c, o, MUSE_ST_CONNECTING, 0);
        }
        if (s == MUSE_ST_CONNECTING || s == MUSE_ST_REGISTERING) c->rereg_pending = true;   /* 标记 */
        return o;                                                     /* 其余状态忽略：下次注册自然用新表 */

    /* ---------------- 调用 ---------------- */
    case MLE_INVOKE:
        if (s == MUSE_ST_ONLINE) {
            o.act |= MLA_DISPATCH;
            return o;
        }
        if (s == MUSE_ST_REGISTERING) {
            o.act |= MLA_REPLY_BUSY;
            return o;
        }
        break;

    /* ---------------- 定时 ---------------- */
    case MLE_TIMER:
        if (s == MUSE_ST_BACKOFF || s == MUSE_ST_PROTOCOL_ERROR) return enter_fetch(c, o);
        if (s == MUSE_ST_LOW_MEM) {
            if (c->mem_ok) return enter_fetch(c, o);                  /* 调用方喂 TIMER 前已 mlc_set_env 刷新过 */
            o.timer_ms = (int32_t)MLC_LOW_MEM_RECHECK_MS;             /* 60 s 复查 */
            return o;
        }
        if (s == MUSE_ST_REGISTERING) return conn_backoff(c, o, now_ms, MLR_NET_ERROR);   /* 注册应答超时 */
        return o;                                                     /* 过期的定时器：忽略 */
    case MLE_TOKEN_DUE:
        if (s == MUSE_ST_ONLINE) o.act |= MLA_BG_REFRESH;
        return o;                                                     /* 不在线时下次 FETCH 遇到 401 自然会刷新 */
    case MLE_VM_SWITCH:
        if (has_session(s)) {                                         /* 换了 VM：旧 bearer 只对旧 VM 有效，必须重取 */
            o.act |= MLA_CLOSE;
            c->rereg_pending = false;
            return enter_fetch(c, o);
        }
        return o;                                                     /* 其余状态：下一次 FETCH 自然按新 vm_id 选 */

    case MLE_COUNT:
    default:
        break;
    }
    o.unexpected = true;                                              /* 矩阵里的"—¹" */
    return o;
}

void mlc_set_env(mlc_t *c, bool net_up, bool mem_ok)
{
    if (!c) return;
    c->net_up = net_up;
    c->mem_ok = mem_ok;
}

const char *mlc_state_str(muse_state_t s)
{
    static const char *const n[MUSE_ST_COUNT] = {
        "disabled", "unpaired", "wait_net", "low_mem", "fetch_vm", "connecting", "registering", "online",
        "backoff", "refreshing", "protocol_error",
    };
    return ((unsigned)s < MUSE_ST_COUNT) ? n[s] : "unknown";
}

const char *mlc_event_str(mle_t e)
{
    static const char *const n[MLE_COUNT] = {
        "enable", "disable", "net_up", "net_down", "mem_low", "mem_ok", "paired",
        "vm_ok", "vm_auth", "vm_err", "ws_up", "ws_auth", "ws_err", "reg_ok", "reg_err", "reg_proto",
        "refresh_ok", "refresh_rejected", "refresh_err", "invoke", "unpaired_evt", "local_unpair", "rereg",
        "timer", "token_due", "vm_switch",
    };
    return ((unsigned)e < MLE_COUNT) ? n[e] : "unknown";
}

const char *mlc_reason_str(mlc_reason_t r)
{
    static const char *const n[MLR_COUNT] = {
        "", "token_revoked", "unpaired_in_app", "local_unpair", "protocol_changed", "registration_rejected",
        "credentials_refused", "ws_auth_rejected", "network_error", "low_memory",
    };
    return ((unsigned)r < MLR_COUNT) ? n[r] : "unknown";
}
