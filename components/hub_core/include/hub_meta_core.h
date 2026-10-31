#pragma once
/* hub_meta_core —— 设备元数据（用户起的名字、房间、推送开关、敏感标记）的表与分页序列化，纯逻辑（docs/15_muse_direct_design_v1.md 6.1
 * "元数据持久化"、附录 A hub.device.update；M5 第二步）。不依赖 ESP-IDF：test_host/hub_meta_test 原样编它。存取与加锁在 hub_meta.c。
 *
 * 与设计 6.1 的出入：设计写"合成一个 blob：hub/meta，约 12 KB"，可 hub_store 单个 blob 最多 HSC_BLOB_MAX（4000 B）——
 * 拆成 HUB_META_PAGES 页（键 meta0…），每页 HUB_META_PER_PAGE 条，条目带页内槽号；改一条只重写它所在的那一页（hub_store 2 s 防抖）。
 * id 存成 [协议 1 B][8 B]（"zb-" / "mt-" 后的 16 位十六进制，高字节在前），不按每台设备一个键（NVS 键名放不下，6.1 原话）。
 * 名字 / 房间按 6.8 的用户名规则净化（hub_sanitize_user_name：拒换行、≤ HUB_NAME_MAX / HUB_ROOM_MAX 字节）；空 = 清掉。
 * 三样都回到默认（没名字、没房间、标志全 0）的条目自动腾出位置。敏感标记只给网页改（Muse 不能改：hub.device.update 没有这个参数，6.2）。
 * 有页这次开机读不出来时不新建条目（回 STORE）：那一页看不见，证明不了这台设备不在里面 —— 在别的页另建一条，重启后同一 id 两条、旧值遮住新值（两轮审计 ❌）。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hub_sanitize.h"

#define HUB_META_MAX          240u      /* Zigbee 64 + Matter 128 + 48 条余量（2026-10-24 Matter 32 → 128；设计 6.1 原写 192）。整页：每页 48 条、页掩码 8 位（≤ 384） */
#define HUB_META_PER_PAGE     48u
#define HUB_META_PAGES        (HUB_META_MAX / HUB_META_PER_PAGE)
#define HUB_META_ALL_PAGES    ((1u << HUB_META_PAGES) - 1u)   /* page_ok 全为 1 */
#define HUB_META_VER          1u        /* hub_store 的格式版本字节 */
#define HUB_META_F_NOTIFY     0x01u     /* 门磁 / 人体这类 SECURITY 事件推送给 Muse（6.7：类别位 AND 设备位） */
#define HUB_META_F_SENSITIVE  0x02u     /* 敏感（6.2：只给网页改） */
enum { HUB_META_PROTO_NONE = 0, HUB_META_PROTO_ZIGBEE = 1, HUB_META_PROTO_MATTER = 2 };

typedef struct {
    uint8_t proto;                      /* HUB_META_PROTO_*；NONE = 空位 */
    uint8_t id8[8];                     /* 高字节在前（与 id 串同序） */
    uint8_t flags;                      /* HUB_META_F_* */
    char    name[HUB_NAME_MAX + 1];
    char    room[HUB_ROOM_MAX + 1];
} hub_meta_ent_t;

/* 页的序列化：[条数][([页内槽号][协议][8 B id][标志][名字 HUB_NAME_MAX+1][房间 HUB_ROOM_MAX+1]) × 条数]（名字 / 房间定长、以 0 结尾） */
#define HUB_META_ENT_BYTES    (1u + 1u + 8u + 1u + (HUB_NAME_MAX + 1u) + (HUB_ROOM_MAX + 1u))
#define HUB_META_PAGE_BYTES   (1u + HUB_META_PER_PAGE * HUB_META_ENT_BYTES)
_Static_assert(HUB_META_PAGE_BYTES <= 4000u, "一页要放进 hub_store 的一个 blob（HSC_BLOB_MAX 4000）");
_Static_assert(HUB_META_PAGES * HUB_META_PER_PAGE == HUB_META_MAX, "页数 × 每页 = 总条数");
_Static_assert(HUB_META_PAGES <= 8u, "只读页掩码（hub_meta_stats_t.pages_ro）只有 8 位");

typedef struct {
    hub_meta_ent_t e[HUB_META_MAX];
} hub_meta_core_t;

/* 一次修改：NULL = 不改；notify < 0 = 不改；sensitive 默认 0 = 不改（只有本地 /api/v1/hub/device/sensitive 设它 —— 6.2：Muse 改不了；
   故意让全零 = 不改：按位置初始化前三项的调用方（hub.device.update）一定碰不到它） */
enum { HUB_META_SENS_KEEP = 0, HUB_META_SENS_OFF = 1, HUB_META_SENS_ON = 2 };
typedef struct {
    const char *name;
    const char *room;
    int         notify;
    uint8_t     sensitive;        /* HUB_META_SENS_* */
} hub_meta_change_t;

typedef enum {
    HUB_META_OK = 0,
    HUB_META_BAD_ID,          /* 不是 "zb-" / "mt-" + 16 位十六进制 */
    HUB_META_NOTHING,         /* 三样都没给 */
    HUB_META_NAME_NEWLINE, HUB_META_NAME_LONG,
    HUB_META_ROOM_NEWLINE, HUB_META_ROOM_LONG,
    HUB_META_FULL,            /* 表满 */
    HUB_META_STORE,           /* 有页这次开机读不出来：它所在的页只读，新条目一律不建（重启网关才可能好） */
    HUB_META_BUSY,            /* 锁拿不到 / 写没排上（hub_meta.c；过一会儿再试） */
} hub_meta_res_t;

/* 开机读一页：hub_store 读的结果（hub_meta.c 把 esp_err_t 映射成这几种）→ 这一页怎么办 */
typedef enum {
    HUB_META_RD_OK = 0,       /* 读到了（ver 是存的格式版本） */
    HUB_META_RD_NONE,         /* 没存过 */
    HUB_META_RD_DOWN,         /* hub_store 整个没起来（写也写不进去，不会盖掉什么） */
    HUB_META_RD_TIMEOUT,      /* 超时（hub_meta.c 剩下的页不再等，一律只读） */
    HUB_META_RD_ERR,          /* 其余：读错、存的格式比固件新（INVALID_VERSION）、比一页还大 */
} hub_meta_rd_t;
typedef enum {
    HUB_META_LD_EMPTY = 0,    /* 空页、可写 */
    HUB_META_LD_LOADED,       /* 读回来了 */
    HUB_META_LD_CORRUPT,      /* 版本对、结构坏：当空页、可写（盖掉无妨） */
    HUB_META_LD_READONLY,     /* 读不出来：这一页这次开机只读，内存里是空的 */
} hub_meta_ld_t;

void hub_meta_core_init(hub_meta_core_t *m);
/* "zb-0011223344556677" / "mt-…"（十六进制大小写都认）→ 协议 + 8 B */
bool hub_meta_parse_id(const char *id, uint8_t *proto, uint8_t id8[8]);
/* 槽号；没有 -1 */
int  hub_meta_find(const hub_meta_core_t *m, uint8_t proto, const uint8_t id8[8]);
/* 改一条。page_ok：第 i 位 = 第 i 页能写（新条目只分配到能写的页；已有条目所在页不能写回 HUB_META_STORE）。*slot_out = 改动所在的槽（-1 = 没改动）。
   条目三样都回到默认时腾出位置（slot_out 仍是那个槽：那一页要重写） */
hub_meta_res_t hub_meta_apply(hub_meta_core_t *m, uint8_t proto, const uint8_t id8[8], const hub_meta_change_t *c, uint32_t page_ok, int *slot_out);
/* 序列化第 page 页；返回字节数（写不下 0） */
size_t hub_meta_pack(const hub_meta_core_t *m, unsigned page, uint8_t *buf, size_t cap);
/* 清掉第 page 页里某个协议的全部条目（重置 Matter 控制器用：节点号从 2 重新发，旧名字 / 敏感标记不能落到新设备头上）。返回清了几条 */
unsigned hub_meta_clear_proto(hub_meta_core_t *m, uint8_t proto, unsigned page);
/* 读回第 page 页（覆盖那一页的槽）。结构不对（长度、槽号越界或重复、协议不认识、名字 / 房间没以 0 结尾）回 -1、那一页不动；
   否则回丢掉的条数：名字 / 房间过不了现在的净化规则的那条单独丢掉（规则收紧时别让一条连累整页 48 条，第一轮审计） */
int  hub_meta_unpack(hub_meta_core_t *m, unsigned page, const uint8_t *buf, size_t len);
/* 开机读一页的判定（真值表在 test_host/hub_meta_test）。*dropped = 读回时丢掉的条数（可为 NULL） */
hub_meta_ld_t hub_meta_load_page(hub_meta_core_t *m, unsigned page, hub_meta_rd_t rd, uint8_t ver, const uint8_t *buf, size_t len, unsigned *dropped);
/* 全部页读完后：同一台设备出现在两个槽（存储被改坏）只留槽号小的那条，回去掉的条数；*pages 第 i 位 = 第 i 页去掉过条目（调用方要立即重写这些页：
   不写的话用户以后清掉留下的那条，去掉的这条重启后会复活 —— 修复复核） */
unsigned hub_meta_dedup(hub_meta_core_t *m, uint32_t *pages);
/* 事件句子里的设备名（6.7 模板）：起了名字 → "名字"，有房间再加 " (房间)"；没起名 → "unnamed <设备类> <id 末四位>"
   （设备类里的 _ 换成空格；kind 为 NULL / "unknown" 时用 "Zigbee device" / "Matter device"）。名字 / 房间里的 " 换成 '、[ ] 换成 ( )，
   同形的 Unicode 引号 / 方括号（弯引号、全角、〝〞〟、【】〔〕⟦⟧﹝﹞，表在 hub_meta_core.c 的 ev_copy）一样换：
   句子里的引号是名字的边界，[id] 是推送尾巴的格式，别让名字自己关上引号或伪造一个 id（两轮审计）。返回写入长度 */
#define HUB_META_EVNAME_MAX   (2u + HUB_NAME_MAX + 2u + HUB_ROOM_MAX + 1u)   /* "名字" (房间)，不含结尾 0 */
size_t hub_meta_event_name(const hub_meta_ent_t *m, uint8_t proto, const char *kind, uint16_t tail, char *out, size_t cap);
const char *hub_meta_res_str(hub_meta_res_t r);
