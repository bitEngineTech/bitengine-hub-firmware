#!/usr/bin/env python3
"""给 managed_components/espressif__mdns 打补丁：「多包应答」+「服务数超过 255 时的整型溢出」（OVERFLOW_PATCHES）
+「代播服务成批进来时的 CPU、队列与包完整性」（SCALE_PATCHES）
+「本机地址只宣告以太网服务 IP」（SVCIP_PATCHES，T9.34）。

── 为什么需要 ────────────────────────────────────────────────────────────
mdns 组件把一次应答的全部记录塞进一个 MDNS_MAX_PACKET_SIZE(1460) 的静态缓冲：
mdns_send.c 的 mdns_priv_dispatch_tx_packet() 依次遍历 questions/answers/
servers/additional 四条链表，append_answer() 放不下就返回 0，**循环照跑、
ANCOUNT 只统计塞进去的那些，然后照发一包** —— 多出来的记录直接消失，
零日志、零返回码、不置 TC 位。

2026-08-17 在板实测（tools 侧脚本向 224.0.0.251 查 _matter._tcp.local）：
2 个 Matter 设备时应答 410 字节，扣掉头部+问题段 36 字节 = 187 字节/设备，
(1460-36)/187 ≈ 7.6 —— **第 8 个设备起就开始丢记录**。而 CONFIG_MDNS_MAX_SERVICES
是 32，也就是说在撞到那个上限之前很久，浏览应答就已经不完整了。
外部表现是「设备配上了、Thread 网里活得好好的，控制器里查无此人」。

── 补丁做什么 ────────────────────────────────────────────────────────────
装不下就把当前包发出去，另起一包继续 —— 每个包都是自洽的合法应答，
各自带正确的 AN/NS/AR 计数。这是 responder 侧的正确做法（RFC 6762 §6：
应答可以分多个包发；§7.2 的 TC 位是查询侧的已知答案抑制，不是这里）。

三个实现要点：
  1. 名字压缩天然安全 —— append_fqdn() 只在 packet[0..*index] 里搜（memchr），
     index 一复位就自动重新基准，不会产生跨包的悬空压缩指针。
  2. append_answer() 返回 0 有两种含义（装不下 / host 非法）。补丁的做法是
     先存 index、失败就回滚，再在**空包**上重试一次：空包还装不下才是真的
     这条有问题，跳过；否则说明只是上一个包满了。回滚同时兜住了
     append_service_ptr_answers() 那种可能已部分写入的情况。
  3. 只对组播应答分包（p->port == MDNS_SERVICE_PORT）。legacy unicast 查询
     （源端口非 5353）的应答要与查询一一对应，分包可能让老解析器困惑，
     那条路保持原行为。

── 为什么是构建期打补丁而不是 fork 整个组件 ──────────────────────────────
managed_components/ 被 .gitignore 挡在版本控制之外（.gitignore:3），改在那里
既不会被提交，一次 idf.py fullclean 也就没了。把整个组件 fork 进 components/
则意味着我们从此拥有它全部 50 来个文件、且丢掉上游更新。
构建期打补丁：diff 小且在 git 里可审、幂等、扛 fullclean、上游版本仍钉死可见。

⚠️ 本脚本用**原文匹配**而不是行号：上游一改动这段代码就会匹配失败并让构建
   停下来，而不是悄悄打歪。升级 mdns 版本时预期会在这里失败一次，那是有意的
   —— 届时请重新核对 dispatch_tx_packet 的结构再更新下面的 ANCHOR。
"""

import argparse
import pathlib
import sys

EXPECT_VERSION = "1.11.0"
MARK = "/* bitEngine: multi-packet split */"

# 判定「包满了」的余量。单条 mDNS 记录最长约 300 字节（长实例名 + TXT + 子类型），
# 400 留一倍安全边际：index 超过 (MAX-400) 之后 append 失败才可能是真的装不下。
SPLIT_HEADROOM = 400

# 原文锚点：整个 mdns_priv_dispatch_tx_packet 函数体。
ANCHOR = """void mdns_priv_dispatch_tx_packet(mdns_tx_packet_t *p)
{
    static uint8_t packet[MDNS_MAX_PACKET_SIZE];
    uint16_t index = MDNS_HEAD_LEN;
    memset(packet, 0, MDNS_HEAD_LEN);
    mdns_out_question_t *q;
    mdns_out_answer_t *a;
    uint8_t count;

    set_u16(packet, MDNS_HEAD_FLAGS_OFFSET, p->flags);
    set_u16(packet, MDNS_HEAD_ID_OFFSET, p->id);

    count = 0;
    q = p->questions;
    while (q) {
        if (append_question(packet, &index, q)) {
            count++;
        }
        q = q->next;
    }
    set_u16(packet, MDNS_HEAD_QUESTIONS_OFFSET, count);

    count = 0;
    a = p->answers;
    while (a) {
        count += append_answer(packet, &index, a, p->tcpip_if);
        a = a->next;
    }
    set_u16(packet, MDNS_HEAD_ANSWERS_OFFSET, count);

    count = 0;
    a = p->servers;
    while (a) {
        count += append_answer(packet, &index, a, p->tcpip_if);
        a = a->next;
    }
    set_u16(packet, MDNS_HEAD_SERVERS_OFFSET, count);

    count = 0;
    a = p->additional;
    while (a) {
        count += append_answer(packet, &index, a, p->tcpip_if);
        a = a->next;
    }
    set_u16(packet, MDNS_HEAD_ADDITIONAL_OFFSET, count);

    DBG_TX_PACKET(p, packet, index);

    mdns_priv_if_write(p->tcpip_if, p->ip_protocol, &p->dst, p->port, packet, index);
}"""

REPLACEMENT = """""" + MARK + """
#define BITENGINE_SPLIT_HEADROOM """ + str(SPLIT_HEADROOM) + """
/* 把当前缓冲里已攒好的记录当作一个完整应答发出去。计数是每包各自的。 */
static void bitengine_tx_flush(mdns_tx_packet_t *p, uint8_t *packet, uint16_t index,
                               uint16_t nq, uint16_t nan, uint16_t nns, uint16_t nar)
{
    set_u16(packet, MDNS_HEAD_QUESTIONS_OFFSET, nq);
    set_u16(packet, MDNS_HEAD_ANSWERS_OFFSET, nan);
    set_u16(packet, MDNS_HEAD_SERVERS_OFFSET, nns);
    set_u16(packet, MDNS_HEAD_ADDITIONAL_OFFSET, nar);
    DBG_TX_PACKET(p, packet, index);
    mdns_priv_if_write(p->tcpip_if, p->ip_protocol, &p->dst, p->port, packet, index);
}

static void bitengine_tx_reset(mdns_tx_packet_t *p, uint8_t *packet, uint16_t *index)
{
    *index = MDNS_HEAD_LEN;
    memset(packet, 0, MDNS_HEAD_LEN);
    set_u16(packet, MDNS_HEAD_FLAGS_OFFSET, p->flags);
    set_u16(packet, MDNS_HEAD_ID_OFFSET, p->id);
}

void mdns_priv_dispatch_tx_packet(mdns_tx_packet_t *p)
{
    static uint8_t packet[MDNS_MAX_PACKET_SIZE];
    uint16_t index;
    uint16_t nq = 0, nan = 0, nns = 0, nar = 0;
    mdns_out_question_t *q;
    mdns_out_answer_t *a;

    /* legacy unicast（源端口非 5353）的应答要与查询一一对应，不分包，保持原行为 */
    const bool may_split = (p->port == MDNS_SERVICE_PORT);

    bitengine_tx_reset(p, packet, &index);

    q = p->questions;
    while (q) {
        if (append_question(packet, &index, q)) {
            nq++;
        }
        q = q->next;
    }

    struct { mdns_out_answer_t *list; uint16_t *cnt; } secs[3] = {
        { p->answers,    &nan },
        { p->servers,    &nns },
        { p->additional, &nar },
    };

    for (int s = 0; s < 3; s++) {
        a = secs[s].list;
        while (a) {
            uint16_t save = index;
            uint8_t r = append_answer(packet, &index, a, p->tcpip_if);
            if (r) {
                *secs[s].cnt += r;
                a = a->next;
                continue;
            }
            /* 没进去。先回滚可能的部分写入（append_service_ptr_answers 会连写多条）。 */
            index = save;
            /* ⚠️ append_answer() 返回 0 有两种含义：装不下 / host 非法。
               只看返回值分不开，而把「host 非法」误当成「包满了」会在远未到
               上限时无谓切包 —— 2026-08-17 首版补丁就是这样在 278 字节处切了一刀
               （实测：1460 上限、2 台设备，本该一个包却发了三个）。
               所以只有**确实接近上限**时才当作满。单条记录最长约 300 字节
               （长实例名 + TXT），留 SPLIT_HEADROOM 的余量足够判定。 */
            if (!may_split || (nan + nns + nar) == 0 ||
                index < (uint16_t)(MDNS_MAX_PACKET_SIZE - BITENGINE_SPLIT_HEADROOM)) {
                /* 不许分包 / 当前是空包 / 离上限还远 —— 跳过这条，行为同补丁前。 */
                a = a->next;
                continue;
            }
            /* 当前包满了：发出去，另起一包重试同一条。 */
            bitengine_tx_flush(p, packet, index, nq, nan, nns, nar);
            nq = nan = nns = nar = 0;   /* questions 只放第一包 */
            bitengine_tx_reset(p, packet, &index);
            save = index;
            r = append_answer(packet, &index, a, p->tcpip_if);
            if (r) {
                *secs[s].cnt += r;
            } else {
                index = save;           /* 空包也装不下，只能跳过 */
            }
            a = a->next;
        }
    }

    bitengine_tx_flush(p, packet, index, nq, nan, nns, nar);
}"""


# ── 第二组：服务数超过 255 时的整型溢出（2026-10-07）─────────────────────────
# 服务数过 255 才够得着 —— 上限 256 时一次全量重探就是 256 条（CONFIG_MDNS_MAX_SERVICES 128 → 256，2026-10-07）。上游 1.11.0 有三处用 8 位整数数服务：
#   ① mdns_priv_create_announce_packet() 的循环变量 uint8_t i 对 size_t len：一次宣告 ≥256 个
#      服务时 i 永远 < len —— 死循环，mdns 任务占满 CPU，任务看门狗 panic；
#   ② mdns_pcb_t.probe_services_len 是 uint8_t：同时探测的服务 >255 时长度被截断，
#      多出来的服务永远不会被宣告，而那块数组照样分配着；
#   ③ 遍历/删除探测集合的循环变量 uint8_t / int，跟着 ② 一起改成 size_t。
# 每条都是 (文件, 原文锚点, 替换)。锚点必须在文件里恰好出现一次；替换里带 OVF_MARK，
# 据此判断打过没有（幂等）。上游一改这几段就匹配失败、构建停下 —— 与上面的多包补丁同一个规矩。
OVF_MARK = "/* bitEngine: >255 services */"
OVERFLOW_PATCHES = [
    # ⚠️ 只改这一行：下面 SCALE_PATCHES ④ 要改紧跟着的 for 循环体，两条补丁的文本不能重叠，
    #    否则后打的会把先打的替换文本改掉，重跑时这条就找不到锚点、构建停下（2026-10-07 栽过）
    ("mdns_send.c",
     """    packet->flags = MDNS_FLAGS_QR_AUTHORITATIVE;

    uint8_t i;
""",
     """    packet->flags = MDNS_FLAGS_QR_AUTHORITATIVE;

    size_t i;   """ + OVF_MARK + """
"""),
    ("mdns_pcb.c",
     """    mdns_srv_item_t **probe_services;
    uint8_t probe_services_len;
""",
     """    mdns_srv_item_t **probe_services;
    size_t probe_services_len;   """ + OVF_MARK + """
"""),
    ("mdns_pcb.c",
     """    if (PCB_STATE_IS_PROBING(_pcb)) {
        uint8_t i;
        //check if we are probing this service""",
     """    if (PCB_STATE_IS_PROBING(_pcb)) {
        size_t i;   """ + OVF_MARK + """
        //check if we are probing this service"""),
    ("mdns_pcb.c",
     """            if (_pcb->probe_services_len > 1) {
                uint8_t n;""",
     """            if (_pcb->probe_services_len > 1) {
                size_t n;   """ + OVF_MARK),
    ("mdns_pcb.c",
     """            for (int i = 0; i < pcb->probe_services_len; ++i) {""",
     """            for (size_t i = 0; i < pcb->probe_services_len; ++i) {   """ + OVF_MARK),
]


# ── 第三组：代播服务成批进来时的 CPU、队列与包完整性（2026-10-07）────────────────────
# 都是注册潮压测（tools/srp_load.py --burst）在板子上量出来、再经对抗性审查改过一轮的，判据与数据见
# docs/test_cases_scale_baseline.md「SRP 代播满载压测」。
#
# ① 代播服务不探测；PCB 忙时记进待宣告，这一轮做完一起宣告。上游给每个新服务都走"探测 3 轮 → 宣告"；PCB 正在
#    探测时，新服务要在**调用方上下文**（SRP 代播就是 OT 任务）里并进探测集合、把全部 P 个在探测的服务重拼一遍
#    探测包（O(P²)）并从头再探。设备成批重注册时探测永远做不完，P 涨到"迄今注册的全部"，总量 O(N³)（实测 128 台时
#    每条注册的代价从 28 ms 涨到 56 ms）。而**探测代播服务在这个组件里没有任何作用**：收到的冲突对非本机主机名
#    一律跳过（mdns_receive.c 里 SRV / TXT / A / AAAA 四处 `if (!is_selfhosted) continue`），既不改名也不撤销。
#    所以代播服务一律不进探测集合（mdns_priv_init_pcb_probe 入口分拣，接口上线 / 改主机名的全量重探也只探本机的）：
#      - PCB 空闲（RUNNING）→ 立刻宣告；
#      - PCB 正在探测 / 宣告 → 记进这个 PCB 的待宣告集合，探测做完生成宣告包时并进去（PROBE_3），宣告做完回到
#        RUNNING 时另起一轮（ANNOUNCE_3）。也不往正在发的宣告包里追加：上游追加会把状态拨回 ANNOUNCE_1，
#        注册潮里每来一条就把已攒的全部记录再发三遍。这样每个服务恰好进一轮宣告，总量线性。
#    本机自己的服务照旧探测。删服务先从待宣告里摘掉（存的是服务项指针）；PCB 关闭时清空；自有服务重新探测要清
#    发送队列时，先把在途宣告里的代播服务收回待宣告（否则那一轮就此丢失）。并包走尾指针 + 局部去重的快路径。
#    顺带修两处上游的"卡死在宣告态"：建宣告包失败前就置了 ANNOUNCE_1；宣告态却没有待发包（被删空 / 被清掉）时
#    之后的宣告请求全被吞掉。现在都当作空闲处理。
#    ⚠️ 2026-10-07 第一版做的是"分批探测"（PCB 忙时把新服务攒到下一批**探测**），对抗性审查找出两处致命问题：
#    正在探测的自有服务冲突改名后探测包还指着已释放的旧名字（UAF）；"回到 RUNNING 就踢下一批探测"会清掉本批
#    还没发出去的宣告包（init_pcb_probe 先清发送队列；而上游状态机会被普通应答包推进）。现在攒的是**宣告**：
#    自有服务改名仍走上游重拼；起一轮宣告只往队列里加包、不清队列。
# ② 删除时的悬空指针（上游缺陷，规模化后高发）：
#    - 删服务：上游按服务类型删探测包里"第一条"问题（同类型的 `_matter._tcp` 会删错），主机问题从不删 —— 留下指向
#      已释放字符串的问题，下一次探测派发就读到已释放内存。改成按指针精确删。
#    - 删代播主机：待发包里的 A/AAAA 存的是主机指针，上游删主机时不摘，派发时 append_host_answer 读已释放内存。
#      现在先摘掉（摘空的包整个丢掉，与删服务同一规矩）。
# ③ SDPTR 去重之后（⑥），一种服务类型在待发包里只剩一条 SDPTR 指向某个代表服务；代表被删时改挂到同类型
#    的另一个服务上，不然这批宣告 / 应答就不再带这个类型。
# ④ 包边界上的多段写入要么全进、要么全不进（多包补丁的配套）。上游 append_fqdn 写不下后几段时返回前面已写
#    的长度（名字没结尾，记录照样被计数，对端整包解析失败）；子类型 PTR、主机的第二个地址写不下时被静默跳过、
#    函数仍返回 ≥1，多包补丁就当它写进去了 —— 按 `_I<fabric>` 子类型浏览的 Matter 控制器会漏掉落在包边界上的
#    设备。改成写不全就回滚 index、返回 0，交给多包补丁换新包重试。
# ⑤ 入队背压。delegate_hostname_add / set_address / remove 是"入队（add 还要等处理完）"，入队却是零等待：
#    mdns 任务（优先级 1）一被饿住、动作队列（16）满了就直接 ESP_ERR_NO_MEM，代播库把整台主机丢掉、回 SERVFAIL。
#    改成最多等 BITENGINE_MDNS_ENQUEUE_WAIT_MS（200 ms）：够吸收尖峰；不用更长 —— 调用方是 OT 任务，等的时候
#    OT 主循环（射频收包、定时器）跟着停，而且它会抢走每个刚腾出的槽位，别的零等待入队者（收包、系统事件）挤不进去。
#    mdns 任务自己调用时仍零等待（只有它在消费队列，等就是自锁）。
# ⑥ SDPTR 按类型去重。解析服务类型枚举查询（_services._dns-sd._udp）时上游给**每个服务**生成一个 SDPTR 问题，
#    拼应答时每个问题再遍历全部服务、每加一条线性去重：N 个相同的 _matter._tcp 问题 × N × O(N) = O(N³)，
#    259 条时一个查询在 mdns 任务里 19.3 s（两个网口各一次 38 s，局域网上谁发一次网关就被任务看门狗重启）。
#    解析时每种（类型, 协议）只生成一个问题；拼应答 / 宣告时每种类型只放一条（线上本来就是同一条记录）。
# ⑦ mdns 定时器别在 esp_timer 任务里死等服务锁。timer_cb（优先级 22，每 100 ms）里原本 portMAX_DELAY 拿锁，
#    持锁的 mdns 任务于是被优先级继承抬到 22，拼一个大应答的那几秒压住 EZSP 桥与 lwIP（258 条时实测任务看门狗
#    报 tcp_to_uart 饿死）。send_packets 最多等 10 ms，query_start_stop 只试不等（搜索晚一拍无妨）：等待超时后
#    FreeRTOS 撤销继承来的优先级，所以 mdns 任务每 100 ms 最多被抬 10 ms。拿不到就跳过这一拍（发送队列按时刻排）。
# ⑧ mdns 任务连续干活超过 200 ms 就睡一个节拍。它的优先级是 1，只比 IDLE 高；应答 `_matter._tcp` 浏览时每个网口
#    各拼一次大应答（252 条时一次 ~0.68 s，附加记录线性去重 O(N²)），几个控制器差不多同时浏览就会背靠背做完，
#    IDLE 一直轮不到 —— 一次浏览实测 IDLE 被饿 1.4 s，4 个就到任务看门狗的 5 s。让一拍之后，IDLE 最长被饿约等于
#    单个最长动作（~0.7 s）。单个动作本身切不开（持着服务锁的一次函数调用），那是 T9.26 的事。
#    让法是睡到 IDLE 真的跑过一次（看 components/thread_br/ot_cpu_guard.c 的 idle hook 时刻，最多 20 拍）：第一版只睡
#    一拍，三连浏览时 IDLE 照样被连续饿了 3.1 s。
# ⑨ 只让探测包 / 宣告包推进 PCB 状态机（上游缺陷）。发送队列里的**每个**包派发完都会经过
#    mdns_priv_pcb_schedule_tx_packet：PCB 在探测 / 宣告态时，一个普通的延迟应答包会被当成探测 / 宣告包再排一次、
#    并把状态推进一格 —— 宣告包被提前放掉（注册潮里实测有实例在 IPv6 上只宣告了 1 次，HA 正在大量解析新实例），
#    探测轮次不足，甚至在 PROBE_3 用应答包生成"宣告"。现在探测包 / 宣告包带标记（bitengine_ctl），状态机只认它们；
#    应答包在任何状态下都是发一次就放。mdns_priv_get_next_packet（只有 PCB 宣告逻辑在用）也只返回带标记的包，
#    "忙态却没有包 = 卡住"的判断因此成立。
SCALE_MARK = "/* bitEngine: scale */"
def smark(note: str) -> str:
    """带说明的标记。说明必须写在注释里面 —— 写成 SCALE_MARK + " 说明" 会把说明落在 */ 外面、编译失败（栽过两次）"""
    return f"/* bitEngine: scale — {note} */"
SCALE_PATCHES = [
    # ---- ① 代播服务不探测；PCB 忙时记进待宣告，这一轮做完一起宣告（mdns_pcb.c） ----
    ("mdns_pcb.c",
     """    uint16_t failed_probes;
} mdns_pcb_t;""",
     """    uint16_t failed_probes;
    mdns_srv_item_t **bitengine_pend;   """ + smark("待宣告的代播服务（SCALE ①）") + """
    size_t bitengine_pend_len;
    size_t bitengine_pend_cap;
} mdns_pcb_t;"""),
    ("mdns_pcb.c",
     """static mdns_pcb_t s_pcbs[MDNS_MAX_INTERFACES][MDNS_IP_PROTOCOL_MAX];
""",
     """static mdns_pcb_t s_pcbs[MDNS_MAX_INTERFACES][MDNS_IP_PROTOCOL_MAX];

/* bitEngine: scale — 代播服务的待宣告集合（见 tools/patch_mdns.py SCALE ①）。
   PCB 正在探测 / 宣告时新来的代播服务记在这里，这一轮做完一起宣告：PROBE_3 生成宣告包时并进去，
   ANNOUNCE_3 回到 RUNNING 时另起一轮。存的是服务项指针 —— 删服务时 mdns_priv_remove_scheduled_service_packets
   先调 mdns_priv_pcb_pend_forget 摘掉；PCB 关闭（deinit_pcb）时整个清空。 */
#include <strings.h>

static bool bitengine_delegated(mdns_srv_item_t **services, size_t len)
{
    const char *self = mdns_priv_get_global_hostname();
    if (!services || !len || mdns_utils_str_null_or_empty(self)) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const char *h = services[i]->service->hostname;
        if (!h || !strcasecmp(h, self)) {
            return false;
        }
    }
    return true;
}

static void bitengine_pend_clear(mdns_pcb_t *pcb)
{
    mdns_mem_free(pcb->bitengine_pend);
    pcb->bitengine_pend = NULL;
    pcb->bitengine_pend_len = 0;
    pcb->bitengine_pend_cap = 0;
}

/* 记进待宣告（按指针去重）。内存不够返回 false，调用方退回上游的路（探测）：慢，但不丢 */
static bool bitengine_pend_add(mdns_pcb_t *pcb, mdns_srv_item_t **services, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        size_t k = 0;
        while (k < pcb->bitengine_pend_len && pcb->bitengine_pend[k] != services[i]) {
            k++;
        }
        if (k < pcb->bitengine_pend_len) {
            continue;
        }
        if (pcb->bitengine_pend_len == pcb->bitengine_pend_cap) {
            size_t cap = pcb->bitengine_pend_cap ? pcb->bitengine_pend_cap * 2 : 16;
            mdns_srv_item_t **n = (mdns_srv_item_t **)mdns_mem_malloc(cap * sizeof(*n));
            if (!n) {
                HOOK_MALLOC_FAILED;
                return false;
            }
            if (pcb->bitengine_pend_len) {
                memcpy(n, pcb->bitengine_pend, pcb->bitengine_pend_len * sizeof(*n));
            }
            mdns_mem_free(pcb->bitengine_pend);
            pcb->bitengine_pend = n;
            pcb->bitengine_pend_cap = cap;
        }
        pcb->bitengine_pend[pcb->bitengine_pend_len++] = services[i];
    }
    return true;
}

/* 把待宣告的代播服务并进宣告包 p 的 answers 段（主机地址一并带上）；skip 里的服务 p 里已经有了。
   全部并进去才清空待宣告；内存不够就留着，下一个时机再带。走 bitengine_append_services_fast：
   上游逐条全表去重 + 走到链表尾，一次并进 252 个要 ~170 万次 PSRAM 结点访问（审查 ⚠️5） */
static bool bitengine_pend_append(mdns_pcb_t *pcb, mdns_tx_packet_t *p, mdns_srv_item_t **skip, size_t skip_len)
{
    if (!pcb->bitengine_pend_len) {
        return true;
    }
    if (!bitengine_append_services_fast(p, pcb->bitengine_pend, pcb->bitengine_pend_len, skip, skip_len)) {
        return false;
    }
    bitengine_pend_clear(pcb);
    return true;
}

/* 自有服务要重新探测时，上游会先 clear_tx_queue_if 清掉这个 PCB 的整个发送队列 —— 正在发的那一轮宣告里的代播
   服务已经不在待宣告里了，会就此再也不被宣告（审查 ❌1：开机 `_meshcop` 发布、冲突重探都会撞上）。清之前收回来 */
static void bitengine_pend_rescue(mdns_if_t tcpip_if, mdns_ip_protocol_t ip_protocol)
{
    mdns_pcb_t *pcb = &s_pcbs[tcpip_if][ip_protocol];
    if (!mdns_priv_get_next_packet(tcpip_if, ip_protocol)) {
        return;     /* 没有状态机包在发，就没有在途的宣告 */
    }
    for (mdns_srv_item_t *it = mdns_priv_get_services(); it; it = it->next) {
        if (bitengine_delegated(&it, 1) && bitengine_srv_in_ctl_packets(tcpip_if, ip_protocol, it->service)) {
            bitengine_pend_add(pcb, &it, 1);   /* 内存不够就丢这一次宣告（同上游），查询照样能答 */
        }
    }
}

/* PCB 空闲（RUNNING）：把待宣告的立刻起一轮宣告（同一个包按上游状态机发三遍） */
static void bitengine_pend_kick(mdns_if_t tcpip_if, mdns_ip_protocol_t ip_protocol)
{
    mdns_pcb_t *pcb = &s_pcbs[tcpip_if][ip_protocol];
    if (!pcb->bitengine_pend_len || pcb->state != PCB_RUNNING || !mdns_priv_if_ready(tcpip_if, ip_protocol)
            || mdns_utils_str_null_or_empty(mdns_priv_get_global_hostname())) {
        return;
    }
    mdns_tx_packet_t *p = mdns_priv_create_announce_packet(tcpip_if, ip_protocol, NULL, 0, false);
    if (!p) {
        return;
    }
    if (!bitengine_pend_append(pcb, p, NULL, 0)) {
        mdns_priv_free_tx_packet(p);
        return;
    }
    p->bitengine_ctl = 1;
    pcb->state = PCB_ANNOUNCE_1;
    mdns_priv_send_after(p, 0);
}

/* 待宣告里有东西时按 PCB 状态安排：探测中 → PROBE_3 生成宣告时带上；宣告中且有包在发 → 回到 RUNNING 时另起一轮；
   宣告态却没有包（被删空 / 被清掉）→ 当作空闲；空闲 → 立刻起一轮 */
static void bitengine_pend_settle(mdns_if_t tcpip_if, mdns_ip_protocol_t ip_protocol)
{
    mdns_pcb_t *pcb = &s_pcbs[tcpip_if][ip_protocol];
    if (!pcb->bitengine_pend_len || PCB_STATE_IS_PROBING(pcb)) {
        return;
    }
    if (PCB_STATE_IS_ANNOUNCING(pcb)) {
        if (mdns_priv_get_next_packet(tcpip_if, ip_protocol)) {
            return;
        }
        pcb->state = PCB_RUNNING;
    }
    bitengine_pend_kick(tcpip_if, ip_protocol);
}

/* 宣告态、这些服务已经在待发包里、而且那个包至少还要再发两遍：记录是派发时现读的，不必再排一轮 */
static bool bitengine_in_flight(mdns_pcb_t *pcb, mdns_tx_packet_t *p, mdns_srv_item_t **services, size_t len)
{
    if (pcb->state != PCB_ANNOUNCE_1 && pcb->state != PCB_ANNOUNCE_2) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const mdns_out_answer_t *a = p->answers;
        while (a && !(a->type == MDNS_TYPE_SRV && a->service == services[i]->service)) {
            a = a->next;
        }
        if (!a) {
            return false;
        }
    }
    return true;
}

void mdns_priv_pcb_pend_forget(mdns_service_t *service)
{
    for (int i = 0; i < MDNS_MAX_INTERFACES; i++) {
        for (int j = 0; j < MDNS_IP_PROTOCOL_MAX; j++) {
            mdns_pcb_t *pcb = &s_pcbs[i][j];
            size_t w = 0;
            for (size_t k = 0; k < pcb->bitengine_pend_len; k++) {
                if (pcb->bitengine_pend[k]->service != service) {
                    pcb->bitengine_pend[w++] = pcb->bitengine_pend[k];
                }
            }
            pcb->bitengine_pend_len = w;
        }
    }
}

void mdns_priv_pcb_pend_settle_all(void)
{
    for (int i = 0; i < MDNS_MAX_INTERFACES; i++) {
        for (int j = 0; j < MDNS_IP_PROTOCOL_MAX; j++) {
            if (mdns_priv_if_ready((mdns_if_t) i, (mdns_ip_protocol_t) j)) {
                bitengine_pend_settle((mdns_if_t) i, (mdns_ip_protocol_t) j);
            }
        }
    }
}
"""),
    ("mdns_pcb.c",
     """    if (mdns_priv_if_ready(tcpip_if, ip_protocol)) {
        if (PCB_STATE_IS_PROBING(_pcb)) {
            mdns_priv_init_pcb_probe(tcpip_if, ip_protocol, services, len, include_ip);
        } else if (PCB_STATE_IS_ANNOUNCING(_pcb)) {
            mdns_tx_packet_t   *p = mdns_priv_get_next_packet(tcpip_if, ip_protocol);
            if (p) {
                for (i = 0; i < len; i++) {
                    if (!mdns_priv_create_answer(&p->answers, MDNS_TYPE_SDPTR, services[i]->service, NULL, false, false)
                            || !mdns_priv_create_answer(&p->answers, MDNS_TYPE_PTR, services[i]->service, NULL, false,
                                                        false)
                            || !mdns_priv_create_answer(&p->answers, MDNS_TYPE_SRV, services[i]->service, NULL, true,
                                                        false)
                            || !mdns_priv_create_answer(&p->answers, MDNS_TYPE_TXT, services[i]->service, NULL, true,
                                                        false)) {
                        break;
                    }
                }
                if (include_ip) {
                    mdns_priv_dealloc_answer(&p->additional, MDNS_TYPE_A, NULL);
                    mdns_priv_dealloc_answer(&p->additional, MDNS_TYPE_AAAA, NULL);
                    mdns_priv_append_host_list_in_services(&p->answers, services, len, true, false);
                }
                _pcb->state = PCB_ANNOUNCE_1;
            }
        } else if (_pcb->state == PCB_RUNNING) {

            if (mdns_utils_str_null_or_empty(mdns_priv_get_global_hostname())) {
                return;
            }

            _pcb->state = PCB_ANNOUNCE_1;
            mdns_tx_packet_t *p = mdns_priv_create_announce_packet(tcpip_if, ip_protocol, services, len, include_ip);
            if (p) {
                mdns_priv_send_after(p, 0);
            }
        }
    }""",
     """    if (mdns_priv_if_ready(tcpip_if, ip_protocol)) {
        /* bitEngine: scale — 代播服务在 PCB 忙（探测 / 宣告中）时不并进探测、也不往正在发的宣告包里追加，
           记进待宣告，这一轮做完一起宣告（SCALE ①）。探测态却没有包（卡住了）不在此列，走下面上游的路自愈 */
        if ((PCB_STATE_IS_PROBING(_pcb) || PCB_STATE_IS_ANNOUNCING(_pcb)) && bitengine_delegated(services, len)) {
            mdns_tx_packet_t *bp = mdns_priv_get_next_packet(tcpip_if, ip_protocol);
            if (bp && (bitengine_in_flight(_pcb, bp, services, len) || bitengine_pend_add(_pcb, services, len))) {
                return;
            }
        }
        if (PCB_STATE_IS_PROBING(_pcb)) {
            mdns_priv_init_pcb_probe(tcpip_if, ip_protocol, services, len, include_ip);
        } else if (PCB_STATE_IS_ANNOUNCING(_pcb)) {
            mdns_tx_packet_t   *p = mdns_priv_get_next_packet(tcpip_if, ip_protocol);
            if (p) {
                for (i = 0; i < len; i++) {
                    if ((!bitengine_sdptr_dup(p->answers, services[i]->service)   """ + smark("SDPTR 按类型去重（SCALE ⑥）") + """
                            && !mdns_priv_create_answer(&p->answers, MDNS_TYPE_SDPTR, services[i]->service, NULL, false, false))
                            || !mdns_priv_create_answer(&p->answers, MDNS_TYPE_PTR, services[i]->service, NULL, false,
                                                        false)
                            || !mdns_priv_create_answer(&p->answers, MDNS_TYPE_SRV, services[i]->service, NULL, true,
                                                        false)
                            || !mdns_priv_create_answer(&p->answers, MDNS_TYPE_TXT, services[i]->service, NULL, true,
                                                        false)) {
                        break;
                    }
                }
                if (include_ip) {
                    mdns_priv_dealloc_answer(&p->additional, MDNS_TYPE_A, NULL);
                    mdns_priv_dealloc_answer(&p->additional, MDNS_TYPE_AAAA, NULL);
                    mdns_priv_append_host_list_in_services(&p->answers, services, len, true, false);
                }
                /* 待宣告不往这里倒：这个包可能已攒了几百条，状态一拨回 ANNOUNCE_1 就全部再发三遍；ANNOUNCE_3 时另起一轮 */
                _pcb->state = PCB_ANNOUNCE_1;
            } else {
                /* bitEngine: scale — 宣告态却没有待发包（被删空 / 被清掉）：当作空闲重新宣告，不然这个 PCB 再也不宣告新东西 */
                _pcb->state = PCB_RUNNING;
                mdns_priv_pcb_announce(tcpip_if, ip_protocol, services, len, include_ip);
            }
        } else if (_pcb->state == PCB_RUNNING) {

            if (mdns_utils_str_null_or_empty(mdns_priv_get_global_hostname())) {
                return;
            }

            mdns_tx_packet_t *p = mdns_priv_create_announce_packet(tcpip_if, ip_protocol, services, len, include_ip);
            if (p) {   """ + smark("建包成功才进宣告态（上游先置状态、建包失败就卡在宣告态）；待宣告的一并带上") + """
                bitengine_pend_append(_pcb, p, services, len);
                p->bitengine_ctl = 1;
                _pcb->state = PCB_ANNOUNCE_1;
                mdns_priv_send_after(p, 0);
            }
        }
    }"""),
    ("mdns_pcb.c",
     """    mdns_mem_free(pcb->probe_services);
    pcb->state = PCB_OFF;
""",
     """    mdns_mem_free(pcb->probe_services);
    bitengine_pend_clear(pcb);   """ + smark("PCB 关了，待宣告作废；重新打开时 restart_pcb 会把全部服务再宣告一遍") + """
    pcb->state = PCB_OFF;
"""),
    ("mdns_pcb.c",
     """    case PCB_PROBE_3:
        a = mdns_priv_create_announce_from_probe(p);
        if (!a) {
            mdns_priv_send_after(p, 250);
            break;
        }
""",
     """    case PCB_PROBE_3:
        a = mdns_priv_create_announce_from_probe(p);
        if (!a) {
            mdns_priv_send_after(p, 250);
            break;
        }
        bitengine_pend_append(pcb, a, NULL, 0);   """ + smark("探测期间攒下的代播服务随这一轮宣告一起发（SCALE ①）") + """
        a->bitengine_ctl = 1;
"""),
    ("mdns_pcb.c",
     """    case PCB_ANNOUNCE_3:
        pcb->state = PCB_RUNNING;
        mdns_priv_free_tx_packet(p);
        break;""",
     """    case PCB_ANNOUNCE_3:
        pcb->state = PCB_RUNNING;
        {   """ + smark("这一轮宣告做完：宣告期间攒下的代播服务另起一轮（SCALE ①）") + """
            mdns_if_t bitengine_if = p->tcpip_if;
            mdns_ip_protocol_t bitengine_ip = p->ip_protocol;
            mdns_priv_free_tx_packet(p);
            bitengine_pend_kick(bitengine_if, bitengine_ip);
        }
        break;"""),
    ("mdns_pcb.c",
     """    mdns_pcb_t *pcb = &s_pcbs[tcpip_if][ip_protocol];

    mdns_priv_clear_tx_queue_if(tcpip_if, ip_protocol);
""",
     """    mdns_pcb_t *pcb = &s_pcbs[tcpip_if][ip_protocol];

    /* bitEngine: scale — 代播服务不进探测集合，记进待宣告（SCALE ①）：对它们探测在这个组件里没有任何作用（收到的冲突
       对非本机主机名一律跳过），只换来 O(P²) 的重拼。原地交换而不是覆盖 —— probe_all_pcbs 把同一个数组依次交给
       每个 PCB，交换只改顺序、不丢不重。 */
    size_t bitengine_own = 0;
    for (size_t k = 0; k < len; k++) {
        if (bitengine_delegated(&services[k], 1) && bitengine_pend_add(pcb, &services[k], 1)) {
            continue;
        }
        mdns_srv_item_t *bitengine_t = services[bitengine_own];
        services[bitengine_own++] = services[k];
        services[k] = bitengine_t;
    }
    bool bitengine_alive = pcb->state == PCB_RUNNING || PCB_STATE_IS_ANNOUNCING(pcb)
                           || (PCB_STATE_IS_PROBING(pcb) && mdns_priv_get_next_packet(tcpip_if, ip_protocol));
    if (len && !bitengine_own && !probe_ip && bitengine_alive) {
        /* 没有要探测的：别碰发送队列（下面的 clear_tx_queue_if 会清掉正在发的宣告），按状态安排宣告。
           PCB 不在正常运转（OFF：接口上线时首次探测内存不够；探测态却没有探测包）不在此列：走下面上游的路
           重新探一轮自愈（审查 ⚠️3），攒下的随那一轮宣告 */
        bitengine_pend_settle(tcpip_if, ip_protocol);
        return;
    }
    len = bitengine_own;

    bitengine_pend_rescue(tcpip_if, ip_protocol);
    mdns_priv_clear_tx_queue_if(tcpip_if, ip_protocol);
"""),
    ("private_include/mdns_pcb.h",
     """void mdns_priv_init_pcb_probe(mdns_if_t tcpip_if, mdns_ip_protocol_t ip_protocol, mdns_srv_item_t **services, size_t len, bool probe_ip);
""",
     """void mdns_priv_init_pcb_probe(mdns_if_t tcpip_if, mdns_ip_protocol_t ip_protocol, mdns_srv_item_t **services, size_t len, bool probe_ip);

/* bitEngine: scale — 待宣告的代播服务（tools/patch_mdns.py SCALE ①）。forget：服务要被删了，从各 PCB 的待宣告里摘掉；
   settle_all：各 PCB 若空闲而待宣告非空，立刻起一轮宣告 */
void mdns_priv_pcb_pend_forget(mdns_service_t *service);
void mdns_priv_pcb_pend_settle_all(void);
"""),
    # ---- ② 按指针精确删探测问题（替换上游按类型删第一条的那段）+ ③ SDPTR 代表改挂 ----
    ("mdns_send.c",
     """        bool had_answers = (q->answers != NULL);

        dealloc_scheduled_service_answers(&(q->answers), service);
        dealloc_scheduled_service_answers(&(q->additional), service);
        dealloc_scheduled_service_answers(&(q->servers), service);

        if (mdns_priv_if_ready(q->tcpip_if, q->ip_protocol)) {
            bool should_remove_questions = false;
            mdns_priv_pcb_check_probing_services(q->tcpip_if, q->ip_protocol, service,
                                                 had_answers && q->answers == NULL, &should_remove_questions);
            if (should_remove_questions && q->questions) {
                mdns_out_question_t *qsn = NULL;
                mdns_out_question_t *qs = q->questions;
                if (qs->type == MDNS_TYPE_ANY
                        && qs->service && strcmp(qs->service, service->service) == 0
                        && qs->proto && strcmp(qs->proto, service->proto) == 0) {
                    q->questions = q->questions->next;
                    mdns_mem_free(qs);
                } else while (qs->next) {
                        qsn = qs->next;
                        if (qsn->type == MDNS_TYPE_ANY
                                && qsn->service && strcmp(qsn->service, service->service) == 0
                                && qsn->proto && strcmp(qsn->proto, service->proto) == 0) {
                            qs->next = qsn->next;
                            mdns_mem_free(qsn);
                            break;
                        }
                        qs = qs->next;
                    }
            }
        }""",
     """        bool had_answers = (q->answers != NULL);

        """ + smark("SDPTR 代表要被删了：改挂到同类型的另一个服务（SCALE ③）") + """
        bitengine_sdptr_repoint(q->answers, service);
        bitengine_sdptr_repoint(q->additional, service);
        bitengine_sdptr_repoint(q->servers, service);
        dealloc_scheduled_service_answers(&(q->answers), service);
        dealloc_scheduled_service_answers(&(q->additional), service);
        dealloc_scheduled_service_answers(&(q->servers), service);

        if (mdns_priv_if_ready(q->tcpip_if, q->ip_protocol)) {
            bool should_remove_questions = false;
            mdns_priv_pcb_check_probing_services(q->tcpip_if, q->ip_protocol, service,
                                                 had_answers && q->answers == NULL && q->bitengine_ctl,   /* 应答包删空不算（SCALE ⑨） */
                                                 &should_remove_questions);
            (void)should_remove_questions;   /* 上游据它按类型删"第一条"问题，会删错，见下 */
        }
        /* bitEngine: scale — 按指针精确删这个服务的问题（SCALE ②）：服务问题的 service 字段、主机问题的 host 字段
           都直接借用服务自己的字符串；自带内存的问题（应答里回显的提问）不是借来的，不动 */
        for (mdns_out_question_t **pp = &q->questions; *pp; ) {
            mdns_out_question_t *x = *pp;
            if (!x->own_dynamic_memory && ((x->service && x->service == service->service) ||
                                           (x->host && (x->host == service->hostname || x->host == service->instance)))) {
                *pp = x->next;
                mdns_mem_free(x);
            } else {
                pp = &x->next;
            }
        }"""),
    # ---- ③ / ⑥ 的帮手：放在 dealloc_scheduled_service_answers 前面 ----
    ("mdns_send.c",
     """/**
 * @brief  Find, remove and free answers and scheduled packets for service
 */
void mdns_priv_remove_scheduled_service_packets(mdns_service_t *service)""",
     """/* bitEngine: scale — 删代播主机前，把待发包里指向它的 A/AAAA 摘掉（上游缺陷：应答里存的是主机指针，主机一删就悬空，
   派发时 append_host_answer 读已释放内存）。摘空的包整个丢掉，与删服务时同一规矩。 */
static void bitengine_dealloc_host_answers(mdns_out_answer_t **destination, mdns_host_item_t *host)
{
    for (mdns_out_answer_t **pp = destination; *pp; ) {
        mdns_out_answer_t *a = *pp;
        if (a->host == host) {
            *pp = a->next;
            mdns_mem_free(a);
        } else {
            pp = &a->next;
        }
    }
}

void bitengine_remove_scheduled_host_answers(mdns_host_item_t *host)
{
    if (!host) {
        return;
    }
    mdns_tx_packet_t *q = s_tx_queue_head;
    while (q) {
        mdns_tx_packet_t *x = q;
        q = q->next;
        bitengine_dealloc_host_answers(&x->answers, host);
        bitengine_dealloc_host_answers(&x->additional, host);
        bitengine_dealloc_host_answers(&x->servers, host);
        if (!x->questions && !x->answers && !x->additional && !x->servers) {
            queueDetach(mdns_tx_packet_t, s_tx_queue_head, x);
            mdns_priv_free_tx_packet(x);
        }
    }
    mdns_priv_pcb_pend_settle_all();   /* 若把某个 PCB 正在发的宣告包摘空了，攒着的别等 */
}

/* bitEngine: scale — SDPTR 代表要被删时，改挂到同类型、还活着的另一个服务（SCALE ③）；找不到就留给后面的 dealloc 删掉 */
static void bitengine_sdptr_repoint(mdns_out_answer_t *list, mdns_service_t *service)
{
    for (mdns_out_answer_t *a = list; a; a = a->next) {
        if (a->type != MDNS_TYPE_SDPTR || a->service != service || !service->service || !service->proto) {
            continue;
        }
        for (mdns_srv_item_t *s = mdns_priv_get_services(); s; s = s->next) {
            if (s->service != service && s->service->service && s->service->proto
                    && !strcasecmp(s->service->service, service->service) && !strcasecmp(s->service->proto, service->proto)) {
                a->service = s->service;
                break;
            }
        }
    }
}

/**
 * @brief  Find, remove and free answers and scheduled packets for service
 */
void mdns_priv_remove_scheduled_service_packets(mdns_service_t *service)"""),
    # ---- ④ 包边界上的多段写入：要么全进、要么全不进 ----
    ("mdns_send.c",
     """    // string is not yet in the packet, so let's add it
    if (!len_location) {
        uint8_t written = append_string(packet, index, strings[0]);
        if (!written) {
            return 0;
        }
        // run the same for the other strings in the name
        return written + append_fqdn(packet, index, &strings[1], count - 1, packet_len);
    }""",
     """    // string is not yet in the packet, so let's add it
    if (!len_location) {
        uint16_t bitengine_start = *index;   """ + smark("名字写不全就整段回滚（SCALE ④）") + """
        uint8_t written = append_string(packet, index, strings[0]);
        if (!written) {
            return 0;
        }
        // run the same for the other strings in the name
        uint16_t rest = append_fqdn(packet, index, &strings[1], count - 1, packet_len);
        if (!rest) {
            *index = bitengine_start;
            return 0;
        }
        return written + rest;
    }"""),
    ("mdns_send.c",
     """    uint8_t appended_answers = 0;

    if (append_ptr_record(packet, index, mdns_utils_get_service_instance_name(service), service->service,
                          service->proto, flush, bye) <= 0) {
        return appended_answers;
    }
    appended_answers++;

    mdns_subtype_t *subtype = service->subtype;
    while (subtype) {
        appended_answers +=
            (append_subtype_ptr_record(packet, index, mdns_utils_get_service_instance_name(service), subtype->subtype,
                                       service->service, service->proto, flush, bye) > 0);
        subtype = subtype->next;
    }

    return appended_answers;""",
     """    uint8_t appended_answers = 0;
    uint16_t bitengine_start = *index;   """ + smark("基础 PTR + 子类型 PTR 要么全进要么全不进（SCALE ④）") + """

    if (append_ptr_record(packet, index, mdns_utils_get_service_instance_name(service), service->service,
                          service->proto, flush, bye) <= 0) {
        *index = bitengine_start;
        return 0;
    }
    appended_answers++;

    mdns_subtype_t *subtype = service->subtype;
    while (subtype) {
        if (append_subtype_ptr_record(packet, index, mdns_utils_get_service_instance_name(service), subtype->subtype,
                                      service->service, service->proto, flush, bye) <= 0) {
            *index = bitengine_start;   /* 写不下：整组回滚，交给多包补丁换新包重试 */
            return 0;
        }
        appended_answers++;
        subtype = subtype->next;
    }

    return appended_answers;"""),
    ("mdns_send.c",
     """    mdns_ip_addr_t *addr = host->address_list;
    uint8_t num_records = 0;

    while (addr != NULL) {
        if (addr->addr.type == address_type) {
#ifdef CONFIG_LWIP_IPV4
            if (address_type == ESP_IPADDR_TYPE_V4 &&
                    append_a_record(packet, index, host->hostname, addr->addr.u_addr.ip4.addr, flush, bye) <= 0) {
                break;
            }
#endif /* CONFIG_LWIP_IPV4 */
#ifdef CONFIG_LWIP_IPV6
            if (address_type == ESP_IPADDR_TYPE_V6 &&
                    append_aaaa_record(packet, index, host->hostname, (uint8_t *)addr->addr.u_addr.ip6.addr, flush,
                                       bye) <= 0) {
                break;
            }
#endif /* CONFIG_LWIP_IPV6 */""",
     """    mdns_ip_addr_t *addr = host->address_list;
    uint8_t num_records = 0;
    uint16_t bitengine_start = *index;   """ + smark("一台主机的地址要么全进要么全不进（SCALE ④）") + """

    while (addr != NULL) {
        if (addr->addr.type == address_type) {
#ifdef CONFIG_LWIP_IPV4
            if (address_type == ESP_IPADDR_TYPE_V4 &&
                    append_a_record(packet, index, host->hostname, addr->addr.u_addr.ip4.addr, flush, bye) <= 0) {
                *index = bitengine_start;
                return 0;
            }
#endif /* CONFIG_LWIP_IPV4 */
#ifdef CONFIG_LWIP_IPV6
            if (address_type == ESP_IPADDR_TYPE_V6 &&
                    append_aaaa_record(packet, index, host->hostname, (uint8_t *)addr->addr.u_addr.ip6.addr, flush,
                                       bye) <= 0) {
                *index = bitengine_start;
                return 0;
            }
#endif /* CONFIG_LWIP_IPV6 */"""),
    # ---- ① / ② 的配套：删服务先从待宣告里摘掉，删完若有 PCB 空着就把攒下的宣告出去；删主机前摘掉指向它的 A/AAAA ----
    ("mdns_send.c",
     """    if (!service) {
        return;
    }
    mdns_tx_packet_t *p = NULL;
    mdns_tx_packet_t *q = s_tx_queue_head;
""",
     """    if (!service) {
        return;
    }
    mdns_priv_pcb_pend_forget(service);   """ + smark("待宣告里存的是服务项指针，删之前摘掉（SCALE ①）") + """
    mdns_tx_packet_t *p = NULL;
    mdns_tx_packet_t *q = s_tx_queue_head;
"""),
    ("mdns_send.c",
     """        p = q;
        q = q->next;
        if (!p->questions && !p->answers && !p->additional && !p->servers) {
            queueDetach(mdns_tx_packet_t, s_tx_queue_head, p);
            mdns_priv_free_tx_packet(p);
        }
    }
}
""",
     """        p = q;
        q = q->next;
        if (!p->questions && !p->answers && !p->additional && !p->servers) {
            queueDetach(mdns_tx_packet_t, s_tx_queue_head, p);
            mdns_priv_free_tx_packet(p);
        }
    }
    mdns_priv_pcb_pend_settle_all();   """ + smark("正在发的宣告包可能被删空了：攒着的别等（SCALE ①）") + """
}
"""),
    ("mdns_responder.c",
     """            mdns_utils_free_address_list(host->address_list);
            mdns_mem_free((char *)host->hostname);
            mdns_mem_free(host);
            break;""",
     """            bitengine_remove_scheduled_host_answers(host);   """ + smark("待发包里的 A/AAAA 存的是主机指针（SCALE ②）") + """
            mdns_utils_free_address_list(host->address_list);
            mdns_mem_free((char *)host->hostname);
            mdns_mem_free(host);
            break;"""),
    # ---- ⑨ 只让探测包 / 宣告包推进 PCB 状态机；应答包发一次就放 ----
    ("private_include/mdns_private.h",
     """    bool queued;
    uint16_t id;
} mdns_tx_packet_t;""",
     """    bool queued;
    uint16_t id;
    uint8_t bitengine_ctl;   """ + smark("1 = 探测 / 宣告包，由 PCB 状态机重发；0 = 应答，发一次就放（SCALE ⑨）") + """
} mdns_tx_packet_t;"""),
    ("mdns_pcb.c",
     """    mdns_priv_send_after(packet, ((pcb->failed_probes > 5) ? 1000 : 120) + (esp_random() & 0x7F));""",
     """    packet->bitengine_ctl = 1;   """ + smark("探测包进状态机（SCALE ⑨）") + """
    mdns_priv_send_after(packet, ((pcb->failed_probes > 5) ? 1000 : 120) + (esp_random() & 0x7F));"""),
    ("mdns_pcb.c",
     """void mdns_priv_pcb_schedule_tx_packet(mdns_tx_packet_t *p)
{
    mdns_pcb_t *pcb = &s_pcbs[p->tcpip_if][p->ip_protocol];
""",
     """void mdns_priv_pcb_schedule_tx_packet(mdns_tx_packet_t *p)
{
    mdns_pcb_t *pcb = &s_pcbs[p->tcpip_if][p->ip_protocol];
    if (!p->bitengine_ctl) {   """ + smark("应答包：发过就放，不推进状态机（SCALE ⑨）") + """
        mdns_priv_free_tx_packet(p);
        return;
    }
"""),
    ("mdns_send.c",
     """        if (q->tcpip_if == tcpip_if && q->ip_protocol == ip_protocol) {
            return q;
        }""",
     """        if (q->tcpip_if == tcpip_if && q->ip_protocol == ip_protocol && q->bitengine_ctl) {   """ + smark("只认状态机包（SCALE ⑨）") + """
            return q;
        }"""),
    ("mdns_send.c",
     """    part_length = append_fqdn(packet, index, sd_str, 4, MDNS_MAX_PACKET_SIZE);

    record_length += part_length;
""",
     """    part_length = append_fqdn(packet, index, sd_str, 4, MDNS_MAX_PACKET_SIZE);
    if (!part_length) {   """ + smark("属主名没写进去就停：④ 让写不下的名字整段回滚后，后面的类型字段反而写得下（审查 ❌2）") + """
        return 0;
    }

    record_length += part_length;
"""),
    # ---- ⑤ 入队背压 ----
    ("mdns_service.c",
     """bool mdns_priv_queue_action(mdns_action_t *action)
{
    if (xQueueSend(s_action_queue, &action, (TickType_t)0) != pdPASS) {
        return false;
    }
    return true;
}
""",
     """bool mdns_priv_queue_action(mdns_action_t *action)
{
    if (xQueueSend(s_action_queue, &action, (TickType_t)0) != pdPASS) {
        return false;
    }
    return true;
}

/* bitEngine: scale — 入队背压（见 tools/patch_mdns.py SCALE ⑤）。mdns 任务自己调用时不等：只有它在消费队列，等就是自锁。 */
bool mdns_priv_queue_action_wait(mdns_action_t *action, uint32_t timeout_ms)
{
    if (xTaskGetCurrentTaskHandle() == s_service_task_handle) {
        return mdns_priv_queue_action(action);
    }
    return xQueueSend(s_action_queue, &action, pdMS_TO_TICKS(timeout_ms)) == pdPASS;
}

/* bitEngine: scale — 限时拿锁（只给 esp_timer 回调用，见 tools/patch_mdns.py SCALE ⑦） */
bool mdns_priv_service_lock_timed(uint32_t timeout_ms)
{
    return xSemaphoreTake(s_service_semaphore, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
"""),
    ("private_include/mdns_service.h",
     """bool mdns_priv_queue_action(mdns_action_t *action);
""",
     """bool mdns_priv_queue_action(mdns_action_t *action);

/* bitEngine: scale — 同上，但队列满时最多等 timeout_ms（mdns 任务自己调用时不等）；代播入口用它（SCALE ⑤） */
bool mdns_priv_queue_action_wait(mdns_action_t *action, uint32_t timeout_ms);
#define BITENGINE_MDNS_ENQUEUE_WAIT_MS 200

/* bitEngine: scale — 限时拿服务锁，拿到返回 true（之后照常 mdns_priv_service_unlock）。只给 esp_timer 回调用（SCALE ⑦） */
bool mdns_priv_service_lock_timed(uint32_t timeout_ms);
#define BITENGINE_MDNS_TIMER_LOCK_MS 10
"""),
    ("mdns_responder.c",
     """    action->type = ACTION_DELEGATE_HOSTNAME_ADD;
    action->data.delegate_hostname.hostname = new_hostname;
    action->data.delegate_hostname.address_list = mdns_utils_copy_address_list(address_list);
    if (!mdns_priv_queue_action(action)) {""",
     """    action->type = ACTION_DELEGATE_HOSTNAME_ADD;
    action->data.delegate_hostname.hostname = new_hostname;
    action->data.delegate_hostname.address_list = mdns_utils_copy_address_list(address_list);
    if (!mdns_priv_queue_action_wait(action, BITENGINE_MDNS_ENQUEUE_WAIT_MS)) {   """ + SCALE_MARK),
    ("mdns_responder.c",
     """    action->type = ACTION_DELEGATE_HOSTNAME_REMOVE;
    action->data.delegate_hostname.hostname = new_hostname;
    if (!mdns_priv_queue_action(action)) {""",
     """    action->type = ACTION_DELEGATE_HOSTNAME_REMOVE;
    action->data.delegate_hostname.hostname = new_hostname;
    if (!mdns_priv_queue_action_wait(action, BITENGINE_MDNS_ENQUEUE_WAIT_MS)) {   """ + SCALE_MARK),
    ("mdns_responder.c",
     """    action->type = ACTION_DELEGATE_HOSTNAME_SET_ADDR;
    action->data.delegate_hostname.hostname = new_hostname;
    action->data.delegate_hostname.address_list = mdns_utils_copy_address_list(address_list);
    if (!mdns_priv_queue_action(action)) {""",
     """    action->type = ACTION_DELEGATE_HOSTNAME_SET_ADDR;
    action->data.delegate_hostname.hostname = new_hostname;
    action->data.delegate_hostname.address_list = mdns_utils_copy_address_list(address_list);
    if (!mdns_priv_queue_action_wait(action, BITENGINE_MDNS_ENQUEUE_WAIT_MS)) {   """ + SCALE_MARK),
    # ---- ⑦ mdns 定时器限时拿锁 ----
    ("mdns_send.c",
     """void mdns_priv_send_packets(void)
{
    mdns_priv_service_lock();
    mdns_tx_packet_t *p = s_tx_queue_head;""",
     """void mdns_priv_send_packets(void)
{
    if (!mdns_priv_service_lock_timed(BITENGINE_MDNS_TIMER_LOCK_MS)) {   """ + SCALE_MARK + """
        return;     /* 服务锁被占着：这一拍不排发送，100 ms 后再来 */
    }
    mdns_tx_packet_t *p = s_tx_queue_head;"""),
    ("mdns_querier.c",
     """void mdns_priv_query_start_stop(void)
{
    mdns_priv_service_lock();
    mdns_search_once_t *s = s_search_once;""",
     """void mdns_priv_query_start_stop(void)
{
    if (!mdns_priv_service_lock_timed(0)) {   """ + smark("只试不等：同一拍 send_packets 已经等过 10 ms") + """
        return;     /* 搜索状态晚一拍推进无妨 */
    }
    mdns_search_once_t *s = s_search_once;"""),
    # ---- ⑧ mdns 任务连续干活太久就让一个节拍，给 IDLE（任务看门狗靠它喂狗） ----
    ("mdns_service.c",
     """static void service_task(void *pvParameters)
{
    mdns_action_t *a = NULL;
    for (;;) {
        if (mdns_priv_is_server_init() && s_action_queue) {
            if (xQueueReceive(s_action_queue, &a, portMAX_DELAY) == pdTRUE) {""",
     """/* bitEngine: scale — mdns 任务连续干活超过 BITENGINE_MDNS_YIELD_MS 就让出 CPU，直到 IDLE 真的跑过一次
   （最多 BITENGINE_MDNS_YIELD_MAX_TICKS 拍）（SCALE ⑧）。就地定义：本文件不 include private_include/mdns_service.h */
#define BITENGINE_MDNS_YIELD_MS 200
#define BITENGINE_MDNS_YIELD_MAX_TICKS 20

/* bitEngine: scale — IDLE 最近一次运行的时刻（µs 低 32 位）。强定义在 components/thread_br/ot_cpu_guard.c（idle hook）；
   没链进来或 hook 没挂上时为 0 = 不知道，退化成只睡一拍 */
__attribute__((weak)) uint32_t bitengine_idle_last_us(void)
{
    return 0;
}
#if CONFIG_BITENGINE_DIAG_SRP_INJECT
extern void bitengine_diag_mdns_action(int type, uint32_t us);   /* components/thread_br/srp_mdns_wrap.c */
#endif

static void service_task(void *pvParameters)
{
    mdns_action_t *a = NULL;
    int64_t bitengine_busy_since = esp_timer_get_time();   """ + smark("上次让出 CPU 的时刻（SCALE ⑧）") + """
    for (;;) {
        if (mdns_priv_is_server_init() && s_action_queue) {
            /* bitEngine: scale — 队列空着就会阻塞 = 让出了 CPU；否则连续干活超过 BITENGINE_MDNS_YIELD_MS 就睡一个节拍。
               本任务优先级 1，只比 IDLE 高：队列里排着几个大应答（几个控制器同时浏览 _matter._tcp，每个网口各拼一次，
               252 条时一次 ~0.7 s）时会背靠背做完、IDLE 一直轮不到，攒够 5 s 任务看门狗就 panic（SCALE ⑧） */
            bool bitengine_yielded = (uxQueueMessagesWaiting(s_action_queue) == 0);
            if (!bitengine_yielded && esp_timer_get_time() - bitengine_busy_since > BITENGINE_MDNS_YIELD_MS * 1000LL) {
                /* 睡到 IDLE 真的跑过一次：只睡一拍不够 —— 10-07 实测三连浏览时 IDLE 照样被连续饿了 3.1 s */
                uint32_t bitengine_i0 = bitengine_idle_last_us();
                int64_t bitengine_t0 = esp_timer_get_time();
                int bitengine_n = 0;
                do {
                    vTaskDelay(1);
                } while (bitengine_i0 && bitengine_idle_last_us() == bitengine_i0 && ++bitengine_n < BITENGINE_MDNS_YIELD_MAX_TICKS);
#if CONFIG_BITENGINE_DIAG_SRP_INJECT
                bitengine_diag_mdns_action(16, (uint32_t)(esp_timer_get_time() - bitengine_t0));   /* YIELD：睡了多久 */
                if (bitengine_i0 && bitengine_idle_last_us() == bitengine_i0) {
                    bitengine_diag_mdns_action(17, 0);                                           /* YIELD_NOIDLE */
                }
#else
                (void)bitengine_t0;
#endif
                bitengine_yielded = true;
            }
            if (xQueueReceive(s_action_queue, &a, portMAX_DELAY) == pdTRUE) {
                if (bitengine_yielded) {
                    bitengine_busy_since = esp_timer_get_time();
                }"""),
    # ---- 诊断构建：mdns 任务里每个动作的耗时（CONFIG_BITENGINE_DIAG_SRP_INJECT，出厂编不进来） ----
    ("mdns_service.c",
     """                MDNS_SERVICE_LOCK();
                execute_action(a);
                MDNS_SERVICE_UNLOCK();""",
     """                MDNS_SERVICE_LOCK();
#if CONFIG_BITENGINE_DIAG_SRP_INJECT   /* bitEngine: scale — 诊断构建：按动作类型计时（components/thread_br/srp_mdns_wrap.c） */
                extern void bitengine_diag_mdns_action(int type, uint32_t us);
                int64_t bt0 = esp_timer_get_time();
                int bty = a->type;     /* execute_action 会释放 a，先记下 */
#endif
                execute_action(a);
#if CONFIG_BITENGINE_DIAG_SRP_INJECT
                bitengine_diag_mdns_action(bty, (uint32_t)(esp_timer_get_time() - bt0));
#endif
                MDNS_SERVICE_UNLOCK();"""),
    # ---- ⑥ 服务类型枚举（_services._dns-sd._udp）按类型去重 ----
    ("mdns_receive.c",
     """                mdns_srv_item_t *a = mdns_priv_get_services();
                while (a) {
                    mdns_parsed_question_t *question = (mdns_parsed_question_t *)mdns_mem_calloc(1, sizeof(mdns_parsed_question_t));""",
     """                mdns_srv_item_t *a = mdns_priv_get_services();
                while (a) {
                    /* bitEngine: scale — 每种（类型, 协议）只生成一个 SDPTR 问题（见 tools/patch_mdns.py SCALE ⑥） */
                    bool bitengine_seen = false;
                    for (mdns_parsed_question_t *pq = parsed_packet->questions; pq; pq = pq->next) {
                        if (pq->type == MDNS_TYPE_SDPTR && pq->service && pq->proto
                                && !strcasecmp(pq->service, a->service->service) && !strcasecmp(pq->proto, a->service->proto)) {
                            bitengine_seen = true;
                            break;
                        }
                    }
                    if (bitengine_seen) {
                        a = a->next;
                        continue;
                    }
                    mdns_parsed_question_t *question = (mdns_parsed_question_t *)mdns_mem_calloc(1, sizeof(mdns_parsed_question_t));"""),
    ("mdns_send.c",
     """static bool create_answer_from_service(mdns_tx_packet_t *packet, mdns_service_t *service,""",
     """/* bitEngine: scale — 同一种服务类型在线上就是同一条 "_services._dns-sd._udp PTR <类型>"：
   list 里已有同类型的 SDPTR 就不再加（见 tools/patch_mdns.py SCALE ⑥；mdns_pcb.c 的宣告追加也用它） */
bool bitengine_sdptr_dup(const mdns_out_answer_t *list, const mdns_service_t *service)
{
    for (const mdns_out_answer_t *d = list; d && service; d = d->next) {
        if (d->type == MDNS_TYPE_SDPTR && d->service && d->service->service && d->service->proto
                && service->service && service->proto
                && !strcasecmp(d->service->service, service->service) && !strcasecmp(d->service->proto, service->proto)) {
            return true;
        }
    }
    return false;
}

/* bitEngine: scale — 待宣告并进宣告包的快路径（tools/patch_mdns.py SCALE ①，审查 ⚠️5）。
   mdns_priv_create_answer 每加一条都把整条链表走一遍去重、再走到尾；一次并进 252 个服务约 170 万次 PSRAM 结点访问，
   持着服务锁。这里只做必要的去重：items 互不相同（待宣告按服务项去过重）、skip 里的已在包里；SDPTR 按（类型, 协议）、
   主机地址按主机各只放一条（含包里已有的）；用尾指针挂。代播主机没有某一族地址就不放那一族的记录 —— 写不出东西的
   记录会让多包派发误以为包满了、提前切包。内存不够返回 false，已挂上的都是合法记录，留着。 */
static bool bitengine_host_has_family(const mdns_host_item_t *host, uint8_t type)
{
    if (host == mdns_priv_get_self_host()) {
        return true;    /* 本机地址派发时现取 */
    }
    for (const mdns_ip_addr_t *a = host->address_list; a; a = a->next) {
        if (a->addr.type == type) {
            return true;
        }
    }
    return false;
}

static mdns_out_answer_t **bitengine_link(mdns_out_answer_t **tail, uint16_t type, mdns_service_t *service,
                                          mdns_host_item_t *host, bool flush)
{
    mdns_out_answer_t *a = (mdns_out_answer_t *)mdns_mem_malloc(sizeof(mdns_out_answer_t));
    if (!a) {
        HOOK_MALLOC_FAILED;
        return NULL;
    }
    memset(a, 0, sizeof(*a));
    a->type = type;
    a->service = service;
    a->host = host;
    a->flush = flush;
    *tail = a;
    return &a->next;
}

bool bitengine_append_services_fast(mdns_tx_packet_t *p, mdns_srv_item_t **items, size_t n,
                                    mdns_srv_item_t **skip, size_t skip_len)
{
    size_t have = 0;
    mdns_out_answer_t **tail = &p->answers;
    for (; *tail; tail = &(*tail)->next) {
        have++;
    }
    mdns_service_t **types = (mdns_service_t **)mdns_mem_malloc((have + n) * sizeof(*types));
    mdns_host_item_t **hosts = (mdns_host_item_t **)mdns_mem_malloc((have + n) * sizeof(*hosts));
    if (!types || !hosts) {
        HOOK_MALLOC_FAILED;
        mdns_mem_free(types);
        mdns_mem_free(hosts);
        return false;
    }
    size_t nt = 0, nh = 0, k;
    for (mdns_out_answer_t *a = p->answers; a; a = a->next) {
        if (a->type == MDNS_TYPE_SDPTR && a->service) {
            types[nt++] = a->service;
        } else if ((a->type == MDNS_TYPE_A || a->type == MDNS_TYPE_AAAA) && a->host) {
            for (k = 0; k < nh && hosts[k] != a->host; k++) {
            }
            if (k == nh) {
                hosts[nh++] = a->host;
            }
        }
    }
    bool ok = true;
    for (size_t i = 0; i < n && ok; i++) {
        mdns_service_t *s = items[i]->service;
        for (k = 0; k < skip_len && skip[k] != items[i]; k++) {
        }
        if (k < skip_len || !s->service || !s->proto) {
            continue;
        }
        for (k = 0; k < nt; k++) {
            if (!strcasecmp(types[k]->service, s->service) && !strcasecmp(types[k]->proto, s->proto)) {
                break;
            }
        }
        if (k == nt) {
            ok = (tail = bitengine_link(tail, MDNS_TYPE_SDPTR, s, NULL, false)) != NULL;
            if (ok) {
                types[nt++] = s;
            }
        }
        ok = ok && (tail = bitengine_link(tail, MDNS_TYPE_PTR, s, NULL, false)) != NULL
             && (tail = bitengine_link(tail, MDNS_TYPE_SRV, s, NULL, true)) != NULL
             && (tail = bitengine_link(tail, MDNS_TYPE_TXT, s, NULL, true)) != NULL;
        mdns_host_item_t *h = ok ? get_host_item(s->hostname) : NULL;
        if (h) {
            for (k = 0; k < nh && hosts[k] != h; k++) {
            }
            if (k == nh) {
                hosts[nh++] = h;
#ifdef CONFIG_LWIP_IPV4
                if (ok && bitengine_host_has_family(h, ESP_IPADDR_TYPE_V4)) {
                    ok = (tail = bitengine_link(tail, MDNS_TYPE_A, NULL, h, true)) != NULL;
                }
#endif
#ifdef CONFIG_LWIP_IPV6
                if (ok && bitengine_host_has_family(h, ESP_IPADDR_TYPE_V6)) {
                    ok = (tail = bitengine_link(tail, MDNS_TYPE_AAAA, NULL, h, true)) != NULL;
                }
#endif
            }
        }
    }
    mdns_mem_free(types);
    mdns_mem_free(hosts);
    return ok;
}

/* bitEngine: scale — 这个 PCB 的状态机包（探测 / 宣告）里有没有 service 的 SRV（审查 ❌1 的收回用） */
bool bitengine_srv_in_ctl_packets(mdns_if_t tcpip_if, mdns_ip_protocol_t ip_protocol, const mdns_service_t *service)
{
    for (mdns_tx_packet_t *q = s_tx_queue_head; q; q = q->next) {
        if (q->tcpip_if != tcpip_if || q->ip_protocol != ip_protocol || !q->bitengine_ctl) {
            continue;
        }
        mdns_out_answer_t *lists[3] = { q->answers, q->servers, q->additional };
        for (int l = 0; l < 3; l++) {
            for (mdns_out_answer_t *a = lists[l]; a; a = a->next) {
                if (a->type == MDNS_TYPE_SRV && a->service == service) {
                    return true;
                }
            }
        }
    }
    return false;
}

static bool create_answer_from_service(mdns_tx_packet_t *packet, mdns_service_t *service,"""),
    ("private_include/mdns_send.h",
     """#ifdef __cplusplus
}
#endif""",
     """/* bitEngine: scale — list 里已有与 service 同类型的 SDPTR（tools/patch_mdns.py SCALE ⑥） */
bool bitengine_sdptr_dup(const mdns_out_answer_t *list, const mdns_service_t *service);

/* bitEngine: scale — 待宣告并进宣告包的快路径；查某 PCB 的探测 / 宣告包里有没有某服务的 SRV（SCALE ①） */
bool bitengine_append_services_fast(mdns_tx_packet_t *p, mdns_srv_item_t **items, size_t n,
                                    mdns_srv_item_t **skip, size_t skip_len);
bool bitengine_srv_in_ctl_packets(mdns_if_t tcpip_if, mdns_ip_protocol_t ip_protocol, const mdns_service_t *service);

/* bitEngine: scale — 删代播主机前摘掉待发包里指向它的 A/AAAA（tools/patch_mdns.py SCALE ②） */
void bitengine_remove_scheduled_host_answers(mdns_host_item_t *host);

#ifdef __cplusplus
}
#endif"""),
    ("mdns_send.c",
     """    } else if (question->type == MDNS_TYPE_SDPTR) {
        if (!mdns_priv_create_answer(&packet->answers, MDNS_TYPE_SDPTR, service, NULL, false, false)) {
            return false;
        }
    }""",
     """    } else if (question->type == MDNS_TYPE_SDPTR) {
        if (!bitengine_sdptr_dup(packet->answers, service)   """ + SCALE_MARK + """
                && !mdns_priv_create_answer(&packet->answers, MDNS_TYPE_SDPTR, service, NULL, false, false)) {
            return false;
        }
    }"""),
    ("mdns_send.c",
     """    for (i = 0; i < len; i++) {
        if (!mdns_priv_create_answer(&packet->answers, MDNS_TYPE_SDPTR, services[i]->service, NULL, false, false)
                || !mdns_priv_create_answer(&packet->answers, MDNS_TYPE_PTR, services[i]->service, NULL, false, false)""",
     """    for (i = 0; i < len; i++) {
        if ((!bitengine_sdptr_dup(packet->answers, services[i]->service)   """ + SCALE_MARK + """
                && !mdns_priv_create_answer(&packet->answers, MDNS_TYPE_SDPTR, services[i]->service, NULL, false, false))
                || !mdns_priv_create_answer(&packet->answers, MDNS_TYPE_PTR, services[i]->service, NULL, false, false)"""),
    ("mdns_send.c",
     """        if (s->type == MDNS_TYPE_SRV) {
            if (!mdns_priv_create_answer(&packet->answers, MDNS_TYPE_SDPTR, s->service, NULL, false, false)
                    || !mdns_priv_create_answer(&packet->answers, MDNS_TYPE_PTR, s->service, NULL, false, false)""",
     """        if (s->type == MDNS_TYPE_SRV) {
            if ((!bitengine_sdptr_dup(packet->answers, s->service)   """ + SCALE_MARK + """
                    && !mdns_priv_create_answer(&packet->answers, MDNS_TYPE_SDPTR, s->service, NULL, false, false))
                    || !mdns_priv_create_answer(&packet->answers, MDNS_TYPE_PTR, s->service, NULL, false, false)"""),
]


# ── 第四组：本机地址只宣告以太网服务 IP（SVCIP，2026-10-07，TODO T9.34 路线 2）─────────────────────
# 上游 1.11.0 把预定义的 STA / ETH 配成"重复接口"：同网段时**地址小的口让位**（mdns_receive.c 的
# check_a_collision），应答的口在 A 记录里把两个口的地址都带上、自己的在前（mdns_send.c）。实抓：插着网线时只有
# WiFi 口应答、A 记录 [.36, .34] —— HA / ZHA / Z2M 添加网关时记下的是 WiFi 地址，一拔网线（.34 浮到 WiFi、
# 顶掉 .36）就连不上。改法：
#   ① 本机 A 记录的地址由 network_mgr 给（钩子 bitengine_mdns_self_ipv4）：FIXED = 只宣告以太网服务 IP（可以不在
#      本口上），HOLD = 开机还没定下来时 A / AAAA 都不给，DEFAULT = 上游行为。换地址时 network_mgr 触发一次宣告，
#      宣告里的 A 带缓存刷新位，旧地址在别人缓存里 1 s 内被清掉（RFC 6762 §10.2），所以不另做撤销。
#   ② 两口同网段时固定一口应答、另一口让位（mdns_priv_pcb_enable；上游按地址比较判出该应答的那口让位时换过来，
#      mdns_priv_pcb_set_duplicate；收到配对口自己的 IPv6 时按身份定，check_aaaa_collision）。两口宣告同一个服务 IP
#      后，上游靠地址不同来判重复的路就断了，必须按身份定。哪一口只在 bitengine_if_answers() 一处定，为以太网
#      （2026-10-07 用户拍板）：平时 mDNS（含 Thread / Matter 代播）走有线、不占与 Thread 分时的射频，WiFi 断连不再
#      让应答口来回切换；代价是台架上代播的发送口从 WiFi 改成以太网（台架上 WiFi 地址大，原来是 WiFi 应答）。
#   ③ 收到的 A 就是我们自己宣告的服务 IP 时认作自己的（check_a_collision）—— 否则本口真实地址比服务 IP 小时
#      会被判"对方赢"：轻则本口被标重复，探测中则主机名被改成 -2。
SVCIP_MARK = "/* bitEngine: svcip */"
SVCIP_PATCHES = [
    # ① 钩子声明（mdns_send.c / mdns_receive.c 都 include mdns_send.h）
    # ⚠️ 锚在上游原有的这条声明上，别锚在 SCALE 塞进来的那几行：SCALE 的替换文本以文件末尾的 #ifdef 块结尾，
    #    往它中间插东西会让 SCALE 认不出自己打过，重跑时再打一遍（2026-10-07 selftest 抓到）
    ("private_include/mdns_send.h",
     """int mdns_priv_append_one_txt_record_entry(uint8_t *packet, uint16_t *index, mdns_txt_linked_item_t *txt);
""",
     """int mdns_priv_append_one_txt_record_entry(uint8_t *packet, uint16_t *index, mdns_txt_linked_item_t *txt);

""" + SVCIP_MARK + """
/* 本机 A 记录用哪个地址（tools/patch_mdns.py SVCIP ①）。强定义在 components/network_mgr（以太网服务 IP 策略），
   mdns_send.c 里只有弱兜底（恒 DEFAULT = 上游行为）。只在 mdns 任务里调：实现不许拿锁、不许阻塞。 */
#define BITENGINE_SELF_DEFAULT  0   /* 上游行为：本口地址（+ 重复口的地址） */
#define BITENGINE_SELF_FIXED    1   /* 只宣告 *out 这一个 IPv4（服务 IP，可以不在本口上） */
#define BITENGINE_SELF_HOLD     2   /* 本机 A / AAAA 暂不给（开机还没定下服务 IP） */
int bitengine_mdns_self_ipv4(esp_netif_t *netif, esp_ip4_addr_t *out);
"""),
    # ① 弱兜底
    ("mdns_send.c",
     """static uint8_t append_answer(uint8_t *packet, uint16_t *index, mdns_out_answer_t *answer, mdns_if_t tcpip_if)
{""",
     SVCIP_MARK + """
/* 弱兜底：没有 network_mgr 的强定义时 = 上游行为（SVCIP ①）。链错了外部零症状，VERIFY 看 ELF 里是哪一个 */
__attribute__((weak)) int bitengine_mdns_self_ipv4(esp_netif_t *netif, esp_ip4_addr_t *out)
{
    (void)netif;
    (void)out;
    return BITENGINE_SELF_DEFAULT;
}

static uint8_t append_answer(uint8_t *packet, uint16_t *index, mdns_out_answer_t *answer, mdns_if_t tcpip_if)
{"""),
    # ① 本机 A 记录
    ("mdns_send.c",
     """            esp_netif_ip_info_t if_ip_info;
            if (!mdns_priv_if_ready(tcpip_if, MDNS_IP_PROTOCOL_V4) && !mdns_priv_pcb_is_duplicate(tcpip_if,
                                                                                                  MDNS_IP_PROTOCOL_V4)) {
                return 0;
            }
""",
     """            esp_netif_ip_info_t if_ip_info;
            if (!mdns_priv_if_ready(tcpip_if, MDNS_IP_PROTOCOL_V4) && !mdns_priv_pcb_is_duplicate(tcpip_if,
                                                                                                  MDNS_IP_PROTOCOL_V4)) {
                return 0;
            }
            """ + SVCIP_MARK + """
            {
                /* FIXED：只给服务 IP，不带本口、也不带重复口的地址；HOLD：这条不给（返回 0 = 跳过，多包补丁不当它满） */
                esp_ip4_addr_t bsv = { 0 };
                int bm = bitengine_mdns_self_ipv4(mdns_priv_get_esp_netif(tcpip_if), &bsv);
                if (bm == BITENGINE_SELF_HOLD) {
                    return 0;
                }
                if (bm == BITENGINE_SELF_FIXED) {
                    return append_a_record(packet, index, mdns_priv_get_global_hostname(), bsv.addr,
                                           answer->flush, answer->bye) > 0 ? 1 : 0;
                }
            }
"""),
    # ① 只宣告本机地址（不带服务）的那种宣告，把 A / AAAA 放进应答段：上游一律放权威段，这种包 ANCOUNT=0，
    #    不读权威段的解析器（bonjour-service 等）看不到新地址。RFC 6762 §8.3：宣告的记录在应答段。带服务的宣告照上游
    ("mdns_send.c",
     """    if (include_ip) {
        if (!mdns_priv_append_host_list_in_services(&packet->servers, services, len, true, false)) {
            mdns_priv_free_tx_packet(packet);
            return NULL;
        }
    }
    return packet;
}
""",
     """    if (include_ip) {
        """ + SVCIP_MARK + """
        mdns_out_answer_t **bdest = len ? &packet->servers : &packet->answers;   /* 只有本机地址时进应答段 */
        if (!mdns_priv_append_host_list_in_services(bdest, services, len, true, false)) {
            mdns_priv_free_tx_packet(packet);
            return NULL;
        }
    }
    return packet;
}
"""),
    # ① 本机 AAAA：HOLD 时也不给（否则 HA 会挑 IPv6 ULA 当连接地址）；其余照上游
    ("mdns_send.c",
     """            if (!mdns_priv_if_ready(tcpip_if, MDNS_IP_PROTOCOL_V6) && !mdns_priv_pcb_is_duplicate(tcpip_if,
                                                                                                  MDNS_IP_PROTOCOL_V6)) {
                return 0;
            }
""",
     """            if (!mdns_priv_if_ready(tcpip_if, MDNS_IP_PROTOCOL_V6) && !mdns_priv_pcb_is_duplicate(tcpip_if,
                                                                                                  MDNS_IP_PROTOCOL_V6)) {
                return 0;
            }
            """ + SVCIP_MARK + """
            {
                esp_ip4_addr_t bsv = { 0 };
                if (bitengine_mdns_self_ipv4(mdns_priv_get_esp_netif(tcpip_if), &bsv) == BITENGINE_SELF_HOLD) {
                    return 0;
                }
            }
"""),
    # ③ 收到自己宣告的服务 IP 不算冲突
    ("mdns_receive.c",
     """    esp_netif_ip_info_t if_ip_info;
    esp_netif_ip_info_t other_ip_info;
    if (!ip->addr) {
        return 1;//denial! they win
    }
""",
     """    esp_netif_ip_info_t if_ip_info;
    esp_netif_ip_info_t other_ip_info;
    if (!ip->addr) {
        return 1;//denial! they win
    }
    """ + SVCIP_MARK + """
    {
        /* 我们宣告出去的服务 IP（可以不在本口上）是自己的：本口真实地址比它小时，上游会判"对方赢"——
           轻则本口被标重复，探测中则主机名被改成 -2 */
        esp_ip4_addr_t bsv = { 0 };
        if (bitengine_mdns_self_ipv4(mdns_priv_get_esp_netif(tcpip_if), &bsv) == BITENGINE_SELF_FIXED &&
                bsv.addr == ip->addr) {
            return 0;
        }
    }
"""),
    # ② 收到配对口自己的 IPv6：按身份定谁让位、跳过这个包剩下的记录（2026-10-07 审计 ❌）
    ("mdns_receive.c",
     """    struct esp_ip6_addr if_ip6;
    struct esp_ip6_addr other_ip6;
    if (mdns_utils_ipv6_address_is_zero(*ip)) {
        return 1;//denial! they win
    }
""",
     """    struct esp_ip6_addr if_ip6;
    struct esp_ip6_addr other_ip6;
    if (mdns_utils_ipv6_address_is_zero(*ip)) {
        return 1;//denial! they win
    }
    """ + SVCIP_MARK + """
    {
        /* 配对口（同网段的另一口）自己的 IPv6 —— 是我们自己，不是冲突：按身份定谁让位（set_duplicate 会换成让
           不该应答的那口让位），跳过这个包剩下的记录。不能落到下面"本口取不到链路本地地址就判对方赢"：插回网线时
           以太网的链路本地地址还在 DAD，而本机 A 记录两口一样、被 SVCIP ③ 放过，于是这条会在探测中把主机名改成 -2 */
        mdns_if_t bo = mdns_priv_netif_get_other_interface(tcpip_if);
        esp_netif_t *bn = (bo != MDNS_MAX_INTERFACES) ? mdns_priv_get_esp_netif(bo) : NULL;
        if (bn) {
            struct esp_ip6_addr bos[NETIF_IPV6_MAX_NUMS];
            int bc = esp_netif_get_all_ip6(bn, bos);
            for (int k = 0; k < bc && k < NETIF_IPV6_MAX_NUMS; k++) {
                if (!memcmp((uint8_t *)bos[k].addr, (uint8_t *)ip->addr, MDNS_UTILS_SIZEOF_IP6_ADDR)) {
                    mdns_priv_pcb_set_duplicate(tcpip_if);
                    return 2;
                }
            }
        }
    }
"""),
    # ② 识别预定义 STA
    ("mdns_netif.c",
     """mdns_if_t mdns_priv_netif_get_other_interface(mdns_if_t tcpip_if)
{
    if (tcpip_if < MDNS_MAX_INTERFACES) {
        return s_esp_netifs[tcpip_if].duplicate;
    }
    return MDNS_MAX_INTERFACES;
}
""",
     """mdns_if_t mdns_priv_netif_get_other_interface(mdns_if_t tcpip_if)
{
    if (tcpip_if < MDNS_MAX_INTERFACES) {
        return s_esp_netifs[tcpip_if].duplicate;
    }
    return MDNS_MAX_INTERFACES;
}

""" + SVCIP_MARK + """
/* 这个口是不是"配对时固定由它应答"的那一口（SVCIP ②，见 mdns_pcb.c / mdns_receive.c）。
   选哪一口只改这里：以太网（2026-10-07 用户拍板）—— 平时走有线，不占与 Thread 分时共用的射频；
   WiFi 断连不再让应答口来回切换，只在拔插网线时切（那时 WiFi 换地址本来就要整轮重新宣告）。 */
bool bitengine_if_answers(mdns_if_t tcpip_if)
{
    return tcpip_if < MDNS_MAX_INTERFACES && s_esp_netifs[tcpip_if].predefined &&
           s_esp_netifs[tcpip_if].predef_if == MDNS_IF_ETH;
}
"""),
    ("private_include/mdns_netif.h",
     """mdns_if_t mdns_priv_netif_get_other_interface(mdns_if_t tcpip_if);
""",
     """mdns_if_t mdns_priv_netif_get_other_interface(mdns_if_t tcpip_if);

""" + SVCIP_MARK + """
bool bitengine_if_answers(mdns_if_t tcpip_if);
"""),
    # ② 打开一个口时按身份定谁让位
    ("mdns_pcb.c",
     """/**
 * @brief  Enable mDNS interface
 */
void mdns_priv_pcb_enable(mdns_if_t tcpip_if, mdns_ip_protocol_t ip_protocol)
{
""",
     SVCIP_MARK + """
/* 成对的两个口此刻都有 IPv4 且在同一网段（IPv6 的 PCB 也按这个定：同网段 = 同一个二层） */
static bool bitengine_pair_same_subnet(mdns_if_t a, mdns_if_t b)
{
    esp_netif_ip_info_t ia;
    esp_netif_ip_info_t ib;
    esp_netif_t *na = mdns_priv_get_esp_netif(a);
    esp_netif_t *nb = mdns_priv_get_esp_netif(b);
    if (!na || !nb || esp_netif_get_ip_info(na, &ia) != ESP_OK || esp_netif_get_ip_info(nb, &ib) != ESP_OK) {
        return false;
    }
    if (!ia.ip.addr || !ib.ip.addr || ia.netmask.addr != ib.netmask.addr) {
        return false;
    }
    return (ia.ip.addr & ia.netmask.addr) == (ib.ip.addr & ib.netmask.addr);
}

/**
 * @brief  Enable mDNS interface
 */
void mdns_priv_pcb_enable(mdns_if_t tcpip_if, mdns_ip_protocol_t ip_protocol)
{
    """ + SVCIP_MARK + """
    /* 两口同网段时按身份定谁应答（SVCIP ②，哪一口见 bitengine_if_answers）：不该应答的那口要上而另一口在应答 ->
       直接记为重复；该应答的那口要上而另一口在应答 -> 先把自己打开，**打开成功之后**再让另一口退成重复（反过来的话
       本口初始化失败 —— 如 IGMP/MLD 组池满 —— 就两口都哑，且上游 mdns_priv_pcb_disable 只在本口就绪时才重开另一口，
       永远回不来；2026-10-07 审计）。该应答的那口关掉时，上游会把记为重复的另一口重新打开，那时它已不在，这里不拦。
       两口不同网段（或一边还没 IPv4）时照上游。 */
    mdns_if_t b_demote = MDNS_MAX_INTERFACES;
    {
        mdns_if_t bo = mdns_priv_netif_get_other_interface(tcpip_if);
        if (bo != MDNS_MAX_INTERFACES && mdns_priv_if_ready(bo, ip_protocol) &&
                bitengine_pair_same_subnet(tcpip_if, bo)) {
            if (!bitengine_if_answers(tcpip_if)) {
                if (mdns_priv_if_ready(tcpip_if, ip_protocol)) {
                    mdns_priv_clear_tx_queue_if(tcpip_if, ip_protocol);
                    deinit_pcb(tcpip_if, ip_protocol);
                }
                s_pcbs[tcpip_if][ip_protocol].state = PCB_DUP;
                return;
            }
            b_demote = bo;
        }
    }
"""),
    # ② 上游判出"STA 让位"时换成 ETH 让位
    ("mdns_pcb.c",
     """    restart_pcb(tcpip_if, ip_protocol);
}

/**
 * @brief  Set interface as duplicate if another is found on the same subnet
 */""",
     """    restart_pcb(tcpip_if, ip_protocol);
    """ + SVCIP_MARK + """
    if (b_demote != MDNS_MAX_INTERFACES && mdns_priv_if_ready(tcpip_if, ip_protocol)) {
        /* 本口已经起来了，这才让另一口退成重复（见本函数开头） */
        mdns_priv_clear_tx_queue_if(b_demote, ip_protocol);
        deinit_pcb(b_demote, ip_protocol);
        s_pcbs[b_demote][ip_protocol].state = PCB_DUP;
    }
}

/**
 * @brief  Set interface as duplicate if another is found on the same subnet
 */"""),
    ("mdns_pcb.c",
     """    mdns_if_t other_if = mdns_priv_netif_get_other_interface(tcpip_if);
    if (other_if == MDNS_MAX_INTERFACES) {
        return; // no other interface found
    }
""",
     """    mdns_if_t other_if = mdns_priv_netif_get_other_interface(tcpip_if);
    if (other_if == MDNS_MAX_INTERFACES) {
        return; // no other interface found
    }
    """ + SVCIP_MARK + """
    if (bitengine_if_answers(tcpip_if)) {
        /* 让位的固定是不该应答的那一口（SVCIP ②）：上游按地址比较判出该应答的那口让位时，换过来 */
        mdns_if_t t = tcpip_if;
        tcpip_if = other_if;
        other_if = t;
    }
"""),
]


def apply_overflow_patches(root: pathlib.Path, check_only: bool, patches=None, what=">255 服务") -> int:
    """每条锚点各自判断：替换文本已在 → 打过了；原文恰好一处 → 打；否则报错停构建。
    patches 默认是 OVERFLOW_PATCHES；SCALE_PATCHES 的锚点写在它之后的原文上，所以必须后打。"""
    rc = 0
    by_file = {}
    for rel, anchor, repl in (OVERFLOW_PATCHES if patches is None else patches):
        by_file.setdefault(rel, []).append((anchor, repl))
    for rel, items in by_file.items():
        src = root / rel
        body = src.read_text(encoding="utf-8")
        changed = False
        for anchor, repl in items:
            if repl in body:
                continue
            if check_only:
                print(f"patch_mdns: ❌ {rel} 缺 {what} 补丁：{anchor.strip().splitlines()[-1].strip()[:60]}",
                      file=sys.stderr)
                rc = 1
                continue
            n = body.count(anchor)
            if n != 1:
                print(f"patch_mdns: ❌ {rel} 里 {what} 补丁的锚点出现 {n} 次（应当恰好 1 次）：\n"
                      f"            {anchor.strip().splitlines()[-1].strip()[:80]}\n"
                      "            上游改了这段代码，请重新核对后更新 OVERFLOW_PATCHES。",
                      file=sys.stderr)
                rc = 1
                continue
            body = body.replace(anchor, repl, 1)
            changed = True
        if changed:
            src.write_text(body, encoding="utf-8")
            print(f"patch_mdns: ✅ {rel} 已应用 {what} 补丁")
    return rc


def apply_all(root: pathlib.Path, check_only: bool) -> int:
    """四组补丁按顺序打（或只检查）。返回 0 = 都在 / 都打上了。"""
    src = root / "mdns_send.c"
    if not src.is_file():
        # 组件还没被解析下来（首次 configure 的早期）——不是错误，下次会再跑
        print(f"patch_mdns: {src} 还不存在，跳过")
        return 0

    # 版本核对：上游换版本时这段代码大概率变了，宁可停下来也别打歪
    idf_yml = root / "idf_component.yml"
    if idf_yml.is_file():
        txt = idf_yml.read_text(encoding="utf-8", errors="replace")
        if f"version: {EXPECT_VERSION}" not in txt and f'version: "{EXPECT_VERSION}"' not in txt:
            print(f"patch_mdns: ⚠️ 组件版本不是预期的 {EXPECT_VERSION}，"
                  f"请重新核对 mdns_send.c 的 dispatch_tx_packet 结构后再更新本脚本",
                  file=sys.stderr)
            return 1

    body = src.read_text(encoding="utf-8")

    if MARK in body:
        print("patch_mdns: 多包补丁已打过，跳过")
    elif check_only:
        print("patch_mdns: ❌ 多包补丁未生效", file=sys.stderr)
        return 1
    elif ANCHOR not in body:
        print("patch_mdns: ❌ 找不到 mdns_priv_dispatch_tx_packet 的预期原文。\n"
              "            上游改了这段代码。请重新核对后更新 tools/patch_mdns.py 的 ANCHOR，\n"
              "            不要绕过这个检查 —— 打歪的后果是 mDNS 应答损坏。",
              file=sys.stderr)
        return 1
    else:
        src.write_text(body.replace(ANCHOR, REPLACEMENT, 1), encoding="utf-8")
        print("patch_mdns: ✅ 已应用多包应答补丁")

    rc = apply_overflow_patches(root, check_only)
    if rc:
        return rc
    rc = apply_overflow_patches(root, check_only, SCALE_PATCHES, "规模（代播不探测 / 包完整 / 背压）")
    if rc:
        return rc
    rc = apply_overflow_patches(root, check_only, SVCIP_PATCHES, "服务地址（svcip）")
    if rc or not check_only:
        return rc
    return check_invariants(root)


# ── 打完之后的不变量（--check 与 --selftest 都会查）──────────────────────────────────────
# --check 只证明"补丁文本在"；这几条钉的是设计本身，改补丁时最容易顺手改坏、且出事时外部零症状：
INVARIANTS = [
    # (文件, 函数, 种类, 记号...)  种类：absent = 函数体里不许出现；order = 都要在，且首次出现的位置依次递增
    # 对抗性审查 ❌2：起一轮宣告若经 init_pcb_probe，会先 clear_tx_queue_if，把还没发完的宣告包清掉
    ("mdns_pcb.c", "bitengine_pend_kick", "absent", "mdns_priv_clear_tx_queue_if", "mdns_priv_init_pcb_probe",
     "mdns_priv_probe_all_pcbs"),
    ("mdns_pcb.c", "bitengine_pend_settle", "absent", "mdns_priv_clear_tx_queue_if", "mdns_priv_init_pcb_probe",
     "mdns_priv_probe_all_pcbs"),
    # 代播服务先分拣、没东西可探就在清发送队列之前返回 —— 否则每条代播注册都会清掉正在发的宣告
    ("mdns_pcb.c", "mdns_priv_init_pcb_probe", "order", "bitengine_pend_add", "bitengine_pend_settle", "return",
     "mdns_priv_clear_tx_queue_if"),
    # 待宣告存的是服务项指针：删服务必须先摘，再动发送队列 / 释放
    ("mdns_send.c", "mdns_priv_remove_scheduled_service_packets", "order", "mdns_priv_pcb_pend_forget",
     "s_tx_queue_head"),
    ("mdns_pcb.c", "deinit_pcb", "order", "bitengine_pend_clear"),
    # ⑨：应答包不许推进状态机 —— 标记判断要在 switch 之前
    ("mdns_pcb.c", "mdns_priv_pcb_schedule_tx_packet", "order", "bitengine_ctl", "switch"),
    # 进状态机的包都要带标记（起宣告的两处）
    ("mdns_pcb.c", "bitengine_pend_kick", "order", "bitengine_ctl", "mdns_priv_send_after"),
    ("mdns_pcb.c", "init_probe_new_service", "order", "bitengine_ctl", "mdns_priv_send_after"),
    ("mdns_pcb.c", "mdns_priv_pcb_announce", "order", "p->bitengine_ctl = 1", "mdns_priv_send_after"),
    # 审查 ⚠️4：宣告态分支不往正在发的包里倒待宣告 —— 待宣告只在 RUNNING 分支（建新包之后）并进去
    ("mdns_pcb.c", "mdns_priv_pcb_announce", "order", "mdns_priv_create_announce_packet", "bitengine_pend_append"),
    # 审查 ❌1：清发送队列之前先把在途的代播宣告收回待宣告
    ("mdns_pcb.c", "mdns_priv_init_pcb_probe", "order", "bitengine_pend_rescue", "mdns_priv_clear_tx_queue_if"),
    # ⑧：取下一个动作之前先看连续干了多久，超了就睡一拍（放在 xQueueReceive 之后就等于每次都先干完再说）
    ("mdns_service.c", "service_task", "order", "uxQueueMessagesWaiting", "bitengine_idle_last_us", "vTaskDelay(1)", "xQueueReceive"),
    # SVCIP ①：本机 A 记录先问钩子，再取本口地址（FIXED 时根本不该走到本口 / 重复口的地址）
    ("mdns_send.c", "append_answer", "order", "bitengine_mdns_self_ipv4", "esp_netif_get_ip_info"),
    # SVCIP ③：收到的 A 先认"是不是我们宣告的服务 IP"，再做上游的地址比较（反过来就可能把主机名改成 -2）
    ("mdns_receive.c", "check_a_collision", "order", "bitengine_mdns_self_ipv4", "memcmp"),
    # SVCIP ②：打开口之前先按身份定谁让位；上游判出 STA 让位时先换成 ETH，再进循环
    ("mdns_pcb.c", "mdns_priv_pcb_enable", "order", "bitengine_pair_same_subnet", "restart_pcb"),
    ("mdns_pcb.c", "mdns_priv_pcb_set_duplicate", "order", "bitengine_if_answers", "for"),
    # SVCIP ②（审计 ❌1.5）：先把本口打开，再让另一口退成重复 —— 降级必须在 restart_pcb 之后
    ("mdns_pcb.c", "mdns_priv_pcb_enable", "order", "restart_pcb", "deinit_pcb(b_demote"),
    # SVCIP（审计 ❌1.4）：配对口的 IPv6 要在"本口取不到链路本地地址就判对方赢"之前认出来
    ("mdns_receive.c", "check_aaaa_collision", "order", "esp_netif_get_all_ip6", "esp_netif_get_ip6_linklocal"),
    # 审查 ❌2：SDPTR 的属主名写不进去就停，不能接着写类型字段
    ("mdns_send.c", "append_sdptr_record", "order", "append_fqdn", "!part_length", "append_type"),
    # 待发包里的 A/AAAA 存的是主机指针：摘掉之后才能释放主机
    ("mdns_responder.c", "delegate_hostname_remove", "order", "bitengine_remove_scheduled_host_answers",
     "mdns_mem_free(host)"),
    # 对抗性审查 ❌1：改实例名必须走上游的重拼（探测包里的问题借用的是旧名字），不能改成直接宣告
    ("mdns_responder.c", "mdns_service_instance_name_set_for_host", "order", "mdns_priv_probe_all_pcbs"),
    ("mdns_responder.c", "mdns_service_instance_name_set_for_host", "absent", "announce_all_pcbs"),
]


def strip_c(src: str) -> str:
    """注释与字符串 / 字符字面量的内容换成空格（换行保留），给结构检查用。"""
    out, i, n, state = [], 0, len(src), None
    while i < n:
        c = src[i]
        if state is None:
            if src.startswith("/*", i):
                state = "block"; out.append("  "); i += 2; continue
            if src.startswith("//", i):
                state = "line"; out.append("  "); i += 2; continue
            if c in "\"'":
                state = c
            out.append(c)
        elif state == "block":
            if src.startswith("*/", i):
                state = None; out.append("  "); i += 2; continue
            out.append("\n" if c == "\n" else " ")
        elif state == "line":
            if c == "\n":
                state = None; out.append(c)
            else:
                out.append(" ")
        else:
            if c == "\\" and i + 1 < n:
                out.append("  "); i += 2; continue
            if c == state or c == "\n":
                state = None; out.append(c)
            else:
                out.append(" ")
        i += 1
    return "".join(out)


def c_func_body(src: str, name: str):
    """C 函数 name 的函数体（从 { 到配对的 }）；只认定义（参数表后紧跟 {）。src 应先过 strip_c。"""
    import re
    m = re.search(r"\b" + re.escape(name) + r"\s*\([^;{)]*(?:\([^)]*\)[^;{)]*)*\)\s*\{", src)
    if not m:
        return None
    i = m.end() - 1
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    return None


def check_invariants(root: pathlib.Path) -> int:
    rc = 0
    cache = {}
    for rel, fn, kind, *toks in INVARIANTS:
        if rel not in cache:
            cache[rel] = strip_c((root / rel).read_text(encoding="utf-8"))
        body = c_func_body(cache[rel], fn)
        if body is None:
            print(f"patch_mdns: ❌ 不变量：{rel} 里找不到函数 {fn}", file=sys.stderr)
            rc = 1
            continue
        if kind == "absent":
            bad = [t for t in toks if t in body]
            if bad:
                print(f"patch_mdns: ❌ 不变量：{fn} 里不该出现 {'、'.join(bad)}", file=sys.stderr)
                rc = 1
        else:
            # 按**首次出现**的位置比：只要后一个记号在前一个之前出现过一次就算违反（"先释放、再摘"那种）
            first = [body.find(t) for t in toks]
            if -1 in first or first != sorted(first):
                print(f"patch_mdns: ❌ 不变量：{fn} 里 {'→'.join(toks)} 的先后不成立（首次出现位置 {first}）",
                      file=sys.stderr)
                rc = 1
    return rc


def cjk_outside_comments(src: str) -> list:
    """去掉 C 注释与字符串 / 字符字面量后，还带中文的行号。补丁里的说明只该出现在注释里。"""
    out, i, n, line, state = set(), 0, len(src), 1, None
    while i < n:
        c = src[i]
        if c == "\n":
            line += 1
        if state is None:
            if src.startswith("/*", i):
                state, i = "block", i + 2
                continue
            if src.startswith("//", i):
                state = "line"
            elif c in "\"'":
                state = c
            elif "\u2e80" <= c <= "\u9fff" or "\uff00" <= c <= "\uffef":
                out.add(line)
        elif state == "block":
            if src.startswith("*/", i):
                state, i = None, i + 2
                continue
        elif state == "line":
            if c == "\n":
                state = None
        else:                                   # 字符串 / 字符字面量
            if c == "\\":
                i += 2
                continue
            if c == state or c == "\n":
                state = None
        i += 1
    return sorted(out)


def selftest(pristine: pathlib.Path) -> int:
    """在原版组件的副本上：打一遍（必须成功）→ 再打一遍（必须一个字节都不改 = 幂等）→ --check（必须全在）。
    抓的是"后一组补丁改到了前一组的替换文本"这类事故：那样第一遍照样成功，重跑时前一组找不到锚点、构建停下
    （2026-10-07 栽过一次：SCALE ④ 改到了 OVERFLOW 那条的 for 循环体）。"""
    import hashlib, shutil, tempfile

    def digest(d: pathlib.Path) -> dict:
        return {str(f.relative_to(d)): hashlib.sha256(f.read_bytes()).hexdigest()
                for f in sorted(d.rglob("*")) if f.is_file()}

    with tempfile.TemporaryDirectory() as t:
        work = pathlib.Path(t) / "mdns"
        shutil.copytree(pristine, work)
        if apply_all(work, False):
            print("patch_mdns selftest: ❌ 原版上第一遍就没打上", file=sys.stderr)
            return 1
        before = digest(work)
        if apply_all(work, False) or digest(work) != before:
            print("patch_mdns selftest: ❌ 第二遍改了东西 / 失败 —— 补丁不幂等（多半是两条补丁的文本重叠）", file=sys.stderr)
            return 1
        if apply_all(work, True):
            print("patch_mdns selftest: ❌ 打完 --check 不过", file=sys.stderr)
            return 1
        stray = [f"{rel}:{ln}" for rel in sorted({r for r, _, _ in OVERFLOW_PATCHES + SCALE_PATCHES + SVCIP_PATCHES} | {"mdns_send.c"})
                 for ln in cjk_outside_comments((work / rel).read_text(encoding="utf-8"))]
        if stray:
            print("patch_mdns selftest: ❌ 注释 / 字符串之外出现了中文（多半是说明写到了 */ 外面）：" + "、".join(stray[:5]),
                  file=sys.stderr)
            return 1
    print("patch_mdns selftest: ✅ 原版上打一遍成功、再打一遍不变、--check 全在")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--component-dir", help="managed_components/espressif__mdns 的路径")
    ap.add_argument("--check", action="store_true",
                    help="只检查是否已打，不修改（供 CI/VERIFY 用）")
    ap.add_argument("--selftest", metavar="PRISTINE_DIR",
                    help="在原版组件（如组件管理器缓存里那份）的副本上验证：能打上、幂等、--check 全在")
    args = ap.parse_args()
    if args.selftest:
        return selftest(pathlib.Path(args.selftest))
    if not args.component_dir:
        ap.error("要 --component-dir（或 --selftest）")
    return apply_all(pathlib.Path(args.component_dir), args.check)


if __name__ == "__main__":
    sys.exit(main())
