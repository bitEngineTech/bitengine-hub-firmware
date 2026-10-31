/* wd_heaptrace.c — 诊断专用：抓"一个时间窗里 DMA 内存池被谁吃掉"
 *
 * 由来（2026-10-05）：拔网线 failover 后 1~4 s，/diag/heap 的 dma.min_free 跌到 1403
 * （硬停判据 6 要 ≥3072），而边界路由换口、ZHA 重连、TLS、MQTT 都已用实测排除。
 * 剩下的只能逐笔看"低谷那一刻是谁的块还占着"。
 *
 * 只在 CONFIG_BITENGINE_DIAG_HEAP_TRACE=y 的诊断固件里编进去，出厂固件里本文件为空。
 *
 * 为什么不用 IDF 自带的两样（都会改变要测的东西）：
 *  - heap tracing 要记调用栈，RISC-V 上必须开 ESP_SYSTEM_USE_FRAME_POINTER，全部代码
 *    变大；C5 的 IRAM 与 DRAM 共用一块 SRAM，等于先把要测的那块内存挤掉一截。
 *  - HEAP_TASK_TRACKING 给每块分配加头部 —— 低谷只剩 ~1.4 KB，加了就不是原来的低谷。
 * 这里用 HEAP_USE_HOOKS：只记 DMA 可用地址上的分配（大小、请求的 caps、时间、发起任务），
 * 记录表与地址索引全在 PSRAM；不加逐块开销，代码只多两个 IRAM 小函数。
 * 代价：没有调用栈，归因到"哪个任务、多大、什么 caps" —— 任务栈（INTERNAL|8BIT、
 * 大小等于某个栈尺寸）、驱动 DMA 缓冲（显式 DMA）、普通小 malloc（DEFAULT）一眼可分。
 *
 * 判读：每次记录一笔 DMA 分配后读一次 DMA 池剩余，记下最低点对应的事件序号 s_min_seq。
 * "最低点那一刻还活着、且是窗口内分配的" = seq <= s_min_seq 且（没释放或释放序号 > s_min_seq）。
 * 窗口开始前就存在、窗口里被释放的块不在表里 —— 它们只会让池子变宽，与"谁吃掉了它"无关。
 * 自检：这批块的大小之和（加每块几个字节的堆块头）应当 ≈ dma_free_at_arm - dma_free_min；
 * 差得多说明开窗太晚或有分配没记上（看 skipped / overflow）。
 *
 * 钩子约束（IDF docs heap_debug.rst「Heap Allocation and Free Function Hooks」；
 * heap_caps_base.c 的 CALL_HOOK 调用点）：
 *  - 在堆锁释放之后调用（multi_heap_malloc 已返回），可能来自 ISR；不得阻塞、不得分配。
 *  - 钩子本体在 IRAM（由头文件声明上的 HEAP_IRAM_ATTR 决定）。PSRAM、flash 里的代码（含本文件其余函数、heap_caps_get_free_size）
 *    只在确认 cache 打开之后才碰；ISR 里一律不记（不进临界区以外的路径）。
 *  - 本工程是 -Og：`inline static` 不保证内联（会落进 flash），所以 DMA 地址判断
 *    直接用 SOC_DMA_LOW/HIGH 宏写，不调 esp_ptr_dma_capable()。
 *  - multi_heap_free_size() 只读一个字段、不取锁，在钩子里调安全。
 */
#include "sdkconfig.h"

#if CONFIG_BITENGINE_DIAG_HEAP_TRACE

#include "wd_heaptrace.h"
#include <string.h>
#include <inttypes.h>
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_eth_com.h"                 /* ETH_EVENT / ETHERNET_EVENT_DISCONNECTED */
#include "esp_log.h"
#include "esp_private/cache_utils.h"     /* spi_flash_cache_enabled()（IRAM） */
#include "soc/soc.h"                     /* SOC_DMA_LOW / SOC_DMA_HIGH */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wd_heaptrace";

#define HT_MAX_RECS        16384u   /* 窗口内 DMA 分配条数上限（下标存 uint16） */
#define HT_HASH_BITS       15
#define HT_HASH_SIZE       (1u << HT_HASH_BITS)   /* 2× 上限：活块 + 墓碑永远填不满 */
#define HT_EMPTY           0u       /* 堆指针 4 字节对齐且远大于 1，不会撞这两个值 */
#define HT_TOMB            1u
#define HT_AUTO_WINDOW_MS  12000u   /* 拔线：低谷在 +1~4 s，换口在 +5 s，留足余量 */

#define HT_IS_DMA(p)  ((uintptr_t)(p) >= SOC_DMA_LOW && (uintptr_t)(p) < SOC_DMA_HIGH)

typedef struct {
    uint32_t ptr, size, caps;
    uint32_t t_us, free_t_us;       /* 相对开窗时刻 */
    uint32_t seq, free_seq;         /* free_seq == 0：窗口结束时仍活着 */
    char     task[12];
} ht_rec_t;

/* 三张表都在 PSRAM（init 时一次分配，不释放）：16384×40 + 32768×4 + 32768×2 ≈ 832 KB */
static ht_rec_t *s_recs;
static uint32_t *s_hkey;
static uint16_t *s_hval;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
/* 钩子在临界区外先读一次做快速过滤，临界区内再复查 —— 两处都必须看到同一个变量 */
static volatile bool          s_armed = false;
static volatile wd_ht_state_t s_state = WD_HT_IDLE;
static volatile uint32_t      s_skipped;
static const char *s_trigger = "";
static uint32_t    s_window_ms;
static int64_t     s_t0;
static uint32_t    s_n, s_seq, s_f0, s_fmin, s_min_seq, s_min_t_us;
static bool        s_overflow;
static esp_timer_handle_t s_end_timer;

/* ---- 地址索引（开放寻址 + 线性探测；只在临界区内、cache 开着时调用）---- */

static inline uint32_t h_slot(uint32_t p)
{
    return ((p >> 2) * 2654435761u) >> (32 - HT_HASH_BITS);
}

static int h_find(uint32_t p)
{
    uint32_t i = h_slot(p);
    for (uint32_t n = 0; n < HT_HASH_SIZE; n++, i = (i + 1) & (HT_HASH_SIZE - 1)) {
        uint32_t k = s_hkey[i];
        if (k == HT_EMPTY) return -1;          /* 墓碑不终止探测 */
        if (k == p) return (int)i;
    }
    return -1;
}

static void h_put(uint32_t p, uint16_t v)
{
    uint32_t i = h_slot(p);
    for (uint32_t n = 0; n < HT_HASH_SIZE; n++, i = (i + 1) & (HT_HASH_SIZE - 1)) {
        if (s_hkey[i] == HT_EMPTY || s_hkey[i] == HT_TOMB) {
            s_hkey[i] = p;
            s_hval[i] = v;
            return;
        }
    }
    /* 不可达：占用（活块 + 墓碑）≤ 记录条数 ≤ HT_MAX_RECS < HT_HASH_SIZE */
}

/* ---- 钩子的慢路径（flash 里；调用前已确认 cache 开着、不在 ISR）---- */

static void ht_on_alloc(uint32_t p, uint32_t size, uint32_t caps)
{
    const uint32_t now = (uint32_t)(esp_timer_get_time() - s_t0);
    const char *tn = pcTaskGetName(NULL);
    portENTER_CRITICAL(&s_mux);
    if (s_armed) {
        const uint32_t seq = ++s_seq;
        int slot = h_find(p);
        if (slot >= 0) {
            /* 同一地址又被分配：原地 realloc，或它的 free 发生在 ISR/cache 关闭时没记上。
               先把旧那条关掉，否则它会被误算成"一直占着"。 */
            ht_rec_t *o = &s_recs[s_hval[slot]];
            o->free_seq  = seq;
            o->free_t_us = now;
            s_hkey[slot] = HT_TOMB;
        }
        if (s_n < HT_MAX_RECS) {
            ht_rec_t *r = &s_recs[s_n];
            r->ptr = p; r->size = size; r->caps = caps;
            r->t_us = now; r->free_t_us = 0;
            r->seq = seq; r->free_seq = 0;
            strlcpy(r->task, tn ? tn : "?", sizeof(r->task));
            h_put(p, (uint16_t)s_n);
            s_n++;
        } else {
            s_overflow = true;
        }
        size_t f = heap_caps_get_free_size(MALLOC_CAP_DMA);
        if (f < s_fmin) {
            s_fmin = (uint32_t)f;
            s_min_seq = seq;
            s_min_t_us = now;
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

static void ht_on_free(uint32_t p)
{
    const uint32_t now = (uint32_t)(esp_timer_get_time() - s_t0);
    portENTER_CRITICAL(&s_mux);
    if (s_armed) {
        int slot = h_find(p);
        if (slot >= 0) {                        /* 不在表里 = 开窗前就有的块，不关心 */
            ht_rec_t *r = &s_recs[s_hval[slot]];
            r->free_seq  = ++s_seq;
            r->free_t_us = now;
            s_hkey[slot] = HT_TOMB;
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

/* ---- 钩子本体（覆盖 esp_heap_caps.h 里的弱声明）----
   ⚠️ 定义处别再写 IRAM_ATTR：声明上已带 HEAP_IRAM_ATTR，再写一次两个段名冲突、-Werror
   直接编不过。HEAP_IRAM_ATTR 只在 HEAP_PLACE_FUNCTION_INTO_FLASH 关着时才是 IRAM ——
   那时堆函数本身也在 IRAM，钩子跟着它走；开着时两者都在 flash，同样一致。
   编完用 nm 看这两个符号的地址应在 0x408xxxxx（SRAM），不是 0x42xxxxxx（flash）。 */

void esp_heap_trace_alloc_hook(void *ptr, size_t size, uint32_t caps)
{
    if (!s_armed || !HT_IS_DMA(ptr)) return;
    /* 先查 cache（IRAM 函数），再碰任何可能在 flash 里的东西 */
    if (!spi_flash_cache_enabled() || xPortInIsrContext()) {
        s_skipped++;
        return;
    }
    ht_on_alloc((uint32_t)(uintptr_t)ptr, (uint32_t)size, caps);
}

void esp_heap_trace_free_hook(void *ptr)
{
    if (!s_armed || !HT_IS_DMA(ptr)) return;
    if (!spi_flash_cache_enabled() || xPortInIsrContext()) {
        s_skipped++;
        return;
    }
    ht_on_free((uint32_t)(uintptr_t)ptr);
}

/* realloc 同堆搬家时（multi_heap_realloc 内部 malloc + 拷贝 + free），IDF 只给新指针调
   分配钩子，旧指针的释放钩子从不调（heap_caps_base.c 的 realloc 路径）—— 旧那条会一直显示
   "还占着"，cJSON 拼报文这类小缓冲正好是嫌疑对象，会误导归因（2026-10-05 审计）。
   链接期 --wrap 包一层（watchdog/CMakeLists.txt，只在本开关打开时加；heap_caps.c 跨文件调
   它，且它带 NOINLINE，包得住），搬家了就给旧指针补记一次释放。
   跨堆搬家那条路径本来就走 heap_caps_free（会调释放钩子），这里再补时旧地址已不在表里，
   是空操作。残余误差：旧地址在两步之间被别的任务立刻重新分配到 —— 窗口只有几条指令。
   放 IRAM：被包的函数本身带 HEAP_IRAM_ATTR。 */
void *__real_heap_caps_realloc_base(void *ptr, size_t size, uint32_t caps);
void *IRAM_ATTR __wrap_heap_caps_realloc_base(void *ptr, size_t size, uint32_t caps)
{
    void *r = __real_heap_caps_realloc_base(ptr, size, caps);
    if (r != NULL && ptr != NULL && r != ptr) {
        esp_heap_trace_free_hook(ptr);
    }
    return r;
}

/* ---- 窗口控制 ---- */

static void ht_end_cb(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_mux);
    s_armed = false;
    s_state = WD_HT_DONE;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "窗口结束（%s）：记录 %" PRIu32 " 笔 DMA 分配，DMA 剩余 %" PRIu32
                  " → 最低 %" PRIu32 "（开窗后 %" PRIu32 " ms）%s，跳过 %" PRIu32,
             s_trigger, s_n, s_f0, s_fmin, s_min_t_us / 1000,
             s_overflow ? "，⚠️ 记录表满了" : "", s_skipped);
}

esp_err_t wd_heaptrace_arm(const char *trigger, uint32_t window_ms)
{
    if (!s_recs || !s_end_timer) return ESP_ERR_INVALID_STATE;
    if (window_ms < 1000 || window_ms > 60000) return ESP_ERR_INVALID_ARG;

    /* 先在临界区里占位：state=ARMED 而 s_armed 仍 false（钩子还不记）。手动开窗与以太网
       断线自动开窗同时来时只有一个占得到，另一个直接返回，不会去清别人正在用的索引。 */
    portENTER_CRITICAL(&s_mux);
    bool mine = (s_state != WD_HT_ARMED);
    if (mine) s_state = WD_HT_ARMED;
    portEXIT_CRITICAL(&s_mux);
    if (!mine) return ESP_ERR_INVALID_STATE;

    /* 只清索引；记录表按 s_n 从头覆盖。钩子此刻看到 s_armed=false，不会碰这两张表。 */
    memset(s_hkey, 0, HT_HASH_SIZE * sizeof(uint32_t));
    portENTER_CRITICAL(&s_mux);
    s_n = 0; s_seq = 0; s_min_seq = 0; s_min_t_us = 0;
    s_overflow = false; s_skipped = 0;
    s_trigger = trigger ? trigger : "manual";
    s_window_ms = window_ms;
    s_t0 = esp_timer_get_time();
    s_f0 = s_fmin = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DMA);
    s_state = WD_HT_ARMED;
    s_armed = true;
    portEXIT_CRITICAL(&s_mux);

    esp_err_t err = esp_timer_start_once(s_end_timer, (uint64_t)window_ms * 1000);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_mux);
        s_armed = false;
        s_state = WD_HT_IDLE;
        portEXIT_CRITICAL(&s_mux);
        return err;
    }
    ESP_LOGW(TAG, "开窗（%s，%" PRIu32 " ms），DMA 剩余 %" PRIu32, s_trigger, window_ms, s_f0);
    return ESP_OK;
}

void wd_heaptrace_reset(void)
{
    portENTER_CRITICAL(&s_mux);
    if (s_state == WD_HT_DONE) s_state = WD_HT_IDLE;   /* 正在开窗/抓取时不动 */
    portEXIT_CRITICAL(&s_mux);
}

/* 跑在默认事件循环里。只在 IDLE 时自动开窗：网线抖动时后一次断线不覆盖第一次的结果。 */
static void ht_eth_down_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    if (s_state != WD_HT_IDLE) return;
    esp_err_t err = wd_heaptrace_arm("eth_down", HT_AUTO_WINDOW_MS);
    if (err != ESP_OK) ESP_LOGE(TAG, "以太网断线开窗失败：%s", esp_err_to_name(err));
}

void wd_heaptrace_summary(wd_ht_summary_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_mux);
    out->state           = s_state;
    out->trigger         = s_trigger;
    out->window_ms       = s_window_ms;
    out->dma_free_at_arm = s_f0;
    out->dma_free_min    = s_fmin;
    out->min_at_ms       = s_min_t_us / 1000;
    out->n_entries       = s_n;
    out->overflow        = s_overflow;
    out->skipped         = s_skipped;
    portEXIT_CRITICAL(&s_mux);
}

bool wd_heaptrace_entry(uint32_t i, wd_ht_entry_t *out)
{
    if (!out || !s_recs || s_state != WD_HT_DONE || i >= s_n) return false;
    const ht_rec_t *r = &s_recs[i];
    out->t_ms     = r->t_us / 1000;
    out->size     = r->size;
    out->caps     = r->caps;
    out->freed_ms = r->free_seq ? (int32_t)(r->free_t_us / 1000) : -1;
    /* 任务名进 JSON：只放行安全字符，其余换成 ?（S1） */
    size_t k = 0;
    for (; k < sizeof(out->task) - 1 && r->task[k]; k++) {
        char c = r->task[k];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        out->task[k] = ok ? c : '?';
    }
    out->task[k] = '\0';
    out->alive_at_min = s_min_seq != 0 && r->seq <= s_min_seq &&
                        (r->free_seq == 0 || r->free_seq > s_min_seq);
    return true;
}

esp_err_t wd_heaptrace_init(void)
{
    if (s_recs) return ESP_OK;                         /* 只初始化一次 */
    s_recs = heap_caps_calloc(HT_MAX_RECS, sizeof(ht_rec_t), MALLOC_CAP_SPIRAM);
    s_hkey = heap_caps_calloc(HT_HASH_SIZE, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    s_hval = heap_caps_calloc(HT_HASH_SIZE, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_recs || !s_hkey || !s_hval) {
        heap_caps_free(s_recs); heap_caps_free(s_hkey); heap_caps_free(s_hval);
        s_recs = NULL; s_hkey = NULL; s_hval = NULL;
        ESP_LOGE(TAG, "PSRAM 分配记录表失败 —— 诊断追踪不可用");
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t targs = { .callback = ht_end_cb, .name = "heaptrace" };
    esp_err_t err = esp_timer_create(&targs, &s_end_timer);
    if (err == ESP_OK) {
        err = esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED,
                                         ht_eth_down_handler, NULL);
    }
    if (err != ESP_OK) {
        if (s_end_timer) { esp_timer_delete(s_end_timer); s_end_timer = NULL; }
        heap_caps_free(s_recs); heap_caps_free(s_hkey); heap_caps_free(s_hval);
        s_recs = NULL; s_hkey = NULL; s_hval = NULL;
        ESP_LOGE(TAG, "初始化失败：%s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGW(TAG, "诊断堆追踪就绪：以太网断线自动开 %u ms 窗口（只记 DMA 可用地址）",
             (unsigned)HT_AUTO_WINDOW_MS);
#if CONFIG_BITENGINE_DIAG_HEAP_TRACE_BOOT_MS > 0
    /* 开机窗口（T9.38）：从这里（网络起来之前）一直记到开机后段 */
    err = wd_heaptrace_arm("boot", CONFIG_BITENGINE_DIAG_HEAP_TRACE_BOOT_MS);
    if (err != ESP_OK) ESP_LOGE(TAG, "开机开窗失败：%s", esp_err_to_name(err));
#endif
    return ESP_OK;
}

#endif /* CONFIG_BITENGINE_DIAG_HEAP_TRACE */
