/* srp_readvertise.c — 网关重启后强制重新宣告 SRP 代播的 mDNS 服务
 *
 * ⚠️⚠️ 适用范围（2026-08-15 实测后补，动它之前先读这一段）⚠️⚠️
 *
 * 本文件下面所有推理的依据都是 **Python 实现**：python matter-server 8.1.0 的
 * device_controller.py，以及 python zeroconf 0.147.0 的 record_manager.py。
 * 整套机制成立的前提是那条链——TTL=0 的 PTR → 缓存移除 → Removed → 再收到同名
 * PTR → Added → 控制器走到有用的分支。
 *
 * **当前这套部署跑的不是它**：ghcr.io/matter-js/matterjs-server（matter.js，
 * TypeScript），mDNS 与发现栈完全另一套。2026-08-15 实测，网关发了 goodbye
 * （日志实证 "sent goodbye for 2 delegated service(s) before reboot"），
 * matter.js 侧**零反应**——那次重启前后整段日志里没有任何 offline / Resolving /
 * Removed / Pairing request / is online，订阅自始至终就没断过。
 *
 * 所以：机制没坏，靶子换了。
 *   - 对 python matter-server 的收益有实证（下面的抓包与源码定位都很具体），
 *     所以不删——代价只有重启时多花 300ms
 *   - 对 matter.js **收益为零，已实测**。别再把它当"网关重启后恢复慢"的解药
 *   - matter.js 侧如果确实存在等效问题，必须重新定位，不能沿用下面这份分析
 *
 * 附带教训：当时观察到的"重启后 6 分钟才恢复"，更可能是终端设备在两个路由器
 * 之间重新选父（见 docs/13），跟订阅根本无关。
 *
 * ── 为什么要有这个文件 ───────────────────────────────────────────────
 *
 * 网关（Thread 边界路由器）一重启，子设备侧的 Matter 会话失效，但 HA 上的
 * matter-server 仍握着已死的订阅对象，只能干等 CHIP 存活超时——电池设备是
 * NODE_SUBSCRIPTION_CEILING_BATTERY_POWERED = 600s。用户看到的是"设备十分钟
 * 不上报"，会以为设备或网关坏了。
 *
 * 之所以能在网关侧修，是因为这些设备的 mDNS 记录**本来就是网关在替它们喊**：
 * 子设备用 SRP 注册到网关内置的 SRP server，libopenthread_br 的 advertising
 * proxy 再通过 ESP-IDF mdns 组件（mdns_delegate_hostname_add /
 * mdns_service_add_for_host）把 _matter._tcp 播到局域网上。
 *
 * 2026-08-10 抓包实证（HA 侧 tcpdump，enp97s0）：
 *     192.168.1.43.5353 > ...
 *       _matter._tcp.local. PTR 088E927C6F07E0AC-0000000000000002._matter._tcp.
 *       ..._matter._tcp. SRV 920E02140E0077C1.local.:5540
 *       920E02140E0077C1.local. AAAA fd16:2b4b:919c:1:...   ← Thread 网内 OMR
 * 全场只有网关这一个源回答 _matter._tcp，且同一个包里还带着网关自己的
 * _meshcop._udp / bitengine.local —— 同一个 mDNS responder 实例在宣告两者。
 *
 * ── 为什么是 bye + 重新宣告，而不是"再播一遍" ────────────────────────
 *
 * 重启后 proxy 本来就会重新发布，但 HA 侧 zeroconf 缓存里的旧记录逐字节相同
 * （设备 IP 与 OMR 前缀跨重启不变），所以顶多算一次 Updated，甚至一个事件都不
 * 产生。实测 7 次重启只有 4 次产生事件，且全是 Updated。
 *
 * 而 matter-server 对这两类事件的处理是（8.1.0，device_controller.py）：
 *   - Updated + node.available → 函数第一行直接 return
 *   - operational node 的 Removed → 分发层就 return（"由订阅逻辑处理"）
 *   - Added → 才会走到真正有用的分支
 * 所以必须制造一次真正的 **Added**。
 *
 * zeroconf 0.147.0 收到 TTL=0 的 PTR 时（_handlers/record_manager.py）会把该
 * 记录立即从缓存移除并回调 Removed；之后再收到同名 PTR 就是全新记录 → Added。
 * 因此：先 mdns_service_remove_for_host()（内部发 TTL=0 的 PTR bye），
 * 隔一段时间再 mdns_service_add_for_host() 重建。
 *
 * ⚠️ 中间的间隔不能为 0：两个动作必须是两个独立的 UDP 包、并且让对端把移除
 * 处理完（matter-server 自己还有 0.5s 的事件去抖）。默认 2000ms。这段时间内
 * 设备在局域网上不可发现，代价可接受——相对于 600s。
 *
 * ⚠️ 本机制只能让"控制器收到 Added"，收到之后要不要重建订阅是控制器的事。
 * 官方版 matter-server 在已有订阅时只会调 trigger_resubscribe_if_scheduled
 * （没排重订阅计划时是空操作），所以仍需自建镜像的 resubscribe 补丁配合，
 * 见 tools/matter-server-patch/。本机制替掉的是外部的轮询式 watchdog 进程
 * （tools/gw_matter_watchdog.py，已于 2026-08-11 删除，需要时从 34f069e 取回）。
 *
 * ── 触发时机 ─────────────────────────────────────────────────────────
 *
 * 每 POLL_MS 扫一次 SRP server 的注册表：某个服务**从无到有**时重新宣告一次。
 * 开机后子设备陆续归队（休眠设备实测 4-6 分钟），每个归队的都会被处理到；
 * 租约续期不会让条目消失，所以稳态下不会反复折腾。
 */

#include "srp_readvertise.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"

static const char *TAG = "srp_readv";

#if defined(CONFIG_OPENTHREAD_ENABLED) && defined(CONFIG_BITENGINE_SRP_READVERTISE)

#include "thread_br.h"
#include "watchdog.h"   /* wd_register / wd_kick：栈余量可见性 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"   /* 闸门用的互斥量；别依赖 IDF 的传递包含 */
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_system.h"        /* esp_register_shutdown_handler()；同 semphr.h，别靠传递包含 */
#include "esp_timer.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "openthread/instance.h"
#include "openthread/srp_server.h"
#include "mdns.h"

/* ⚠️ **跟着 CONFIG_MDNS_MAX_SERVICES 走，别写死。**
   这两个数必须同量级：mdns 那个是真正的硬限（减去网关自用 4 条就是可代播余量），
   本表比它小的话，服务一多就会 dropped++ 并吼一句"表满"——而那句话在功能上
   通常没后果（FORCE_BYE 出厂关着，readvertise_one 什么都不发），纯属吓人。
   历史：12（按个位数设备定的，第 13 台起就满）→ 64 → 跟随配置。
   单条 170 字节、整块在 PSRAM（见下方分配点注释），内部 SRAM 零成本，
   所以跟随不用跟内部 SRAM 讨价还价。
   ⚠️ 曾经这里和下面 MISS_WARN_ROUNDS 的注释各写死过一个"当前 N"，
   两处都腐烂成了错的（写着 32，实际先是 64）。所以现在一个数字都不写。 */
#define MAX_SERVICES     CONFIG_MDNS_MAX_SERVICES
#define MAX_SUBTYPES      4      /* Matter 只用 1 个（_I<compressed-fabric-id>） */
#define MAX_TXT_ITEMS    16
#define MAX_TXT_BYTES   256      /* Matter operational TXT 实测 ~30B（SII/SAI/SAT/T） */
#define LABEL_LEN        64      /* DNS 标签上限 63 + NUL */
#define TYPE_LEN         24      /* "_matter" 这一截 */
#define PROTO_LEN         8      /* "_tcp" / "_udp" */

#define POLL_MS        5000
#define BYE_GAP_MS     CONFIG_BITENGINE_SRP_READV_BYE_GAP_MS

/* ⚠️ goodbye 会被静默丢弃的窗口 —— 这两个常数是本功能成败所系。
 *
 * mdns_priv_pcb_send_bye_service()（mdns_pcb.c:437）只在
 * s_pcbs[if][proto].state == PCB_RUNNING 时才真的发包。而每当有服务被加进来，
 * 整个 PCB 会退回 PROBE_1 并依次走 PROBE_1..3 → ANNOUNCE_1..3 → RUNNING。
 * 2026-08-10 在板抓包实测这段窗口 ≈ 3.2s（探测 3 包间隔 250-300ms，
 * 通告 3 包跨 2.2s）。落在窗口里发的 bye 不会有任何错误返回，就是没了——
 * 对端于是只看到一次内容相同的 Updated，matter-server 直接丢弃，等于白做。
 *
 * 两处都得躲：
 *   SETTLE_ROUNDS —— 代播服务刚被 proxy 发布时（开机后子设备归队那一刻）。
 *                    要求连续见到 N 轮再动手，N*POLL_MS 必须 > 3.2s。
 *   SETTLE_MS     —— 我们自己 add 之后 PCB 同样在探测。连着处理第二个服务时
 *                    必须等够，否则第 2 个及以后的 bye 全被吞（单设备测不出来）。
 */
#define SETTLE_ROUNDS     2      /* 2 * 5s = 10s，实测窗口 3.2s，留足余量 */
#define SETTLE_MS      4000
#define ADD_RETRY         8      /* 删掉了却加不回来 = 设备失联，往死里重试 */
#define ADD_RETRY_MS   1000
#define OT_LOCK_MS     2000

/* 一个代播服务的完整快照。取 OT 锁时填，出锁后才用（mdns 调用不能持 OT 锁）。 */
typedef struct {
    char     instance[LABEL_LEN];
    char     type[TYPE_LEN];
    char     proto[PROTO_LEN];
    char     host[LABEL_LEN];
    uint16_t port;
    uint8_t  txt[MAX_TXT_BYTES];
    uint16_t txt_len;
    char     subtype[MAX_SUBTYPES][TYPE_LEN + 8];
    uint8_t  subtype_count;
} srp_svc_t;

/* 跟踪表：记录哪些服务已经宣告过。条目在服务从 SRP 注册表消失时回收，
   于是设备下次重新注册（= 重新入网）会再触发一次。 */
typedef struct {
    char instance[LABEL_LEN];
    char type[TYPE_LEN];
    char proto[PROTO_LEN];
    char host[LABEL_LEN];   /* 重启前发 bye 要用；宣告成功那一刻记下来 */
    bool used;
    bool present;   /* 本轮扫描在 SRP 表里见到了 */
    bool done;      /* 已经重新宣告过 */
    uint16_t misses;/* 在 SRP 里有、在 mdns 里却查不到的连续轮数 */
    uint16_t rounds;/* 连续见到的轮数，用于等 mdns 那边安顿下来 */
    bool warned;    /* 上面那件事已经吼过一次了 */
} track_t;

/* 连续多少轮查不到就告警。CONFIG_MDNS_MAX_SERVICES 是硬上限，而网关自己就占了
   _zigbee-bridge/_zigbee-coordinator/_http 加上 BR 库发布的 _meshcop 四个，所以**可代播余量 = 该配置 − 4**
   （别在注释里写死数字，这里历史上写死过两次、两次都腐烂成错的）。
   2026-09-15 实测确认了这条算术：配置 64 时墙在 60，配置 128 时墙在 124。
   代播服务一多 proxy 就会静默发布失败
   ——不吼出来就只能靠抓包才发现。
   ⚠️ 撞到上限的后果比"少一个服务"重得多：mdns 返 ESP_ERR_NO_MEM → BR 库转成
   OT_ERROR_NO_BUFS → SRP Server 走错误分支 **aHost.Free() 丢掉整台主机** +
   回 SERVFAIL，而 SRP client 被拒后最长退避 1 小时。 */
#define MISS_WARN_ROUNDS 12

/* 这几块的大小随 MAX_SERVICES 走（单条 track_t 约 170 字节；配置 128 时约 24KB，
   64 时约 12.2KB —— 这里给的是换算方式而不是一个会腐烂的定值）。
   全部在 PSRAM，内部 SRAM 零占用 —— 这正是当初把它们从静态
   数组改成 PSRAM 分配的价值：抬上限不用再跟内部 SRAM 讨价还价。
   曾经是静态数组，但板上内部 SRAM 一度只剩 13.9KB（见 7103b6b：内部 SRAM/DMA
   耗尽会让 WiFi 缓冲区每秒失败 4 次），不能占。改为启动时一次性从 PSRAM 取，之后不再释放
   ——只分配一次、活到进程结束，不产生碎片，所以"禁 malloc"那条规矩的初衷仍然
   满足。任务本身 5s 才跑一轮，PSRAM 的访问延迟无关紧要。 */
#define SCRATCH_LEN  (MAX_TXT_BYTES + MAX_TXT_ITEMS + 2)

static track_t     *s_track;            /* MAX_SERVICES 条 */
/* 跟踪表的哈希索引（2026-10-07）。扫描时每个 SRP 服务要在跟踪表里查一次，原来是线性找 —— 每轮 O(N²)、全程持 OT 锁：
   123 个服务实测 7.4 ms，代播上限抬到几百条就是每 5 s 上百毫秒地卡住 OT 任务（收发、MLE、SRP 本身）。
   开放寻址、槽数 = 2 × MAX_SERVICES（装填率 ≤ 1/2），值 = s_track 下标 + 1，0 = 空。回收条目（used=false）时不删索引项：
   查找照样比对名字，过期项只占位；每轮扫描开头整体重建一次，过期项随之清掉；两次重建之间插入攒到一半就提前重建，
   探测链因此有界。整块在 PSRAM（同下方分配点）。 */
#define IDX_SLOTS (2 * MAX_SERVICES)
static uint16_t    *s_idx;
static int          s_idx_n;            /* 自上次重建以来插入的项数（含过期项） */
static int          s_free_hint;        /* 下标小于它的槽都在用：加条目时从这里往后找空槽，摊下来 O(1) */
static srp_svc_t   *s_svc;              /* 当前处理的服务 */
static srp_svc_t   *s_orphan;           /* 已 bye 但没加回去的，必须补救 */
static bool         s_orphan_valid;

static mdns_txt_item_t *s_txt_items;    /* MAX_TXT_ITEMS 条 */
static char            *s_txt_scratch;  /* SCRATCH_LEN 字节 */

static TaskHandle_t s_task;
static volatile bool s_task_run;
static volatile bool s_force;

/* OT 区间闸门。srp_readv_stop() 之后 thread_br_stop() 马上就会
   esp_openthread_deinit()，把 OT 锁本身删掉；如果本任务此刻正阻塞在
   esp_openthread_lock_acquire() 上就是 use-after-free。所以所有会取 OT 锁的
   代码都在这道闸门里跑，stop() 必须拿到闸门才返回——拿到就说明任务不在区间内，
   而"任务是否还该进入区间"由闸门内的 s_task_run 复检保证。
   OT 区间里绝不 delay，所以这个等待是有界的（实测远小于 1ms）。 */
static SemaphoreHandle_t s_ot_gate;

static bool ot_section_enter(void)
{
    if (!s_ot_gate) return false;
    if (xSemaphoreTake(s_ot_gate, portMAX_DELAY) != pdTRUE) return false;
    if (!s_task_run) {          /* 复检必须在闸门内 */
        xSemaphoreGive(s_ot_gate);
        return false;
    }
    return true;
}

static void ot_section_exit(void)
{
    xSemaphoreGive(s_ot_gate);
}

static uint32_t s_stat_readv;
/* 每轮扫描**持 OT 锁**的时长（微秒）与本轮见到的服务数（2026-10-07 起）。扫描是"每个 SRP 服务在跟踪表里查一次"，
   代播条数抬到几百条以后这一段会变长，而它卡着的是整个 OT 任务（收发、MLE、SRP 本身）—— 所以单独量、单独报，
   GET /api/v1/thread/readvertise 的 scan_lock_us / scan_lock_max_us / scan_services。 */
static uint32_t s_stat_scan_lock_us;
static uint32_t s_stat_scan_lock_max_us;
static uint32_t s_stat_scan_services;
static uint32_t s_stat_fail;
#if CONFIG_BITENGINE_SRP_READV_FORCE_BYE
static int64_t  s_last_add_us;   /* 上次我们自己往 mdns 加服务的时刻 */
#endif

/* ─────────────────── 名字解析 ─────────────────── */

/* "920E02140E0077C1.default.service.arpa." → "920E02140E0077C1" */
static bool first_label(const char *full, char *out, size_t out_len)
{
    if (!full || !out || out_len == 0) return false;
    const char *dot = strchr(full, '.');
    size_t n = dot ? (size_t)(dot - full) : strlen(full);
    if (n == 0 || n >= out_len) return false;
    memcpy(out, full, n);
    out[n] = '\0';
    return true;
}

/* "_matter._tcp.default.service.arpa." → type="_matter", proto="_tcp" */
static bool split_service_name(const char *full, char *type, size_t tl,
                               char *proto, size_t pl)
{
    if (!first_label(full, type, tl)) return false;
    const char *dot = strchr(full, '.');
    if (!dot) return false;
    if (!first_label(dot + 1, proto, pl)) return false;
    /* proto 必须是 _tcp / _udp，否则说明名字不是预期结构，宁可跳过 */
    if (strcmp(proto, "_tcp") != 0 && strcmp(proto, "_udp") != 0) return false;
    return true;
}

/* DNS-SD TXT 线格式（<len><key=value> 串接）→ mdns_txt_item_t[]。
   scratch 里放 "key\0value\0"，items 指进去。返回条目数。 */
static int parse_txt(const uint8_t *data, uint16_t len,
                     mdns_txt_item_t *items, int max_items,
                     char *scratch, size_t scratch_len)
{
    int n = 0;
    size_t w = 0;
    uint16_t i = 0;

    while (i < len && n < max_items) {
        uint8_t elen = data[i++];
        if (elen == 0) continue;                 /* 空串，跳过 */
        if ((size_t)(i + elen) > len) break;      /* 越界的 TXT：丢掉剩余部分 */
        if (w + elen + 2 > scratch_len) break;    /* scratch 满 */

        char *key = scratch + w;
        memcpy(key, data + i, elen);
        key[elen] = '\0';
        w += elen + 1;
        i += elen;

        char *eq = strchr(key, '=');
        if (eq) {
            *eq = '\0';
            items[n].key   = key;
            items[n].value = eq + 1;
        } else {
            /* 无值条目（DNS-SD 允许）。mdns 组件只接受字符串，用空值代替，
               线上会变成 "key=" —— 与原始的 "key" 有一字节差异，Matter 的
               TXT 全部是 key=value 形式，实际不会走到这里。 */
            items[n].key   = key;
            items[n].value = "";
        }
        n++;
    }
    if (i < len) {
        ESP_LOGW(TAG, "TXT truncated while parsing (%u/%u bytes used)", i, len);
    }
    return n;
}

/* ─────────────────── SRP 注册表访问（必须持 OT 锁）─────────────────── */

static uint32_t track_hash(const char *instance, const char *type, const char *proto)
{
    /* FNV-1a，三段之间夹一个分隔字节（否则 "ab"+"c" 与 "a"+"bc" 同哈希） */
    const char *part[3] = { instance, type, proto };
    uint32_t h = 2166136261u;
    for (int k = 0; k < 3; k++) {
        for (const char *p = part[k]; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
        h = (h ^ 0x1Fu) * 16777619u;
    }
    return h;
}

static void idx_insert(int i)
{
    uint32_t k = track_hash(s_track[i].instance, s_track[i].type, s_track[i].proto) % IDX_SLOTS;
    while (s_idx[k]) k = (k + 1) % IDX_SLOTS;          /* 装填率 ≤ 1/2，必有空位 */
    s_idx[k] = (uint16_t)(i + 1);
    s_idx_n++;
}

static void idx_rebuild(void)
{
    memset(s_idx, 0, IDX_SLOTS * sizeof(*s_idx));
    s_idx_n = 0;
    for (int i = 0; i < MAX_SERVICES; i++) {
        if (s_track[i].used) idx_insert(i);
    }
}

static track_t *track_find(const char *instance, const char *type, const char *proto)
{
    uint32_t k = track_hash(instance, type, proto) % IDX_SLOTS;
    for (int n = 0; n < IDX_SLOTS && s_idx[k]; n++, k = (k + 1) % IDX_SLOTS) {
        track_t *t = &s_track[s_idx[k] - 1];
        if (t->used &&
            strcmp(t->instance, instance) == 0 &&
            strcmp(t->type, type) == 0 &&
            strcmp(t->proto, proto) == 0) {
            return t;
        }
    }
    return NULL;
}

static track_t *track_add(const char *instance, const char *type, const char *proto)
{
    /* 从提示处往后找；找不到再从头兜一圈（谁绕过 track_release 直接改了 used，提示就不准了 —— 主机测试就这么干） */
    for (int n = 0; n < MAX_SERVICES; n++) {
        int i = (s_free_hint + n) % MAX_SERVICES;
        if (!s_track[i].used) {
            memset(&s_track[i], 0, sizeof(s_track[i]));
            snprintf(s_track[i].instance, LABEL_LEN, "%s", instance);
            snprintf(s_track[i].type, TYPE_LEN, "%s", type);
            snprintf(s_track[i].proto, PROTO_LEN, "%s", proto);
            s_track[i].used = true;
            s_free_hint = (i + 1) % MAX_SERVICES;
            if (s_idx_n >= MAX_SERVICES) idx_rebuild();   /* 过期项攒多了：先清，保证装填率 ≤ 1/2 */
            else                         idx_insert(i);
            return &s_track[i];
        }
    }
    return NULL;
}

/* 回收一条：标空、把空槽提示往前挪（索引里的过期项留到下次重建） */
static void track_release(int i)
{
    s_track[i].used = false;
    if (i < s_free_hint) s_free_hint = i;
}

/* 扫一遍 SRP 注册表，同步跟踪表的 present 标志、登记新出现的服务。
   只碰内存，不调 mdns，可以安全地在 OT 锁内做。 */
static void scan_registry(void)
{
    for (int i = 0; i < MAX_SERVICES; i++) s_track[i].present = false;
    idx_rebuild();          /* 在取 OT 锁之前做：O(N)，只碰自己的内存 */

    if (!ot_section_enter()) return;
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(OT_LOCK_MS))) {
        ESP_LOGW(TAG, "OT lock busy — skip this round");
        ot_section_exit();
        return;
    }
    int64_t t0 = esp_timer_get_time();
    otInstance *inst = esp_openthread_get_instance();
    int dropped = 0;
    uint32_t seen = 0;

    if (inst) {
        const otSrpServerHost *host = NULL;
        while ((host = otSrpServerGetNextHost(inst, host)) != NULL) {
            if (otSrpServerHostIsDeleted(host)) continue;

            const otSrpServerService *svc = NULL;
            while ((svc = otSrpServerHostGetNextService(host, svc)) != NULL) {
                if (otSrpServerServiceIsDeleted(svc)) continue;

                char instance[LABEL_LEN], type[TYPE_LEN], proto[PROTO_LEN];
                const char *label = otSrpServerServiceGetInstanceLabel(svc);
                if (!label || snprintf(instance, LABEL_LEN, "%s", label) >= LABEL_LEN) continue;
                if (!split_service_name(otSrpServerServiceGetServiceName(svc),
                                        type, TYPE_LEN, proto, PROTO_LEN)) continue;

                seen++;
                track_t *e = track_find(instance, type, proto);
                if (!e) e = track_add(instance, type, proto);
                if (e) e->present = true;
                else   dropped++;
            }
        }
    }
    uint32_t held_us = (uint32_t)(esp_timer_get_time() - t0);
    esp_openthread_lock_release();
    ot_section_exit();
    s_stat_scan_lock_us = held_us;
    if (held_us > s_stat_scan_lock_max_us) s_stat_scan_lock_max_us = held_us;
    s_stat_scan_services = seen;

    /* 去抖：表满是稳态而不是事件，原来每轮（POLL_MS=5s）打一行 —— 一天 17280 行，
       会把 log_store 的 600 槽环形缓冲冲成单一噪声，/api/v1/logs 从此没有诊断
       价值，串口同理。只在"开始满"和"不再满"两个边沿各打一次。 */
    static bool s_full_warned;
    if (dropped && !s_full_warned) {
        s_full_warned = true;
        ESP_LOGW(TAG, "tracking table full (%d) — %d proxied service(s) will NOT be "
                      "re-advertised; raise MAX_SERVICES", MAX_SERVICES, dropped);
    } else if (!dropped && s_full_warned) {
        s_full_warned = false;
        ESP_LOGI(TAG, "tracking table no longer full");
    }

    /* 本轮没见到的条目回收，让设备下次重新注册时能再触发一次 */
    for (int i = 0; i < MAX_SERVICES; i++) {
        if (s_track[i].used && !s_track[i].present) track_release(i);
    }
}

/* 把某个服务的完整参数抓进 out。持 OT 锁。 */
static bool snapshot(const track_t *key, srp_svc_t *out)
{
    bool found = false;

    if (!ot_section_enter()) return false;
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(OT_LOCK_MS))) {
        ot_section_exit();
        return false;
    }
    otInstance *inst = esp_openthread_get_instance();

    const otSrpServerHost *host = NULL;
    while (inst && !found && (host = otSrpServerGetNextHost(inst, host)) != NULL) {
        if (otSrpServerHostIsDeleted(host)) continue;

        const otSrpServerService *svc = NULL;
        while ((svc = otSrpServerHostGetNextService(host, svc)) != NULL) {
            if (otSrpServerServiceIsDeleted(svc)) continue;

            memset(out, 0, sizeof(*out));
            const char *label = otSrpServerServiceGetInstanceLabel(svc);
            if (!label || snprintf(out->instance, LABEL_LEN, "%s", label) >= LABEL_LEN) continue;
            if (!split_service_name(otSrpServerServiceGetServiceName(svc),
                                    out->type, TYPE_LEN, out->proto, PROTO_LEN)) continue;
            if (strcmp(out->instance, key->instance) != 0 ||
                strcmp(out->type, key->type) != 0 ||
                strcmp(out->proto, key->proto) != 0) continue;

            if (!first_label(otSrpServerHostGetFullName(host), out->host, LABEL_LEN)) continue;
            out->port = otSrpServerServiceGetPort(svc);

            uint16_t txt_len = 0;
            const uint8_t *txt = otSrpServerServiceGetTxtData(svc, &txt_len);
            if (txt && txt_len) {
                if (txt_len > MAX_TXT_BYTES) {
                    ESP_LOGW(TAG, "%s: TXT %u B > %d B cap — truncated",
                             out->instance, txt_len, MAX_TXT_BYTES);
                    txt_len = MAX_TXT_BYTES;
                }
                memcpy(out->txt, txt, txt_len);
                out->txt_len = txt_len;
            }

            uint16_t nsub = otSrpServerServiceGetNumberOfSubTypes(svc);
            for (uint16_t i = 0; i < nsub && out->subtype_count < MAX_SUBTYPES; i++) {
                const char *sub = otSrpServerServiceGetSubTypeServiceNameAt(svc, i);
                char lbl[TYPE_LEN + 8];
                if (sub && otSrpServerParseSubTypeServiceName(sub, lbl, sizeof(lbl)) == OT_ERROR_NONE) {
                    snprintf(out->subtype[out->subtype_count], sizeof(out->subtype[0]), "%s", lbl);
                    out->subtype_count++;
                }
            }
            if (nsub > MAX_SUBTYPES) {
                ESP_LOGW(TAG, "%s: %u subtypes > %d cap — extras dropped",
                         out->instance, nsub, MAX_SUBTYPES);
            }
            found = true;
            break;
        }
    }
    esp_openthread_lock_release();
    ot_section_exit();
    return found;
}

/* ─────────────────── mDNS 侧动作（绝不持 OT 锁）─────────────────── */

/* 只做"加回去"这一半。orphan 补救和正常路径共用。 */
static esp_err_t republish(const srp_svc_t *s)
{
    int n = parse_txt(s->txt, s->txt_len, s_txt_items, MAX_TXT_ITEMS,
                      s_txt_scratch, SCRATCH_LEN);

    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < ADD_RETRY; attempt++) {
        err = mdns_service_add_for_host(s->instance, s->type, s->proto,
                                        s->host, s->port, s_txt_items, n);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "%s.%s.%s: add attempt %d/%d failed: %s",
                 s->instance, s->type, s->proto, attempt + 1, ADD_RETRY,
                 esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(ADD_RETRY_MS));
    }
    if (err != ESP_OK) return err;

    for (int i = 0; i < s->subtype_count; i++) {
        esp_err_t serr = mdns_service_subtype_add_for_host(s->instance, s->type, s->proto,
                                                           s->host, s->subtype[i]);
        if (serr != ESP_OK) {
            /* 子类型丢了不影响 matter-server（它浏览的是 _matter._tcp 本体），
               但按子类型发现的控制器会看不到，所以要吼一声。 */
            ESP_LOGW(TAG, "%s: subtype %s add failed: %s",
                     s->instance, s->subtype[i], esp_err_to_name(serr));
        }
    }
    return ESP_OK;
}

static esp_err_t readvertise_one(const srp_svc_t *s)
{
#if !CONFIG_BITENGINE_SRP_READV_FORCE_BYE
    /* 默认路径：只跟踪、不动记录。撤销+重新宣告的收益只对 python matter-server
       成立（见文件头），而代价（BYE_GAP_MS 内设备不可发现）对谁都存在。
       返回 OK 让跟踪表标 done，否则每轮都会重试同一个服务。 */
    (void)s;
    return ESP_OK;
#else
    if (!mdns_service_exists_with_instance(s->instance, s->type, s->proto, s->host)) {
        /* proxy 还没把它发布到 mdns，或者刚被撤掉。下一轮再说。 */
        ESP_LOGD(TAG, "%s.%s.%s not in mdns yet — retry next round",
                 s->instance, s->type, s->proto);
        return ESP_ERR_NOT_FOUND;
    }

    /* ❶ 先等 mdns 那边安顿：PCB 只要还在探测/通告，bye 就会被静默丢弃 */
    int64_t since_ms = (esp_timer_get_time() - s_last_add_us) / 1000;
    if (s_last_add_us && since_ms < SETTLE_MS) {
        vTaskDelay(pdMS_TO_TICKS(SETTLE_MS - since_ms));
    }

    /* ❷ goodbye：内部发 TTL=0 的 PTR，对端 zeroconf 会立即从缓存移除 */
    esp_err_t err = mdns_service_remove_for_host(s->instance, s->type, s->proto, s->host);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: remove failed: %s", s->instance, esp_err_to_name(err));
        return err;
    }

    /* ❸ 间隔：必须让 bye 和重新宣告成为两个独立事件（见文件头） */
    vTaskDelay(pdMS_TO_TICKS(BYE_GAP_MS));

    /* ❹ 原样加回去 —— 从这里开始，失败就等于设备失联，必须补回来 */
    err = republish(s);
    s_last_add_us = esp_timer_get_time();   /* 不管成没成，PCB 都已被打回探测 */
    if (err != ESP_OK) {
        memcpy(s_orphan, s, sizeof(*s_orphan));
        s_orphan_valid = true;
        ESP_LOGE(TAG, "%s.%s.%s: removed but re-add failed (%s) — device is now "
                      "UNDISCOVERABLE, will keep retrying every %d ms",
                 s->instance, s->type, s->proto, esp_err_to_name(err), POLL_MS);
        return err;
    }

    ESP_LOGI(TAG, "re-advertised %s.%s.%s host=%s port=%u txt=%uB subtypes=%u",
             s->instance, s->type, s->proto, s->host, s->port,
             s->txt_len, s->subtype_count);
    return ESP_OK;
#endif /* CONFIG_BITENGINE_SRP_READV_FORCE_BYE */
}

/* ─────────────────── 任务 ─────────────────── */

static void readv_task(void *arg)
{
    (void)arg;
#if CONFIG_BITENGINE_SRP_READV_FORCE_BYE
    ESP_LOGI(TAG, "started (poll=%dms, force-bye ON, gap=%dms)", POLL_MS, BYE_GAP_MS);
#else
    ESP_LOGI(TAG, "started (poll=%dms, force-bye OFF — 只跟踪不动记录)", POLL_MS);
#endif

    /* 注册进看门狗登记表：本任务用的是**内部** SRAM 栈（4096，psram_task.h 规则 2
       挡着不能迁——它会自删），而它恰恰是工作量随 Thread 设备数增长的那类。
       不注册的话 stack_left 看不到，栈溢出只会以"随机崩溃"出现且无法归因。
       severity 取 observe：它停转不影响设备可达性（FORCE_BYE 默认关时本就不动记录）。*/
    static const wd_task_spec_t readv_spec = {
        .name = "srp_readv", .deadline_ms = 60000, .severity = WD_SEV_OBSERVE,
    };
    int wd_slot = wd_register(&readv_spec);

    while (s_task_run) {
        wd_kick(wd_slot);          /* 顺带自采栈余量，见 wd_registry.c 的注释 */
        /* 用通知等待而不是 vTaskDelay：stop() 一来立刻醒，不用干等一整轮 */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLL_MS));
        if (!s_task_run) break;
        if (!thread_br_is_running()) continue;

        /* 上一轮删掉却没加回去的，优先补救 */
        if (s_orphan_valid) {
            if (republish(s_orphan) == ESP_OK) {
                s_orphan_valid = false;
                ESP_LOGW(TAG, "%s: recovered after failed re-add", s_orphan->instance);
            } else {
                continue;   /* 还没救回来就别再动别的服务 */
            }
        }

        if (s_force) {
            s_force = false;
            for (int i = 0; i < MAX_SERVICES; i++) s_track[i].done = false;
            ESP_LOGI(TAG, "manual trigger — re-advertising everything");
        }

        scan_registry();

        for (int i = 0; i < MAX_SERVICES && s_task_run; i++) {
            if (!s_track[i].used || !s_track[i].present || s_track[i].done) continue;

            /* 刚出现的服务先晾几轮：proxy 把它加进 mdns 时 PCB 正在探测，
               这时候发 bye 等于没发（见 SETTLE_ROUNDS）。 */
            if (s_track[i].rounds < SETTLE_ROUNDS) {
                s_track[i].rounds++;
                continue;
            }

            if (!snapshot(&s_track[i], s_svc)) {
                ESP_LOGW(TAG, "%s: vanished from SRP registry before snapshot",
                         s_track[i].instance);
                continue;
            }
            esp_err_t err = readvertise_one(s_svc);
            if (err == ESP_OK) {
                s_track[i].done = true;
                /* 留住 host —— 重启前的 goodbye 拿不到 SRP 注册表（那要 OT 锁，
                   而 shutdown handler 里不能等锁），只能靠这份快照 */
                snprintf(s_track[i].host, LABEL_LEN, "%s", s_svc->host);
#if CONFIG_BITENGINE_SRP_READV_FORCE_BYE
                /* 只在真的重新宣告过时才计数。开关关掉时 readvertise_one() 直接
                   返回 OK（只为让跟踪表标 done、不再每轮重试），那时什么都没发出去
                   ——照加的话 /api/v1/thread/readvertise 就会报一个"宣告了 2 个"
                   的假数字，排查的人会被它带偏。 */
                s_stat_readv++;
#endif
            } else if (err == ESP_ERR_NOT_FOUND) {
                /* SRP 里有、mdns 里没有。刚注册上、proxy 还没发布是正常的，
                   一直这样就不正常了（多半是 mdns 服务表满）。 */
                if (++s_track[i].misses >= MISS_WARN_ROUNDS && !s_track[i].warned) {
                    s_track[i].warned = true;
                    ESP_LOGW(TAG, "%s.%s.%s registered in SRP but never showed up in "
                                  "mDNS after %d rounds — advertising proxy likely "
                                  "failed to publish it (CONFIG_MDNS_MAX_SERVICES=%d)",
                             s_track[i].instance, s_track[i].type, s_track[i].proto,
                             MISS_WARN_ROUNDS, CONFIG_MDNS_MAX_SERVICES);
                }
            } else {
                s_stat_fail++;
                s_track[i].done = true;   /* 别在同一个服务上死循环 */
            }
            if (s_orphan_valid) break;    /* 出事了，先补救再说 */
        }
    }

    ESP_LOGI(TAG, "stopped");
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ── 重启前的 goodbye ─────────────────────────────────────────────────
 *
 * 开机后的 bye+重新宣告（本文件主体）解决的是"控制器收不到 Added"。它解决不了
 * 另一半：**重启期间控制器压根没察觉出过事**。
 *
 * 2026-08-15 实测：网关重启后 srp_readv 在 +49s 就把门窗传感器的记录重新宣告了，
 * 但 matter-server 自始至终没把该节点标 offline（日志里没有任何 offline 记录），
 * 于是握着死订阅继续等，直到 6 分钟后用户按了设备按键、设备自己发起 CASE 才恢复。
 * 对照组那台常供电设备 2 分钟就自己回来了——**恢复是设备驱动的**。
 *
 * 所以要在重启**之前**把记录撤掉：对端 zeroconf 收到 TTL=0 的 PTR 会立即从缓存
 * 移除，控制器当场知道节点不可达、丢掉死订阅；网关起来后的重新宣告才是一次货真
 * 价实的 Added，控制器会主动发起 CASE。而休眠子设备每 1-2 秒就向父节点轮询一次
 * （实测 age 常年 1-4s），握手能在秒级完成——它并非"联系不上"，只是没人找它。
 *
 * 挂 shutdown handler 而不是在每个重启点各调一次：esp_restart() 的调用点有十来
 * 处（按键、WebUI、两条 OTA、恢复出厂、看门狗自愈…），漏一个就是一次静默退化。
 *
 * ⚠️ 只发 bye、不重新加回来 —— 马上就要重启了，加回来毫无意义，反而会让 PCB
 * 重新进入探测期、把 bye 吞掉。 */
#define BYE_FLUSH_MS  300     /* 让 mdns 任务把 TTL=0 的包真正发出去 */


void srp_readv_bye_all(void)
{
#if !CONFIG_BITENGINE_SRP_READV_FORCE_BYE
    /* 默认关。清掉对端缓存恰好落在最脆弱的时刻：网关正要下电，而那份缓存正是
       扛过"重启到设备重新向 SRP 注册"那段空窗的东西（实测 21-35s，远短于记录的
       120s TTL，本来完全无感）。收益只对 python matter-server 成立。 */
    return;
#else
    if (!s_track) return;     /* 从没启动过 */

    int sent = 0;
    for (int i = 0; i < MAX_SERVICES; i++) {
        /* 只撤我们确实宣告成功过的：host 是那一刻记下的快照 */
        if (!s_track[i].used || !s_track[i].done || s_track[i].host[0] == '\0') continue;
        if (mdns_service_remove_for_host(s_track[i].instance, s_track[i].type,
                                         s_track[i].proto, s_track[i].host) == ESP_OK) {
            sent++;
        }
    }
    if (sent == 0) return;

    /* 不能立刻返回：mdns 的删除是投递给 mdns 任务去做的，这里必须让出 CPU 让它
       把包发出去。shutdown handler 跑在调用方任务上下文、调度器还活着，所以
       vTaskDelay 是安全的，而且比忙等更好——忙等反而饿死 mdns 任务。 */
    vTaskDelay(pdMS_TO_TICKS(BYE_FLUSH_MS));
    ESP_LOGW(TAG, "sent goodbye for %d delegated service(s) before reboot", sent);
#endif /* CONFIG_BITENGINE_SRP_READV_FORCE_BYE */
}

esp_err_t srp_readv_start(void)
{
    if (s_task) {
        if (s_task_run) return ESP_OK;
        /* 上一次 stop() 的任务还没退干净，等它把 s_task 清掉再重建，
           否则这里直接 return 会留下"以为在跑、其实马上要死"的空档。 */
        for (int i = 0; i < 100 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(10));
        if (s_task) {
            ESP_LOGE(TAG, "previous task did not exit — not restarting");
            return ESP_ERR_INVALID_STATE;
        }
    }

    /* 覆盖所有 esp_restart() 路径，不用每个调用点各加一次。重复注册返回
       INVALID_STATE，忽略即可（stop→start 会走到这里第二次）。 */
    esp_err_t reg = esp_register_shutdown_handler(srp_readv_bye_all);
    if (reg != ESP_OK && reg != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "pre-reboot goodbye hook not registered (%s) — 控制器要等到"
                 "订阅超时才会发现网关重启过", esp_err_to_name(reg));
    }

    if (!s_ot_gate) {
        s_ot_gate = xSemaphoreCreateMutex();   /* 只创建一次，随进程存活 */
        if (!s_ot_gate) {
            ESP_LOGE(TAG, "gate create failed");
            return ESP_ERR_NO_MEM;
        }
    }

    /* 缓冲区优先落 PSRAM（见声明处）。只在第一次 start 时分配，之后复用。
       PSRAM 拿不到就退回默认堆——功能比省内存重要，但要吼一声。 */
    {
        /* 逐块判空：原来整段只看 s_track，上一次分配到一半失败的话，下次 start 会跳过、其余缓冲一直是 NULL */
        struct { void **p; size_t n; const char *name; } bufs[] = {
            { (void **)&s_track,       MAX_SERVICES * sizeof(track_t),        "track"   },
            { (void **)&s_idx,         IDX_SLOTS * sizeof(uint16_t),          "index"   },
            { (void **)&s_svc,         sizeof(srp_svc_t),                     "svc"     },
            { (void **)&s_orphan,      sizeof(srp_svc_t),                     "orphan"  },
            { (void **)&s_txt_items,   MAX_TXT_ITEMS * sizeof(mdns_txt_item_t), "txt"   },
            { (void **)&s_txt_scratch, SCRATCH_LEN,                           "scratch" },
        };
        for (size_t i = 0; i < sizeof(bufs) / sizeof(bufs[0]); i++) {
            if (*bufs[i].p) continue;
            *bufs[i].p = heap_caps_calloc(1, bufs[i].n, MALLOC_CAP_SPIRAM);
            if (!*bufs[i].p) {
                *bufs[i].p = calloc(1, bufs[i].n);
                if (!*bufs[i].p) {
                    ESP_LOGE(TAG, "%s buffer alloc failed (%u B)",
                             bufs[i].name, (unsigned)bufs[i].n);
                    return ESP_ERR_NO_MEM;   /* 已分配的留着，下次 start 复用 */
                }
                ESP_LOGW(TAG, "%s buffer fell back to internal RAM (%u B)",
                         bufs[i].name, (unsigned)bufs[i].n);
            }
        }
    }

    memset(s_track, 0, MAX_SERVICES * sizeof(track_t));
    memset(s_idx, 0, IDX_SLOTS * sizeof(uint16_t));
    s_idx_n = 0;
    s_free_hint = 0;
    s_orphan_valid = false;
    s_force = false;
    s_task_run = true;

    if (xTaskCreate(readv_task, "srp_readv", 4096, NULL, 4, &s_task) != pdPASS) {
        s_task_run = false;
        s_task = NULL;
        ESP_LOGE(TAG, "task create failed — Matter clients will fall back to the "
                      "600s subscription timeout after a reboot");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t srp_readv_stop(void)
{
    TaskHandle_t t = s_task;
    if (!t || !s_ot_gate) return ESP_OK;

    if (s_task_run) {
        s_task_run = false;     /* 必须先置位，再抢闸门（见 ot_section_enter） */
        xTaskNotifyGive(t);     /* 只在自己翻转标志时叫醒它：否则任务可能正在
                                   退出，句柄随时会失效 */
    }

    /* 不等任务整个退出（它可能正在 bye 之后的 2s 间隔里，甚至在重试加回服务），
       只等它离开 OT 区间——之后调用方拆 OpenThread 就是安全的。
       mdns 那半截不碰 OpenThread，让它自己跑完反而更好：那是在把服务加回去。 */
    if (xSemaphoreTake(s_ot_gate, pdMS_TO_TICKS(3000)) == pdTRUE) {
        xSemaphoreGive(s_ot_gate);
    } else {
        ESP_LOGW(TAG, "task still inside an OT section after 3s — "
                      "OpenThread teardown may race with it");
    }
    return ESP_OK;
}

esp_err_t srp_readv_trigger_all(void)
{
    if (!s_task) return ESP_ERR_INVALID_STATE;
    s_force = true;
    return ESP_OK;
}

void srp_readv_get_scan_stats(uint32_t *lock_us, uint32_t *lock_max_us, uint32_t *services)
{
    if (lock_us)     *lock_us     = s_stat_scan_lock_us;
    if (lock_max_us) *lock_max_us = s_stat_scan_lock_max_us;
    if (services)    *services    = s_stat_scan_services;
}

void srp_readv_get_stats(uint32_t *readvertised, uint32_t *failed, uint32_t *tracked)
{
    if (readvertised) *readvertised = s_stat_readv;
    if (failed)       *failed       = s_stat_fail;
    if (tracked) {
        /* s_track 要到首次 srp_readv_start() 才分配。thread_br_start() 有多条
           提前返回的失败路径（eventfd / esp_openthread_init / ot_main 建不起来），
           走到那些分支时本模块从未启动，而 GET /api/v1/thread/readvertise 是
           无条件注册的 —— 不判空就是一查询就崩。 */
        uint32_t n = 0;
        if (s_track) {
            for (int i = 0; i < MAX_SERVICES; i++) if (s_track[i].used) n++;
        }
        *tracked = n;
    }
}

#else  /* !CONFIG_OPENTHREAD_ENABLED || !CONFIG_BITENGINE_SRP_READVERTISE */

esp_err_t srp_readv_start(void)        { ESP_LOGD(TAG, "disabled at build time"); return ESP_OK; }
esp_err_t srp_readv_stop(void)         { return ESP_OK; }
esp_err_t srp_readv_trigger_all(void)  { return ESP_ERR_NOT_SUPPORTED; }
void      srp_readv_bye_all(void)      { }
void      srp_readv_get_scan_stats(uint32_t *lock_us, uint32_t *lock_max_us, uint32_t *services)
{
    if (lock_us)     *lock_us     = 0;
    if (lock_max_us) *lock_max_us = 0;
    if (services)    *services    = 0;
}
void      srp_readv_get_stats(uint32_t *readvertised, uint32_t *failed, uint32_t *tracked)
{
    if (readvertised) *readvertised = 0;
    if (failed)       *failed       = 0;
    if (tracked)      *tracked      = 0;
}

#endif
