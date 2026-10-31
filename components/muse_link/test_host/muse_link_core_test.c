/* muse_link_core 主机测试（docs/15 设计 4.2：状态 × 事件全排列，CLAUDE.md 编码后第 1 条）。编的是产品源文件 ../muse_link_core.c。
 *
 * 期望表照 4.2 的矩阵另写一份（不从实现推），与矩阵的出入在 muse_link_core.h 文件头逐条写着出处。
 * 每一格的环境：启用、已配对、网络在、内存够；DISABLED 时未启用，UNPAIRED 时未配对，WAIT_NET 时网断着，LOW_MEM 时内存不够；
 * ONLINE 已在线 100 s（稳定），连接退避在初始值 1 s，本轮没刷新过令牌，没有待重注册标记。 */
#include "muse_link_core.h"

#include <stdio.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) { g_pass++; } else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { int to; uint32_t act; int32_t timer; bool unexp; } cell_t;
#define STAY  -1
#define K     MLC_TIMER_KEEP
#define S              { STAY, 0, K, false }        /* 留在原状态，什么都不做 */
#define U              { STAY, 0, K, true }         /* 矩阵里的"—¹" */
#define A(a)           { STAY, (a), K, false }
#define ST(t)          { STAY, 0, (t), false }
#define T(s, a, t)     { (s), (a), (t), false }

enum { E_N = MLE_COUNT };
#define CL   MLA_CLOSE
#define CC   MLA_CLEAR_CREDS
#define F    MLA_FETCH_VM

/* 列顺序：ENABLE DISABLE NET_UP NET_DOWN MEM_LOW MEM_OK PAIRED | VM_OK VM_AUTH VM_ERR | WS_UP WS_AUTH WS_ERR |
 *         REG_OK REG_ERR REG_PROTO | REFRESH_OK REFRESH_REJECTED REFRESH_ERR | INVOKE UNPAIRED_EVT LOCAL_UNPAIR REREG TIMER TOKEN_DUE
 *         VM_SWITCH */
static const cell_t k_matrix[MUSE_ST_COUNT][E_N] = {
    [MUSE_ST_DISABLED] = {
        T(MUSE_ST_FETCH_VM, F, 0), S, S, S, S, S, S,
        U, U, U,  U, U, U,  U, U, U,  U, U, U,
        U, U, A(CC), S, S, S,
        S },
    [MUSE_ST_UNPAIRED] = {
        S, T(MUSE_ST_DISABLED, 0, 0), S, S, S, S, T(MUSE_ST_FETCH_VM, F, 0),
        U, U, U,  U, U, U,  U, U, U,  U, U, U,
        U, U, S, S, S, S,
        S },
    [MUSE_ST_WAIT_NET] = {
        S, T(MUSE_ST_DISABLED, 0, 0), T(MUSE_ST_FETCH_VM, F, 0), S, S, S, S,
        U, U, U,  U, U, U,  U, U, U,  U, U, U,
        U, U, T(MUSE_ST_UNPAIRED, CC, 0), S, S, S,
        S },
    [MUSE_ST_LOW_MEM] = {
        S, T(MUSE_ST_DISABLED, 0, 0), S, T(MUSE_ST_WAIT_NET, 0, 0), S, T(MUSE_ST_FETCH_VM, F, 0), S,
        U, U, U,  U, U, U,  U, U, U,  U, U, U,
        U, U, T(MUSE_ST_UNPAIRED, CC, 0), S, ST(60000), S,
        S },
    [MUSE_ST_FETCH_VM] = {
        S, T(MUSE_ST_DISABLED, 0, 0), S, T(MUSE_ST_WAIT_NET, 0, 0), T(MUSE_ST_LOW_MEM, 0, 60000), S, S,
        T(MUSE_ST_CONNECTING, MLA_CONNECT, 0), T(MUSE_ST_REFRESHING, MLA_REFRESH, 0), T(MUSE_ST_BACKOFF, 0, 1000),
        U, U, U,  U, U, U,  U, U, U,
        U, U, T(MUSE_ST_UNPAIRED, CC, 0), S, S, S,
        S },
    [MUSE_ST_CONNECTING] = {
        S, T(MUSE_ST_DISABLED, CL, 0), S, T(MUSE_ST_WAIT_NET, CL, 0), T(MUSE_ST_LOW_MEM, CL, 60000), S, S,
        U, U, U,
        T(MUSE_ST_REGISTERING, MLA_REGISTER, 30000), T(MUSE_ST_BACKOFF, CL, 60000), T(MUSE_ST_BACKOFF, CL, 1000),
        U, U, U,  U, U, U,
        U, U, T(MUSE_ST_UNPAIRED, CL | CC, 0), S, S, S,
        T(MUSE_ST_FETCH_VM, CL | F, 0) },
    [MUSE_ST_REGISTERING] = {
        S, T(MUSE_ST_DISABLED, CL, 0), S, T(MUSE_ST_WAIT_NET, CL, 0), T(MUSE_ST_LOW_MEM, CL, 60000), S, S,
        U, U, U,
        U, U, T(MUSE_ST_BACKOFF, CL, 1000),
        T(MUSE_ST_ONLINE, 0, 0), T(MUSE_ST_BACKOFF, CL, 60000), T(MUSE_ST_PROTOCOL_ERROR, CL, 21600000),
        U, U, U,
        A(MLA_REPLY_BUSY), T(MUSE_ST_UNPAIRED, CL | CC, 0), T(MUSE_ST_UNPAIRED, CL | CC, 0), S, T(MUSE_ST_BACKOFF, CL, 1000), S,
        T(MUSE_ST_FETCH_VM, CL | F, 0) },
    [MUSE_ST_ONLINE] = {
        S, T(MUSE_ST_DISABLED, CL, 0), S, T(MUSE_ST_WAIT_NET, CL, 0), T(MUSE_ST_LOW_MEM, CL, 60000), S, S,
        U, U, U,
        U, U, T(MUSE_ST_BACKOFF, CL, 1000),
        U, U, U,
        S, S, S,                                   /* 后台定期刷新：成功 / 被拒（保留旧令牌，上游 allow_stale）/ 出错 都不动会话 */
        A(MLA_DISPATCH), T(MUSE_ST_UNPAIRED, CL | CC, 0), T(MUSE_ST_UNPAIRED, CL | CC, 0),
        T(MUSE_ST_CONNECTING, CL | MLA_CONNECT, 0), S, A(MLA_BG_REFRESH),
        T(MUSE_ST_FETCH_VM, CL | F, 0) },
    [MUSE_ST_BACKOFF] = {
        S, T(MUSE_ST_DISABLED, 0, 0), S, T(MUSE_ST_WAIT_NET, 0, 0), S, S, S,
        U, U, U,  U, U, U,  U, U, U,  U, U, U,
        U, U, T(MUSE_ST_UNPAIRED, CC, 0), S, T(MUSE_ST_FETCH_VM, F, 0), S,
        S },
    [MUSE_ST_REFRESHING] = {
        S, T(MUSE_ST_DISABLED, 0, 0), S, T(MUSE_ST_WAIT_NET, 0, 0), S, S, S,
        U, U, U,  U, U, U,  U, U, U,
        T(MUSE_ST_FETCH_VM, F, 0), T(MUSE_ST_UNPAIRED, CC, 0), T(MUSE_ST_BACKOFF, 0, 1000),
        U, U, T(MUSE_ST_UNPAIRED, CC, 0), S, S, S,
        S },
    [MUSE_ST_PROTOCOL_ERROR] = {
        S, T(MUSE_ST_DISABLED, 0, 0), S, S, S, S, S,
        U, U, U,  U, U, U,  U, U, U,  U, U, U,
        U, U, T(MUSE_ST_UNPAIRED, CC, 0), S, T(MUSE_ST_FETCH_VM, F, 0), S,
        S },
};

static const uint64_t NOW = 1000000;

/* 定时器到点：时钟推进到退避结束（not_before）再喂 TIMER */
static mlc_out_t fire(mlc_t *c, uint64_t *t)
{
    if (c->not_before_ms > *t) *t = c->not_before_ms;
    return mlc_step(c, MLE_TIMER, *t);
}

static void ctx_for(mlc_t *c, muse_state_t s)
{
    memset(c, 0, sizeof(*c));
    c->st = s;
    c->enabled = s != MUSE_ST_DISABLED;
    c->paired = s != MUSE_ST_UNPAIRED;
    c->net_up = s != MUSE_ST_WAIT_NET;
    c->mem_ok = s != MUSE_ST_LOW_MEM;
    c->conn_backoff_ms = MLC_CONN_BACKOFF_MIN_MS;
    c->online_since_ms = NOW - 100000;
}

static void test_matrix(void)
{
    int cells = 0;
    for (int s = 0; s < MUSE_ST_COUNT; s++) {
        for (int e = 0; e < E_N; e++) {
            mlc_t c;
            ctx_for(&c, (muse_state_t)s);
            const cell_t *w = &k_matrix[s][e];
            mlc_out_t o = mlc_step(&c, (mle_t)e, NOW);
            const int want_to = w->to == STAY ? s : w->to;
            bool ok = o.from == (muse_state_t)s && o.to == (muse_state_t)want_to && c.st == (muse_state_t)want_to
                      && o.act == w->act && o.timer_ms == w->timer && o.unexpected == w->unexp;
            CHECK(ok, "%s × %s: got %s act=0x%x timer=%d unexp=%d; want %s act=0x%x timer=%d unexp=%d",
                  mlc_state_str((muse_state_t)s), mlc_event_str((mle_t)e), mlc_state_str(o.to), (unsigned)o.act, (int)o.timer_ms,
                  o.unexpected, mlc_state_str((muse_state_t)want_to), (unsigned)w->act, (int)w->timer, w->unexp);
            /* 不变量：离开有会话的状态必关；没会话的状态之间不发 CLOSE；清凭据之后不再算已配对 */
            bool from_sess = s == MUSE_ST_CONNECTING || s == MUSE_ST_REGISTERING || s == MUSE_ST_ONLINE;
            bool to_sess = c.st == MUSE_ST_CONNECTING || c.st == MUSE_ST_REGISTERING || c.st == MUSE_ST_ONLINE;
            if (from_sess && !to_sess) CHECK(o.act & MLA_CLOSE, "%s × %s leaves a session without CLOSE", mlc_state_str(s), mlc_event_str(e));
            if (!from_sess) CHECK(!(o.act & MLA_CLOSE), "%s × %s: CLOSE without a session", mlc_state_str(s), mlc_event_str(e));
            if (o.act & MLA_CLEAR_CREDS) CHECK(!c.paired, "cleared creds but still paired");
            cells++;
        }
    }
    CHECK(cells == 11 * 26, "matrix size %d", cells);
}

/* FETCH 401 → 刷新 → 再 401 = 新凭据不认（不是撤销）：退避 1/2/4/8/16/30 min，VM_OK 清零 */
static void test_auth_refused_backoff(void)
{
    mlc_t c;
    mlc_out_t o = mlc_init(&c, true, true, true, true);
    CHECK(c.st == MUSE_ST_FETCH_VM && (o.act & MLA_FETCH_VM), "init -> fetch");
    const uint32_t want[] = { 60000, 120000, 240000, 480000, 960000, 1800000, 1800000 };
    uint64_t t = NOW;
    for (int i = 0; i < 7; i++) {
        o = mlc_step(&c, MLE_VM_AUTH, t);
        CHECK(c.st == MUSE_ST_REFRESHING && (o.act & MLA_REFRESH), "round %d: first 401 -> refresh", i);
        o = mlc_step(&c, MLE_REFRESH_OK, t);
        CHECK(c.st == MUSE_ST_FETCH_VM && (o.act & MLA_FETCH_VM), "round %d: refreshed -> fetch again", i);
        o = mlc_step(&c, MLE_VM_AUTH, t);
        CHECK(c.st == MUSE_ST_BACKOFF && o.timer_ms == (int32_t)want[i] && c.paired && !(o.act & MLA_CLEAR_CREDS),
              "round %d: refused after refresh -> backoff %d (want %u), still paired", i, (int)o.timer_ms, (unsigned)want[i]);
        CHECK(c.reason == MLR_AUTH_REFUSED, "reason credentials_refused");
        o = fire(&c, &t);
        CHECK(c.st == MUSE_ST_FETCH_VM, "round %d: timer -> fetch", i);
    }
    o = mlc_step(&c, MLE_VM_OK, NOW);
    CHECK(c.auth_refusals == 0 && c.st == MUSE_ST_CONNECTING, "VM_OK resets refusals");
    CHECK(mlc_auth_refused_backoff_ms(0) == 60000 && mlc_auth_refused_backoff_ms(255) == 1800000, "backoff edges");
    /* 刷新令牌本身被拒：撤销 → 清令牌 → UNPAIRED */
    mlc_init(&c, true, true, true, true);
    mlc_step(&c, MLE_VM_AUTH, NOW);
    o = mlc_step(&c, MLE_REFRESH_REJECTED, NOW);
    CHECK(c.st == MUSE_ST_UNPAIRED && (o.act & MLA_CLEAR_CREDS) && c.reason == MLR_TOKEN_REVOKED, "refresh rejected -> token_revoked");
}

/* 连接类退避：1 → 2 → 4 → 8 → 15 → 15；在线超过 30 s 后断才回到 1 s，短会话接着加倍 */
static void test_conn_backoff(void)
{
    mlc_t c;
    mlc_init(&c, true, true, true, true);
    const int32_t want[] = { 1000, 2000, 4000, 8000, 15000, 15000 };
    uint64_t t = NOW;
    for (int i = 0; i < 6; i++) {
        mlc_out_t o = mlc_step(&c, MLE_VM_ERR, t);
        CHECK(c.st == MUSE_ST_BACKOFF && o.timer_ms == want[i], "VM_ERR #%d backoff %d want %d", i, (int)o.timer_ms, (int)want[i]);
        fire(&c, &t);
    }
    /* 连上、注册，在线 10 s 就断：不重置 */
    mlc_step(&c, MLE_VM_OK, t);
    mlc_step(&c, MLE_WS_UP, t);
    mlc_step(&c, MLE_REG_OK, t);
    t += 10000;
    mlc_out_t o = mlc_step(&c, MLE_WS_ERR, t);
    CHECK(o.timer_ms == 15000, "short session keeps backoff (%d)", (int)o.timer_ms);
    /* 在线 31 s 再断：回到 1 s */
    fire(&c, &t);
    mlc_step(&c, MLE_VM_OK, t);
    mlc_step(&c, MLE_WS_UP, t);
    mlc_step(&c, MLE_REG_OK, t + 100);
    o = mlc_step(&c, MLE_WS_ERR, t + 100 + 31000);
    CHECK(o.timer_ms == 1000 && (o.act & MLA_CLOSE), "stable session resets backoff (%d)", (int)o.timer_ms);
    /* 开关一次也重置 */
    mlc_step(&c, MLE_DISABLE, NOW);
    for (int i = 0; i < 3; i++) { mlc_step(&c, MLE_ENABLE, t + 200000); }
    o = mlc_step(&c, MLE_VM_ERR, t + 200000);
    CHECK(o.timer_ms == 1000 || c.st != MUSE_ST_BACKOFF, "enable resets backoff (%d)", (int)o.timer_ms);
}

/* 重注册：注册在路上时权限变了 → 注册成功后马上关了重连；CONNECTING 时的标记在 WS_UP 时作废 */
static void test_rereg(void)
{
    mlc_t c;
    mlc_init(&c, true, true, true, true);
    mlc_step(&c, MLE_VM_OK, NOW);
    mlc_step(&c, MLE_WS_UP, NOW);
    mlc_step(&c, MLE_REREG, NOW);
    mlc_out_t o = mlc_step(&c, MLE_REG_OK, NOW);
    CHECK(c.st == MUSE_ST_CONNECTING && (o.act & MLA_CLOSE) && (o.act & MLA_CONNECT) && c.registrations == 1, "rereg during registering");
    o = mlc_step(&c, MLE_WS_UP, NOW);
    o = mlc_step(&c, MLE_REG_OK, NOW);
    CHECK(c.st == MUSE_ST_ONLINE && c.registrations == 2, "second registration online");
    mlc_init(&c, true, true, true, true);
    mlc_step(&c, MLE_VM_OK, NOW);
    mlc_step(&c, MLE_REREG, NOW);                 /* CONNECTING：标记 */
    mlc_step(&c, MLE_WS_UP, NOW);                 /* 这次注册已经用新表：标记作废 */
    o = mlc_step(&c, MLE_REG_OK, NOW);
    CHECK(c.st == MUSE_ST_ONLINE, "mark during connecting consumed by the registration");
    o = mlc_step(&c, MLE_REREG, NOW);
    CHECK(c.st == MUSE_ST_CONNECTING && (o.act & MLA_CLOSE) && (o.act & MLA_CONNECT) && !(o.act & MLA_FETCH_VM), "online rereg reconnects without fetch");
}

/* 注册被拒：60 s 起加倍、封顶 30 min，注册成功清零；注册应答超时走连接退避 */
static void test_reg_reject_backoff_and_timeout(void)
{
    mlc_t c;
    mlc_init(&c, true, true, true, true);
    const int32_t want[] = { 60000, 120000, 240000, 480000, 960000, 1800000, 1800000 };
    uint64_t t = NOW;
    for (int i = 0; i < 7; i++) {
        mlc_step(&c, MLE_VM_OK, t);
        mlc_step(&c, MLE_WS_UP, t);
        mlc_out_t o = mlc_step(&c, MLE_REG_ERR, t);
        CHECK(c.st == MUSE_ST_BACKOFF && o.timer_ms == want[i] && (o.act & MLA_CLOSE), "reg reject #%d backoff %d", i, (int)o.timer_ms);
        fire(&c, &t);
    }
    mlc_step(&c, MLE_VM_OK, t);
    mlc_step(&c, MLE_WS_UP, t);
    mlc_step(&c, MLE_REG_OK, t);
    CHECK(c.reg_rejects == 0, "REG_OK resets reg rejects");
    t += 100000;
    mlc_step(&c, MLE_WS_ERR, t);
    fire(&c, &t);
    mlc_step(&c, MLE_VM_OK, t);
    mlc_step(&c, MLE_WS_UP, t);
    mlc_out_t o = mlc_step(&c, MLE_REG_ERR, t);
    CHECK(o.timer_ms == 60000, "after a success the next reject starts at 60 s again (%d)", (int)o.timer_ms);
    /* 注册发出后服务端不应答：30 s 定时器到点 → 关会话、连接退避 */
    mlc_init(&c, true, true, true, true);
    mlc_step(&c, MLE_VM_OK, NOW);
    o = mlc_step(&c, MLE_WS_UP, NOW);
    CHECK(o.timer_ms == 30000, "registration timer armed");
    o = mlc_step(&c, MLE_TIMER, NOW + 30000);
    CHECK(c.st == MUSE_ST_BACKOFF && (o.act & MLA_CLOSE) && o.timer_ms == 1000, "registration timeout -> close + conn backoff");
    CHECK(mlc_reg_rejected_backoff_ms(0) == 60000 && mlc_reg_rejected_backoff_ms(255) == 1800000, "reg backoff edges");
}

/* LOW_MEM 的 60 s 复查：调用方喂 TIMER 前 mlc_set_env 刷新内存，够了就走 */
static void test_low_mem_recheck(void)
{
    mlc_t c;
    mlc_init(&c, true, true, true, false);
    mlc_out_t o = mlc_step(&c, MLE_TIMER, NOW);
    CHECK(c.st == MUSE_ST_LOW_MEM && o.timer_ms == 60000, "still low: re-arm 60 s");
    mlc_set_env(&c, true, true);
    o = mlc_step(&c, MLE_TIMER, NOW);
    CHECK(c.st == MUSE_ST_FETCH_VM && (o.act & MLA_FETCH_VM), "recheck finds enough memory -> fetch");
    mlc_init(&c, true, true, true, false);
    mlc_set_env(&c, false, true);
    o = mlc_step(&c, MLE_TIMER, NOW);
    CHECK(c.st == MUSE_ST_WAIT_NET, "recheck with net down -> wait_net");
    mlc_set_env(NULL, true, true);
}

/* 计数器的清零：ENABLE、解绑都把退避与拒绝计数清掉；30 s 稳定判据的边界 */
static void test_counter_resets(void)
{
    mlc_t c;
    mlc_init(&c, true, true, true, true);
    mlc_step(&c, MLE_VM_AUTH, NOW); mlc_step(&c, MLE_REFRESH_OK, NOW); mlc_step(&c, MLE_VM_AUTH, NOW);
    CHECK(c.auth_refusals == 1, "one refusal");
    mlc_step(&c, MLE_DISABLE, NOW);
    mlc_step(&c, MLE_ENABLE, NOW);
    CHECK(c.auth_refusals == 0 && c.reg_rejects == 0 && c.conn_backoff_ms == MLC_CONN_BACKOFF_MIN_MS, "enable resets counters");
    uint64_t t = NOW;
    mlc_step(&c, MLE_VM_AUTH, t); mlc_step(&c, MLE_REFRESH_OK, t); mlc_step(&c, MLE_VM_AUTH, t);
    fire(&c, &t); mlc_step(&c, MLE_VM_ERR, t); fire(&c, &t); mlc_step(&c, MLE_VM_ERR, t);
    CHECK(c.auth_refusals == 1 && c.conn_backoff_ms > MLC_CONN_BACKOFF_MIN_MS, "counters raised");
    mlc_step(&c, MLE_LOCAL_UNPAIR, NOW);
    CHECK(c.auth_refusals == 0 && c.reg_rejects == 0 && c.conn_backoff_ms == MLC_CONN_BACKOFF_MIN_MS, "unpair resets counters");
    /* 稳定判据：恰好 30 000 ms 不算稳定，30 001 ms 算 */
    for (int extra = 0; extra <= 1; extra++) {
        mlc_init(&c, true, true, true, true);
        uint64_t tt = NOW;
        mlc_step(&c, MLE_VM_ERR, tt); fire(&c, &tt);                       /* 退避升到 2 s */
        mlc_step(&c, MLE_VM_OK, tt); mlc_step(&c, MLE_WS_UP, tt); mlc_step(&c, MLE_REG_OK, tt);
        mlc_out_t o = mlc_step(&c, MLE_WS_ERR, tt + 30000 + (uint64_t)extra);
        CHECK(o.timer_ms == (extra ? 1000 : 2000), "stable boundary +%d: %d", extra, (int)o.timer_ms);
    }
}

/* 审计 a1：退避期间断网又恢复，不许绕过退避；a2：稳定会话后不论怎么离开 ONLINE，连接退避都回到 1 s；a3：换 VM */
static void test_audit_round2(void)
{
    mlc_t c;
    /* a1：新凭据被拒 → 60 s 退避；+5 s 断网、+10 s 恢复 → 回 BACKOFF 等剩下的 50 s，不取 VM */
    mlc_init(&c, true, true, true, true);
    mlc_step(&c, MLE_VM_AUTH, NOW); mlc_step(&c, MLE_REFRESH_OK, NOW);
    mlc_out_t o = mlc_step(&c, MLE_VM_AUTH, NOW);
    CHECK(c.st == MUSE_ST_BACKOFF && o.timer_ms == 60000, "refused -> 60 s");
    mlc_step(&c, MLE_NET_DOWN, NOW + 5000);
    o = mlc_step(&c, MLE_NET_UP, NOW + 10000);
    CHECK(c.st == MUSE_ST_BACKOFF && !(o.act & MLA_FETCH_VM) && o.timer_ms == 50000, "net flap does not bypass backoff (%s, %d)", mlc_state_str(c.st), (int)o.timer_ms);
    o = mlc_step(&c, MLE_TIMER, NOW + 60000);
    CHECK(c.st == MUSE_ST_FETCH_VM && (o.act & MLA_FETCH_VM), "fetch when the backoff is due");
    /* 同样适用于注册被拒的长退避，以及经 WAIT_NET → LOW_MEM 回来的路径 */
    mlc_init(&c, true, true, true, true);
    mlc_step(&c, MLE_VM_OK, NOW); mlc_step(&c, MLE_WS_UP, NOW); mlc_step(&c, MLE_REG_ERR, NOW);   /* 60 s */
    mlc_step(&c, MLE_NET_DOWN, NOW + 500);
    mlc_step(&c, MLE_MEM_LOW, NOW + 600);
    o = mlc_step(&c, MLE_NET_UP, NOW + 1000);
    CHECK(c.st == MUSE_ST_LOW_MEM, "net back with low mem -> low_mem");
    o = mlc_step(&c, MLE_MEM_OK, NOW + 2000);
    CHECK(c.st == MUSE_ST_BACKOFF && o.timer_ms == 58000, "low-mem round trip does not bypass reg backoff (%d)", (int)o.timer_ms);
    /* a2：开机失败 3 次（退避升到 8 s）→ 在线 1 h → 拔线 → 插回后第一次失败只等 1 s */
    mlc_init(&c, true, true, true, true);
    for (int i = 0; i < 3; i++) { mlc_step(&c, MLE_VM_ERR, NOW); mlc_step(&c, MLE_TIMER, NOW + 100000); }
    CHECK(c.conn_backoff_ms == 8000, "backoff raised to 8 s (%u)", (unsigned)c.conn_backoff_ms);
    mlc_step(&c, MLE_VM_OK, NOW + 100000); mlc_step(&c, MLE_WS_UP, NOW + 100000); mlc_step(&c, MLE_REG_OK, NOW + 100000);
    mlc_step(&c, MLE_NET_DOWN, NOW + 100000 + 3600000);
    CHECK(c.conn_backoff_ms == 1000, "leaving a stable session via NET_DOWN resets backoff (%u)", (unsigned)c.conn_backoff_ms);
    mlc_step(&c, MLE_NET_UP, NOW + 100000 + 3600000 + 1);
    o = mlc_step(&c, MLE_VM_ERR, NOW + 100000 + 3600000 + 2);
    CHECK(o.timer_ms == 1000, "first failure after replug waits 1 s (%d)", (int)o.timer_ms);
    /* 短会话经 REREG 离开：不复位 */
    mlc_init(&c, true, true, true, true);
    mlc_step(&c, MLE_VM_ERR, NOW); mlc_step(&c, MLE_TIMER, NOW + 10000);
    mlc_step(&c, MLE_VM_OK, NOW + 10000); mlc_step(&c, MLE_WS_UP, NOW + 10000); mlc_step(&c, MLE_REG_OK, NOW + 10000);
    mlc_step(&c, MLE_REREG, NOW + 15000);
    CHECK(c.conn_backoff_ms == 2000, "short session left via REREG keeps backoff (%u)", (unsigned)c.conn_backoff_ms);
    /* a3：换 VM → 关会话、重取 VM（不是带着旧 bearer 重连） */
    mlc_init(&c, true, true, true, true);
    mlc_step(&c, MLE_VM_OK, NOW); mlc_step(&c, MLE_WS_UP, NOW); mlc_step(&c, MLE_REG_OK, NOW);
    o = mlc_step(&c, MLE_VM_SWITCH, NOW);
    CHECK(c.st == MUSE_ST_FETCH_VM && (o.act & MLA_CLOSE) && (o.act & MLA_FETCH_VM) && !(o.act & MLA_CONNECT), "vm switch -> close + fetch");
    /* 禁用时清掉原因 */
    mlc_step(&c, MLE_VM_ERR, NOW);
    CHECK(c.reason == MLR_NET_ERROR, "reason set");
    mlc_step(&c, MLE_DISABLE, NOW);
    CHECK(c.reason == MLR_NONE, "disable clears reason");
}

static void test_init_and_strings(void)
{
    mlc_t c;
    mlc_out_t o = mlc_init(&c, false, true, true, true);
    CHECK(c.st == MUSE_ST_DISABLED && o.act == 0, "init disabled");
    o = mlc_init(&c, true, false, true, true);
    CHECK(c.st == MUSE_ST_UNPAIRED && o.act == 0, "init unpaired");
    o = mlc_init(&c, true, true, false, true);
    CHECK(c.st == MUSE_ST_WAIT_NET && o.act == 0, "init wait_net");
    o = mlc_init(&c, true, true, true, false);
    CHECK(c.st == MUSE_ST_LOW_MEM && o.timer_ms == 60000 && c.reason == MLR_LOW_MEM, "init low_mem");
    o = mlc_init(&c, true, true, true, true);
    CHECK(c.st == MUSE_ST_FETCH_VM && o.act == MLA_FETCH_VM, "init fetch");
    /* 网在 WAIT_NET 时内存不够：NET_UP 进 LOW_MEM；BACKOFF 到点时网断了：回 WAIT_NET */
    mlc_init(&c, true, true, false, true);
    mlc_step(&c, MLE_MEM_LOW, NOW);
    o = mlc_step(&c, MLE_NET_UP, NOW);
    CHECK(c.st == MUSE_ST_LOW_MEM, "net up with low mem -> low_mem");
    mlc_init(&c, true, true, true, true);
    mlc_step(&c, MLE_VM_ERR, NOW);
    c.net_up = false;
    o = mlc_step(&c, MLE_TIMER, NOW);
    CHECK(c.st == MUSE_ST_WAIT_NET && !(o.act & MLA_FETCH_VM), "backoff timer with net down -> wait_net");
    /* PROTOCOL_ERROR 期间断网不动，定时器到点时网在才去取 */
    mlc_init(&c, true, true, true, true);
    mlc_step(&c, MLE_VM_OK, NOW); mlc_step(&c, MLE_WS_UP, NOW);
    o = mlc_step(&c, MLE_REG_PROTO, NOW);
    CHECK(c.st == MUSE_ST_PROTOCOL_ERROR && c.reason == MLR_PROTOCOL_CHANGED && o.timer_ms == 21600000, "protocol error 6 h");
    for (int s = 0; s < MUSE_ST_COUNT; s++) CHECK(strcmp(mlc_state_str(s), "unknown") != 0, "state str %d", s);
    for (int e = 0; e < MLE_COUNT; e++) CHECK(strcmp(mlc_event_str(e), "unknown") != 0, "event str %d", e);
    for (int r = 0; r < MLR_COUNT; r++) CHECK(strcmp(mlc_reason_str(r), "unknown") != 0, "reason str %d", r);
    CHECK(strcmp(mlc_state_str(MUSE_ST_COUNT), "unknown") == 0 && strcmp(mlc_event_str(MLE_COUNT), "unknown") == 0, "out of range strs");
    o = mlc_step(NULL, MLE_TIMER, NOW);
    CHECK(o.unexpected, "NULL ctx");
    o = mlc_step(&c, (mle_t)99, NOW);
    CHECK(o.unexpected && c.st == MUSE_ST_PROTOCOL_ERROR, "unknown event is unexpected, no change");
}

int main(void)
{
    test_matrix();
    test_auth_refused_backoff();
    test_conn_backoff();
    test_rereg();
    test_reg_reject_backoff_and_timeout();
    test_low_mem_recheck();
    test_counter_resets();
    test_audit_round2();
    test_init_and_strings();
    printf("muse_link_core: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
