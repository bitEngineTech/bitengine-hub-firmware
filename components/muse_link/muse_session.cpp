/* muse_session —— 一次 Muse 会话（见 muse_session.h）。改写自上游 esp32/main/noise_control.cpp（e6fe517）的 run_session、
 * ws_upgrade、noise_handshake、flush_outbound、send_json_body_chunk；行号见各函数注释。
 *
 * H1（2026-10-22 对照 IDF v6.0.2 esp_tls.h 与 noise_core 头文件）：
 *   - esp_tls_conn_new_sync() 返回 1 成功、-1 失败（含 cfg.timeout_ms 超时）；失败后照样 esp_tls_conn_destroy()。
 *   - esp_tls_conn_read() > 0 读到字节、0 对端关闭、< 0 出错；ESP_TLS_ERR_SSL_WANT_READ / _WANT_WRITE 不是错误（非阻塞时没数据）。
 *   - esp_tls_conn_write() 返回写进去的字节数（可能少于请求的），WANT_* 时要再写。
 *   - Noise 握手 msg3 的载荷必须是空的：鉴权靠 WS 升级的 Authorization 头（上游 :805-822 的长注释，rev-proxy 会拒非 protobuf）。
 *   - ClientSession::WriteNextOutboundWebSocketPayload 的缓冲按头文件要求给足 kMaxOutboundWebSocketPayloadSize。
 *   - ProcessInboundWebSocketPayload 返回 ResourceExhausted 时会话必须断（ClientSession 不自己判失败，接着收 nonce 会错位）。
 *   - ServiceType::Daemon、/link-control 走 stream 1（上游 CTRL_STREAM_ID），控制消息是 4 字节小端长度 + JSON。 */
#include "muse_session.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/platform_util.h"

#include "muse_frame.h"
#include "muse_link_priv.h"
#include "noise_upgrade.h"
#include "ws_mask.h"

#include <xplat/noise/core/ClientSession.h>
#include <xplat/noise/core/PsaCryptoBackend.h>
#include <xplat/noise/core/ServiceCodec.h>

using namespace musegadgets::noise::core;

static const char *TAG = "muse_sess";

#define NOISE_PATH          "/v1/noise"
#define NOISE_PORT          443
#define CTRL_STREAM_ID      1
#define IO_TIMEOUT_US       (15LL * 1000 * 1000)    /* 单次读写的总时限（上游 WS_FRAME_TIMEOUT_US） */
#define TLS_TIMEOUT_MS      15000
#define KEEPALIVE_MS        60000                   /* 空闲 60 s 发一次 WS ping（4.6；上游 KEEPALIVE_INTERVAL_MS） */
#define RX_DEAD_MS          150000                  /* 这么久一个字节都没收到（ping 也没换回 pong）= 半开连接，断。上游只记日志不处理 */
#define IDLE_POLL_MS        100                     /* 空闲时最多等这么久就回去轮询一次外部事件 */
#define YIELD_EVERY_MS      50                      /* 入站数据连续不断时，最多这么久让出一次 CPU：本任务优先级 4，一直不进 idle_wait
                                                       会饿死 IDLE，直连型的任务看门狗（5 s、查 IDLE）就会 panic 整机重启（N7；第二轮审计） */
#define UPGRADE_HDR_CAP     4096                    /* 边缘对错误回 ~1.4 KB 的 Proxy-Status 头（上游 :718-725） */

/* 缓冲（全在 PSRAM，4.12；尺寸见 UPSTREAM.md 的 noise_core 调用方义务与设计 4.6 的注记） */
#define WS_HEADROOM         32                      /* 帧头放在载荷前面，一帧一次 TLS 写（上游 :149-157） */
#define WS_BUF_SIZE         ClientSession::kMaxOutboundWebSocketPayloadSize
#define RX_PAYLOAD_CAP      ClientSession::kMaxOutboundWebSocketPayloadSize   /* 入站帧同样不超过一个传输帧 + 标签 */
#define IO_CHUNK            4096                    /* 一次从 TLS 读多少 */
#define SVC_FRAME_SCRATCH   (64 * 1024)
#define OUT_SCRATCH         (12 * 1024)
#define CTRL_BODY_CHUNK_MAX 8192                    /* 出站控制消息按这个大小分块（上游 :139-145） */
#define CTL_MSG_CAP         (16 * 1024)             /* 入站控制消息重组上限；更大的整条跳过（invoke 本身 ≤ 4 KB，4.8） */

static_assert(WS_BUF_SIZE >= CTRL_BODY_CHUNK_MAX, "first chunk is staged in ws_buf");

namespace {

struct Bufs {
    uint8_t *ws = nullptr;        /* WS_HEADROOM + WS_BUF_SIZE，32 字节对齐 */
    uint8_t *rx = nullptr;        /* 一个入站 WS 帧的载荷 */
    uint8_t *io = nullptr;        /* 从 TLS 读进来的原始字节 */
    uint8_t *tf = nullptr;        /* ProcessInbound 的传输帧 scratch */
    uint8_t *sr = nullptr;        /* ProcessInbound 的跨帧重组 scratch */
    uint8_t *svc = nullptr;       /* StartOutbound* 的 service frame scratch */
    uint8_t *env = nullptr;       /* StartOutbound* 的 envelope scratch */
    uint8_t *ctl = nullptr;       /* 入站控制消息重组 */
    bool alloc()
    {
        /* 只要 PSRAM：失败就不连，不退回内部内存（4.12 的内部常驻 ≤ 2 KB） */
        const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        ws = static_cast<uint8_t *>(heap_caps_aligned_alloc(WS_HEADROOM, WS_HEADROOM + WS_BUF_SIZE, caps));
        rx = static_cast<uint8_t *>(heap_caps_malloc(RX_PAYLOAD_CAP, caps));
        io = static_cast<uint8_t *>(heap_caps_malloc(IO_CHUNK, caps));
        tf = static_cast<uint8_t *>(heap_caps_malloc(SVC_FRAME_SCRATCH, caps));
        sr = static_cast<uint8_t *>(heap_caps_malloc(SVC_FRAME_SCRATCH, caps));
        svc = static_cast<uint8_t *>(heap_caps_malloc(OUT_SCRATCH, caps));
        env = static_cast<uint8_t *>(heap_caps_malloc(OUT_SCRATCH, caps));
        ctl = static_cast<uint8_t *>(heap_caps_malloc(CTL_MSG_CAP, caps));
        return ws && rx && io && tf && sr && svc && env && ctl;
    }
    ~Bufs()
    {
        /* 解密后的明文与出站明文都在这些缓冲里：释放前清零（会话密钥本身随 ClientSession 析构清零） */
        if (rx) mbedtls_platform_zeroize(rx, RX_PAYLOAD_CAP);
        if (tf) mbedtls_platform_zeroize(tf, SVC_FRAME_SCRATCH);
        if (sr) mbedtls_platform_zeroize(sr, SVC_FRAME_SCRATCH);
        if (ctl) mbedtls_platform_zeroize(ctl, CTL_MSG_CAP);
        heap_caps_free(ws);
        heap_caps_free(rx);
        heap_caps_free(io);
        heap_caps_free(tf);
        heap_caps_free(sr);
        heap_caps_free(svc);
        heap_caps_free(env);
        heap_caps_free(ctl);
    }
};

/* 一次会话的上下文：msess_send 从钩子里调到，靠它找到当前会话 */
struct Ctx {
    esp_tls_t *tls = nullptr;
    ClientSession *session = nullptr;
    Bufs *b = nullptr;
    bool send_failed = false;
    int64_t last_activity_us = 0;    /* 收或发 */
    int64_t last_rx_us = 0;          /* 只算收 */
    int64_t last_yield_us = 0;       /* 上次让出 CPU（idle_wait 或 vTaskDelay） */
    int64_t next_req = MSESS_REQ_BASE;  /* 下一条请求流的 id（每次会话从头编号：新的 Noise 会话） */
};

Ctx *s_cur = nullptr;              /* 只在会话任务里读写 */

ssize_t write_all(esp_tls_t *tls, const void *buf, size_t len)
{
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    size_t off = 0;
    const int64_t deadline = esp_timer_get_time() + IO_TIMEOUT_US;
    while (off < len) {
        if (esp_timer_get_time() >= deadline) return -1;
        const ssize_t n = esp_tls_conn_write(tls, p + off, len - off);
        if (n > 0) {
            off += static_cast<size_t>(n);
        } else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
        } else {
            return -1;
        }
    }
    return static_cast<ssize_t>(off);
}

/* 带掩码的客户端帧；载荷在 payload，前面至少 WS_HEADROOM 字节可写（上游 ws_send_frame_inplace :353-379）。
   载荷会被原地掩码，之后就是废数据。 */
bool ws_send_inplace(esp_tls_t *tls, uint8_t opcode, uint8_t *payload, size_t len)
{
    uint8_t mask[4];
    const uint32_t r = esp_random();
    memcpy(mask, &r, 4);
    uint8_t hdr[MWS_HDR_MAX];
    const size_t hl = mws_encode_header(hdr, opcode, len, mask);
    if (hl == 0 || hl > WS_HEADROOM) return false;
    ws_mask_inplace(payload, len, mask);
    uint8_t *frame = payload - hl;
    memcpy(frame, hdr, hl);
    return write_all(tls, frame, hl + len) == static_cast<ssize_t>(hl + len);
}

/* 小帧（握手消息、pong）：拷进 ws 缓冲再发（ws 缓冲此刻是空闲的） */
bool ws_send_copy(esp_tls_t *tls, Bufs &b, uint8_t opcode, const uint8_t *p, size_t len)
{
    if (len > WS_BUF_SIZE) return false;
    uint8_t *payload = b.ws + WS_HEADROOM;
    if (len) memmove(payload, p, len);
    return ws_send_inplace(tls, opcode, payload, len);
}

/* 阻塞（有时限）收一个二进制帧，握手阶段用；ping 照回 pong。返回载荷长度，失败 -1。 */
ssize_t ws_recv_binary_blocking(esp_tls_t *tls, Bufs &b, mws_rx_t &rx, const uint8_t *pre, size_t pre_len)
{
    const int64_t deadline = esp_timer_get_time() + IO_TIMEOUT_US;
    const uint8_t *in = pre;
    size_t in_len = pre_len;
    for (;;) {
        while (in_len > 0) {
            size_t used = 0, fl = 0;
            const mws_ev_t e = mws_feed(&rx, in, in_len, &used, b.rx, RX_PAYLOAD_CAP, &fl);
            in += used;
            in_len -= used;
            if (e == MWS_NEED_MORE) continue;
            if (e == MWS_BINARY) {
                if (in_len) ESP_LOGW(TAG, "握手阶段多收了 %u 字节，丢弃", (unsigned)in_len);
                return static_cast<ssize_t>(fl);
            }
            if (e == MWS_PING) {
                if (!ws_send_copy(tls, b, MWS_OP_PONG, b.rx, fl)) return -1;
                continue;
            }
            if (e == MWS_PONG) continue;
            ESP_LOGW(TAG, "握手阶段 WS 帧错误：%d", static_cast<int>(e));
            return -1;
        }
        if (esp_timer_get_time() >= deadline) return -1;
        const ssize_t n = esp_tls_conn_read(tls, b.io, IO_CHUNK);
        if (n > 0) {
            in = b.io;
            in_len = static_cast<size_t>(n);
        } else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
        } else {
            return -1;
        }
    }
}

/* WS 升级（上游 ws_upgrade :684-776）。日志只打三位状态码：头里可能有令牌或边缘的内部信息（上游 :660-667）。
   *leftover 收升级应答之后多出来的字节（不丢，交给 WS 解析器）。 */
noise_upgrade_result_t ws_upgrade(esp_tls_t *tls, const msess_conn_t *c, Bufs &b, size_t *leftover)
{
    *leftover = 0;
    const size_t req_cap = 512 + NOISE_UPGRADE_ESCAPED_CAP(128) + strlen(c->bearer);
    char *req = static_cast<char *>(heap_caps_malloc(req_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!req) return NOISE_UPGRADE_FAILED;
    const int req_len = noise_upgrade_build_request(NOISE_PATH, c->vm_id, c->host, c->bearer, req, req_cap);
    bool sent = req_len > 0 && write_all(tls, req, static_cast<size_t>(req_len)) == req_len;
    mbedtls_platform_zeroize(req, req_cap);                 /* 里面有 bearer */
    heap_caps_free(req);
    if (req_len <= 0) {
        ESP_LOGE(TAG, "升级请求没发：参数不合格（vm_id / host / 令牌为空或带 CR/LF）");
        return NOISE_UPGRADE_FAILED;
    }
    if (!sent) {
        ESP_LOGE(TAG, "升级请求写失败");
        return NOISE_UPGRADE_FAILED;
    }
    char *hdr = reinterpret_cast<char *>(b.ws);             /* ws 缓冲此刻空闲，借来收应答头 */
    size_t have = 0, scanned = 0;
    const int64_t deadline = esp_timer_get_time() + IO_TIMEOUT_US;
    while (esp_timer_get_time() < deadline) {
        if (have >= UPGRADE_HDR_CAP) {
            const noise_upgrade_result_t r = noise_upgrade_classify(hdr, have, true);
            ESP_LOGE(TAG, "升级应答过长：%s", noise_upgrade_status_code(hdr, have) >= 0 ? "有状态码" : "不是 HTTP");
            return r;
        }
        const ssize_t n = esp_tls_conn_read(tls, hdr + have, UPGRADE_HDR_CAP - have);
        if (n > 0) {
            have += static_cast<size_t>(n);
            for (size_t i = scanned; i + 3 < have; i++) {
                if (hdr[i] == '\r' && hdr[i + 1] == '\n' && hdr[i + 2] == '\r' && hdr[i + 3] == '\n') {
                    const noise_upgrade_result_t r = noise_upgrade_classify(hdr, have, false);
                    const int st = noise_upgrade_status_code(hdr, have);
                    if (r != NOISE_UPGRADE_OK) {
                        ESP_LOGE(TAG, "升级被拒：状态 %d", st);
                    } else if (have > i + 4) {
                        *leftover = have - (i + 4);
                        memmove(b.io, hdr + i + 4, *leftover < IO_CHUNK ? *leftover : IO_CHUNK);
                        if (*leftover > IO_CHUNK) *leftover = IO_CHUNK;
                    }
                    mbedtls_platform_zeroize(hdr, have);
                    return r;
                }
            }
            scanned = have >= 3 ? have - 3 : 0;
        } else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
        } else {
            ESP_LOGE(TAG, "读升级应答出错：%d", static_cast<int>(n));
            return NOISE_UPGRADE_FAILED;
        }
    }
    ESP_LOGE(TAG, "等升级应答超时");
    return NOISE_UPGRADE_FAILED;
}

/* Noise XX（上游 noise_handshake :778-844）。msg3 的载荷是空的（见文件头 H1）。 */
bool noise_handshake(esp_tls_t *tls, ClientSession &session, Bufs &b, mws_rx_t &rx, size_t pre_len)
{
    uint8_t *payload = b.ws + WS_HEADROOM;
    const StatusWithSize m1 = session.WriteHandshakeMessage1(ByteSpan(payload, WS_BUF_SIZE));
    if (!m1.ok() || !ws_send_inplace(tls, MWS_OP_BINARY, payload, m1.size())) {
        ESP_LOGE(TAG, "握手 msg1 失败");
        return false;
    }
    const ssize_t m2 = ws_recv_binary_blocking(tls, b, rx, b.io, pre_len);
    if (m2 <= 0) {
        ESP_LOGE(TAG, "握手 msg2 没收到");
        return false;
    }
    uint8_t peer_extra[256];
    size_t peer_extra_len = 0;
    const Status s2 = session.ReadHandshakeMessage2(ConstByteSpan(b.rx, static_cast<size_t>(m2)),
                                                    ByteSpan(peer_extra, sizeof(peer_extra)), peer_extra_len);
    mbedtls_platform_zeroize(peer_extra, sizeof(peer_extra));
    if (!s2.ok()) {
        ESP_LOGE(TAG, "握手 msg2 不对：%s", s2.str());
        return false;
    }
    const StatusWithSize m3 = session.WriteHandshakeMessage3(ConstByteSpan(), ByteSpan(payload, WS_BUF_SIZE));
    if (!m3.ok() || !ws_send_inplace(tls, MWS_OP_BINARY, payload, m3.size())) {
        ESP_LOGE(TAG, "握手 msg3 失败");
        return false;
    }
    return session.isEstablished();
}

/* 把 ClientSession 里排着的出站载荷全部发出去（上游 flush_outbound :846-866） */
bool flush_outbound(Ctx &x)
{
    uint8_t *payload = x.b->ws + WS_HEADROOM;
    while (x.session->HasOutboundWebSocketPayload()) {
        const StatusWithSize s = x.session->WriteNextOutboundWebSocketPayload(ByteSpan(payload, WS_BUF_SIZE));
        if (!s.ok() || !ws_send_inplace(x.tls, MWS_OP_BINARY, payload, s.size())) return false;
    }
    x.last_activity_us = esp_timer_get_time();
    return true;
}

/* 打开 POST /link-control（stream 1，Daemon，不结束请求体：控制消息都走它的 body chunk）—— 上游 :2086-2104 */
bool open_control_stream(Ctx &x)
{
    ApplicationRequestView req;
    req.verb = StringView("POST");
    req.path = StringView("/link-control");
    req.end_body = false;
    const StatusWithSize s = x.session->StartOutboundApplicationRequest(
        ServiceType::Daemon, CTRL_STREAM_ID, req, ByteSpan(x.b->svc, OUT_SCRATCH), ByteSpan(x.b->env, OUT_SCRATCH));
    if (!s.ok()) {
        ESP_LOGE(TAG, "打开 /link-control 失败：%s", s.status().str());
        return false;
    }
    return flush_outbound(x);
}

}  // namespace

/* 一条控制消息分块发完（上游 send_json_body_chunk :890-954；那边每圈发一块好给收方向让路，我们的消息 ≤ 64 KB，同步发完） */
extern "C" bool msess_send(const char *json, size_t len)
{
    Ctx *x = s_cur;
    if (!x || !json || len == 0 || len > UINT32_MAX || x->send_failed) return false;
    size_t off = 0;
    bool first = true;
    while (off < len || first) {
        size_t pfx = first ? 4 : 0;
        size_t n = len - off;
        if (n > CTRL_BODY_CHUNK_MAX - pfx) n = CTRL_BODY_CHUNK_MAX - pfx;
        const uint8_t *data = reinterpret_cast<const uint8_t *>(json) + off;
        if (first) {
            /* 第一块要带 4 字节长度前缀：拼在 ws 缓冲里（StartOutbound 会先拷进 svc/env，flush 再覆盖 ws，没有冲突） */
            mctl_prefix(x->b->ws, static_cast<uint32_t>(len));
            memcpy(x->b->ws + 4, data, n);
            data = x->b->ws;
        }
        BodyChunkView chunk;
        chunk.data = ConstByteSpan(data, pfx + n);
        chunk.end_body = false;
        const StatusWithSize s = x->session->StartOutboundBodyChunk(
            ServiceType::Daemon, CTRL_STREAM_ID, chunk, ByteSpan(x->b->svc, OUT_SCRATCH), ByteSpan(x->b->env, OUT_SCRATCH));
        if (!s.ok() || !flush_outbound(*x)) {
            ESP_LOGE(TAG, "发控制消息失败（%u / %u 字节）", static_cast<unsigned>(off), static_cast<unsigned>(len));
            x->send_failed = true;                           /* 封装失败会毒化 Noise：不重试，会话结束 */
            return false;
        }
        off += n;
        first = false;
    }
    return true;
}

/* 请求流（上游 req_open :1110-1148 的同步版：我们只有推送一种请求，不排队） */
extern "C" int64_t msess_post(const char *path, const char *json, size_t len)
{
    Ctx *x = s_cur;
    if (!x || !path || !json || len == 0 || x->send_failed) return 0;
    char req_id[32];
    snprintf(req_id, sizeof(req_id), "muse-%08" PRIx32 "-%08" PRIx32, esp_random(), esp_random());
    const HeaderView hdrs[] = {
        { StringView("x-request-id"), StringView(req_id) },
        { StringView("x-app-id"), StringView("hatch-web") },
        { StringView("Content-Type"), StringView("application/json") },
        { StringView("Accept"), StringView("application/json") },
    };
    ApplicationRequestView req;
    req.verb = StringView("POST");
    req.path = StringView(path);
    req.headers = Span<const HeaderView>(hdrs, sizeof(hdrs) / sizeof(hdrs[0]));
    req.body = ConstByteSpan(reinterpret_cast<const uint8_t *>(json), len);
    req.end_body = true;
    const int64_t id = x->next_req++;
    const StatusWithSize s = x->session->StartOutboundApplicationRequest(
        ServiceType::Daemon, id, req, ByteSpan(x->b->svc, OUT_SCRATCH), ByteSpan(x->b->env, OUT_SCRATCH));
    if (!s.ok()) {
        ESP_LOGW(TAG, "请求 %lld（POST %s）没发出去：%s", static_cast<long long>(id), path, s.status().str());
        return 0;
    }
    if (!flush_outbound(*x)) {
        ESP_LOGE(TAG, "请求 %lld 写不出去", static_cast<long long>(id));
        x->send_failed = true;                           /* 同 msess_send：封装后写失败会毒化 Noise，会话结束 */
        return 0;
    }
    return id;
}

extern "C" bool msess_cancel(int64_t id)
{
    Ctx *x = s_cur;
    if (!x || id < MSESS_REQ_BASE || x->send_failed) return false;
    ResetView rv{ ResetCode::Cancelled, StringView("cancelled") };
    const StatusWithSize s = x->session->StartOutboundReset(
        ServiceType::Daemon, id, rv, ByteSpan(x->b->svc, OUT_SCRATCH), ByteSpan(x->b->env, OUT_SCRATCH));
    if (!s.ok()) return false;
    if (!flush_outbound(*x)) {
        x->send_failed = true;
        return false;
    }
    return true;
}

/* 等到有数据或超时（上游 idle_wait :1977-1991） */
static void idle_wait(esp_tls_t *tls, int ms)
{
    int fd = -1;
    if (esp_tls_get_bytes_avail(tls) > 0 || esp_tls_get_conn_sockfd(tls, &fd) != ESP_OK || fd < 0) {
        vTaskDelay(1);
        return;
    }
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    lwip_select(fd + 1, &rd, nullptr, nullptr, &tv);
}

enum class FrameOutcome { Continue, Stop, Fail };

/* 控制流上的一段 body 字节：重组成控制消息逐条交给钩子 */
static FrameOutcome feed_control(Ctx &x, mctl_rx_t &ctl, ConstByteSpan data)
{
    const uint8_t *p = data.data();
    size_t left = data.size();
    while (left > 0) {
        size_t used = 0, ml = 0;
        const mctl_ev_t e = mctl_feed(&ctl, p, left, &used, x.b->ctl, CTL_MSG_CAP, &ml);
        p += used;
        left -= used;
        if (e == MCTL_MESSAGE) {
            const bool go_on = ml_session_message(reinterpret_cast<const char *>(x.b->ctl), ml);
            if (x.send_failed) return FrameOutcome::Fail;
            if (!go_on) return FrameOutcome::Stop;
        } else if (e == MCTL_SKIPPED) {
            ml_session_skipped(ml);
        } else if (used == 0) {
            break;
        }
    }
    return FrameOutcome::Continue;
}

/* 处理一个解密好的服务帧：Stop = 钩子要求结束（不是故障），Fail = 服务端拒绝 / 复位 / 结束了控制流，或发送失败。
   与上游（:2223-2258）的出入：上游不看控制流的 end_body、也丢掉 Response 自带的 body。服务端结束了控制流以后这条会话
   再也收不到消息，而 WS ping 照样有 pong，RX_DEAD_MS 判不出来 —— 会"显示在线、实际聋了"，所以这里断开重连；
   Response 的 body 按控制流的字节处理（协议允许首个应答带数据）。 */
static FrameOutcome on_service_frame(Ctx &x, const DecodedServiceFrame &f, mctl_rx_t &ctl)
{
    if (f.stream_id >= MSESS_REQ_BASE) {                 /* 请求流（推送的回包）：交给 muse_link，不影响控制会话 */
        switch (f.kind) {
        case ServiceFrameKind::Response:
            ml_session_response(f.stream_id, f.response.status, f.response.body.data(), f.response.body.size(),
                                f.response.end_body);
            break;
        case ServiceFrameKind::BodyChunk:
            ml_session_response(f.stream_id, 0, f.body_chunk.data.data(), f.body_chunk.data.size(), f.body_chunk.end_body);
            break;
        case ServiceFrameKind::Reset:
            ml_session_response(f.stream_id, -1, nullptr, 0, true);
            break;
        default:
            break;
        }
        return x.send_failed ? FrameOutcome::Fail : FrameOutcome::Continue;
    }
    if (f.stream_id != CTRL_STREAM_ID) return FrameOutcome::Continue;   /* 别的流不认（不开隧道、不取身份） */
    switch (f.kind) {
    case ServiceFrameKind::BodyChunk: {
        const FrameOutcome o = feed_control(x, ctl, f.body_chunk.data);
        if (o != FrameOutcome::Continue) return o;
        if (f.body_chunk.end_body) {
            ESP_LOGW(TAG, "服务端结束了 /link-control 的 body，重连");
            return FrameOutcome::Fail;
        }
        return FrameOutcome::Continue;
    }
    case ServiceFrameKind::Response: {
        if (f.response.status != 200) {
            ESP_LOGE(TAG, "/link-control 被拒：%d", static_cast<int>(f.response.status));
            return FrameOutcome::Fail;
        }
        const FrameOutcome o = feed_control(x, ctl, f.response.body);
        if (o != FrameOutcome::Continue) return o;
        if (f.response.end_body) {
            ESP_LOGW(TAG, "/link-control 的应答直接结束了 body，重连");
            return FrameOutcome::Fail;
        }
        return FrameOutcome::Continue;
    }
    case ServiceFrameKind::Reset:
        ESP_LOGW(TAG, "服务端复位了控制流");
        return FrameOutcome::Fail;
    default:
        return FrameOutcome::Continue;
    }
}

extern "C" msess_result_t msess_run(const msess_conn_t *c, bool *established)
{
    if (established) *established = false;
    if (!c || !c->host || !c->vm_id || !c->bearer || !c->host[0] || !c->vm_id[0] || !c->bearer[0]) return MSESS_FAILED;

    Bufs b;
    if (!b.alloc()) {
        ESP_LOGE(TAG, "会话缓冲（PSRAM）分配失败，不连");
        return MSESS_NO_MEM;
    }

    esp_tls_t *tls = esp_tls_init();
    if (!tls) return MSESS_NO_MEM;
    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = TLS_TIMEOUT_MS;
    if (esp_tls_conn_new_sync(c->host, static_cast<int>(strlen(c->host)), NOISE_PORT, &cfg, tls) != 1) {
        ESP_LOGE(TAG, "TLS 连不上 %s:%d", c->host, NOISE_PORT);
        esp_tls_conn_destroy(tls);
        return MSESS_FAILED;
    }

    msess_result_t result = MSESS_FAILED;
    size_t leftover = 0;
    const noise_upgrade_result_t up = ws_upgrade(tls, c, b, &leftover);
    if (up != NOISE_UPGRADE_OK) {
        esp_tls_conn_destroy(tls);
        return up == NOISE_UPGRADE_AUTH_REJECTED ? MSESS_AUTH_REJECTED : MSESS_FAILED;
    }

    {   /* ClientSession 与后端放在会话任务的栈上（PSRAM 栈），作用域结束即析构、清会话密钥（UPSTREAM.md 调用方义务） */
        PsaCryptoBackend crypto;
        ClientSession session(crypto);
        mws_rx_t rx;
        mws_rx_init(&rx);
        Ctx x;
        x.tls = tls;
        x.session = &session;
        x.b = &b;
        x.last_activity_us = x.last_rx_us = x.last_yield_us = esp_timer_get_time();

        if (!noise_handshake(tls, session, b, rx, leftover)) {
            result = MSESS_AUTH_REJECTED;                    /* 上游同样按鉴权失败算（:2070-2073） */
            goto done;
        }
        s_cur = &x;
        if (!open_control_stream(x)) goto done;

        /* 握手完成后切非阻塞（上游 :2133-2145） */
        {
            int fd = -1;
            if (esp_tls_get_conn_sockfd(tls, &fd) != ESP_OK || fd < 0) goto done;
            const int fl = lwip_fcntl(fd, F_GETFL, 0);
            lwip_fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        }
        if (established) *established = true;
        ESP_LOGI(TAG, "会话已建立：%s（vm %s）", c->host, c->vm_id);
        if (!ml_session_established() || x.send_failed) {
            result = x.send_failed ? MSESS_FAILED : MSESS_STOPPED;   /* 注册没发出去是故障，不是"钩子要结束" */
            goto done;
        }

        {
            mctl_rx_t ctl;
            mctl_rx_init(&ctl);
            HeaderView hdr_storage[16];
            for (;;) {
                bool did = false;
                const ssize_t n = esp_tls_conn_read(tls, b.io, IO_CHUNK);
                if (n == 0) {
                    ESP_LOGI(TAG, "对端关闭了连接");
                    break;
                }
                if (n < 0 && n != ESP_TLS_ERR_SSL_WANT_READ && n != ESP_TLS_ERR_SSL_WANT_WRITE) {
                    ESP_LOGW(TAG, "读出错：%d", static_cast<int>(n));
                    break;
                }
                bool fail = false;
                const uint8_t *in = b.io;
                size_t in_len = n > 0 ? static_cast<size_t>(n) : 0;
                while (in_len > 0 && !fail) {
                    size_t used = 0, fl = 0;
                    const mws_ev_t e = mws_feed(&rx, in, in_len, &used, b.rx, RX_PAYLOAD_CAP, &fl);
                    in += used;
                    in_len -= used;
                    did = true;
                    x.last_activity_us = x.last_rx_us = esp_timer_get_time();
                    switch (e) {
                    case MWS_NEED_MORE:
                        break;
                    case MWS_PING:
                        if (!ws_send_copy(tls, b, MWS_OP_PONG, b.rx, fl)) fail = true;
                        break;
                    case MWS_PONG:
                        break;
                    case MWS_BINARY: {
                        const ClientSessionInboundResult r = session.ProcessInboundWebSocketPayload(
                            ConstByteSpan(b.rx, fl), ByteSpan(b.tf, SVC_FRAME_SCRATCH), ByteSpan(b.sr, SVC_FRAME_SCRATCH),
                            Span<HeaderView>(hdr_storage, 16));
                        if (!r.ok()) {
                            /* ResourceExhausted 也要断：接着收 nonce 会错位（UPSTREAM.md 调用方义务） */
                            ESP_LOGW(TAG, "入站帧处理失败：%s", r.status.str());
                            fail = true;
                        } else if (r.frame_status == InboundFrameStatus::Complete) {
                            const FrameOutcome o = on_service_frame(x, r.frame, ctl);
                            if (o != FrameOutcome::Continue) {
                                fail = true;
                                if (o == FrameOutcome::Stop) result = MSESS_STOPPED;   /* 钩子要求结束，不是故障 */
                            }
                        }
                        break;
                    }
                    default:
                        ESP_LOGW(TAG, "WS：%s", e == MWS_CLOSE ? "对端关闭" : e == MWS_ERR_TOO_LARGE ? "帧过大" : "协议错误");
                        fail = true;
                        break;
                    }
                }
                if (fail) {
                    if (result != MSESS_STOPPED) result = MSESS_FAILED;
                    break;
                }
                if (!session.isEstablished()) {                  /* 封装失败会永久毒化 Noise（上游 :2394-2401） */
                    ESP_LOGW(TAG, "Noise 会话已失效");
                    result = MSESS_FAILED;
                    break;
                }
                if (!ml_session_poll()) {
                    result = x.send_failed ? MSESS_FAILED : MSESS_STOPPED;
                    break;
                }
                if (x.send_failed) {
                    result = MSESS_FAILED;
                    break;
                }
                if (esp_timer_get_time() - x.last_rx_us >= RX_DEAD_MS * 1000LL) {
                    ESP_LOGW(TAG, "%d s 没收到任何字节（ping 也没回），按断线处理", RX_DEAD_MS / 1000);
                    result = MSESS_FAILED;
                    break;
                }
                if (!did && esp_timer_get_time() - x.last_activity_us >= KEEPALIVE_MS * 1000LL) {
                    if (!ws_send_copy(tls, b, MWS_OP_PING, nullptr, 0)) {
                        ESP_LOGW(TAG, "保活 ping 发不出去");
                        result = MSESS_FAILED;
                        break;
                    }
                    x.last_activity_us = esp_timer_get_time();
                    did = true;
                }
                if (!did) {
                    idle_wait(tls, IDLE_POLL_MS);
                    x.last_yield_us = esp_timer_get_time();
                } else if (esp_timer_get_time() - x.last_yield_us >= YIELD_EVERY_MS * 1000LL) {
                    vTaskDelay(1);
                    x.last_yield_us = esp_timer_get_time();
                }
            }
        }
    done:
        s_cur = nullptr;
    }   /* ClientSession / PsaCryptoBackend 析构：会话密钥清零 */

    esp_tls_conn_destroy(tls);
    return result;
}
