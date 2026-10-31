#!/bin/bash
# frontend/direct/test_muse.sh —— Muse 页（直连型）的行为断言（不用板子，只要 node；docs/15 设计 10.2）
#
#   bash frontend/direct/test_muse.sh
#
# 设计 10.2 点名的三条：token 框永不回显、没设密码时配对按钮禁用、权限回读一致。另外钉住：
#   · 显示的值来自设备响应；开关显示的是设备**改完之后**报的值（固件拒了就弹回去），不是刚发出去的值（CLAUDE.md 7.5）
#   · 发出去的 JSON 符合 muse_api.c 的契约（只发改的那一项；解绑带 confirm:"UNPAIR"）
#   · 启用前必须勾"我已了解"（出站数据清单，设计第十三节）；开敏感权限要二次确认
#   · 设备给的字符串（审计、原因）进 innerHTML 前转义
#   · 配对：202 之后等 /api/v1/status 报 mode:"muse_pair" 再整页刷新到配对剖面的进度页
#   · 外壳：去掉 WiFi 菜单、在仪表盘后面加 Muse
#
# DOM 桩沿用 ../test_led_mqtt.sh 的思路（不解析 HTML、不执行内联 onclick、不套 CSS），所以内联 onclick 的按钮
# 这里直接调对应的方法。

cd "$(dirname "$0")"
command -v node >/dev/null || { echo "需要 node"; exit 1; }

RUN=$(mktemp /tmp/musepage.XXXX.cjs)
trap 'rm -f "$RUN"' EXIT

cat > "$RUN" << 'HARNESS'
const fs = require('fs');
const FILES = ['../js/i18n.js', '../js/i18n/de.js', '../js/i18n/zh.js', 'i18n/zh.js',
               '../js/api.js', '../js/ws.js', '../js/app.js', 'js/shell.js', 'js/muse.js', 'js/devices.js'];
const missing = FILES.filter(f => !fs.existsSync(f));
if (missing.length) { console.log(`❌ 源文件缺失：${missing.join(', ')}`); console.log('\nResult: 0 passed, 1 failed'); process.exit(1); }
const SRC = FILES.map(f => fs.readFileSync(f, 'utf8')).join('\n');

function makeHarness(fetchImpl, opts = {}) {
    const els = new Map(), owner = new Map();
    const mkEl = (id, ownerId) => {
        const el = {
            id, value: '', type: '', textContent: '', disabled: false, checked: false, placeholder: '',
            style: {}, dataset: {}, _html: '', _listeners: {},
            get innerHTML() { return this._html; },
            set innerHTML(v) { this._html = v; dropOwned(this.id); scan(v, this.id); },
            addEventListener(t, f) { (this._listeners[t] ||= []).push(f); },
            focus() {}, setAttribute() {},
            classList: { toggle() {}, add() {}, remove() {} },
        };
        els.set(id, el);
        if (ownerId !== undefined) owner.set(id, ownerId);
        return el;
    };
    const dropOwned = (o) => { for (const [id, ow] of [...owner]) if (ow === o) { dropOwned(id); owner.delete(id); els.delete(id); } };
    const scan = (html, o) => {
        for (const m of String(html).matchAll(/id="([^"]+)"/g)) {
            if (els.has(m[1])) continue;
            const e = mkEl(m[1], o);
            // checkbox 的初始 checked / disabled 属性（报警那一行写死了 checked disabled）
            const tag = String(html).slice(String(html).lastIndexOf('<', m.index), String(html).indexOf('>', m.index));
            if (/\schecked[\s>]/.test(tag + '>')) e.checked = true;
            if (/\sdisabled[\s>]/.test(tag + '>')) e.disabled = true;
            if (/type="checkbox"/.test(tag)) e.type = 'checkbox';
        }
    };
    mkEl('app'); mkEl('content');

    // 外壳测试用：#app 里的 nav 项按 data-page 认
    const nav = new Map(), order = [];
    const navEl = (page) => {
        if (nav.has(page)) return nav.get(page);
        if (!new RegExp(`data-page="${page}"`).test(els.get('app')._html)) return null;
        const n = { page, removed: false, remove() { this.removed = true; }, after(x) { order.push([page, x]); } };
        nav.set(page, n);
        return n;
    };
    const document = {
        activeElement: null,
        getElementById: id => els.get(id) || null,
        querySelector: sel => { const m = /data-page="(\w+)"/.exec(sel); return m ? navEl(m[1]) : null; },
        querySelectorAll: () => [],
        createElement: () => ({ dataset: {}, _listeners: {}, addEventListener(t, f) { (this._listeners[t] ||= []).push(f); },
                                after(x) { order.push([this.dataset.page, x]); } }),
    };
    const reloads = [];
    const location = { _h: '', host: '127.0.0.1', reload() { reloads.push(1); },
        get hash() { return this._h; }, set hash(v) { this._h = v ? (String(v).startsWith('#') ? String(v) : '#' + v) : ''; } };
    const window = {};
    const WebSocket = function () { this.readyState = 0; this.close = () => {}; };
    const timers = [];
    const setInterval  = (fn, ms) => { timers.push({ fn, ms, cleared: false }); return timers.length - 1; };
    const clearInterval = (h) => { if (timers[h]) timers[h].cleared = true; };
    const timeouts = [];
    const fakeTimeout = (fn, ms) => { timeouts.push({ fn, ms }); return timeouts.length; };
    const confirms = [];
    const confirm = (msg) => { confirms.push(msg); return opts.confirm !== undefined ? opts.confirm : true; };

    const factory = new Function(
        'window', 'document', 'location', 'fetch', 'WebSocket',
        'setTimeout', 'clearTimeout', 'setInterval', 'clearInterval', 'console', 'TextEncoder', 'confirm',
        SRC + '\n; return { App, API, MusePage, I18N, Lang, t };');
    const ctx = factory(window, document, location, fetchImpl, WebSocket,
                        fakeTimeout, () => {}, setInterval, clearInterval, console, TextEncoder, confirm);
    return Object.assign(ctx, { document, els, timers, timeouts, confirms, reloads, nav, order });
}

const reply = (status, body) => ({ ok: status >= 200 && status < 300, status, json: async () => body });
let PASS = 0, FAIL = 0;
const check = (name, cond, detail) => {
    if (cond) { console.log(`✅ ${name}`); PASS++; }
    else { console.log(`❌ ${name}${detail ? ' — ' + detail : ''}`); FAIL++; }
};
const el = (h, id) => h.document.getElementById(id) || { value: '', textContent: '(元素不存在)', checked: false, disabled: false, style: {}, innerHTML: '(元素不存在)' };
const settle = async () => { for (let i = 0; i < 6; i++) await new Promise(r => setImmediate(r)); };
const dom = (h) => [...h.els.values()].map(e => [e._html, e.textContent, e.value, e.placeholder].join('\n')).join('\n');

const TOKEN = 'mgst_' + 'AbCdEfGhIjKlMnOpQrStUvWxYz0123456789-_abcd' + 'E';   // 5 + 43 = 48

// ---- 固件桩：字段名与 muse_api.c / hub_api.c 的回包逐字对齐 ----
const freshState = () => ({
    calls: [],
    cfg: { enabled: true, display_name: 'Kitchen Hub', sdk_token: 'set', paired: true, password_set: true,
           perm: { control: true, manage: false, sensitive: false }, perm_unreadable: false,
           notify: { alarm: true, security: true, availability: true, lifecycle: false, gateway: true, battery: true } },
    cfgStatus: 200,
    st: { hubdb_ready: true, state: 'idle', pair_boot: false, window_s: 180, remaining_s: 0, ble_name: '',
          last_result: 'ok', last_result_epoch: 0, paired: true, sdk_token: 'set', enabled: true,
          blew: { state: 'idle', selfcheck: { state: 'pass', consecutive_degraded: 0 } },
          link: { state: 'online', since_s: 5, last_error: '', node_id: 'n', display_name: 'Kitchen Hub', vm_name: 'vm-1',
                  registrations: 1, heartbeat: true, invokes: { total: 0, denied: 0, failed: 0 }, stack_free: 1000,
                  pushes: { sent: 4, dropped: 1, queued: 2 } },
          thread_inventory: {}, notify_mask: 61 },
    audit: { entries: [
        { seq: 9, age_s: 3, source: 'muse', command: 'hub.devices.list', result: 'ok', ms: 12, repeat: 3 },
        { seq: 8, age_s: 70, time: '21:15', source: 'rest', command: 'hub.<b>x', device: 'th-<i>', action: 'a"b', result: 'forbidden', ms: 1 } ],
        count: 2 },
    // POST /muse/config 的行为：默认照单全收；可以换成拒绝
    onConfig: null,
    pairStatus: [],          // /api/v1/status 依次回的内容（配对重启那段）
});

const mkFetch = (S) => async (p, o) => {
    const method = (o && o.method) || 'GET';
    const body = o && o.body ? JSON.parse(o.body) : null;
    S.calls.push({ p, method, body });
    if (p === '/api/v1/muse/config' && method === 'GET')  return reply(S.cfgStatus, S.cfgStatus === 200 ? JSON.parse(JSON.stringify(S.cfg)) : { error: 'hubdb_not_ready' });
    if (p === '/api/v1/muse/config' && method === 'POST') {
        if (S.onConfig) { const r = S.onConfig(body, S); if (r) return reply(r[0], r[1]); }
        if ('enabled' in body) S.cfg.enabled = body.enabled;
        if ('display_name' in body) S.cfg.display_name = body.display_name;
        if ('sdk_token' in body) S.cfg.sdk_token = body.sdk_token ? 'set' : 'unset';
        if (body.perm) Object.assign(S.cfg.perm, body.perm);
        if (body.notify) Object.assign(S.cfg.notify, body.notify);
        return reply(200, { status: 'ok' });
    }
    if (p === '/api/v1/muse/status') return reply(200, JSON.parse(JSON.stringify(S.st)));
    if (p.startsWith('/api/v1/muse/audit')) return reply(200, S.audit);
    if (p === '/api/v1/muse/pair') return reply(202, { status: 'accepted', window_s: 300, policy: 'isolated' });
    if (p === '/api/v1/muse/unpair') return reply(200, { status: 'ok', hint: 'remove the device in the Muse app too' });
    if (p === '/api/v1/muse/test_push') return reply(202, { status: 'queued' });
    if (p === '/api/v1/system/reboot') return reply(200, { status: 'rebooting' });
    if (p === '/api/v1/status') {
        const n = S.pairStatus.shift();
        if (n === undefined) return Promise.reject(new Error('down'));
        return n.__status ? reply(n.__status, { error: 'unauthorized' }) : reply(200, n);
    }
    return reply(404, { error: 'not_found' });
};

const open = async (S, opts) => {
    const h = makeHarness(mkFetch(S), opts);
    h.MusePage.render(el(h, 'content'));
    await settle();
    return h;
};

(async () => {

// ======================= 外壳 =======================
{
    const S = freshState(), h = makeHarness(mkFetch(S));
    check('muse 页已注册', !!h.App.pages['muse']);
    check('直连型的打包里没有 WiFi 页', !h.App.pages['wifi']);
    h.App._renderShell();
    check('外壳：WiFi 菜单项被去掉', h.nav.get('wifi') && h.nav.get('wifi').removed);
    const ins = h.order.find(([page]) => page === 'dashboard');
    check('外壳：Muse 菜单项插在仪表盘后面', ins && ins[1].dataset.page === 'muse' && ins[1].href === '#muse');
    const insd = h.order.find(([page]) => page === 'muse');
    check('外壳：设备菜单项插在 Muse 后面', insd && insd[1].dataset.page === 'devices' && insd[1].href === '#devices');
    S.calls.length = 0;
    await h.API.reboot();
    check('外壳：重启走 /api/v1/system/reboot（直连型不编 /api2/reboot，设置页 / 固件页的重启按钮不能假装成功）',
          S.calls.length === 1 && S.calls[0].p === '/api/v1/system/reboot' && S.calls[0].method === 'POST');
}

// ======================= 显示的值来自设备 =======================
{
    const S = freshState(), h = await open(S);
    check('显示名来自设备', el(h, 'mu-name').value === 'Kitchen Hub', el(h, 'mu-name').value);
    check('启用开关来自设备', el(h, 'mu-en').checked === true);
    check('权限位逐个回填', el(h, 'mu-perm-control').checked === true && el(h, 'mu-perm-manage').checked === false && el(h, 'mu-perm-sensitive').checked === false);
    check('推送类别逐个回填', el(h, 'mu-ntf-security').checked === true && el(h, 'mu-ntf-lifecycle').checked === false && el(h, 'mu-ntf-gateway').checked === true);
    check('报警开关恒开且不可点', el(h, 'mu-ntf-alarm').checked === true && el(h, 'mu-ntf-alarm').disabled === true);
    check('连接状态：Online', /Online/.test(el(h, 'mu-link').innerHTML), el(h, 'mu-link').innerHTML);
    check('推送计数来自 link.pushes', el(h, 'mu-pushes').textContent === '4' && /2 waiting, 1 dropped/.test(el(h, 'mu-pushes-sub').textContent));
    check('上次配对结果按表翻成人话', /Last attempt: Paired/.test(el(h, 'mu-last').textContent), el(h, 'mu-last').textContent);
    check('token 框是空的（固件只报 set）', el(h, 'mu-token').value === '');
    check('已存 token 时有"输入新的可替换"的提示', /replace/.test(el(h, 'mu-token-hint').textContent));
    check('在线时有"发测试通知"、已配对时有"解绑"', el(h, 'mu-test').style.display === '' && el(h, 'mu-unpair').style.display === '');
    check('审计表：合并过的显示次数（×N）', /&times;3/.test(el(h, 'mu-audit').innerHTML), el(h, 'mu-audit').innerHTML.slice(0, 300));
    check('审计表：设备给的字符串转义后才进 innerHTML', /hub\.&lt;b&gt;x/.test(el(h, 'mu-audit').innerHTML) && !/<b>x/.test(el(h, 'mu-audit').innerHTML)
          && /a&quot;b/.test(el(h, 'mu-audit').innerHTML) && /th-&lt;i&gt;/.test(el(h, 'mu-audit').innerHTML));
    check('审计表：有钟点用钟点，没有用"N s ago"', /21:15/.test(el(h, 'mu-audit').innerHTML) && /3 s ago/.test(el(h, 'mu-audit').innerHTML));
    check('页面挂了 3 s 的状态轮询', h.timers.some(x => x.ms === 3000 && !x.cleared));
    h.MusePage.destroy();
    check('离开页面清掉轮询', h.timers.every(x => x.cleared));
}

// ======================= token：永不回显 =======================
{
    const S = freshState(); S.cfg.sdk_token = 'unset'; S.st.sdk_token = 'unset';
    const h = await open(S);
    el(h, 'mu-token').value = TOKEN;
    S.calls.length = 0;
    await h.MusePage._saveToken();
    await settle();
    const post = S.calls.find(c => c.p === '/api/v1/muse/config' && c.method === 'POST');
    check('保存 token：请求体只有 sdk_token', post && JSON.stringify(Object.keys(post.body)) === '["sdk_token"]' && post.body.sdk_token === TOKEN);
    check('保存之后 token 框立刻清空', el(h, 'mu-token').value === '');
    check('整个 DOM 里找不到 token 本身', !dom(h).includes(TOKEN.slice(5)));
    check('保存后按设备读回显示"已存"', /replace/.test(el(h, 'mu-token-hint').textContent) && el(h, 'mu-token').placeholder !== 'mgst_...');

    el(h, 'mu-token').value = 'mgst_short';
    S.calls.length = 0;
    await h.MusePage._saveToken();
    check('格式不对的 token：不发请求、给出格式说明', !S.calls.some(c => c.method === 'POST') && /mgst_/.test(el(h, 'mu-token-msg').innerHTML));
    check('格式不对也清空输入框', el(h, 'mu-token').value === '');

    S.onConfig = (b) => b.sdk_token ? [409, { error: 'password_required' }] : null;
    el(h, 'mu-token').value = TOKEN;
    await h.MusePage._saveToken();
    await settle();
    check('固件拒了（409）：显示原因、输入框照样清空', /admin password/.test(el(h, 'mu-token-msg').innerHTML) && el(h, 'mu-token').value === '');
    check('被拒之后 DOM 里仍找不到 token', !dom(h).includes(TOKEN.slice(5)));

    S.onConfig = null;
    S.calls.length = 0;
    await h.MusePage._clearToken();
    const del = S.calls.find(c => c.method === 'POST');
    check('删除 token 发 sdk_token:null（先确认）', del && del.body.sdk_token === null && h.confirms.length >= 1);
}

// ======================= 没设密码：配对按钮禁用 =======================
{
    /* 只缺密码（token 在、已启用）：按钮禁用必须是密码这一条造成的，别和"没启用"混在一起 */
    const S0 = freshState(); S0.cfg.password_set = false;
    const h0 = await open(S0);
    check('只缺密码：配对按钮禁用', el(h0, 'mu-pair').disabled === true);
    check('只缺密码：说明只有密码那一条', /admin password/.test(el(h0, 'mu-pair-why').innerHTML) && !/Turn Muse on|SDK token/.test(el(h0, 'mu-pair-why').innerHTML));
    check('已启用时没密码：启用开关仍可点（可以关掉）', el(h0, 'mu-en').disabled === false);
    const S = freshState(); S.cfg.password_set = false; S.cfg.enabled = false; S.cfg.paired = false; S.st.paired = false;
    S.st.link.state = 'disabled';
    const h = await open(S);
    check('没设密码：配对按钮禁用', el(h, 'mu-pair').disabled === true);
    check('没设密码：说明写出来（不是悄悄灰掉）', /admin password/.test(el(h, 'mu-pair-why').innerHTML));
    check('没设密码：保存 token 禁用', el(h, 'mu-token-save').disabled === true);
    check('没设密码且没启用：启用开关禁用、带说明', el(h, 'mu-en').disabled === true && /admin password/.test(el(h, 'mu-en-why').textContent));
    check('没设密码：敏感权限开关禁用', el(h, 'mu-perm-sensitive').disabled === true);
    check('没配对：不显示解绑；不在线：不显示测试通知', el(h, 'mu-unpair').style.display === 'none' && el(h, 'mu-test').style.display === 'none');
}
{
    const S = freshState(); S.cfg.sdk_token = 'unset'; S.st.sdk_token = 'unset';
    const h = await open(S);
    check('有密码没 token：配对禁用、说先存 token', el(h, 'mu-pair').disabled === true && /SDK token/.test(el(h, 'mu-pair-why').innerHTML));
}
{
    const S = freshState(); S.cfg.enabled = false;
    const h = await open(S);
    check('没启用：配对禁用、说先启用', el(h, 'mu-pair').disabled === true && /Turn Muse on/.test(el(h, 'mu-pair-why').innerHTML));
}
{
    const S = freshState();
    const h = await open(S);
    check('密码、token、启用都齐：配对可点', el(h, 'mu-pair').disabled === false && el(h, 'mu-pair-why').innerHTML === '');
}
{
    const S = freshState(); S.cfgStatus = 503;
    const h = await open(S);
    check('设置读不出（503）：横幅说明、按钮全禁用、安排重试', /still starting/.test(el(h, 'mu-banner').innerHTML)
          && el(h, 'mu-pair').disabled && el(h, 'mu-en').disabled && el(h, 'mu-perm-control').disabled
          && h.timeouts.some(x => x.ms === 3000));
}
{
    const S = freshState(); S.cfg.perm_unreadable = true; S.cfg.perm = { control: false, manage: false, sensitive: false };
    S.audit = { error: 'audit_unavailable' };
    S.st.link.vm_name = '<img src=x onerror=1>';
    const mk = mkFetch(S);
    const h = makeHarness(async (p, o) => p.startsWith('/api/v1/muse/audit') ? reply(503, { error: 'audit_unavailable' }) : mk(p, o));
    h.MusePage.render(el(h, 'content'));
    await settle();
    check('权限位读不出（perm_unreadable）：提示按只读处理、请重新设置', /read-only/.test(el(h, 'mu-perm-bad').innerHTML));
    check('审计环没起：翻成人话（不是 audit_unavailable 原码）', /not running/.test(el(h, 'mu-audit').innerHTML), el(h, 'mu-audit').innerHTML);
    check('vm_name 转义后显示', /&lt;img/.test(el(h, 'mu-link-why').innerHTML) && !/<img/.test(el(h, 'mu-link-why').innerHTML), el(h, 'mu-link-why').innerHTML);
}

// ======================= 权限 / 推送：回读一致 =======================
{
    const S = freshState(), h = await open(S);
    S.calls.length = 0;
    el(h, 'mu-perm-manage').checked = true;
    await h.MusePage._setPerm('manage', true);
    await settle();
    const p = S.calls.find(c => c.method === 'POST');
    check('改权限只发那一项', p && JSON.stringify(p.body) === '{"perm":{"manage":true}}', p && JSON.stringify(p.body));
    check('改完重新读 /muse/config', S.calls.filter(c => c.p === '/api/v1/muse/config' && c.method === 'GET').length >= 1);
    check('开关显示设备读回的值', el(h, 'mu-perm-manage').checked === true);

    S.onConfig = (b) => b.perm ? [503, { error: 'hubdb_busy' }] : null;
    el(h, 'mu-perm-control').checked = false;
    await h.MusePage._setPerm('control', false);
    await settle();
    check('固件没存上：开关弹回设备的值（不是刚点的）', el(h, 'mu-perm-control').checked === true);
    check('并显示原因', /busy/i.test(el(h, 'mu-perm-msg').innerHTML), el(h, 'mu-perm-msg').innerHTML);

    S.onConfig = null;
    el(h, 'mu-ntf-battery').checked = false;
    S.calls.length = 0;
    await h.MusePage._setNotify('battery', false);
    await settle();
    const n = S.calls.find(c => c.method === 'POST');
    check('推送类别只发那一项', n && JSON.stringify(n.body) === '{"notify":{"battery":false}}');
    check('推送开关显示读回的值', el(h, 'mu-ntf-battery').checked === false);
}
{
    const S = freshState(), h = await open(S, { confirm: false });
    S.calls.length = 0;
    el(h, 'mu-perm-sensitive').checked = true;
    await h.MusePage._setPerm('sensitive', true);
    check('开敏感权限要二次确认；取消就不发、开关弹回', h.confirms.some(m => /unlock doors/.test(m)) && !S.calls.some(c => c.method === 'POST')
          && el(h, 'mu-perm-sensitive').checked === false);
}

// ======================= 启用：先勾"我已了解" =======================
{
    const S = freshState(); S.cfg.enabled = false;
    const h = await open(S);
    S.calls.length = 0;
    el(h, 'mu-en').checked = true;
    await h.MusePage._setEnabled(true);
    check('没勾"我已了解"：不发、开关弹回、给说明', !S.calls.some(c => c.method === 'POST') && el(h, 'mu-en').checked === false
          && /I understand/.test(el(h, 'mu-en-msg').innerHTML));
    el(h, 'mu-ack').checked = true;
    el(h, 'mu-en').checked = true;
    await h.MusePage._setEnabled(true);
    await settle();
    const p = S.calls.find(c => c.method === 'POST');
    check('勾了之后发 {enabled:true}', p && JSON.stringify(p.body) === '{"enabled":true}');
    const page = el(h, 'content').innerHTML;
    check('出站数据清单在页面上', /id="mu-outbound"/.test(page) && /Sent to Meta/.test(page) && /Never sent/.test(page));
    /* 设计第十三节逐行（加 5.3：MAC 在配对窗口里也能被附近的人经蓝牙读到）—— 少一项就是少告知用户 */
    const SENT = ['Display name, firmware version, model', 'MAC address', 'read it over Bluetooth', 'your SDK token (to Muse only)',
                  'Device names, rooms, types, capabilities, state, online status, battery, link quality',
                  'Thread network name, channel, PAN ID, role, member count, OMR prefix',
                  'Device hardware addresses', 'RLOC16', 'SRP host names', 'remaining leases', 'names advertised by devices waiting to be added',
                  'Matter fabric summary and node IDs', 'uptime, uplink, free memory', 'whether the Zigbee radio answers and its network is up',
                  'The text of notifications'];
    const NOT_SENT = ['Zigbee and Thread network keys, PSKc', 'Your admin password or its hash', 'LAN IP addresses', 'Raw Zigbee or Matter messages'];
    const miss = [...SENT, ...NOT_SENT].filter(x => !page.includes(x));
    check('出站清单与第十三节逐项一致（会送 14 项、不会送 4 项）', miss.length === 0, miss.join(' | '));
}

// ======================= 状态：原因、退化提示 =======================
{
    const S = freshState(); S.st.link.state = 'unpaired'; S.st.link.last_error = 'token_revoked';
    S.st.blew.selfcheck.consecutive_degraded = 2;
    const h = await open(S);
    check('last_error 的原因码翻成人话', /revoked/.test(el(h, 'mu-link-why').innerHTML), el(h, 'mu-link-why').innerHTML);
    check('BLE 后 Thread 连续两次退化：提示断电重上电（不是按复位）', /Unplug/.test(el(h, 'mu-banner-deg').innerHTML) && /reset button is not enough/.test(el(h, 'mu-banner-deg').innerHTML));
    await h.MusePage._setNotify('battery', false);
    await settle();
    check('保存设置（重读配置）不会把退化提示冲掉', /Unplug/.test(el(h, 'mu-banner-deg').innerHTML));
    S.st.link.last_error = 'vm: <img src=x>';
    await h.MusePage._poll();
    check('不认识的原因串转义后显示', /&lt;img/.test(el(h, 'mu-link-why').innerHTML) && !/<img/.test(el(h, 'mu-link-why').innerHTML));
}

// ======================= 解绑、测试通知 =======================
{
    const S = freshState(), h = await open(S);
    S.calls.length = 0;
    await h.MusePage._unpair();
    const u = S.calls.find(c => c.p === '/api/v1/muse/unpair');
    check('解绑：先确认、带 confirm:"UNPAIR"', h.confirms.some(m => /Unpair/.test(m)) && u && u.body.confirm === 'UNPAIR');
    check('解绑后提示去 App 里也删', /Muse app/.test(el(h, 'mu-pair-msg').innerHTML));
    await h.MusePage._testPush();
    check('测试通知：202 queued 显示已排队', /Queued/.test(el(h, 'mu-pair-msg').innerHTML));
}

// ======================= 配对：等进配对剖面再刷新 =======================
{
    const S = freshState(), h = await open(S);
    S.calls.length = 0;
    await h.MusePage._pair();
    const c = h.confirms.find(m => /Pair with Muse\?/.test(m)) || '';
    check('确认框写了四件事：窗口时长（来自 status 的 window_s=180）、要重启（Zigbee/Thread 离线）、先开 Developer mode、按键 2–10 秒',
          /about 3 minutes/.test(c) && /restarts into pairing mode/.test(c) && /Developer mode/.test(c) && /2 to 10 seconds/.test(c), c);
    check('确认框说清配对期间按短了不起作用、按长了恢复出厂（2026-10-22 起配对剖面里短按不重启）', /shorter than 2 seconds does nothing while pairing/.test(c) && /factory settings/.test(c) && !/restarts the hub/.test(c));
    check('确认框写了残余风险那句（设计第十三节"配对"行）', /Only press the button when your own Muse app is asking for it/.test(c));
    check('发了 POST /muse/pair', S.calls.some(c => c.p === '/api/v1/muse/pair' && c.method === 'POST'));
    const w = h.timers.find(x => x.ms === 2000 && !x.cleared);
    check('202 之后开始每 2 s 探 /api/v1/status、停掉 3 s 的状态轮询', !!w && h.timers.filter(x => x.ms === 3000).every(x => x.cleared));
    S.pairStatus.push({ profile: 'direct', uptime: 900 });      // 还没重启（旧的正常剖面）
    await w.fn(); await settle();
    check('还是正常剖面且没断过：不刷新', h.reloads.length === 0);
    await w.fn(); await settle();                                // 重启中：连不上
    S.pairStatus.push({ profile: 'direct', mode: 'muse_pair', uptime: 3 });
    await w.fn(); await settle();
    check('看到 mode:"muse_pair" 就整页刷新（配对剖面的进度页接手）', h.reloads.length === 1);
}
{
    const S = freshState(), h = await open(S);
    await h.MusePage._pair();
    const w = h.timers.find(x => x.ms === 2000 && !x.cleared);
    await w.fn(); await settle();                                // 断了
    S.pairStatus.push({ profile: 'direct', uptime: 4 });         // 直接回到正常剖面（例如没网，配对剖面马上收尾重启）
    await w.fn(); await settle();
    check('断过之后回到正常剖面：也刷新（显示配对结果）', h.reloads.length === 1);
}
{
    const S = freshState(), h = await open(S, { confirm: false });
    S.calls.length = 0;
    await h.MusePage._pair();
    check('确认框点取消：不发配对请求', !S.calls.some(c => c.p === '/api/v1/muse/pair'));
}
{
    const S = freshState(), h = await open(S);
    await h.MusePage._pair();
    const w = h.timers.find(x => x.ms === 2000 && !x.cleared);
    S.pairStatus.push({ __status: 401 });                       // 重启前：正常剖面开着 REST 认证
    await w.fn(); await settle();
    check('还没断过的 401：不刷新', h.reloads.length === 0);
    await w.fn(); await settle();                                // 断了
    S.pairStatus.push({ __status: 401 });                       // 回来了，正常剖面要登录
    await w.fn(); await settle();
    check('断过之后的 401（开着 REST 认证的正常剖面）：也刷新', h.reloads.length === 1);
}
{
    const S = freshState(), h = await open(S);
    const first = h.MusePage._pair();
    const second = h.MusePage._pair();                           // 连点两次
    await first; await second; await settle();
    check('连点两次配对：只发一次请求、只起一个重启探测', S.calls.filter(c => c.p === '/api/v1/muse/pair').length === 1
          && h.timers.filter(x => x.ms === 2000).length === 1);
    h.MusePage.destroy();
    check('离开页面：重启探测也清掉', h.timers.every(x => x.cleared));
}
{
    const S = freshState(), h = makeHarness(mkFetch(S));
    let c = '';
    h.MusePage._st = null;
    const realConfirm = h.confirms;
    h.MusePage.render(el(h, 'content'));                         // 不等 status 回来就点
    h.MusePage._st = null;
    await h.MusePage._pair();
    c = realConfirm.find(m => /Pair with Muse\?/.test(m)) || '';
    check('status 还没回来：窗口时长按 Kconfig 默认 180 s 写（about 3 minutes）', /about 3 minutes/.test(c), c);
}
{
    const S = freshState(); S.st.pair_boot = true;
    const h = await open(S);
    check('轮询看到 pair_boot（别的标签页发起了配对）：整页刷新到进度页', h.reloads.length >= 1);
}

console.log(`\nResult: ${PASS} passed, ${FAIL} failed`);
process.exit(FAIL === 0 ? 0 : 1);
})();
HARNESS

node "$RUN"
