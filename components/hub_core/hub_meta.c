/* hub_meta —— 设备元数据的存取（规则见 include/hub_meta.h、include/hub_meta_core.h）。 */
#include "hub_meta.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hub_store.h"

static const char *TAG = "hub_meta";

#define LOAD_WAIT_MS   3000                        /* 开机读一页最多等这么久（hub_store 开机时可能正忙着别的；超时 = 这一页只读，后面的页也不再等） */
/* 一次 update 最坏：写锁 2 s + 表锁 0.5 s + 投递 1 s + 撤回 0.5 s = 4 s，在命令的 timeout_ms（5 s）之内（第二轮审计） */
#define WR_LOCK_MS     2000                        /* 写锁：前一个更新最坏卡在 hub_store_put 的 1 s 队列等待上 */
#define TBL_LOCK_MS    500                         /* 表锁：别人只持有几微秒 */
#define PUT_WAIT_MS    1000

static hub_meta_core_t  *s_m;                      /* PSRAM，约 13 KB */
static uint8_t          *s_page;                   /* PSRAM，打包 / 读回缓冲（HUB_META_PAGE_BYTES）；开机后只在持有 s_wr 时用 */
static SemaphoreHandle_t s_mtx;                    /* 表；也是"起来了"的标志：最后发布（release），读的人 acquire（第二轮审计：httpd 比 hub_svc 先起） */
static SemaphoreHandle_t s_wr;                     /* 更新排队 */
static bool              s_page_bad[HUB_META_PAGES];
static volatile uint32_t s_updates, s_update_fail, s_lookup_busy, s_dropped;

static SemaphoreHandle_t ready_mtx(void) { return __atomic_load_n(&s_mtx, __ATOMIC_ACQUIRE); }

static void key_of(unsigned page, char key[8]) { snprintf(key, 8, "meta%u", page); }

/* esp_err_t → 读的结果（判定本身在 hub_meta_core 的 hub_meta_load_page，主机测试有真值表） */
static hub_meta_rd_t rd_of(esp_err_t e)
{
    switch (e) {
    case ESP_OK:                return HUB_META_RD_OK;
    case ESP_ERR_NOT_FOUND:     return HUB_META_RD_NONE;
    case ESP_ERR_INVALID_STATE: return HUB_META_RD_DOWN;
    case ESP_ERR_TIMEOUT:       return HUB_META_RD_TIMEOUT;
    default:                    return HUB_META_RD_ERR;   /* INVALID_VERSION（存的比固件新）、INVALID_SIZE、读错 */
    }
}

/* 读一页。返回 false = 超时：剩下的页不再一页页等（开机在 main 任务里，别把 zhub_start 拖上 4 × 3 s），直接按只读 */
static bool load_page(unsigned p)
{
    char key[8];
    key_of(p, key);
    uint8_t ver = 0;
    size_t len = HUB_META_PAGE_BYTES;
    const esp_err_t e = hub_store_get(HUB_STORE_NS_HUB, key, HUB_META_VER, &ver, s_page, &len, LOAD_WAIT_MS);
    const hub_meta_rd_t rd = rd_of(e);
    unsigned dropped = 0;
    switch (hub_meta_load_page(s_m, p, rd, ver, s_page, len, &dropped)) {
    case HUB_META_LD_LOADED:
        if (dropped) {
            s_dropped += dropped;
            ESP_LOGW(TAG, "%s 有 %u 条名字 / 房间过不了现在的净化规则：丢掉（同页别的照常）", key, dropped);
        }
        break;
    case HUB_META_LD_CORRUPT:
        ESP_LOGW(TAG, "%s 内容不对（%u B）：当空页用，下次改动时重写", key, (unsigned)len);
        break;
    case HUB_META_LD_READONLY:
        s_page_bad[p] = true;
        ESP_LOGW(TAG, "%s 读不出来（%s、版本 %u）：这一页这次开机只读，新条目这次开机一律不建（重启后再试）", key, esp_err_to_name(e), (unsigned)ver);
        break;
    case HUB_META_LD_EMPTY:
    default:
        if (rd == HUB_META_RD_DOWN) ESP_LOGE(TAG, "hub_store 没起来：设备名字 / 房间这次开机读不回，也改不了");
        break;
    }
    return rd != HUB_META_RD_TIMEOUT;
}

esp_err_t hub_meta_init(void)
{
    if (ready_mtx()) return ESP_OK;
    hub_meta_core_t *m = heap_caps_calloc(1, sizeof(*m), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *page = heap_caps_malloc(HUB_META_PAGE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    SemaphoreHandle_t mtx = xSemaphoreCreateMutex(), wr = xSemaphoreCreateMutex();
    if (!m || !page || !mtx || !wr) {
        heap_caps_free(m);
        heap_caps_free(page);
        if (mtx) vSemaphoreDelete(mtx);
        if (wr) vSemaphoreDelete(wr);
        ESP_LOGE(TAG, "内存不够：这次开机没有设备名字 / 房间（列表里一律空），hub.device.update 回 unavailable");
        return ESP_ERR_NO_MEM;
    }
    hub_meta_core_init(m);
    s_m = m;                                       /* 读回期间还没人能查（s_mtx 还没发布） */
    s_page = page;
    for (unsigned p = 0; p < HUB_META_PAGES; p++) {
        if (load_page(p)) continue;
        for (unsigned q = p + 1; q < HUB_META_PAGES; q++) s_page_bad[q] = true;
        if (p + 1 < HUB_META_PAGES) ESP_LOGW(TAG, "hub_store 读超时：后面的 meta%u…meta%u 不再等，这次开机都只读", p + 1, HUB_META_PAGES - 1);
        break;
    }
    uint32_t dup_pages = 0;
    const unsigned dup = hub_meta_dedup(s_m, &dup_pages);
    if (dup) {
        /* 立即重写去掉过条目的页（都是读回来的、可写）：只改内存的话，用户以后清掉留下的那条，存储里这条重启后会复活（修复复核） */
        ESP_LOGW(TAG, "同一台设备存了两条（存储被改坏）：去掉 %u 条、留槽号小的，重写页 0x%x", dup, (unsigned)dup_pages);
        for (unsigned p = 0; p < HUB_META_PAGES; p++) {
            if (!(dup_pages & (1u << p))) continue;
            char key[8];
            key_of(p, key);
            const size_t n = hub_meta_pack(s_m, p, s_page, HUB_META_PAGE_BYTES);
            const esp_err_t e = n ? hub_store_put_wait(HUB_STORE_NS_HUB, key, HUB_META_VER, s_page, n, PUT_WAIT_MS) : ESP_ERR_INVALID_SIZE;
            if (e != ESP_OK) ESP_LOGW(TAG, "%s 去重后重写没排上（%s）：下次改这一页时再写", key, esp_err_to_name(e));
        }
    }
    unsigned used = 0;
    for (unsigned i = 0; i < HUB_META_MAX; i++)
        if (s_m->e[i].proto != HUB_META_PROTO_NONE) used++;
    s_wr = wr;
    __atomic_store_n(&s_mtx, mtx, __ATOMIC_RELEASE);   /* 最后发布：前面的写（表、s_wr、s_page_bad）对 acquire 的读者都可见 */
    ESP_LOGI(TAG, "设备元数据：%u 条", used);
    return ESP_OK;
}

void hub_meta_id8_from_ieee(const uint8_t ieee[8], uint8_t id8[8])
{
    for (unsigned k = 0; k < 8; k++) id8[k] = ieee[7 - k];
}

int hub_meta_lookup_ex(uint8_t proto, const uint8_t id8[8], hub_meta_ent_t *out, uint32_t lock_ms)
{
    if (!out) return HUB_META_LK_UNKNOWN;
    memset(out, 0, sizeof(*out));
    SemaphoreHandle_t mtx = ready_mtx();
    if (!mtx || !id8) return HUB_META_LK_UNKNOWN;
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(lock_ms)) != pdTRUE) {
        s_lookup_busy++;                           /* 拿不到 = 这条按"没名字、不推送"走了：计数，别无声无息（第二轮审计） */
        return HUB_META_LK_UNKNOWN;
    }
    const int i = hub_meta_find(s_m, proto, id8);
    if (i >= 0) *out = s_m->e[i];
    xSemaphoreGive(mtx);
    return i >= 0 ? HUB_META_LK_FOUND : HUB_META_LK_NONE;
}

bool hub_meta_lookup(uint8_t proto, const uint8_t id8[8], hub_meta_ent_t *out, uint32_t lock_ms)
{
    return hub_meta_lookup_ex(proto, id8, out, lock_ms) == HUB_META_LK_FOUND;
}

static uint32_t page_ok_mask(void)
{
    uint32_t ok = 0;
    for (unsigned p = 0; p < HUB_META_PAGES; p++)
        if (!s_page_bad[p]) ok |= 1u << p;
    return ok;
}

hub_meta_res_t hub_meta_update(const char *id, const hub_meta_change_t *c, hub_meta_ent_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    uint8_t proto, id8[8];
    if (!c || !hub_meta_parse_id(id, &proto, id8)) return HUB_META_BAD_ID;
    SemaphoreHandle_t mtx = ready_mtx();
    if (!mtx) return HUB_META_STORE;               /* 没起来（内存不够）：重启才可能好 */
    if (xSemaphoreTake(s_wr, pdMS_TO_TICKS(WR_LOCK_MS)) != pdTRUE) return HUB_META_BUSY;
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(TBL_LOCK_MS)) != pdTRUE) {
        xSemaphoreGive(s_wr);
        return HUB_META_BUSY;
    }
    /* 改之前的样子：写没排上时撤回（apply 只动一个槽：已有的那条，或新分配的空位） */
    const int old = hub_meta_find(s_m, proto, id8);
    hub_meta_ent_t before;
    memset(&before, 0, sizeof(before));
    if (old >= 0) before = s_m->e[old];
    int slot = -1;
    hub_meta_res_t r = hub_meta_apply(s_m, proto, id8, c, page_ok_mask(), &slot);
    size_t n = 0;
    if (r == HUB_META_OK && slot >= 0) {
        n = hub_meta_pack(s_m, (unsigned)slot / HUB_META_PER_PAGE, s_page, HUB_META_PAGE_BYTES);
        if (n == 0) {                              /* 不会发生（一页的缓冲按满页开的）：防御 */
            s_m->e[slot] = before;
            r = HUB_META_BUSY;
        }
    }
    if (r == HUB_META_OK && out) {
        const int i = hub_meta_find(s_m, proto, id8);
        if (i >= 0) *out = s_m->e[i];
        else {                                     /* 回到默认、腾了位置：照样把 id 带回去 */
            out->proto = proto;
            memcpy(out->id8, id8, 8);
        }
    }
    xSemaphoreGive(mtx);
    if (r == HUB_META_OK && slot >= 0) {
        /* 表锁外投递（hub_store_put 队列满时最多等 1 s，别让查名字的人陪着等）；写锁还拿着，后来的更新排在这一次之后 */
        char key[8];
        key_of((unsigned)slot / HUB_META_PER_PAGE, key);
        const esp_err_t e = hub_store_put_wait(HUB_STORE_NS_HUB, key, HUB_META_VER, s_page, n, PUT_WAIT_MS);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "%s 没排上写（%s）：这次改动撤回", key, esp_err_to_name(e));
            if (xSemaphoreTake(mtx, pdMS_TO_TICKS(TBL_LOCK_MS)) == pdTRUE) {   /* 只有 lookup 与本函数拿它，都只持有几微秒 */
                s_m->e[slot] = before;
                xSemaphoreGive(mtx);
            } else {
                ESP_LOGE(TAG, "%s 撤回时拿不到表锁：内存里留着没落盘的改动，重启后回到旧值", key);
            }
            /* hub_store 没起来（INVALID_STATE：没起来或恢复出厂擦完了）重试也没用；队列满过一会儿再试 */
            r = e == ESP_ERR_INVALID_STATE ? HUB_META_STORE : HUB_META_BUSY;
        }
    }
    if (r == HUB_META_OK) s_updates++;
    else if (r == HUB_META_STORE || r == HUB_META_BUSY || r == HUB_META_FULL) s_update_fail++;
    xSemaphoreGive(s_wr);
    return r;
}

esp_err_t hub_meta_forget_proto(uint8_t proto)
{
    SemaphoreHandle_t mtx = ready_mtx();
    if (!mtx || proto == HUB_META_PROTO_NONE) return ESP_ERR_INVALID_STATE;
    /* 有页这次开机读不出来：它在内存里是空的，看不出里面有没有这个协议的条目 —— 跳过它就会让旧条目留在 flash 里、下次开机复活（审计 ❌）。
       整件不做，让用户重启后再来 */
    for (unsigned p = 0; p < HUB_META_PAGES; p++)
        if (s_page_bad[p]) return ESP_ERR_INVALID_STATE;
    hub_meta_ent_t *before = heap_caps_malloc(sizeof(hub_meta_ent_t) * HUB_META_PER_PAGE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   /* 约 3.4 KB：别放调用方栈上 */
    if (!before) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(s_wr, pdMS_TO_TICKS(WR_LOCK_MS)) != pdTRUE) {
        heap_caps_free(before);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ESP_OK;
    /* 每一页都重写（不看内存里有没有这个协议）：上一次的写若最终没落盘，内存已经清过、看不出来 —— 无条件重写才幂等（审计） */
    for (unsigned p = 0; p < HUB_META_PAGES && err == ESP_OK; p++) {
        if (xSemaphoreTake(mtx, pdMS_TO_TICKS(TBL_LOCK_MS)) != pdTRUE) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        memcpy(before, &s_m->e[p * HUB_META_PER_PAGE], sizeof(hub_meta_ent_t) * HUB_META_PER_PAGE);
        (void)hub_meta_clear_proto(s_m, proto, p);
        const size_t n = hub_meta_pack(s_m, p, s_page, HUB_META_PAGE_BYTES);
        xSemaphoreGive(mtx);
        char key[8];
        key_of(p, key);
        const esp_err_t e = n ? hub_store_put_wait(HUB_STORE_NS_HUB, key, HUB_META_VER, s_page, n, PUT_WAIT_MS) : ESP_ERR_INVALID_SIZE;
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "%s 没排上写（%s）：这一页的删除撤回", key, esp_err_to_name(e));
            if (xSemaphoreTake(mtx, pdMS_TO_TICKS(TBL_LOCK_MS)) == pdTRUE) {
                memcpy(&s_m->e[p * HUB_META_PER_PAGE], before, sizeof(hub_meta_ent_t) * HUB_META_PER_PAGE);
                xSemaphoreGive(mtx);
            }
            err = e == ESP_ERR_INVALID_STATE ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
        }
    }
    xSemaphoreGive(s_wr);
    heap_caps_free(before);
    return err;
}

void hub_meta_get_status(hub_meta_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    SemaphoreHandle_t mtx = ready_mtx();
    if (!mtx) return;
    out->ready = true;
    for (unsigned p = 0; p < HUB_META_PAGES; p++)
        if (s_page_bad[p]) out->pages_ro |= (uint8_t)(1u << p);
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(HUB_META_LOOKUP_MS)) == pdTRUE) {
        for (unsigned i = 0; i < HUB_META_MAX; i++)
            if (s_m->e[i].proto != HUB_META_PROTO_NONE) out->used++;
        xSemaphoreGive(mtx);
    }
    out->updates = s_updates;
    out->update_fail = s_update_fail;
    out->lookup_busy = s_lookup_busy;
    out->dropped = s_dropped;
}
