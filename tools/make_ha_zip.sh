#!/bin/bash
# tools/make_ha_zip.sh —— 打包 Home Assistant 自定义集成，随固件一起发
#
#   bash tools/make_ha_zip.sh
#
# 产出 dist/bitengine-ha-v<版本>.zip（dist/ 在 .gitignore 里，不入库）。
#
# ── 压缩包的形状是刻意的 ────────────────────────────────────────────
#
# 包里**只有** custom_components/bitengine/**，没有 README、没有 hacs.json、
# 没有顶层目录。因为用户的动作就一条：
#
#     unzip bitengine-ha-v0.2.0.zip -d /config
#
# 路径天然落在 HA 期望的位置，升级时原地覆盖，也不会往 /config 里扔垃圾。
# 多包一层目录会让用户多一步"把里面那个文件夹拷出来"，那步正是会做错的那步。
# 安装说明放 Hub 使用手册和 ha_integration/README.md —— 用户在那里读，
# 不是在 /config 里读。
#
# ── 为什么不入库 ────────────────────────────────────────────────────
#
# 同 make_windows_kit.sh：包是生成物，源头只有 ha_integration/ 一处。
# 进 git 等于同一份代码存两份，而且两份会不同步。
#
# ── 版本取自 manifest.json ──────────────────────────────────────────
#
# 不用 git describe。HA 自己认的版本号就是 manifest 里那个（HACS 也按它判断
# 有没有更新），包名与它不一致的话，用户装完在 HA 里看到的版本对不上文件名。

set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd)

SRC="$ROOT/ha_integration/custom_components/bitengine"
MANIFEST="$SRC/manifest.json"

die() { echo "❌ $1" >&2; exit 1; }

command -v zip >/dev/null || die "需要 zip（apt install zip）"
[ -f "$MANIFEST" ] || die "找不到 $MANIFEST"

VER=$(python3 -c "
import json,sys
m = json.load(open('$MANIFEST'))
for k in ('domain','version','name'):
    if k not in m: sys.exit(f'manifest 缺 {k}')
if m['domain'] != 'bitengine': sys.exit('domain 与目录名不符: ' + m['domain'])
print(m['version'])
") || die "manifest.json 不合格：$VER"

# ---- 打包**之前**先验源码树的形状 ---------------------------------------
# 这条和下面的"包 vs 源码"比对是两件事，别合并：
#   · 这里拦的是「源码树里本就不该有的文件」—— 它会一路进包、解压进用户的
#     /config/custom_components/。源码里多一个 STRAY.txt，包 vs 源码的比对是
#     **看不见的**（两边一起变），2026-09-11 的变异测试证实了这一点。
#   · 下面那条拦的是「打包过程漏掉或弄坏了文件」。
# 一个 HA 集成只该有 .py / .json 两类文件，外加 translations/ 一个子目录。
# ⚠️ 别用 `find -path './bitengine' -prune`：那会把整个目录剪掉，find 压根不往里
#    走，目录**内部**的杂物一个都查不出来（2026-09-11 变异测试 B/C 就这么逃掉的）。
#    所以是逐文件对白名单，不是剪枝。
BAD=$(cd "$ROOT/ha_integration/custom_components" && \
      find . -mindepth 1 ! -path '*__pycache__*' ! -name '*.pyc' -print | \
      grep -vE '^\./bitengine$|^\./bitengine/translations$|^\./bitengine/[^/]+\.(py|json)$|^\./bitengine/translations/[^/]+\.json$')
[ -z "$BAD" ] || die "源码树里有不该发布的东西（会解压进用户的 /config）：
$(echo "$BAD" | sed 's/^/     /')"

OUT="$ROOT/dist/bitengine-ha-v${VER}.zip"
mkdir -p "$ROOT/dist"
rm -f "$OUT"

# -x 掉 __pycache__：跑过的 HA 会在源码目录里留下 .pyc，它们被 .gitignore 忽略
# 所以在 git 里看不见，但**在工作区里是真实存在的**，不排除就会跟着进包。
# 带着另一个 Python 版本的字节码发给用户，轻则多几十 KB，重则误导排查。
( cd "$ROOT/ha_integration" && zip -q -r -X "$OUT" custom_components \
      -x '*__pycache__*' -x '*.pyc' ) || die "打包失败"

# ---- 自检：证明包的内容和形状都对，而不是"zip 没报错" -------------------
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
unzip -q "$OUT" -d "$TMP" || die "产出的 zip 解不开"

[ -f "$TMP/custom_components/bitengine/manifest.json" ] \
    || die "包里没有 custom_components/bitengine/manifest.json（路径形状错了）"

# 顶层只能有 custom_components 一项 —— 多出来的东西会被解压进用户的 /config
TOP=$(cd "$TMP" && ls -A)
[ "$TOP" == "custom_components" ] || die "包顶层不干净，含：$TOP"

if find "$TMP" -name '__pycache__' -o -name '*.pyc' | grep -q .; then
    die "包里混进了 __pycache__ / .pyc"
fi

# 逐文件比对：包里每个文件都必须与工作区逐字节相同，且**两边一个不多一个不少**。
# ⚠️ 比对范围是整个 custom_components/，不是 bitengine/ 子树 —— 2026-09-11 的变异
#    测试就栽在这里：往 custom_components/ 根放一个 STRAY.txt，它进了包、会被解压
#    进用户的 /config/custom_components/，而当时只数 bitengine/ 的自检一路全绿。
#    包里多出来的任何东西都会落进用户的配置目录，所以两边必须是**集合相等**。
ZIP_LIST=$(cd "$TMP/custom_components" && find . -type f | sort)
SRC_LIST=$(cd "$ROOT/ha_integration/custom_components" \
           && find . -type f ! -path '*__pycache__*' ! -name '*.pyc' | sort)
if [ "$ZIP_LIST" != "$SRC_LIST" ]; then
    echo "❌ 包内文件清单与源码不符：" >&2
    diff <(echo "$SRC_LIST") <(echo "$ZIP_LIST") | sed 's/^/     /' >&2
    exit 1
fi

MISMATCH=$(cd "$TMP/custom_components" && find . -type f | while read -r f; do
    cmp -s "$f" "$ROOT/ha_integration/custom_components/$f" || echo "$f"
done)
[ -z "$MISMATCH" ] || die "包内文件内容与源码不一致：$MISMATCH"

ZIP_N=$(echo "$ZIP_LIST" | wc -l)

echo "✅ $(basename "$OUT")"
echo "   版本 v$VER · $ZIP_N 个文件 · $(du -h "$OUT" | cut -f1)"
echo "   用户装法：unzip $(basename "$OUT") -d /config，然后完整重启 HA"
