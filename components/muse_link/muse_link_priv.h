#pragma once
/* muse_link 组件内部：muse_session 回调 muse_link 的钩子（都在会话任务里调，同一个任务）。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Noise 握手完成、/link-control 已打开。返回 false = 结束会话。 */
bool ml_session_established(void);
/* 控制流里来了一条完整消息（json[0..len)，不以 NUL 结尾）。len 超过缓冲的已经被跳过，不会到这里。返回 false = 结束会话。 */
bool ml_session_message(const char *json, size_t len);
/* 一条超过重组缓冲的消息被整条跳过（只记日志）。 */
void ml_session_skipped(size_t len);
/* 每圈调一次（空闲时至少每 100 ms）：处理外部事件、心跳、看门狗。返回 false = 结束会话。 */
bool ml_session_poll(void);
/* 请求流（id ≥ MSESS_REQ_BASE）上的帧：status > 0 是应答头（Response 的 HTTP 状态，data 是它自带的 body），0 是后续 body 块，
   -1 是服务端复位了这条流；end = 这条流结束。请求流上的事不影响控制会话（上游 req_on_frame 的注释）。 */
void ml_session_response(int64_t id, int status, const uint8_t *data, size_t len, bool end);

#ifdef __cplusplus
}
#endif
