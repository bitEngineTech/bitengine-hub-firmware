#!/usr/bin/env python3
"""factory_test.sh 与 factory_test.ps1 必须做同一件事（2026-09-11）。

    python3 tools/check_factory_test_parity.py

两个脚本分别跑在 Linux 和产线的 Windows 机器上，而**放行标准必须一样**。
.ps1 的文件头已经写了"两边同时改"，但那只是一句嘱托 —— 本脚本把它变成判据。

⚠️ 本机没装 pwsh，但 PowerShell 的 Linux 便携版不需要 root：从
   github.com/PowerShell/PowerShell/releases 下 `powershell-<ver>-linux-x64.tar.gz`，
   解开直接 `./pwsh -File tools/factory_test.ps1 -GW <IP> -Pass <密码>`。
   2026-09-13 这样**首次真跑**了 .ps1，当场抓到 401 预检是死代码（见下）。
   memory `windows-factory-kit-and-parked-items` 记着：Windows 套件五次失败全是
   脚本自己的问题 —— 所以 .ps1 每次改动都该这样跑一遍，别只靠本脚本比形状。
   ⚠️ pwsh 7 ≠ Windows PowerShell 5.1：响应体解码、错误对象形状有差异，
   5.1 特有的行为仍要在 Windows 机器上走一遍。

比什么（有意只比这些 —— 比得太细会变成每次改措辞都要同步的负担）：
  · 每个可 ack 的自检项，两边都必须出现
  · 两边都要有 MAC 查重与台账
  · 两边的开关要一一对应（--power/-Power、--retest/-Retest、--auto/-Auto）
  · 401 预检探针：两边同一端点，且**到 rest_api.c 里确认**那个 GET handler 是
    无条件 AUTH_REQUIRED。2026-09-13 教训：两边都探 /api/v1/status
    （AUTH_REQUIRED_UNLESS_OPEN，出厂配置下永不 401），预检从写下那天起就是死代码，
    而本脚本因为"两边一致"照样绿 —— 只比两个脚本彼此相同，抓不到一致地错。
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sh = (ROOT / "factory_test.sh").read_text(encoding="utf-8")
ps = (ROOT / "factory_test.ps1").read_text(encoding="utf-8")

# ACKABLE 白名单以固件为准，两个脚本都必须覆盖到
fw = (ROOT / ".." / "components" / "webui" / "factory_api.c").read_text(encoding="utf-8")
m = re.search(r"ACKABLE\[\]\s*=\s*\{(.*?)\};", fw, re.S)
if not m:
    sys.exit("❌ 在 factory_api.c 里找不到 ACKABLE 白名单")
ackable = re.findall(r'"([a-z0-9_]+)"', m.group(1))
if not ackable:
    sys.exit("❌ ACKABLE 白名单解析为空")

# ⚠️ 不能只判"名字在文件里出现过"：项名在文件头的用法说明里也会出现
#    （-Power poe 那行就含 "poe"），那样删掉整段 PoE 处理照样绿 —— 第一版就是
#    这么写的，变异测试当场没红。所以判的是**调用点**：要么回写 ack，要么读它的
#    state（button 那项就不 ack，而是轮询到真的按过为止）。
SH_USE = lambda it: re.search(rf"\b(?:ack|item_state)\s+{re.escape(it)}\b", sh)
PS_USE = lambda it: re.search(rf"(?:Set-Ack|Get-ItemState \$d)\s+'{re.escape(it)}'", ps)

bad = []
for item in ackable:
    if not SH_USE(item):
        bad.append(f"factory_test.sh 没有处理 manual 项 '{item}'"
                   f"（既没 ack 也没读它的 state）")
    if not PS_USE(item):
        bad.append(f"factory_test.ps1 没有处理 manual 项 '{item}'"
                   f"（既没 Set-Ack 也没 Get-ItemState）")

# 成对的开关与功能
for name, in_sh, in_ps in [
    ("MAC 查重台账", ".factory_test_log.csv", ".factory_test_log.csv"),
    ("--auto / -Auto", "--auto", "$Auto"),
    ("供电方式开关", "--power=usb", "-Power"),
    ("复测开关", "--retest", "Retest"),
]:
    if in_sh not in sh:
        bad.append(f"factory_test.sh 缺少「{name}」（找不到 {in_sh}）")
    if in_ps not in ps:
        bad.append(f"factory_test.ps1 缺少「{name}」（找不到 {in_ps}）")

# 401 预检探针：跨侧验。光比两个脚本相同不够，还要到 rest_api.c 里确认那个端点
# 真的会 401 —— 探 AUTH_REQUIRED_UNLESS_OPEN 的端点（/status、/led/config GET、
# /api2/*）在 rest_auth 出厂关时永不 401，预检就是死代码。
PROBE_RE_SH = r'401 预检.*?"http://\$GW(/api/v1/[a-z0-9_/]+)"'
PROBE_RE_PS = r'401 预检.*?Invoke-GW "http://\$GW(/api/v1/[a-z0-9_/]+)"'
sh_probe = re.search(PROBE_RE_SH, sh, re.S)
ps_probe = re.search(PROBE_RE_PS, ps, re.S)
if not sh_probe or not ps_probe:
    bad.append("找不到 401 预检探针（注释「401 预检」之后第一条 http://$GW/api/v1/... 请求）")
elif sh_probe.group(1) != ps_probe.group(1):
    bad.append(f"401 预检探针端点不一致：.sh={sh_probe.group(1)} .ps1={ps_probe.group(1)}")
else:
    probe = sh_probe.group(1)
    rest = (ROOT / ".." / "components" / "webui" / "rest_api.c").read_text(encoding="utf-8")
    ent = re.search(rf'URI_ENTRY\("{re.escape(probe)}",\s*HTTP_GET,\s*([a-z0-9_]+)\)', rest)
    if not ent:
        bad.append(f"401 预检探针 {probe} 在 rest_api.c 里没有 GET 注册")
    else:
        handler = ent.group(1)
        body = re.search(rf"static esp_err_t {handler}\(httpd_req_t \*req\)\s*\{{(.*?)\n\}}", rest, re.S)
        first = re.search(r"\b(AUTH_REQUIRED(?:_UNLESS_OPEN)?)\s*\(", body.group(1)) if body else None
        if not first or first.group(1) != "AUTH_REQUIRED":
            got = first.group(1) if first else "无守卫"
            bad.append(f"401 预检探针 {probe} 的 handler {handler} 不是无条件 AUTH_REQUIRED（实际：{got}）"
                       f"—— 出厂配置下永不 401，预检是死代码")

if bad:
    print("❌ 两个产测脚本不同步：")
    for b in bad:
        print("   ·", b)
    sys.exit(1)
sys.exit(0)
