#!/bin/bash
# 主机侧测试：.gbl 容器解析 / 签名定位 / ECDSA 验签 / 剥签名 —— 不需要板子。
#
#   bash components/ota_mgr/test_host/run.sh
#
# 需要：gcc、OpenSSL 3 开发头（libssl-dev）、python3 + cryptography。
# 本机有 Simplicity Commander 时（COMMANDER= 或 ~/commander/commander/commander）
# 会多跑一段"金标"：用一把临时钥匙让 commander 给仓库里的真 NCP 镜像签名，
# 再用我们的 C 代码验签、剥签名，剥完必须与 commander 自己出的未签名包逐字节相同。
# 那一段证明的是"我们对 GBL 签名格式的理解 = Silicon Labs 工具的实际行为"，
# python 造的向量证明不了这一点。
#
# 为什么存在：验签逻辑写错的板上表现只有两种 —— 官方包被拒（用户刷不了），
# 或者假包被放行（白做了）。两种都不会给出指向根因的线索。这里把同一份
# gbl_image.c 编到主机上，对着真实 ECDSA-P256 跑 13 个向量。

set -e
cd "$(dirname "$0")"

CC=${CC:-gcc}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
FLAGS="-Wall -Wextra -Wno-unused-parameter -O1"
REPO=../../..

echo "=== 1/4 向量（python + cryptography，两把临时钥匙）==="
python3 make_vectors.py "$OUT/v"

echo
echo "=== 2/4 编译 gbl_image.c + OpenSSL 版平台层 ==="
$CC $FLAGS -I ../include -o "$OUT/gbl_test" gbl_test.c host_crypto.c ../gbl_image.c -lcrypto

echo
echo "=== 3/4 向量测试 ==="
"$OUT/gbl_test" "$OUT/v"

echo
echo "=== 3a/4 防降级（T9.25）：真实镜像 —— 当前发布版能刷，git 历史里两份老版（缓存 200）必须拒 ==="
"$OUT/gbl_test" buildtag "$REPO/mg24/release/zigbee_ncp_uart_hw.gbl" 400 allow
if git -C "$REPO" cat-file -e 84cc707:mg24/release/zigbee_ncp_uart_hw.gbl 2>/dev/null; then
    git -C "$REPO" show 84cc707:mg24/release/zigbee_ncp_uart_hw.gbl > "$OUT/old_xncp.gbl"     # 有 XNCP 构建串、没有 nvm3c=
    git -C "$REPO" show ee65940:mg24/release/zigbee_ncp_uart_hw.gbl > "$OUT/old_prexncp.gbl"  # 连构建串都没有
    "$OUT/gbl_test" buildtag "$OUT/old_xncp.gbl" -1 ncp_image_downgrade
    "$OUT/gbl_test" buildtag "$OUT/old_prexncp.gbl" -1 ncp_image_downgrade
else
    echo "⏭  不在 git 仓库里，跳过两份历史老镜像"
fi

echo
echo "=== 3b/4 开发者解锁状态机（假时钟）==="
$CC $FLAGS -I ../include -o "$OUT/unlock_test" unlock_test.c ../ncp_unlock.c
"$OUT/unlock_test"

echo
echo "=== 3c/4 进 bootloader 的串口时序（假 MG24：NCP / 已在 bootloader / RAILtest / 死机，两组波特率）==="
$CC $FLAGS -I ../include -o "$OUT/bl_entry_test" bl_entry_test.c ../mg24_bl_entry.c
"$OUT/bl_entry_test"

echo
echo "=== 4/4 公钥生成脚本：入库公钥必须通过，垃圾必须拒 ==="
python3 "$REPO/tools/gen_mg24_pubkey.py" "$REPO/mg24/release/mg24_gbl_signing_key.pub" "$OUT/pk.c"
grep -q 'mg24_gbl_pubkey_xy\[64\]' "$OUT/pk.c" && echo "✅ generator: committed key accepted"
printf -- '-----BEGIN PUBLIC KEY-----\nAAAA\n-----END PUBLIC KEY-----\n' > "$OUT/garbage.pub"
if python3 "$REPO/tools/gen_mg24_pubkey.py" "$OUT/garbage.pub" "$OUT/g.c" 2>/dev/null; then
    echo "❌ generator accepted garbage"; exit 1
else
    echo "✅ generator: garbage rejected"
fi

CMD=${COMMANDER:-$HOME/commander/commander/commander}
S37="$REPO/mg24/release/zigbee_ncp_uart_hw.s37"
if [ -x "$CMD" ] && [ -f "$S37" ]; then
    echo
    echo "=== 金标：commander 真实签名包（临时钥匙）==="
    "$CMD" util genkey --type ecc-p256 --privkey "$OUT/k.pem" --pubkey "$OUT/k.pub" >/dev/null 2>&1
    "$CMD" gbl create "$OUT/signed.gbl" --app "$S37" --sign "$OUT/k.pem" >/dev/null 2>&1
    "$CMD" gbl create "$OUT/unsigned.gbl" --app "$S37" >/dev/null 2>&1
    python3 "$REPO/tools/gen_mg24_pubkey.py" "$OUT/k.pub" --raw "$OUT/k.xy"
    "$OUT/gbl_test" golden "$OUT/signed.gbl" "$OUT/unsigned.gbl" "$OUT/k.xy"
else
    echo
    echo "(金标跳过：没有 commander 或 $S37)"
fi
