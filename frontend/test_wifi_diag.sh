#!/bin/bash
# frontend/test_wifi_diag.sh —— WiFi 页「链路健康」显示的确实是设备报的回传诊断（不用板子，只要 node）
#
# 为什么要有这个文件（2026-10-06）：固件 v0.5.0-395 起 /status 就有 wifi_disconnects /
# wifi_down_s / wifi_last_outage_s / wifi_longest_outage_s / wifi_channel / wifi_dfs，
# 但前端零显示 —— 用户手册排障只能叫人去读原始 JSON。字段语义有三个坑，正是本测试要钉的：
#   1. wifi_down_s 是"当前这次断线已持续多久"（在线为 0），不是累计时长
#   2. last / longest 在还没有一次断线恢复过时是 null（0 秒是真实可能的值）→ 显示"暂无"
#   3. 老固件没有这些字段 → 一律 "--"，绝不能当成 0 显示成"一切正常"
# 另外 DFS 提示里的推荐信道与用户手册第一节两侧都验（CLAUDE.md 7.6 规矩 3）。
#
# DOM 桩照抄 test_device_info.sh，同样的局限：不解析 HTML、不执行内联 onclick。

cd "$(dirname "$0")"
command -v node >/dev/null || { echo "需要 node"; exit 1; }

RUN=$(mktemp /tmp/wifidiagtest.XXXX.cjs)
trap 'rm -f "$RUN"' EXIT

cat > "$RUN" << 'HARNESS'
const fs = require('fs');
const FILES = ['js/i18n.js', 'js/i18n/de.js', 'js/i18n/zh.js',
               'js/api.js', 'js/tz.js', 'js/ws.js', 'js/app.js', 'js/pages/wifi.js'];
const missing = FILES.filter(f => !fs.existsSync(f));
if (missing.length) {
    console.log(`❌ 源文件缺失：${missing.join(', ')}`);
    console.log('\nResult: 0 passed, 1 failed');
    process.exit(1);
}
const SRC = FILES.map(f => fs.readFileSync(f, 'utf8')).join('\n');

function makeHarness(fetchImpl) {
    const els = new Map(), owner = new Map();
    const mkEl = (id, ownerId) => {
        const el = {
            id, value: '', type: '', textContent: '', disabled: false, checked: false,
            style: {}, className: '', _html: '', _listeners: {}, onclick: null,
            get innerHTML() { return this._html; },
            set innerHTML(v) { this._html = v; dropOwned(this.id); scan(v, this.id); },
            addEventListener(t, f) { (this._listeners[t] ||= []).push(f); },
            focus() {}, setAttribute() {},
            classList: { toggle() {}, add() {}, remove() {}, contains() { return false; } },
        };
        els.set(id, el);
        if (ownerId !== undefined) owner.set(id, ownerId);
        return el;
    };
    const dropOwned = (o) => {
        for (const [id, ow] of [...owner])
            if (ow === o) { dropOwned(id); owner.delete(id); els.delete(id); }
    };
    const scan = (html, o) => {
        for (const m of String(html).matchAll(/id="([^"]+)"/g))
            if (!els.has(m[1])) mkEl(m[1], o);
    };
    mkEl('app'); mkEl('content');

    const document = {
        getElementById: id => els.get(id) || null,
        querySelector: () => null, querySelectorAll: () => [],
        createElement: () => mkEl('tmp' + Math.random()),
    };
    const location = { _h: '', host: '127.0.0.1',
        get hash() { return this._h; },
        set hash(v) { this._h = v ? (String(v).startsWith('#') ? String(v) : '#' + v) : ''; } };
    const window = {};
    const WebSocket = function () { this.readyState = 0; this.close = () => {}; };
    const navigator = { clipboard: { writeText: async () => {} }, language: 'en-US' };
    const timers = [];
    const setInterval  = (fn, ms) => { timers.push({ fn, ms, cleared: false }); return timers.length; };
    const clearInterval = (id) => { if (timers[id - 1]) timers[id - 1].cleared = true; };

    const factory = new Function(
        'window', 'document', 'location', 'navigator', 'fetch', 'WebSocket',
        'setTimeout', 'clearTimeout', 'setInterval', 'clearInterval', 'console', 'TextEncoder', 'Intl',
        SRC + '\n; return { App, API, WS, WiFiPage, Lang };');
    const ctx = factory(window, document, location, navigator, fetchImpl, WebSocket,
                        setTimeout, clearTimeout, setInterval, clearInterval, console, TextEncoder, Intl);
    return Object.assign(ctx, { document, els, timers });
}

const reply = (status, body) => ({ ok: status >= 200 && status < 300, status, json: async () => body });
let PASS = 0, FAIL = 0;
const check = (name, cond, detail) => {
    if (cond) { console.log(`✅ ${name}`); PASS++; }
    else { console.log(`❌ ${name}${detail ? ' — ' + detail : ''}`); FAIL++; }
};
const el = (h, id) => h.document.getElementById(id) ||
    { value: '', textContent: '(元素不存在)', checked: false, style: {}, innerHTML: '(元素不存在)', _html: '(元素不存在)' };
const shown = (h, id) => { const e = el(h, id); return e._html !== undefined && e._html !== '' ? e._html : e.textContent; };
const settle = async () => { for (let i = 0; i < 6; i++) await new Promise(r => setTimeout(r, 0)); };
process.on('unhandledRejection', () => { /* 其他卡片的 404 桩会让无关代码路径抛错，与本测试无关 */ });

const base = {
    wifi_connected: true, wifi_ssid: 'MyWiFi', wifi_ip: '192.168.1.36', wifi_rssi: -51,
    wifi_channel: 40, wifi_dfs: false, wifi_disconnects: 0, wifi_down_s: 0,
    wifi_last_outage_s: null, wifi_longest_outage_s: null,
};
const mkFetch = (status) => async (p) => {
    if (p === '/api/v1/status') return reply(200, status);
    return reply(404, { error: 'not_found' });
};
const page = async (status) => {
    const h = makeHarness(mkFetch(status));
    h.WiFiPage.render(el(h, 'content')); await settle();
    return h;
};

(async () => { try {
    // ---- 1. 在线、从没断过 ----
    let h = await page(base);
    check('有信道卡片，值 = /status.wifi_channel', shown(h, 'wifi-ch') === '40', JSON.stringify(shown(h, 'wifi-ch')));
    check('非 DFS 信道不挂 DFS 徽章', !/DFS/.test(shown(h, 'wifi-ch')));
    check('非 DFS 信道不出提示', el(h, 'wifi-dfs-warn').style.display === 'none');
    check('断线次数 = /status.wifi_disconnects（0 照实显示）', el(h, 'wifi-disc').textContent === '0', JSON.stringify(el(h, 'wifi-disc').textContent));
    check('在线时"当前断线"显示"无"', /badge-green/.test(shown(h, 'wifi-down')) && /None/.test(shown(h, 'wifi-down')), shown(h, 'wifi-down'));
    check('还没断过：上次断线显示"暂无"而不是 0', el(h, 'wifi-last').textContent === 'None yet', JSON.stringify(el(h, 'wifi-last').textContent));
    check('还没断过：最长断线显示"暂无"而不是 0', el(h, 'wifi-long').textContent === 'None yet');

    // ---- 2. DFS 信道 ----
    h = await page({ ...base, wifi_channel: 100, wifi_dfs: true });
    check('DFS 信道挂徽章', /100/.test(shown(h, 'wifi-ch')) && /DFS/.test(shown(h, 'wifi-ch')), shown(h, 'wifi-ch'));
    check('DFS 信道出提示', el(h, 'wifi-dfs-warn').style.display === '', JSON.stringify(el(h, 'wifi-dfs-warn').style.display));
    check('提示里带实际信道号', /Channel 100 is a DFS channel/.test(shown(h, 'wifi-dfs-warn')), shown(h, 'wifi-dfs-warn'));
    check('提示给出推荐信道 36–48 / 149–165', /36–48/.test(shown(h, 'wifi-dfs-warn')) && /149–165/.test(shown(h, 'wifi-dfs-warn')));

    // ---- 3. 断过几次、都恢复了 ----
    h = await page({ ...base, wifi_disconnects: 3, wifi_last_outage_s: 75, wifi_longest_outage_s: 3700 });
    check('断线次数 3', el(h, 'wifi-disc').textContent === '3');
    check('上次断线 75 s → 1m 15s', el(h, 'wifi-last').textContent === '1m 15s', JSON.stringify(el(h, 'wifi-last').textContent));
    check('最长断线 3700 s → 1h 1m', el(h, 'wifi-long').textContent === '1h 1m', JSON.stringify(el(h, 'wifi-long').textContent));
    h = await page({ ...base, wifi_disconnects: 1, wifi_last_outage_s: 0, wifi_longest_outage_s: 0 });
    check('断线 0 秒就恢复（真实值 0）显示 0s，不是"暂无"', el(h, 'wifi-last').textContent === '0s', JSON.stringify(el(h, 'wifi-last').textContent));

    // ---- 4. 正在断线（页面此刻是从以太网打开的）----
    h = await page({ ...base, wifi_connected: false, wifi_channel: null, wifi_dfs: null,
                     wifi_disconnects: 2, wifi_down_s: 42, wifi_last_outage_s: 10, wifi_longest_outage_s: 10 });
    check('正在断线：当前断线显示已持续 42s（红）', /badge-red/.test(shown(h, 'wifi-down')) && /42s/.test(shown(h, 'wifi-down')), shown(h, 'wifi-down'));
    check('没连上时信道显示 --', shown(h, 'wifi-ch') === '--', JSON.stringify(shown(h, 'wifi-ch')));
    check('没连上时不出 DFS 提示', el(h, 'wifi-dfs-warn').style.display === 'none');

    // ---- 5. WiFi 从没连过（只插网线用）----
    h = await page({ ...base, wifi_connected: false, wifi_channel: null, wifi_dfs: null, wifi_down_s: 0 });
    check('WiFi 从没连过：当前断线显示 --，不显示"无"', el(h, 'wifi-down').textContent === '--', JSON.stringify(shown(h, 'wifi-down')));

    // ---- 6. 老固件：字段缺失 ----
    const old = { wifi_connected: true, wifi_ssid: 'MyWiFi', wifi_ip: '192.168.1.36', wifi_rssi: -51 };
    h = await page(old);
    for (const id of ['wifi-ch', 'wifi-disc', 'wifi-down', 'wifi-last', 'wifi-long'])
        check(`老固件缺字段时 ${id} 显示 --（不当成 0）`, shown(h, id) === '--', JSON.stringify(shown(h, id)));

    // ---- 7. 中文译文到位 ----
    h = makeHarness(mkFetch(base));
    if (h.Lang && typeof h.Lang.set === 'function') h.Lang.set('zh');
    else if (h.Lang) h.Lang._cur = 'zh';
    h.WiFiPage.render(el(h, 'content')); await settle();
    check('中文下"还没断过"显示"暂无"', el(h, 'wifi-last').textContent === '暂无', JSON.stringify(el(h, 'wifi-last').textContent));

    console.log(`\nResult: ${PASS} passed, ${FAIL} failed`);
    process.exit(FAIL ? 1 : 0);
} catch (e) { console.log('❌ harness 抛错：' + (e && e.stack || e)); console.log('\nResult: 0 passed, 1 failed'); process.exit(1); } })();
HARNESS

node "$RUN"; RC=$?

# ---- 文档判据（7.6 规矩 3）：DFS 提示里的推荐信道，代码与用户手册两侧都验 ----
D_PASS=0; D_FAIL=0
dcheck() { if eval "$2" >/dev/null 2>&1; then echo "✅ $1"; D_PASS=$((D_PASS+1)); else echo "❌ $1"; D_FAIL=$((D_FAIL+1)); fi; }
MANUAL=../docs/guide/Hub使用手册.md
dcheck "手册第一节推荐 36–48 / 149–165"   "grep -q '36–48 或 149–165' $MANUAL"
dcheck "页面 DFS 提示推荐同样的信道"       "grep -q 'set the router to channel 36–48 or 149–165' js/pages/wifi.js"
dcheck "手册排障指向 WiFi 页「链路健康」"  "grep -q '链路健康' $MANUAL"
echo "Docs: $D_PASS passed, $D_FAIL failed"

[ $RC -eq 0 ] && [ $D_FAIL -eq 0 ]
