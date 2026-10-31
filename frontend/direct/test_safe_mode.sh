#!/bin/bash
# frontend/direct/test_safe_mode.sh —— 直连型的安全模式提示条（docs/15 附录 E33）的行为断言（不用板子，只要 node）
#
#   bash frontend/direct/test_safe_mode.sh
#
# 钉住：
#   · 只在 /status 的 safe_mode 为 true 时出现；不在安全模式时不去读 /diag/watchdog
#   · 原因（设备给的字符串）只经 textContent 进页面，不进 innerHTML
#   · 两个按钮的请求体：退出 = {}，退出并先关掉 Muse = {"muse_off":true}
#   · "Muse 会一直关着"只在回包确认 muse_off 时才说（老固件不认这个字段，回 ok 但没记下）
#   · 失败如实说：muse_hold_failed / not_in_safe_mode 各有各的话，按钮恢复可点
#   · 外壳每次渲染都查一次（App._renderShell 被包住）
# DOM 桩是 test_hub_devices.sh 的精简版，多一个 document.body.prepend。

cd "$(dirname "$0")"
command -v node >/dev/null || { echo "需要 node"; exit 1; }

RUN=$(mktemp /tmp/safemode.XXXX.cjs)
trap 'rm -f "$RUN"' EXIT

cat > "$RUN" << 'HARNESS'
const fs = require('fs');
const FILES = ['../js/i18n.js', '../js/i18n/de.js', '../js/i18n/zh.js', 'i18n/zh.js',
               '../js/api.js', '../js/ws.js', '../js/app.js', 'js/shell.js'];
const missing = FILES.filter(f => !fs.existsSync(f));
if (missing.length) { console.log(`❌ 源文件缺失：${missing.join(', ')}`); console.log('\nResult: 0 passed, 1 failed'); process.exit(1); }
const SRC = FILES.map(f => fs.readFileSync(f, 'utf8')).join('\n');

function makeHarness(fetchImpl) {
    const els = new Map();
    const mkEl = (id) => {
        const el = {
            id, className: '', textContent: '', disabled: false, style: {}, dataset: {}, _html: '', _listeners: {},
            get innerHTML() { return this._html; },
            set innerHTML(v) { this._html = v; for (const m of String(v).matchAll(/id="([^"]+)"/g)) if (!els.has(m[1])) mkEl(m[1]); },
            addEventListener(t, f) { (this._listeners[t] ||= []).push(f); },
            click() { (this._listeners.click || []).forEach(f => f({ preventDefault() {} })); },
            after() {}, remove() {}, setAttribute() {},
            classList: { toggle() {}, add() {}, remove() {} },
        };
        if (id) els.set(id, el);
        return el;
    };
    mkEl('app'); mkEl('content');
    const prepended = [];
    const document = {
        body: { prepend(x) { prepended.push(x); if (x.id) els.set(x.id, x); } },
        activeElement: null,
        getElementById: id => els.get(id) || null,
        querySelector: () => null,
        querySelectorAll: () => [],
        createElement: () => {
            const e = mkEl('');
            /* id 是创建之后才赋的：赋值时登记 */
            let _id = '';
            Object.defineProperty(e, 'id', { get: () => _id, set: v => { _id = v; els.set(v, e); } });
            return e;
        },
    };
    const location = { hash: '', host: '127.0.0.1', reload() {} };
    const factory = new Function(
        'window', 'document', 'location', 'fetch', 'WebSocket',
        'setTimeout', 'clearTimeout', 'setInterval', 'clearInterval', 'console', 'TextEncoder', 'confirm',
        SRC + '\n; return { App, API, SafeMode, I18N, Lang, t };');
    const ctx = factory({}, document, location, fetchImpl, function () { this.close = () => {}; },
                        () => 0, () => {}, () => 0, () => {}, console, TextEncoder, () => true);
    return Object.assign(ctx, { document, els, prepended });
}

const reply = (status, body) => ({ ok: status >= 200 && status < 300, status, headers: { get: () => null }, json: async () => body });
let PASS = 0, FAIL = 0;
const check = (name, cond, detail) => {
    if (cond) { console.log(`✅ ${name}`); PASS++; }
    else { console.log(`❌ ${name}${detail ? ' — ' + detail : ''}`); FAIL++; }
};
const settle = async () => { for (let i = 0; i < 8; i++) await new Promise(r => setImmediate(r)); };
const EVIL = '<img src=x onerror=alert(1)>自愈 3 次(上限 3): muse_sess';

const mkFetch = (S) => async (p, o) => {
    const method = (o && o.method) || 'GET';
    S.calls.push({ p, method, body: o && o.body !== undefined ? JSON.parse(o.body) : undefined });
    if (p === '/api/v1/status') return reply(200, { profile: 'direct', safe_mode: S.safe });
    if (p === '/api/v1/diag/watchdog') return reply(200, { safe_mode: S.safe, safe_reason: S.reason });
    if (p === '/api/v1/diag/safemode/exit') return reply(S.exitStatus, S.exitBody(o && o.body ? JSON.parse(o.body) : {}));
    return reply(404, { error: 'not_found' });
};
const okBody = b => b.muse_off === true ? { status: 'ok', note: 'rebooting', muse_off: true } : { status: 'ok', note: 'rebooting' };

(async () => {
    // ---- 不在安全模式 ----
    {
        const S = { calls: [], safe: false, reason: '' };
        const h = makeHarness(mkFetch(S));
        await h.SafeMode.check(); await settle();
        check('不在安全模式：没有提示条', !h.document.getElementById('safe-banner') && h.prepended.length === 0);
        check('不在安全模式：不去读 /diag/watchdog', !S.calls.some(c => c.p === '/api/v1/diag/watchdog'));
    }

    // ---- 在安全模式：出现、原因不进 innerHTML ----
    const S = { calls: [], safe: true, reason: EVIL, exitStatus: 200, exitBody: okBody };
    const h = makeHarness(mkFetch(S));
    await h.SafeMode.check(); await settle();
    const ban = h.document.getElementById('safe-banner');
    check('安全模式：出现提示条（banner-red）', !!ban && /banner-red/.test(ban.className));
    const msg = h.document.getElementById('safe-banner-msg');
    check('原因显示出来了（textContent）', !!msg && msg.textContent.includes(EVIL));
    check('原因不进任何 innerHTML', ![...h.els.values()].some(e => e._html.includes('onerror')));
    check('有两个按钮', !!h.document.getElementById('safe-exit') && !!h.document.getElementById('safe-exit-muse'));
    check('提示里说了"反复重启的是 Muse"该怎么选', /Exit and keep Muse off/.test(h.document.getElementById('safe-banner-hint').textContent));
    await h.SafeMode.check(); await settle();
    check('重复检查只插一条', h.prepended.length === 1);

    // ---- 退出并先关掉 Muse ----
    S.calls.length = 0;
    const p1 = h.SafeMode.exit(true);
    check('请求中两个按钮都禁用', h.document.getElementById('safe-exit').disabled && h.document.getElementById('safe-exit-muse').disabled);
    await p1;
    const c1 = S.calls.find(c => c.p === '/api/v1/diag/safemode/exit');
    check('关 Muse：POST {"muse_off":true}', c1 && c1.method === 'POST' && JSON.stringify(c1.body) === '{"muse_off":true}', JSON.stringify(c1 && c1.body));
    check('回包确认 muse_off：说 Muse 会一直关着', /Muse will stay off/.test(msg.textContent), msg.textContent);

    // ---- 普通退出 ----
    S.calls.length = 0;
    h.document.getElementById('safe-exit').click(); await settle();
    const c2 = S.calls.find(c => c.p === '/api/v1/diag/safemode/exit');
    check('普通退出（按钮）：POST {}', c2 && JSON.stringify(c2.body) === '{}', JSON.stringify(c2 && c2.body));
    check('普通退出：只说正在重启', msg.textContent === 'Restarting...', msg.textContent);

    // ---- 老固件：回 ok 但没有 muse_off ----
    S.exitBody = () => ({ status: 'ok', note: 'rebooting' });
    await h.SafeMode.exit(true);
    check('回包没确认 muse_off：不许承诺 Muse 关着', !/Muse will stay off/.test(msg.textContent), msg.textContent);

    // ---- 失败如实说 ----
    S.exitStatus = 500; S.exitBody = () => ({ error: 'muse_hold_failed' });
    await h.SafeMode.exit(true);
    check('muse_hold_failed：说没记下、没重启', /did not restart/.test(msg.textContent), msg.textContent);
    check('失败后按钮恢复可点', !h.document.getElementById('safe-exit').disabled && !h.document.getElementById('safe-exit-muse').disabled);
    S.exitStatus = 409; S.exitBody = () => ({ error: 'not_in_safe_mode' });
    await h.SafeMode.exit(false);
    check('not_in_safe_mode：说已经不在安全模式', /no longer in safe mode/.test(msg.textContent), msg.textContent);
    S.exitStatus = 503; S.exitBody = () => ({ error: 'whatever' });
    await h.SafeMode.exit(false);
    check('其它错误：说没能退出', /Could not exit safe mode/.test(msg.textContent), msg.textContent);

    // ---- 没收到回包 / 回包不是 JSON：按钮恢复，说清楚两种可能 ----
    S.exitStatus = 200; S.exitBody = okBody;
    const realFetch = h.API._fetch;
    h.API._fetch = async () => null;                               // 网络断了 / 设备已经在重启
    await h.SafeMode.exit(true);
    check('没收到回包：说"可能正在重启，过一分钟刷新"', /reload this page in a minute/.test(msg.textContent), msg.textContent);
    check('没收到回包：按钮恢复可点', !h.document.getElementById('safe-exit-muse').disabled);
    h.API._fetch = async () => ({ ok: true, status: 200, headers: { get: () => null }, json: async () => { throw new SyntaxError('not json'); } });
    let threw = false;
    try { await h.SafeMode.exit(false); } catch (e) { threw = true; }
    check('回包不是 JSON：不抛异常、说没能退出', !threw && /Could not exit safe mode/.test(msg.textContent), msg.textContent);
    check('回包不是 JSON：按钮恢复可点', !h.document.getElementById('safe-exit').disabled);
    h.API._fetch = realFetch;

    // ---- 外壳每次渲染都查一次 ----
    {
        const S2 = { calls: [], safe: true, reason: '' , exitStatus: 200, exitBody: okBody };
        const h2 = makeHarness(mkFetch(S2));
        try { h2.App._renderShell(); } catch (e) { /* 桩里的 DOM 不全：只关心有没有去查 */ }
        await settle();
        check('App._renderShell 之后查了 /status', S2.calls.some(c => c.p === '/api/v1/status'));
    }

    // ---- 中文 ----
    {
        const S3 = { calls: [], safe: true, reason: 'x', exitStatus: 200, exitBody: okBody };
        const h3 = makeHarness(mkFetch(S3));
        h3.Lang.current = 'zh'; if (h3.Lang.set) try { h3.Lang.set('zh'); } catch (e) {}
        await h3.SafeMode.check(); await settle();
        const m3 = h3.document.getElementById('safe-banner-msg');
        check('中文：提示条是中文', !!m3 && /安全模式/.test(m3.textContent), m3 && m3.textContent);
    }

    console.log(`\nResult: ${PASS} passed, ${FAIL} failed`);
    process.exit(FAIL ? 1 : 0);
})();
HARNESS

node "$RUN"
