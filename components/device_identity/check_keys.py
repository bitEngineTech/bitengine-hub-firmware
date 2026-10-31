#!/usr/bin/env python3
"""VERIFY.sh 的跨侧键名检查（单独成文件，不塞进 shell 里转义）。

    python3 components/device_identity/check_keys.py

三条不变量：
  · 产线工具写的键名 == 固件读的键名（逐字，集合相等）
  · 每个键 ≤ 15 字符 —— NVS 的硬上限，超了 nvs_partition_gen 直接拒绝生成，
    而 docs/10 §6.2 给的 hardware_revision(17)/manufacture_date(16) 正好超
  · 两边都不出现 device_secret 这个键（本期决策：不写 secret）

只改一边的表现是"分区在、能打开、一个键都读不到"，很容易被误判成产线没写，
所以这条必须自动验。
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
GEN = ROOT / "tools" / "generate_nvs.py"
FW = ROOT / "components" / "device_identity" / "device_identity.c"

def main() -> int:
    gen, fw = GEN.read_text(encoding="utf-8"), FW.read_text(encoding="utf-8")
    gk = set(re.findall(r'\("([a-z_]+)", "data", "string"', gen))
    fk = set(re.findall(r'read_key\(h, "([a-z_]+)"', fw))
    bad = []
    if not gk:
        bad.append("在 generate_nvs.py 里没认出任何键（写法变了？）")
    if not fk:
        bad.append("在 device_identity.c 里没认出任何键（写法变了？）")
    if gk != fk:
        bad.append(f"键名对不上：工具写 {sorted(gk)}，固件读 {sorted(fk)}")
    for k in gk | fk:
        if len(k) > 15:
            bad.append(f"键名 {k!r} 有 {len(k)} 字符，超过 NVS 的 15 上限")
    for name, txt in (("generate_nvs.py", gen), ("device_identity.c", fw)):
        if '"device_secret"' in txt:
            bad.append(f"{name} 里出现了 device_secret 键（本期决策不写 secret）")
    for b in bad:
        print(f"❌ {b}", file=sys.stderr)
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
