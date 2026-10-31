#!/bin/bash
# components/button/VERIFY.sh — 按键模块设计约束的形状检查
# 编译通过后运行：bash components/button/VERIFY.sh
cd "$(dirname "$0")"
PASS=0; FAIL=0

check() {
    if eval "$2" > /dev/null 2>&1; then
        echo "✅ $1"; ((PASS++))
    else
        echo "❌ $1"; ((FAIL++))
    fi
}

echo "=== Button Verification ==="
echo ""
echo "--- 时长映射 ---"
check "100ms 下界（挡蹭碰）"       "grep -q 'PRESS_MIN_MS     100' button.c"
check "2s 上界（重启区间）"        "grep -q 'PRESS_MAX_MS    2000' button.c"
check "10s 恢复出厂"               "grep -q 'HOLD_FACTORY_MS 10000' button.c"
check "20s 卡键上界"               "grep -q 'HOLD_STUCK_MS   20000' button.c"
# 没有上界的话，被外壳压住一分钟的按键在弹起那一刻就是一次恢复出厂
check "计时循环受卡键上界约束"     "grep -q 'held < HOLD_STUCK_MS' button.c"
check "重启同时判上下界"           "grep -q 'held >= PRESS_MIN_MS && held < PRESS_MAX_MS' button.c"

echo ""
echo "--- 只有两个动作 ---"
# 切桥模式 / 清密码 已从按键撤掉，走 WebUI。加回来前先读 docs/07 §2.3
check "不切桥模式"                 "! grep -q 'ezsp_bridge_switch_mode' button.c"
check "不清 WebUI 密码"            "! grep -q 'config_mgr_clear_password' button.c"
check "不单独复位 MG24"            "! grep -q 'watchdog_reset_mg24' button.c"

echo ""
echo "--- 2-10s 保护带只发一个事件，不做动作（T1.2 步骤 3，2026-09-14）---"
# 那一档现在向默认事件循环发 BUTTON_EVENT_HOLD_CONFIRM；订阅者只有 ota_mgr，
# 且只在网页先发起了解锁时才有意义。没人 arm 时按键仍然什么都不做。
check "事件基底在 ifdef 之外定义（无按键构型也能链接）" "[ $(grep -n 'ESP_EVENT_DEFINE_BASE(BUTTON_EVENT)' button.c | cut -d: -f1) -lt $(grep -n '#ifdef CONFIG_BITENGINE_HAS_BUTTON' button.c | head -1 | cut -d: -f1) ]"
check "只在 2-10s 档发事件（有且只有一处）" "[ \$(grep -c 'BUTTON_EVENT_HOLD_CONFIRM, &held' button.c) -eq 1 ] && grep -B1 'BUTTON_EVENT_HOLD_CONFIRM, &held' button.c | grep -q 'held >= PRESS_MAX_MS && held < HOLD_FACTORY_MS'"
check "发事件不阻塞按键任务（等待 0）" "grep -A1 'BUTTON_EVENT_HOLD_CONFIRM, &held' button.c | grep -q 'sizeof(held), 0)'"
check "保护带里除发事件外没有动作"     "! grep -A3 'held >= PRESS_MAX_MS && held < HOLD_FACTORY_MS' button.c | grep -q 'restart\|factory_reset\|switch_mode'"
check "事件 id 在头文件里声明"          "grep -q 'BUTTON_EVENT_HOLD_CONFIRM' include/button.h && grep -q 'ESP_EVENT_DECLARE_BASE(BUTTON_EVENT)' include/button.h"

echo ""
echo "--- 恢复出厂保留 Thread ---"
check "按键传 KEEP_THREAD"         "grep -q 'config_mgr_factory_reset(FACTORY_RESET_KEEP_THREAD)' button.c"

echo ""
echo "--- 去抖与 LED ---"
# 松开边沿不去抖的话，一次接触抖动就等于按当时已累计的时长触发一次动作
check "松开边沿也去抖"             "grep -A5 '松开也要去抖' button.c | grep -q 'DEBOUNCE_MS'"
# set_state 会顶掉 s_state，而夜间模式是边沿触发的 —— 按一次按钮灯亮一整夜。
# 只禁调用（带括号），注释里解释为什么不用它是允许的
check "LED 走覆盖层不碰 s_state"   "! grep -q 'led_ctrl_set_state(' button.c"
# 两档必须不同色：用户松手前唯一的依据就是颜色，而区别是"重启"和"配置全没"
check "恢复出厂档色与重启档不同"   "grep -q 'led_ctrl_test(-1, 255, w, w' button.c"

echo ""
echo "--- 资源 ---"
# 恢复出厂在这个栈上跑一串 NVS 操作；太小会溢出（表现为"按满 10s 就重启"），
# 太大会挤掉本就只剩几百字节的内部 RAM 低水位。实测值见 reset path 那条日志
check "栈 = 3072（实测后可调）"    "grep -q 'xTaskCreate(button_task, \"button\", 3072' button.c"
check "gpio_config 返回值被检查"   "grep -q 'ESP_RETURN_ON_ERROR(gpio_config' button.c"
check "xTaskCreate 返回值被检查"   "grep -q 'xTaskCreate(button_task.*!= pdPASS' button.c"

echo ""
echo "--- 直连型：Muse 配对剖面里短按不重启（2026-10-22 上板）---"
# App 说"按一下按钮"时用户多半短按，原来那一下就是重启、配对被打断。只在 BITENGINE_ENABLE_MUSE_LINK 下编，HA 型的短按重启一个字节都不变
check "屏蔽分支只在 #if CONFIG_BITENGINE_ENABLE_MUSE_LINK 里，且在重启之前 continue" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('button.c').read())
m=re.search(r'#if CONFIG_BITENGINE_ENABLE_MUSE_LINK\\s*if \\(button_restart_inhibited\\(\\)\\) \\{[^}]*s_last_action = \\\"ignored\\\"; continue; \\}\\s*#endif\\s*ESP_LOGW\\(TAG, \\\"Short press[^;]*;\\s*watchdog_restart_esp32\\(\\);', open('button.c').read())
sys.exit(0 if m else 1)\""
check "设置 / 读取函数只在直连型编" "python3 -c \"
import re,sys; s=open('button.c').read()
m=re.search(r'#if CONFIG_BITENGINE_ENABLE_MUSE_LINK\\s*(?:/\\*.*?\\*/\\s*)?static volatile bool s_restart_inhibit;\\s*void button_set_restart_inhibit\\(bool on\\).*?bool button_restart_inhibited\\(void\\).*?#endif', s, re.S)
sys.exit(0 if m else 1)\""
check "muse_pair 在建配对任务之前打开屏蔽（不留空窗）" "python3 -c \"
import re,sys; s=open('../muse_pair/muse_pair.c').read()
f=s[s.index('esp_err_t muse_pair_boot_run('):]
i=f.find('button_set_restart_inhibit(true)'); j=f.find('xTaskCreate(pair_task')
sys.exit(0 if 0 <= i < j else 1)\""

echo ""
echo "--- 产测留痕（按键是整机唯一没有可观测性的输入）---"
# 四个动作里短按重启、>10s 擦配置，产线都不能碰；安全的只有 2-10s 保护带那一档。
# 在此之前那一按不留痕迹，"按键焊没焊好、去抖对不对"在产线上根本测不了。
check "暴露 button_get_stats"     "grep -q 'void button_get_stats' button.c"
check "无硬件分支也有桩"          "[ $(grep -c 'void button_get_stats' button.c) -eq 2 ]"
# 重启和恢复出厂都不返回，记账写在它们后面就永远记不上
check "留痕先于执行动作"          "[ $(grep -n 's_press_count++' button.c | cut -d: -f1) -lt $(grep -n 'watchdog_restart_esp32()' button.c | head -1 | cut -d: -f1) ]"
check "记录时长与分档"            "grep -q 's_last_action = (held' button.c"

echo ""
echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
