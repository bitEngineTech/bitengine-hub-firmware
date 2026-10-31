/* hub_store 的纯逻辑（说明见 hub_store_core.h）。
 * ⚠️ 不许引 ESP-IDF 头文件：test_host/run.sh 原样编这个文件，VERIFY.sh 钉着。 */
#include "hub_store_core.h"

#include <string.h>

/* now 在 49.7 天后回绕：比先后用有符号差 */
static bool due(uint32_t due_ms, uint32_t now_ms) { return (int32_t)(due_ms - now_ms) <= 0; }

bool hsc_name_ok(const char *name)
{
    if (!name || !name[0]) return false;
    size_t n = 0;
    for (; name[n]; n++) {
        if (n >= HSC_NAME_MAX) return false;
        char c = name[n];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                  || c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

void hsc_init(hsc_t *s, const hsc_backend_t *be, void *(*alloc)(size_t), void (*dealloc)(void *))
{
    memset(s, 0, sizeof(*s));
    s->be = *be;
    s->alloc = alloc;
    s->dealloc = dealloc;
}

static hsc_pending_t *find(hsc_t *s, const char *ns, const char *key)
{
    for (int i = 0; i < HSC_PENDING_MAX; i++) {
        hsc_pending_t *p = &s->slots[i];
        if (p->used && strcmp(p->ns, ns) == 0 && strcmp(p->key, key) == 0) return p;
    }
    return NULL;
}

static void release(hsc_t *s, hsc_pending_t *p)
{
    if (p->blob) s->dealloc(p->blob);
    memset(p, 0, sizeof(*p));
}

/* 执行一条。返回 true = 这条结束了（成功、被拒、或重试用尽）；false = 失败但还会重试。
   *gave_up 置 true 表示重试用尽。 */
static bool execute(hsc_t *s, hsc_pending_t *p, uint32_t now_ms, bool *gave_up)
{
    int rc;
    *gave_up = false;
    if (p->op == HSC_OP_ERASE) {
        rc = s->be.erase(s->be.ctx, p->ns, p->key);
    } else {
        /* 规则 4：已存的版本比这次写的新 → 不覆盖（固件被降级了，留给再升级回去时用） */
        size_t len = 0;
        rc = s->be.get(s->be.ctx, p->ns, p->key, NULL, 0, &len);
        if (rc == HSC_OK && len >= 1) {
            uint8_t *old = s->alloc(len);
            if (!old) {
                rc = HSC_ENOMEM;
            } else {
                size_t got = len;
                rc = s->be.get(s->be.ctx, p->ns, p->key, old, len, &got);
                uint8_t stored_ver = (rc == HSC_OK && got >= 1) ? old[0] : 0;
                s->dealloc(old);
                if (rc == HSC_OK && stored_ver > p->blob[0]) {
                    s->stats.refused_newer++;
                    strcpy(s->stats.last_refused_ns, p->ns);     /* 长度已由 hsc_name_ok 保证 */
                    strcpy(s->stats.last_refused_key, p->key);
                    s->stats.last_refused_stored = stored_ver;
                    s->stats.last_refused_ver = p->blob[0];
                    return true;
                }
            }
        } else if (rc == HSC_ENOENT) {
            rc = HSC_OK;           /* 没有旧值，直接写 */
        }
        if (rc == HSC_OK) rc = s->be.set(s->be.ctx, p->ns, p->key, p->blob, p->blob_len);
    }
    if (rc == HSC_OK) {
        s->stats.writes++;
        return true;
    }
    s->stats.last_err = rc;
    p->tries++;
    if (p->tries >= HSC_MAX_TRIES) {
        s->stats.failures++;
        *gave_up = true;
        return true;
    }
    s->stats.retries++;
    p->due_ms = now_ms + HSC_RETRY_MS;
    return false;
}

int hsc_run(hsc_t *s, uint32_t now_ms, bool force)
{
    int gave_up_count = 0;
    for (int i = 0; i < HSC_PENDING_MAX; i++) {
        hsc_pending_t *p = &s->slots[i];
        if (!p->used || (!force && !due(p->due_ms, now_ms))) continue;
        bool gave_up;
        if (execute(s, p, now_ms, &gave_up)) {
            if (gave_up) gave_up_count++;
            release(s, p);
        }
    }
    return gave_up_count;
}

/* 表满时腾一格：执行最早到期的那条（不管到没到期）。执行失败且还要重试的，这条留着，腾不出来就报 ENOMEM。 */
static hsc_pending_t *free_slot(hsc_t *s, uint32_t now_ms)
{
    hsc_pending_t *earliest = NULL;
    for (int i = 0; i < HSC_PENDING_MAX; i++) {
        hsc_pending_t *p = &s->slots[i];
        if (!p->used) return p;
        if (!earliest || (int32_t)(p->due_ms - earliest->due_ms) < 0) earliest = p;
    }
    bool gave_up;
    if (execute(s, earliest, now_ms, &gave_up)) {
        release(s, earliest);
        return earliest;
    }
    return NULL;
}

int hsc_put(hsc_t *s, const char *ns, const char *key, uint8_t ver, const void *data, size_t len, uint32_t now_ms)
{
    if (!s || !hsc_name_ok(ns) || !hsc_name_ok(key) || ver == 0 || len > HSC_BLOB_MAX || (len && !data)) {
        return HSC_EINVAL;
    }
    uint8_t *blob = s->alloc(len + 1);
    if (!blob) return HSC_ENOMEM;
    blob[0] = ver;
    if (len) memcpy(blob + 1, data, len);

    hsc_pending_t *p = find(s, ns, key);
    if (p) {
        s->stats.coalesced++;
        if (p->blob) s->dealloc(p->blob);
    } else {
        p = free_slot(s, now_ms);
        if (!p) {
            s->dealloc(blob);
            return HSC_ENOMEM;
        }
        p->used = true;
        strcpy(p->ns, ns);       /* 长度已由 hsc_name_ok 保证 ≤ HSC_NAME_MAX */
        strcpy(p->key, key);
    }
    p->op = HSC_OP_PUT;
    p->blob = blob;
    p->blob_len = len + 1;
    p->due_ms = now_ms + HSC_DEBOUNCE_MS;
    p->tries = 0;
    return HSC_OK;
}

int hsc_erase(hsc_t *s, const char *ns, const char *key, uint32_t now_ms)
{
    if (!s || !hsc_name_ok(ns) || !hsc_name_ok(key)) return HSC_EINVAL;
    hsc_pending_t *p = find(s, ns, key);
    if (p) {
        if (p->blob) s->dealloc(p->blob);
        p->blob = NULL;
        p->blob_len = 0;
    } else {
        p = free_slot(s, now_ms);
        if (!p) return HSC_ENOMEM;
        p->used = true;
        strcpy(p->ns, ns);
        strcpy(p->key, key);
    }
    p->op = HSC_OP_ERASE;
    p->due_ms = now_ms;          /* 删除不防抖 */
    p->tries = 0;
    return HSC_OK;
}

int hsc_get(hsc_t *s, const char *ns, const char *key, uint8_t max_ver,
            uint8_t *ver, void *buf, size_t *len_inout)
{
    if (!s || !hsc_name_ok(ns) || !hsc_name_ok(key) || !ver || !len_inout || (*len_inout && !buf)) {
        return HSC_EINVAL;
    }
    const uint8_t *blob = NULL;
    size_t blob_len = 0;
    uint8_t *owned = NULL;

    hsc_pending_t *p = find(s, ns, key);
    if (p) {
        if (p->op == HSC_OP_ERASE) return HSC_ENOENT;
        blob = p->blob;
        blob_len = p->blob_len;
    } else {
        size_t len = 0;
        int rc = s->be.get(s->be.ctx, ns, key, NULL, 0, &len);
        if (rc != HSC_OK) return rc == HSC_ENOENT ? HSC_ENOENT : HSC_EIO;
        if (len < 1) return HSC_EIO;          /* 连版本字节都没有：不是我们写的 */
        owned = s->alloc(len);
        if (!owned) return HSC_ENOMEM;
        size_t got = len;
        rc = s->be.get(s->be.ctx, ns, key, owned, len, &got);
        if (rc != HSC_OK || got != len) {
            s->dealloc(owned);
            return HSC_EIO;
        }
        blob = owned;
        blob_len = len;
    }

    int rc = HSC_OK;
    if (blob[0] == 0) {
        rc = HSC_EIO;                          /* 版本 0 从不写出去：坏数据 */
    } else if (blob[0] > max_ver) {
        rc = HSC_ENEWER;
    } else if (blob_len - 1 > *len_inout) {
        *len_inout = blob_len - 1;
        rc = HSC_ETOOBIG;
    } else {
        *ver = blob[0];
        *len_inout = blob_len - 1;
        if (blob_len > 1) memcpy(buf, blob + 1, blob_len - 1);
    }
    if (owned) s->dealloc(owned);
    return rc;
}

uint32_t hsc_next_due_in(const hsc_t *s, uint32_t now_ms)
{
    uint32_t best = UINT32_MAX;
    for (int i = 0; i < HSC_PENDING_MAX; i++) {
        const hsc_pending_t *p = &s->slots[i];
        if (!p->used) continue;
        int32_t d = (int32_t)(p->due_ms - now_ms);
        uint32_t in = d <= 0 ? 0 : (uint32_t)d;
        if (in < best) best = in;
    }
    return best;
}

int hsc_pending_count(const hsc_t *s)
{
    int n = 0;
    for (int i = 0; i < HSC_PENDING_MAX; i++) n += s->slots[i].used ? 1 : 0;
    return n;
}

int hsc_drop_ns(hsc_t *s, const char *ns)
{
    if (!s || !ns) return 0;
    int n = 0;
    for (int i = 0; i < HSC_PENDING_MAX; i++) {
        if (s->slots[i].used && strcmp(s->slots[i].ns, ns) == 0) {
            release(s, &s->slots[i]);
            n++;
        }
    }
    return n;
}

void hsc_drop_all(hsc_t *s)
{
    for (int i = 0; i < HSC_PENDING_MAX; i++) {
        if (s->slots[i].used) release(s, &s->slots[i]);
    }
}
