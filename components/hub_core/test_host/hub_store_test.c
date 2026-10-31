/* hub_store 纯逻辑的真值表 —— 主机侧，不用板子（docs/15 设计 2.5）。
 *
 *   bash components/hub_core/test_host/run.sh
 *
 * 编的是产品源文件本身（../hub_store_core.c）。后端是内存表，可以注入写失败。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hub_store_core.h"

static int fails = 0;
#define EXPECT(cond, why) do { if (cond) printf("✅ %s\n", why); else { printf("❌ %s（%s:%d）\n", why, __FILE__, __LINE__); fails++; } } while (0)

/* ---- 内存后端 ---- */
typedef struct { char ns[16], key[16]; uint8_t data[4100]; size_t len; int used; } ent_t;
typedef struct { ent_t e[32]; int sets, erases, fail_sets, fail_gets; } mem_t;

static ent_t *mfind(mem_t *m, const char *ns, const char *key)
{
    for (int i = 0; i < 32; i++)
        if (m->e[i].used && !strcmp(m->e[i].ns, ns) && !strcmp(m->e[i].key, key)) return &m->e[i];
    return NULL;
}
static int mget(void *c, const char *ns, const char *key, uint8_t *buf, size_t cap, size_t *len)
{
    mem_t *m = c;
    if (m->fail_gets > 0) { m->fail_gets--; return HSC_EIO; }
    ent_t *e = mfind(m, ns, key);
    if (!e) return HSC_ENOENT;
    *len = e->len;
    if (!buf) return HSC_OK;
    if (cap < e->len) return HSC_ETOOBIG;
    memcpy(buf, e->data, e->len);
    return HSC_OK;
}
static int mset(void *c, const char *ns, const char *key, const uint8_t *buf, size_t len)
{
    mem_t *m = c;
    if (m->fail_sets > 0) { m->fail_sets--; return HSC_EIO; }
    ent_t *e = mfind(m, ns, key);
    for (int i = 0; !e && i < 32; i++) if (!m->e[i].used) e = &m->e[i];
    if (!e) return HSC_EIO;
    e->used = 1; strcpy(e->ns, ns); strcpy(e->key, key); memcpy(e->data, buf, len); e->len = len;
    m->sets++;
    return HSC_OK;
}
static int merase(void *c, const char *ns, const char *key)
{
    mem_t *m = c;
    ent_t *e = mfind(m, ns, key);
    if (e) e->used = 0;
    m->erases++;
    return HSC_OK;
}

static int allocs = 0;
static void *talloc(size_t n) { allocs++; return malloc(n); }
static void tfree(void *p) { if (p) allocs--; free(p); }

static int inited = 0;
static void setup(hsc_t *s, mem_t *m)
{
    if (inited) hsc_drop_all(s);          /* 上一段留下的等写条目释放掉，ASan 才能盯真泄漏 */
    inited = 1;
    memset(m, 0, sizeof(*m));
    hsc_backend_t be = { .ctx = m, .get = mget, .set = mset, .erase = merase };
    hsc_init(s, &be, talloc, tfree);
}

int main(void)
{
    hsc_t s; mem_t m; uint8_t buf[64]; size_t len; uint8_t ver;

    /* ---- 防抖：2 s 内连写三次只落盘一次，落的是最后那次 ---- */
    setup(&s, &m);
    EXPECT(hsc_put(&s, "hub", "meta", 1, "a", 1, 1000) == HSC_OK, "put 合法");
    EXPECT(hsc_put(&s, "hub", "meta", 1, "bb", 2, 1500) == HSC_OK, "同键再 put → 合并");
    EXPECT(hsc_put(&s, "hub", "meta", 1, "ccc", 3, 2000) == HSC_OK, "同键第三次");
    EXPECT(s.stats.coalesced == 2, "合并计数 2");
    hsc_run(&s, 3999, false);
    EXPECT(m.sets == 0, "最后一次 put 后 1999 ms：还没写（防抖从最后一次算）");
    EXPECT(hsc_next_due_in(&s, 3999) == 1, "离到期还有 1 ms");
    hsc_run(&s, 4000, false);
    EXPECT(m.sets == 1, "满 2000 ms：写了一次");
    len = sizeof(buf);
    EXPECT(hsc_get(&s, "hub", "meta", 1, &ver, buf, &len) == HSC_OK && len == 3 && !memcmp(buf, "ccc", 3) && ver == 1,
           "落盘的是最后那次（ccc），版本 1");
    EXPECT(hsc_pending_count(&s) == 0 && hsc_next_due_in(&s, 4000) == UINT32_MAX, "等写表空了");

    /* ---- 读己之写：还没落盘就能读到；等着删除的读不到 ---- */
    setup(&s, &m);
    hsc_put(&s, "muse", "tok", 2, "xyz", 3, 0);
    len = sizeof(buf);
    EXPECT(hsc_get(&s, "muse", "tok", 2, &ver, buf, &len) == HSC_OK && len == 3 && ver == 2 && m.sets == 0,
           "未落盘的值读得到（读己之写），后端还没写");
    hsc_erase(&s, "muse", "tok", 10);
    len = sizeof(buf);
    EXPECT(hsc_get(&s, "muse", "tok", 2, &ver, buf, &len) == HSC_ENOENT, "删除顶掉等写的值 → 读不到");
    EXPECT(hsc_next_due_in(&s, 10) == 0, "删除不防抖，立即到期（device.unpair 要先删后回）");
    hsc_run(&s, 10, false);
    EXPECT(m.erases == 1 && m.sets == 0, "只执行了删除，被顶掉的写没有落盘");

    /* ---- 版本规则（设计 2.5 规则 4）---- */
    setup(&s, &m);
    hsc_put(&s, "zhub", "dev1", 3, "v3", 2, 0); hsc_run(&s, 0, true);
    len = sizeof(buf);
    EXPECT(hsc_get(&s, "zhub", "dev1", 2, &ver, buf, &len) == HSC_ENEWER, "已存版本 3 > 认识的 2 → ENEWER（当作没有）");
    len = sizeof(buf);
    EXPECT(hsc_get(&s, "zhub", "dev1", 5, &ver, buf, &len) == HSC_OK && ver == 3, "认识更高版本的读到版本 3（由调用方迁移）");
    hsc_put(&s, "zhub", "dev1", 2, "v2", 2, 0); hsc_run(&s, 0, true);
    len = sizeof(buf);
    hsc_get(&s, "zhub", "dev1", 9, &ver, buf, &len);
    EXPECT(ver == 3 && s.stats.refused_newer == 1 && !memcmp(buf, "v3", 2), "降级固件写版本 2 → 不覆盖版本 3（留给升级回去时用）");
    EXPECT(!strcmp(s.stats.last_refused_ns, "zhub") && !strcmp(s.stats.last_refused_key, "dev1")
           && s.stats.last_refused_stored == 3 && s.stats.last_refused_ver == 2, "被拒的键与两个版本号留给胶水层打日志");
    hsc_put(&s, "zhub", "dev1", 3, "w3", 2, 0); hsc_run(&s, 0, true);
    len = sizeof(buf);
    hsc_get(&s, "zhub", "dev1", 9, &ver, buf, &len);
    EXPECT(ver == 3 && !memcmp(buf, "w3", 2), "同版本照常覆盖");
    hsc_put(&s, "zhub", "dev1", 4, "x4", 2, 0); hsc_run(&s, 0, true);
    len = sizeof(buf);
    hsc_get(&s, "zhub", "dev1", 9, &ver, buf, &len);
    EXPECT(ver == 4 && !memcmp(buf, "x4", 2), "更高版本照常覆盖（升级迁移）");

    /* ---- 重试：失败 2 次第 3 次成功；失败 3 次放弃（设计 2.5 规则 3）---- */
    setup(&s, &m);
    m.fail_sets = 2;
    hsc_put(&s, "hub", "a", 1, "1", 1, 0);
    EXPECT(hsc_run(&s, 2000, false) == 0 && m.sets == 0 && s.stats.retries == 1, "第 1 次失败 → 重排");
    EXPECT(hsc_next_due_in(&s, 2000) == HSC_RETRY_MS, "隔 HSC_RETRY_MS 再试");
    EXPECT(hsc_run(&s, 2000 + HSC_RETRY_MS, false) == 0 && s.stats.retries == 2, "第 2 次失败 → 再重排");
    EXPECT(hsc_run(&s, 2000 + 2 * HSC_RETRY_MS, false) == 0 && m.sets == 1 && s.stats.failures == 0, "第 3 次成功");
    setup(&s, &m);
    m.fail_sets = 3;
    hsc_put(&s, "hub", "a", 1, "1", 1, 0);
    hsc_run(&s, 2000, false); hsc_run(&s, 2500, false);
    EXPECT(hsc_run(&s, 3000, false) == 1 && s.stats.failures == 1 && s.stats.last_err == HSC_EIO,
           "连败 3 次 → 放弃，返回 1（胶水据此 5 s 红闪）");
    EXPECT(hsc_pending_count(&s) == 0, "放弃的条目移出等写表");
    setup(&s, &m);
    m.fail_gets = 1;
    hsc_put(&s, "hub", "a", 1, "1", 1, 0);
    EXPECT(hsc_run(&s, 2000, false) == 0 && s.stats.retries == 1, "写之前读旧版本失败也算一次失败，会重试");

    /* ---- 参数（P3）---- */
    setup(&s, &m);
    EXPECT(hsc_put(&s, "", "k", 1, "x", 1, 0) == HSC_EINVAL, "空命名空间 → EINVAL");
    EXPECT(hsc_put(&s, "hub", "0123456789abcdef", 1, "x", 1, 0) == HSC_EINVAL, "16 字符的键 → EINVAL（NVS 上限 15）");
    EXPECT(hsc_put(&s, "hub", "0123456789abcde", 1, "x", 1, 0) == HSC_OK, "15 字符的键 → OK");
    EXPECT(hsc_put(&s, "hub", "a b", 1, "x", 1, 0) == HSC_EINVAL, "键里有空格 → EINVAL");
    EXPECT(hsc_put(&s, "hub", "k", 0, "x", 1, 0) == HSC_EINVAL, "版本 0 → EINVAL（0 留给\"坏数据\"）");
    EXPECT(hsc_put(&s, "hub", "k", 1, NULL, 1, 0) == HSC_EINVAL, "有长度没数据 → EINVAL");
    static uint8_t big[HSC_BLOB_MAX + 1];
    EXPECT(hsc_put(&s, "hub", "k", 1, big, sizeof(big), 0) == HSC_EINVAL, "负载超过 HSC_BLOB_MAX → EINVAL");
    EXPECT(hsc_put(&s, "hub", "k", 1, big, HSC_BLOB_MAX, 0) == HSC_OK, "正好 HSC_BLOB_MAX → OK");
    EXPECT(hsc_put(&s, "hub", "e", 1, NULL, 0, 0) == HSC_OK, "空负载合法（只有版本字节）");
    len = 2;
    hsc_run(&s, 0, true);
    EXPECT(hsc_get(&s, "hub", "k", 1, &ver, buf, &len) == HSC_ETOOBIG && len == HSC_BLOB_MAX, "缓冲不够 → ETOOBIG，告诉要多大");
    len = sizeof(buf);
    EXPECT(hsc_get(&s, "hub", "nope", 1, &ver, buf, &len) == HSC_ENOENT, "没有的键 → ENOENT");
    EXPECT(hsc_get(&s, "hub", "k", 1, NULL, buf, &len) == HSC_EINVAL, "ver 为 NULL → EINVAL");

    /* ---- 坏数据：版本字节为 0、或空 blob ---- */
    setup(&s, &m);
    mset(&m, "hub", "z", (const uint8_t *)"\0abc", 4); m.sets = 0;
    len = sizeof(buf);
    EXPECT(hsc_get(&s, "hub", "z", 9, &ver, buf, &len) == HSC_EIO, "版本字节为 0 → EIO（不是我们写的）");
    mset(&m, "hub", "y", (const uint8_t *)"", 0);
    len = sizeof(buf);
    EXPECT(hsc_get(&s, "hub", "y", 9, &ver, buf, &len) == HSC_EIO, "0 字节的 blob → EIO");

    /* ---- 表满：先把最早到期的写掉，再收新的 ---- */
    setup(&s, &m);
    char key[8];
    for (int i = 0; i < HSC_PENDING_MAX; i++) { snprintf(key, sizeof(key), "k%d", i); hsc_put(&s, "hub", key, 1, "x", 1, (uint32_t)i); }
    EXPECT(hsc_pending_count(&s) == HSC_PENDING_MAX && m.sets == 0, "16 条都在等");
    EXPECT(hsc_put(&s, "hub", "extra", 1, "y", 1, 100) == HSC_OK && m.sets == 1, "第 17 条：先写掉最早到期的 k0，再收下");
    len = sizeof(buf);
    EXPECT(mget(&m, "hub", "k0", buf, sizeof(buf), &len) == HSC_OK, "  被腾掉的是 k0");

    /* ---- 回绕：now 跨过 2^32 ---- */
    setup(&s, &m);
    hsc_put(&s, "hub", "w", 1, "x", 1, 0xFFFFFFFFu - 500);
    hsc_run(&s, 0xFFFFFFFFu, false);
    EXPECT(m.sets == 0, "回绕前没到期");
    hsc_run(&s, 1500, false);     /* 0xFFFFFFFF-500 + 2000 回绕后 = 1499 */
    EXPECT(m.sets == 1, "回绕后到期照常写");

    /* ---- 内存：所有分配都还回去了 ---- */
    setup(&s, &m);
    hsc_put(&s, "hub", "a", 1, "1", 1, 0); hsc_put(&s, "hub", "a", 1, "2", 1, 0);
    hsc_erase(&s, "hub", "a", 0); hsc_put(&s, "hub", "b", 1, "3", 1, 0);
    hsc_run(&s, 0, true);
    len = sizeof(buf); hsc_get(&s, "hub", "b", 1, &ver, buf, &len);
    EXPECT(allocs == 0, "合并、顶掉、执行、读之后没有泄漏");
    hsc_put(&s, "hub", "c", 1, "4", 1, 0); hsc_erase(&s, "hub", "d", 0);
    int sets0 = m.sets, erases0 = m.erases;
    hsc_drop_all(&s);
    EXPECT(allocs == 0 && hsc_pending_count(&s) == 0 && m.sets == sets0 && m.erases == erases0,
           "drop_all：全部释放、不写后端");

    /* ---- drop_ns：只丢一个命名空间（恢复出厂擦 muse 之前用），别的照写 ---- */
    setup(&s, &m);
    hsc_put(&s, "muse", "acc_tok", 1, "A", 1, 0); hsc_erase(&s, "muse", "x", 0);
    hsc_put(&s, "zhub", "dev1", 1, "Z", 1, 0);
    int dropped = hsc_drop_ns(&s, "muse");
    EXPECT(dropped == 2 && hsc_pending_count(&s) == 1, "drop_ns：muse 的两条丢掉，zhub 那条还在");
    sets0 = m.sets; erases0 = m.erases;
    hsc_run(&s, 0, true);
    EXPECT(m.sets == sets0 + 1 && m.erases == erases0, "drop_ns：之后只写 zhub，muse 一条都不写回去");
    EXPECT(hsc_drop_ns(&s, "muse") == 0 && hsc_drop_ns(NULL, "muse") == 0 && hsc_drop_ns(&s, NULL) == 0,
           "drop_ns：没有可丢的 / NULL 不崩");
    hsc_drop_all(&s);
    EXPECT(allocs == 0, "drop_ns 之后没有泄漏");

    printf("\n%s\n", fails ? "有失败" : "全部通过");
    return fails ? 1 : 0;
}
