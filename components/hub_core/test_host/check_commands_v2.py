#!/usr/bin/env python3
"""金标：hub_registry 生成的 commands_v2（权限全开、后端全在）必须与 docs/15_muse_direct_design_v1.md 附录 A 逐项相同，
命令顺序也相同。命令说明是给 Muse 的模型看的，改一个字都会改变它的行为：改了任何一边，另一边必须同步（设计 14.1 ②）。

用法：check_commands_v2.py <hub_registry_test 可执行文件>"""
import json
import subprocess
import sys
from pathlib import Path

DOC = Path(__file__).resolve().parents[3] / "docs" / "15_muse_direct_design_v1.md"


def appendix_a() -> dict:
    s = DOC.read_text(encoding="utf-8")
    i = s.index("## 附录 A：`commands_v2` 全文")
    j = s.index("```json", i) + len("```json")
    k = s.index("```", j)
    return json.loads(s[j:k])


def main() -> int:
    out = subprocess.run([sys.argv[1], "--emit"], capture_output=True, text=True, check=True).stdout
    got = json.loads(out)
    want = appendix_a()
    bad = 0
    if list(got) != list(want):
        print(f"❌ 命令集合或顺序不同：\n   代码 {list(got)}\n   附录 {list(want)}")
        bad += 1
    for name in want:
        if name in got and got[name] != want[name]:
            bad += 1
            for field in ("description", "required", "optional", "timeout_ms"):
                if got[name].get(field) != want[name].get(field):
                    print(f"❌ {name}.{field} 与附录 A 不同：\n   代码 {json.dumps(got[name].get(field), ensure_ascii=False)[:300]}\n"
                          f"   附录 {json.dumps(want[name].get(field), ensure_ascii=False)[:300]}")
    if bad:
        return 1
    print(f"✅ commands_v2 与附录 A 一致（{len(got)} 条命令，紧凑 {len(out)} 字节）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
