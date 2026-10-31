#!/bin/bash
# tools/signing_key.sh — 固件签名私钥的保管 / 校验 / 恢复
#
# 为什么需要这个脚本：本项目开的是「OTA 固件签名验证」档
# （CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT，见 sdkconfig.defaults 末尾），
# 信任锚是**当前运行的 app 自己的签名块**，不是 eFuse。所以：
#
#   * 用错钥匙签出来的固件，OTA 到已出货设备上会被拒（且拒得很安静）
#   * 私钥丢了 = 已出货设备永久失去 OTA 能力
#
# 而"我这次签对了钥匙吗"用肉眼看不出来 —— 签名块就是一坨二进制。
# 本脚本把它变成机器判据：仓库里入库了公钥指纹 secure_boot_pubkey.digest，
# 私钥推导出的指纹必须与它逐字节相同。
#
# 用法：
#   bash tools/signing_key.sh check              # 私钥在不在、指纹对不对
#   bash tools/signing_key.sh link               # 重建仓库根的软链接
#   bash tools/signing_key.sh ensure             # 构建期自动调用：没有钥匙就生成一把开发钥匙
#   bash tools/signing_key.sh verify <app.bin>   # 某个固件是不是这把钥匙签的
#   bash tools/signing_key.sh backup <目标目录>   # 导出私钥副本（会提示保管要求）

set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KEY_MASTER="${BITENGINE_SIGNING_KEY:-$HOME/.bitengine-keys/secure_boot_signing_key.pem}"
KEY_LINK="$REPO/secure_boot_signing_key.pem"
DIGEST_REF="$REPO/secure_boot_pubkey.digest"
# MG24 NCP 固件（.gbl）的签名钥匙：验签在 C5 侧（T1.2，2026-09-14），公钥编进固件、
# 私钥只在发 NCP 固件时用。构建不需要它，所以 ensure 不管它；check/backup 管。
MG24_KEY="$(dirname "$KEY_MASTER")/mg24_gbl_signing_key.pem"
MG24_PUB_REF="$REPO/mg24/release/mg24_gbl_signing_key.pub"

PASS=0; FAIL=0
ok()   { echo "✅ $1"; PASS=$((PASS+1)); }
bad()  { echo "❌ $1"; FAIL=$((FAIL+1)); }

need_espsecure() {
    if ! command -v espsecure >/dev/null 2>&1; then
        echo "espsecure 不在 PATH 里 —— 先 source ~/esp-idf-v6.0.2/export.sh" >&2
        exit 2
    fi
}

digest_of() {   # $1 = 私钥路径 → stdout 十六进制指纹
    local tmp; tmp="$(mktemp)"
    espsecure digest-sbv2-public-key --keyfile "$1" --output "$tmp" >/dev/null 2>&1 || { rm -f "$tmp"; return 1; }
    xxd -p "$tmp" | tr -d '\n'
    rm -f "$tmp"
}

cmd_link() {
    [ -f "$KEY_MASTER" ] || { echo "私钥本体不存在：$KEY_MASTER"; echo "若是新机器，从备份恢复到该路径后重跑。"; exit 1; }
    ln -sfn "$KEY_MASTER" "$KEY_LINK"
    echo "已重建软链接：$KEY_LINK -> $KEY_MASTER"
}

cmd_check() {
    need_espsecure
    echo "=== 私钥 ==="
    if [ -f "$KEY_MASTER" ]; then
        ok "私钥本体存在：$KEY_MASTER"
        local perm; perm="$(stat -c %a "$KEY_MASTER")"
        # 400/600 都可以；关键是 group/other 一位都不给
        case "$perm" in 400|600) ok "权限 $perm（group/other 无权限）";;
                        *)       bad "权限 $perm —— 应为 400 或 600，跑 chmod 400 '$KEY_MASTER'";; esac
    else
        bad "私钥本体缺失：$KEY_MASTER"
        echo "   → 从备份恢复到该路径，再跑 $0 link"
        echo ""; echo "Result: $PASS passed, $FAIL failed"; exit 1
    fi

    if [ -L "$KEY_LINK" ]; then
        ok "仓库根是软链接（git clean -xdf 删链接不删本体）"
    elif [ -f "$KEY_LINK" ]; then
        bad "仓库根是**真实文件**不是软链接 —— 一次 git clean -xdf 就会永久删掉它"
        echo "   → 先确认它已备份，再跑 $0 link 换成软链接"
    else
        bad "仓库根缺 $KEY_LINK —— 构建会失败。跑 $0 link"
    fi

    echo "=== 指纹（信任锚）==="
    [ -f "$DIGEST_REF" ] || { bad "缺 $DIGEST_REF —— 无从判断钥匙对不对"; echo ""; echo "Result: $PASS passed, $FAIL failed"; exit 1; }
    local want have
    want="$(xxd -p "$DIGEST_REF" | tr -d '\n')"
    have="$(digest_of "$KEY_MASTER")" || { bad "无法从私钥推导指纹（文件损坏？）"; echo ""; echo "Result: $PASS passed, $FAIL failed"; exit 1; }
    if [ "$want" = "$have" ]; then
        ok "指纹与入库值一致：${have:0:16}…${have: -8}"
    else
        bad "指纹与入库值不符 —— 用这把钥匙签出的固件推不动已出货设备"
        echo "   入库值: $want"
        echo "   本机值: $have"
        # 两种完全不同的处境共用这一条判据，别把它们搞混：
        echo "   两种可能："
        echo "     a) 这是构建期自动生成的**开发钥匙**（全新克隆）—— 开发时正常，"
        echo "        你有自己的信任域；但**绝不能用它签发版镜像**。"
        echo "     b) 官方私钥拿错/恢复错了 —— 从备份重新恢复到 $KEY_MASTER。"
    fi

    echo "=== MG24 .gbl 签名钥匙（验签在 C5 侧，T1.2）==="
    if [ -f "$MG24_KEY" ]; then
        ok "MG24 私钥存在：$MG24_KEY"
        local mperm; mperm="$(stat -c %a "$MG24_KEY")"
        case "$mperm" in 400|600) ok "权限 $mperm";;
                         *)       bad "权限 $mperm —— 应为 400 或 600，跑 chmod 400 '$MG24_KEY'";; esac
        if [ -f "$MG24_PUB_REF" ]; then
            if diff -q <(openssl ec -in "$MG24_KEY" -pubout 2>/dev/null) "$MG24_PUB_REF" >/dev/null 2>&1; then
                ok "私钥推出的公钥 = 入库的 mg24/release/mg24_gbl_signing_key.pub"
            else
                bad "私钥推出的公钥与入库公钥不符 —— 用它签的 .gbl 网关会拒（ncp_signature_invalid）"
            fi
        else
            bad "缺 $MG24_PUB_REF —— 固件编不进公钥，构建会失败"
        fi
    else
        bad "MG24 私钥缺失：$MG24_KEY —— 发不了 NCP 固件（构建不受影响，只用公钥）"
        echo "   → 从备份恢复；没有备份 = 换钥匙 + 先 OTA 一版带新公钥的 C5 固件"
    fi

    echo ""; echo "Result: $PASS passed, $FAIL failed"
    [ $FAIL -eq 0 ] || exit 1
}

cmd_ensure() {
    # 构建期自动调用（顶层 CMakeLists.txt）。目的只有一个：让**素未谋面的贡献者**
    # `git clone && idf.py build` 一次通过，而不是撞上
    # "Secure Boot Signing Key ... does not exist" 直接退出。
    #
    # 为什么不是「关掉签名」：信任锚是运行中 app 自己的签名块（见文件头），
    # 所以每人一把自己的钥匙天然自洽 —— 自建→自刷→自己的 OTA 也能装。
    # 这与 Home Assistant OS 的做法同构（构建时无证书就自签一张并打进镜像 keyring）。
    #
    # ⚠️ 三种情形必须区别对待，第 2 种是本函数存在的真正理由：
    #   1. 钥匙能解析到       → 什么都不做，静默（否则每次构建都刷屏）
    #   2. **软链接悬空**      → 大声失败，**绝不生成**
    #   3. 三者皆无（新克隆） → 生成开发钥匙 + 打横幅
    #
    # 第 2 种为什么危险：软链接存在说明「这台机器本该有一把真钥匙」，
    # 悬空通常意味着加密卷没挂载 / 备份没恢复 / 路径被挪走。此时若照常生成，
    # espsecure 会**顺着悬空链接把新钥匙写到 KEY_MASTER 那个路径上**，
    # 于是真钥匙的规范位置被一把冒牌货占住，而构建一路绿灯 ——
    # 等发现时，已经用错钥匙签了发版固件。
    if [ -r "$KEY_LINK" ]; then
        return 0
    fi

    if [ -L "$KEY_LINK" ]; then
        local target; target="$(readlink -f "$KEY_LINK" 2>/dev/null || readlink "$KEY_LINK")"
        cat >&2 <<EOF

╔══════════════════════════════════════════════════════════════════════╗
║  签名私钥的软链接悬空 —— 已停止构建，且**没有**生成新钥匙            ║
╚══════════════════════════════════════════════════════════════════════╝
  $KEY_LINK
      -> $target   （不存在）

  软链接存在说明这台机器本该有一把真钥匙。常见原因：
    * 存放私钥的加密卷/外部盘没挂载
    * 换了机器，备份还没恢复
    * 私钥被挪到了别处

  此处**故意不自动生成** —— 若照常生成，新钥匙会顺着悬空链接写到上面
  那个路径，真钥匙的规范位置就被冒牌货占住，而构建全程绿灯。
  等到发现时，发版固件已经用错钥匙签过了。

  恢复：把私钥还原到 $KEY_MASTER 后重新构建。
  确实想要一把开发钥匙：先 rm '$KEY_LINK'，再构建。
EOF
        exit 1
    fi

    # 情形 3：全新克隆。本体存在就只补链接，否则生成。
    if [ ! -f "$KEY_MASTER" ]; then
        need_espsecure
        mkdir -p "$(dirname "$KEY_MASTER")"
        # 方案必须与 sdkconfig.defaults 的 CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME 一致
        espsecure generate-signing-key --version 2 --scheme rsa3072 "$KEY_MASTER" >/dev/null 2>&1 \
            || { echo "espsecure generate-signing-key 失败" >&2; exit 1; }
        chmod 400 "$KEY_MASTER"
        local d; d="$(digest_of "$KEY_MASTER")"
        cat >&2 <<EOF

╔══════════════════════════════════════════════════════════════════════╗
║  已为你生成一把**开发用**固件签名钥匙                                ║
╚══════════════════════════════════════════════════════════════════════╝
  $KEY_MASTER   （权限 400，不在仓库内，不会被 git 收录）
  公钥指纹: ${d:0:16}…

  这是什么意思：
    ✅ 你能构建、能串口刷、你自建固件之间也能互相 OTA
    ⚠️ 刷过自建固件的设备会**拒绝官方 OTA** —— 两把钥匙 = 两个信任域
    ✅ 串口刷回官方固件即可恢复官方 OTA（信任锚取自当前运行的固件本身）

  出货固件请用官方钥匙签；详见 docs/guide/固件签名与密钥保管.md
EOF
    fi
    ln -sfn "$KEY_MASTER" "$KEY_LINK"
}

cmd_verify() {
    need_espsecure
    local img="${1:-}"
    [ -f "$img" ] || { echo "用法：$0 verify <app.bin>"; exit 2; }
    # espsecure 用私钥推公钥再验；不需要设备、不需要板子。
    # ⚠️ 判据只能用**退出码**（0=通过 / 2=失败），绝不能 grep 输出：
    # 通过时打的是 "Signature block 0 is valid"，失败时打的是
    # "Signature block 0 invalid" —— 后者含前者的子串，grep 'valid' 恒真。
    # 2026-08-19 第一版就是这么写的，变异测试里未签名镜像照样报绿。
    if espsecure verify-signature --version 2 --keyfile "$KEY_MASTER" "$img" >/dev/null 2>&1; then
        ok "$img 由本仓库的签名钥匙签发"
    else
        bad "$img 验签失败 —— 未签名，或签的是另一把钥匙"
        echo "   ⚠️ 把这个镜像刷进设备后，它将拒绝一切后续 OTA"
        exit 1
    fi
}

cmd_backup() {
    local dst="${1:-}"
    [ -n "$dst" ] || { echo "用法：$0 backup <目标目录>"; exit 2; }
    [ -f "$KEY_MASTER" ] || { echo "私钥本体不存在：$KEY_MASTER"; exit 1; }
    mkdir -p "$dst"
    install -m 400 "$KEY_MASTER" "$dst/secure_boot_signing_key.pem"
    cp "$DIGEST_REF" "$dst/secure_boot_pubkey.digest" 2>/dev/null || true
    if [ -f "$MG24_KEY" ]; then
        install -m 400 "$MG24_KEY" "$dst/mg24_gbl_signing_key.pem"
        cp "$MG24_PUB_REF" "$dst/mg24_gbl_signing_key.pub" 2>/dev/null || true
        echo "（含 MG24 .gbl 签名钥匙）"
    fi
    echo "已导出到 $dst/"
    cat <<'NOTE'

⚠️ 保管要求（这是本档唯一的不可逆点）
  * 至少两份离线副本，物理隔离（不是同一台机器的两个目录）
  * 不进 git、不进云盘同步目录、不进聊天记录
  * 丢失后果：**已出货设备永久失去 OTA 能力**，只能拆机串口重刷。
    换一把钥匙救不回来 —— 设备信任的是它自己固件里那把。
  * 泄露后果：任何人都能签出被设备接受的固件。此时唯一的补救是
    带新公钥的固件先 OTA 铺开，未升级的设备仍然暴露。
NOTE
}

case "${1:-check}" in
    check)  cmd_check ;;
    ensure) cmd_ensure ;;
    link)   cmd_link ;;
    verify) shift; cmd_verify "$@" ;;
    backup) shift; cmd_backup "$@" ;;
    *) echo "用法：$0 {check|link|ensure|verify <app.bin>|backup <目录>}"; exit 2 ;;
esac
