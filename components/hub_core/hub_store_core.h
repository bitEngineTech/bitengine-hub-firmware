#pragma once
/* hub_store 的纯逻辑：防抖合并、读己之写、格式版本规则、重试计数（docs/15_muse_direct_design_v1.md 2.5）。
 *
 * 不依赖 ESP-IDF：存储后端经函数指针注入（固件里是 hubdb 分区上的 NVS，test_host 里是内存表），
 * 时间由调用方传入。hub_store.c 只做任务、队列、NVS 和事件这层胶水。
 *
 * 存储格式：每个键一个 blob，第一个字节是格式版本（1..255），其后是负载。
 *  - 读：已存版本 > 调用方认识的最高版本 → HSC_ENEWER，当作没有（固件被降级了，规则 4）；
 *        已存版本 ≤ 认识的最高版本 → 连同版本号交回，旧版本由调用方迁移。
 *  - 写：已存版本 > 这次要写的版本 → 不覆盖（refused_newer++），留给再升级回去时用。
 *
 * 不是线程安全的：只许 hub_store 任务一个调用者（hub_store.c 的队列保证）。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HSC_NAME_MAX      15      /* NVS 命名空间与键名的上限（NVS_KEY_NAME_MAX_SIZE - 1） */
#define HSC_BLOB_MAX      4000    /* 单个负载上限（不含版本字节）；zhub 设备库按条存，够用（设计 8.9） */
#define HSC_PENDING_MAX   16      /* 同时挂着等写的键数；满了就先把最早到期的那条写掉 */
#define HSC_DEBOUNCE_MS   2000    /* 关键设计决策 7：配置写入 2 s 防抖 */
#define HSC_MAX_TRIES     3       /* 设计 2.5 规则 3：共试 3 次（首次 + 隔 HSC_RETRY_MS 重试 2 次）仍失败 → 5 s 红闪 + 日志 */
#define HSC_RETRY_MS      500     /* 失败后隔多久再试 */

/* 返回码（负数为错误） */
#define HSC_OK            0
#define HSC_ENOENT       (-1)     /* 没有这个键（或挂着一条待执行的删除） */
#define HSC_EINVAL       (-2)     /* 参数不对：名字空 / 超长 / 含非法字符，版本为 0，负载超长 */
#define HSC_ENOMEM       (-3)
#define HSC_ENEWER       (-4)     /* 已存的格式版本比调用方认识的新：当作没有 */
#define HSC_EIO          (-5)     /* 后端读写失败 */
#define HSC_ETOOBIG      (-6)     /* 调用方给的缓冲装不下 */

typedef struct {
    void *ctx;
    /* 读：buf == NULL 时只回长度。返回 HSC_OK / HSC_ENOENT / HSC_ETOOBIG（cap 不够，*len 写入需要的长度）/ HSC_EIO。
       *len 是整个 blob 的长度（含版本字节）。 */
    int (*get)(void *ctx, const char *ns, const char *key, uint8_t *buf, size_t cap, size_t *len);
    /* 写（含提交）。返回 HSC_OK / HSC_EIO */
    int (*set)(void *ctx, const char *ns, const char *key, const uint8_t *buf, size_t len);
    /* 删（含提交）；本来就没有也算 HSC_OK */
    int (*erase)(void *ctx, const char *ns, const char *key);
} hsc_backend_t;

typedef enum { HSC_OP_PUT = 0, HSC_OP_ERASE = 1 } hsc_op_t;

typedef struct {
    bool     used;
    hsc_op_t op;
    char     ns[HSC_NAME_MAX + 1];
    char     key[HSC_NAME_MAX + 1];
    uint8_t *blob;            /* PUT：版本字节 + 负载，归本模块所有 */
    size_t   blob_len;
    uint32_t due_ms;          /* 到这个时刻执行 */
    uint8_t  tries;           /* 已失败的次数 */
} hsc_pending_t;

typedef struct {
    uint32_t writes;          /* 写进后端成功的次数（含删除） */
    uint32_t coalesced;       /* 防抖窗口里被后来者合并掉的写 */
    uint32_t retries;         /* 失败后重排的次数 */
    uint32_t failures;        /* 重试用尽、最终放弃的次数 */
    uint32_t refused_newer;   /* 已存版本更新、拒绝覆盖的次数 */
    int      last_err;        /* 最近一次后端错误（HSC_*） */
    char     last_refused_ns[HSC_NAME_MAX + 1];    /* 最近一次被拒的键（规则 4 要记日志，胶水层据此打） */
    char     last_refused_key[HSC_NAME_MAX + 1];
    uint8_t  last_refused_stored, last_refused_ver;   /* 已存的版本 / 这次想写的版本 */
} hsc_stats_t;

typedef struct {
    hsc_backend_t be;
    void *(*alloc)(size_t);
    void  (*dealloc)(void *);
    hsc_pending_t slots[HSC_PENDING_MAX];
    hsc_stats_t   stats;
} hsc_t;

void     hsc_init(hsc_t *s, const hsc_backend_t *be, void *(*alloc)(size_t), void (*dealloc)(void *));

/* 排一条写：同一个键已在等的就合并（后来者赢，到期时刻顺延到 now + 防抖）。表满时先执行最早到期的那条。 */
int      hsc_put(hsc_t *s, const char *ns, const char *key, uint8_t ver, const void *data, size_t len, uint32_t now_ms);

/* 排一条删除：立即到期（"先删后回"一类的语义要的是快，比如 device.unpair，设计 4.10）；顶掉该键在等的写。 */
int      hsc_erase(hsc_t *s, const char *ns, const char *key, uint32_t now_ms);

/* 读：先看等写表（读己之写；等着删除的返回 HSC_ENOENT），再读后端。成功时 *ver 为格式版本，
   负载拷进 buf（不含版本字节），*len_inout 进来是 buf 容量、出去是负载长度。 */
int      hsc_get(hsc_t *s, const char *ns, const char *key, uint8_t max_ver,
                 uint8_t *ver, void *buf, size_t *len_inout);

/* 执行到期的条目（force = 不管到没到期，全做）。返回这一轮"重试用尽、最终放弃"的条数，调用方据此点灯。 */
int      hsc_run(hsc_t *s, uint32_t now_ms, bool force);

/* 离下一条到期还有多少毫秒；没有等写的条目返回 UINT32_MAX。 */
uint32_t hsc_next_due_in(const hsc_t *s, uint32_t now_ms);

int      hsc_pending_count(const hsc_t *s);

/* 丢弃全部等写条目并释放内存（不写后端）。恢复出厂整区擦 hubdb 之前、测试换段时用。 */
void     hsc_drop_all(hsc_t *s);
/* 丢掉某个命名空间里所有还在等的操作（不写后端），返回丢了几条。恢复出厂擦命名空间之前用：不然擦完它们又被写回去。 */
int      hsc_drop_ns(hsc_t *s, const char *ns);

/* 名字是否合法：非空、≤ HSC_NAME_MAX、只含 [A-Za-z0-9_.-]。 */
bool     hsc_name_ok(const char *name);
