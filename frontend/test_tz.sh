#!/bin/bash
# frontend/test_tz.sh —— 验证 js/tz.js 的 POSIX TZ 推导（不用板子，只要 node）
#
# 只断言**功能部分**：UTC 偏移 + 夏令时切换规则。
# 不断言时区缩写：POSIX 只拿名字做 %Z 格式化，固件从不使用；Intl 对很多区
# 只给 "GMT+8" 这种非法名字，我们退回 LST/LDT 占位，那是有意为之。
# 也接受省略 "/2"：POSIX 规定切换时刻省略时默认 02:00，`M3.5.0` ≡ `M3.5.0/2`。

cd "$(dirname "$0")"
PASS=0; FAIL=0

command -v node >/dev/null || { echo "需要 node"; exit 1; }

RUN=$(mktemp /tmp/tzrun.XXXX.js)
trap 'rm -f "$RUN"' EXIT
cat js/tz.js > "$RUN"
echo 'console.log(TZ.detect().tz);' >> "$RUN"

# 期望值里的 /2 一律写成省略形式，与实现保持一致
check_tz() {
    local zone="$1" want="$2"
    local got
    got=$(TZ="$zone" node "$RUN" 2>&1 | tail -1)
    # 名字段替换成 N，只比较偏移与规则
    local gotn wantn
    gotn=$(echo "$got"  | sed 's/[A-Za-z]\{3,\}/N/g')
    wantn=$(echo "$want" | sed 's/[A-Za-z]\{3,\}/N/g')
    if [ "$gotn" == "$wantn" ]; then
        echo "✅ $zone → $got"; PASS=$((PASS+1))
    else
        echo "❌ $zone → $got（期望等价于 $want）"; FAIL=$((FAIL+1))
    fi
}

echo "=== POSIX TZ 推导 ==="
# ⚠️ POSIX 符号与直觉相反：PST8PDT = UTC-8，CST-8 = UTC+8。
#    下面的期望值若看着"反了"，那是对的，改之前先读 js/tz.js 顶部注释。
check_tz America/Los_Angeles "PST8PDT,M3.2.0,M11.1.0"
check_tz America/New_York    "EST5EDT,M3.2.0,M11.1.0"
check_tz America/Denver      "MST7MDT,M3.2.0,M11.1.0"
check_tz Asia/Shanghai       "CST-8"
check_tz Asia/Tokyo          "JST-9"
check_tz Asia/Kolkata        "IST-5:30"          # 半小时偏移
check_tz Europe/London       "GMT0BST,M3.5.0/1,M10.5.0"
check_tz Europe/Berlin       "CET-1CEST,M3.5.0,M10.5.0/3"
check_tz Australia/Sydney    "AEST-10AEDT,M10.1.0,M4.1.0/3"   # 南半球，切换顺序相反
check_tz Pacific/Auckland    "NZST-12NZDT,M9.5.0,M4.1.0/3"    # 南半球 + 最后一周
check_tz UTC                 "UTC0"

echo ""
echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
