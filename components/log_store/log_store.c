/* log_store —— 把 ESP_LOG 输出留一份在内存里，供 GET /api/v1/logs 远程查看。
 *
 * 为什么需要：现场排障目前只能挂串口。取证（"为什么重启"）已经由
 * watchdog/wd_forensics.c 覆盖（RTC_NOINIT + NVS 环形），本模块补的是另一半 ——
 * 事件发生**之前**的叙事：取证告诉你"看门狗复位了任务 X"，日志告诉你 X 当时在干什么。
 *
 * 存储三层，**没有一层落 Flash**（不磨损、不动分区表）：
 *   PSRAM 环形     最近 LOG_SLOTS 行完整日志。掉电/复位都丢。
 *   RTC_NOINIT     最后 RTC_LINES 行。掉电清零，**软复位保留** —— 正好覆盖
 *                  看门狗/panic 重启，那几行是最想看的内容。开机时把它们
 *                  倒进 PSRAM 环形，于是重启前的最后几行会自然出现在日志开头。
 *   （已有）NVS/coredump  取证记录与崩溃回溯，不在本模块职责内。
 */

#include "log_store.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>   /* strtoul —— 解析日志行里的时间戳 */
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 600 槽 × ~172B ≈ 103KB，放 PSRAM。内部 SRAM 是稀缺资源，不能用它做这个。
   槽数按 OpenThread 的实际话痨程度定：它在建网期间每秒能刷十几行，
   400 槽只够存几十秒。PSRAM 有 8MB，这点开销可以忽略。 */
#define LOG_SLOTS    600

/* ⚠️ 第二个环：只收 E/W。
   为什么必须单独一个 —— 2026-08-22 在板上量到的：主环 600 槽的实际保留窗口
   只有 **111 秒**，丢弃率 90.4%（5637/6238），因为 OPENTHREAD 的 INFO 占了 88%
   （BorderRouting 每隔几秒一条 RA/RIO），剩下的还有一半是我们自己每 30 秒一条的
   Stats/Net 心跳。结果是 /api/v1/logs 对"刚才发生了什么"完全答不上来 ——
   用户报 Zigbee 掉线时打开日志，现场早在两分钟前就被挤掉了。
   而 ?level=W 也救不了：那个过滤是**取回之后**才做的，取回的 100 条里本来就
   没有告警。
   ⚠️ 2026-08-23 订正上一句：原文写「200 槽能装下几天份」，**是错的**，
   上板量出来是 **72 条 W/E 每小时**（tag 分布：thread_br 29% / tcp_server 26% /
   OPENTHREAD 16% / httpd 26%，全是常规运行产生的，不是故障态）。
   200 槽只够 **2.8 小时** —— 而这个环存在的全部理由就是"事后还能回看"。
   实测中它被灌满过不止一次（dropped=307）。

   改成 2000 槽 ≈ 336KB PSRAM ≈ 4.3% 的空闲 PSRAM ≈ **28 小时**，
   够覆盖"用户第二天早上才来报"。两个环都在 PSRAM，内部 RAM 不受影响。
   ⚠️ 要再改先量，别拍脑袋：72 条/小时是**这台设备**在**这个负载**下的数字，
      设备数、Thread 拓扑、httpd 访问频率都会改变它。量法见本注释开头那种做法
      （取回 N 条，用首尾的 ms 差算速率）。 */
#define WARN_SLOTS   2000

/* RTC 慢速内存有限（watchdog 已占一小块），只留最后 8 行。
   128 字节一行，够放大多数日志的正文。 */
#define RTC_LINES    8
#define RTC_LINE_LEN 128
#define RTC_MAGIC    0x4C475301u   /* "LGS" + version */

static const char *TAG = "log_store";

/* ---- PSRAM 环形 ---- */
static log_rec_t       *s_ring;
static uint32_t         s_head;        /* 下一个写入槽位 */
static uint32_t         s_count;       /* 有效记录数，≤ LOG_SLOTS */
static uint32_t         s_next_seq = 1;
static uint32_t         s_dropped;
/* 被 tag 过滤器主动挡掉的行数。**与 s_dropped 分开计**：那个是"环满了覆盖掉旧的"
   （真的丢了数据，说明容量不够），这个是"我们不想要"。混在一起会让人看着
   dropped=1107 去加容量，而真正的原因是噪声源。 */
static uint32_t         s_filtered;

/* E/W 专用环 —— 与主环同结构，独立的 head/count/dropped。 */
static log_rec_t       *s_wring;
static uint32_t         s_whead;
static uint32_t         s_wcount;
static uint32_t         s_wdropped;
static portMUX_TYPE     s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t   s_orig;

/* ---- RTC 尾巴（软复位保留）---- */
typedef struct {
    uint32_t magic;
    uint32_t head;
    uint32_t ms[RTC_LINES];
    char     line[RTC_LINES][RTC_LINE_LEN];
} rtc_tail_t;
static RTC_NOINIT_ATTR rtc_tail_t s_rtc;

/* ---------------- 解析 ---------------- */

/* 就地剥离 ANSI 转义（ESP-IDF 默认给日志加颜色，\033[0;32m ... \033[0m）。
   不剥的话这些控制字符会直接进 JSON，前端显示成乱码。 */
static void strip_ansi(char *s)
{
    char *w = s;
    for (char *r = s; *r; ) {
        if (*r == 0x1b) {                 /* ESC */
            while (*r && *r != 'm') r++;  /* 吃到 'm' 为止 */
            if (*r) r++;
            continue;
        }
        *w++ = *r++;
    }
    *w = '\0';
}

/* 截断点退到 UTF-8 字符边界：cut 是第一个不要的字节的下标，它若是续字节（10xxxxxx），说明前一个
   字符被切开了，往回退到那个字符的起点。2026-10-06 之前按字节硬截：中文日志超过 140 字节就切在字符
   中间，/api/v1/logs 的 JSON 里出现非法 UTF-8，严格的解析器（Python json）整包读不了。 */
static size_t utf8_cut(const char *s, size_t cut)
{
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
    return cut;
}

/* 解析 "I (12345) tag: message" 三段。
   解析不出来不丢日志：level='?'、tag 空、msg 存原文 —— 宁可格式难看，
   不可让一行日志凭空消失。 */
static void parse_line(char *line, log_rec_t *rec)
{
    rec->level = '?';
    rec->tag[0] = '\0';
    rec->ms = (uint32_t)(esp_timer_get_time() / 1000);

    /* 两种前缀都要认：ESP-IDF 是 "I (1234) tag: ..."，
       OpenThread 是 "I(1234) OPENTHREAD: ..."（少一个空格）。
       只认前者的话，OT 那一大半日志会全部降级成 level='?'。 */
    const char *body = line;
    const char *num = NULL;
    if (line[0] && strchr("EWIDV", line[0])) {
        if (line[1] == '(')                        num = line + 2;
        else if (line[1] == ' ' && line[2] == '(') num = line + 3;
    }
    if (num) {
        rec->level = line[0];
        char *rp = strstr(num, ") ");
        if (rp) {
            rec->ms = (uint32_t)strtoul(num, NULL, 10);
            /* 找 tag 后面的冒号。**不能只认 ": "**：ESP-IDF 的 wifi 驱动打的是
               `wifi:(cap)capinfo...`，冒号后面没有空格，只认 ": " 会把整段
               当成正文、tag 全空 —— 而 tag 正是筛日志最常用的维度。 */
            const char *ts = rp + 2;
            const char *colon = strchr(ts, ':');
            if (colon && (size_t)(colon - ts) < LOG_TAG_MAX && colon > ts) {
                size_t tl = colon - ts;
                memcpy(rec->tag, ts, tl);
                rec->tag[tl] = '\0';
                body = colon + 1;
                if (*body == ' ') body++;      /* 有空格就吃掉，没有就算了 */
            } else {
                body = ts;
            }
        }
    }

    size_t n = strlen(body);
    while (n && (body[n - 1] == '\n' || body[n - 1] == '\r')) n--;   /* 去尾换行 */
    if (n >= LOG_MSG_MAX) {
        /* 截断要留痕，否则读者不知道后面还有内容（truncated）；截在字符边界上（utf8_cut） */
        size_t cut = utf8_cut(body, LOG_MSG_MAX - 4);
        memcpy(rec->msg, body, cut);
        memcpy(rec->msg + cut, "...", 4);
    } else {
        memcpy(rec->msg, body, n);
        rec->msg[n] = '\0';
    }
}

/* ---------------- 写入 ---------------- */

/* ⚠️ 这个函数里**绝不能**调 ESP_LOG：它由日志钩子调用，打日志就是无限递归。 */
/* 只在**这个环形缓冲**里挡掉的噪声源。串口输出一个字都不少 ——
   log_vprintf 里 s_orig() 在前面就已经调过了，过滤发生在它之后。
 *
 * 为什么需要：2026-08-23 实测主环最近 100 条里 **93 条**是 OpenThread 的逐帧
 * INFO（Sending Beacon Request / Receive Advertisement / Mac----- 之类），
 * 600 槽只覆盖 5-9 分钟、dropped 已经 1400。挡掉之后覆盖时长约 14 倍。
 *
 * ⚠️ 别把这件事和告警环搞混：W/E 早就单独存在 WARN_SLOTS 那个环里了
 * （200 槽、实测 dropped=0，覆盖整个运行时长）。**错误和告警本来就不丢**。
 * 这里解决的是另一半 —— "事件前后的 INFO 上下文"，主环被噪声冲得只剩几分钟。
 *
 * ⚠️ 只挡 I/D/V，**W 和 E 永远留**。OpenThread 的告警恰恰是排 Thread 故障时
 *    最该看到的（比如射频静默死掉那类），一并挡掉就把功能挡没了。
 * ⚠️ 挡的是 tag 不是内容：按关键词过滤会随上游改文案而失效，且没人会发现。 */
static const char *const k_noisy_tags[] = {
    "OPENTHREAD",
};

static bool tag_is_noisy(const log_rec_t *rec)
{
    if (rec->level != 'I' && rec->level != 'D' && rec->level != 'V') return false;
    for (size_t i = 0; i < sizeof(k_noisy_tags) / sizeof(k_noisy_tags[0]); i++) {
        if (strcmp(rec->tag, k_noisy_tags[i]) == 0) return true;
    }
    return false;
}

static void store_line(char *line)
{
    strip_ansi(line);

    log_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    parse_line(line, &rec);

    /* 在**分配 seq 之前**返回：被挡的行不占号，客户端按 ?since= 翻页不会看到空洞。
       也因此不会进 RTC 尾巴 —— 那 8 行是"重启前最后发生了什么"的唯一线索，
       被逐帧 INFO 占满等于没有。 */
    if (tag_is_noisy(&rec)) {
        portENTER_CRITICAL(&s_mux);
        s_filtered++;
        portEXIT_CRITICAL(&s_mux);
        return;
    }

    /* RTC 那一行**先在锁外格式化好**。临界区是关中断的，里面做 snprintf
       会把关中断时间拉到几微秒以上 —— 这个项目已经被 802.15.4 的时序坑过，
       临界区里只留定长 memcpy。 */
    /* RTC 行比 msg 短，正文按预算显式截断 —— 让编译器看得出不会溢出，
       也让"这里会截断"成为写在代码里的事实而不是隐含行为。
       预算 = 行长 - level(1) - 空格(1) - tag - ": "(2) - NUL(1) 再留点余量 */
    char rtc_line[RTC_LINE_LEN];
    int  rtc_budget = RTC_LINE_LEN - LOG_TAG_MAX - 8;
    if ((int)strlen(rec.msg) > rtc_budget) rtc_budget = (int)utf8_cut(rec.msg, (size_t)rtc_budget);
    snprintf(rtc_line, sizeof(rtc_line), "%c %s: %.*s",
             rec.level, rec.tag, rtc_budget, rec.msg);

    portENTER_CRITICAL(&s_mux);
    rec.seq = s_next_seq++;
    if (s_count == LOG_SLOTS) s_dropped++;       /* 覆盖掉一条旧的 */
    else                      s_count++;
    s_ring[s_head] = rec;
    s_head = (s_head + 1) % LOG_SLOTS;

    /* E/W 再存一份到告警环。定长 memcpy，临界区不变长。 */
    if (s_wring && (rec.level == 'E' || rec.level == 'W')) {
        if (s_wcount == WARN_SLOTS) s_wdropped++;
        else                        s_wcount++;
        s_wring[s_whead] = rec;
        s_whead = (s_whead + 1) % WARN_SLOTS;
    }

    /* 同步一份进 RTC 尾巴：软复位后这几行还在，是"重启前最后发生了什么"的唯一线索 */
    uint32_t ri = s_rtc.head % RTC_LINES;
    s_rtc.ms[ri] = rec.ms;
    memcpy(s_rtc.line[ri], rtc_line, RTC_LINE_LEN);
    s_rtc.head = (s_rtc.head + 1) % RTC_LINES;
    s_rtc.magic = RTC_MAGIC;
    portEXIT_CRITICAL(&s_mux);
}

/* 按换行重组整行后再入库。
 *
 * 为什么必须做：**一次回调不等于一行日志**。OpenThread 会把一条日志拆成
 * 多次调用（前缀一次、正文一次、换行一次），直接每次回调存一条的话，得到的
 * 是大量空条目和半截行 —— 2026-08-13 首版实测 16 秒就把 400 个槽位耗光，
 * dropped=687，而真正有用的内容一条都读不出来。
 */
static char   s_partial[LOG_TAG_MAX + LOG_MSG_MAX + 64];
static size_t s_partial_len;

static void capture(const char *chunk, size_t len)
{
    char complete[sizeof(s_partial)];
    bool have_line = false;

    /* ❶ 短临界区：只做拼接与取出，不做格式化 */
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < len; i++) {
        char c = chunk[i];
        if (c == '\n') {
            memcpy(complete, s_partial, s_partial_len);
            complete[s_partial_len] = '\0';
            s_partial_len = 0;
            have_line = true;
            break;                       /* 一次回调里出现多行的情况极少，留到下次 */
        }
        if (s_partial_len < sizeof(s_partial) - 1) s_partial[s_partial_len++] = c;
        /* 满了就丢弃后续字符：宁可截断这一行，也不能让缓冲越界 */
    }
    portEXIT_CRITICAL(&s_mux);

    /* ❷ 锁外解析并入库。空行直接扔掉（OpenThread 的分片会产生一堆空行）。 */
    if (have_line && complete[0]) store_line(complete);
}

static int log_vprintf(const char *fmt, va_list ap)
{
    /* va_list 只能遍历一次，转给原输出之前必须复制一份自己用 */
    va_list copy;
    va_copy(copy, ap);

    /* 串口输出优先，且绝不能因为本模块出错而丢 */
    int n = s_orig ? s_orig(fmt, ap) : vprintf(fmt, ap);

    /* ⚠️ ISR 上下文直接跳过：flash 写入期间 cache 关闭，此时从 ISR 访问
       PSRAM 会触发 cache 异常。日志功能不值得为此冒崩溃的风险。 */
    if (s_ring && !xPortInIsrContext()) {
        char buf[LOG_TAG_MAX + LOG_MSG_MAX + 64];
        int len = vsnprintf(buf, sizeof(buf), fmt, copy);
        if (len > 0) {
            if (len >= (int)sizeof(buf)) len = sizeof(buf) - 1;
            capture(buf, (size_t)len);
        }
    }
    va_end(copy);
    return n;
}

/* ---------------- 对外 ---------------- */

/* 把上次运行遗留在 RTC 里的最后几行倒进环形缓冲。
   它们带着原来的时间戳，会自然排在本次启动的日志之前。 */
static void drain_rtc_tail(void)
{
    if (s_rtc.magic != RTC_MAGIC) {     /* 掉电过或首次启动 */
        memset(&s_rtc, 0, sizeof(s_rtc));
        return;
    }
    for (int i = 0; i < RTC_LINES; i++) {
        uint32_t idx = (s_rtc.head + i) % RTC_LINES;   /* 从最旧的开始 */
        if (s_rtc.line[idx][0] == '\0') continue;
        log_rec_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.seq   = s_next_seq++;
        rec.ms    = s_rtc.ms[idx];
        rec.level = '?';
        strlcpy(rec.tag, "prev-boot", sizeof(rec.tag));
        strlcpy(rec.msg, s_rtc.line[idx], sizeof(rec.msg));
        s_ring[s_head] = rec;
        s_head = (s_head + 1) % LOG_SLOTS;
        if (s_count < LOG_SLOTS) s_count++;
    }
    memset(&s_rtc, 0, sizeof(s_rtc));   /* 倒完就清，避免下次重复出现 */
}

esp_err_t log_store_init(void)
{
    if (s_ring) return ESP_OK;

    s_ring = heap_caps_calloc(LOG_SLOTS, sizeof(log_rec_t), MALLOC_CAP_SPIRAM);
    if (!s_ring) {
        /* 拿不到 PSRAM 就不做 —— 绝不退而求其次去占内部 SRAM，
           那是本项目最稀缺的资源（见 docs/开发索引.md 里内部 SRAM 的几行）。 */
        ESP_LOGW(TAG, "PSRAM 分配失败（需要 %u 字节），日志留存关闭",
                 (unsigned)(LOG_SLOTS * sizeof(log_rec_t)));
        return ESP_ERR_NO_MEM;
    }

    /* 告警环拿不到就算了 —— 主环更重要，不能因为它分配失败就整个功能不启用。 */
    s_wring = heap_caps_calloc(WARN_SLOTS, sizeof(log_rec_t), MALLOC_CAP_SPIRAM);
    if (!s_wring) ESP_LOGW(TAG, "告警环分配失败（%u 字节），E/W 只留在主环里",
                           (unsigned)(WARN_SLOTS * sizeof(log_rec_t)));

    drain_rtc_tail();
    s_orig = esp_log_set_vprintf(log_vprintf);

    ESP_LOGI(TAG, "日志留存已启用：主环 %d 槽 + 告警环 %d 槽 / 共 %u KB PSRAM，RTC 尾巴 %d 行",
             LOG_SLOTS, s_wring ? WARN_SLOTS : 0,
             (unsigned)((LOG_SLOTS + (s_wring ? WARN_SLOTS : 0)) * sizeof(log_rec_t) / 1024),
             RTC_LINES);
    return ESP_OK;
}

int log_store_fetch(uint32_t since_seq, log_rec_t *out, int max)
{
    if (!s_ring || !out || max <= 0) return 0;

    portENTER_CRITICAL(&s_mux);
    uint32_t count = s_count;
    uint32_t start = (s_head + LOG_SLOTS - count) % LOG_SLOTS;
    portEXIT_CRITICAL(&s_mux);

    /* since_seq==0（调用方没指定起点）时给**最新的** max 条，而不是最旧的。
       日志的默认语义就是 tail —— 没人 `curl /logs` 是想看开机那一刻的。
       2026-08-13 实测踩到：拿 ?limit=100 去找刚产生的一条日志，翻到的全是
       启动日志，白白怀疑了一轮功能。
       前端不受影响：它第一次也传 0（于是直接看到近况，更好），之后按
       next_seq 增量拉取。 */
    if (since_seq == 0 && count > (uint32_t)max) {
        start = (start + (count - (uint32_t)max)) % LOG_SLOTS;
        count = (uint32_t)max;
    }

    int n = 0;
    for (uint32_t i = 0; i < count && n < max; i++) {
        /* 逐条持锁：一条 172 字节的 memcpy 是有界的短临界区。
           整段遍历都持锁的话会关中断上万个周期，802.15.4 的时序扛不住。
           代价是极端高频日志下可能读到刚被覆盖的槽 —— 靠 seq 过滤兜底，
           客户端按 next_seq 增量拉取不受影响。 */
        portENTER_CRITICAL(&s_mux);
        log_rec_t rec = s_ring[(start + i) % LOG_SLOTS];
        portEXIT_CRITICAL(&s_mux);

        if (rec.seq > since_seq) out[n++] = rec;
    }
    return n;
}

uint32_t log_store_filtered(void)
{
    return s_filtered;
}

void log_store_stats(uint32_t *next_seq, uint32_t *dropped,
                     uint32_t *capacity, uint32_t *stored)
{
    portENTER_CRITICAL(&s_mux);
    if (next_seq) *next_seq = s_next_seq;
    if (dropped)  *dropped  = s_dropped;
    if (stored)   *stored   = s_count;
    portEXIT_CRITICAL(&s_mux);
    if (capacity) *capacity = LOG_SLOTS;
}

/* 只取 E/W。语义与 log_store_fetch 完全一致（含 since_seq==0 给最新 max 条），
   区别只是数据源是告警环 —— 那里不会被 INFO 挤掉。 */
int log_store_fetch_warn(uint32_t since_seq, log_rec_t *out, int max)
{
    if (!s_wring || !out || max <= 0) return 0;

    portENTER_CRITICAL(&s_mux);
    uint32_t count = s_wcount;
    uint32_t start = (s_whead + WARN_SLOTS - count) % WARN_SLOTS;
    portEXIT_CRITICAL(&s_mux);

    if (since_seq == 0 && count > (uint32_t)max) {
        start = (start + (count - (uint32_t)max)) % WARN_SLOTS;
        count = (uint32_t)max;
    }

    int n = 0;
    for (uint32_t i = 0; i < count && n < max; i++) {
        portENTER_CRITICAL(&s_mux);
        log_rec_t rec = s_wring[(start + i) % WARN_SLOTS];
        portEXIT_CRITICAL(&s_mux);
        if (rec.seq > since_seq) out[n++] = rec;
    }
    return n;
}

void log_store_warn_stats(uint32_t *dropped, uint32_t *capacity, uint32_t *stored)
{
    portENTER_CRITICAL(&s_mux);
    if (dropped) *dropped = s_wdropped;
    if (stored)  *stored  = s_wcount;
    portEXIT_CRITICAL(&s_mux);
    if (capacity) *capacity = s_wring ? WARN_SLOTS : 0;
}
