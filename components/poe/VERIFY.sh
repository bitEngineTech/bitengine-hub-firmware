#!/bin/bash
# components/poe/VERIFY.sh — 设计约束的机器验证
# ⚠️ 本文件是**补写**的（2026-08-13）：建组件时违反了 P1「先写 VERIFY.sh 再写代码」。
#    补写的代价是显性的 —— verdict 那个误报硬件故障的 bug，如果先写了
#    「判定只能依赖 no_pull」这条检查，写代码时就不会加进去。
cd "$(dirname "$0")"
PASS=0; FAIL=0
check() { if eval "$2" >/dev/null 2>&1; then echo "✅ $1"; ((PASS++)); else echo "❌ $1"; ((FAIL++)); fi; }

echo "=== poe 模块验证 ==="
check "poe.c 存在"            "test -f poe.c"
check "poe.h 存在"            "test -f include/poe.h"
check "poe_init"              "grep -q 'esp_err_t poe_init' poe.c"
check "poe_is_present"        "grep -q 'bool poe_is_present' poe.c"
check "poe_probe"             "grep -q 'esp_err_t poe_probe' poe.c"

echo ""
echo "--- 电气安全 ---"
# 外部接着来自 5V 轨的分压，配成输出就是内部驱动器对顶外部电路
check "只配输入，绝不配输出"  "! grep -q 'GPIO_MODE_OUTPUT' poe.c"
check "不驱动该脚"            "! grep -q 'gpio_set_level' poe.c"
# 探测完必须恢复无上下拉：留着上拉会让无 PoE 时读到不确定区电平
check "探测后恢复无上下拉"    "grep -A3 'configure(GPIO_FLOATING)' poe.c | tail -5 | grep -q 'return err'"
check "引脚号来自 devkit_pins" "grep -q 'POE_DETECT_PIN' poe.c && ! grep -qE 'GPIO_NUM_0|pin_bit_mask = 1ULL << 0' poe.c"

echo ""
echo "--- 判定逻辑（防止再次误报硬件故障）---"
# 曾经想用 pull_up 区分"引脚悬空"和"正常无 PoE"，实测证明分辨不了：
#   真悬空+上拉 → 3.3V；无 PoE+上拉 → 2.28V；GPIO 翻转点 ≈1.65V，两者都读 1。
# 会让人去查一个不存在的硬件问题。
check "REST 不输出 floating"   "! grep -q '\"floating\"' ../webui/rest_api.c"
check "判定只取 no_pull"       "grep -q 'np ? \"poe\" : \"usb\"' ../webui/rest_api.c"
check "头文件写明分辨不了"     "grep -q '分辨不了' include/poe.h"
# 固件测不出电压上限，必须在回包里说出来，否则会被当成"已验证安全"
check "回包声明需万用表核算"   "grep -q 'meter' ../webui/rest_api.c"

echo ""
echo "==============================="
echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
