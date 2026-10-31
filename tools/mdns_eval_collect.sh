#!/bin/bash
# 采集 matter-server 的 MDNS-EVAL 埋点，用于评估"能否让 matter-server 自己
# 在收到 mDNS Added 时强制重订阅、从而去掉外部 watchdog"。
#
# 要回答的两个问题见 tools/matter-server-patch/apply_patch.py 里的说明：
#   Q1 稳态下 Added 有多频繁（太频繁则强制重订阅会导致订阅抖动）
#   Q2 网关重启时设备产生的是 Added 还是 Updated（只有 Updated 则方案不成立）
#
# 用 docker logs --follow 而不是事后 grep：容器日志会轮转，事后捞可能已经丢了。
OUT=${1:-/var/tmp/mdns_eval.log}
echo "# collecting MDNS-EVAL -> $OUT (started $(date -Is))" >> "$OUT"
docker logs --follow --since 0m matter-server 2>&1 \
  | grep --line-buffered "MDNS-EVAL" \
  | while IFS= read -r line; do
        printf '%s %s\n' "$(date -Is)" "$line" >> "$OUT"
    done
