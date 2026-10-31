#!/usr/bin/env python3
"""WiFi 只许 5 GHz（CLAUDE.md 关键设计决策 4）—— 四道防线的结构检查（2026-10-10，10-21 收紧）。

只锁 band mode 不够：射频预扫时拧下 ANT2、5 GHz 看不见，网页触发的扫描没限定频段、把 2.4 GHz 也扫了，
同时进行的重连从扫描结果里挑中了同名路由器的 2.4 GHz（日志 "connected with MyWiFi ... channel 11"）；
随后开热点，ch36 配置被驱动拒绝，模式却没改回，热点就在 2.4 GHz 上开着、还连上了客户端。

钉住的事（只看代码，注释和字符串先剥掉；扫 components/ 与 main/ 全部 .c，不只本组件）：
  1. esp_wifi_set_band_mode( 全仓只有一处，在 wifi_lock_5g 里：先读回、已是 5G_ONLY 就返回 OK（驱动在
     扫描 / 连接中 / 热点有客户端时拒绝改频段），否则设 5G_ONLY 并原样交回结果；没有旧接口 esp_wifi_set_band(
  2. esp_wifi_connect( 全仓只在 wifi_mgr.c，而且每一处都紧跟在
     "err = wifi_lock_5g(...); if (err == ESP_OK)" 之后 —— 锁没上成功就不连，失败照样走重排
  3. esp_wifi_scan_start( 全仓只有一处，在 wifi_mgr_scan_5g 里，传的是设了位图的 scan_cfg：
     跳过 2.4 GHz、扫全部 5 GHz、不设 .channel（设了位图就被忽略）
  4. net_events.c：STA_CONNECTED 里 "if (... wifi_mgr_reject_24g_link(...)) break;" 在标记已连上之前；
     STA_GOT_IP 里先 "if (!wifi_mgr_is_connected()) { ... break; }"（静态 IP 时 GOT_IP 不等 DHCP 就来）；
     reject 先断开再上锁，热点开着就记下；STA_DISCONNECTED 里调 wifi_mgr_softap_recheck() 把被拖走的热点拉回 ch36
  5. 热点：wifi_mgr_start_softap 已开着就直接返回；锁 / 配置失败就改回 STA 并返回错误，成功后才置 s_softap_active；
     network_mgr.c 的 s_softap_on 只在 wifi_mgr_start_softap() 返回 ESP_OK 之后置 true

用法：python3 check_band_lock.py [组件目录]   （VERIFY.sh 在组件目录里调）
退出码 0 = 全部满足；1 = 有违规（或解析不到该有的东西 —— 别让它恒绿）。
改完这个脚本要做变异测试：每条判据各改坏一处，确认它会红（做法见 memory mutation-testing-in-scratchpad）。
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from grep_code import strip_comments  # noqa: E402


def fn_body(code: str, name: str) -> str:
    """取函数定义（行首起、以 ')' 结尾的定义行）到第一个顶格 '}' 的函数体。找不到返回空串（判据随之变红）。"""
    m = re.search(r"^[^\n;]*\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\n?\{", code, re.M)
    if not m:
        return ""
    end = re.search(r"^\}", code[m.end():], re.M)
    return code[m.start(): m.end() + (end.start() if end else 0)]


def case_body(code: str, label: str) -> str:
    """switch 里 'case <label>:' 到下一个 case / default 标签之间的文本。"""
    m = re.search(r"\bcase\s+" + re.escape(label) + r"\s*:", code)
    if not m:
        return ""
    nxt = re.search(r"\bcase\s+\w+\s*:|\bdefault\s*:", code[m.end():])
    return code[m.end(): m.end() + (nxt.start() if nxt else len(code) - m.end())]


def block(text: str, open_pos: int) -> str:
    """text[open_pos] 是 '{'：返回配对的 '}' 之前的内容（允许嵌套）。不配对返回空串。"""
    depth = 0
    for i in range(open_pos, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_pos + 1: i]
    return ""


def if_block(text: str, cond: str) -> tuple[str, int, int]:
    """找 'if (<cond>) {'（cond 是正则），返回 (块内容, if 起点, 块结束位置)；找不到返回 ("", -1, -1)。"""
    m = re.search(r"\bif\s*\(\s*" + cond + r"\s*\)\s*\{", text)
    if not m:
        return "", -1, -1
    body = block(text, m.end() - 1)
    return body, m.start(), m.end() + len(body)


def count(pattern: str, text: str) -> int:
    return len(re.findall(pattern, text))


def main() -> int:
    d = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    repo = d.resolve().parents[1]
    code = {f: strip_comments(f.read_text(encoding="utf-8", errors="replace"))
            for top in ("components", "main") for f in sorted((repo / top).rglob("*.c"))
            if "/test_host/" not in f.as_posix()}
    wifi_path = (d / "wifi_mgr.c").resolve()
    wifi = code.get(wifi_path, "")
    events = code.get((d / "net_events.c").resolve(), "")
    netmgr = code.get((d / "network_mgr.c").resolve(), "")
    bad: list[str] = []
    if not wifi or not events or not netmgr:
        bad.append(f"读不到 {d}/wifi_mgr.c / net_events.c / network_mgr.c —— 路径不对？")

    def where(pat: str) -> list[str]:
        return [f"{f.relative_to(repo)}×{count(pat, c)}" for f, c in code.items() if re.search(pat, c)]

    # 1. 频段锁只有一处：先读回，已是 5G_ONLY 不再设；要设就设 5G_ONLY 并原样交回结果
    hits = where(r"\besp_wifi_set_band_mode\s*\(")
    if hits != [f"{wifi_path.relative_to(repo)}×1"]:
        bad.append(f"esp_wifi_set_band_mode( 应全仓只在 wifi_mgr.c 出现 1 次（wifi_lock_5g），实际：{hits or '没有'}")
    lock = fn_body(wifi, "wifi_lock_5g")
    if not re.search(r"\besp_wifi_get_band_mode\s*\(\s*&\s*(\w+)\s*\)\s*==\s*ESP_OK\s*&&\s*\1\s*==\s*WIFI_BAND_MODE_5G_ONLY\s*\)"
                     r"\s*return\s+ESP_OK\s*;", lock):
        bad.append("wifi_lock_5g() 要先 'if (esp_wifi_get_band_mode(&x) == ESP_OK && x == WIFI_BAND_MODE_5G_ONLY) return ESP_OK;'")
    if not re.search(r"\besp_err_t\s+err\s*=\s*esp_wifi_set_band_mode\s*\(\s*WIFI_BAND_MODE_5G_ONLY\s*\)\s*;", lock) \
            or count(r"\berr\s*=(?!=)", lock) != 1 or not re.search(r"\breturn\s+err\s*;\s*$", lock.rstrip().rstrip("}").rstrip()):
        bad.append("wifi_lock_5g() 要 'esp_err_t err = esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY);'、err 不再被改写、最后 'return err;'")
    legacy = where(r"\besp_wifi_set_band\s*\(")
    if legacy:
        bad.append(f"代码里出现了旧接口 esp_wifi_set_band(：{legacy}")

    # 2. 每次连接前上锁，锁失败不连
    hits = where(r"\besp_wifi_connect\s*\(")
    if [h for h in hits if not h.startswith(str(wifi_path.relative_to(repo)))]:
        bad.append(f"esp_wifi_connect() 只许在 wifi_mgr.c 里调（每次先上 5 GHz 锁），实际：{hits}")
    n_conn = count(r"\besp_wifi_connect\s*\(", wifi)
    n_guarded = count(r"\berr\s*=\s*wifi_lock_5g\s*\([^;]*\)\s*;\s*"
                      r"if\s*\(\s*err\s*==\s*ESP_OK\s*\)\s*err\s*=\s*esp_wifi_connect\s*\(\s*\)\s*;", wifi)
    if n_conn == 0:
        bad.append("wifi_mgr.c 里一个 esp_wifi_connect( 都没找到 —— 解析坏了？")
    elif n_guarded != n_conn:
        bad.append(f"wifi_mgr.c 有 {n_conn} 处 esp_wifi_connect()，只有 {n_guarded} 处写成 "
                   "'err = wifi_lock_5g(...); if (err == ESP_OK) err = esp_wifi_connect();'")

    # 3. 扫描只有一处，跳过 2.4 GHz
    hits = where(r"\besp_wifi_scan_start\s*\(")
    if hits != [f"{wifi_path.relative_to(repo)}×1"]:
        bad.append(f"esp_wifi_scan_start( 应全仓只在 wifi_mgr_scan_5g 里出现 1 次，实际：{hits or '没有'}")
    scan = fn_body(wifi, "wifi_mgr_scan_5g")
    init = re.search(r"\bwifi_scan_config_t\s+scan_cfg\s*=\s*\{(.*?)\};", scan, re.S)
    init = init.group(1) if init else ""
    if not re.search(r"\.ghz_2_channels\s*=\s*WIFI_SCAN_BYPASS_BAND\b", init):
        bad.append("wifi_mgr_scan_5g() 的 scan_cfg 没有 .ghz_2_channels = WIFI_SCAN_BYPASS_BAND")
    if not re.search(r"\.ghz_5_channels\s*=\s*WIFI_SCAN_5G_ALL\b", init):
        bad.append("wifi_mgr_scan_5g() 的 scan_cfg 没有 .ghz_5_channels = WIFI_SCAN_5G_ALL")
    if re.search(r"\bscan_cfg\s*\.\s*channel_bitmap\b", scan) or count(r"\bghz_2_channels\s*=", wifi) != 1:
        bad.append("扫描位图只许在 scan_cfg 的初始化里设一次（初始化之后再改 = 绕过跳过 2.4 GHz）")
    if re.search(r"\.channel\s*=", init) or re.search(r"\bscan_cfg\s*\.\s*channel\s*=", scan):
        bad.append("wifi_mgr_scan_5g() 设了 .channel —— 非 0 时位图被忽略、按单信道扫")
    if not re.search(r"\besp_wifi_scan_start\s*\(\s*&\s*scan_cfg\s*,", scan):
        bad.append("wifi_mgr_scan_5g() 没有把 &scan_cfg 传给 esp_wifi_scan_start()（传 NULL = 全频段）")
    if not re.search(r"#define\s+WIFI_SCAN_BYPASS_BAND\s+1u?\b", wifi):
        bad.append("WIFI_SCAN_BYPASS_BAND 应为 1（ghz_2_channels 的 bit0 = 整段跳过）")
    if not re.search(r"#define\s+WIFI_SCAN_5G_ALL\s+0x1FFFFFFEu?\b", wifi):
        bad.append("WIFI_SCAN_5G_ALL 应为 0x1FFFFFFE（wifi_5g_channel_bit_t 的 bit1..bit28 = ch36..ch177）")

    # 4. 落在 2.4 GHz 的关联不算连上，它的 GOT_IP 也不算；被它拖走的热点拉回 ch36
    conn = case_body(events, "WIFI_EVENT_STA_CONNECTED")
    rej = re.search(r"\bif\s*\([^;{]*\bwifi_mgr_reject_24g_link\s*\([^;{]*\)\s*\)\s*break\s*;", conn)
    up = re.search(r"\bwifi_mgr_set_sta_connected\s*\(\s*true\s*\)", conn)
    if not rej:
        bad.append("net_events.c 的 STA_CONNECTED 没有 'if (... wifi_mgr_reject_24g_link(...)) break;'")
    elif not up or up.start() < rej.start():
        bad.append("net_events.c：wifi_mgr_set_sta_connected(true) 不在 2.4 GHz 检查之后")
    got = case_body(events, "IP_EVENT_STA_GOT_IP")
    gbody, gpos, _ = if_block(got, r"!\s*wifi_mgr_is_connected\s*\(\s*\)")
    sm = got.find("NET_EVT_WIFI_GOT_IP")
    if gpos < 0 or not re.search(r"\bbreak\s*;", gbody) or sm < 0 or sm < gpos:
        bad.append("net_events.c 的 STA_GOT_IP 要先 'if (!wifi_mgr_is_connected()) { ... break; }' 再进状态机")
    disc = case_body(events, "WIFI_EVENT_STA_DISCONNECTED")
    if not re.search(r"\bwifi_mgr_softap_recheck\s*\(\s*\)\s*;", disc):
        bad.append("net_events.c 的 STA_DISCONNECTED 没有调 wifi_mgr_softap_recheck()（被拒的关联把热点拖到 2.4 GHz 后没人拉回）")
    rejf = fn_body(wifi, "wifi_mgr_reject_24g_link")
    if not re.search(r"\bif\s*\(\s*channel\s*==\s*0\s*\|\|\s*channel\s*>\s*WIFI_2G_MAX_CHANNEL\s*\)\s*return\s+false\s*;", rejf) \
            or not re.search(r"\breturn\s+true\s*;", rejf):
        bad.append("wifi_mgr_reject_24g_link() 要 'if (channel == 0 || channel > WIFI_2G_MAX_CHANNEL) return false;'，其余断开并返回 true")
    d_at, l_at = rejf.find("esp_wifi_disconnect"), rejf.find("wifi_lock_5g")
    if d_at < 0 or l_at < 0 or l_at < d_at:
        bad.append("wifi_mgr_reject_24g_link() 要先 esp_wifi_disconnect() 再 wifi_lock_5g()（关联着改不了频段）")
    if not re.search(r"\bif\s*\(\s*s_softap_active\s*\)\s*s_softap_recheck\s*=\s*true\s*;", rejf):
        bad.append("wifi_mgr_reject_24g_link() 热点开着时要 's_softap_recheck = true'")
    rc = fn_body(wifi, "wifi_mgr_softap_recheck")
    if not re.search(r"\berr\s*=\s*wifi_lock_5g\s*\([^;]*\)\s*;\s*if\s*\(\s*err\s*==\s*ESP_OK\s*\)\s*err\s*=\s*esp_wifi_set_config\s*\(\s*WIFI_IF_AP\b", rc):
        bad.append("wifi_mgr_softap_recheck() 要先上锁再下发 ch36 的热点配置")
    rbody, _, _ = if_block(rc, r"err\s*!=\s*ESP_OK")
    if not re.search(r"\bwifi_mgr_stop_softap\s*\(\s*\)", rbody):
        bad.append("wifi_mgr_softap_recheck() 拉不回 5 GHz 时要关掉热点（决策 5）")
    if not re.search(r"\.channel\s*=\s*36\s*,", fn_body(wifi, "softap_build_config")):
        bad.append("softap_build_config() 的热点信道应为 36（决策 5）")
    if not re.search(r"#define\s+WIFI_2G_MAX_CHANNEL\s+14\b", wifi):
        bad.append("WIFI_2G_MAX_CHANNEL 应为 14（2.4 GHz 是 ch1–14）")

    # 5. 热点失败回滚
    ap = fn_body(wifi, "wifi_mgr_start_softap")
    if not re.search(r"\{\s*if\s*\(\s*s_softap_active\s*\)\s*return\s+ESP_OK\s*;", ap):
        bad.append("wifi_mgr_start_softap() 开头要 'if (s_softap_active) return ESP_OK;'（重跑失败会拆掉开着的热点）")
    m = re.search(r"\berr\s*=\s*wifi_lock_5g\s*\([^;]*\)\s*;\s*if\s*\(\s*err\s*==\s*ESP_OK\s*\)\s*"
                  r"err\s*=\s*esp_wifi_set_config\s*\(\s*WIFI_IF_AP\b[^;]*;\s*", ap)
    fbody, fpos, fend = if_block(ap[m.end():], r"err\s*!=\s*ESP_OK") if m else ("", -1, -1)
    if not m or fpos != 0 or not re.search(r"esp_wifi_set_mode\s*\(\s*WIFI_MODE_STA\s*\)", fbody) \
            or not re.search(r"\breturn\s+err\s*;", fbody):
        bad.append("wifi_mgr_start_softap()：锁 / 配置失败时要改回 WIFI_MODE_STA 并 return err")
    else:
        act = re.search(r"\bs_softap_active\s*=\s*true\s*;", ap)
        if not act or act.start() < m.end() + fend:
            bad.append("wifi_mgr_start_softap()：s_softap_active = true 要在失败回滚之后")
    n_on = count(r"\bs_softap_on\s*=\s*true\s*;", netmgr)
    n_on_ok = count(r"(?:\bwifi_mgr_start_softap\s*\(\s*\)|\berr)\s*==\s*ESP_OK\s*\)\s*\{?\s*"
                    r"(?:s_state\s*=\s*\w+\s*;\s*)?s_softap_on\s*=\s*true\s*;", netmgr)
    if n_on == 0 or n_on_ok != n_on:
        bad.append(f"network_mgr.c 有 {n_on} 处 s_softap_on = true，只有 {n_on_ok} 处在 wifi_mgr_start_softap() 成功之后")
    if not re.search(r"\berr\s*=\s*wifi_mgr_start_softap\s*\(\s*\)\s*;", fn_body(netmgr, "network_mgr_start_softap")):
        bad.append("network_mgr_start_softap() 要先 'err = wifi_mgr_start_softap();' 再按结果置 s_softap_on")

    if bad:
        print("❌ 5 GHz 锁的结构检查不过：")
        for b in bad:
            print("   · " + b)
        return 1
    print("✅ 5 GHz 锁：频段锁一处（先读回）、每次连接前上锁、扫描跳过 2.4G、2.4G 关联即断且不认它的 GOT_IP、被拖走的热点拉回 ch36、热点失败回滚")
    return 0


if __name__ == "__main__":
    sys.exit(main())
