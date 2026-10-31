#!/usr/bin/env python3
"""两种产品配置的代码形状不变量（docs/15 设计 14.2 "产品配置"一行；架构 5.8 R-1、R-2、R-9）。

  python3 tools/check_profile_rules.py            # 全部
  python3 tools/check_profile_rules.py --only r9  # 只跑一组（components/*/VERIFY.sh 这样调）
  python3 tools/check_profile_rules.py --self-test   # 变异自测：每条判据改坏一处必须报红，正当写法必须不红

钉的是源码形状；"HA 型产物里没有直连型组件"（R-3）看构建产物，在 tools/check_profiles.py。
注释先剥掉再匹配（CLAUDE.md 7.4），R-2 连字符串一起查（配置键就写在字符串里）。
扫 components/ 与 main/ 下的 .c / .cpp / .cc 和所有 Kconfig（managed_components 不扫：将来 esp-matter 的
OTA Requestor 是第三条写入路径，引入时要在 matter_ctrl 里关掉它或给它加守卫，见设计 2.7 "还没做"）。
退出码 0 = 全部满足；1 = 有违规，或该找到的东西没找到（别让它恒绿）。
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from grep_code import strip_comments  # noqa: E402

REST = ROOT / "components" / "webui" / "rest_api.c"
CLOUD = ROOT / "components" / "ota_cloud" / "ota_cloud.c"
KCONFIG_MAIN = ROOT / "main" / "Kconfig.projbuild"

# 设计 2.1：这些选项只属于直连型，HA 型里看不见、打不开。还没加进 Kconfig 的跳过（加进来那天自动开始查）。
DIRECT_ONLY = (
    "BITENGINE_NET_ETH_ONLY", "BITENGINE_ENABLE_MUSE_LINK", "BITENGINE_MUSE_API_URL", "BITENGINE_MUSE_NOISE_HOST",
    "BITENGINE_MUSE_DEVICE_FAMILY", "BITENGINE_MUSE_MODEL_ID", "BITENGINE_MUSE_PAIR_WINDOW_S",
    "BITENGINE_MUSE_MIN_INTERNAL_FREE", "BITENGINE_MUSE_MIN_DMA_FREE", "BITENGINE_BLE_WINDOW_POLICY",
    "BITENGINE_ENABLE_ZHUB", "BITENGINE_ZHUB_MAX_DEVICES", "BITENGINE_ENABLE_MATTER_CTRL",
    "BITENGINE_MATTER_MAX_NODES", "BITENGINE_MATTER_BLE_COMMISSIONING",
    "BITENGINE_DIAG_HUBDB",         # M1 第二部分：hub_store 的开发期诊断端点
)
CODE_SUFFIXES = (".c", ".cpp", ".cc")
WRITE_CALLS = r"\b(?:esp_ota_begin|esp_https_ota_begin|esp_https_ota)\s*\("

# 自测时用来替换磁盘上的内容（路径 → 文本）；平时为空
OVERRIDES: dict[Path, str] = {}


def read(p: Path) -> str:
    return OVERRIDES[p] if p in OVERRIDES else p.read_text(encoding="utf-8", errors="replace")


def code_files() -> list[Path]:
    found = {f for top in ("components", "main") for f in (ROOT / top).rglob("*")
             if f.suffix in CODE_SUFFIXES and "/test_host/" not in f.as_posix()}
    found |= {p for p in OVERRIDES if p.suffix in CODE_SUFFIXES}
    return sorted(found)


def kconfig_files() -> list[Path]:
    found = {KCONFIG_MAIN} | {f for f in (ROOT / "components").rglob("Kconfig*") if f.is_file()}
    found |= {p for p in OVERRIDES if p.name.startswith("Kconfig")}
    return sorted(found)


def fn_body(code: str, name: str) -> str:
    """函数定义（行首起、以 ')' 结尾的定义行）到第一个顶格 '}'。找不到返回空串（判据随之变红）。"""
    m = re.search(r"^[^\n;]*\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\n?\{", code, re.M)
    if not m:
        return ""
    end = re.search(r"^\}", code[m.end():], re.M)
    return code[m.start(): m.end() + (end.start() if end else 0)]


def block(text: str, open_pos: int) -> str:
    depth = 0
    for i in range(open_pos, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_pos + 1: i]
    return ""


def if_block_on(text: str, var: str) -> str:
    """紧跟着的 'if (var) {' 的块内容；没有返回空串。"""
    m = re.search(r"\bif\s*\(\s*" + re.escape(var) + r"\s*\)\s*\{", text)
    return block(text, m.end() - 1) if m else ""


def statement_at(text: str, pos: int) -> str:
    """pos 所在的那条语句（上一个 ; { } 之后到下一个 ;）。"""
    start = max(text.rfind(";", 0, pos), text.rfind("{", 0, pos), text.rfind("}", 0, pos)) + 1
    end = text.find(";", pos)
    return text[start: end + 1 if end >= 0 else len(text)]


def call_args(text: str, name_pos: int) -> str:
    """name_pos 处的函数调用（名字后的第一个 '('）括号里的全部内容，允许嵌套括号。"""
    open_pos = text.find("(", name_pos)
    depth = 0
    for i in range(open_pos, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return text[open_pos + 1: i]
    return ""


def first(pattern: str, text: str) -> int:
    m = re.search(pattern, text)
    return m.start() if m else -1


# ---------------- R-1：Kconfig ----------------
def kconfig_blocks(text: str) -> dict[str, list[str]]:
    """'config NAME' / 'choice NAME' → 每一处定义的块文本（到下一个 config/choice/menu/endchoice/… 之前）。
    一个符号可以定义多处（Kconfig 允许，各处的依赖是"或"的关系：任一处没依赖，它在 HA 型里就看得见），
    所以按列表收，不能让后一处盖掉前一处。"""
    out: dict[str, list[str]] = {}
    heads = list(re.finditer(r"^\s*(?:config|menuconfig|choice)\s+(\w+)\s*$", text, re.M))
    for h in heads:
        end = re.search(r"^\s*(?:config|menuconfig|choice|endchoice|menu|endmenu|if|endif|source|comment)\b",
                        text[h.end():], re.M)
        out.setdefault(h.group(1), []).append(text[h.end(): h.end() + (end.start() if end else len(text) - h.end())])
    return out


def check_r1(bad: list[str]) -> None:
    blocks: dict[str, list[str]] = {}
    for f in kconfig_files():
        if f.exists() or f in OVERRIDES:
            for name, defs in kconfig_blocks(read(f)).items():
                blocks.setdefault(name, []).extend(defs)
    ch = "".join(blocks.get("BITENGINE_PROFILE", []))
    if not ch:
        bad.append("R-1：没有 choice BITENGINE_PROFILE（两种配置的总开关）")
    else:
        if not re.search(r"^\s*default\s+BITENGINE_PROFILE_HA\s*$", ch, re.M):
            bad.append("R-1：BITENGINE_PROFILE 的默认值必须是 BITENGINE_PROFILE_HA")
        for opt in ("BITENGINE_PROFILE_HA", "BITENGINE_PROFILE_DIRECT"):
            if opt not in blocks:
                bad.append(f"R-1：choice BITENGINE_PROFILE 里没有 {opt}")
    # 依赖要么直接写 BITENGINE_PROFILE_DIRECT，要么经过一个本身已满足的直连型选项（如 MATTER_CTRL → NET_ETH_ONLY）。
    # 表达式里有 "||" 时只认直接写的：A || B 里 A 依赖直连型推不出整个选项依赖直连型。
    present = [o for o in DIRECT_ONLY if o in blocks]
    ok: set[str] = set()

    def expr_direct(expr: str) -> bool:
        names = set(re.findall(r"\b[A-Z][A-Z0-9_]+\b", expr))
        return "BITENGINE_PROFILE_DIRECT" in names or ("||" not in expr and bool(names & ok))

    def satisfied(block: str) -> bool:
        return expr_direct(" && ".join(re.findall(r"^\s*(?:depends\s+on|def_bool|def_tristate)\s+(.+)$", block, re.M)))

    changed = True
    while changed:
        changed = False
        for o in present:
            if o not in ok and all(satisfied(b) for b in blocks[o]):      # 每一处定义都得依赖直连型
                ok.add(o)
                changed = True
    for o in present:
        if o not in ok:
            bad.append(f"R-1：{o} 没有 depends on BITENGINE_PROFILE_DIRECT（HA 型里不许看得见、打得开）")
    # select / imply 绕得过 depends on：HA 型里看得见的选项 select 一个直连型专属选项，HA 型就把它打开了。
    # 所以发起 select 的那一处定义自己得依赖直连型，或者 select 带的 if 条件依赖直连型。
    for name, defs in blocks.items():
        for b in defs:
            for m in re.finditer(r"^\s*(?:select|imply)\s+(\w+)(?:\s+if\s+(.+?))?\s*$", b, re.M):
                if m.group(1) in DIRECT_ONLY and not satisfied(b) and not expr_direct(m.group(2) or ""):
                    bad.append(f"R-1：{name} 在 HA 型里可见，却 select/imply 了直连型专属的 {m.group(1)}")


# ---------------- R-2：桥与 zhub 的分界只认编译期开关 ----------------
def check_r2(bad: list[str]) -> None:
    hits = []
    for f in code_files():
        src = strip_comments(read(f), keep_strings=True)
        # 整词：zigbee.host_ever_seen / zigbee_host_absent_s 是"上位机连过没有"的状态，不是选主机的开关
        for m in re.finditer(r"\bzigbee[._]host\b", src, re.I):
            hits.append(f"{f.relative_to(ROOT)}:{src.count(chr(10), 0, m.start()) + 1}")
    if hits:
        bad.append(f"R-2：出现了运行时的\"Zigbee 主机\"配置（zigbee.host / zigbee_host）：{hits}"
                   " —— 桥与 zhub 的分界只认 CONFIG_BITENGINE_ENABLE_ZHUB，不做运行时开关")


# ---------------- R-9：防串刷 ----------------
def check_r9(bad: list[str]) -> None:
    srcs = {f: strip_comments(read(f)) for f in code_files()}

    # 不许冒出第三条 app 写入路径：新路径必须先在这里登记、带上比对
    allowed: dict[Path, list[range]] = {}
    for f, fn in ((REST, "handle_ota_esp32"), (CLOUD, "install_task")):
        body = fn_body(srcs.get(f, ""), fn)
        if body:
            start = srcs[f].find(body)
            allowed.setdefault(f, []).append(range(start, start + len(body)))
    for f, src in srcs.items():
        for m in re.finditer(WRITE_CALLS, src):
            if not any(m.start() in r for r in allowed.get(f, [])):
                line = src.count("\n", 0, m.start()) + 1
                bad.append(f"R-9：{f.relative_to(ROOT)}:{line} 有一条没登记的 app OTA 写入路径（{m.group(0).rstrip('(').strip()}）"
                           " —— 先加工程名比对，再把它登记进 tools/check_profile_rules.py")

    # 网页上传：比对结果存进一个变量，紧接着 if (那个变量) 拒装；比对在任何写 flash 的调用之前
    web = fn_body(srcs.get(REST, ""), "handle_ota_esp32")
    gm = re.search(r"\b(\w+)\s*=\s*ota_cloud_profile_check_image\s*\(", web)
    writes = [p for p in (first(r"\besp_ota_begin\s*\(", web), first(r"\besp_ota_write\s*\(", web),
                          first(r"\besp_ota_end\s*\(", web), first(r"\besp_ota_set_boot_partition\s*\(", web)) if p >= 0]
    if not web:
        bad.append("R-9：rest_api.c 里找不到 handle_ota_esp32 —— 改名了？判据要跟着改")
    elif not gm:
        bad.append("R-9：网页上传（handle_ota_esp32）没有把 ota_cloud_profile_check_image() 的结果存进变量再判")
    elif writes and min(writes) < gm.start():
        bad.append("R-9：网页上传在比对工程名之前就开始写 flash 了")
    else:
        body = if_block_on(web[gm.end():], gm.group(1))
        need = (r"\bfree\s*\(\s*fw_buf\s*\)", r"\bs_ota_in_progress\s*=\s*false\s*;",
                r"\breturn\s+send_error\s*\([^;]*\b" + re.escape(gm.group(1)) + r"\b")
        if not body or any(not re.search(n, body) for n in need) or re.search(WRITE_CALLS, body):
            bad.append(f"R-9：网页上传要紧接着 if ({gm.group(1)}) {{ free(fw_buf); s_ota_in_progress = false; "
                       f"return send_error(..., {gm.group(1)}); }}，且不写 flash")

    # 云端：begin 之后、perform 之前读镜像描述，比的是两边的 project_name；两条失败分支都 abort、清 handle、置失败
    task = fn_body(srcs.get(CLOUD, ""), "install_task")
    b = first(r"\besp_https_ota_begin\s*\(", task)
    d = first(r"\besp_https_ota_get_img_desc\s*\(", task)
    c = first(r"\bota_profile_match\s*\(", task)
    p = first(r"\besp_https_ota_perform\s*\(", task)
    if not task:
        bad.append("R-9：ota_cloud.c 里找不到 install_task —— 改名了？判据要跟着改")
        return
    if min(b, d, c, p) < 0 or not (b < d < c < p):
        bad.append("R-9：云端安装要在 esp_https_ota_begin 之后、esp_https_ota_perform 之前 get_img_desc + ota_profile_match")
        return
    clean = (r"\besp_https_ota_abort\s*\(\s*h\s*\)", r"\bh\s*=\s*NULL\s*;", r"\bset_state\s*\(\s*OTA_CLOUD_FAILED\s*,")
    dm = re.search(r"\bif\s*\(\s*esp_https_ota_get_img_desc\s*\([^;{]*\)\s*!=\s*ESP_OK\s*\)\s*\{", task)
    dbody = block(task, dm.end() - 1) if dm else ""
    if not dbody or any(not re.search(n, dbody) for n in clean):
        bad.append("R-9：云端 get_img_desc 失败时要 esp_https_ota_abort(h)、h = NULL（不清会在 done: 二次释放）、set_state(OTA_CLOUD_FAILED, ...)")
    stmt = statement_at(task, c)
    am = re.match(r"\s*(?:const\s+)?(?:char\s*\*\s*|\w+\s+)?(\w+)\s*=", stmt)
    margs = call_args(task, c)
    if not re.search(r"->\s*project_name\b", margs) or not re.search(r"\.\s*project_name\b", margs):
        bad.append("R-9：云端比对的不是两边的 project_name（运行中的 esp_app_get_description()->project_name 与新镜像描述的 project_name）")
    if not am:
        bad.append("R-9：云端 ota_profile_match 的结果要存进变量再判")
    else:
        body = if_block_on(task[c:], am.group(1))
        if not body or any(not re.search(n, body) for n in clean):
            bad.append(f"R-9：云端比对不一致时要紧接着 if ({am.group(1)}) {{ esp_https_ota_abort(h); h = NULL; set_state(OTA_CLOUD_FAILED, ...); }}")


# ---------------- 纯以太网剖面（设计 2.2、14.2 "network_mgr（纯以太网）"一行）----------------
ETH_ONLY_EXEMPT = (ROOT / "components" / "network_mgr" / "wifi_mgr.c",     # 这两个在剖面打开时根本不编
                   ROOT / "components" / "network_mgr" / "wifi_probe.c")   # （network_mgr/CMakeLists.txt 选源文件）
WIFI_REFS = r"\besp_wifi_\w+\s*\(|\bWIFI_EVENT\b|\besp_coex_wifi_i154_enable\s*\("


def guarded_lines(src: str, macro: str) -> list[bool]:
    """逐行标出"macro 打开时这一行不编"：处在 #if !macro（或含 && !…）的分支里，或处在 #if macro 的 #else 分支里。
    只认这个宏，别的条件一概不当保护。"""
    out: list[bool] = []
    stack: list[tuple[str, bool]] = []          # (条件文本, 是否在 #else 分支)
    neg = re.compile(r"!\s*(?:defined\s*\(\s*)?" + macro + r"\b")
    pos = re.compile(r"(?<![!\w])(?:defined\s*\(\s*)?" + macro + r"\b")
    for line in src.split("\n"):
        t = line.strip()
        m = re.match(r"#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", t)
        if m:
            kw, rest = m.group(1), m.group(2)
            if kw in ("if", "ifdef", "ifndef"):
                cond = ("!" + rest.strip()) if kw == "ifndef" else rest
                stack.append((cond, False))
            elif kw == "elif" and stack:
                stack[-1] = (rest, False)
            elif kw == "else" and stack:
                stack[-1] = (stack[-1][0], True)
            elif kw == "endif" and stack:
                stack.pop()
        # "#if !X || Y" 不算保护：Y 成立时这一支照样编（审计 2026-10-22）
        guarded = any((not in_else and neg.search(c) and "||" not in c) or
                      (in_else and pos.search(c) and not neg.search(c) and "||" not in c and "&&" not in c)
                      for c, in_else in stack)
        out.append(guarded)
    return out


def eth_guarded_lines(src: str) -> list[bool]:
    return guarded_lines(src, "CONFIG_BITENGINE_NET_ETH_ONLY")


def check_eth(bad: list[str]) -> None:
    cm = read(ROOT / "components" / "network_mgr" / "CMakeLists.txt")
    if not re.search(r'if\s*\(\s*CONFIG_BITENGINE_NET_ETH_ONLY\s*\)\s*set\s*\(\s*\w+\s+"wifi_none\.c"\s*\)', cm):
        bad.append("纯以太网：network_mgr/CMakeLists.txt 没在剖面打开时用 wifi_none.c 代替 wifi_mgr.c / wifi_probe.c")
    for f in code_files():
        if f in ETH_ONLY_EXEMPT:
            continue
        src = strip_comments(read(f))
        if not re.search(WIFI_REFS, src) and f != REST:
            continue
        guard = eth_guarded_lines(src)
        for m in re.finditer(WIFI_REFS, src):
            ln = src.count("\n", 0, m.start())
            if not guard[ln]:
                bad.append(f"纯以太网：{f.relative_to(ROOT)}:{ln + 1} 的 {m.group(0).rstrip('(').strip()} 不在 "
                           "#if !CONFIG_BITENGINE_NET_ETH_ONLY 里 —— 剖面打开时会把 Wi-Fi 库链接进来")
    rsrc = strip_comments(read(REST), keep_strings=True)
    rguard = eth_guarded_lines(rsrc)
    for m in re.finditer(r'URI_ENTRY\s*\(\s*"/api/v1/(?:wifi/|network/softap|diag/wifi_probe)[^"]*"', rsrc):
        ln = rsrc.count("\n", 0, m.start())
        if not rguard[ln]:
            bad.append(f"纯以太网：rest_api.c:{ln + 1} 的路由 {m.group(0)[len('URI_ENTRY('):].strip()} 不在 "
                       "#if !CONFIG_BITENGINE_NET_ETH_ONLY 里（剖面打开时不注册 Wi-Fi 端点，设计 2.2）")
    wn = read(ROOT / "components" / "network_mgr" / "wifi_none.c")
    if re.search(WIFI_REFS, strip_comments(wn)):
        bad.append("纯以太网：wifi_none.c 里引用了 esp_wifi_* / WIFI_EVENT —— 空实现自己就把 Wi-Fi 库链接进来了")


# ---------------- R-2 另一半：直连型不带桥（设计 2.1 BITENGINE_ENABLE_ZHUB、2.6 第 5/6 步、2.8 A-04）----------------
# ENABLE_ZHUB 打开时：不编 :6638 的 TCP 服务与经 USB 的桥、SMLIGHT 兼容层，不宣告 _zigbee-*，不注册只服务 :6638 的端点；
# 直连型绝不走 IO23 复位 MG24（watchdog_reset_mg24：AM1 上 IO23 的毛刺拖垮 EN，网线常在线时还会撞 IO28）。
ZHUB = "CONFIG_BITENGINE_ENABLE_ZHUB"
BRIDGE_ONLY_SRCS = {   # CMakeLists → 只许在 HA 型（if(NOT CONFIG_BITENGINE_ENABLE_ZHUB)）里出现的源文件
    ROOT / "components" / "ezsp_bridge" / "CMakeLists.txt": ("tcp_server.c", "usb_bridge.c", "client_buffer.c"),
    ROOT / "components" / "webui" / "CMakeLists.txt": ("smlight_compat.c",),
}
BRIDGE_REFS = [  # (文件, 正则, 说明)：命中处必须在 #if !CONFIG_BITENGINE_ENABLE_ZHUB 里
    (ROOT / "components" / "mdns_svc" / "mdns_svc.c", r"_zigbee-", "_zigbee-* 宣告"),
    (REST, r'URI_ENTRY\s*\(\s*"/api/v1/(?:mode|zigbee/clients/disconnect|whitelist)"', "只服务 :6638 的端点"),
    (ROOT / "components" / "webui" / "webui_server.c", r"\bregister_smlight_handlers\s*\(", "SMLIGHT 兼容层注册"),
    (ROOT / "main" / "main.c", r"\bezsp_bridge_init\s*\(", "完整桥的初始化（直连型调 ezsp_bridge_init_uart_only）"),
    (ROOT / "components" / "ezsp_bridge" / "ezsp_bridge.c", r"\b(?:tcp_server|usb_bridge)_\w+\s*\(", "TCP / USB 桥的调用"),
]
# 拉 IO23 的两个入口。不要求后面跟括号：当函数指针传出去（比如登记成恢复回调）同样会被调到（审计 2026-10-22）。
IO23_CALL = r"\b(?:watchdog_reset_mg24|mg24_do_reset)\b"
IO23_EXEMPT = (ROOT / "components" / "webui" / "smlight_compat.c",)   # 直连型整个文件不编（上面 BRIDGE_ONLY_SRCS）
IO23_IMPL = (  # 实现本身：定义行，以及 watchdog_reset_mg24 包一层调 mg24_do_reset 的那一行（health_monitor.c）
    r"\s*esp_err_t\s+(?:watchdog_reset_mg24|mg24_do_reset)\s*\(\s*void\s*\)",
    r"\s*return\s+mg24_do_reset\s*\(\s*\)\s*;",
)
# 直连型只有 DISABLED：谁要把桥切回 LAN / USB，必须只在 HA 型（MG24 退出 bootloader 那处就是这么漏的）
SWITCH_TO_BRIDGE = r"\bezsp_bridge_switch_mode\s*\(\s*BRIDGE_MODE_(?:LAN|USB)\b"


def cmake_guarded(text: str, needle: str) -> list[bool]:
    """needle 每次出现时是否处在 if(NOT CONFIG_BITENGINE_ENABLE_ZHUB) 分支、或 if(CONFIG_BITENGINE_ENABLE_ZHUB) 的 else() 分支里。"""
    res, stack = [], []          # (条件, 是否在 else)
    for line in strip_cmake_comments(text).split("\n"):
        t = line.strip()
        m = re.match(r"(if|elseif|else|endif)\s*\((.*)\)\s*$", t, re.I)
        if m:
            kw, cond = m.group(1).lower(), m.group(2)
            if kw == "if":
                stack.append((cond, False))
            elif kw == "elseif" and stack:
                stack[-1] = (cond, False)
            elif kw == "else" and stack:
                stack[-1] = (stack[-1][0], True)
            elif kw == "endif" and stack:
                stack.pop()
        for _ in range(t.count(needle)):
            res.append(any((not e and re.fullmatch(r"\s*NOT\s+" + ZHUB + r"\s*", c)) or
                           (e and re.fullmatch(r"\s*" + ZHUB + r"\s*", c)) for c, e in stack))
    return res


def strip_cmake_comments(text: str) -> str:
    return "\n".join(re.sub(r"#.*$", "", ln) for ln in text.split("\n"))


def check_bridge(bad: list[str]) -> None:
    for cm, srcs in BRIDGE_ONLY_SRCS.items():
        text = read(cm)
        for src in srcs:
            hits = cmake_guarded(text, f'"{src}"')
            if not hits:
                bad.append(f"桥：{cm.relative_to(ROOT)} 里找不到 \"{src}\" —— 判据要跟着改")
            elif not all(hits):
                bad.append(f"桥：{cm.relative_to(ROOT)} 的 {src} 不在 if(NOT {ZHUB}) 里 —— 直连型会把桥编进来（R-2）")
    for f, pat, what in BRIDGE_REFS:
        src = strip_comments(read(f), keep_strings=True)
        guard = guarded_lines(src, ZHUB)
        found = list(re.finditer(pat, src))
        if not found:
            bad.append(f"桥：{f.relative_to(ROOT)} 里找不到 {what}（{pat}）—— 判据要跟着改")
        for m in found:
            ln = src.count("\n", 0, m.start())
            if not guard[ln]:
                bad.append(f"桥：{f.relative_to(ROOT)}:{ln + 1} 的{what}不在 #if !{ZHUB} 里（直连型不带桥，R-2 / A-04）")
    for f in code_files():
        if f in IO23_EXEMPT:
            continue
        src = strip_comments(read(f))
        if not re.search(IO23_CALL, src):
            continue
        guard = guarded_lines(src, ZHUB)
        for m in re.finditer(IO23_CALL, src):
            ln = src.count("\n", 0, m.start())
            line = src.split("\n")[ln]
            if any(re.match(p, line) for p in IO23_IMPL):
                continue                                   # 实现本身（mg24_reset.c / health_monitor.c）
            if not guard[ln]:
                bad.append(f"桥：{f.relative_to(ROOT)}:{ln + 1} 引用 {m.group(0)}（IO23）不在 #if !{ZHUB} 里 —— "
                           "直连型绝不走 IO23（设计 2.8 \"重启 Zigbee 芯片\"一行）")
    for f in code_files():
        src = strip_comments(read(f))
        if not re.search(SWITCH_TO_BRIDGE, src):
            continue
        guard = guarded_lines(src, ZHUB)
        for m in re.finditer(SWITCH_TO_BRIDGE, src):
            ln = src.count("\n", 0, m.start())
            if not guard[ln]:
                bad.append(f"桥：{f.relative_to(ROOT)}:{ln + 1} 把桥切回 LAN / USB 不在 #if !{ZHUB} 里 —— "
                           "直连型只有 DISABLED，这一下会回 NOT_SUPPORTED 并被当成失败")


GROUPS = {"r1": check_r1, "r2": check_r2, "r9": check_r9, "eth": check_eth, "bridge": check_bridge}


def run(groups) -> list[str]:
    bad: list[str] = []
    for name in groups:
        GROUPS[name](bad)
    return bad


# ---------------- 变异自测 ----------------
def self_test() -> int:
    # 锚点要唯一：Kconfig 里不止一个 choice（2026-10-22 加了 BLE 窗口策略），只认总开关那个 choice 的结尾
    KC_END = "            bool \"Direct (Hub + Muse, Ethernet only) — in development\"\n    endchoice\n"
    muse = "\n    config BITENGINE_ENABLE_MUSE_LINK\n        bool \"m\"\n"
    M = [  # (说明, 文件, 原文 or None=追加, 新文, 期望红)
        ("R-1 默认改成直连型", KCONFIG_MAIN, "        default BITENGINE_PROFILE_HA\n", "        default BITENGINE_PROFILE_DIRECT\n", True),
        ("R-1 少了 DIRECT 选项", KCONFIG_MAIN, "        config BITENGINE_PROFILE_DIRECT\n", "        config BITENGINE_PROFILE_XX\n", True),
        ("R-1 纯以太网开关的依赖被删掉", KCONFIG_MAIN,
         "    config BITENGINE_NET_ETH_ONLY\n        bool \"Ethernet-only network profile (no Wi-Fi)\"\n        depends on BITENGINE_PROFILE_DIRECT\n",
         "    config BITENGINE_NET_ETH_ONLY\n        bool \"Ethernet-only network profile (no Wi-Fi)\"\n", True),
        ("R-1 同一开关在别处再定义一次、那一处没依赖", KCONFIG_MAIN, KC_END, KC_END + muse, True),
        ("R-1 再定义的那一处依赖写对 → 不红", KCONFIG_MAIN, KC_END, KC_END + muse + "        depends on BITENGINE_PROFILE_DIRECT && SPIRAM\n", False),
        ("R-1 HA 型可见的选项 select 直连型专属选项", KCONFIG_MAIN, KC_END, KC_END +
         "\n    config BITENGINE_XX_SELECTOR\n        bool \"x\"\n        select BITENGINE_ENABLE_MUSE_LINK\n", True),
        ("R-1 imply 同样算", KCONFIG_MAIN, KC_END, KC_END +
         "\n    config BITENGINE_XX_SELECTOR\n        bool \"x\"\n        imply BITENGINE_NET_ETH_ONLY\n", True),
        ("R-1 发起 select 的选项自己依赖直连型 → 不红", KCONFIG_MAIN, KC_END, KC_END +
         "\n    config BITENGINE_XX_SELECTOR\n        bool \"x\"\n        depends on BITENGINE_PROFILE_DIRECT\n        select BITENGINE_ENABLE_MUSE_LINK\n", False),
        ("R-1 select 带 if 直连型 → 不红", KCONFIG_MAIN, KC_END, KC_END +
         "\n    config BITENGINE_XX_SELECTOR\n        bool \"x\"\n        select BITENGINE_ENABLE_MUSE_LINK if BITENGINE_PROFILE_DIRECT\n", False),
        ("R-1 Muse 开关的依赖被删掉", KCONFIG_MAIN, "        depends on BITENGINE_PROFILE_DIRECT && SPIRAM\n",
         "        depends on SPIRAM\n", True),
        ("R-1 传递依赖 && → 不红", KCONFIG_MAIN, KC_END, KC_END +
         "\n    config BITENGINE_ENABLE_MATTER_CTRL\n        bool \"x\"\n        depends on BITENGINE_NET_ETH_ONLY && SPIRAM\n", False),
        ("R-1 传递依赖里有 || → 红", KCONFIG_MAIN, KC_END, KC_END +
         "\n    config BITENGINE_ENABLE_MATTER_CTRL\n        bool \"x\"\n        depends on BITENGINE_NET_ETH_ONLY || SPIRAM\n", True),
        ("R-1 组件自己的 Kconfig 里放了没依赖的直连型选项", ROOT / "components" / "zhub" / "Kconfig", None,
         "menu \"zhub\"\n    config BITENGINE_ZHUB_MAX_DEVICES\n        int \"z\"\nendmenu\n", True),
        ("R-2 代码里读 zigbee.host", ROOT / "components" / "config_mgr" / "config_mgr.c", None,
         "\nstatic int zz(void){ return g_config.zigbee.host; }\n", True),
        ("R-2 配置键 \"zigbee_host\"（字符串里）", REST, None, '\nstatic const char *zk = "zigbee_host";\n', True),
        ("R-2 zigbee.host_ever_seen 是状态 → 不红", ROOT / "components" / "config_mgr" / "config_mgr.c", None,
         "\nstatic int zy(void){ return g_config.zigbee.host_ever_seen; }\n", False),
        ("R-9 网页上传先写 flash 再比对", REST, "    const char *pwhy = ota_cloud_profile_check_image(",
         "    { esp_ota_handle_t x; esp_ota_begin(update_part, OTA_SIZE_UNKNOWN, &x); }\n    const char *pwhy = ota_cloud_profile_check_image(", True),
        ("R-9 网页上传去掉比对", REST, "    const char *pwhy = ota_cloud_profile_check_image(fw_buf, (size_t)received);",
         "    const char *pwhy = NULL; (void)fw_buf;", True),
        ("R-9 网页上传拒装时不复位进行中标志", REST, "        free(fw_buf);\n        s_ota_in_progress = false;\n        esp_event_post(WEBUI_EVENT, WEBUI_EVENT_OTA_FAILED",
         "        free(fw_buf);\n        esp_event_post(WEBUI_EVENT, WEBUI_EVENT_OTA_FAILED", True),
        ("R-9 网页上传判的是别的变量（审计①）", REST, "    if (pwhy) {\n        free(fw_buf);",
         "    const char *pother = NULL;\n    if (pother) {\n        free(fw_buf);", True),
        ("R-9 云端在 perform 之后才比对", CLOUD, "        esp_app_desc_t new_desc;\n",
         "        (void)esp_https_ota_perform(h);\n        esp_app_desc_t new_desc;\n", True),
        ("R-9 云端拒装后不清 handle", CLOUD, "            esp_https_ota_abort(h);\n            h = NULL;\n            set_state(OTA_CLOUD_FAILED, pwhy);",
         "            esp_https_ota_abort(h);\n            set_state(OTA_CLOUD_FAILED, pwhy);", True),
        ("R-9 云端 get_img_desc 失败不清 handle（审计②）", CLOUD,
         "            esp_https_ota_abort(h);\n            h = NULL;               /* 同下：abort 已释放 handle */",
         "            esp_https_ota_abort(h);", True),
        ("R-9 云端比的是 version 不是 project_name（审计③）", CLOUD,
         "ota_profile_match(esp_app_get_description()->project_name,\n                                                             new_desc.project_name, sizeof(new_desc.project_name))",
         "ota_profile_match(esp_app_get_description()->version,\n                                                             new_desc.version, sizeof(new_desc.version))", True),
        ("R-9 云端判的是别的变量", CLOUD, "        if (pwhy) {\n            esp_https_ota_abort(h);",
         "        const char *pother = NULL;\n        if (pother) {\n            esp_https_ota_abort(h);", True),
        ("R-9 冒出第三条写入路径（.c）", ROOT / "components" / "ota_mgr" / "ota_mg24.c", None,
         "\nstatic void zz9(void){ esp_ota_handle_t h; esp_ota_begin(NULL, 0, &h); }\n", True),
        ("R-9 冒出第三条写入路径（.cpp）", ROOT / "components" / "zz_new" / "zz.cpp", None,
         "void zz8() { esp_https_ota_handle_t h; esp_https_ota_begin(nullptr, &h); }\n", True),
        ("纯以太网：health_monitor 的 RSSI 查询露在保护外", ROOT / "components" / "watchdog" / "health_monitor.c", None,
         "\nstatic void zz7(void){ wifi_ap_record_t ap; (void)esp_wifi_sta_get_ap_info(&ap); }\n", True),
        ("纯以太网：net_events 的 WIFI_EVENT 注册去掉保护", ROOT / "components" / "network_mgr" / "net_events.c",
         "#if !CONFIG_BITENGINE_NET_ETH_ONLY\n    err = esp_event_handler_instance_register(\n        WIFI_EVENT",
         "#if 1\n    err = esp_event_handler_instance_register(\n        WIFI_EVENT", True),
        ("纯以太网：thread_br 共存调用去掉 && !ETH_ONLY", ROOT / "components" / "thread_br" / "thread_br.c",
         "CONFIG_SOC_IEEE802154_SUPPORTED && !CONFIG_BITENGINE_NET_ETH_ONLY", "CONFIG_SOC_IEEE802154_SUPPORTED", True),
        ("纯以太网：#else 分支写反（在 ETH_ONLY 那一支调驱动）", ROOT / "components" / "watchdog" / "health_monitor.c",
         "#if CONFIG_BITENGINE_NET_ETH_ONLY\n    /* 纯以太网剖面", "#if !CONFIG_BITENGINE_NET_ETH_ONLY\n    /* 纯以太网剖面", True),
        ("纯以太网：/wifi/scan 路由露在保护外", REST,
         '#if !CONFIG_BITENGINE_NET_ETH_ONLY   /* 纯以太网剖面不注册 /wifi 下的端点（设计 2.2） */\n        URI_ENTRY("/api/v1/wifi/scan"',
         '        URI_ENTRY("/api/v1/wifi/scan"', True),
        ("纯以太网：wifi_none.c 自己调了驱动", ROOT / "components" / "network_mgr" / "wifi_none.c", None,
         "\nstatic void zz6(void){ (void)esp_wifi_disconnect(); }\n", True),
        ("纯以太网：CMake 不再换掉 wifi_mgr.c", ROOT / "components" / "network_mgr" / "CMakeLists.txt",
         'set(_bitengine_wifi_srcs "wifi_none.c")', 'set(_bitengine_wifi_srcs "wifi_mgr.c" "wifi_probe.c")', True),
        ("纯以太网：别的宏的 #if 不算保护", ROOT / "components" / "watchdog" / "health_monitor.c", None,
         "\n#if CONFIG_BITENGINE_SVC_IDENTITY\nstatic void zz5(void){ (void)esp_wifi_stop(); }\n#endif\n", True),
        # ---- 桥（R-2 另一半）----
        ("桥：mDNS 多了一处没保护的 _zigbee-coordinator", ROOT / "components" / "mdns_svc" / "mdns_svc.c", None,
         '\nstatic const char *zb1 = "_zigbee-coordinator";\n', True),
        ("桥：保护好的 _zigbee-bridge → 不红", ROOT / "components" / "mdns_svc" / "mdns_svc.c", None,
         '\n#if !CONFIG_BITENGINE_ENABLE_ZHUB\nstatic const char *zb2 = "_zigbee-bridge";\n#endif\n', False),
        ("桥：/whitelist 路由露在保护外", REST, None,
         '\nstatic const int zb3[] = { URI_ENTRY("/api/v1/whitelist", HTTP_GET, handle_whitelist_get) };\n', True),
        ("桥：直连型能走到 IO23 复位", REST, None, "\nstatic void zb4(void){ (void)watchdog_reset_mg24(); }\n", True),
        ("桥：IO23 在 #if ZHUB 的 #else 里 → 不红", REST, None,
         "\n#if CONFIG_BITENGINE_ENABLE_ZHUB\n#else\nstatic void zb8(void){ (void)watchdog_reset_mg24(); }\n#endif\n", False),
        ("桥：别的宏的 #if 不算保护（IO23）", REST, None,
         "\n#if CONFIG_BITENGINE_HAS_MG24_RESET_PIN\nstatic void zb9(void){ (void)watchdog_reset_mg24(); }\n#endif\n", True),
        ("桥：main.c 直连型也调完整桥初始化", ROOT / "main" / "main.c", None,
         "\nstatic void zb5(void){ (void)ezsp_bridge_init(); }\n", True),
        ("桥：ezsp_bridge.c 直连型也起 TCP 服务", ROOT / "components" / "ezsp_bridge" / "ezsp_bridge.c", None,
         "\nstatic void zb6(void){ (void)tcp_server_start(); }\n", True),
        ("桥：直连型也注册 SMLIGHT 兼容层", ROOT / "components" / "webui" / "webui_server.c", None,
         "\nstatic void zb7(void){ register_smlight_handlers(0); }\n", True),
        ("桥：CMake 在保护外又列了 tcp_server.c", ROOT / "components" / "ezsp_bridge" / "CMakeLists.txt", None,
         '\nset(_zb10 "tcp_server.c")\n', True),
        ("桥：CMake 在保护外又列了 smlight_compat.c", ROOT / "components" / "webui" / "CMakeLists.txt", None,
         '\nset(_zb11 "smlight_compat.c")\n', True),
        ("桥：IO23 当函数指针传出去", REST, None, "\nstatic void *zb12 = (void *)watchdog_reset_mg24;\n", True),
        ("桥：直接调 mg24_do_reset", ROOT / "components" / "watchdog" / "wd_recovery.c", None,
         "\nstatic void zb13(void){ (void)mg24_do_reset(); }\n", True),
        ("桥：#if !ZHUB || X 不算保护", REST, None,
         "\n#if !CONFIG_BITENGINE_ENABLE_ZHUB || CONFIG_FOO\nstatic void zb14(void){ (void)watchdog_reset_mg24(); }\n#endif\n", True),
        ("桥：MG24 退出 bootloader 后切回 LAN 露在保护外", ROOT / "components" / "ota_mgr" / "ota_mg24.c",
         "#if !CONFIG_BITENGINE_ENABLE_ZHUB   /* 直连型没有桥", "#if CONFIG_BITENGINE_FOO   /* 直连型没有桥", True),
        ("桥：别处切 USB 露在保护外", ROOT / "components" / "button" / "button.c", None,
         "\nstatic void zb15(void){ (void)ezsp_bridge_switch_mode(BRIDGE_MODE_USB); }\n", True),
    ]
    fails = 0
    base = run(sorted(GROUPS))
    if base:
        print("❌ 原样就是红的，先修再自测：", base)
        return 1
    for why, path, old, new, want_red in M:
        OVERRIDES.clear()
        orig = read(path) if path.exists() else ""
        if old is not None and orig.count(old) != 1:
            print(f"⚠️ {why}：锚点出现 {orig.count(old)} 次，变异没做成（代码改了？同步改自测）")
            fails += 1
            continue
        OVERRIDES[path] = orig + new if old is None else orig.replace(old, new)
        red = bool(run(sorted(GROUPS)))
        good = red == want_red
        print(("✅ " if good else "❌ ") + ("抓到 " if want_red else "不红 ") + why)
        fails += 0 if good else 1
    OVERRIDES.clear()
    print(f"\n自测：{'全部符合' if not fails else f'{fails} 处不符合'}（{len(M)} 条）")
    return 1 if fails else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", choices=sorted(GROUPS), action="append")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    groups = a.only or sorted(GROUPS)
    bad = run(groups)
    if bad:
        print("❌ 产品配置不变量（docs/15 设计 14.2）：")
        for x in bad:
            print("   · " + x)
        return 1
    print("✅ 产品配置不变量：" + "、".join(n.upper() for n in groups))
    return 0


if __name__ == "__main__":
    sys.exit(main())
