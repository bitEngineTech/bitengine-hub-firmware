#!/bin/bash
# tools/zhc_export/regen.sh —— 从社区设备库重新生成涂鸦表（components/zhub/zh_tuya_db.c）、小米表（zh_lumi_db.c）与金样例
# （components/zhub/test_host/zh_tuya_gold.inc 读方向、zh_tuya_gold_ctl.inc 写方向、zh_lumi_gold.inc 小米）
#
#   bash tools/zhc_export/regen.sh            # 装 ZHC_VERSION 那一版到临时目录、重新生成五份文件
#   bash tools/zhc_export/regen.sh --check    # 只核对：仓库里的五份是不是这一版重新生成的结果（逐字节）
#   ZHC_PKG=<已装好的包目录> bash tools/zhc_export/regen.sh [--check]   # 不联网，用现成的包
#
# 升级库版本：改 ZHC_VERSION、重跑、看导出脚本打印的统计与 git diff，跑 components/zhub/test_host/run.sh 与 VERIFY.sh。
# 需要：node（≥ 18）、npm（没给 ZHC_PKG 时）。生成物提交进仓库：编固件不需要 node。
set -euo pipefail
ZHC_VERSION=26.123.0
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DB="$ROOT/components/zhub/zh_tuya_db.c"
GOLD="$ROOT/components/zhub/test_host/zh_tuya_gold.inc"
GOLDC="$ROOT/components/zhub/test_host/zh_tuya_gold_ctl.inc"   # 导出脚本写在读方向那份旁边
LDB="$ROOT/components/zhub/zh_lumi_db.c"
LGOLD="$ROOT/components/zhub/test_host/zh_lumi_gold.inc"
CHECK=0
[ "${1:-}" = "--check" ] && CHECK=1

TMP="$(mktemp -d /tmp/zhc-export.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT
PKG="${ZHC_PKG:-}"
if [ -z "$PKG" ]; then
    (cd "$TMP" && npm init -y >/dev/null && npm install --ignore-scripts --no-audit --no-fund "zigbee-herdsman-converters@$ZHC_VERSION" >/dev/null)
    PKG="$TMP/node_modules/zigbee-herdsman-converters"
fi
GOT="$(node -p "require('$PKG/package.json').version")"
if [ "$GOT" != "$ZHC_VERSION" ]; then
    echo "❌ 包的版本是 $GOT，不是 ZHC_VERSION=$ZHC_VERSION" >&2
    exit 1
fi

if [ $CHECK = 1 ]; then
    node "$ROOT/tools/zhc_export/export_tuya.js" "$PKG" "$TMP/db.c" "$TMP/gold.inc"
    node "$ROOT/tools/zhc_export/export_lumi.js" "$PKG" "$TMP/ldb.c" "$TMP/lgold.inc"
    cmp -s "$TMP/db.c" "$DB" && cmp -s "$TMP/gold.inc" "$GOLD" && cmp -s "$TMP/zh_tuya_gold_ctl.inc" "$GOLDC" || { echo "❌ 仓库里的涂鸦生成物不是 $ZHC_VERSION 重新生成的结果（diff \"$TMP/db.c\" \"$DB\"）" >&2; exit 1; }
    cmp -s "$TMP/ldb.c" "$LDB" && cmp -s "$TMP/lgold.inc" "$LGOLD" || { echo "❌ 仓库里的小米生成物不是 $ZHC_VERSION 重新生成的结果（diff \"$TMP/ldb.c\" \"$LDB\"）" >&2; exit 1; }
    echo "✅ 生成物与 zigbee-herdsman-converters $ZHC_VERSION 一致"
else
    node "$ROOT/tools/zhc_export/export_tuya.js" "$PKG" "$DB" "$GOLD"
    node "$ROOT/tools/zhc_export/export_lumi.js" "$PKG" "$LDB" "$LGOLD"
fi
