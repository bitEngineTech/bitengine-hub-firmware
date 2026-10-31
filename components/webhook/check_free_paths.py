#!/usr/bin/env python3
"""webhook_task() 里每条 continue 都必须先把已分配的东西释放掉。

这个任务每秒醒一次、每来一个事件走一遍。漏一条 continue 就是持续泄漏，
而症状是"跑几天之后内存慢慢没了"，最难归因的一类。

判据：在 body 分配点之后的每一个 continue 之前，必须出现过 free(body)；
循环末尾也必须有一次。同理检查 backup_json。

⚠️ 单独成文件，别塞进 VERIFY.sh：这段正则经 bash 再进 python 会被吃掉转义
（2026-09-11 第一版就是这么写坏的，device_identity 那边也栽过同一跤）。
"""

import re
import sys
from pathlib import Path


def main() -> int:
    src = (Path(__file__).resolve().parent / "webhook.c").read_text(encoding="utf-8")
    m = re.search(r"static void webhook_task\(void \*arg\)\s*\{(.*?)\n\}", src, re.S)
    if not m:
        print("❌ 找不到 webhook_task()", file=sys.stderr)
        return 1
    body = m.group(1)

    alloc = body.find("*body = heap_caps_malloc")
    if alloc < 0:
        print("❌ 找不到 body 的分配点（改写法了？）", file=sys.stderr)
        return 1

    bad = []
    tail = body[alloc:]

    def guarded_null(window: str, var: str) -> bool:
        """这条 continue 是不是就在 `if (!var)` 的失败分支里。
        那时指针本来就是 NULL，不需要也不应该 free —— 不排除掉它就是误报
        （2026-09-11 第一版对 body 和 backup_json 各误报了一次）。"""
        return f"if (!{var})" in window[-160:]

    for mm in re.finditer(r"\bcontinue\s*;", tail):
        window = tail[max(0, mm.start() - 300):mm.start()]
        if guarded_null(window, "body"):
            continue
        if "free(body)" not in window:
            line = body[:alloc].count("\n") + tail[:mm.start()].count("\n") + 1
            bad.append(f"webhook_task 第 ~{line} 行的 continue 之前没有 free(body)")

    if "free(body)" not in tail[-500:]:
        bad.append("循环末尾没有 free(body)")

    # backup_json：从拿到它开始，一直到它被释放并置空为止，这中间的每条
    # continue 都要先释放。
    # ⚠️ 扫的是**整个函数体**，不能只扫 body 分配点之前 —— `if (!body)` 那条
    #    失败路径就在分配点之后，删掉那里的 free(backup_json) 会在内存已经紧张时
    #    再泄漏一份 1KB 的导出，正是最不该泄漏的时刻。
    #    （2026-09-11：第一版只扫了前半段，这条变异一路放行。）
    got = body.find("config_mgr_export_json")
    if got >= 0:
        # ⚠️ 从 got **之后**开始找置空点：函数开头那句声明
        #    `char *backup_json = NULL;` 也匹配这个串，从 0 开始找会命中它，
        #    于是 end 退化成整个函数、把后面所有 continue 都误报一遍
        #    （2026-09-11 第一版就是这样，line 93 平白红了一次）。
        cleared = body.find("backup_json = NULL", got)
        end = cleared if cleared > got else len(body)
        span = body[got:end]
        for mm in re.finditer(r"\bcontinue\s*;", span):
            seg = span[:mm.start()]
            if guarded_null(seg, "backup_json"):
                continue
            if "free(backup_json)" not in seg[-300:]:
                line = body[:got].count("\n") + seg.count("\n") + 1
                bad.append(f"webhook_task 第 ~{line} 行的 continue 之前没有 free(backup_json)")

    for b in bad:
        print("❌ " + b, file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
