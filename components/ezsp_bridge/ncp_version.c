/*
 * Minimal ASH + EZSP client, used for exactly one thing: reading the MG24
 * NCP's firmware version. (Reading the Zigbee channel is NOT possible from
 * here — see the long note in ncp_version.h.)
 *
 * Every wire-format constant here was read out of the SDK that built the image
 * in mg24/release/ (Simplicity SDK v2025.6.3), not from memory — file:line
 * references are inline so the next person can re-check them. The pieces that
 * are easy to get wrong, and what the source actually says:
 *
 *   · CRC is CCITT (poly 0x1021, init 0xFFFF), transmitted HIGH byte first
 *     (platform/service/legacy_hal/src/crc.c:28, ash-common.c:98-101).
 *   · Only DATA frames have their payload randomized; RSTACK/ERROR/ACK do not
 *     (ash-ncp.c:289 and :446 are the only ashRandomizeBuffer() call sites,
 *     both on DATA; RSTACK is built as a short frame at ash-ncp.c:546).
 *   · The LFSR is seed 0x42, poly 0xB8, shifted after each byte
 *     (ash-common.c:204-215).
 *   · The FIRST `version` command must use the legacy 3-byte header; anything
 *     else uses the extended 5-byte header (ezsp.c:525-531 startCommand()).
 *   · getValue's response status is a 4-byte sl_status_t, not a 1-byte
 *     EmberStatus — the 8.x SDKs widened it (command-functions.h:233).
 *   · VERSION_INFO is 7 bytes: build u16 LE, major, minor, patch, special,
 *     type (ezsp.c:94-112 sl_zigbee_ezsp_get_version_struct()).
 *
 * This is deliberately not a general EZSP stack. It runs two transactions and
 * stops; the bridge remains a pure byte-stream pipe (docs/02, decision 1).
 */

#include "ncp_version.h"

#include "bridge_stats.h"
#include "uart_bridge.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "ncp_version";

/* ---- ASH (platform/service/legacy_common_ash/inc/ash-protocol.h:39-51) ---- */
#define ASH_FLAG        0x7E
#define ASH_ESC         0x7D
#define ASH_XON         0x11
#define ASH_XOFF        0x13
#define ASH_SUB         0x18
#define ASH_CAN         0x1A
#define ASH_WAKE        0xFF
#define ASH_FLIP        0x20

#define ASH_CTRL_ACK    0x80    /* + ackNum */
#define ASH_CTRL_NAK    0xA0
#define ASH_CTRL_RST    0xC0
#define ASH_CTRL_RSTACK 0xC1
#define ASH_CTRL_ERROR  0xC2

#define ASH_IS_DATA(c)     (((c) & 0x80) == 0)
#define ASH_GET_FRMNUM(c)  (((c) & 0x70) >> 4)

#define LFSR_SEED       0x42
#define LFSR_POLY       0xB8

/* RX must accept a full-size frame — the CRC covers the whole thing, so a
   frame cannot be validated in pieces. Data field max is EZSP_MAX_FRAME_LENGTH
   (220), plus control and 2 CRC bytes. */
#define ASH_MAX_RAW     223

/* TX is sized for what this module actually sends: the longest command is
   getValue at 6 bytes. Sizing the transmit path for 220 bytes of payload
   (plus worst-case escaping, which doubles it) would put ~700 bytes of dead
   stack on the OTA task's 4 KB for no reason. */
#define ASH_TX_MAX_DATA 16
#define ASH_TX_MAX_RAW  (ASH_TX_MAX_DATA + 3)

/* ---- EZSP (protocol/zigbee/app/util/ezsp/) ---- */
#define EZSP_PROTOCOL_WANTED    0x12    /* ezsp-protocol.h:33 */
#define EZSP_FRAME_ID_VERSION   0x0000  /* ezsp-enum.h:757 */
#define EZSP_FRAME_ID_GET_VALUE 0x00AA  /* ezsp-enum.h:766 */
/* getConfigurationValue —— 读 NCP 的表容量。H1 已核（Simplicity SDK v2025.6.3）：
     帧 ID           ezsp-enum.h:758  SL_ZIGBEE_EZSP_GET_CONFIGURATION_VALUE = 0x0052
     请求载荷        1 字节 configId（command-functions.h 的 appendInt8u）
     应答参数        **4 字节 sl_status_t**（fetchInt32u）+ 2 字节 value（fetchInt16u）
                     —— status 是 4 字节不是 1 字节，与 getValue 同一个坑
   这些是 NCP 的上电值。2026-10-06 实测：上位机多数表只能调小、抬不上去（各表是编译期定长的
   静态数组），ZHA 握手后读回一项不变；Z2M 只写直连子设备，但**每次握手都写**，值取 stack_config.json
   （默认 32，不用用户动手）—— 接 Z2M 时直连子设备不是运行值，其余表与 ZHA 下一样上电值就是运行值；
   ZHA 用户在 ezsp_config 里写了更小的值时例外（mg24/README.md「关于设备数」）。 */
#define EZSP_FRAME_ID_GET_CONFIG 0x0052  /* ezsp-enum.h:758 */
#define EZSP_VALUE_VERSION_INFO 0x11    /* ezsp-enum.h:240 */
/* customFrame（ezsp-enum.h:799）：参数 payloadLength(1) + payload；应答 sl_status_t(4) +
   replyLength(1) + reply。载荷是 XNCP 信封 <cmd lo><cmd hi><status><payload>，应答信封同形，
   rsp_id = cmd | 0x8000。0x7001 BE_SET_REGION 是本项目私有命令（docs/14 第 5.2 节），
   应答 11 字节：class cap requested host_cls gw_cls host_cc[2] gw_cc[2] clamped[2]。
   旧 NCP（无 XNCP）对 customFrame 回非 0 status + 长度 0（xncp-stubs.c）。 */
#define EZSP_FRAME_ID_CUSTOM_FRAME 0x0047
/* 0x0003 GET_BUILD_STRING（bitengine_xncp.c 的 BE_XNCP_CMD_GET_BUILD_STRING）：请求载荷空；应答信封
   <03 80><status> 之后是**原始 ASCII、无长度前缀、无 NUL**，最长 BE_XNCP_MAX_PAYLOAD(119)-3 字节。
   请求信封与 mg24/ncp/test_host/vectors.txt 的 build_string 行逐字节相同（那行由 bellows 实跑生成）。 */
#define XNCP_CMD_GET_BUILD_STRING  0x0003
#define XNCP_RSP_GET_BUILD_STRING  0x8003
#define XNCP_CMD_BE_SET_REGION     0x7001
#define XNCP_RSP_BE_SET_REGION     0xF001
#define XNCP_STATE_LEN             11
#define EZSP_EXT_FORMAT_VERSION 0x01    /* ezsp-protocol.h:151 */
#define EZSP_EXT_FORMAT_MASK    0x03    /* ezsp-protocol.h:150 */


/* Budgets. The NCP answers a RST in well under a second when healthy; these
   are ceilings for "it is not going to answer", and the whole query has to fit
   inside boot without being noticeable. */
#define ASH_RESET_TIMEOUT_MS    2500
#define EZSP_RESPONSE_TIMEOUT_MS 1500
#define READ_CHUNK_MS           50

static ncp_version_t s_cache;


static uint8_t s_frm_tx;    /* next DATA frame number we send */
static uint8_t s_frm_rx;    /* next DATA frame number we expect */
static uint8_t s_seq;       /* EZSP sequence byte */

/* ---------------------------------------------------------------- helpers */

/* Byte-at-a-time port of halCommonCrc16() (legacy_hal/src/crc.c:28). Kept
   shape-identical to the original so it can be diffed against it. */
static uint16_t crc16_byte(uint8_t new_byte, uint16_t prev)
{
    prev = (uint16_t)((prev >> 8) | (uint16_t)(prev << 8));
    prev ^= new_byte;
    prev ^= (uint16_t)((prev & 0xFF) >> 4);
    prev ^= (uint16_t)((uint16_t)(prev << 8) << 4);
    prev ^= (uint16_t)((uint8_t)((uint8_t)(prev & 0xFF) << 5)
                       | (uint16_t)((uint16_t)(uint8_t)((uint8_t)(prev & 0xFF) >> 3) << 8));
    return prev;
}

static uint16_t crc16_buf(const uint8_t *buf, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) crc = crc16_byte(buf[i], crc);
    return crc;
}

/* ash-common.c:204 ashRandomizeArray(). XOR is its own inverse, so the same
   call both randomizes on TX and recovers on RX. */
static void ash_randomize(uint8_t *buf, int len)
{
    uint8_t seed = LFSR_SEED;
    while (len--) {
        *buf++ ^= seed;
        seed = (seed & 1) ? (uint8_t)((seed >> 1) ^ LFSR_POLY) : (uint8_t)(seed >> 1);
    }
}

static bool needs_escape(uint8_t b)
{
    return b == ASH_FLAG || b == ASH_ESC || b == ASH_XON ||
           b == ASH_XOFF || b == ASH_SUB || b == ASH_CAN;
}

/* Build control + (randomized) data + CRC, byte-stuff, terminate with FLAG. */
static esp_err_t ash_send(uint8_t control, const uint8_t *data, int len)
{
    if (len < 0 || len > ASH_TX_MAX_DATA) return ESP_ERR_INVALID_ARG;

    uint8_t raw[ASH_TX_MAX_RAW];
    int n = 0;
    raw[n++] = control;
    if (len > 0) {
        memcpy(raw + n, data, (size_t)len);
        if (ASH_IS_DATA(control)) ash_randomize(raw + n, len);
        n += len;
    }
    uint16_t crc = crc16_buf(raw, n);
    raw[n++] = (uint8_t)(crc >> 8);      /* high byte first — ash-common.c:98 */
    raw[n++] = (uint8_t)(crc & 0xFF);

    uint8_t out[2 * ASH_TX_MAX_RAW + 1];   /* every byte could need escaping */
    int o = 0;
    for (int i = 0; i < n; i++) {
        if (needs_escape(raw[i])) {
            out[o++] = ASH_ESC;
            out[o++] = raw[i] ^ ASH_FLIP;
        } else {
            out[o++] = raw[i];
        }
    }
    out[o++] = ASH_FLAG;

    return uart_bridge_write(out, o, 500) == o ? ESP_OK : ESP_FAIL;
}

/*
 * Receive one valid frame.
 *
 * Returns the data-field length (>= 0) and fills @p control, or -1 on timeout.
 * Frames that fail CRC, or that the NCP marked with SUB (framing/overrun
 * error), are dropped and the wait continues until the deadline — a corrupt
 * frame is not a reason to give up while there is still time.
 */
static int ash_recv(uint8_t *control, uint8_t *data, int max_data, int timeout_ms)
{
    uint8_t raw[ASH_MAX_RAW];
    int n = 0;
    bool escaped = false;
    bool discard = false;

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() < deadline) {
        uint8_t b;
        /* One byte at a time: uart_read_bytes() waits for the full count, so
           asking for more than is coming burns the whole timeout. */
        if (uart_bridge_read(&b, 1, READ_CHUNK_MS) != 1) continue;

        /* ASH_WAKE is handled ahead of the switch, not as a case in it, so that
           inside a frame it falls through to the ordinary data path below.
           0xFF is NOT in the byte-stuffing set (ashEncodeStuffByte(),
           ash-common.c:111-125) and the SDK decoder has no case for it, so
           within a frame it is a plain data byte — and DATA payloads are
           LFSR-randomized, so any value can turn up. Skipping it unconditionally
           dropped that byte, failed the CRC and timed the transaction out.
           Because the randomizer is deterministic, whether a given response
           contains a 0xFF depends only on its content: the symptom is "this NCP
           build never reports its version", not intermittent flakiness.
           Between frames 0xFF is the wake / data-pending signal — ignore it. */
        if (b == ASH_WAKE && n == 0 && !escaped) continue;

        switch (b) {
        case ASH_CAN:                 /* cancel: drop the partial frame */
            n = 0; escaped = false; discard = false;
            continue;
        case ASH_SUB:                 /* substituted byte: frame is garbage */
            discard = true;
            continue;
        case ASH_XON:
        case ASH_XOFF:
            /* The NCP escapes these when they occur as data (ash-common.c:113),
               so an unescaped one is always out-of-band flow control. With
               RTS/CTS in use the SDK host ignores them too (ash-common.c:171). */
            continue;
        case ASH_ESC:
            escaped = true;
            continue;
        case ASH_FLAG:
            break;                    /* end of frame — handled below */
        default:
            if (n < (int)sizeof(raw)) {
                raw[n++] = escaped ? (uint8_t)(b ^ ASH_FLIP) : b;
            } else {
                discard = true;       /* oversized: keep draining to the flag */
            }
            escaped = false;
            continue;
        }

        /* ---- FLAG ---- */
        bool bad = discard || n < 3;
        int data_len = n - 3;
        if (!bad) {
            uint16_t want = (uint16_t)((raw[n - 2] << 8) | raw[n - 1]);
            if (crc16_buf(raw, n - 2) != want) {
                ESP_LOGW(TAG, "ASH CRC mismatch (%d bytes)", n);
                bad = true;
            }
        }
        if (bad) {
            n = 0; escaped = false; discard = false;
            continue;                 /* keep listening until the deadline */
        }

        if (data_len > max_data) data_len = max_data;
        *control = raw[0];
        if (data_len > 0) {
            memcpy(data, raw + 1, (size_t)data_len);
            if (ASH_IS_DATA(raw[0])) ash_randomize(data, data_len);
        }
        return data_len;
    }

    return -1;
}

/* RST → RSTACK. Resets both the NCP and our own frame counters. */
static esp_err_t ash_reset_ncp(void)
{
    uart_bridge_flush_input();

    /* Leading CAN cancels any partial frame the NCP may be mid-way through —
       this is what the stock host sends (the familiar 1A C0 38 BC 7E). */
    const uint8_t cancel = ASH_CAN;
    uart_bridge_write(&cancel, 1, 200);

    esp_err_t err = ash_send(ASH_CTRL_RST, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "UART write failed sending ASH RST");
        return err;
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ASH_RESET_TIMEOUT_MS);
    while (xTaskGetTickCount() < deadline) {
        uint8_t control;
        uint8_t data[8];
        int len = ash_recv(&control, data, sizeof(data), ASH_RESET_TIMEOUT_MS);
        if (len < 0) break;

        if (control == ASH_CTRL_RSTACK) {
            /* data = { ash version, reset reason } — ash-ncp.c:546-551 */
            ESP_LOGI(TAG, "NCP reset ack (ASH v%u, reset code 0x%02X)",
                     len > 0 ? data[0] : 0, len > 1 ? data[1] : 0);
            s_frm_tx = 0;
            s_frm_rx = 0;
            return ESP_OK;
        }
        if (control == ASH_CTRL_ERROR) {
            ESP_LOGW(TAG, "NCP sent ASH ERROR (code 0x%02X)", len > 1 ? data[1] : 0);
            return ESP_FAIL;
        }
        /* Anything else is stale traffic from before the reset — keep waiting. */
    }

    return ESP_ERR_TIMEOUT;
}

/*
 * One EZSP command/response round trip.
 *
 * A NAK means the NCP wants a retransmission; this path does not retry — a
 * failed version read is cosmetic, and a retry loop here would be more code
 * than the feature warrants. It is logged so it is not silent.
 */
static esp_err_t ezsp_txn(const uint8_t *cmd, int cmd_len,
                          uint8_t *resp, int *resp_len)
{
    uint8_t control = (uint8_t)((s_frm_tx << 4) | s_frm_rx);
    esp_err_t err = ash_send(control, cmd, cmd_len);
    if (err != ESP_OK) return err;
    s_frm_tx = (uint8_t)((s_frm_tx + 1) & 0x07);

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(EZSP_RESPONSE_TIMEOUT_MS);
    while (xTaskGetTickCount() < deadline) {
        uint8_t c;
        /* Straight into the caller's buffer: ash_recv() validates the CRC over
           its own full-size copy first and only then hands over the data field,
           so a short buffer here truncates the payload but never the check. */
        int len = ash_recv(&c, resp, *resp_len, EZSP_RESPONSE_TIMEOUT_MS);
        if (len < 0) break;

        if (ASH_IS_DATA(c)) {
            s_frm_rx = (uint8_t)((ASH_GET_FRMNUM(c) + 1) & 0x07);
            *resp_len = len;
            /* Acknowledge, or the NCP retransmits until its ACK timer gives up
               and it declares an error condition. */
            ash_send((uint8_t)(ASH_CTRL_ACK | s_frm_rx), NULL, 0);
            return ESP_OK;
        }
        if ((c & 0xE0) == ASH_CTRL_NAK) {
            ESP_LOGW(TAG, "NCP NAKed the command (control 0x%02X)", c);
            return ESP_FAIL;
        }
        if (c == ASH_CTRL_ERROR) {
            ESP_LOGW(TAG, "NCP entered ASH ERROR state");
            return ESP_FAIL;
        }
        /* ACK of our own DATA frame — the response is still coming. */
    }

    return ESP_ERR_TIMEOUT;
}

/* Response parameters start at 3 (legacy) or 5 (extended). Byte 2 is the
   frame-control high byte in extended frames and the frame ID in legacy ones;
   the SDK host disambiguates on exactly this test (ezsp.c:552). */
static int params_index(const uint8_t *resp, int len)
{
    if (len < 3) return -1;
    return ((resp[2] & EZSP_EXT_FORMAT_MASK) == EZSP_EXT_FORMAT_VERSION) ? 5 : 3;
}

/* --------------------------------------------------------------- internals */

/*
 * getValue(VERSION_INFO) — the build number, the only field that tells two
 * builds of the same EmberZNet release apart.
 *
 * Returns nothing: every failure here is non-fatal. The stack version read
 * before it is already published, and the session still has network parameters
 * to fetch — which is why this was pulled out of the main flow, where each
 * failure used to `return ESP_OK` and skip everything after it.
 */
static void read_version_detail(ncp_version_t *v)
{
    uint8_t gcmd[8] = { s_seq++, 0x00, EZSP_EXT_FORMAT_VERSION,
                        (uint8_t)(EZSP_FRAME_ID_GET_VALUE & 0xFF),
                        (uint8_t)(EZSP_FRAME_ID_GET_VALUE >> 8),
                        EZSP_VALUE_VERSION_INFO };
    uint8_t resp[64];
    int resp_len = sizeof(resp);

    esp_err_t err = ezsp_txn(gcmd, 6, resp, &resp_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "getValue(VERSION_INFO) failed (%s) — reporting stack "
                      "version only", esp_err_to_name(err));
        return;
    }

    int pi = params_index(resp, resp_len);
    /* params: sl_status_t (4, LE) + valueLength (1) + value */
    if (pi < 0 || resp_len < pi + 5) {
        ESP_LOGW(TAG, "short getValue response (%d bytes)", resp_len);
        return;
    }
    uint32_t status = (uint32_t)resp[pi] | ((uint32_t)resp[pi + 1] << 8) |
                      ((uint32_t)resp[pi + 2] << 16) | ((uint32_t)resp[pi + 3] << 24);
    uint8_t value_len = resp[pi + 4];
    if (status != 0 /* SL_STATUS_OK */ || value_len != 7 ||
        resp_len < pi + 5 + 7) {
        ESP_LOGW(TAG, "getValue(VERSION_INFO) status=0x%08" PRIx32 " len=%u",
                 status, value_len);
        return;
    }

    const uint8_t *val = resp + pi + 5;
    v->build        = (uint16_t)(val[0] | (val[1] << 8));
    v->major        = val[2];
    v->minor        = val[3];
    v->patch        = val[4];
    v->special      = val[5];
    v->type         = val[6];
    v->detail_valid = true;
    s_cache = *v;

    ESP_LOGI(TAG, "MG24 NCP: EmberZNet %u.%u.%u.%u build %u (EZSP 0x%02X)",
             v->major, v->minor, v->patch, v->special, v->build, v->ezsp_protocol);
}

/*
 * 读 NCP 的表容量。加这个是因为「Zigbee 能不能接 50 个设备」的约束**全在 MG24 侧**：
 * C5 是纯字节流透传（docs/02 决策 1），设备数对它是 0 字节开销、也是 0 可见性。
 * 加之前这些表有多大，本仓库**零记录**（当时的 NCP 全用 SDK 默认）；10-06 起表容量在
 * mg24/ncp/bitengine_ncp.slcp 里照 ZBT-2 开大，值与验证见 mg24/README.md「NCP 表容量」。
 *
 * 失败即收手：MG24 死了的时候，8 个配置项会变成 8 次超时，白白拖长启动。
 */
static void read_ncp_config(ncp_version_t *v)
{
    static const struct { uint8_t id; const char *name; uint16_t *slot; } items[] = {
        { 0x01, "packet_buffer_heap",   NULL },
        { 0x02, "neighbor_table",       NULL },
        { 0x03, "aps_unicast_msgs",     NULL },
        { 0x05, "address_table",        NULL },
        { 0x07, "route_table",          NULL },
        { 0x11, "max_end_device_children", NULL },
        { 0x1A, "source_route_table",   NULL },
        { 0x1E, "key_table",            NULL },
    };
    uint16_t *slots[] = {
        &v->cfg.packet_buffer_heap, &v->cfg.neighbor_table, &v->cfg.aps_unicast_msgs,
        &v->cfg.address_table, &v->cfg.route_table, &v->cfg.max_end_device_children,
        &v->cfg.source_route_table, &v->cfg.key_table,
    };

    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        uint8_t cmd[8] = { s_seq++, 0x00, EZSP_EXT_FORMAT_VERSION,
                           (uint8_t)(EZSP_FRAME_ID_GET_CONFIG & 0xFF),
                           (uint8_t)(EZSP_FRAME_ID_GET_CONFIG >> 8),
                           items[i].id };
        uint8_t resp[64];
        int resp_len = sizeof(resp);

        if (ezsp_txn(cmd, 6, resp, &resp_len) != ESP_OK) {
            ESP_LOGW(TAG, "getConfig(%s) 失败 —— 放弃读余下的表容量", items[i].name);
            return;                     /* 一次失败就收手，别攒 8 次超时 */
        }
        int pi = params_index(resp, resp_len);
        /* params: sl_status_t (4, LE) + value (2, LE) */
        if (pi < 0 || resp_len < pi + 6) {
            ESP_LOGW(TAG, "getConfig(%s) 应答过短 (%d)", items[i].name, resp_len);
            return;
        }
        uint32_t status = (uint32_t)resp[pi] | ((uint32_t)resp[pi + 1] << 8) |
                          ((uint32_t)resp[pi + 2] << 16) | ((uint32_t)resp[pi + 3] << 24);
        if (status != 0 /* SL_STATUS_OK */) {
            /* 单项不支持不算致命：不同 EZSP 版本的配置项集合不同 */
            ESP_LOGD(TAG, "getConfig(%s) status=0x%08" PRIx32, items[i].name, status);
            continue;
        }
        *slots[i] = (uint16_t)(resp[pi + 4] | (resp[pi + 5] << 8));
    }

    v->cfg.valid = true;
    s_cache = *v;
    ESP_LOGI(TAG, "MG24 表容量: 子设备=%u 邻居=%u 地址表=%u 源路由=%u 密钥=%u "
                  "路由表=%u APS单播=%u 缓冲堆=%u",
             v->cfg.max_end_device_children, v->cfg.neighbor_table, v->cfg.address_table,
             v->cfg.source_route_table, v->cfg.key_table, v->cfg.route_table,
             v->cfg.aps_unicast_msgs, v->cfg.packet_buffer_heap);
}

/* One NCP session: RST, EZSP version handshake, then the build number. */
/* 下发网关地区并取回 NCP 的功率状态。失败只影响 txp（xncp=false），版本读取不受影响。 */
static void sync_region(ncp_version_t *v, const char *gw_region)
{
    uint8_t cmd[16] = { s_seq++, 0x00, EZSP_EXT_FORMAT_VERSION,
                        (uint8_t)(EZSP_FRAME_ID_CUSTOM_FRAME & 0xFF),
                        (uint8_t)(EZSP_FRAME_ID_CUSTOM_FRAME >> 8),
                        0 /* payloadLength，下面填 */,
                        (uint8_t)(XNCP_CMD_BE_SET_REGION & 0xFF),
                        (uint8_t)(XNCP_CMD_BE_SET_REGION >> 8),
                        0x00 /* 信封 status */ };
    int n = 9;
    bool has_cc = gw_region && gw_region[0] && gw_region[1];
    if (has_cc) {
        cmd[n++] = (uint8_t)gw_region[0];
        cmd[n++] = (uint8_t)gw_region[1];
    }
    cmd[5] = (uint8_t)(n - 6);          /* 3 或 5 */

    v->txp.xncp = false;
    uint8_t resp[64];
    int resp_len = sizeof(resp);
    if (ezsp_txn(cmd, n, resp, &resp_len) != ESP_OK) {
        ESP_LOGW(TAG, "customFrame(BE_SET_REGION) 无应答 —— 地区未下发到 NCP");
        s_cache = *v;
        return;
    }
    int pi = params_index(resp, resp_len);
    if (pi < 0 || resp_len < pi + 5) {
        ESP_LOGW(TAG, "customFrame 应答过短 (%d)", resp_len);
        s_cache = *v;
        return;
    }
    uint32_t status = (uint32_t)resp[pi] | ((uint32_t)resp[pi + 1] << 8) |
                      ((uint32_t)resp[pi + 2] << 16) | ((uint32_t)resp[pi + 3] << 24);
    uint8_t rl = resp[pi + 4];
    if (status != 0 /* SL_STATUS_OK */ || rl < 3 || resp_len < pi + 5 + rl) {
        /* 旧 NCP 固件：桩回 INVALID_STATE、长度 0。地区这时只作用于 Thread。 */
        ESP_LOGI(TAG, "NCP 没有 XNCP（status=0x%08" PRIx32 " len=%u）—— 地区上限只作用于 Thread，"
                      "Zigbee 那颗要升级 NCP 固件", status, rl);
        s_cache = *v;
        return;
    }
    const uint8_t *r = resp + pi + 5;
    uint16_t rsp_id = (uint16_t)(r[0] | (r[1] << 8));
    if (rsp_id != XNCP_RSP_BE_SET_REGION || r[2] != 0 || rl != 3 + XNCP_STATE_LEN) {
        /* 有 XNCP 但不认这条：别家的 XNCP 固件（rsp 0xFFFF），或版本不对 */
        ESP_LOGW(TAG, "XNCP 不认 BE_SET_REGION（rsp=0x%04X st=0x%02X len=%u）", rsp_id, r[2], rl);
        s_cache = *v;
        return;
    }
    const uint8_t *st = r + 3;
    v->txp.cls           = st[0];
    v->txp.cap_dbm       = (int8_t)st[1];
    v->txp.requested_dbm = (int8_t)st[2];
    v->txp.host_cls      = st[3];
    v->txp.gw_cls        = st[4];
    /* 没声明时 NCP 发两个空格；对外一律 "" */
    v->txp.host_cc[0] = st[5] == ' ' ? '\0' : (char)st[5];
    v->txp.host_cc[1] = st[5] == ' ' ? '\0' : (char)st[6];
    v->txp.host_cc[2] = '\0';
    v->txp.gw_cc[0]   = st[7] == ' ' ? '\0' : (char)st[7];
    v->txp.gw_cc[1]   = st[7] == ' ' ? '\0' : (char)st[8];
    v->txp.gw_cc[2]   = '\0';
    v->txp.clamped_count = (uint16_t)(st[9] | (st[10] << 8));
    const bridge_stats_t *bs = bridge_stats_get();
    v->txp.read_uptime_s = bs ? bs->uptime_seconds : 0;
    v->txp.xncp = true;
    s_cache = *v;
    ESP_LOGI(TAG, "MG24 地区/功率: 网关声明=%s(cls %u) ZHA声明=%s(cls %u) 生效上限=%d dBm "
                  "栈内请求=%d dBm 钳位次数=%u",
             v->txp.gw_cc[0] ? v->txp.gw_cc : "-", v->txp.gw_cls,
             v->txp.host_cc[0] ? v->txp.host_cc : "-", v->txp.host_cls,
             v->txp.cap_dbm, v->txp.requested_dbm, v->txp.clamped_count);
}

/* 读 XNCP 构建串（T9.19 ⑦）。读不到就留空，不影响别的字段 —— 只在"没有指纹"时被云端用来比对。 */
static void read_build_string(ncp_version_t *v)
{
    v->build_string[0] = '\0';
    uint8_t cmd[9] = { s_seq++, 0x00, EZSP_EXT_FORMAT_VERSION,
                       (uint8_t)(EZSP_FRAME_ID_CUSTOM_FRAME & 0xFF),
                       (uint8_t)(EZSP_FRAME_ID_CUSTOM_FRAME >> 8),
                       3 /* payloadLength = 信封 */,
                       (uint8_t)(XNCP_CMD_GET_BUILD_STRING & 0xFF),
                       (uint8_t)(XNCP_CMD_GET_BUILD_STRING >> 8),
                       0x00 /* 信封 status */ };
    uint8_t resp[160];
    int resp_len = sizeof(resp);
    if (ezsp_txn(cmd, sizeof(cmd), resp, &resp_len) != ESP_OK) {
        ESP_LOGW(TAG, "customFrame(GET_BUILD_STRING) 无应答");
        return;
    }
    int pi = params_index(resp, resp_len);
    if (pi < 0 || resp_len < pi + 5) return;
    uint32_t status = (uint32_t)resp[pi] | ((uint32_t)resp[pi + 1] << 8) |
                      ((uint32_t)resp[pi + 2] << 16) | ((uint32_t)resp[pi + 3] << 24);
    uint8_t rl = resp[pi + 4];
    if (status != 0 || rl < 3 || resp_len < pi + 5 + rl) return;     /* 旧 NCP：没有 XNCP */
    const uint8_t *r = resp + pi + 5;
    if ((uint16_t)(r[0] | (r[1] << 8)) != XNCP_RSP_GET_BUILD_STRING || r[2] != 0) return;
    /* 外部输入：只收可打印 ASCII，遇到别的就截断（会进 JSON / 日志） */
    size_t n = 0;
    for (int i = 3; i < rl && n + 1 < sizeof(v->build_string); i++) {
        uint8_t c = r[i];
        if (c < 0x20 || c > 0x7E || c == '"' || c == '\\') break;
        v->build_string[n++] = (char)c;
    }
    v->build_string[n] = '\0';
    ESP_LOGI(TAG, "NCP 构建串: %s", v->build_string);
}

static esp_err_t ncp_session(const char *gw_region)
{
    const bridge_stats_t *st = bridge_stats_get();
    if (st && st->active_clients > 0) {
        /* Z2M/ZHA owns the ASH session; an RST here would break its handshake. */
        return ESP_ERR_INVALID_STATE;
    }

    /* Fill a local and publish it whole. HTTP handlers read s_cache from other
       tasks, and a partially written struct flagged .valid would hand them a
       version number assembled from two different firmwares. Drop the old
       reading first: it describes an NCP we are about to reset. */
    ncp_version_t v;
    memset(&v, 0, sizeof(v));
    s_cache = v;

    esp_err_t err = ash_reset_ncp();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NCP did not answer ASH RST — version unknown");
        return err;
    }

    /* ---- version (legacy frame format) ---- */
    uint8_t cmd[8] = { s_seq++, 0x00, (uint8_t)EZSP_FRAME_ID_VERSION,
                       EZSP_PROTOCOL_WANTED };
    uint8_t resp[64];
    int resp_len = sizeof(resp);

    err = ezsp_txn(cmd, 4, resp, &resp_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "EZSP version command failed (%s)", esp_err_to_name(err));
        return err;
    }

    int pi = params_index(resp, resp_len);
    if (pi < 0 || resp_len < pi + 4) {
        ESP_LOGW(TAG, "short version response (%d bytes)", resp_len);
        return ESP_ERR_INVALID_RESPONSE;
    }

    v.ezsp_protocol = resp[pi];
    v.stack_type    = resp[pi + 1];
    v.stack_version = (uint16_t)(resp[pi + 2] | (resp[pi + 3] << 8));
    v.valid         = true;

    /* stack/config/config.h:41 — major<<12 | minor<<8 | patch<<4 | special */
    v.major   = (uint8_t)((v.stack_version >> 12) & 0x0F);
    v.minor   = (uint8_t)((v.stack_version >> 8)  & 0x0F);
    v.patch   = (uint8_t)((v.stack_version >> 4)  & 0x0F);
    v.special = (uint8_t)(v.stack_version & 0x0F);
    s_cache = v;

    if (v.ezsp_protocol != EZSP_PROTOCOL_WANTED) {
        /* The NCP reports its own version regardless of what we asked for, so
           the reading above is still good. Renegotiating would be required to
           issue further commands, which is more than this feature needs. */
        ESP_LOGW(TAG, "NCP speaks EZSP 0x%02X, we asked for 0x%02X — "
                      "skipping build-number query",
                 v.ezsp_protocol, EZSP_PROTOCOL_WANTED);
        return ESP_OK;
    }

    read_version_detail(&v);
    read_ncp_config(&v);
    read_build_string(&v);
    sync_region(&v, gw_region);
    return ESP_OK;
}

/* ------------------------------------------------------------------ public */

static void (*s_persist)(const ncp_version_t *v);

void ncp_version_set_persist_hook(void (*hook)(const ncp_version_t *v))
{
    s_persist = hook;
}

esp_err_t ncp_version_query(const char *gw_region)
{
    esp_err_t err = ncp_session(gw_region);
    /* INVALID_STATE = 有客户端、什么都没发，s_cache 还是上一次的，不算新读数。
       其余情况会话真跑过：读到了就存，读失败（s_cache 已清成无效）也回调 —— 落盘那边据此作废
       旧缓存，否则下次软重启会把上一版芯片的读数当成现状（第二轮审计 A2）。 */
    if (s_persist && err != ESP_ERR_INVALID_STATE) {
        s_persist(&s_cache);
    }
    return err;
}

void ncp_version_adopt_cached(const ncp_version_t *v)
{
    if (!v || !v->valid) return;
    ncp_version_t c = *v;            /* 整份发布，理由同 ncp_session() */
    c.from_cache = true;
    c.txp.read_uptime_s = 0;
    s_cache = c;
}

const ncp_version_t *ncp_version_get(void)
{
    return &s_cache;
}

void ncp_version_format(char *buf, size_t len)
{
    if (!buf || len == 0) return;

    if (!s_cache.valid) {
        strlcpy(buf, "unknown", len);
    } else if (s_cache.detail_valid) {
        snprintf(buf, len, "%u.%u.%u.%u build %u",
                 s_cache.major, s_cache.minor, s_cache.patch,
                 s_cache.special, s_cache.build);
    } else {
        snprintf(buf, len, "%u.%u.%u.%u",
                 s_cache.major, s_cache.minor, s_cache.patch, s_cache.special);
    }
}
