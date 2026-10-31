#!/bin/bash
# frontend/test_region.sh —— 射频地区设置（docs/14 第 7.5 节）的行为断言，不用板子，只要 node。
#
# 钉住的事（每一条都是"存了值 ≠ 生效"那类教训的翻版）：
#   1. 设置页的下拉框按 /api/v1/radio/region 回的 menu 渲染，多一项"未设置"，当前值来自设备
#   2. 保存前必须 confirm（应用会断 Zigbee 约 40 s）；用户取消就一个字节都不发
#   3. 发出去的 JSON 是 {"region":"US"} / {"region":null}，不是空串、不是小写
#   4. 结果行显示的是回包里的数（Zigbee 上限 / Thread dBm），页面自己不算
#   5. 回包 applied=deferred 时如实说"下次重启生效"；zigbee.ncp_xncp=false 时提示升级 NCP
#   6. Zigbee 页的快照表同样只显示回包字段（那部分在 test_zigbee.sh，这里不重复）
#
# DOM 桩照抄 test_device_info.sh；同样的局限：不解析 HTML、不执行内联 onclick。
cd "$(dirname "$0")"
command -v node >/dev/null || { echo "需要 node"; exit 1; }
RUN=$(mktemp /tmp/regiontest.XXXX.cjs)
trap 'rm -f "$RUN"' EXIT
cat > "$RUN" << 'HARNESS'
const fs = require('fs');
const FILES = ['js/i18n.js', 'js/i18n/de.js', 'js/i18n/zh.js',
               'js/api.js', 'js/tz.js', 'js/ws.js', 'js/app.js',
               'js/pages/dashboard.js', 'js/pages/settings.js', 'js/pages/zigbee.js'];
const missing = FILES.filter(f => !fs.existsSync(f));
if (missing.length) {
    console.log(`❌ 源文件缺失：${missing.join(', ')}`);
    console.log('\nResult: 0 passed, 1 failed');
    process.exit(1);
}
const SRC = FILES.map(f => fs.readFileSync(f, 'utf8')).join('\n');

function makeHarness(fetchImpl, confirmFn) {
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
    const Intl_ = Intl;

    const factory = new Function(
        'window', 'document', 'location', 'navigator', 'fetch', 'WebSocket',
        'setTimeout', 'clearTimeout', 'setInterval', 'clearInterval', 'console', 'TextEncoder', 'Intl', 'confirm',
        SRC + '\n; return { App, API, WS, SettingsPage, ZigbeePage };');
    const ctx = factory(window, document, location, navigator, fetchImpl, WebSocket,
                        setTimeout, clearTimeout, setInterval, clearInterval, console, TextEncoder, Intl_, confirmFn);
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
const settle = async () => { for (let i = 0; i < 8; i++) await new Promise(r => setTimeout(r, 0)); };
process.on('unhandledRejection', () => { /* 其他卡片的 404 桩会让无关代码路径抛错，与本测试无关 */ });

const baseStatus = {
    uptime: 4242, temperature: 41.5, free_heap: 100000, fw_version: 'v0.5.0-test', chip_revision: 'v1.0',
    hostname: 'bitengine', www_version: 'abc-def', mg24_alive: true, zigbee_channel: 20,
    active_interface: 'eth', eth_ip: '192.168.1.45', wifi_ip: '192.168.1.43', wifi_connected: true,
    eth_link: true, mac: 'aa:bb', bridge_mode: 'lan', tcp_clients: 1, mg24_resets: 0,
    uart_rx_bytes: 1, uart_tx_bytes: 1, poe_present: true, nvs_write_failures: 0, safe_mode: false,
    serial_number: 'BEH-000001',
};
/* 固件回包（docs/14 第 7.5 节）。数字故意各不相同，好证明页面显示的是回包字段而不是算出来的。 */
const regionGet = {
    region: 'EU', declared: true, menu: ['EU', 'US', 'CA', 'ZZ'], applied: null, reason: '',
    zigbee: { ncp_xncp: true, class: 2, cap_dbm: 10, requested_dbm: 8, host_class: 0, gw_class: 2,
              host_region: '', gw_region: 'EU', clamped_count: 0, read_uptime_s: 12 },
    thread_tx_dbm: 10,
};
let postReply = null;           // POST /radio/region 的回包，每个场景自己设
const calls = [];
const mkFetch = () => async (p, opts) => {
    const method = (opts && opts.method) || 'GET';
    calls.push({ p, method, body: opts && opts.body });
    if (p === '/api/v1/status') return reply(200, baseStatus);
    if (p === '/api/v1/radio/region' && method === 'GET') return reply(200, regionGet);
    if (p === '/api/v1/radio/region' && method === 'POST') return postReply;
    return reply(404, { error: 'not_found' });
};
const confirmState = { result: true, calls: 0 };
const confirmFn = () => { confirmState.calls++; return confirmState.result; };
const posts = () => calls.filter(c => c.p === '/api/v1/radio/region' && c.method === 'POST');

(async () => { try {
    // ---- 1. 下拉框来自设备的 menu，当前值来自设备 ----
    let h = makeHarness(mkFetch(), confirmFn);
    h.SettingsPage.render(el(h, 'content')); await settle();
    const sel = el(h, 'set-region');
    check('设置页有地区下拉框', !!h.document.getElementById('set-region'));
    const opts = [...String(sel._html).matchAll(/<option value="([^"]*)"/g)].map(m => m[1]);
    check('选项 = "" + 设备 menu（EU/US/CA/ZZ）', JSON.stringify(opts) === JSON.stringify(['', 'EU', 'US', 'CA', 'ZZ']), JSON.stringify(opts));
    check('当前值 = 回包 region（EU）', sel.value === 'EU', JSON.stringify(sel.value));
    check('菜单标签是人话不是码', /European Union/.test(sel._html) && /United States/.test(sel._html) && /Not set/.test(sel._html));
    const res = el(h, 'set-region-result').textContent;
    check('结果行显示回包里的 Zigbee 上限与 Thread dBm', /Zigbee cap: 10 dBm/.test(res) && /Thread: 10 dBm/.test(res), res);

    // ---- 2. 取消 confirm → 一个字节都不发 ----
    confirmState.result = false; confirmState.calls = 0;
    sel.value = 'US';
    await h.SettingsPage._saveRegion(); await settle();
    check('保存前问过 confirm', confirmState.calls === 1);
    check('用户取消时不发 POST', posts().length === 0);

    // ---- 3. 确认 → POST {"region":"US"}，结果行更新为回包的新值 ----
    confirmState.result = true;
    postReply = reply(200, { ...regionGet, region: 'US', applied: 'now', reason: '',
        zigbee: { ...regionGet.zigbee, class: 3, cap_dbm: 19, gw_class: 3, gw_region: 'US' }, thread_tx_dbm: 20 });
    await h.SettingsPage._saveRegion(); await settle();
    check('发出 POST /api/v1/radio/region', posts().length === 1);
    check('POST 载荷 = {"region":"US"}', posts()[0] && posts()[0].body === JSON.stringify({ region: 'US' }), posts()[0] && posts()[0].body);
    check('applied=now → 徽章"已保存并应用"', /Saved and applied/.test(el(h, 'set-region-badge').textContent), el(h, 'set-region-badge').textContent);
    const res2 = el(h, 'set-region-result').textContent;
    check('结果行 = 回包的新值（19 / 20）', /Zigbee cap: 19 dBm/.test(res2) && /Thread: 20 dBm/.test(res2), res2);

    // ---- 4. 选"未设置" → POST {"region":null}（不是 "" 也不是漏掉键）----
    sel.value = '';
    postReply = reply(200, { ...regionGet, region: null, declared: false, applied: 'now' });
    await h.SettingsPage._saveRegion(); await settle();
    check('未设置 → POST 载荷 = {"region":null}', posts()[1] && posts()[1].body === JSON.stringify({ region: null }), posts()[1] && posts()[1].body);

    // ---- 5. 回包 deferred（MG24 正在升级）→ 如实说下次重启生效 ----
    sel.value = 'CA';
    postReply = reply(200, { ...regionGet, region: 'CA', applied: 'deferred', reason: 'bridge_busy' });
    await h.SettingsPage._saveRegion(); await settle();
    check('applied=deferred → 徽章写明下次重启生效', /after the next reboot/.test(el(h, 'set-region-badge').textContent), el(h, 'set-region-badge').textContent);

    // ---- 6. 旧 NCP 固件：zigbee.ncp_xncp=false → 提示升级，不显示假上限 ----
    postReply = reply(200, { ...regionGet, region: 'US', applied: 'now', zigbee: { ncp_xncp: false, read_uptime_s: 3 }, thread_tx_dbm: 20 });
    await h.SettingsPage._saveRegion(); await settle();
    const res6 = el(h, 'set-region-result').textContent;
    check('NCP 无 XNCP 时结果行提示升级、不编上限', /lacks region support/.test(res6) && !/Zigbee cap/.test(res6), res6);

    // ---- 7. 固件拒绝（不在菜单里）→ 显示固件的判决 ----
    postReply = reply(400, { error: 'region_not_in_menu' });
    await h.SettingsPage._saveRegion(); await settle();
    check('固件拒绝时显示"不在菜单里"', /not in the menu/.test(el(h, 'set-region-badge').textContent), el(h, 'set-region-badge').textContent);

    // ---- 8. 小写选项值不会出现：菜单码由设备给，页面不改写 ----
    check('没有向别的端点发过地区', !calls.some(c => /region/.test(c.p) && c.p !== '/api/v1/radio/region'));

    // ---- 9. GET 失败（老固件没有这个端点）→ 下拉框显示 unknown，页面不崩 ----
    h = makeHarness(async (p, opts) => (p === '/api/v1/status' ? reply(200, baseStatus) : reply(404, { error: 'not_found' })), confirmFn);
    h.SettingsPage.render(el(h, 'content')); await settle();
    check('老固件无端点时下拉框显示 unknown', /unknown/.test(el(h, 'set-region')._html), el(h, 'set-region')._html);
} catch (e) { console.log('❌ 测试脚本异常：' + (e && e.stack || e)); FAIL++; }
    console.log('');
    console.log(`Result: ${PASS} passed, ${FAIL} failed`);
    process.exit(FAIL === 0 ? 0 : 1);
})();
HARNESS
node "$RUN"
