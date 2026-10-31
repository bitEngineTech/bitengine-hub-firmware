#pragma once
/* hub_json —— 流式 JSON 写入器（docs/15_muse_direct_design_v1.md 6.9）。
 *
 * 直接写进调用方给的缓冲（会话与命令层的缓冲都在 PSRAM），不建 cJSON 树：CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024
 * 会让 cJSON 的每个小节点落进内部内存。纯 C、不依赖 ESP-IDF：test_host/ 原样编它。
 *
 * 规则：
 *   - 每个记号整体写入：放不下就一个字节都不写，置 truncated，之后的写入全部忽略（只有 hj_finish 还干活）。
 *     所以缓冲里永远不会有半截字符串、半截转义或被切开的 UTF-8。
 *   - 闭合括号的空间预先留着：任何时候 hj_finish 都能把还开着的对象 / 数组合法地闭合。
 *   - 列表分页：每写一项之前 hj_mark，写完发现 truncated 就 hj_rollback 把这一项整个撤掉，再写 truncated / next_cursor。
 *     这几个尾字段的空间用 hj_set_reserve 预留（预留的部分普通写入用不了，hj_set_reserve(w, 0) 放出来）。
 *   - 字符串：转义 " \ 和 C0 控制字符；不合法的 UTF-8 每个坏字节换成 '?'，输出永远是合法 JSON。
 *     这不是字符串卫生（6.8 在 hub_sanitize）：这里只保证 JSON 合法。
 *   - 对象里键和值是一体的：键写进去了、值放不下，键也撤掉，闭合之后不会出现 {"a":}。
 *   - 用错（对象里不写键就写值、数组里写键、括号不配对、嵌套超过 HJ_MAX_DEPTH、顶层写第二个值、在键之后 mark、
 *     截断之后 mark —— 那样 rollback 会把"前面已经丢了字段"的 truncated 一并清掉、rollback 到不是最近一次的 mark）
 *     置 error，hj_finish 返回 -1。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HJ_MAX_DEPTH 12

typedef struct {
    char    *buf;
    size_t   cap;          /* 含结尾 NUL */
    size_t   len;
    size_t   reserve;      /* 留给 hj_rollback 之后的尾字段 */
    uint8_t  depth;
    char     stack[HJ_MAX_DEPTH];   /* '{' 或 '[' */
    bool     need_comma;   /* 当前容器里已经有成员 */
    bool     after_key;    /* 对象里刚写了键，等值 */
    bool     key_had_comma;/* 那个键前面写了逗号（值放不下时连键一起撤，要恢复 need_comma） */
    size_t   key_start;    /* 那个键（含前导逗号）开始的位置 */
    bool     top_done;     /* 顶层已经写过一个值 */
    bool     truncated;
    bool     error;
    uint8_t  mark_floor;   /* 最近一次 hj_mark 时的深度 */
    bool     mark_broken;  /* mark 之后弹栈弹到了 mark_floor 以下：那个 mark 不能再用 */
    uint16_t mark_gen;     /* 每次 hj_mark 加一：rollback 只认最近那个 mark，旧的一律判错 */
} hj_t;

typedef struct {           /* hj_mark 存下的位置；只有最近一次 mark 有效（代号比对），只对同一层嵌套有效 */
    size_t   len;
    uint8_t  depth;
    bool     need_comma, top_done;
    uint16_t gen;
} hj_mark_t;

/* cap 至少 2（"{}" 之外再加 NUL 的最小情形由调用方保证；太小时第一次写入就 truncated） */
void hj_init(hj_t *w, char *buf, size_t cap);
void hj_set_reserve(hj_t *w, size_t n);              /* n 超过 cap 按 cap 算（= 普通写入一律放不下） */

void hj_obj_begin(hj_t *w);
void hj_obj_end(hj_t *w);
void hj_arr_begin(hj_t *w);
void hj_arr_end(hj_t *w);
void hj_key(hj_t *w, const char *k);
void hj_str(hj_t *w, const char *s);                 /* s 为 NULL 写 null */
void hj_strn(hj_t *w, const char *s, size_t n);      /* 不要求 NUL 结尾 */
void hj_int(hj_t *w, int64_t v);
void hj_uint(hj_t *w, uint64_t v);
void hj_bool(hj_t *w, bool v);
void hj_null(hj_t *w);
/* 原样写一个值（json 必须是一个完整、合法的 JSON 值 —— 只给本固件自己用 hj 写出来的文本用，比如作业的结果体；
   不校验内容。放不下同普通记号：整个不写、置 truncated） */
void hj_raw(hj_t *w, const char *json);

/* 对象里的 "键":值 一步写完 */
void hj_kstr(hj_t *w, const char *k, const char *v);
void hj_kint(hj_t *w, const char *k, int64_t v);
/* 定点数：v / 10^decimals，小数位照写（-512, 2 → -5.12；10130, 1 → 1013.0）。decimals ≤ 6；读数用（M5 第一步） */
void hj_fixed(hj_t *w, int64_t v, unsigned decimals);
void hj_kfixed(hj_t *w, const char *k, int64_t v, unsigned decimals);
void hj_kuint(hj_t *w, const char *k, uint64_t v);
void hj_kbool(hj_t *w, const char *k, bool v);

hj_mark_t hj_mark(hj_t *w);
/* 回到 mark 的位置并清掉 truncated。mark 之后弹栈弹到了 mark 那一层以下（那一层的栈内容可能已被覆盖）：置 error。 */
void hj_rollback(hj_t *w, hj_mark_t m);

/* 闭合所有还开着的容器、写 NUL。返回长度（不含 NUL）；用错返回 -1（缓冲内容不可用）。 */
int hj_finish(hj_t *w);
