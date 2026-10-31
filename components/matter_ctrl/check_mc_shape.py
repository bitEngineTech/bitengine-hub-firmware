#!/usr/bin/env python3
"""matter_ctrl 的跨行形状检查（VERIFY.sh 调；grep_code.py 一次只看一行，多行的调用 / 次序在这里判）。
   python3 check_mc_shape.py <检查名>   —— 过 = 退出码 0。先剥注释（保留字符串字面量：有的判据要认 fail("…") 的步骤名）。"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from grep_code import strip_comments  # noqa: E402

HERE = Path(__file__).resolve().parent


def code(name: str) -> str:
    return strip_comments((HERE / name).read_text(encoding="utf-8"), keep_strings=True)


def body(src: str, head: str) -> str:
    """从函数定义（不是前向声明：头后面先遇到 { 而不是 ;）开始到第一个顶格的 } 为止"""
    i = src.find(head)
    while i >= 0:
        k = min([x for x in (src.find(";", i), src.find("{", i)) if x >= 0], default=-1)
        if k >= 0 and src[k] == "{":
            j = src.find("\n}", i)
            return src[i:j] if j > 0 else ""
        i = src.find(head, i + 1)
    return ""


def no_icac() -> bool:
    s = code("mc_issuer.cpp")
    return "NewICAX509Cert" not in s and s.count("X509CertRequestParams req = { rand_serial(), kCertNotBefore, kNullCertTime,") == 2


def ipk_ctrl() -> bool:
    return re.search(r"SetSingleIpkEpochKey\([^;]*s_issuer\.ipk\(\)", code("matter_ctrl.cpp"), re.S) is not None


def save_before_load() -> bool:
    b = body(code("matter_ctrl.cpp"), "static void setup_in_chip_task(")
    sv, f, ld = b.find("s_hooks->ident_save("), b.find('fail("ident_save"'), b.find("s_issuer.load(")
    return 0 < sv < f < ld


def wipes() -> bool:
    return code("matter_ctrl.cpp").count("mc_ident_wipe(&s_ident") >= 4 and "mc_ident_wipe(ser.Bytes()" in code("mc_issuer.cpp")


def ident_lost() -> bool:
    b = body(code("matter_ctrl.cpp"), "static void start_task(")
    i = b.find("bind_marker_present(&marked)")
    j = b.find('step = "ident_lost"')
    k = b.find("s_ident.fabric_id = rand_fabric_id()")
    return 0 < i < j < k


def wipe_on_start_fail() -> bool:
    b = body(code("matter_ctrl.cpp"), "static void start_task(")
    s, f1, f2 = b.find("esp_matter::start("), b.find('fail("esp_matter_start"'), b.find('fail("schedule_setup"')
    seg1, seg2 = b[s:f1], b[f1:f2]
    return s > 0 and "mc_ident_wipe(&s_ident" in seg1 and "mc_ident_wipe(&s_ident" in seg2


def factory_reset() -> bool:
    hs = strip_comments((HERE.parent / "hub_core" / "hub_store.c").read_text(encoding="utf-8"), keep_strings=True)
    b = body(hs, "esp_err_t config_mgr_hubdb_factory_reset(bool wipe_all)")
    f, r, e = b.find("matter_ctrl_forget_binding()"), b.find("hub_store_factory_reset(wipe_all)"), b.find("hub_store_erase_ns(HUB_STORE_NS_MCTRL")
    guarded = re.search(r"if \(err == ESP_OK && forget_matter\) \{\s*err = hub_store_erase_ns\(HUB_STORE_NS_MCTRL", b) is not None
    frozen = re.search(r"s_frozen_hub && \(!strcmp\(m->ns, HUB_STORE_NS_HUB\) \|\| !strcmp\(m->ns, HUB_STORE_NS_MCTRL\)\)", hs) is not None
    return 0 < f < r < e and guarded and frozen


def literals(src: str):
    """相邻的字符串字面量（中间只有空白）拼成一句"""
    for m in re.finditer(r'"(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*', src):
        yield "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(0)))


def msg_len() -> bool:
    """配网 / 移除的句子放得下：晚到的结论进 matter_ctrl_result_t.msg[200]（再进作业的 err_msg），同步拒的进 hub_call_t.err_msg[160]
       （第三步上板：not_found 那句 222 字节被截成 "...try again with a "）"""
    s = (HERE / "mc_comm.cpp").read_text(encoding="utf-8")
    if any(len(t) > 199 for t in literals(s)):
        return False
    for m in re.finditer(r"set_why\(why, why_len, ((?:\"[^\"]*\"\s*)+)\)", s):
        if len("".join(re.findall(r'"([^"]*)"', m.group(1)))) > 159:
            return False
    return True


def node_before_pair() -> bool:
    """节点号落盘读回确认之后才进入等待 / 配网（MC12）；失败路径删记录"""
    b = body(code("mc_comm.cpp"), "static void comm_start_in_chip(")
    sv, ph = b.find("nodes_save()"), b.find("s_op.phase = PH_WAIT_LOCAL")
    return 0 < sv < ph and 'comm_fail("storage_error"' in b[sv:ph]


def pair_direct() -> bool:
    """直接调 commissioner 的 PairDevice（esp-matter 的 pairing_code 吞返回值、失败不撤委托）；先撤残留委托；失败自己撤"""
    s = code("mc_comm.cpp")
    b = body(s, "static void comm_pair(")
    clr, reg, pd = b.find("RegisterPairingDelegate(nullptr)"), b.find("RegisterPairingDelegate(&pairing_command::get_instance())"), b.find("comm->PairDevice(")
    after = b[pd:]
    return (0 < clr < reg < pd and "kDiscoveryNetworkOnly" in after and "RegisterPairingDelegate(nullptr)" in after.split("comm_fail(")[0]
            and "pairing_code(" not in s)


def ble_only_rejected() -> bool:
    b = body(code("mc_comm.cpp"), "esp_err_t matter_ctrl_commission(")
    return re.search(r"rendezvousInformation\.HasValue\(\) &&\s*!pl\.rendezvousInformation\.Value\(\)\.Has\(chip::RendezvousInformationFlag::kOnNetwork\)\)", b) is not None and "ESP_ERR_NOT_SUPPORTED" in b


def pin_wiped() -> bool:
    s = code("mc_comm.cpp")
    f = body(s, "static void finish(bool ok, const char *code, const char *fmt, ...)\n")
    c = body(s, "esp_err_t matter_ctrl_commission(")
    return "memset(s_op.code, 0, sizeof(s_op.code))" in f and c.count("memset(norm, 0, sizeof(norm))") >= 4


def deadlines() -> bool:
    s = code("mc_comm.cpp")
    tot = re.search(r"#define MC_TOTAL_MS\s+(\d+)", s)
    reg = (HERE.parent / "hub_core" / "hub_registry.c").read_text(encoding="utf-8")
    m = re.search(r'\.name = "hub\.matter\.commission".*?\.timeout_ms = (\d+)', reg, re.S)
    return bool(tot and m and int(tot.group(1)) < int(m.group(1)) and "MC_FIND_MS" in s and "fail_and_stop(\"not_found\"" in body(s, "static void op_timer("))


def rest_guards() -> bool:
    api = strip_comments((HERE.parent / "webui" / "hub_api.c").read_text(encoding="utf-8"), keep_strings=True)
    ok = True
    for fn in ("handle_job", "handle_device_remove"):
        b = body(api, f"static esp_err_t {fn}(httpd_req_t *req)")
        ok = ok and re.match(r"static esp_err_t \w+\(httpd_req_t \*req\)\n\{\s*CROSS_ORIGIN_GUARD\(req\);\s*AUTH_REQUIRED\(req\);", b) is not None
    rm = body(api, "static esp_err_t handle_device_remove(httpd_req_t *req)")
    return ok and "config_mgr_has_password()" in rm and 'strcmp(confirm->valuestring, "REMOVE")' in rm


def muse_deferred() -> bool:
    ml = strip_comments((HERE.parent / "muse_link" / "muse_link.c").read_text(encoding="utf-8"), keep_strings=True)
    est = body(ml, "bool ml_session_established(void)")
    return ("if (call.job && pend_add(call.job, inv->id)) return \"accepted\";" in ml and "pend_clear();" in est
            and "hub_jobs_set_listener(job_listener);" in ml and "pend_flush();" in body(ml, "bool ml_session_poll(void)"))


def stop_after_finish() -> bool:
    """先收尾再 StopPairing（它会同步回调；审计 #1）；计时器在 comm_pair 之后按代号判断；三个回调都看阶段"""
    s = code("mc_comm.cpp")
    f = body(s, "static void fail_and_stop(")
    t = body(s, "static void op_timer(")
    ok = 0 < f.find("comm_fail(code, msg);") < f.find("StopPairing(node)") and "s_op.node" not in f[f.find("comm_fail(code, msg);"):]
    ok = ok and "comm->StopPairing" not in t and t.count("fail_and_stop(") == 2 and "if (s_op.gen != gen) return;" in t
    for cb in ("static void on_pase(", "static void on_ok(", "static void on_fail("):
        ok = ok and "s_op.phase != PH_PAIRING" in body(s, cb)
    return ok


def save_fail_consistent() -> bool:
    """配网成功、落盘失败：报失败，内存里也撤掉（comm_fail 删记录；审计 #2）"""
    b = body(code("mc_comm.cpp"), "static void on_ok(")
    i = b.find("if (se != ESP_OK) {")
    return i > 0 and 'comm_fail("storage_error"' in b[i:b.find("}", i)]


def boot_load_safe() -> bool:
    """节点表读回：解不开就报错、不覆盖（覆盖 = next_node 回到 2）；丢 PENDING 只在读回成功之后；s_nodes_ok 按结果置"""
    b = body(code("mc_comm.cpp"), "esp_err_t mc_nodes_boot_load(")
    bad = b.find("if (de != MC_NODES_OK) {")
    seg = b[bad:b.find("} else {", bad)]
    return (bad > 0 and "err = ESP_ERR_INVALID_CRC;" in seg and "nodes_save" not in seg
            and b.find("mc_nodes_drop_pending(&s_nodes)") > b.find("} else {", bad) and "s_nodes_ok = err == ESP_OK;" in b)


def jobs_audit() -> bool:
    j = strip_comments((HERE.parent / "hub_core" / "hub_jobs.c").read_text(encoding="utf-8"), keep_strings=True)
    sv = strip_comments((HERE.parent / "hub_core" / "hub_svc.c").read_text(encoding="utf-8"), keep_strings=True)
    hm = strip_comments((HERE.parent / "hub_core" / "hub_matter.c").read_text(encoding="utf-8"), keep_strings=True)
    ae = body(j, "static void after_end(")
    return ("hub_audit_record(" in ae and "fn(j->id)" in ae and "hub_jobs_tick();" in body(sv, "static void svc_task(")
            and '"rejected"' not in hm and hm.count("hub_job_abandon(job);") == 2)


def iv_added_only() -> bool:
    """"加好了"（EV_ADDED，会推给 Muse）只给配网刚成功时排的那次访谈；开机补访谈、记录读不回来重做的都只是 EV_INTERVIEWED（第四步上板发现）"""
    iv = code("mc_iv.cpp")
    end, req, boot = body(iv, "static void iv_end("), body(iv, "void mc_iv_request("), body(iv, "esp_err_t mc_iv_boot_load(")
    ok_on = body(code("mc_comm.cpp"), "static void on_ok(")
    return ("const bool added = sl->added;" in end and "added ? MATTER_CTRL_EV_ADDED : MATTER_CTRL_EV_INTERVIEWED" in end
            and "sl->added = false;" in end
            and "s_iv[i].added = added;" in req and "added" not in boot and "mc_iv_request(s_op.node, true);" in ok_on)


def iv_one_at_a_time() -> bool:
    """同一时刻只有一个访谈读请求（回调对象静态共用）：开下一台要 slot 空闲且上一个读请求已 OnDone；发出去成功才置 s_reading"""
    iv = code("mc_iv.cpp")
    tick, conn = body(iv, "static void iv_tick("), body(iv, "static void on_connected(")
    i = conn.find("send_request(")
    return ("if (s_cur.slot < 0 && !s_reading && mc_running())" in tick and "s_reading = false;" in body(iv, "void iv_cb::OnDone(")
            and i > 0 and conn.find("s_reading = true;") > conn.find("if (e != ESP_OK)", i) > i)


def iv_deadline() -> bool:
    """访谈有总时限（睡眠设备也得有个头）；超时收尾后迟到的回调不认（iv_end 先把 slot 置 -1）；失败有退避与次数上限"""
    iv = code("mc_iv.cpp")
    tick, end = body(iv, "static void iv_tick("), body(iv, "static void iv_end(")
    return ("now_ms() - s_cur.t0_ms >= MC_IV_TIMEOUT_MS" in tick and "iv_end(false);" in tick
            and end.find("s_cur.slot = -1;") < end.find("if (!ok)") and "sl->queued = sl->tries < MC_IV_TRIES;" in end
            and "arm_tick();" in tick)


def iv_forget_on_remove() -> bool:
    """移除（设备确认的、强制的）落盘成功后都忘掉访谈记录（含 hubdb 里那条，钩子 iv_erase）"""
    cm = code("mc_comm.cpp")
    fg = body(code("mc_iv.cpp"), "void mc_iv_forget(")
    return (cm.count("mc_iv_forget(s_op.node);") == 2 and "if (se == ESP_OK) mc_iv_forget(s_op.node);" in body(cm, "static void op_timer(")
            and "h->iv_erase(node)" in fg)


def iv_buf_fresh() -> bool:
    """BufferedReadCallback 每次读之前原地重建（它的 OnError 不清正在拼的路径，审计）；重建在 send_request 之前"""
    c = body(code("mc_iv.cpp"), "static void on_connected(")
    i, j = c.find("s_buf_cb = new (s_buf_mem) chip::app::BufferedReadCallback(s_cb);"), c.find("send_request(")
    return 0 < c.find("if (s_buf_cb) s_buf_cb->~BufferedReadCallback();") < i < j and "*s_buf_cb);" in c[j:]


def iv_complete_needed() -> bool:
    """读成功但没有根端点的簇表（路径级错误）不算访谈完了：按失败退避，不置 DONE、不落盘（审计）"""
    e = body(code("mc_iv.cpp"), "static void iv_end(")
    i = e.find("if (ok && !mc_iv_complete(&s_cur.work)) {")
    return 0 < i < e.find("if (!ok) {") < e.find("s_cur.work.flags |= MC_IV_F_DONE;")


def iv_cross_task() -> bool:
    """别的任务读访谈状态：s_cur.slot 只读一次再判、带上界；开机读回的记录在 s_lock 里写（httpd 可能正在拷，审计）"""
    iv = code("mc_iv.cpp")
    b, bl = body(iv, "bool matter_ctrl_node_iv_busy("), body(iv, "esp_err_t mc_iv_boot_load(")
    i = bl.find("xSemaphoreTake(s_lock, portMAX_DELAY);")
    return ("const int cur = s_cur.slot;" in b and "s_cur.slot" not in b.split("const int cur = s_cur.slot;")[1]
            and 0 < i < bl.find("if (got) s_iv[i].iv = iv;") < bl.find("xSemaphoreGive(s_lock);", i))


def sub_create() -> bool:
    """订阅：SendAutoResubscribeRequest(对端) 由 ReadClient 自己建 CASE；间隔 [1, 300]；清掉设备上旧的订阅；s->rc 先挂上再发（同步失败的回调要找它）；
       失败走 teardown（路径已经在 OnDeallocatePaths 里还了，别再 free）"""
    c = body(code("mc_sub.cpp"), "static bool create(")
    i, j = c.find("s->rc = rc;"), c.find("rc->SendAutoResubscribeRequest(peer, std::move(params));")
    k = c.find("if (e != CHIP_NO_ERROR) {", j)
    return (0 < i < j < k and "teardown(s);" in c[k:] and "MemoryFree(paths)" not in c[k:]
            and "params.mMinIntervalFloorSeconds = MC_SUB_MIN_IV_S;" in c and "params.mMaxIntervalCeilingSeconds = MC_SUB_MAX_IV_S;" in c
            and "params.mKeepSubscriptions = false;" in c)


def sub_resub_policy() -> bool:
    """断了按 9.5 自己退避（5 → 30 → 120 s），超时断的重建 CASE；设备拒了路径数就收窄重建；不用 CHIP 的默认策略（它让 LIT ICD 睡着等 check-in）"""
    r = body(code("mc_sub.cpp"), "    CHIP_ERROR OnResubscriptionNeeded(")
    r = r[:r.find("\n    }\n")] if "\n    }\n" in r else r
    m = code("mc_sub.cpp")
    rl = body(m, "static void retry_later(")
    i = r.find("if (s->retries++ == 0) {")
    return (0 < i < r.find("mc_sub_jitter(mc_sub_backoff_ms(1), esp_random())") < r.find("rc->ScheduleResubscription(ms, chip::NullOptional, cause == CHIP_ERROR_TIMEOUT)")
            and r.rstrip().endswith("return cause;")
            and "Status::PathsExhausted" in r and "s->narrow = true;" in r and "DefaultResubscribePolicy" not in m
            and "mc_sub_jitter(mc_sub_backoff_ms(s->fails + 1), esp_random())" in rl
            and "retry_later(s);" in body(m, "static void sub_tick(") and "retry_later(s);" in body(m, "    void OnDone(ReadClient *rc) override"))


def sub_lifetime() -> bool:
    """ReadClient 的生命周期：OnDone 里删它和回调对象（Close 的最后一句）；主动拆时先删 ReadClient（析构要调回调的 OnDeallocatePaths）再删回调；
       移除设备时拆订阅；同一时刻最多 MC_SUB_SETUP_MAX 条在建"""
    m = code("mc_sub.cpp")
    d, t, tk = body(m, "    void OnDone(ReadClient *rc) override"), body(m, "static void teardown("), body(m, "static void sub_tick(")
    d = d[:d.find("\n    }\n")] if "\n    }\n" in d else d
    return (d.find("chip::Platform::Delete(rc);") < d.find("chip::Platform::Delete(this);") and "chip::Platform::Delete(rc);" in d
            and 0 <= t.find("chip::Platform::Delete(s->rc);") < t.find("chip::Platform::Delete(s->cb);")
            and "setting_up < MC_SUB_SETUP_MAX" in tk and "mc_sub_judge(" in tk
            and "mc_sub_forget(node);" in body(code("mc_iv.cpp"), "void mc_iv_forget("))


def sub_wiring() -> bool:
    """访谈完（含开机读回）就按簇开订；控制器起来照表立即开订（9.5）；路径没变不动订阅"""
    iv = code("mc_iv.cpp")
    w = body(code("mc_sub.cpp"), "void mc_sub_want(")
    return ("mc_sub_want(sl->iv.node_id, &sl->iv);" in body(iv, "static void iv_end(") and "mc_sub_want(s_iv[i].iv.node_id, &s_iv[i].iv);" in body(iv, "void mc_iv_on_setup(")
            and "mc_sub_on_setup();" in body(code("mc_comm.cpp"), "void mc_comm_on_setup(") and "mc_sub_paths(iv, paths, MC_SUB_PATHS_MAX)" in w
            and "mc_sub_paths_eq(s->paths, paths, n)" in w)


def sub_cross_task() -> bool:
    """读数 / 在线在 CHIP 任务里写、别的任务经 s_lock 拷；回调里改读数在锁里"""
    m = code("mc_sub.cpp")
    a, lv = body(m, "    void OnAttributeData("), body(m, "bool matter_ctrl_node_live(")
    seq = re.search(r"xSemaphoreTake\(s_lock, portMAX_DELAY\);\s*const bool ok = mc_sub_apply\(&s->live\.st, field, &v, now, &sev\);\s*xSemaphoreGive\(s_lock\);", a)
    return bool(seq) and "xSemaphoreTake(s_lock, portMAX_DELAY);" in lv and "*out = s_sub[i].live;" in lv


def sub_audit5() -> bool:
    """第五步审计：空保活报告也算听到（NotifySubscriptionStillActive）；先置 SETUP 再发起；路径逐字段比；计时器挂不上要说、再补挂；
       移除落盘失败也拆订阅"""
    m = code("mc_sub.cpp")
    na = body(m, "    void NotifySubscriptionStillActive(")
    na = na[:na.find("\n    }\n")]
    c = body(m, "static bool create(")
    at = body(m, "static void arm_tick(")
    return ("s->live.last_ms = now_ms();" in na and 0 < c.find("set_sub(s, MC_SUB_SETUP);") < c.find("SendAutoResubscribeRequest(")
            and "mc_sub_paths_eq(s->paths, paths, n)" in body(m, "void mc_sub_want(") and "memcmp" not in body(m, "void mc_sub_want(")
            and "s_timer_on = e == CHIP_NO_ERROR;" in at and "ESP_LOGE" in at and "arm_tick();" in body(m, "void mc_sub_want(")
            and "s_timer_on = e == CHIP_NO_ERROR;" in body(code("mc_iv.cpp"), "static void arm_tick(")
            and code("mc_comm.cpp").count("mc_sub_forget(s_op.node);") == 2)


def ctl_single() -> bool:
    """控制同一时刻只做一件（拿不到锁回 busy、不排队）；结论按 gen 认领、只认一次；完成信号是静态的、下一件开始前清掉；
       兜底计时器在发起连接之前挂上；调用方等超时就放弃这件（迟到的认不上）"""
    m = code("mc_ctl.cpp")
    c, fin, st = body(m, "esp_err_t matter_ctrl_control("), body(m, "static void ctl_finish("), body(m, "static void ctl_start(")
    return ("if (xSemaphoreTake(s_mtx, 0) != pdTRUE) return ESP_ERR_TIMEOUT;" in c and "(void)xSemaphoreTake(s_done, 0);" in c
            and c.find("(void)xSemaphoreTake(s_done, 0);") < c.find("gen = ++s_op.gen;") < c.find("ScheduleWork(ctl_start, (intptr_t)gen)")
            and "if (s_op.gen == gen && !s_op.pending) {" in c and "if (done) break;" in c
            and "if (s_op.pending && s_op.gen == gen) {" in fin and "xSemaphoreCreateBinaryStatic(&s_done_buf)" in m
            and 0 < st.find("StartTimer(") < st.find("GetConnectedDevice(") and c.count("xSemaphoreGive(s_mtx);") >= 4)


def ctl_results() -> bool:
    """结论口径（9.5）：invoke 成功后 3 s 内报回目标 = confirmed，本来就是目标且订阅建好着也算；否则 delivered。
       没发出去（一定没执行）与发了没回应（可能执行了）分开；设备回的 IM 状态带出来；连接回调核对端节点号"""
    m = code("mc_ctl.cpp")
    c, oc, inv = body(m, "esp_err_t matter_ctrl_control("), body(m, "static void on_connected("), body(m, "static CHIP_ERROR invoke(")
    return ("const bool already = known && before.sub == MC_SUB_UP && mc_ctl_already(&before.st, act, level_pct);" in c
            and "mc_ctl_reached(&lv.st, act, want_on, level_pct, t0)" in c and "now_ms() - t < MC_CTL_CONFIRM_MS" in c
            and "out->result = MATTER_CTRL_CTL_DELIVERED;" in c and "if (sh->GetPeer().GetNodeId() != node) return;" in oc
            and "ctl_finish(gen, MATTER_CTRL_CTL_E_UNREACHABLE, 0);" in oc and "if (e.IsIMStatus()) {" in inv
            and "MATTER_CTRL_CTL_E_REFUSED, (uint8_t)st.mStatus" in inv and "MATTER_CTRL_CTL_E_NO_ANSWER" in inv)


def ctl_audit6() -> bool:
    """第六步审计：发了没有（sent）在调 InvokeCommandRequest 之前、锁里记下；兜底到点与调用方放弃都按 sent 分"没回应 / 连不上"；
       调用方醒来核对是不是这一件的；hub_matter 查不了敏感标记回忙（不按不敏感放行）"""
    m = code("mc_ctl.cpp")
    oc, dl, c = body(m, "static void on_connected("), body(m, "static void ctl_deadline("), body(m, "esp_err_t matter_ctrl_control(")
    hm = strip_comments((HERE.parent / "hub_core" / "hub_matter.c").read_text(encoding="utf-8"), keep_strings=True)
    ci = body(hm, "int hub_matter_ctl_info(")
    return (0 < oc.find("if (mine) s_op.sent = true;") < oc.find("invoke(em, sh, ep,")
            and "sent ? MATTER_CTRL_CTL_E_NO_ANSWER : MATTER_CTRL_CTL_E_UNREACHABLE" in dl
            and "err = s_op.sent ? MATTER_CTRL_CTL_E_NO_ANSWER : MATTER_CTRL_CTL_E_UNREACHABLE;" in c
            and "s_op.sent = false;" in c and "hub_meta_lookup_ex(HUB_META_PROTO_MATTER, id8, &m, HUB_META_LOOKUP_MS)" in ci
            and "if (lk == HUB_META_LK_UNKNOWN) rc = HUB_MT_BUSY;" in ci and "hub_meta_lookup(" not in ci)


def allow_test() -> bool:
    """"允许测试设备"（MC22，10-24 用户定）：默认关；测试 PAA 只在开关开着、生产 PAA 找不到时才查；setup 之后按开关设 CD 测试密钥；
       启动时从钩子读（读不出来按关）；切换投到 CHIP 任务里设；编译开关 SUPPORT_TEST_CD 仍是 n"""
    p = code("mc_paa.cpp")
    g = body(p, "    CHIP_ERROR GetProductAttestationAuthorityCert(")
    g = g[:g.find("\n    }\n")]
    m = code("matter_ctrl.cpp")
    st, setup, sa = body(m, "static void start_task("), body(m, "static void setup_in_chip_task("), body(m, "esp_err_t matter_ctrl_set_allow_test_devices(")
    dflt = (HERE.parent.parent / "sdkconfig.defaults.direct").read_text(encoding="utf-8")
    return ("static std::atomic<bool> s_allow_test{false};" in p
            and "if (e != CHIP_ERROR_CA_CERT_NOT_FOUND || !s_allow_test.load()) return e;" in g and "GetTestAttestationTrustStore()" in g
            and "s_hooks->allow_test_load()" in m and m.find("mc_paa_set_allow_test(s_hooks") < m.find("esp_err_t err = ident_read(&need_new);")
            and setup.find("client.setup_commissioner();") < setup.find("s_st.state = MATTER_CTRL_RUNNING;") < setup.find("mc_paa_apply_test();")
            and "ScheduleWork(apply_test_work, 0)" in sa and "v->EnableCdTestKeySupport(on);" in body(p, "void mc_paa_apply_test(")
            and "CONFIG_ESP_MATTER_COMMISSIONER_SUPPORT_TEST_CD=n" in dflt)


CHECKS = {"no_icac": no_icac, "ipk_ctrl": ipk_ctrl, "save_before_load": save_before_load, "wipes": wipes,
          "ident_lost": ident_lost, "wipe_on_start_fail": wipe_on_start_fail, "factory_reset": factory_reset,
          "msg_len": msg_len, "node_before_pair": node_before_pair, "pair_direct": pair_direct, "ble_only_rejected": ble_only_rejected,
          "pin_wiped": pin_wiped, "deadlines": deadlines, "rest_guards": rest_guards, "muse_deferred": muse_deferred,
          "stop_after_finish": stop_after_finish, "save_fail_consistent": save_fail_consistent, "boot_load_safe": boot_load_safe,
          "jobs_audit": jobs_audit, "iv_added_only": iv_added_only, "iv_one_at_a_time": iv_one_at_a_time, "iv_deadline": iv_deadline,
          "iv_forget_on_remove": iv_forget_on_remove, "iv_buf_fresh": iv_buf_fresh, "iv_complete_needed": iv_complete_needed,
          "iv_cross_task": iv_cross_task, "sub_create": sub_create, "sub_resub_policy": sub_resub_policy, "sub_lifetime": sub_lifetime,
          "sub_wiring": sub_wiring, "sub_cross_task": sub_cross_task, "sub_audit5": sub_audit5,
          "ctl_single": ctl_single, "ctl_results": ctl_results, "ctl_audit6": ctl_audit6, "allow_test": allow_test}

if __name__ == "__main__":
    if len(sys.argv) != 2 or sys.argv[1] not in CHECKS:
        sys.exit(f"用法：{sys.argv[0]} {{{'|'.join(CHECKS)}}}")
    sys.exit(0 if CHECKS[sys.argv[1]]() else 1)
