#!/usr/bin/env python3
"""产线自检的三条结构不变量（T2.2，2026-09-11）。

    python3 check_selftest_items.py

① **项数固定**。判据的唯一来源就是 factory_api.c —— 项数变了，说明有人加了或
   删了一项判定，那么 `docs/guide/产测指南.md` 的验收表和 tools/factory_test.sh
   的预期都得跟着改。让数字写死在这里，是为了强迫改的人去看那两处。

② **只开 selftest 命名空间**。flash_rw 要写 NVS，而这台设备上有一块**擦不得**
   的区域：出厂身份在 fctry 分区，擦掉 = 设备永久失去身份，且外部零症状
   （见 components/device_identity）。

③ **fctry 在代码和字符串字面量里都不许出现**（注释里解释"为什么不碰它"是好事）。
   ⚠️ 所以这里保留字符串字面量只剥注释 —— 如果连字符串一起剥，
      `nvs_open("fctry", …)` 会被剥成 `nvs_open( , …)`，这条检查就成了恒绿。
"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / ".." / ".." / "tools"))
from grep_code import strip_comments          # noqa: E402

WANT_ITEMS = 19   # 2026-10-07 +ncp_tables（工位 3 认 NCP 版本，T9.12②）；产测指南验收表已同步，factory_test.sh/.ps1 逐项通用不用改

src = (HERE / "factory_api.c").read_text(encoding="utf-8")
code = strip_comments(src, keep_strings=True)

# ① 项数：只数报告里真正 item(rp, "name", ...) 的调用
# ⚠️ 数的是**去重后**的名字，不是调用点：同一项在互斥分支里各写一次 item() 是
#    正常的（wifi_scan 有 4 个分支：已连上 / 扫不出去 / 扫到 / 没扫到），
#    按调用点数会把它算成 4 项。
names = re.findall(r'\bitem\(rp,\s*"([a-z0-9_]+)"', code)
uniq = sorted(set(names))
if len(uniq) != WANT_ITEMS:
    sys.exit(f"❌ 自检项 {len(uniq)} 项，期望 {WANT_ITEMS}：{uniq}\n"
             f"   加/删判定项时，docs/guide/产测指南.md 的验收表和 "
             f"tools/factory_test.sh 的预期必须一起改，改完再更新本脚本的 WANT_ITEMS")

# ② nvs_open 的命名空间：只允许 SELFTEST_NS（或直接写 "selftest"）
opens = re.findall(r'nvs_open\(\s*([^,]+),', code)
for ns in opens:
    ns = ns.strip()
    if ns not in ('SELFTEST_NS', '"selftest"'):
        sys.exit(f"❌ 自检打开了非 selftest 命名空间：nvs_open({ns}, …)")
if len(opens) < 2:
    sys.exit("❌ flash_rw 回读前没有重开句柄 —— 那只证明了 NVS 的内存缓存，"
             "证明不了真的落盘")

# ③ fctry：代码与字符串字面量里都不许出现
if re.search(r"fctry", code):
    sys.exit("❌ 自检代码里出现 fctry —— 擦掉出厂身份是不可逆的，且外部零症状")

sys.exit(0)
