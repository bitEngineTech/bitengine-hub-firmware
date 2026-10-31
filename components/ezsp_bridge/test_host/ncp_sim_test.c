/*
 * 把真正的 components/ezsp_bridge/ncp_version.c 编到主机上，对着一个模拟 MG24 跑。
 * 模拟侧的 CRC / LFSR 用的是 SDK 原版函数（从 mg24-fw/sdk 逐字复制），
 * 与被测代码是两份独立实现——对得上才算数。
 *
 * 覆盖：RST→RSTACK 握手、DATA 帧序号/ACK、legacy vs extended 响应格式判别、
 *       sl_status_t 4 字节宽度、VERSION_INFO 7 字节布局、NCP 无响应、CRC 损坏。
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

#include "ncp_version.h"
#include "bridge_stats.h"

/* ==================== 桩：时间 / 统计 / 错误名 ==================== */
uint32_t xTaskGetTickCount(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static bridge_stats_t g_stats;
bridge_stats_t *bridge_stats_get(void) { return &g_stats; }

const char *esp_err_to_name(esp_err_t e)
{
    switch (e) {
    case 0: return "ESP_OK";
    case 0x107: return "ESP_ERR_TIMEOUT";
    case 0x103: return "ESP_ERR_INVALID_STATE";
    case 0x108: return "ESP_ERR_INVALID_RESPONSE";
    default: return "ESP_FAIL";
    }
}

/* ==================== SDK 原版实现（模拟 NCP 用）==================== */
static uint16_t sdk_crc16(uint8_t newByte, uint16_t prevResult)
{
  prevResult = ((uint16_t) (prevResult >> 8)) | ((uint16_t) (prevResult << 8));
  prevResult ^= newByte;
  prevResult ^= (prevResult & 0xff) >> 4;
  prevResult ^= (uint16_t) (((uint16_t) (prevResult << 8)) << 4);
  prevResult ^= ((uint8_t) (((uint8_t) (prevResult & 0xff)) << 5))
                | ((uint16_t) ((uint16_t) ((uint8_t) (((uint8_t) (prevResult & 0xff)) >> 3)) << 8));
  return prevResult;
}
static uint8_t sdk_randomize(uint8_t seed, uint8_t *buf, uint8_t len)
{
  if (seed == 0) seed = 0x42;
  while (len--) {
    *buf++ ^= seed;
    seed = (seed & 1) ? ((seed >> 1) ^ 0xB8) : (seed >> 1);
  }
  return seed;
}

/* ==================== 模拟 MG24 ==================== */
#define BUFSZ 4096
static uint8_t ncp_tx[BUFSZ]; static int ncp_tx_len, ncp_tx_rd;   /* NCP → 主机 */
static uint8_t host_frame[BUFSZ]; static int host_frame_len;      /* 正在收的主机帧 */

/* 故障注入开关 */
#define SIM_BUILD_STRING "bitEngine-xncp/1.0 v0.5.0-418-gc9ac5d1 2026-10-07 nvm3c=400"
static bool sim_dead;            /* NCP 完全不响应 */
static bool sim_corrupt_crc;     /* 响应 CRC 故意写错 */
static bool sim_legacy_getvalue; /* getValue 用 legacy 格式回（不该发生，测健壮性）*/
static bool sim_no_getvalue;     /* getValue 超时不回 */
static bool sim_xncp;            /* NCP 带我们的 XNCP（customFrame 0x7001 能答）*/
static bool sim_xncp_foreign;    /* 有 XNCP 但不认 0x7001（回 FF FF 2D）*/
static uint8_t sim_last_custom[32];   /* 最近一次收到的 customFrame 载荷（XNCP 信封）*/
static int     sim_last_custom_len;
static int  ncp_frm_tx;          /* NCP 侧 DATA 帧序号 */

static void ncp_emit(uint8_t control, const uint8_t *data, int len)
{
    uint8_t raw[512]; int n = 0;
    raw[n++] = control;
    if (len) {
        memcpy(raw + n, data, len);
        if ((control & 0x80) == 0) sdk_randomize(0, raw + n, (uint8_t)len);
        n += len;
    }
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < n; i++) crc = sdk_crc16(raw[i], crc);
    if (sim_corrupt_crc) crc ^= 0xFFFF;
    raw[n++] = (uint8_t)(crc >> 8);
    raw[n++] = (uint8_t)(crc & 0xFF);

    for (int i = 0; i < n; i++) {
        uint8_t b = raw[i];
        if (b == 0x7E || b == 0x7D || b == 0x11 || b == 0x13 || b == 0x18 || b == 0x1A) {
            ncp_tx[ncp_tx_len++] = 0x7D; ncp_tx[ncp_tx_len++] = b ^ 0x20;
        } else ncp_tx[ncp_tx_len++] = b;
    }
    ncp_tx[ncp_tx_len++] = 0x7E;
}

/* 处理一个收齐的主机帧（已去 FLAG） */
static void ncp_on_frame(const uint8_t *stuffed, int slen)
{
    uint8_t raw[512]; int n = 0; bool esc = false;
    for (int i = 0; i < slen; i++) {
        if (stuffed[i] == 0x7D) { esc = true; continue; }
        raw[n++] = esc ? (stuffed[i] ^ 0x20) : stuffed[i];
        esc = false;
    }
    if (n < 3) return;
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < n - 2; i++) crc = sdk_crc16(raw[i], crc);
    if (crc != (uint16_t)((raw[n-2] << 8) | raw[n-1])) {
        printf("   [sim] 主机帧 CRC 错误，丢弃\n");
        return;
    }
    uint8_t control = raw[0];
    int dlen = n - 3;
    uint8_t data[512];
    memcpy(data, raw + 1, dlen);
    if ((control & 0x80) == 0) sdk_randomize(0, data, (uint8_t)dlen);

    if (sim_dead) return;

    if (control == 0xC0) {                     /* RST */
        printf("   [sim] 收到 RST → 回 RSTACK\n");
        ncp_frm_tx = 0;
        uint8_t rst[2] = { 0x02, 0x0B };       /* ash 版本, 复位原因 */
        ncp_emit(0xC1, rst, 2);
        return;
    }
    if ((control & 0x80) == 0) {               /* DATA */
        uint8_t frm = (control >> 4) & 7;
        ncp_emit((uint8_t)(0x80 | ((frm + 1) & 7)), NULL, 0);   /* ACK */

        uint8_t seq = data[0];
        bool extended = (data[1 + 1] & 0x03) == 0x01;  /* data[2] 是 FC HB 或 frameID */
        uint16_t frame_id = extended ? (uint16_t)(data[3] | (data[4] << 8)) : data[2];
        printf("   [sim] 收到 EZSP 命令 id=0x%04X (%s格式)\n",
               frame_id, extended ? "extended" : "legacy");

        uint8_t resp[160]; int rn = 0;   /* 构建串应答 ~70 字节（T9.19 ⑦） */
        if (frame_id == 0x0000 && !extended) {
            /* version：legacy 格式回，参数 protocolVersion/stackType/stackVersion */
            resp[rn++] = seq; resp[rn++] = 0x80; resp[rn++] = 0x00;
            resp[rn++] = 0x12;                 /* EZSP 18 */
            resp[rn++] = 0x02;                 /* mesh */
            resp[rn++] = 0x20; resp[rn++] = 0x82;  /* 0x8220 = 8.2.2.0 (LE) */
        } else if (frame_id == 0x00AA) {
            if (sim_no_getvalue) { printf("   [sim] 故意不回 getValue\n"); return; }
            if (sim_legacy_getvalue) {
                resp[rn++] = seq; resp[rn++] = 0x80; resp[rn++] = 0xAA;
            } else {
                resp[rn++] = seq; resp[rn++] = 0x80; resp[rn++] = 0x01;
                resp[rn++] = 0xAA; resp[rn++] = 0x00;
            }
            resp[rn++] = 0; resp[rn++] = 0; resp[rn++] = 0; resp[rn++] = 0; /* SL_STATUS_OK, 4 字节 */
            resp[rn++] = 7;                    /* valueLength */
            resp[rn++] = 0x14; resp[rn++] = 0x02;  /* build 532 LE */
            resp[rn++] = 8; resp[rn++] = 2; resp[rn++] = 2; resp[rn++] = 0;
            resp[rn++] = 0xAA;                 /* type */
        } else if (frame_id == 0x0047 && extended) {
            /* customFrame：参数 payloadLength(1) + payload；应答 sl_status_t(4) + replyLength(1) + reply */
            uint8_t plen = data[5];
            sim_last_custom_len = plen > (uint8_t)sizeof(sim_last_custom) ? (int)sizeof(sim_last_custom) : plen;
            memcpy(sim_last_custom, data + 6, (size_t)sim_last_custom_len);
            resp[rn++] = seq; resp[rn++] = 0x80; resp[rn++] = 0x01; resp[rn++] = 0x47; resp[rn++] = 0x00;
            if (!sim_xncp) {
                printf("   [sim] 旧固件：customFrame 回 INVALID_STATE + 长度 0\n");
                resp[rn++] = 0x02; resp[rn++] = 0; resp[rn++] = 0; resp[rn++] = 0;   /* SL_STATUS_INVALID_STATE */
                resp[rn++] = 0;
            } else if (!sim_xncp_foreign && plen >= 3 && data[6] == 0x03 && data[7] == 0x00) {
                /* 0x0003 GET_BUILD_STRING：信封后跟原始 ASCII，无长度前缀（bitengine_xncp.c） */
                static const char bs[] = SIM_BUILD_STRING;
                printf("   [sim] GET_BUILD_STRING → %s\n", bs);
                resp[rn++] = 0; resp[rn++] = 0; resp[rn++] = 0; resp[rn++] = 0;
                resp[rn++] = (uint8_t)(3 + sizeof(bs) - 1);
                resp[rn++] = 0x03; resp[rn++] = 0x80; resp[rn++] = 0x00;
                memcpy(resp + rn, bs, sizeof(bs) - 1); rn += (int)sizeof(bs) - 1;
            } else if (sim_xncp_foreign || plen < 3 || data[6] != 0x01 || data[7] != 0x70) {
                printf("   [sim] XNCP 不认这条 → FF FF 2D\n");
                resp[rn++] = 0; resp[rn++] = 0; resp[rn++] = 0; resp[rn++] = 0;
                resp[rn++] = 3; resp[rn++] = 0xFF; resp[rn++] = 0xFF; resp[rn++] = 0x2D;
            } else {
                /* 0x7001 BE_SET_REGION：按 docs/14 第 5.2 节回 11 字节；host 未声明、栈内请求 8 */
                uint8_t gw_cls = 0; char c1 = ' ', c2 = ' ';
                if (plen == 5) {
                    /* data[6..8] = 信封 cmd(2)+status(1)，国家码在 data[9..10] */
                    c1 = (char)data[9]; c2 = (char)data[10];
                    if ((c1 == 'U' && c2 == 'S') || (c1 == 'C' && c2 == 'A')) gw_cls = 3;
                    else if (c1 == 'E' && c2 == 'U') gw_cls = 2;
                    else gw_cls = 1;
                }
                uint8_t cap = gw_cls == 3 ? 19 : gw_cls == 1 ? 8 : 10;
                printf("   [sim] BE_SET_REGION %c%c → gw_cls=%u cap=%u\n", c1, c2, gw_cls, cap);
                resp[rn++] = 0; resp[rn++] = 0; resp[rn++] = 0; resp[rn++] = 0;
                resp[rn++] = 3 + 11;
                resp[rn++] = 0x01; resp[rn++] = 0xF0; resp[rn++] = 0x00;
                resp[rn++] = gw_cls; resp[rn++] = cap; resp[rn++] = 8; resp[rn++] = 0; resp[rn++] = gw_cls;
                resp[rn++] = ' '; resp[rn++] = ' '; resp[rn++] = (uint8_t)c1; resp[rn++] = (uint8_t)c2;
                resp[rn++] = 0; resp[rn++] = 0;
            }
        } else {
            return;
        }
        ncp_emit((uint8_t)((ncp_frm_tx << 4) | ((frm + 1) & 7)), resp, rn);
        ncp_frm_tx = (ncp_frm_tx + 1) & 7;
    }
}

/* ==================== 桩：UART ==================== */
int uart_bridge_write(const uint8_t *buf, int len, int timeout_ms)
{
    (void)timeout_ms;
    for (int i = 0; i < len; i++) {
        if (buf[i] == 0x1A) { host_frame_len = 0; continue; }   /* CAN */
        if (buf[i] == 0x7E) {
            if (host_frame_len) ncp_on_frame(host_frame, host_frame_len);
            host_frame_len = 0;
            continue;
        }
        if (host_frame_len < BUFSZ) host_frame[host_frame_len++] = buf[i];
    }
    return len;
}

int uart_bridge_read(uint8_t *buf, int max_len, int timeout_ms)
{
    (void)timeout_ms;
    int n = 0;
    while (n < max_len && ncp_tx_rd < ncp_tx_len) buf[n++] = ncp_tx[ncp_tx_rd++];
    if (n == 0) {
        struct timespec ts = { 0, 2 * 1000 * 1000 };   /* 2ms，别空转烧 CPU */
        nanosleep(&ts, NULL);
    }
    return n;
}

void uart_bridge_flush_input(void) { ncp_tx_rd = ncp_tx_len = 0; }

/* ==================== 测试 ==================== */
static int fails;
static void check(const char *name, bool ok)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) fails++;
}

static void reset_sim(void)
{
    ncp_tx_len = ncp_tx_rd = host_frame_len = ncp_frm_tx = 0;
    sim_dead = sim_corrupt_crc = sim_legacy_getvalue = sim_no_getvalue = false;
    sim_xncp = sim_xncp_foreign = false;
    sim_last_custom_len = 0;
    g_stats.uptime_seconds = 0;
    g_stats.active_clients = 0;
    g_stats.tcp_connects   = 0;
}

/* 与 mg24/ncp/test_host/vectors.txt 共用样例：C5 发出的 XNCP 信封必须逐字节等于那一行的请求。
   两侧各自的实现都对着同一份字节，这就是"两侧共用样例"的意义（docs/14 第 5.4 节）。 */
#define VECTORS_PATH "../../../mg24/ncp/test_host/vectors.txt"
static bool vec_matches(const char *name, const uint8_t *got, int got_len)
{
    FILE *f = fopen(VECTORS_PATH, "r");
    if (!f) { printf("   [vec] 打不开 %s\n", VECTORS_PATH); return false; }
    char line[512];
    bool found = false, ok = false;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] != 'V') continue;
        char nm[64] = {0};
        if (sscanf(line + 1, " %63s", nm) != 1 || strcmp(nm, name) != 0) continue;
        found = true;
        const char *bar1 = strchr(line, '|'); if (!bar1) break;
        const char *bar2 = strchr(bar1 + 1, '|'); if (!bar2) break;
        uint8_t want[64]; int wl = 0;
        for (const char *q = bar1 + 1; q < bar2; ) {
            while (q < bar2 && (*q == ' ' || *q == '\t')) q++;
            if (q >= bar2) break;
            unsigned v; if (sscanf(q, "%2x", &v) != 1) break;
            want[wl++] = (uint8_t)v; q += 2;
        }
        ok = (wl == got_len) && memcmp(want, got, (size_t)wl) == 0;
        if (!ok) {
            printf("   [vec] %s: got", name); for (int i = 0; i < got_len; i++) printf(" %02X", got[i]);
            printf(" want"); for (int i = 0; i < wl; i++) printf(" %02X", want[i]); printf("\n");
        }
        break;
    }
    fclose(f);
    if (!found) printf("   [vec] vectors.txt 里没有 %s\n", name);
    return ok;
}

static int g_persist_calls;
static ncp_version_t g_persisted;
static void persist_hook(const ncp_version_t *v) { g_persist_calls++; g_persisted = *v; }

int main(void)
{
    char buf[64];

    /* ---- 1. 正常路径 ---- */
    printf("\n=== 1. 正常路径（EmberZNet 8.2.2.0 build 532）===\n");
    reset_sim();
    esp_err_t err = ncp_version_query(NULL);
    const ncp_version_t *v = ncp_version_get();
    ncp_version_format(buf, sizeof(buf));
    printf("   → \"%s\"\n", buf);
    check("查询成功", err == ESP_OK);
    check("valid + detail_valid", v->valid && v->detail_valid);
    check("EZSP 协议版本 = 0x12", v->ezsp_protocol == 0x12);
    check("stack_type = 0x02", v->stack_type == 0x02);
    check("stack_version 原始值 = 0x8220", v->stack_version == 0x8220);
    check("版本号 = 8.2.2.0", v->major==8 && v->minor==2 && v->patch==2 && v->special==0);
    check("build = 532", v->build == 532);
    check("格式化 = \"8.2.2.0 build 532\"", strcmp(buf, "8.2.2.0 build 532") == 0);

    /* ---- 2. 有 TCP 客户端时必须拒绝（否则会打断 Z2M 握手）---- */
    printf("\n=== 2. 有客户端连着时拒绝查询 ===\n");
    reset_sim();
    g_stats.active_clients = 1;
    err = ncp_version_query(NULL);
    check("返回 INVALID_STATE", err == ESP_ERR_INVALID_STATE);
    /* 拒绝 = 什么都没碰，上一次读到的版本依然成立，不该被抹掉 */
    check("拒绝时保留既有缓存", ncp_version_get()->valid);

    /* ---- 3. NCP 无响应 ---- */
    printf("\n=== 3. NCP 不应答 ===\n");
    reset_sim();
    sim_dead = true;
    err = ncp_version_query(NULL);
    ncp_version_format(buf, sizeof(buf));
    check("返回超时", err == ESP_ERR_TIMEOUT);
    check("格式化为 \"unknown\"", strcmp(buf, "unknown") == 0);

    /* ---- 4. getValue 超时：仍应报出 stack 版本 ---- */
    printf("\n=== 4. getValue 无响应，降级只报 stack 版本 ===\n");
    reset_sim();
    sim_no_getvalue = true;
    err = ncp_version_query(NULL);
    v = ncp_version_get();
    ncp_version_format(buf, sizeof(buf));
    printf("   → \"%s\"\n", buf);
    check("整体仍算成功", err == ESP_OK);
    check("valid 但 detail_valid=false", v->valid && !v->detail_valid);
    check("降级格式 = \"8.2.2.0\"", strcmp(buf, "8.2.2.0") == 0);
    /* ---- 5. 响应 CRC 损坏 ---- */
    printf("\n=== 5. 响应 CRC 损坏 ===\n");
    reset_sim();
    sim_corrupt_crc = true;
    err = ncp_version_query(NULL);
    check("不接受坏帧（超时而非误判）", err != ESP_OK);
    check("缓存无效", !ncp_version_get()->valid);

    /* ---- 6. getValue 意外用 legacy 格式回（越界解析防护）---- */
    printf("\n=== 6. getValue 用 legacy 格式回 ===\n");
    reset_sim();
    sim_legacy_getvalue = true;
    err = ncp_version_query(NULL);
    v = ncp_version_get();
    ncp_version_format(buf, sizeof(buf));
    printf("   → \"%s\"\n", buf);
    check("不崩溃，stack 版本仍有效", err == ESP_OK && v->valid);

    /* ---- 7. XNCP：下发网关地区 US，取回功率状态（docs/14 第 7.5 节）---- */
    printf("\n=== 7. XNCP 下发地区 US ===\n");
    reset_sim();
    sim_xncp = true;
    g_stats.uptime_seconds = 77;
    err = ncp_version_query("US");
    v = ncp_version_get();
    check("查询成功", err == ESP_OK);
    check("txp.xncp = true", v->txp.xncp);
    check("构建串读到了（T9.19 ⑦）", strcmp(v->build_string, SIM_BUILD_STRING) == 0);
    check("发出的信封 = vectors.txt 的 set_region_US", vec_matches("set_region_US", sim_last_custom, sim_last_custom_len));
    check("gw_cls=FCC gw_cc=\"US\" cap=19", v->txp.gw_cls == 3 && strcmp(v->txp.gw_cc, "US") == 0 && v->txp.cap_dbm == 19);
    check("host 未声明 → host_cc 空串、host_cls=0", v->txp.host_cc[0] == '\0' && v->txp.host_cls == 0);
    check("requested_dbm=8（栈内请求值）", v->txp.requested_dbm == 8);
    check("read_uptime_s 取自 bridge_stats", v->txp.read_uptime_s == 77);

    /* ---- 8. 清除网关声明（没设地区时每次开机也发）---- */
    printf("\n=== 8. 清除网关声明 ===\n");
    reset_sim();
    sim_xncp = true;
    err = ncp_version_query(NULL);
    v = ncp_version_get();
    check("查询成功", err == ESP_OK);
    check("发出的信封 = vectors.txt 的 set_region_clear", vec_matches("set_region_clear", sim_last_custom, sim_last_custom_len));
    check("gw_cls=NONE cap=10 gw_cc 空", v->txp.gw_cls == 0 && v->txp.cap_dbm == 10 && v->txp.gw_cc[0] == '\0');
    err = ncp_version_query("");
    check("空串与 NULL 等价", v->txp.gw_cls == 0 && vec_matches("set_region_clear", sim_last_custom, sim_last_custom_len));

    /* ---- 9. 旧 NCP 固件（没有 XNCP）：版本照读，txp.xncp=false ---- */
    printf("\n=== 9. 旧 NCP 固件 ===\n");
    reset_sim();
    err = ncp_version_query("US");
    v = ncp_version_get();
    check("版本仍读到", err == ESP_OK && v->valid && v->detail_valid);
    check("txp.xncp = false", !v->txp.xncp);
    check("没有我们的 XNCP 时构建串为空", v->build_string[0] == '\0');
    check("read_uptime_s = 0（从没读到）", v->txp.read_uptime_s == 0);

    /* ---- 10. 别家的 XNCP（有 customFrame 但不认 0x7001）---- */
    printf("\n=== 10. 别家 XNCP ===\n");
    reset_sim();
    sim_xncp = true;
    sim_xncp_foreign = true;
    err = ncp_version_query("US");
    v = ncp_version_get();
    check("版本仍读到", err == ESP_OK && v->valid);
    check("txp.xncp = false", !v->txp.xncp);
    check("没有我们的 XNCP 时构建串为空", v->build_string[0] == '\0');

    /* ---- T9.31 ②：读数落盘回调与沿用缓存 ---- */
    printf("\n=== T9.31 ② 落盘回调 / 沿用缓存 ===\n");
    ncp_version_set_persist_hook(persist_hook);
    reset_sim(); g_persist_calls = 0;
    err = ncp_version_query("US");
    check("会话跑过就回调一次，读到的是有效读数", err == ESP_OK && g_persist_calls == 1 && g_persisted.valid);
    check("回调拿到的是真读数", !g_persisted.from_cache && g_persisted.build == 532);
    reset_sim(); g_persist_calls = 0; sim_dead = true;
    (void)ncp_version_query(NULL);
    check("NCP 不应答也回调，但交出去的是无效读数（落盘那边据此作废旧缓存）",
          g_persist_calls == 1 && !g_persisted.valid);
    reset_sim(); (void)ncp_version_query(NULL);         /* 先有一份有效读数 */
    reset_sim(); g_persist_calls = 0; g_stats.active_clients = 1;
    (void)ncp_version_query(NULL);
    check("有客户端拒绝时不回调（旧读数还在也不算新的）",
          ncp_version_get()->valid && g_persist_calls == 0);
    g_stats.active_clients = 0;
    ncp_version_t c = g_persisted; c.txp.read_uptime_s = 1234;
    ncp_version_adopt_cached(&c);
    ncp_version_format(buf, sizeof(buf));
    check("沿用缓存：版本照常显示", strcmp(buf, "8.2.2.0 build 532") == 0);
    check("沿用缓存：from_cache=true、read_uptime_s=0",
          ncp_version_get()->from_cache && ncp_version_get()->txp.read_uptime_s == 0);
    ncp_version_t bad = c; bad.valid = false;
    ncp_version_adopt_cached(&bad);
    check("无效缓存不采用", ncp_version_get()->valid && ncp_version_get()->from_cache);
    ncp_version_set_persist_hook(NULL);

    printf("\n%s (%d failed)\n", fails ? "有失败项" : "全部通过", fails);
    return fails ? 1 : 0;
}
