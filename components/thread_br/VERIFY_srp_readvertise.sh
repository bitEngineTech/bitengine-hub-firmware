#!/bin/bash
# components/thread_br/VERIFY_srp_readvertise.sh
#   SRP 代播服务重新宣告（bye + re-announce）的设计合规性自动验证
#   编译通过后运行：bash components/thread_br/VERIFY_srp_readvertise.sh
#
# 设计约束来源：本次会话的实测结论（见 srp_readvertise.c 顶部注释）
#   - 网关是 Thread 子设备 _matter._tcp 记录的唯一宣告者（tcpdump 已证）
#   - zeroconf 收到 TTL=0 的 PTR 会立即从缓存移除并回调 Removed
#   - matter-server 对 operational node 的 Removed 直接忽略，只有 Added 有用
#   - 所以必须 bye → 等一会 → 重新宣告，且间隔不能为 0

cd "$(dirname "$0")" || exit 1
SRC=srp_readvertise.c
HDR=include/srp_readvertise.h

PASS=0; FAIL=0
check() {
    if eval "$2" > /dev/null 2>&1; then
        echo "✅ $1"; ((PASS++))
    else
        echo "❌ $1"; ((FAIL++))
    fi
}

echo "=== 文件与构建接入 ==="
check "源文件存在"                  "[ -f $SRC ]"
check "头文件存在"                  "[ -f $HDR ]"
check "已加入 CMakeLists"           "grep -q 'srp_readvertise.c' CMakeLists.txt"
check "CMake 依赖 mdns 组件"        "grep -q 'mdns' CMakeLists.txt"
check "Kconfig 开关已定义"          "grep -q 'BITENGINE_SRP_READVERTISE' ../../main/Kconfig.projbuild"
check "bye 间隔可配置"              "grep -q 'BITENGINE_SRP_READV_BYE_GAP_MS' ../../main/Kconfig.projbuild"
check "thread_br 启动时拉起"        "grep -q 'srp_readv_start' thread_br.c"
check "thread_br 停止时收回"        "grep -q 'srp_readv_stop' thread_br.c"

echo ""
echo "=== 公开 API（声明 vs 定义）==="
for fn in srp_readv_start srp_readv_stop srp_readv_trigger_all srp_readv_get_stats; do
    check "$fn 已声明"              "grep -q '$fn' $HDR"
    check "$fn 已定义"              "grep -q '$fn(' $SRC"
done
check "OpenThread 关闭时有桩实现"   "grep -q '#else' $SRC && grep -c 'srp_readv_start' $SRC | grep -qv '^1$'"

echo ""
echo "=== 协议时序（这段错了功能就是零）==="
check "bye 与重宣告之间有延时"      "grep -A12 'err = mdns_service_remove_for_host' $SRC | grep -q 'vTaskDelay(pdMS_TO_TICKS(BYE_GAP_MS))'"
check "间隔下限 >= 1000ms"          "grep -q 'range 1000' ../../main/Kconfig.projbuild"
check "重宣告用 add_for_host"       "grep -q 'mdns_service_add_for_host' $SRC"
check "子类型（_I<fabric>）已重建"  "grep -q 'mdns_service_subtype_add_for_host' $SRC"
check "TXT 按 DNS-SD 线格式解析"    "grep -q 'parse_txt' $SRC"
# 2026-08-10 在板实测：PCB 处于 PROBE/ANNOUNCE（约 3.2s）时 bye 被静默丢弃，
# 开机那次就栽在这里（只产生 Updated）。两处等待缺一不可。
check "新服务先晾够再动手"          "grep -q 'SETTLE_ROUNDS' $SRC && grep -q 'rounds < SETTLE_ROUNDS' $SRC"
check "晾的时间 > 实测 3.2s 窗口"   "[ \$(( \$(grep -oP '#define SETTLE_ROUNDS\s+\K[0-9]+' $SRC) * \$(grep -oP '#define POLL_MS\s+\K[0-9]+' $SRC) )) -gt 3200 ]"
check "自己 add 之后也要等 PCB"     "grep -q 'SETTLE_MS' $SRC && grep -q 's_last_add_us' $SRC"
check "SETTLE_MS > 实测 3.2s 窗口"  "[ \$(grep -oP '#define SETTLE_MS\s+\K[0-9]+' $SRC) -gt 3200 ]"
check "add 后无条件记时刻"          "grep -A1 'err = republish(s);' $SRC | grep -q 's_last_add_us = esp_timer_get_time'"
check "删除前先确认服务确实存在"    "grep -q 'mdns_service_exists_with_instance' $SRC"

echo ""
echo "=== OT API 使用（H1：每个都查过文档）==="
for api in otSrpServerGetNextHost otSrpServerHostIsDeleted otSrpServerHostGetFullName \
           otSrpServerHostGetNextService otSrpServerServiceIsDeleted \
           otSrpServerServiceGetInstanceLabel otSrpServerServiceGetServiceName \
           otSrpServerServiceGetPort otSrpServerServiceGetTxtData \
           otSrpServerServiceGetNumberOfSubTypes otSrpServerServiceGetSubTypeServiceNameAt \
           otSrpServerParseSubTypeServiceName; do
    check "$api 已使用"             "grep -q '$api' $SRC"
done

echo ""
echo "=== S2 线程上下文 ==="
check "取 OT 锁有超时（非 portMAX_DELAY）" "! grep -n 'esp_openthread_lock_acquire(portMAX_DELAY)' $SRC"
# 只数真实调用：注释里也会提到这两个名字
check "OT 锁成对释放"               "[ \$(grep -c 'esp_openthread_lock_acquire(pdMS_TO_TICKS' $SRC) -eq \$(grep -c 'esp_openthread_lock_release()' $SRC) ]"
check "取锁失败的分支也放闸门"      "[ \$(grep -c 'ot_section_enter()' $SRC) -lt \$(grep -c 'ot_section_exit()' $SRC) ]"
check "持锁期间不调 mdns_*"         "! awk '/esp_openthread_lock_acquire/,/esp_openthread_lock_release/' $SRC | grep -q 'mdns_service'"
check "持锁期间不 vTaskDelay"       "! awk '/esp_openthread_lock_acquire/,/esp_openthread_lock_release/' $SRC | grep -q 'vTaskDelay'"
check "trigger_all 只置标志不干活"  "grep -A6 'srp_readv_trigger_all' $SRC | grep -q 's_force'"
check "任务有退出条件"              "grep -q 'while (s_task_run' $SRC"

echo ""
echo "=== S3 资源与失败路径 ==="
check "删掉后加不回来会重试"        "grep -q 'orphan' $SRC"
check "add 失败有重试循环"          "grep -q 'ADD_RETRY' $SRC"
check "add 最终失败记 ESP_LOGE"     "grep -B4 'ESP_LOGE' $SRC | grep -q 'add_for_host\|orphan'"
check "快照缓冲区有边界检查"        "grep -q 'MAX_TXT_BYTES' $SRC && grep -q 'txt_len >' $SRC"
check "服务数量有上限"              "grep -q 'MAX_SERVICES' $SRC"
check "TXT 条目数有上限"            "grep -q 'MAX_TXT_ITEMS' $SRC"
check "跟踪表条目会随服务消失回收"  "grep -q 'present' $SRC"
# 缓冲区 ~2.8KB，内部 SRAM 只剩 ~19KB（历史最低 13.9KB），必须落 PSRAM。
# 只在首次 start 分配一次、不释放 —— 不产生碎片，"禁 malloc"的初衷仍满足。
check "大缓冲区从 PSRAM 取"        "grep -q 'MALLOC_CAP_SPIRAM' $SRC"
check "只分配一次（有 guard）"      "grep -q 'if (!s_track)' $SRC"
check "PSRAM 拿不到有回退且告警"    "grep -A4 'heap_caps_calloc' $SRC | grep -q 'calloc(1' && grep -q 'fell back to internal RAM' $SRC"
check "静态占用 < 128B"            "[ \$(size -A ../../build/esp-idf/thread_br/CMakeFiles/__idf_thread_br.dir/srp_readvertise.c.obj 2>/dev/null | grep -E '^\.s?bss' | awk '{s+=\$2} END {print s+0}') -lt 128 ]"

echo ""
echo "=== S1 输入可信度（SRP 数据来自空口，全部当不可信）==="
check "标签复制用带长度的接口"      "! grep -qE '\bstrcpy\(|\bsprintf\(' $SRC"
check "标签缓冲区有显式长度"        "grep -q 'LABEL_LEN' $SRC"
check "服务名解析失败会跳过"        "grep -q 'split_service_name' $SRC"

echo ""; 
echo ""
echo "--- 重启前 goodbye（2026-08-15 新增）---"
# 开机后的 bye+重新宣告只解决"控制器收不到 Added"，解决不了"控制器压根没察觉
# 网关重启过"。实测：重启后 +49s 就宣告了，matter-server 全程没标 offline，
# 握着死订阅等到 6 分钟后设备被人按醒才恢复。
check "有 bye_all"               "grep -q 'void srp_readv_bye_all' srp_readvertise.c"
# 挂 shutdown handler 而不是逐个调用点加：esp_restart() 有十来处，漏一个就是静默退化
check "挂在 shutdown handler 上"  "grep -q 'esp_register_shutdown_handler(srp_readv_bye_all)' srp_readvertise.c"
# host 是宣告成功那一刻的快照 —— shutdown handler 里拿不到 SRP 注册表（要 OT 锁）
check "track 表存了 host"         "grep -q 'char host\[LABEL_LEN\]' srp_readvertise.c"
check "宣告成功时记 host"         "grep -q 's_track\[i\].host, LABEL_LEN' srp_readvertise.c"
# 只撤不加：马上要重启了，加回来会让 PCB 重进探测期反而吞掉 bye
check "bye_all 不重新发布"        "! grep -A20 'void srp_readv_bye_all' srp_readvertise.c | grep -q 'republish\|mdns_service_add'"
# 窗口放宽到 A34：函数体前面多了 FORCE_BYE 的提前返回块
check "发完等一下再返回"          "grep -A34 'void srp_readv_bye_all' srp_readvertise.c | grep -q 'vTaskDelay(pdMS_TO_TICKS(BYE_FLUSH_MS))'"

echo ""
echo "--- 强制 bye 默认关（2026-08-15）---"
# 收益只对 python matter-server 成立；代价（清对端缓存 / BYE_GAP_MS 不可发现）
# 对任何对端都存在，且恰好落在最脆弱的时刻。matter.js 实测零反应。
check "两处 bye 都受开关约束"     "[ $(grep -c 'CONFIG_BITENGINE_SRP_READV_FORCE_BYE' srp_readvertise.c) -ge 4 ]"
check "Kconfig 里默认 n"          "grep -A3 'config BITENGINE_SRP_READV_FORCE_BYE' ../../main/Kconfig.projbuild | grep -q 'default n'"
check "sdkconfig 实际是关的"      "! grep -q '^CONFIG_BITENGINE_SRP_READV_FORCE_BYE=y' ../../sdkconfig"
# 跟踪与告警不受影响：那部分只观察不改记录，是排查 proxy 静默失败的唯一手段
check "跟踪与告警仍然开着"        "grep -q '^CONFIG_BITENGINE_SRP_READVERTISE=y' ../../sdkconfig"
# 开关关掉时 readvertise_one() 直接返回 OK（只为标 done），此时什么都没宣告，
# 计数器不能照加 —— 否则 /api/v1/thread/readvertise 会报一个假数字
check "关闭时不虚报 readvertised"  "grep -B6 's_stat_readv++' srp_readvertise.c | grep -q 'CONFIG_BITENGINE_SRP_READV_FORCE_BYE'"


echo ""
echo "--- thread 灯不得由子节点数衍生（2026-08-15 回归）---"
# 子节点数只统计网关的直连子节点。网状网里终端设备可以挂在别的路由器下面，
# 网关子节点数为 0 完全正常。曾用 pending 在 total==0 时硬造"恢复中"，灯永远不转绿。
# 匹配"赋值给变量"这种调用形式，而不是裸词 —— 上面的注释里就提到了这个函数名，
# 用裸词会被自己的注释骗过去（同一个坑栽过第二次了）
check "LED 不掺 srp pending"      "! grep -qE '= *srp_readv_pending *\\(' ../thread_br/thread_br.c"

echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
