#!/usr/bin/env python3
"""httpd 的 URI handler 槽位必须留足余量 —— 构建期判据。

    python3 tools/check_uri_slots.py

── 在防什么 ────────────────────────────────────────────────────────

静态通配 handler（"/*"）是**最后**注册的，所以 max_uri_handlers 一旦用满，
被挤掉的必然是它。现象：

    整个前端 404（httpd 自带的 "Nothing matches the given URI"），
    而所有 /api/v1 端点完全正常。

这个形状踩过两次：

  · 2026-08-11  上限 80，正好用满，加两个端点就出事。
    修法：调到 120 + static_files.c 加一条 ESP_LOGE。
  · 2026-09-16  120 又被正好用满（120/120，零余量），加一个
    /api/v1/diag/ncp_line 就越界。**那条 ESP_LOGE 只在串口里**，
    而发版金丝雀只测了 API、没打开过首页 —— 于是 v0.5.0-358 和 -365
    两个版本都带着"前端全 404"发布了出去，直到同事烧新板子才发现。

第二次复发的教训很明确：**运行时日志不是判据**，没人会在发版前专门读串口。
所以有了本脚本。它在 CI / VERIFY 里跑，数不够就红。

⚠️ 本脚本数的是**静态注册点**，不含运行时动态注册（本项目没有）。
   数法偏保守：数不准时宁可多算，不要少算。
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
W = ROOT / "components" / "webui"

# 主 httpd 上注册的各处（OTBR 的第二个 server 用自己的上限，不在此列）
# ⚠️ 只写**函数名**，不写返回类型 —— factory_api 的是 esp_err_t 而不是 void，
#    第一版按 "void register_..." 匹配，于是它被静默漏数了（少算 8 条）。
#    一个会少算的判据比没有判据更危险：它给人"已经检查过了"的错觉。
# 第三列：只在哪种配置里注册（None = 两种都有）。SMLIGHT 兼容层只在 HA 型、Muse / hub 端点只在直连型 + Muse。
# 每种配置分别算"共用 + 本配置专属"，与本配置的上限比（webui_server.c 里上限按配置分开写；HA 型的数一直是 160）。
# 共用部分照旧全数（直连型其实不注册 Wi-Fi、白名单等端点）：偏保守，多算不少算。
SOURCES = [
    ("auth.c",            "register_auth_handlers",     None),
    ("rest_api.c",        "register_rest_api_handlers", None),
    ("websocket.c",       "register_ws_handler",        None),
    ("otbr_api.c",        "register_otbr_handlers",     None),
    ("factory_api.c",     "register_factory_handlers",  None),
    ("smlight_compat.c",  "register_smlight_handlers",  "ha"),
    ("muse_api.c",        "register_muse_handlers",     "direct"),
    ("hub_api.c",         "register_hub_handlers",      "direct"),
]
PROFILES = ("ha", "direct")
MARGIN = 20   # 至少要留这么多空槽，别再出现"正好够"

def count_in(path: Path, func: str) -> int:
    s = path.read_text(encoding="utf-8")
    m = re.search(re.escape(func) + r"\s*\(httpd_handle_t[^;]*?\{", s)
    i = m.start() if m else -1
    if i < 0:
        # 硬失败，不是警告：漏数会让本脚本报出一个**偏小**的总数并放行，
        # 那比没有判据更糟 —— 它给人"已经检查过了"的错觉。
        print(f"❌ {path.name}: 找不到 {func}() 的定义 —— 注册函数改名或挪走了。"
              f"本脚本会因此漏数它的端点，请同步改 SOURCES 里的名字。")
        sys.exit(1)
    j = s.find("\n}\n", i)
    body = s[i:j if j > 0 else len(s)]
    # 两种写法：httpd_uri_t 数组字面量，或 URI_ENTRY 宏
    return len(re.findall(r"\{\s*\.uri\s*=", body)) + len(re.findall(r"URI_ENTRY\(", body))

total = 0
only = {}
print("主 httpd 上的 URI handler：")
for fn, func, prof in SOURCES:
    p = W / fn
    if not p.exists():
        print(f"❌ 找不到 {p.relative_to(ROOT)}"); sys.exit(1)
    n = count_in(p, func)
    if prof:
        only[prof] = only.get(prof, 0) + n
    else:
        total += n
    print(f"  {fn:20} {n:3}{'（只在 ' + prof + '）' if prof else ''}")
total += 1   # static_files.c 的 "/*" 通配，单独注册
print(f"  {'静态通配 /*':20}   1")

# 上限：要么一处（两种配置同值），要么 "#if CONFIG_BITENGINE_PROFILE_DIRECT … #else … #endif" 两处。别的写法硬失败 ——
# 认错了哪个是哪个，就会拿 HA 型的数去比直连型的上限
srv = (W / "webui_server.c").read_text(encoding="utf-8")
assigns = re.findall(r"config\.max_uri_handlers\s*=\s*(\d+)", srv)
pair = re.search(r"#if CONFIG_BITENGINE_PROFILE_DIRECT\s*\n\s*config\.max_uri_handlers\s*=\s*(\d+);[^\n]*\n"
                 r"#else\s*\n\s*config\.max_uri_handlers\s*=\s*(\d+);[^\n]*\n#endif", srv)
if len(assigns) == 1:
    limits = {p: int(assigns[0]) for p in PROFILES}
elif len(assigns) == 2 and pair:
    limits = {"direct": int(pair.group(1)), "ha": int(pair.group(2))}
else:
    print("❌ webui_server.c 里 config.max_uri_handlers 的写法认不出（要么一处，要么 #if CONFIG_BITENGINE_PROFILE_DIRECT / #else 两处）"
          " —— 判据无所依托")
    sys.exit(1)

bad = False
for prof in PROFILES:
    t = total + only.get(prof, 0)
    limit = limits[prof]
    free = limit - t
    print(f"\n  {prof:6} 合计 {t}（共用 {total} + 专属 {only.get(prof, 0)}）/ 上限 {limit}  →  余量 {free}（要求 ≥ {MARGIN}）")
    if t > limit:
        print(f"❌ {prof}：**已经超了**。静态通配 handler 注册不上，整个前端会返回 404"
              f"（\"Nothing matches the given URI\"），而 API 全部正常 —— "
              f"这个组合极难定位，别让它发出去。把 max_uri_handlers 调大。")
        bad = True
    elif free < MARGIN:
        print(f"❌ {prof}：余量只剩 {free}，不足 {MARGIN}。这个坑踩过两次，两次都是"
              f"\"正好够\"之后有人加了一个端点。现在就把 max_uri_handlers 调大，"
              f"代价是每个槽一个指针。")
        bad = True
if bad:
    sys.exit(1)

# ── 附带钉一条文档判据 ───────────────────────────────────────────────
# 这次事故的另一半是"发版清单里没写要验首页"。清单补进了运维手册，
# 那就得有人盯着它别被删掉 —— 否则下一个人照着删剩的清单发版，重蹈覆辙。
man = ROOT / "docs" / "guide" / "云端OTA运维手册.md"
if not (ROOT / "docs" / "开发索引.md").exists():      # 公开仓库不导出 docs/（tools/oss_export.py）
    print("✅ 槽位余量充足（运维手册不在本检出，其判据跳过）")
    sys.exit(0)
if not man.exists():
    print(f"❌ 找不到 {man.relative_to(ROOT)}")
    sys.exit(1)
mt = man.read_text(encoding="utf-8")
for kw, why in [("金丝雀验什么", "发版金丝雀的检查清单"),
                ("必须 200",     "「首页必须回 200」这一条"),
                ("串口烧不算金丝雀", "「串口烧不算金丝雀」这条前提")]:
    if kw not in mt:
        print(f"❌ 云端OTA运维手册.md 里找不到「{kw}」—— {why}是 2026-09-19 "
              f"用两个坏版本换来的，删了它下一个人会照着删剩的清单发版")
        sys.exit(1)

print("✅ 槽位余量充足；运维手册的金丝雀清单在位")
sys.exit(0)
