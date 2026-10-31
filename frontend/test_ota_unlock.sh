#!/bin/bash
# frontend/test_ota_unlock.sh —— 升级页「开发者解锁」徽章的**行为**断言（不用板子，只要 node）
#
# 为什么要有这个文件（2026-09-14，T1.2 步骤 3 上板时用户发现）：按键确认后设备 11 秒内就解锁了，
# 页面却过了一分钟才显示"允许中，剩 8:56"。原因是轮询链有一个静默断点：API.get() 把任何一次
# 失败的请求变成 null，_renderUnlock(null) 直接 return，**不再排下一次轮询** —— 一次丢包就让
# 倒计时冻在旧数字上，直到用户重进页面。设备是唯一的时间源，页面必须一直问下去。
#
# 钉住四件事：
#   1. 发起后进入轮询（1 s 一次）
#   2. 轮询失败一次（fetch 抛错 → null）链**不能断**，要继续排下一次
#   3. 设备回 unlocked 时徽章立刻变红、显示 mm:ss、出现「重新锁定」
#   4. 回到 locked 时停止轮询；destroy() 清掉定时器
#
# DOM 桩照抄 test_device_info.sh；这里把 setTimeout/clearTimeout 也换成桩，才能手动"拨"轮询。

cd "$(dirname "$0")"
command -v node >/dev/null || { echo "需要 node"; exit 1; }

RUN=$(mktemp /tmp/otaunlock.XXXX.cjs)
trap 'rm -f "$RUN"' EXIT

cat > "$RUN" << 'HARNESS'
const fs = require('fs');
const FILES = ['js/i18n.js', 'js/i18n/de.js', 'js/i18n/zh.js',
               'js/api.js', 'js/ws.js', 'js/app.js', 'js/pages/ota.js'];
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
            style: {}, className: '', _html: '', _listeners: {}, onclick: null, files: [],
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
    /* 定时器桩：id 从 1 起（0 会被 `if (this._x)` 当成没有）。测试自己"拨"它们。 */
    const timeouts = [];
    const setTimeout_  = (fn, ms) => { timeouts.push({ fn, ms, cleared: false, fired: false }); return timeouts.length; };
    const clearTimeout_ = (id) => { if (timeouts[id - 1]) timeouts[id - 1].cleared = true; };
    const intervals = [];
    const setInterval_  = (fn, ms) => { intervals.push({ fn, ms, cleared: false }); return intervals.length; };
    const clearInterval_ = (id) => { if (intervals[id - 1]) intervals[id - 1].cleared = true; };
    const dialogs = { confirmAnswer: true, confirms: 0, alerts: [] };
    const confirm_ = () => { dialogs.confirms++; return dialogs.confirmAnswer; };
    const alert_ = (m) => { dialogs.alerts.push(String(m)); };

    const factory = new Function(
        'window', 'document', 'location', 'navigator', 'fetch', 'WebSocket',
        'setTimeout', 'clearTimeout', 'setInterval', 'clearInterval', 'console', 'TextEncoder', 'Intl',
        'confirm', 'alert',
        SRC + '\n; return { App, API, WS, OTAPage };');
    const ctx = factory(window, document, location, navigator, fetchImpl, WebSocket,
                        setTimeout_, clearTimeout_, setInterval_, clearInterval_, console, TextEncoder, Intl,
                        confirm_, alert_);
    return Object.assign(ctx, { document, els, timeouts, intervals, dialogs });
}

const reply = (status, body) => ({ ok: status >= 200 && status < 300, status, json: async () => body });
let PASS = 0, FAIL = 0;
const check = (name, cond, detail) => {
    if (cond) { console.log(`✅ ${name}`); PASS++; }
    else { console.log(`❌ ${name}${detail ? ' — ' + detail : ''}`); FAIL++; }
};
const el = (h, id) => h.document.getElementById(id) ||
    { value: '', textContent: '(元素不存在)', checked: false, style: {}, className: '', innerHTML: '(元素不存在)' };
const settle = async () => { for (let i = 0; i < 8; i++) await new Promise(r => setTimeout(r, 0)); };
process.on('unhandledRejection', () => { /* 无关卡片的 404 桩可能抛错 */ });

const LOCKED   = { status: 'ok', armed: false, arm_remaining_s: 0,  unlocked: false, unlock_remaining_s: 0,   signature_required: true };
const ARMED    = { status: 'ok', armed: true,  arm_remaining_s: 30, unlocked: false, unlock_remaining_s: 0,   signature_required: true };
const UNLOCKED = { status: 'ok', armed: false, arm_remaining_s: 0,  unlocked: true,  unlock_remaining_s: 599, signature_required: true };

/* 解锁端点的桩：GET 按队列出答案（'FAIL' = fetch 抛错 → API.get 变 null），POST 记下 body。 */
const script = { gets: [], posts: [] };
const mkFetch = () => async (p, init) => {
    if (p === '/api/v1/status') return reply(200, { fw_version: 'v-test', www_version: 'w', ncp_version: 'n', uptime: 10 });
    if (p === '/api/v1/ota/cloud/status') return reply(200, { state: 'idle' });
    if (p === '/api/v1/ota/zigbee/unlock') {
        if (init && init.method === 'POST') {
            script.posts.push(JSON.parse(init.body || '{}'));
            return reply(200, script.posts[script.posts.length - 1].lock ? LOCKED : { ...ARMED, status: 'armed' });
        }
        const next = script.gets.length ? script.gets.shift() : LOCKED;
        if (next === 'FAIL') throw new TypeError('network down');
        return reply(200, next);
    }
    return reply(404, { error: 'not_found' });
};
/* 当前挂着的解锁轮询定时器（未清、未拨过的那一个）；拨它 = 模拟时间到了 */
const pending = (h) => { const id = h.OTAPage._unlockTimer; const t = id ? h.timeouts[id - 1] : null; return (t && !t.cleared && !t.fired) ? t : null; };
const fire = async (h) => { const t = pending(h); if (!t) return false; t.fired = true; t.fn(); await settle(); return true; };

(async () => { try {
    // ---- 1. 进页面：默认锁着，不轮询 ----
    let h = makeHarness(mkFetch());
    script.gets = [LOCKED];
    h.OTAPage.render(el(h, 'content')); await settle();
    check('徽章存在', !!h.document.getElementById('zb-unlock-state'));
    check('默认显示 locked', el(h, 'zb-unlock-state').textContent === 'Unsigned firmware: locked', JSON.stringify(el(h, 'zb-unlock-state').textContent));
    check('locked 时不轮询', pending(h) === null);
    check('locked 时显示「允许」按钮、藏「重新锁定」', el(h, 'zb-unlock-btn').style.display !== 'none' && el(h, 'zb-lock-btn').style.display === 'none');

    // ---- 2. 点「允许」：确认弹窗 → POST {} → armed → 开始 1 s 轮询 ----
    h.dialogs.confirmAnswer = false;
    await h.OTAPage._unlockNcp(); await settle();
    check('弹窗取消 → 不发 POST', script.posts.length === 0);
    h.dialogs.confirmAnswer = true;
    await h.OTAPage._unlockNcp(); await settle();
    check('弹窗确认 → POST /ota/zigbee/unlock {}', script.posts.length === 1 && !script.posts[0].lock, JSON.stringify(script.posts));
    check('armed 徽章带倒计时', el(h, 'zb-unlock-state').textContent === 'Waiting for the button: 30s', JSON.stringify(el(h, 'zb-unlock-state').textContent));
    check('armed 时手势提示提到"红色常亮"与"10 s"', /solid red/.test(el(h, 'zb-unlock-hint').textContent) && /10 s/.test(el(h, 'zb-unlock-hint').textContent));
    check('armed 后排了 1 s 轮询', pending(h) !== null && pending(h).ms === 1000, `timer=${JSON.stringify(pending(h) && pending(h).ms)}`);

    // ---- 3. ★ 轮询失败一次，链不能断 ----
    script.gets = ['FAIL'];
    check('拨轮询（这次 fetch 抛错）', await fire(h));
    check('★ 失败一次后仍排着下一次轮询（链没断）', pending(h) !== null, '没有下一次轮询 —— 徽章会冻在旧数字上');
    check('失败时徽章保持上一状态，不乱跳', el(h, 'zb-unlock-state').textContent === 'Waiting for the button: 30s');

    // ---- 4. 设备回 unlocked：立刻变红 + mm:ss + 重新锁定 ----
    script.gets = [UNLOCKED];
    await fire(h);
    check('unlocked → 红色徽章', /badge-red/.test(el(h, 'zb-unlock-state').className), el(h, 'zb-unlock-state').className);
    check('unlocked → 显示 9:59', el(h, 'zb-unlock-state').textContent === 'Unsigned firmware: allowed for 9:59', JSON.stringify(el(h, 'zb-unlock-state').textContent));
    check('unlocked → 出现「重新锁定」、藏「允许」', el(h, 'zb-lock-btn').style.display !== 'none' && el(h, 'zb-unlock-btn').style.display === 'none');
    check('unlocked 期间继续轮询', pending(h) !== null);
    script.gets = [{ ...UNLOCKED, unlock_remaining_s: 536 }];
    await fire(h);
    check('秒数补零：536 s → 8:56', el(h, 'zb-unlock-state').textContent === 'Unsigned firmware: allowed for 8:56', JSON.stringify(el(h, 'zb-unlock-state').textContent));

    // ---- 5. 点「重新锁定」→ POST {lock:true} → locked → 停止轮询 ----
    await h.OTAPage._lockNcp(); await settle();
    check('重新锁定 → POST {lock:true}', script.posts.length === 2 && script.posts[1].lock === true, JSON.stringify(script.posts));
    check('locked → 徽章回 locked、不再轮询', el(h, 'zb-unlock-state').textContent === 'Unsigned firmware: locked' && pending(h) === null);

    // ---- 6. armed 时离开页面：destroy 清定时器 ----
    h.OTAPage._renderUnlock(ARMED);
    check('再次 armed 后有轮询', pending(h) !== null);
    h.OTAPage.destroy();
    check('destroy() 清掉解锁轮询', pending(h) === null);

    // ---- 7. 开发者构建（signature_required=false）：不出按钮，也不轮询 ----
    h = makeHarness(mkFetch());
    script.gets = [{ ...LOCKED, signature_required: false }];
    h.OTAPage.render(el(h, 'content')); await settle();
    check('开发者构建：徽章说明允许、没有解锁按钮', /developer build/.test(el(h, 'zb-unlock-state').textContent) && el(h, 'zb-unlock-btn').style.display === 'none' && pending(h) === null);

    // ---- 8. T9.25 防降级的三个拒绝码：手动上传与云端安装失败都要译成人话；不认识的码原样给（用户要贴给我们）----
    for (const [code, re] of [['ncp_image_downgrade', /older than what this chip has already run/],
                              ['ncp_image_wipe', /erases the Zigbee network at every start/],
                              ['ncp_image_unreadable', /Cannot read the build information/]]) {
        check(`手动上传拒绝码 ${code} 有提示`, re.test(h.OTAPage._zigbeeError(code)), h.OTAPage._zigbeeError(code));
    }
    check('不认识的码原样给', h.OTAPage._zigbeeError('some_new_code') === 'some_new_code');
    {
        const box = el(h, 'cloud-ncp'), msg = el(h, 'cloud-ncp-msg');
        if (box && msg) {
            h.OTAPage._renderCloudNcp({ ncp_update: true, ncp_state: 'failed', ncp_error: 'ncp_image_downgrade' });
            check('云端装 NCP 被防降级拒：页面给人话，不是裸码', /older than what this chip has already run/.test(msg.textContent), msg.textContent);
            /* T9.19 ⑨：写进去但新固件没答话 —— 云端那一侧报 ncp_unconfirmed，要给人话、指向重装 */
            h.OTAPage._renderCloudNcp({ ncp_update: true, ncp_state: 'failed', ncp_error: 'ncp_unconfirmed' });
            check('NCP 写入未确认：页面给人话、指向重装', /did not answer afterwards/.test(msg.textContent) &&
                  /official Zigbee firmware again/.test(msg.textContent), msg.textContent);
        } else {
            check('页面里有云端 NCP 区块（cloud-ncp / cloud-ncp-msg）', false, 'missing');
        }
    }

    console.log(`\nResult: ${PASS} passed, ${FAIL} failed`);
    process.exit(FAIL ? 1 : 0);
} catch (e) { console.log('❌ harness 抛错：' + (e && e.stack || e)); console.log('\nResult: 0 passed, 1 failed'); process.exit(1); } })();
HARNESS

node "$RUN"
