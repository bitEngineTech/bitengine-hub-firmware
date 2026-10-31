#!/bin/bash
# 主机侧测试 —— 不需要板子、不需要 ESP-IDF，只要 gcc。
#
#   bash components/hub_core/test_host/run.sh
#
# hub_store_core.c 是 hubdb 落盘的全部判据（防抖、读己之写、格式版本、重试）。写错的表现要么是设置悄悄丢了，
# 要么是降级固件把新版本的数据覆盖掉 —— 板子上都极难归因，所以先在这里证明。编的是产品源文件本身。
# hub_json.c（6.9）：转义、UTF-8、截断后仍合法、分页回滚 —— 测试自带严格 JSON 校验器，把每个缓冲大小都跑一遍。
# hub_sanitize.c（6.8）：设备字符串与用户名的卫生（防提示注入）。hub_evlog.c（6.7）：最近事件环。
set -e
cd "$(dirname "$0")"
CC=${CC:-gcc}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
$CC -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I.. -o "$OUT/hub_store_test" hub_store_test.c ../hub_store_core.c
"$OUT/hub_store_test"
$CC -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I.. -I../include -o "$OUT/hub_json_test" hub_json_test.c ../hub_json.c
"$OUT/hub_json_test"
$CC -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I.. -I../include -o "$OUT/hub_sanitize_test" hub_sanitize_test.c ../hub_sanitize.c
"$OUT/hub_sanitize_test"
# hub_registry.c（6.3–6.5、附录 A）：权限 × 后端的过滤矩阵、永不注册的名字、体积；生成的 commands_v2 与附录 A 逐项比对（金标）
$CC -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I.. -I../include -o "$OUT/hub_registry_test" hub_registry_test.c ../hub_registry.c ../hub_json.c
"$OUT/hub_registry_test"
python3 check_commands_v2.py "$OUT/hub_registry_test"
# hub_policy.c / hub_model.c（6.2、6.5）：动作表、权限 × 动作 × 敏感矩阵、值解析、限速令牌桶
$CC -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I.. -I../include -o "$OUT/hub_policy_test" hub_policy_test.c ../hub_policy.c ../hub_model.c ../hub_registry.c ../hub_json.c
"$OUT/hub_policy_test"

# hub_jobs_core.c（6.6 作业表；M7 第三步）：开 / 结束 / 超时、在途上限、复用最早结束的格、迟到的完成不覆盖、时钟回绕
$CC -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I.. -I../include -o "$OUT/hub_jobs_test" hub_jobs_test.c ../hub_jobs_core.c
"$OUT/hub_jobs_test"

# hub_mcap.c（9.6 Matter 能力 / 类别；M7 第四步）：Eve Energy、Aqara 门窗两台真设备的访谈结果 + 表里每一行
$CC -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I.. -I../include -I../../matter_ctrl/include -o "$OUT/hub_mcap_test" hub_mcap_test.c ../hub_mcap.c ../../matter_ctrl/mc_iv_core.c
"$OUT/hub_mcap_test"

# hub_evlog.c（6.7）：最近事件环（64 条、新的在前、按时间 / 条数筛、放不下撤半条带 truncated）。结果用 cJSON 解析回来比
CJSON="../../../managed_components/espressif__cjson/cJSON"
[ -f "$CJSON/cJSON.c" ] || { echo "❌ 找不到 $CJSON/cJSON.c（先 idf.py build 拉 managed_components）"; exit 1; }
$CC -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I.. -I../include -I"$CJSON" -o "$OUT/hub_evlog_test" hub_evlog_test.c ../hub_evlog.c ../hub_json.c "$CJSON/cJSON.c"
"$OUT/hub_evlog_test"

# hub_ti_core.c（第七节 thread_inventory 的纯逻辑；TC-TI01–04 主机侧）：子节点 / 经路由器 / 路由器关联、Matter 实例名与 _matterc TXT、
# 设备 id 与粘连、两轮缺席才算离开、撞车、满表 JSON
$CC -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I.. -I../include -I"$CJSON" -o "$OUT/hub_ti_test" hub_ti_test.c ../hub_ti_core.c ../hub_sanitize.c ../hub_json.c "$CJSON/cJSON.c"
"$OUT/hub_ti_test"

# hub_audit_core.c（6.10 审计环）：字段净化（命令名 / 设备 id / 动作可能来自 Muse 的报文）、新的在前、满环覆盖、截断
$CC -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I.. -I../include -I"$CJSON" -o "$OUT/hub_audit_test" hub_audit_test.c ../hub_audit_core.c ../hub_json.c "$CJSON/cJSON.c"
"$OUT/hub_audit_test"

# hub_zcap.c（8.8 Zigbee 能力与设备类；M5 第一步）：IAS 按 ZoneType、灯 / 插座 / 开关、按钮、传感器、电池只给非路由器、没访谈完 unknown
$CC -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I.. -I../include -I../../zhub/include -DCONFIG_BITENGINE_ENABLE_ZHUB=1 -o "$OUT/hub_zcap_test" hub_zcap_test.c ../hub_zcap.c ../../zhub/zh_dev.c ../../zhub/zh_ezsp_core.c ../../zhub/zh_zcl.c ../../zhub/zh_tbl.c ../../zhub/zh_tuya.c ../../zhub/zh_tuya_db.c ../../zhub/zh_lumi.c ../../zhub/zh_lumi_db.c -lm
"$OUT/hub_zcap_test"

# hub_meta_core.c（6.1 元数据、附录 A hub.device.update；M5 第二步）：id 解析、名字 / 房间净化与拒收、回到默认腾位、表满、
# 有页读不出来时不新建、读不出来的页不改、分页序列化来回一致、结构坏（长度 / 槽号 / 协议 / 没以 0 结尾）整页拒收、
# 单条名字不合格只丢那条、开机读页判定、跨页去重、事件句子里的设备名
$CC -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I.. -I../include -o "$OUT/hub_meta_test" hub_meta_test.c ../hub_meta_core.c ../hub_sanitize.c
"$OUT/hub_meta_test"
