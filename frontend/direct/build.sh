#!/bin/bash
# Build the Direct-profile frontend (docs/15_muse_direct_design_v1.md 2.7, cross-check A-29).
#
#   bash frontend/direct/build.sh        → frontend/direct/dist/index.html(.gz), www-direct/index.html.gz
#
# The Direct bundle is derived from the HA bundle instead of from a second file
# list. That keeps ../build.sh the only place that lists the shared sources, and
# leaves it — and everything else the HA bundle is made of — untouched, so the
# HA www.bin cannot change because of anything in this directory (architecture
# 5.8: the HA product must not change at all). tools/stage_www.py leaves
# frontend/direct/ out of the HA version's git ref for the same reason, and
# tools/check_profiles.py checks both bundles' section markers.
#
#   Direct = HA bundle
#            − the sections in DROP (pages for things the Direct profile does not have)
#            + i18n/zh.js right after js/i18n/zh.js (extra keys merged into the same table)
#            + js/shell.js right after js/app.js (navigation changes)
#            + every other js/*.js at the end (Direct-only pages, alphabetical)
#
# Every anchor is required: if ../build.sh renames or drops one, this fails
# instead of quietly shipping, say, a WiFi page to a hub that has no WiFi.
set -e
cd "$(dirname "$0")"

bash ../build.sh >/dev/null

mkdir -p dist
python3 - <<'EOF'
import pathlib, re, sys

DROP = ["js/pages/wifi.js"]          # no WiFi in the Direct profile (design 2.2); its endpoints are not registered
src = pathlib.Path("../dist/index.html").read_text(encoding="utf-8")

def die(msg):
    sys.exit(f"frontend/direct/build.sh: {msg}")

# ../build.sh writes "// --- <file> ---" before every inlined file
parts = re.split(r"^// --- (\S+) ---\n", src, flags=re.M)
if len(parts) < 3:
    die("no section markers in ../dist/index.html (did ../build.sh change format?)")
prefix, sections = parts[0], list(zip(parts[1::2], parts[2::2]))
tail_at = sections[-1][1].rfind("</script>\n</body>")
if tail_at < 0:
    die("closing </script></body> not found after the last section")
last_name, last_body = sections[-1]
sections[-1] = (last_name, last_body[:tail_at])
tail = last_body[tail_at:]

names = [n for n, _ in sections]
for need in DROP + ["js/i18n/zh.js", "js/app.js"]:
    if need not in names:
        die(f"anchor {need} not found in the HA bundle")

def section(path):
    p = pathlib.Path(path)
    if not p.is_file():
        die(f"{path} missing")
    return (f"direct/{path}", p.read_text(encoding="utf-8") + "\n")

pages = sorted(str(p) for p in pathlib.Path("js").glob("*.js") if p.name != "shell.js")
out = []
for name, body in sections:
    if name in DROP:
        continue
    out.append((name, body))
    if name == "js/i18n/zh.js":
        out.append(section("i18n/zh.js"))
    elif name == "js/app.js":
        out.append(section("js/shell.js"))
out += [section(p) for p in pages]

html = prefix + "".join(f"// --- {n} ---\n{b}" for n, b in out) + tail
pathlib.Path("dist/index.html").write_text(html, encoding="utf-8")
EOF

OUT=dist/index.html
RAW=$(wc -c < "$OUT")
# -n: no filename/mtime in the gzip header, so the bytes are stable (see ../build.sh)
gzip -n -k -f "$OUT"
GZ=$(wc -c < "$OUT.gz")
echo "Built Direct frontend: frontend/direct/$OUT"
echo "  Raw:  $(( RAW / 1024 )) KB"
echo "  Gzip: $(( GZ / 1024 )) KB"

# The Direct firmware build packs www.bin from here (top-level CMakeLists.txt)
mkdir -p ../../www-direct
cp "$OUT.gz" ../../www-direct/index.html.gz
