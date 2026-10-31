#!/usr/bin/env python3
"""两种产品配置的构建产物检查（docs/15 设计 2.7 的检查表；架构 5.8 R-2 / R-3 / R-9；用例 TC-PF02）。

  python3 tools/check_profiles.py                 # 两种都查（build/ 与 build-direct/ 都得先编过）
  python3 tools/check_profiles.py --only ha       # 只查一种
  python3 tools/check_profiles.py --self-test     # 变异自测：每条判据改坏一处，必须报红

判的是**产物**（esp_idf_size 的库字节数、镜像里的工程名、分区表、ELF 符号、前端打包清单），不 grep 源码：
被 --gc-sections 回收的库成员照样出现在 map 里，grep map 会永远报红（设计 2.2，2026-10-15 实测）。
源码形状的不变量（R-1 / R-2 / R-9）在 tools/check_profile_rules.py。

直连型那几条按配置开关自动生效：BITENGINE_NET_ETH_ONLY、BITENGINE_ENABLE_ZHUB、直连型分区表还没进 sdkconfig.direct 时
打 ⏳ 说明还没到那一步（设计第十七节的里程碑），不算过也不算挂 —— 开关一打开就开始真查，不用改这个脚本。
退出码 0 = 没有 ❌；1 = 有 ❌（或该有的产物不存在）。
"""
from __future__ import annotations

import argparse
import csv
import io
import gzip
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

PROFILES = {
    "ha":     {"build": ROOT / "build",        "project": "bitengine",        "kconfig": "CONFIG_BITENGINE_PROFILE_HA"},
    "direct": {"build": ROOT / "build-direct", "project": "bitengine-direct", "kconfig": "CONFIG_BITENGINE_PROFILE_DIRECT"},
}
# R-3：HA 型产物里不许有字节数的直连型归档（设计 2.7 检查表）
HA_FORBIDDEN_ARCHIVES = ("libmuse_link.a", "libmuse_pair.a", "libnoise_core.a", "libble_window.a",
                         "libhub_core.a", "libzhub.a", "libmatter_ctrl.a", "libespressif__esp_matter.a")
# 放在共用组件 thread_br 里的直连型符号，按归档查不出来（交叉清单 B-08）：thread_br_get_ack_*（M2 自检）；thread_inventory_*
# 原计划放 thread_br、M3 实现放在 hub_core（按归档就查得到），符号检查照留，万一哪天挪回共用组件还兜得住
# zhub 落地（2026-10-22，MD-13）之后，共用组件里给它开的口子（ezsp_bridge 的认领 / 计数、wd_probes 的直连型探针）全在
# #if CONFIG_BITENGINE_ENABLE_ZHUB 里 —— 按归档查不出来（它们编在 libezsp_bridge.a / libwatchdog.a 里），按符号钉住（第二轮审计建议）
HA_FORBIDDEN_SYMBOL = ("thread_inventory_", "thread_br_get_ack_",
                       "zhub_", "zh_host_", "zh_ash_", "zh_ses_", "zh_ezsp_", "ezsp_bridge_zhub_", "ezsp_bridge_set_zhub_hooks",
                       "ezsp_bridge_note_uart", "probe_mg24_zhub")
WIFI_ZERO = ("libnet80211.a", "libpp.a", "libwpa_supplicant.a")
WIFI_INIT_MAX = 100                              # libesp_wifi.a 剩 wifi_init.c 的一个常量（G0-3 实测 74 字节）
HA_PARTITIONS = {"www": (0x620000, None), "fctry": (0x6B0000, None)}            # B-13：HA 型分区表不动
DIRECT_PARTITIONS = {"nvs": (0x9000, 0x10000), "otadata": (0x19000, 0x2000), "matter": (0x1C000, 0x10000),
                     "ota_0": (0x30000, 0x380000), "ota_1": (0x3B0000, 0x380000), "www": (0x730000, 0x80000),
                     "hubdb": (0x7B0000, 0x30000), "coredump": (0x7E0000, 0x10000), "fctry": (0x7F0000, 0x4000)}
# R-2。键是"库名:目标文件"（esp_idf_size --files 的键取 basename 后就是这个样子）。⚠️ 别写裸文件名：
# 2026-10-22 之前这里写的是 "tcp_server.c.obj"，永远查不到、恒读成 0，直连型那条"0 字节"一直是空过的 ——
# 是加了下面 HA 型的正向对照才发现的。
BRIDGE_OBJS = ("libezsp_bridge.a:tcp_server.c.obj", "libezsp_bridge.a:client_buffer.c.obj",
               "libezsp_bridge.a:usb_bridge.c.obj", "libwebui.a:smlight_compat.c.obj")
UART_OBJ = "libezsp_bridge.a:uart_bridge.c.obj"     # 直连型照样编（只建 UART）：证明键格式对得上
DIRECT_PARTITION_CSV = "partitions_direct.csv"   # 设计 2.4
# R-9：esp-matter 里能写 app 分区的东西（OTA Requestor 与它的 BDX 下载器、esp_matter 的 OTA 封装）与它带进来的增量 / 加密 OTA 库，
# 直连型的产物里都不许有字节（防串刷比对只在我们自己的两条 OTA 路径上）
MATTER_OTA_OBJS = ("OTARequestor", "ota-requestor", "BDXDownloader", "esp_matter_ota")
MATTER_OTA_ARCHIVES = ("libespressif__esp_delta_ota.a", "libespressif__esp_encrypted_img.a")


# ---------------- 从一个构建目录里抽事实 ----------------
def idf_python() -> str:
    return os.environ.get("IDF_PYTHON_ENV_PATH", "") and str(Path(os.environ["IDF_PYTHON_ENV_PATH"]) / "bin" / "python") or sys.executable


def esp_idf_size(map_path: Path, mode: str) -> dict:
    out = subprocess.run([idf_python(), "-m", "esp_idf_size", f"--{mode}", "--format", "json2", str(map_path)],
                         capture_output=True, text=True, check=True).stdout
    d = json.loads(out)
    table = d.get(mode, d)
    return {os.path.basename(k): int(v.get("size", 0)) for k, v in table.items()}


def project_name_of(bin_path: Path) -> str:
    data = bin_path.read_bytes()[:512]
    # 与设备侧同一套常量：components/ota_cloud/ota_profile.h（这里不 import C，照抄偏移并核对 magic）
    if len(data) < 112 or data[0] != 0xE9 or int.from_bytes(data[32:36], "little") != 0xABCD5432:
        return ""
    return data[80:112].split(b"\0")[0].decode("ascii", "replace")


def partitions_of(build: Path) -> dict[str, tuple[int, int]]:
    ptbin = build / "partition_table" / "partition-table.bin"
    gen = Path(os.environ.get("IDF_PATH", Path.home() / "esp-idf-v6.0.2")) / "components" / "partition_table" / "gen_esp32part.py"
    out = subprocess.run([idf_python(), str(gen), "--quiet", str(ptbin)], capture_output=True, text=True, check=True).stdout
    parts = {}
    for row in csv.reader(io.StringIO(out)):
        if not row or row[0].strip().startswith("#") or len(row) < 5:
            continue
        name, off, size = row[0].strip(), row[3].strip(), row[4].strip()
        conv = lambda v: int(v[:-1], 0) * 1024 if v.upper().endswith("K") else (int(v[:-1], 0) * 1024 * 1024 if v.upper().endswith("M") else int(v, 0))
        parts[name] = (conv(off), conv(size))
    return parts


def symbols_of(elf: Path) -> set[str]:
    nm = shutil.which("riscv32-esp-elf-nm")
    if not nm:
        raise RuntimeError("找不到 riscv32-esp-elf-nm（先 source ~/esp-idf-v6.0.2/export.sh）")
    out = subprocess.run([nm, "--defined-only", str(elf)], capture_output=True, text=True, check=True).stdout
    return {line.split()[-1] for line in out.splitlines() if line.split()}


def facts_of(profile: str) -> dict:
    p = PROFILES[profile]
    b = p["build"]
    name = p["project"]
    need = [b / f"{name}.bin", b / f"{name}.elf", b / f"{name}.map", b / "config" / "sdkconfig.json",
            b / "partition_table" / "partition-table.bin"]
    missing = [str(x.relative_to(ROOT)) for x in need if not x.exists()]
    if missing:
        raise FileNotFoundError(f"{profile}：缺产物 {missing} —— 先 bash tools/build_profiles.sh --only {profile}")
    cfg = json.loads((b / "config" / "sdkconfig.json").read_text())
    www = b / "www_image"
    return {
        "profile": profile,
        "sdk": {k: v for k, v in cfg.items() if k.startswith(("BITENGINE_", "PARTITION_TABLE_"))},
        "project_name": project_name_of(b / f"{name}.bin"),
        "bin_size": (b / f"{name}.bin").stat().st_size,
        "archives": esp_idf_size(b / f"{name}.map", "archives"),
        "files": esp_idf_size(b / f"{name}.map", "files"),
        "partitions": partitions_of(b),
        "symbol_count": len(syms := symbols_of(b / f"{name}.elf")),
        "symbols_hit": sorted(s for s in syms if s.startswith(HA_FORBIDDEN_SYMBOL)),   # str.startswith 收元组
        "www_files": sorted(str(f.relative_to(www)) for f in www.rglob("*") if f.is_file()) if www.is_dir() else None,
        "www_sections": www_sections_of(www),
        "www_version": www_version_of(www),
    }


# 前端是单个 index.html.gz（frontend/build.sh 把各 js 内联进去，每段前面一行 "// --- <文件> ---"），所以打包清单里看不到
# 直连型的文件名 —— 得解开 gzip 看段标记（A-29；frontend/direct/build.sh 派生出的段名以 direct/ 开头）
def www_sections_of(www: Path):
    idx = www / "index.html.gz"
    if not idx.is_file():
        return None
    html = gzip.decompress(idx.read_bytes()).decode("utf-8", "replace")
    return re.findall(r"^// --- (\S+) ---$", html, flags=re.M)


def www_version_of(www: Path):
    v = www / "version.json"
    try:
        return json.loads(v.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


# ---------------- 判 ----------------
def judge(f: dict) -> tuple[list[str], list[str], list[str]]:
    """返回 (✅, ⏳, ❌) 三组说明。"""
    ok, pend, bad = [], [], []
    prof = f["profile"]
    want = PROFILES[prof]["project"]
    sdk = f["sdk"]
    kc = PROFILES[prof]["kconfig"][len("CONFIG_"):]

    # 判据没失明：下面"某某没有"类的判据，在表取空时会恒绿。表必须真有东西（数量级按 v1.0.0 的实际产物定得很保守）
    blind = []
    if len(f["archives"]) < 20 or f["archives"].get("libmain.a", 0) <= 0:
        blind.append(f"库字节数表只有 {len(f['archives'])} 项 / libmain.a {f['archives'].get('libmain.a', 0)}")
    if len(f["files"]) < 100:
        blind.append(f"目标文件表只有 {len(f['files'])} 项")
    if f["symbol_count"] < 1000:
        blind.append(f"ELF 符号只有 {f['symbol_count']} 个")
    (bad if blind else ok).append("产物解析正常（esp_idf_size / nm 没失明）" + (f" —— {blind}" if blind else ""))
    (ok if sdk.get(kc) is True else bad).append(f"sdkconfig 选的是 {kc}")
    (ok if f["project_name"] == want else bad).append(f"镜像工程名 = \"{f['project_name']}\"（应为 \"{want}\"，防串刷认的就是它）")

    slot = f["partitions"].get("ota_0", (0, 0))[1]
    if slot <= 0:
        bad.append("分区表里没有 ota_0")
    else:
        margin = slot - f["bin_size"]
        (ok if margin > 0 else bad).append(f"app {f['bin_size']:,} 字节，ota_0 槽 {slot:,}，余 {margin:,}（{margin * 100 // slot}%）")

    if prof == "ha":
        hits = {a: f["archives"].get(a, 0) for a in HA_FORBIDDEN_ARCHIVES if f["archives"].get(a, 0) > 0}
        (bad if hits else ok).append("R-3：HA 型里没有直连型组件的归档" + (f" —— 有：{hits}" if hits else ""))
        (bad if f["symbols_hit"] else ok).append("B-08 / MD-13：HA 型 ELF 里没有 thread_inventory_* / thread_br_get_ack_* / zhub 相关符号"
                                                + (f" —— 有：{f['symbols_hit'][:5]}" if f["symbols_hit"] else ""))
        for pname, (off, _) in HA_PARTITIONS.items():
            got = f["partitions"].get(pname, (None, None))[0]
            (ok if got == off else bad).append(f"B-13：HA 型 {pname} 在 {off:#x}（实际 {got:#x}）" if got is not None
                                               else f"B-13：HA 型分区表里没有 {pname}")
        if f["www_files"] is None:
            bad.append("没有 build/www_image（前端打包清单）—— 构建不完整？")
        else:
            leak = [x for x in f["www_files"] if "direct/" in x.replace(os.sep, "/") or x.startswith("direct")]
            (bad if leak else ok).append("A-29：HA 型前端打包清单里没有直连型页面" + (f" —— 有：{leak[:5]}" if leak else ""))
        secs, ver = f["www_sections"], f["www_version"]
        if not secs:
            bad.append("A-29：HA 型 www_image/index.html.gz 读不出段标记 —— 前端没打进去，或 frontend/build.sh 改了格式（本判据就瞎了）")
        else:
            leak = [x for x in secs if x.startswith("direct/")]
            good = not leak and "js/pages/wifi.js" in secs
            (ok if good else bad).append(f"A-29：HA 型前端里没有直连型的段、照样有 WiFi 页（{len(secs)} 段）" + (f" —— 有：{leak[:5]}" if leak else ""))
        (ok if ver is not None and "profile" not in ver else bad).append(
            "HA 型 version.json 没有 profile 字段（直连型才有；HA 型的格式与以前相同）" if ver is not None else "HA 型 www_image 里没有 version.json")
        # 正向对照：HA 型里桥的目标文件必须有字节 —— 证明 HA 型没被一并去掉桥，也防止目标文件改名后
        # 直连型那条"0 字节"变成恒绿（审计 2026-10-22）
        ha_objs = {o: f["files"].get(o, 0) for o in (BRIDGE_OBJS[0], BRIDGE_OBJS[3])}
        (ok if all(ha_objs.values()) else bad).append(f"R-2 对照：HA 型照样带桥（{ha_objs}）")
    else:
        secs, ver = f["www_sections"], f["www_version"]
        if not secs:
            bad.append("A-29：直连型 www_image/index.html.gz 读不出段标记 —— 前端没打进去？")
        else:
            good = "direct/js/muse.js" in secs and "js/pages/wifi.js" not in secs and "direct/js/shell.js" in secs
            (ok if good else bad).append(f"A-29：直连型前端是单独构建的那份（有 Muse 页与外壳改动、没有 WiFi 页；{len(secs)} 段）")
        (ok if ver and ver.get("profile") == "direct" else bad).append("直连型 version.json 标了 profile:direct")
        if sdk.get("BITENGINE_NET_ETH_ONLY") is True:
            wifi = {a: f["archives"].get(a, 0) for a in WIFI_ZERO}
            esp_wifi = f["archives"].get("libesp_wifi.a", 0)
            good = all(v == 0 for v in wifi.values()) and esp_wifi <= WIFI_INIT_MAX
            (ok if good else bad).append(f"2.2：纯以太网剖面没有链接 Wi-Fi（{wifi}，libesp_wifi.a {esp_wifi} ≤ {WIFI_INIT_MAX}）")
        else:
            pend.append("2.2 Wi-Fi 库 0 字节：等 BITENGINE_NET_ETH_ONLY（M1 第二部分）")
        if sdk.get("BITENGINE_ENABLE_ZHUB") is True:
            # :6638 的 TCP 服务、它的发送环形缓冲、经 USB 的桥、SMLIGHT 兼容层（设计 2.8 A-04）
            objs = {o: f["files"].get(o, 0) for o in BRIDGE_OBJS}
            (ok if not any(objs.values()) else bad).append(f"R-2：直连型不编桥（{objs}）")
            uart = f["files"].get(UART_OBJ, 0)
            (ok if uart > 0 else bad).append(f"R-2 对照：直连型照样编 UART（{UART_OBJ} {uart} 字节；为 0 说明键格式对不上，上一条是空过）")
        else:
            pend.append("R-2 桥的目标文件 0 字节：等 BITENGINE_ENABLE_ZHUB")
        if os.path.basename(str(sdk.get("PARTITION_TABLE_CUSTOM_FILENAME", ""))) == DIRECT_PARTITION_CSV:
            for pname, (off, size) in DIRECT_PARTITIONS.items():
                got = f["partitions"].get(pname)
                (ok if got == (off, size) else bad).append(f"2.4：直连型 {pname} = {off:#x}/{size:#x}（实际 {got}）")
        else:
            pend.append(f"2.4 直连型分区表：sdkconfig.direct 还在用 {sdk.get('PARTITION_TABLE_CUSTOM_FILENAME')}（M1 第二部分换 {DIRECT_PARTITION_CSV}）")
        if sdk.get("BITENGINE_ENABLE_MATTER_CTRL") is True:
            em = f["archives"].get("libespressif__esp_matter.a", 0)
            (ok if em > 0 else bad).append(f"9.2：直连型链接了 esp-matter（libespressif__esp_matter.a {em} 字节；为 0 说明控制器没编进去）")
            # 第三条写 app 的路径（设计 2.7「还没做」、架构 R-9；CHECKLIST MC05）：esp-matter 的 OTA Requestor 在 managed_components 里，
            # 它调 esp_ota_begin 不经防串刷比对，check_profile_rules.py 扫不到源码 —— 按产物钉"没链接进来"
            ota = {k: v for k, v in f["files"].items() if v and any(t in k for t in MATTER_OTA_OBJS)}
            ota.update({a: f["archives"].get(a, 0) for a in MATTER_OTA_ARCHIVES if f["archives"].get(a, 0)})
            (ok if not ota else bad).append(f"R-9：直连型没有链接 Matter 的 OTA Requestor / 增量与加密 OTA（{ota or '0 字节'}）")
        else:
            pend.append("9.2 Matter 控制器：等 BITENGINE_ENABLE_MATTER_CTRL（M7）")
        diag = sorted(k for k, v in sdk.items() if k.startswith("BITENGINE_DIAG_") and v is True)
        if diag:   # 开发期允许；tools/publish_ota.sh 见到就拒发，这里只是每次构建都提醒一次
            pend.append(f"直连型开着诊断项 {', '.join(diag)}：发第一份直连型镜像前从 sdkconfig.defaults.direct 去掉（publish_ota.sh 会拦）")
    return ok, pend, bad


def report(f: dict) -> int:
    ok, pend, bad = judge(f)
    print(f"== {f['profile']}（{PROFILES[f['profile']]['build'].relative_to(ROOT)}/）")
    for x in ok:
        print("  ✅ " + x)
    for x in pend:
        print("  ⏳ " + x)
    for x in bad:
        print("  ❌ " + x)
    return len(bad)


# ---------------- 变异自测（TC-PF02 的后半）----------------
def self_test() -> int:
    """每条判据改坏一处必须报红；原样必须不红。先在抽出来的事实上改（覆盖每条判据），
    再真改一份 bin 的工程名重新抽（覆盖"从产物里读名字"这一步本身）。"""
    import copy
    fails = 0
    base = {p: facts_of(p) for p in PROFILES if (PROFILES[p]["build"] / f"{PROFILES[p]['project']}.bin").exists()}
    if "ha" not in base:
        print("❌ 自测要先有 HA 型构建（build/）"); return 1
    for p, f in base.items():
        if judge(f)[2]:
            print(f"❌ 原样的 {p} 就是红的，先修再自测：{judge(f)[2]}"); fails += 1

    def mut(p, why, fn):
        nonlocal fails
        if p not in base:
            print(f"⏭  {why}（没有 {p} 的构建）"); return
        f = copy.deepcopy(base[p]); fn(f)
        red = bool(judge(f)[2])
        print(("✅ 抓到 " if red else "❌ 漏了 ") + f"{p}：{why}"); fails += 0 if red else 1

    mut("ha", "esp_idf_size 失明：库字节数表是空的", lambda f: f.__setitem__("archives", {}))
    mut("ha", "esp_idf_size 失明：目标文件表是空的", lambda f: f.__setitem__("files", {}))
    mut("ha", "nm 失明：一个符号都没读到", lambda f: (f.__setitem__("symbol_count", 0), f.__setitem__("symbols_hit", [])))
    mut("ha", "HA 型里混进 libzhub.a 1200 字节", lambda f: f["archives"].__setitem__("libzhub.a", 1200))
    mut("ha", "HA 型里混进 esp-matter", lambda f: f["archives"].__setitem__("libespressif__esp_matter.a", 9))
    mut("ha", "HA 型工程名被改成 bitengine-direct", lambda f: f.__setitem__("project_name", "bitengine-direct"))
    mut("ha", "HA 型工程名读不出来（空）", lambda f: f.__setitem__("project_name", ""))
    mut("ha", "HA 型 ELF 里出现 thread_inventory_start", lambda f: f.__setitem__("symbols_hit", ["thread_inventory_start"]))
    mut("ha", "HA 型 ELF 里出现 ezsp_bridge_zhub_claim", lambda f: f.__setitem__("symbols_hit", ["ezsp_bridge_zhub_claim"]))
    mut("ha", "HA 型 fctry 挪到 0x7F0000", lambda f: f["partitions"].__setitem__("fctry", (0x7F0000, 0x4000)))
    mut("ha", "HA 型前端打包清单里出现 js/direct/muse.js", lambda f: f["www_files"].append("js/direct/muse.js"))
    mut("ha", "HA 型前端里内联了 direct/js/muse.js", lambda f: f["www_sections"].append("direct/js/muse.js"))
    mut("ha", "HA 型前端读不出段标记（gzip 坏了 / 格式改了）", lambda f: f.__setitem__("www_sections", None))
    mut("ha", "HA 型 version.json 带了 profile", lambda f: f["www_version"].__setitem__("profile", "direct"))
    mut("direct", "直连型打包的还是 HA 型前端（没有 Muse 页）", lambda f: f["www_sections"].remove("direct/js/muse.js"))
    mut("direct", "直连型前端里还有 WiFi 页", lambda f: f["www_sections"].append("js/pages/wifi.js"))
    mut("direct", "直连型 version.json 没标 profile", lambda f: f["www_version"].pop("profile", None))
    mut("direct", "直连型开着 Matter 却没链接 esp-matter", lambda f: f["archives"].__setitem__("libespressif__esp_matter.a", 0))
    mut("direct", "直连型链接进了 OTA Requestor", lambda f: f["files"].__setitem__("libespressif__esp_matter.a:DefaultOTARequestor.cpp.obj", 4096))
    mut("direct", "直连型链接进了增量 OTA 库", lambda f: f["archives"].__setitem__("libespressif__esp_delta_ota.a", 512))
    mut("ha", "HA 型 sdkconfig 选成了直连型", lambda f: f["sdk"].update({"BITENGINE_PROFILE_HA": False, "BITENGINE_PROFILE_DIRECT": True}))
    mut("ha", "app 超出槽", lambda f: f.__setitem__("bin_size", f["partitions"]["ota_0"][1] + 1))
    mut("direct", "直连型工程名还是 bitengine", lambda f: f.__setitem__("project_name", "bitengine"))
    mut("direct", "直连型开了纯以太网却还链着 libnet80211.a",
        lambda f: (f["sdk"].__setitem__("BITENGINE_NET_ETH_ONLY", True), f["archives"].__setitem__("libnet80211.a", 2000)))
    mut("direct", "直连型开了 zhub 却还编着 tcp_server.c",
        lambda f: (f["sdk"].__setitem__("BITENGINE_ENABLE_ZHUB", True), f["files"].__setitem__(BRIDGE_OBJS[0], 3000)))
    mut("ha", "HA 型把桥也去掉了", lambda f: f["files"].__setitem__(BRIDGE_OBJS[0], 0))
    mut("direct", "键格式对不上（直连型连 UART 都读成 0）", lambda f: f["files"].pop(UART_OBJ, None))
    mut("direct", "直连型开了 zhub 却还编着 SMLIGHT 兼容层",
        lambda f: (f["sdk"].__setitem__("BITENGINE_ENABLE_ZHUB", True), f["files"].__setitem__(BRIDGE_OBJS[3], 1200)))
    mut("direct", "直连型换了分区表但 fctry 位置不对",
        lambda f: (f["sdk"].__setitem__("PARTITION_TABLE_CUSTOM_FILENAME", "partitions_direct.csv"),
                   f["partitions"].update({k: v for k, v in DIRECT_PARTITIONS.items()}),
                   f["partitions"].__setitem__("fctry", (0x6B0000, 0x4000))))

    # 真改产物：复制 HA 型的 bin，把工程名改成 bitengine-direct，从字节里重新读
    with tempfile.TemporaryDirectory() as td:
        src = PROFILES["ha"]["build"] / "bitengine.bin"
        dst = Path(td) / "bitengine.bin"
        data = bytearray(src.read_bytes())
        data[80:112] = b"bitengine-direct".ljust(32, b"\0")
        dst.write_bytes(bytes(data))
        got = project_name_of(dst)
        f = copy.deepcopy(base["ha"]); f["project_name"] = got
        red = bool(judge(f)[2])
        print(("✅ 抓到 " if red and got == "bitengine-direct" else "❌ 漏了 ") + f"ha：真改了 bin 里的工程名（读出 \"{got}\"）")
        fails += 0 if (red and got == "bitengine-direct") else 1
        data[32] ^= 0xFF                                   # 描述 magic 坏掉 → 读不出名字 → 必须红
        dst.write_bytes(bytes(data))
        f = copy.deepcopy(base["ha"]); f["project_name"] = project_name_of(dst)
        red = bool(judge(f)[2])
        print(("✅ 抓到 " if red else "❌ 漏了 ") + "ha：bin 里的 app 描述 magic 坏了")
        fails += 0 if red else 1

    print(f"\n自测：{'全部抓到' if not fails else f'{fails} 处没抓到'}")
    return 1 if fails else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", choices=sorted(PROFILES))
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    bad = 0
    for p in ([a.only] if a.only else list(PROFILES)):
        try:
            bad += report(facts_of(p))
        except subprocess.CalledProcessError as e:
            hint = "" if os.environ.get("IDF_PATH") else "（没进 ESP-IDF 环境：先 source ~/esp-idf-v6.0.2/export.sh，或用 tools/build_profiles.sh）"
            print(f"❌ {p}：{' '.join(map(str, e.cmd[:3]))} … 失败{hint}\n   {(e.stderr or '').strip()[:300]}")
            bad += 1
        except (FileNotFoundError, RuntimeError) as e:
            print(f"❌ {p}：{e}")
            bad += 1
    print("\n" + ("✅ 两种配置的产物检查通过" if not bad else f"❌ {bad} 处不过"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
