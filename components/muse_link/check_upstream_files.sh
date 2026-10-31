#!/bin/bash
# 核对从 muse-gadget-sdk 引入的文件（docs/15 设计附录 C；清单与登记在同目录 UPSTREAM.md）。
#
#   bash components/muse_link/check_upstream_files.sh                 # 只查本地
#   bash components/muse_link/check_upstream_files.sh ~/src/muse-gadget-sdk
#                                                                     # 再对上游固定提交逐字节比对，并列出上游远端
#                                                                     # （origin/HEAD）自固定提交以来动过的、我们引入了的文件
#
# 本地检查三条，任一不过就红：
#   1. 引入文件的 sha256 等于清单（改了引入文件却没同步清单 = 红：改动必须先在 UPSTREAM.md 登记，附录 C）；
#   2. muse_pair / muse_link / noise_core 下的每个文件都已登记：在上游清单里，或在"自写"清单里。两边都没有 = 红 ——
#      否则"改了引入文件、顺手把它从清单里删掉"就绕过了第 1 条（2026-10-22 审计实测绕得过去）；
#   3. "自写"清单里的文件都在，且不和上游清单重叠。
# 清单行带第四列 modified 的（已登记的本地改动）：本地哈希照查，带上游目录时不要求与上游逐字节相同。
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
COMPS="$(cd "$HERE/.." && pwd)"
DOC="$HERE/UPSTREAM.md"
SDK="${1:-}"

block() { sed -n "/^\`\`\`$1\$/,/^\`\`\`\$/p" "$DOC" | sed '1d;$d'; }
PIN="$(grep -m1 -oE '^`[0-9a-f]{40}`' "$DOC" | tr -d '`')"
MANIFEST="$(block manifest)"
LOCAL="$(block local)"
fail=0
[ -n "$PIN" ] || { echo "❌ UPSTREAM.md 里找不到固定提交"; exit 1; }
[ -n "$MANIFEST" ] || { echo "❌ UPSTREAM.md 里找不到上游文件清单"; exit 1; }
[ -n "$LOCAL" ] || { echo "❌ UPSTREAM.md 里找不到自写文件清单"; exit 1; }

# 1. 引入文件的哈希
n=0
while read -r sum lp up mod; do
    n=$((n + 1))
    if [ ! -f "$COMPS/$lp" ]; then
        echo "❌ 清单里的文件不见了：$lp"; fail=1; continue
    fi
    actual="$(sha256sum "$COMPS/$lp" | cut -d' ' -f1)"
    [ "$actual" = "$sum" ] || { echo "❌ 改过但清单没同步：$lp"; fail=1; }
done <<< "$MANIFEST"

# 2. 每个文件都已登记；3. 自写清单有效
up_listed="$(awk '{print $2}' <<< "$MANIFEST" | sort)"
local_listed="$(sort <<< "$LOCAL")"
while read -r f; do
    grep -qxF "$f" <<< "$local_listed" || continue
    grep -qxF "$f" <<< "$up_listed" && { echo "❌ 同时在上游清单和自写清单里：$f"; fail=1; }
done <<< "$local_listed"
while read -r f; do
    [ -f "$COMPS/$f" ] || { echo "❌ 自写清单里的文件不见了：$f"; fail=1; }
done <<< "$local_listed"
while read -r f; do
    grep -qxF "$f" <<< "$up_listed" || grep -qxF "$f" <<< "$local_listed" \
        || { echo "❌ 没登记的文件（UPSTREAM.md 的上游清单和自写清单里都没有）：$f"; fail=1; }
done < <(cd "$COMPS" && find muse_pair muse_link noise_core -type f -not -path '*/__pycache__/*' | sort)
[ $fail -eq 0 ] && echo "✅ 本地：$n 个引入文件与清单一致、$(wc -l <<< "$local_listed") 个自写文件已登记（固定提交 ${PIN:0:7}）"

if [ -n "$SDK" ]; then
    git -C "$SDK" cat-file -e "$PIN^{commit}" 2>/dev/null || { echo "❌ $SDK 里没有提交 $PIN（先 git fetch）"; exit 1; }
    while read -r sum lp up mod; do
        if [ "${mod:-}" = "modified" ]; then
            echo "  · 已登记的本地改动，不比上游：$lp"; continue
        fi
        cmp -s "$COMPS/$lp" <(git -C "$SDK" show "$PIN:$up") || { echo "❌ 与上游 ${PIN:0:7} 不一致：$lp ← $up"; fail=1; }
    done <<< "$MANIFEST"
    [ $fail -eq 0 ] && echo "✅ 上游：与 ${PIN:0:7} 逐字节一致"
    # 漂移看远端，不看本地 HEAD：检出是故意停在固定提交上的，HEAD 永远等于 PIN（审计抓到的空过）
    remote="$(git -C "$SDK" rev-parse -q --verify origin/HEAD 2>/dev/null || git -C "$SDK" rev-parse -q --verify origin/main 2>/dev/null)"
    if [ -z "$remote" ]; then
        echo "⏳ $SDK 没有 origin/HEAD 或 origin/main，看不了上游漂移"
    elif [ "$remote" = "$PIN" ]; then
        echo "✅ 上游远端（最近一次 fetch 时）就是 ${PIN:0:7}，没有漂移 —— 要看最新的先 git -C $SDK fetch"
    else
        changed="$(git -C "$SDK" diff --name-only "$PIN" "$remote" -- $(awk '{print $3}' <<< "$MANIFEST" | sort -u))"
        if [ -n "$changed" ]; then
            echo "⏳ 上游 ${PIN:0:7}..${remote:0:7} 动过这些我们引入的文件（跟进时逐个看 diff，结论记进 UPSTREAM.md 跟进记录）："
            sed 's/^/     /' <<< "$changed"
        else
            echo "✅ 上游 ${PIN:0:7}..${remote:0:7} 有新提交，但没动过我们引入的文件"
        fi
    fi
fi
[ $fail -eq 0 ] || exit 1
