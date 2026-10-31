/* hub_meta_core 的真值表（docs/15 设计 6.1 元数据、附录 A hub.device.update；M5 第二步）。编的是产品源文件 ../hub_meta_core.c。
 *   M1 id 解析   M2 改名字 / 房间 / 推送开关、净化与拒收、敏感位不被碰   M3 占位与腾位   M4 表满、有页读不出来时不新建、读不出来的页不改
 *   M5 分页序列化来回一致、结构坏整页拒收且不动原表、单条名字不合格只丢那条   M6 开机读页判定   M7 跨页去重   M8 事件句子里的设备名 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hub_meta_core.h"

static int s_fail, s_pass;
#define CHECK(cond, ...) do { if (cond) { s_pass++; } else { s_fail++; printf("❌ %s:%d ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static hub_meta_core_t M, M2;
static const uint32_t ALL = (1u << HUB_META_PAGES) - 1u;

int main(void)
{
    uint8_t proto, id[8];
    int slot;
    /* ---- M1 ---- */
    CHECK(hub_meta_parse_id("zb-b40e060fffe26179", &proto, id) && proto == HUB_META_PROTO_ZIGBEE && id[0] == 0xB4 && id[7] == 0x79, "M1 zb- 解析、高字节在前");
    CHECK(hub_meta_parse_id("MT-00000000000000AB", &proto, id) && proto == HUB_META_PROTO_MATTER && id[7] == 0xAB, "M1 mt- 大写也认");
    CHECK(!hub_meta_parse_id("th-0011223344556677", &proto, id) && !hub_meta_parse_id("zb-00112233445566", &proto, id) &&
          !hub_meta_parse_id("zb-00112233445566zz", &proto, id) && !hub_meta_parse_id("zb_0011223344556677", &proto, id) && !hub_meta_parse_id(NULL, &proto, id),
          "M1 Thread 成员 / 长度不对 / 非十六进制 / 分隔符不对 / NULL：拒");
    /* ---- M2 ---- */
    hub_meta_core_init(&M);
    hub_meta_parse_id("zb-b40e060fffe26179", &proto, id);
    CHECK(hub_meta_apply(&M, proto, id, &(hub_meta_change_t){ "Hall motion", "Hall", 1, HUB_META_SENS_KEEP }, ALL, &slot) == HUB_META_OK && slot == 0 &&
          !strcmp(M.e[0].name, "Hall motion") && !strcmp(M.e[0].room, "Hall") && (M.e[0].flags & HUB_META_F_NOTIFY), "M2 名字 + 房间 + 推送");
    CHECK(hub_meta_apply(&M, proto, id, &(hub_meta_change_t){ NULL, NULL, 0, HUB_META_SENS_KEEP }, ALL, &slot) == HUB_META_OK && slot == 0 && !(M.e[0].flags & HUB_META_F_NOTIFY) &&
          !strcmp(M.e[0].name, "Hall motion"), "M2 只关推送：名字房间不动");
    CHECK(hub_meta_apply(&M, proto, id, &(hub_meta_change_t){ "Line1\nLine2", NULL, -1, HUB_META_SENS_KEEP }, ALL, &slot) == HUB_META_NAME_NEWLINE && slot == -1 &&
          !strcmp(M.e[0].name, "Hall motion"), "M2 名字带换行：拒、原值不动");
    CHECK(hub_meta_apply(&M, proto, id, &(hub_meta_change_t){ "123456789012345678901234567890123", NULL, -1, HUB_META_SENS_KEEP }, ALL, &slot) == HUB_META_NAME_LONG, "M2 名字 33 B：拒");
    CHECK(hub_meta_apply(&M, proto, id, &(hub_meta_change_t){ NULL, "1234567890123456789012345", -1, HUB_META_SENS_KEEP }, ALL, &slot) == HUB_META_ROOM_LONG, "M2 房间 25 B：拒");
    CHECK(hub_meta_apply(&M, proto, id, &(hub_meta_change_t){ NULL, "Ki\r\ntchen", -1, HUB_META_SENS_KEEP }, ALL, &slot) == HUB_META_ROOM_NEWLINE, "M2 房间带换行：拒");
    CHECK(hub_meta_apply(&M, proto, id, &(hub_meta_change_t){ "  Front\tdoor\x01  ", NULL, -1, HUB_META_SENS_KEEP }, ALL, &slot) == HUB_META_OK && !strcmp(M.e[0].name, "Front door"),
          "M2 名字净化：去控制字符、压空白、去首尾（得到 \"%s\"）", M.e[0].name);
    CHECK(hub_meta_apply(&M, proto, id, &(hub_meta_change_t){ "客厅灯", NULL, -1, HUB_META_SENS_KEEP }, ALL, &slot) == HUB_META_OK && !strcmp(M.e[0].name, "客厅灯"), "M2 中文名字");
    /* 敏感标记（6.2：只有本地接口设；全零 = 不改，所以 hub.device.update 那样只给前三项的碰不到它） */
    CHECK(hub_meta_apply(&M, proto, id, &(hub_meta_change_t){ NULL, NULL, -1, HUB_META_SENS_ON }, ALL, &slot) == HUB_META_OK && (M.e[slot].flags & HUB_META_F_SENSITIVE),
          "M2s 本地设敏感：标上");
    CHECK(hub_meta_apply(&M, proto, id, &(hub_meta_change_t){ "Renamed", NULL, 0, HUB_META_SENS_KEEP }, ALL, &slot) == HUB_META_OK && (M.e[slot].flags & HUB_META_F_SENSITIVE) &&
          !(M.e[slot].flags & HUB_META_F_NOTIFY), "M2s 只给名字 / notify 的修改（Muse 的 hub.device.update 形状）：敏感标记不动");
    CHECK(hub_meta_apply(&M, proto, id, &(hub_meta_change_t){ NULL, NULL, -1, 7 }, ALL, &slot) == HUB_META_NOTHING && (M.e[0].flags & HUB_META_F_SENSITIVE),
          "M2s 越界的操作码：拒、不动");
    CHECK(hub_meta_apply(&M, proto, id, &(hub_meta_change_t){ NULL, NULL, -1, HUB_META_SENS_OFF }, ALL, &slot) == HUB_META_OK && !(M.e[slot].flags & HUB_META_F_SENSITIVE),
          "M2s 本地清敏感：去掉");
    {
        static const uint8_t ids[8] = { 0x5E, 0x45, 1, 2, 3, 4, 5, 6 };
        CHECK(hub_meta_apply(&M, proto, ids, &(hub_meta_change_t){ NULL, NULL, -1, HUB_META_SENS_ON }, ALL, &slot) == HUB_META_OK && slot >= 0 &&
              (M.e[slot].flags & HUB_META_F_SENSITIVE), "M2s 没有条目的设备设敏感：建条目");
        CHECK(hub_meta_apply(&M, proto, ids, &(hub_meta_change_t){ NULL, NULL, -1, HUB_META_SENS_OFF }, ALL, &slot) == HUB_META_OK && hub_meta_find(&M, proto, ids) < 0,
              "M2s 清掉之后回到默认：腾出条目");
    }
    hub_meta_apply(&M, proto, id, &(hub_meta_change_t){ "客厅灯", NULL, -1, HUB_META_SENS_KEEP }, ALL, &slot);
    CHECK(hub_meta_apply(&M, proto, id, &(hub_meta_change_t){ NULL, NULL, -1, HUB_META_SENS_KEEP }, ALL, &slot) == HUB_META_NOTHING, "M2 什么都没给：NOTHING");
    CHECK(hub_meta_apply(&M, 3, id, &(hub_meta_change_t){ "x", NULL, -1, HUB_META_SENS_KEEP }, ALL, &slot) == HUB_META_BAD_ID, "M2 协议不认识：BAD_ID");
    M.e[0].flags |= HUB_META_F_SENSITIVE;          /* 网页设的敏感标记（6.2）：update 只碰 notify 位 */
    CHECK(hub_meta_apply(&M, proto, id, &(hub_meta_change_t){ NULL, NULL, 1, HUB_META_SENS_KEEP }, ALL, &slot) == HUB_META_OK &&
          M.e[0].flags == (HUB_META_F_SENSITIVE | HUB_META_F_NOTIFY), "M2 开 notify：敏感位还在");
    CHECK(hub_meta_apply(&M, proto, id, &(hub_meta_change_t){ "", "", 0, HUB_META_SENS_KEEP }, ALL, &slot) == HUB_META_OK && hub_meta_find(&M, proto, id) == 0 &&
          M.e[0].flags == HUB_META_F_SENSITIVE, "M2 名字 / 房间 / notify 全清：敏感位还在、条目不腾位");
    M.e[0].flags = 0;
    hub_meta_apply(&M, proto, id, &(hub_meta_change_t){ "Front door", NULL, -1, HUB_META_SENS_KEEP }, ALL, &slot);
    /* ---- M3 ---- */
    CHECK(hub_meta_apply(&M, proto, id, &(hub_meta_change_t){ "", "", 0, HUB_META_SENS_KEEP }, ALL, &slot) == HUB_META_OK && slot == 0 && M.e[0].proto == HUB_META_PROTO_NONE &&
          hub_meta_find(&M, proto, id) < 0, "M3 三样都清空：腾出位置（slot 仍给出，那一页要重写）");
    uint8_t id2[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    CHECK(hub_meta_apply(&M, HUB_META_PROTO_ZIGBEE, id2, &(hub_meta_change_t){ "", NULL, 0, HUB_META_SENS_KEEP }, ALL, &slot) == HUB_META_OK && slot == -1 && hub_meta_find(&M, 1, id2) < 0,
          "M3 不存在的设成默认值：不占位、没有改动");
    CHECK(hub_meta_apply(&M, HUB_META_PROTO_MATTER, id2, &(hub_meta_change_t){ NULL, NULL, 1, HUB_META_SENS_KEEP }, ALL, &slot) == HUB_META_OK && hub_meta_find(&M, 2, id2) == slot &&
          hub_meta_find(&M, 1, id2) < 0, "M3 同样 8 B、不同协议是两台");
    /* ---- M4 ---- */
    hub_meta_core_init(&M);
    for (unsigned k = 0; k < HUB_META_MAX; k++) {
        uint8_t x[8] = { 0, 0, 0, 0, 0, 0, (uint8_t)(k >> 8), (uint8_t)k };
        hub_meta_apply(&M, HUB_META_PROTO_ZIGBEE, x, &(hub_meta_change_t){ "n", NULL, -1, HUB_META_SENS_KEEP }, ALL, &slot);
    }
    uint8_t xo[8] = { 9, 9, 9, 9, 9, 9, 9, 9 };
    CHECK(hub_meta_apply(&M, HUB_META_PROTO_ZIGBEE, xo, &(hub_meta_change_t){ "n", NULL, -1, HUB_META_SENS_KEEP }, ALL, &slot) == HUB_META_FULL && slot == -1, "M4 %u 条满了：FULL", HUB_META_MAX);
    hub_meta_core_init(&M);
    /* 第 2 页读不出来：它上面可能就有这台设备 —— 不新建（两轮审计 ❌：在别的页另建一条，重启后同一 id 两条、旧值遮住新值） */
    const uint32_t P2_BAD = ALL & ~0x4u;
    CHECK(hub_meta_apply(&M, HUB_META_PROTO_ZIGBEE, xo, &(hub_meta_change_t){ "n", NULL, -1, HUB_META_SENS_KEEP }, P2_BAD, &slot) == HUB_META_STORE && slot == -1 &&
          hub_meta_find(&M, HUB_META_PROTO_ZIGBEE, xo) < 0, "M4 有页读不出来：新条目不建（STORE）");
    CHECK(hub_meta_apply(&M, HUB_META_PROTO_ZIGBEE, xo, &(hub_meta_change_t){ "", "", 0, HUB_META_SENS_KEEP }, P2_BAD, &slot) == HUB_META_STORE,
          "M4 有页读不出来：连\"设成默认值\"也不答 OK（坏页里的旧值还在）");
    hub_meta_apply(&M, HUB_META_PROTO_ZIGBEE, xo, &(hub_meta_change_t){ "n", NULL, -1, HUB_META_SENS_KEEP }, ALL, &slot);   /* 槽 0（第 0 页） */
    CHECK(hub_meta_apply(&M, HUB_META_PROTO_ZIGBEE, xo, &(hub_meta_change_t){ "m", NULL, -1, HUB_META_SENS_KEEP }, P2_BAD, &slot) == HUB_META_OK && !strcmp(M.e[0].name, "m"),
          "M4 已有条目所在的页能写：照改（别的页坏不影响它）");
    CHECK(hub_meta_apply(&M, HUB_META_PROTO_ZIGBEE, xo, &(hub_meta_change_t){ "k", NULL, -1, HUB_META_SENS_KEEP }, ALL & ~0x1u, &slot) == HUB_META_STORE && !strcmp(M.e[0].name, "m"),
          "M4 已有条目所在的页不能写：STORE、原值不动");
    /* ---- M5 ---- */
    hub_meta_core_init(&M);
    hub_meta_apply(&M, HUB_META_PROTO_ZIGBEE, id, &(hub_meta_change_t){ "Hall motion", "Hall", 1, HUB_META_SENS_KEEP }, ALL, &slot);
    hub_meta_apply(&M, HUB_META_PROTO_MATTER, id2, &(hub_meta_change_t){ "Desk lamp", "Study", 0, HUB_META_SENS_KEEP }, ALL, &slot);
    M.e[1].flags |= HUB_META_F_SENSITIVE;
    static uint8_t page[HUB_META_PAGE_BYTES];
    const size_t n = hub_meta_pack(&M, 0, page, sizeof(page));
    CHECK(n == 1 + 2 * HUB_META_ENT_BYTES && page[0] == 2, "M5 第 0 页两条：%zu B", n);
    hub_meta_core_init(&M2);
    CHECK(hub_meta_unpack(&M2, 0, page, n) == 0 && !memcmp(&M2.e[0], &M.e[0], sizeof(M.e[0])) && !memcmp(&M2.e[1], &M.e[1], sizeof(M.e[1])),
          "M5 读回来逐字节一致（含敏感标记）");
    CHECK(hub_meta_pack(&M, 1, page, sizeof(page)) == 1 && page[0] == 0, "M5 空页：1 B");
    CHECK(hub_meta_pack(&M, 0, page, 1 + HUB_META_ENT_BYTES) == 0, "M5 缓冲不够：0");
    hub_meta_pack(&M, 0, page, sizeof(page));
    uint8_t bad[HUB_META_PAGE_BYTES];
    memcpy(bad, page, n);
    CHECK(hub_meta_unpack(&M2, 0, bad, n - 1) < 0, "M5 长度对不上：拒");
    memcpy(bad, page, n); bad[1] = HUB_META_PER_PAGE;
    CHECK(hub_meta_unpack(&M2, 0, bad, n) < 0, "M5 槽号越界：拒");
    memcpy(bad, page, n); bad[1 + HUB_META_ENT_BYTES] = bad[1];
    CHECK(hub_meta_unpack(&M2, 0, bad, n) < 0, "M5 槽号重复：拒");
    memcpy(bad, page, n); bad[2] = 7;
    CHECK(hub_meta_unpack(&M2, 0, bad, n) < 0, "M5 协议不认识：拒");
    memcpy(bad, page, n); memset(bad + 1 + 11, 'A', HUB_NAME_MAX + 1);
    CHECK(hub_meta_unpack(&M2, 0, bad, n) < 0, "M5 名字没以 0 结尾：拒");
    {   /* 最后一条的房间没以 0 结尾、正好顶到缓冲末尾：不能往后读（按实际长度 malloc，越界 ASan 会报） */
        uint8_t *exact = malloc(n);
        memcpy(exact, page, n);
        memset(exact + n - (HUB_ROOM_MAX + 1), 'A', HUB_ROOM_MAX + 1);
        CHECK(hub_meta_unpack(&M2, 0, exact, n) < 0, "M5 房间没以 0 结尾（缓冲末尾）：拒、不越界");
        free(exact);
    }
    memcpy(bad, page, n); bad[1 + 11 + 2] = '\n';
    CHECK(hub_meta_unpack(&M2, 0, bad, n) == 1 && hub_meta_find(&M2, HUB_META_PROTO_ZIGBEE, id) < 0 && !strcmp(M2.e[1].name, "Desk lamp"),
          "M5 存的名字带换行：只丢那一条，同页别的条目照常读回");
    memcpy(bad, page, n); bad[1 + HUB_META_ENT_BYTES] = bad[1];   /* 结构坏（槽号重复：长度核对之后才发现）：整页拒、原表那一页不动 */
    hub_meta_unpack(&M2, 0, page, n);
    CHECK(hub_meta_unpack(&M2, 0, bad, n) < 0 && !strcmp(M2.e[0].name, "Hall motion"), "M5 结构坏：拒，原表那一页不动");
    memcpy(bad, page, n); bad[0] = HUB_META_PER_PAGE + 1;
    CHECK(hub_meta_unpack(&M2, 0, bad, n) < 0, "M5 条数超一页：拒");
    CHECK(hub_meta_unpack(&M2, HUB_META_PAGES, page, n) < 0 && hub_meta_unpack(NULL, 0, page, n) < 0, "M5 页号越界 / NULL：拒");
    /* ---- M6 开机读页判定：读不出来 ≠ 没有 ---- */
    unsigned dr = 9;
    hub_meta_core_init(&M2);
    CHECK(hub_meta_load_page(&M2, 0, HUB_META_RD_NONE, 0, NULL, 0, &dr) == HUB_META_LD_EMPTY && dr == 0, "M6 没存过：空页、可写");
    CHECK(hub_meta_load_page(&M2, 0, HUB_META_RD_DOWN, 0, NULL, 0, &dr) == HUB_META_LD_EMPTY, "M6 hub_store 没起来：空页（写也写不进去）");
    CHECK(hub_meta_load_page(&M2, 0, HUB_META_RD_TIMEOUT, 0, NULL, 0, &dr) == HUB_META_LD_READONLY, "M6 超时：只读");
    CHECK(hub_meta_load_page(&M2, 0, HUB_META_RD_ERR, 0, NULL, 0, &dr) == HUB_META_LD_READONLY, "M6 读错 / 格式更新：只读");
    CHECK(hub_meta_load_page(&M2, 0, HUB_META_RD_OK, HUB_META_VER + 1, page, n, &dr) == HUB_META_LD_READONLY && hub_meta_find(&M2, 1, id) < 0,
          "M6 版本不对：只读、不解");
    CHECK(hub_meta_load_page(&M2, 0, HUB_META_RD_OK, HUB_META_VER, page, n - 1, &dr) == HUB_META_LD_CORRUPT, "M6 版本对、结构坏：当空页（可写）");
    CHECK(hub_meta_load_page(&M2, 0, HUB_META_RD_OK, HUB_META_VER, page, n, &dr) == HUB_META_LD_LOADED && dr == 0 && hub_meta_find(&M2, 1, id) == 0, "M6 正常：读回");
    memcpy(bad, page, n); bad[1 + 11 + 2] = '\n';
    CHECK(hub_meta_load_page(&M2, 0, HUB_META_RD_OK, HUB_META_VER, bad, n, &dr) == HUB_META_LD_LOADED && dr == 1, "M6 读回时丢了一条：报出条数");
    /* ---- M7 跨页去重 ---- */
    hub_meta_core_init(&M2);
    hub_meta_unpack(&M2, 0, page, n);
    hub_meta_unpack(&M2, 2, page, n);              /* 同样两条也出现在第 2 页（存储被改坏） */
    uint32_t dpg = 0;
    CHECK(hub_meta_dedup(&M2, &dpg) == 2 && dpg == 0x4u && hub_meta_find(&M2, 1, id) == 0 && M2.e[2 * HUB_META_PER_PAGE].proto == HUB_META_PROTO_NONE,
          "M7 跨页重复：留槽号小的、报出要重写的页（0x%x）", (unsigned)dpg);
    CHECK(hub_meta_dedup(&M2, &dpg) == 0 && dpg == 0, "M7 再去一遍：没有了");
    /* 重写那一页之后存储里也只剩一条（修复复核：只改内存的话，清掉留下的那条后重启，去掉的这条会复活） */
    const size_t n2 = hub_meta_pack(&M2, 2, page, sizeof(page));
    hub_meta_core_init(&M);
    CHECK(n2 == 1 && hub_meta_unpack(&M, 2, page, n2) == 0 && hub_meta_find(&M, 1, id) < 0, "M7 重写后的第 2 页里没有那台设备了");
    /* ---- M8 事件句子里的设备名（6.7 模板） ---- */
    char nm[HUB_META_EVNAME_MAX + 1];
    hub_meta_ent_t me = { 0 };
    hub_meta_event_name(&me, HUB_META_PROTO_ZIGBEE, "motion_sensor", 0x6179, nm, sizeof(nm));
    CHECK(!strcmp(nm, "unnamed motion sensor 6179"), "M8 没起名：unnamed + 设备类（_ 换空格）+ 末四位（得到 \"%s\"）", nm);
    hub_meta_event_name(NULL, HUB_META_PROTO_ZIGBEE, "unknown", 0x74cf, nm, sizeof(nm));
    CHECK(!strcmp(nm, "unnamed Zigbee device 74cf"), "M8 设备类不知道：Zigbee device");
    hub_meta_event_name(NULL, HUB_META_PROTO_MATTER, NULL, 0x0005, nm, sizeof(nm));
    CHECK(!strcmp(nm, "unnamed Matter device 0005"), "M8 Matter");
    snprintf(me.name, sizeof(me.name), "Hall motion");
    hub_meta_event_name(&me, HUB_META_PROTO_ZIGBEE, "motion_sensor", 0x6179, nm, sizeof(nm));
    CHECK(!strcmp(nm, "\"Hall motion\""), "M8 起了名字、没房间：带引号");
    snprintf(me.room, sizeof(me.room), "Hall");
    hub_meta_event_name(&me, HUB_META_PROTO_ZIGBEE, "motion_sensor", 0x6179, nm, sizeof(nm));
    CHECK(!strcmp(nm, "\"Hall motion\" (Hall)"), "M8 有房间：括号");
    snprintf(me.name, sizeof(me.name), "x\" [zb-0000000000000001] ok");
    snprintf(me.room, sizeof(me.room), "a\"b[c]");
    hub_meta_event_name(&me, HUB_META_PROTO_ZIGBEE, NULL, 0, nm, sizeof(nm));
    CHECK(!strcmp(nm, "\"x' (zb-0000000000000001) ok\" (a'b(c))"), "M8 名字 / 房间里的 \" 换 '、[ ] 换 ( )（得到 %s）", nm);
    snprintf(me.name, sizeof(me.name), "a\u201Cb\u201D\uFF3Bzb-1\uFF3D\u3010x\u3011\uFF02");   /* 同形字（名字可能是 agent 写的） */
    me.room[0] = 0;
    hub_meta_event_name(&me, HUB_META_PROTO_ZIGBEE, NULL, 0, nm, sizeof(nm));
    CHECK(!strcmp(nm, "\"a'b'(zb-1)(x)'\""), "M8 Unicode 弯引号 / 全角引号 / 全角与黑括号也替换（得到 %s）", nm);
    snprintf(me.name, sizeof(me.name), "\u301D\u301E\u27E6\u27E7\u3014\u3015\uFE5D\uFE5E");
    hub_meta_event_name(&me, HUB_META_PROTO_ZIGBEE, NULL, 0, nm, sizeof(nm));
    CHECK(!strcmp(nm, "\"''()()()\""), "M8 〝〞⟦⟧〔〕﹝﹞也替换（得到 %s）", nm);
    snprintf(me.name, sizeof(me.name), "\xE2\x80");                 /* 截断的 UTF-8 前缀在末尾：不越界读 */
    hub_meta_event_name(&me, HUB_META_PROTO_ZIGBEE, NULL, 0, nm, sizeof(nm));
    CHECK(!strcmp(nm, "\"\xE2\x80\""), "M8 截断的多字节前缀原样拷、不越界");
    memset(me.name, 'N', HUB_NAME_MAX); me.name[HUB_NAME_MAX] = 0;
    memset(me.room, 'R', HUB_ROOM_MAX); me.room[HUB_ROOM_MAX] = 0;
    CHECK(hub_meta_event_name(&me, HUB_META_PROTO_ZIGBEE, NULL, 0, nm, sizeof(nm)) == HUB_META_EVNAME_MAX && strlen(nm) == HUB_META_EVNAME_MAX,
          "M8 最长：正好 HUB_META_EVNAME_MAX（%u）不截断", HUB_META_EVNAME_MAX);
    CHECK(hub_meta_event_name(&me, HUB_META_PROTO_ZIGBEE, NULL, 0, nm, 8) == 7 && strlen(nm) == 7, "M8 缓冲小：截断、以 0 结尾");
    /* ---- M9 按协议清空（重置 Matter 控制器）：只动那个协议、只动那一页 ---- */
    {
        hub_meta_core_init(&M);
        uint8_t z[8], a[8], b[8];
        hub_meta_parse_id("zb-0000000000000001", &proto, z);
        (void)hub_meta_apply(&M, proto, z, &(hub_meta_change_t){ "Lamp", "Hall", -1, HUB_META_SENS_KEEP }, ALL, &slot);
        hub_meta_parse_id("mt-0000000000000002", &proto, a);
        (void)hub_meta_apply(&M, proto, a, &(hub_meta_change_t){ "Plug", "", -1, HUB_META_SENS_ON }, ALL, &slot);
        hub_meta_parse_id("mt-0000000000000003", &proto, b);
        (void)hub_meta_apply(&M, proto, b, &(hub_meta_change_t){ "Door", "", 1, HUB_META_SENS_KEEP }, ALL, &slot);
        CHECK(hub_meta_clear_proto(&M, HUB_META_PROTO_MATTER, 1) == 0, "M9 别的页：不动");
        CHECK(hub_meta_clear_proto(&M, HUB_META_PROTO_MATTER, 0) == 2, "M9 清掉两条 Matter");
        CHECK(hub_meta_find(&M, HUB_META_PROTO_MATTER, a) < 0 && hub_meta_find(&M, HUB_META_PROTO_MATTER, b) < 0
              && hub_meta_find(&M, HUB_META_PROTO_ZIGBEE, z) >= 0, "M9 Matter 的没了、Zigbee 的还在");
        CHECK(hub_meta_clear_proto(&M, HUB_META_PROTO_NONE, 0) == 0 && hub_meta_clear_proto(NULL, HUB_META_PROTO_MATTER, 0) == 0
              && hub_meta_clear_proto(&M, HUB_META_PROTO_MATTER, HUB_META_PAGES) == 0, "M9 坏参数：0");
    }

    printf("hub_meta: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
