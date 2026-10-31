#pragma once
/* muse_session —— 一次 Muse 会话：TLS → WebSocket 升级 → Noise XX → 打开 /link-control 控制流 → 收发控制消息
 * （docs/15_muse_direct_design_v1.md 4.6；改写自上游 esp32/main/noise_control.cpp，e6fe517，见 UPSTREAM.md）。
 *
 * 只在会话任务（muse_link 的 muse_sess）里用，不是线程安全的：msess_run 阻塞到会话结束，期间通过 muse_link 实现的
 * 钩子（muse_link_priv.h）把事情交回去 —— 会话建立、来了一条控制消息、每圈轮询一次（处理外部事件、心跳、看门狗、推送）、
 * 请求流上来了回包。
 * 钩子里可以调 msess_send 发控制消息（同一个任务，同步发完才返回），msess_post 在请求流上发一个 POST（推送用，4.9），
 * 请求流上回来的帧经钩子 ml_session_response 交回去。
 *
 * 与上游的差别：不开隧道（MD-3）、不取 agent 身份、不订阅聊天（4.9）；请求流同步发、一条请求一帧带完（上游经队列、可分块）；WebSocket 分帧换成 muse_frame 的增量解析；
 * 控制消息跨 body chunk 重组；缓冲只从 PSRAM 分配、失败就不连（UPSTREAM.md 的 noise_core 调用方义务）；
 * 每次会话结束 ClientSession 随作用域销毁，会话密钥跟着清零。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *host;       /* noise_host（"hatch.metaaivm.com"） */
    const char *vm_id;      /* fetch_vms 选中的 VM（vm_connect_params 给的） */
    const char *bearer;     /* 同一条 VM 的 vm_auth_token：只放内存，走 WS 升级的 Authorization 头（4.5） */
} msess_conn_t;

typedef enum {
    MSESS_STOPPED = 0,      /* 钩子要求结束（状态机说要关：禁用、断网、重注册、解绑…） */
    MSESS_FAILED,           /* 连接 / 读写 / 协议错误（→ MLE_WS_ERR） */
    MSESS_AUTH_REJECTED,    /* WS 升级回 401 / 403，或 Noise 握手失败（上游同样按鉴权失败算，→ MLE_WS_AUTH） */
    MSESS_NO_MEM,           /* PSRAM 缓冲分配失败（→ MLE_MEM_LOW） */
} msess_result_t;

/* 跑一次会话，阻塞到结束。*established 报告有没有走到"控制流已打开"（调用方据此区分 WS_ERR 是连接失败还是在线断开）。 */
msess_result_t msess_run(const msess_conn_t *c, bool *established);

/* 发一条控制消息（4 字节小端长度 + JSON，按 ≤ 8 KB 分块）。只能在 msess_run 的钩子里调。失败 = 会话该断了（返回 false，
   msess_run 会在钩子返回后结束）。 */
bool msess_send(const char *json, size_t len);

#define MSESS_REQ_BASE 16   /* 请求流 id 从这里起（上游 noise_control.cpp REQ_STREAM_BASE；更小的是 Link 自己的，stream 1 是控制流） */

/* 在当前会话上发一个 POST（Daemon；头部照上游 muse_chat_link.c:507-512：x-request-id "muse-%08x-%08x"、x-app-id hatch-web、
   Content-Type / Accept application/json；body 与请求头一帧带完、end_body —— 同 Linux SDK 的 encrypt_http_request，64 KB 以内不分块）。
   只能在 msess_run 的钩子里调。返回流 id（≥ MSESS_REQ_BASE），没发出去返回 0：封装失败不算会话故障（上游 req_open 同样只丢这条），
   写 TLS 失败同 msess_send（会话随即结束）。回包经 ml_session_response 回来。 */
int64_t msess_post(const char *path, const char *json, size_t len);
/* 取消一条请求（Reset Cancelled，上游 req_pump_tx 的 Cancel 分支）；之后还可能到的帧照样经 ml_session_response 回来，按 id 丢弃。 */
bool msess_cancel(int64_t id);

#ifdef __cplusplus
}
#endif
