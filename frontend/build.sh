#!/bin/bash
# Build bitEngine Hub frontend: inline CSS+JS into single HTML, then gzip
set -e
cd "$(dirname "$0")"

OUT="dist/index.html"
mkdir -p dist

echo "Building bitEngine Hub frontend..."

# Start HTML
cat > "$OUT" << 'HEAD'
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>bitEngine Hub</title>
<style>
HEAD

# Inline CSS
cat css/style.css >> "$OUT"

cat >> "$OUT" << 'MID'
</style>
</head>
<body>
<div id="app">
    <div class="text-center" style="padding:40px;color:#888">Loading...</div>
</div>
<script>
MID

# Inline all JS in order.
# ⚠️ When editing this list, update the <script> list in index.html too (that
#    one is used when opening the sources directly during development).
# i18n.js must come first: the language tables call registerLang() from it, and
# every page calls t().
for f in js/i18n.js js/i18n/de.js js/i18n/zh.js js/api.js js/tz.js js/ws.js js/app.js js/pages/login.js js/pages/setup.js js/pages/dashboard.js js/pages/wifi.js js/pages/ethernet.js js/pages/zigbee.js js/pages/thread.js js/pages/spectrum.js js/pages/led.js js/pages/mqtt_webhook.js js/pages/ota.js js/pages/logs.js js/pages/settings.js; do
    echo "// --- $f ---" >> "$OUT"
    cat "$f" >> "$OUT"
    echo "" >> "$OUT"
done

cat >> "$OUT" << 'TAIL'
</script>
</body>
</html>
TAIL

# Size check
RAW=$(wc -c < "$OUT")
# -n: keep the original filename and mtime out of the gzip header. Without it
# every build produces different bytes, www.bin changes every time, and "did the
# frontend actually change?" becomes unanswerable (tools/stage_www.py relies on
# this being byte-stable).
gzip -n -k -f "$OUT"
GZ=$(wc -c < "$OUT.gz")

echo "Built: $OUT"
echo "  Raw:  $(( RAW / 1024 )) KB"
echo "  Gzip: $(( GZ / 1024 )) KB"

# Copy into the SPIFFS source dir: the top-level CMake www_stage target packs
# www.bin from here.
mkdir -p ../www
cp "$OUT.gz" ../www/index.html.gz
echo "Done."
