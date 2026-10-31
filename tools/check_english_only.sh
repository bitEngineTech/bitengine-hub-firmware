#!/bin/bash
# tools/check_english_only.sh —— 前端源码「英文是唯一源语言」的机器检查
#
#   bash tools/check_english_only.sh            # 0 = 干净
#
# 规则：frontend/js、frontend/direct（直连型页面，docs/15 设计 2.7）、frontend/index.html、frontend/css 里不得出现 CJK 字符；
# 例外只有这几处：frontend/js/i18n/*.js 与 frontend/direct/i18n/*.js（译文是产品数据，不是源语言）、构建产物 dist/，和 js/i18n.js
# 本身（语言选择器里各语言用自己的文字命名，"中文" 必须是中文才找得到）。
# 用户可见的文案一律写英文并包进 t()，中文放进 js/i18n/zh.js。
#
# 为什么：t() 用英文原文当键，源码里一旦混进中文，它既进不了翻译表也换不了
# 语言，界面就成了中英夹杂（2026-09-05 之前 5 个页面正是这个状态）。
# i18n.js / zh.js 的头注释都引用了本脚本；frontend/test_i18n.sh 里有同样的断言，
# 这里单独成文件是为了能挂进 CI / pre-commit 而不用拉起 node。

set -u
cd "$(dirname "$0")/.."
# Capture first: `grep | head` would make the if-condition test head's exit
# status (always 0), i.e. always report a failure.
hits=$(grep -rnP '[\x{3400}-\x{9fff}\x{f900}-\x{faff}]' frontend/js frontend/direct frontend/index.html frontend/css \
        --include='*.js' --include='*.html' --include='*.css' \
        --exclude-dir=i18n --exclude-dir=dist --exclude=i18n.js)
if [ -n "$hits" ]; then
    echo "$hits" | head -20
    echo "❌ CJK text found outside frontend/js/i18n/ — write it in English and wrap it in t(); put the Chinese in js/i18n/zh.js"
    exit 1
fi
echo "✅ frontend sources are English-only (translations live in frontend/js/i18n/)"
