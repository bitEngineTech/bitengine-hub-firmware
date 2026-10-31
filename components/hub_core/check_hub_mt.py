#!/usr/bin/env python3
"""hub_core 里 Matter 设备（mt-）的跨行形状检查（VERIFY.sh 调；M7 第四步起）。
   python3 check_hub_mt.py <检查名>   —— 过 = 退出码 0。先剥注释（保留字符串字面量）。"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from grep_code import strip_comments  # noqa: E402

HERE = Path(__file__).resolve().parent


def code(name: str) -> str:
    return strip_comments((HERE / name).read_text(encoding="utf-8"), keep_strings=True)


def body(src: str, head: str) -> str:
    """从函数定义（不是前向声明）开始到第一个顶格的 } 为止"""
    i = src.find(head)
    while i >= 0:
        k = min([x for x in (src.find(";", i), src.find("{", i)) if x >= 0], default=-1)
        if k >= 0 and src[k] == "{":
            j = src.find("\n}", i)
            return src[i:j] if j > 0 else ""
        i = src.find(head, i + 1)
    return ""


def mt_ids() -> bool:
    """mt- 的写法只有一处：输出小写（同 zb- / th-、hub.device.update 的回读），解析不分大小写、两个入口共用"""
    m = code("hub_matter.c")
    return ('static void mt_id(uint64_t node, char out[20]) { snprintf(out, 20, "mt-%016" PRIx64, node); }' in m
            and '"mt-%016" PRIX64' not in m and 'strncasecmp(id, "mt-", 3)' in body(m, "static bool mt_parse(")
            and "mt_parse(id, &node)" in body(m, "int hub_matter_write_device(") and "mt_parse(id, &node)" in body(m, "esp_err_t hub_matter_remove("))


def ev_name() -> bool:
    """事件句子里的称呼走 hub_meta_event_name（引号 / 方括号已换掉，用户起的名字伪造不了 [id]）、不用设备自报的字符串；
       访谈里只有"加好了"推送，重新访谈只进环"""
    b = body(code("hub_matter.c"), "static void hk_event(")
    a, iv = b.find("case MATTER_CTRL_EV_ADDED:"), b.find("case MATTER_CTRL_EV_INTERVIEWED:")
    nx = b.find("case ", iv + 5)
    return ("hub_meta_event_name(named ? &m : NULL, HUB_META_PROTO_MATTER," in b and 0 < a < iv < nx
            and "HEV_F_PUSH" in b[a:iv] and "HEV_F_PUSH" not in b[iv:nx] and '"matter_identified", t, 0);' in b[iv:nx]
            and "iv.vendor" not in b and "iv.product" not in b)


def list_order() -> bool:
    """列表顺序 Zigbee → Matter → Thread，游标是合在一起的序号；Thread 成员里已按 mt- 列了的跳过（7.3，序号照占），快照用完就放"""
    b = body(code("hub_cmds.c"), "static void list_cb(")
    z, m, t = b.find("hub_zigbee_write_list("), b.find("hub_matter_write_list("), b.find("ti_write_entry(")
    sk = b.find("if (hub_matter_own_has(own, raw, &l->e[i], NULL)) continue;")
    return (0 < z < m < b.find("hub_matter_own_get()") < sk < t < b.find("hub_matter_own_free(own);") and "next = nz + nm + i;" in b
            and "next = nz + mnext;" in b and "c->start > nz + nm ? c->start - nz - nm : 0" in b and "if (mr == HUB_MT_BUSY) {" in b)


def matter_needs_inventory() -> bool:
    """只要 Zigbee 不碰清单；要 Matter 时取清单里的 Thread 信息（不现刷），清单拿不到照样回（不挂 thread）；只有要 Thread 成员时才回 unavailable"""
    b = body(code("hub_cmds.c"), "static int cmd_devices_list(")
    return ("if (c.want_thread || c.want_matter) ie = thread_inventory_with(c.want_thread ? TI_FRESH_MS : 0, list_cb, &c);" in b
            and "if (ie != ESP_OK && !c.want_thread) list_cb(NULL, NULL, &c);" in b
            and "if ((ie != ESP_OK && c.want_thread) || c.rc != 0)" in b and "MATTER_ON && (all || !strcmp(proto, \"matter\"))" in b)


def get_paths() -> bool:
    """device.get：mt- 走 hub_matter（清单拿不到照样回、只是不挂 Thread）；已按 mt- 列的 th- 不给第二份说法，指向 mt-"""
    s = code("hub_cmds.c")
    g, cb = body(s, "static int cmd_device_get("), body(s, "static void get_cb(")
    i = cb.find("c->moved = hub_matter_own_has(own, raw, e, c->mt);")
    return ("if (thread_inventory_with(TI_FRESH_MS, mget_cb, &mc) != ESP_OK) mget_cb(NULL, NULL, &mc);" in g
            and "if (c.moved) {" in g and 0 < i < cb.find("if (c->moved) return;") < cb.find("ti_write_entry(")
            and "hub_matter_own_free(own);" in cb)


def update_mt() -> bool:
    """hub.device.update 认本网关 fabric 上的 Matter 设备（只问在不在，不写 JSON）"""
    b = body(code("hub_cmds.c"), "static int cmd_device_update(")
    return 'strncasecmp(id, "mt-", 3) == 0' in b and "hub_matter_write_device(NULL, id, NULL, NULL)" in b and "known = mr == HUB_MT_DONE;" in b


def entry_shape() -> bool:
    """列表项：只列配好了的；能力 / 设备类来自 hub_mcap；能开关 / 调亮度的才 controllable（第六步）；Thread 信息只在找到本 fabric 的条目时挂"""
    m = code("hub_matter.c")
    we, wl, wd = body(m, "static void write_entry("), body(m, "int hub_matter_write_list("), body(m, "int hub_matter_write_device(")
    return ("hub_mcap_caps(iv)" in we and "hub_mcap_kind(iv, caps)" in we and 'hj_kbool(w, "controllable", (caps & HUB_MT_CTL_CAPS) != 0);' in we
            and "if (raw && te) ti_write_thread_obj(w, raw, te, full);" in we and "if (!nodes[i].commissioned) continue;" in wl
            and "!nodes[i].commissioned" in wd and "heap_caps_free(nodes);" in wl and "heap_caps_free(iv);" in wd
            and "_Static_assert(CONFIG_BITENGINE_MATTER_MAX_NODES <= MC_LIST_MAX" in m)


def health_count() -> bool:
    """device.health 的 total 与列表同一个口径：只从 Thread 成员那一份里扣掉已按 mt- 列的，再加配好的 Matter 设备与 Zigbee；同一份快照（max_age 0）"""
    s = code("hub_cmds.c")
    h, cb = body(s, "static int cmd_device_health("), body(s, "static void fab_cnt_cb(")
    return ("thread_inventory_with(0, fab_cnt_cb, &fc) == ESP_OK) thn -= fc.n < thn ? fc.n : thn;" in h
            and "unsigned long total = thn;" in h and "total += (unsigned long)mn;" in h and "partial = mn < 0;" in h
            and "partial = partial || zn < 0;" in h and "hub_matter_own_has(c->own, raw, &l->e[i], NULL)" in cb
            and "if (nodes[i].commissioned) k++;" in body(code("hub_matter.c"), "int hub_matter_count("))


def own_needs_table() -> bool:
    """跳过 th- 的判据：实例属于本 fabric **且**节点号是节点表里配好了的（审计 ❌：只看 fabric，强制移除 / 表没读回 / 配到一半的设备会整台消失）"""
    m = code("hub_matter.c")
    g, h = body(m, "hub_mt_own_t *hub_matter_own_get("), body(m, "bool hub_matter_own_has(")
    return ("if (nodes[i].commissioned) o->node[o->n++] = nodes[i].node_id;" in g and "if (!cfid[0]) return NULL;" in g
            and "if (!o || !ti_entry_on_fabric(raw, e, o->cfid, hex)) return false;" in h and "if (o->node[i] != node) continue;" in h
            and "hub_matter_cfid" not in code("hub_cmds.c"))


def mt_events() -> bool:
    """事件钩子认全部五种；离线 / 回来是 AVAILABILITY、推（同 Zigbee）；读数翻转：门磁 / 人体 SECURITY、漏水 / 烟雾 ALARM（推）、电量低 BATTERY"""
    m = code("hub_matter.c")
    h, se = body(m, "static void hk_event("), body(m, "static void state_event(")
    cases = all(f"case MATTER_CTRL_EV_{e}:" in h for e in ("ADDED", "INTERVIEWED", "OFFLINE", "ONLINE", "STATE")) and "default:" in h
    return (cases and 'emit_ev(HUB_EVC_AVAILABILITY, id, "matter_offline", t, HEV_F_PUSH);' in h
            and 'emit_ev(HUB_EVC_AVAILABILITY, id, "matter_online", t, HEV_F_PUSH);' in h
            and 'emit_ev(HUB_EVC_SECURITY, id, open ? "contact_open" : "contact_closed", t, HEV_F_PUSH);' in se
            and 'emit_ev(HUB_EVC_SECURITY, id, "motion_detected", t, HEV_F_PUSH);' in se
            and 'emit_ev(HUB_EVC_ALARM, id, on ? "water_leak" : "water_leak_cleared", t, alarm_flags);' in se
            and 'emit_ev(HUB_EVC_ALARM, id, on ? "smoke_detected" : "smoke_cleared", t, alarm_flags);' in se
            and "const unsigned alarm_flags = HEV_F_PUSH;" in se
            and 'emit_ev(HUB_EVC_BATTERY, id, "battery_low", t, HEV_F_PUSH);' in se)


def mt_state_keys() -> bool:
    """读数的键名与 Zigbee 一致（门磁是 "open" / "closed" 字符串）；state 与 state_age_s 都写；不知道在不在线就不写 online"""
    m, z = code("hub_matter.c"), code("hub_zigbee.c")
    ws, we = body(m, "static void write_state("), body(m, "static void write_entry(")
    keys = ("contact", "water_leak", "motion", "battery_pct", "temperature_c", "humidity_pct", "pressure_hpa", "illuminance_lux", "onoff", "level_pct")
    return (all(f'"{k}"' in ws and f'"{k}"' in z for k in keys) and 'hj_kstr(w, "contact", s->contact_open ? "open" : "closed");' in ws
            and 'hj_kstr(w, "contact", a1 ? "open" : "closed");' in z
            and "write_state(w, &lv.st, false, now);" in we and "write_state(w, &lv.st, true, now);" in we
            and 'if (lv.online >= 0) hj_kbool(w, "online", lv.online == 1);' in we)


def rl_refund() -> bool:
    """MX21：配网当场拒（码不对 / 名字不合法 / 控制器没在跑）把限速令牌还回去，受理了才不还；两个出口都看 rl_refund、豁免的不还"""
    m = code("hub_matter.c")
    c = body(m, "int hub_matter_cmd_commission(")
    api = strip_comments((HERE.parent / "webui" / "hub_api.c").read_text(encoding="utf-8"), keep_strings=True)
    ml = strip_comments((HERE.parent / "muse_link" / "muse_link.c").read_text(encoding="utf-8"), keep_strings=True)
    pol = body(code("hub_policy.c"), "void hub_rl_refund(")
    first = c.find("call->rl_refund = true;")
    return (0 < first < c.find("return -1;") and c.find("call->rl_refund = false;") > c.rfind("return -1;")
            and "if (call.rl_refund && !rl_exempt) hub_rl_refund(&s_rl, cmd->rl," in api
            and "if (call.rl_refund && !rl_exempt) hub_rl_refund(&s_rl, cmd->rl, now_ms());" in ml
            and "if (cb->tokens < k_class_spec[c].capacity) cb->tokens++;" in pol and "rl->total.tokens++" in pol)


def reset_and_opts() -> bool:
    """重置只在 ident_lost 时受理；顺序：清 Matter 名字 → 清 hubdb mctrl → 最后删绑定记号（之前失败都能再来）。开关落盘读回确认后才生效。
       两个本地接口：先 CROSS_ORIGIN_GUARD 再 AUTH_REQUIRED（红线 5）、要管理密码、记审计；Muse 那边碰不到"""
    m = code("hub_matter.c")
    r, a = body(m, "esp_err_t hub_matter_reset("), body(m, "esp_err_t hub_matter_set_allow_test(")
    api = strip_comments((HERE.parent / "webui" / "hub_api.c").read_text(encoding="utf-8"), keep_strings=True)
    ml = strip_comments((HERE.parent / "muse_link" / "muse_link.c").read_text(encoding="utf-8"), keep_strings=True)
    ok = ('strcmp(st.fail_step, "ident_lost") != 0' in r
          and 0 < r.find("hub_meta_forget_proto(HUB_META_PROTO_MATTER)") < r.find("hub_store_erase_ns(HUB_STORE_NS_MCTRL") < r.find("matter_ctrl_forget_binding()")
          and 0 < a.find("if (!on) {") < a.find("matter_ctrl_set_allow_test_devices(false);") < a.find("save_confirmed(OPTS_KEY", a.find("if (!on) {"))
          and a.rfind("save_confirmed(OPTS_KEY") < a.find("matter_ctrl_set_allow_test_devices(true)"))
    hm = body(code("hub_meta.c"), "esp_err_t hub_meta_forget_proto(")
    ok = ok and 0 < hm.find("if (s_page_bad[p]) return ESP_ERR_INVALID_STATE;") < hm.find("hub_meta_clear_proto(") and "if (!has)" not in hm
    hs = code("hub_store.c")
    ok = ok and "if (wipe_all) (void)hub_store_erase(HUB_STORE_NS_MCTRL, HUB_MATTER_OPTS_KEY);" in hs
    ok = ok and hs.find("hub_store_erase(HUB_STORE_NS_MCTRL, HUB_MATTER_OPTS_KEY)") < hs.find("esp_err_t err = hub_store_factory_reset(wipe_all);")
    for fn, tag in (("handle_matter_allow_test", "matter.allow_test_devices"), ("handle_matter_reset", "matter.reset")):
        b = body(api, f"static esp_err_t {fn}(")
        ok = ok and bool(re.match(r"static esp_err_t \w+\(httpd_req_t \*req\)\s*\{\s*CROSS_ORIGIN_GUARD\(req\);\s*AUTH_REQUIRED\(req\);\s*if \(!config_mgr_has_password\(\)\)", b))
        ok = ok and f'hub_audit_note(HAU_SRC_REST, "{tag}"' in b
    al, rs = body(api, "static esp_err_t handle_matter_allow_test("), body(api, "static esp_err_t handle_matter_reset(")
    ok = ok and 'hub_audit_note(HAU_SRC_REST, "matter.allow_test_devices", on ? "on" : "off", e == ESP_OK || e == ESP_ERR_NOT_FINISHED ? "ok" : "unavailable");' in al
    ok = ok and 0 < rs.find('hub_audit_note(HAU_SRC_REST, "matter.reset", NULL, "ok");') < rs.find("esp_restart();")
    ok = ok and 0 < rs.find("if (ota_mg24_busy() || ota_cloud_busy() || webui_ota_busy()) {") < rs.find("hub_matter_reset(why, sizeof(why))")
    ok = ok and rs.find("config_mgr_flush(500);") < rs.find("esp_restart();")
    return ok and "allow_test" not in ml and "matter_reset" not in ml and "hub_matter_reset" not in code("hub_cmds.c")


CHECKS = {"mt_ids": mt_ids, "ev_name": ev_name, "list_order": list_order, "matter_needs_inventory": matter_needs_inventory,
          "get_paths": get_paths, "update_mt": update_mt, "entry_shape": entry_shape, "health_count": health_count,
          "own_needs_table": own_needs_table, "mt_events": mt_events, "mt_state_keys": mt_state_keys,
          "rl_refund": rl_refund, "reset_and_opts": reset_and_opts}

if __name__ == "__main__":
    if len(sys.argv) != 2 or sys.argv[1] not in CHECKS:
        sys.exit(f"用法：{sys.argv[0]} {{{'|'.join(CHECKS)}}}")
    sys.exit(0 if CHECKS[sys.argv[1]]() else 1)
