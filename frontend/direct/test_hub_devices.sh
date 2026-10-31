#!/bin/bash
# frontend/direct/test_hub_devices.sh —— 设备页（直连型）的行为断言（不用板子，只要 node；docs/15 设计 10.2"新的前端测试"）
#
#   bash frontend/direct/test_hub_devices.sh
#
# 设计 10.2 点名的：显示的值确实来自设备、控制请求符合契约、敏感动作没开时按钮不可用（后一条在直连型里表现为：
# 敏感标记只在本地设、取消要先确认 —— Muse 能不能控制敏感设备由 Muse 权限位决定，网页本地操作不受它约束）。另外钉住：
#   · 控制之后显示的是网关回包里的状态，不是刚发出去的值；改名 / 推送开关之后重新读列表（CLAUDE.md 7.5）
#   · 请求的 JSON 符合 hub_api.c / hub_cmds.c 的契约：只发改了的字段、亮度是字符串、permit_join 的 seconds 是数字
#   · 设备给的字符串（名字、房间、型号）进 innerHTML 前转义；onclick 里只放 [a-z0-9] 的 key
#   · 没设管理密码时写操作回 409 password_required：如实说，不是"Failed"
#   · 分页（truncated / next_cursor）、协议 / 房间筛选、推送开关只给门窗 / 人体类
# DOM 桩与 fetch 桩照抄 test_muse.sh。

cd "$(dirname "$0")"
command -v node >/dev/null || { echo "需要 node"; exit 1; }

RUN=$(mktemp /tmp/devpage.XXXX.cjs)
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
        SRC + '\n; return { App, API, MusePage, DevicesPage, I18N, Lang, t };');
    const ctx = factory(window, document, location, fetchImpl, WebSocket,
                        fakeTimeout, () => {}, setInterval, clearInterval, console, TextEncoder, confirm);
    return Object.assign(ctx, { document, els, timers, timeouts, confirms, reloads, nav, order });
}

const reply = (status, body) => ({ ok: status >= 200 && status < 300, status, json: async () => body });
let PASS = 0, FAIL = 0, DONE = false;
// 某个 await 永远不回（被测代码挡不住、跟着桩里的闸门等）时 node 会悄悄退出、退出码 0：当失败算
process.on('exit', () => { if (!DONE) { console.log(`❌ 测试中途挂住（有 await 没回来），停在第 ${PASS + FAIL + 1} 条附近`); process.exitCode = 1; } });
const check = (name, cond, detail) => {
    if (cond) { console.log(`✅ ${name}`); PASS++; }
    else { console.log(`❌ ${name}${detail ? ' — ' + detail : ''}`); FAIL++; }
};
const el = (h, id) => h.document.getElementById(id) || { value: '', textContent: '(元素不存在)', checked: false, disabled: false, style: {}, innerHTML: '(元素不存在)' };
const settle = async () => { for (let i = 0; i < 6; i++) await new Promise(r => setImmediate(r)); };
const dom = (h) => [...h.els.values()].map(e => [e._html, e.textContent, e.value, e.placeholder].join('\n')).join('\n');


// ---- 固件桩：字段名与 hub_api.c / hub_cmds.c 的回包逐字对齐 ----
const LIGHT = { id: 'zb-b0ce181403586d1a', protocol: 'zigbee', name: 'Living room light', room: 'Living room', kind: 'dimmable_light',
                model: 'E11-G13', capabilities: ['onoff', 'level', 'energy'], controllable: true, online: true, last_seen_s: 12,
                state: { onoff: true, level_pct: 100 }, link: { lqi: 120, rssi: -70 }, notify: false, sensitive: false,
                zigbee: { nwk: '0x58E8', node_type: 'end_device', sleepy: false, identified: true, interview: 'done' } };
const MOTION = { id: 'zb-b40e060fffe26179', protocol: 'zigbee', name: '', room: '', kind: 'motion_sensor', model: '3RMS16BZ',
                 capabilities: ['motion', 'battery'], controllable: false, online: true, last_seen_s: 4000,
                 state: { motion: false, battery_pct: 45 }, notify: true, sensitive: false };
const EVIL = { id: 'zb-00158d008c8274cf', protocol: 'zigbee', name: '<img src=x onerror=alert(1)>', room: 'K"itchen<b>', kind: 'climate_sensor',
               model: '<script>x</script>', capabilities: ['temperature', 'humidity'], controllable: false, online: false, last_seen_s: null,
               state: { temperature_c: 22.19 }, notify: false, sensitive: false };
const THREAD = { id: 'th-920e02140e0077c1', protocol: 'thread', role: 'sleepy_end_device', online: true, last_seen_s: 3, controllable: false };
// M7 第四步：本网关 fabric 上的 Matter 设备（hub_matter.c write_entry；在线 / 读数随订阅，第五步）
const MATTER = { id: 'mt-0000000000000006', protocol: 'matter', name: 'Eve plug', room: '', kind: 'plug', manufacturer: 'Eve Systems',
                 model: 'Eve Energy 20EBU4101', capabilities: ['onoff', 'power', 'energy', 'identify'], controllable: false,
                 thread: { rssi_avg: -57, link_margin: 63 }, notify: false, sensitive: false,
                 matter: { node_id: '0000000000000006', vendor_id: 4874, product_id: 83, interview: 'done' } };
const freshState = () => ({
    calls: [],
    devs: [LIGHT, MOTION, EVIL, THREAD].map(d => JSON.parse(JSON.stringify(d))),
    pageSize: 0,                  // > 0：按这个条数分页（truncated / next_cursor）
    password: true,
    ctlState: null,               // 控制命令回包里的 state（默认按命令算；设成别的模拟"设备没照做"）
    notifyReject: false,
    win: { state: 'closed', left_s: 0 },
    pjOpening: false,             // permit_join 1 s 内没落定：回 {open:false, opening:true}（hub_cmds.c）
});
const mkFetch = (S) => async (p, o) => {
    const method = (o && o.method) || 'GET';
    const body = o && o.body ? JSON.parse(o.body) : null;
    S.calls.push({ p, method, body });
    if (p.startsWith('/api/v1/hub/devices')) {
        const m = /cursor=(\d+)/.exec(p), start = m ? +m[1] : 0;
        const n = S.pageSize || S.devs.length;
        const page = S.devs.slice(start, start + n);
        const res = { devices: JSON.parse(JSON.stringify(page)) };
        if (start + n < S.devs.length) { res.truncated = true; res.next_cursor = String(start + n); }
        return reply(200, { ok: true, result: res });
    }
    if (p === '/api/v1/hub/invoke' && method === 'POST') {
        const { command, params } = body;
        const nopw = () => reply(409, { error: 'password_required', message: 'set an admin password before changing devices from the local API' });
        if (command === 'hub.device.control') {
            if (!S.password) return nopw();
            if (S.ctlGate) await S.ctlGate;                    // 设备迟迟不回：网关同步等着（hub_cmds.c，最长约 10 s）
            if (S.ctlFail) return reply(504, { error: 'timeout', message: 'The command did not reach the device.' });
            const d = S.devs.find(x => x.id === params.id);
            const st = S.ctlState || Object.assign({}, d.state,
                params.action === 'turn_on' ? { onoff: true } : params.action === 'turn_off' ? { onoff: false } : { onoff: true, level_pct: +params.value });
            d.state = st;
            return reply(200, { ok: true, result: { result: 'confirmed', state: st, detail: 'The device confirmed the command (35 ms).' } });
        }
        if (command === 'hub.device.update') {
            if (!S.password) return nopw();
            const d = S.devs.find(x => x.id === params.id);
            if ('name' in params) d.name = params.name;
            if ('room' in params) d.room = params.room;
            if ('notify' in params && !S.notifyReject) d.notify = params.notify;
            return reply(200, { ok: true, result: { id: d.id, name: d.name, room: d.room, notify: d.notify, notify_active: d.notify } });
        }
        if (command === 'hub.matter.commission') {
            if (!S.password) return nopw();
            if (params.setup_code === 'bad<b>') return reply(400, { error: 'invalid_params', message: 'not a valid Matter pairing code <b>' });
            if (params.setup_code === 'ble') return reply(404, { error: 'unsupported', message: 'This code is for a new device that has to be added over Bluetooth first.' });
            return reply(202, { ok: true, result: { job: '7', state: 'running', detail: 'Adding the device; this takes up to two minutes.' } });
        }
        if (command === 'hub.zigbee.permit_join') {
            if (!S.password) return nopw();
            if (params.seconds === 0) { S.win = { state: 'closed', left_s: 0 }; return reply(200, { ok: true, result: { open: false, message: 'Zigbee pairing is closed.' } }); }
            S.win = { state: 'open', left_s: params.seconds };
            if (S.pjOpening) return reply(200, { ok: true, result: { open: false, opening: true, message: 'Zigbee pairing is being opened; check again in a moment.' } });
            return reply(200, { ok: true, result: { open: true, seconds_left: params.seconds, message: 'Zigbee pairing is open; put the new device into pairing mode now.' } });
        }
        return reply(404, { error: 'unsupported' });
    }
    if (p === '/api/v1/muse/config') return S.musePerm ? reply(200, { enabled: true, perm: S.musePerm }) : reply(404, {});
    if (p === '/api/v1/matter/status' && S.mstatusFail) return reply(503, { error: 'unavailable', message: 'x' });
    if (p === '/api/v1/matter/status') return reply(200, Object.assign({ state: 'running', fail_step: null, allow_test_devices: false }, S.mstatus || {}));
    if (p === '/api/v1/matter/allow_test_devices' && method === 'POST') {
        if (!S.password) return reply(409, { error: 'password_required', message: 'set an admin password before changing Matter settings from the local API' });
        S.mstatus = Object.assign({}, S.mstatus || {}, { allow_test_devices: body.allow });
        return reply(200, { allow: body.allow });
    }
    if (p === '/api/v1/matter/reset' && method === 'POST') {
        if (!(S.mstatus && S.mstatus.fail_step === 'ident_lost')) return reply(409, { error: 'conflict', message: 'the Matter controller only needs a reset when its identity is lost; it is running' });
        return reply(200, { status: 'ok', message: 'rebooting' });
    }
    if (p.startsWith('/api/v1/hub/job?id=')) {
        const id = decodeURIComponent(p.split('=')[1]);
        if (S.jobNet && S.jobNet[id] && S.jobNet[id]-- > 0) return null;   // 模拟网络抖动：fetch 没回包
        const q = (S.jobs && S.jobs[id]) || [];
        const st = q.length > 1 ? q.shift() : q[0];
        if (!st) return reply(404, { error: 'not_found', message: 'no such job' });
        return reply(200, { ok: true, result: Object.assign({ job: id, command: 'x', age_s: 1 }, st) });
    }
    if (p === '/api/v1/hub/device/remove' && method === 'POST') {
        if (!S.password) return reply(409, { error: 'password_required', message: 'x' });
        return reply(202, { ok: true, result: { job: body.force ? '9' : '8', state: 'running' } });
    }
    if (p === '/api/v1/hub/device/sensitive' && method === 'POST') {
        if (!S.password) return reply(409, { error: 'password_required', message: 'x' });
        const d = S.devs.find(x => x.id === body.id);
        d.sensitive = body.sensitive;
        return reply(200, { ok: true, result: { id: d.id, sensitive: d.sensitive } });
    }
    if (p === '/api/v1/zigbee/status') return reply(200, { hub: { join_window: S.win } });
    return reply(404, {});
};
const open = async (S, opts) => {
    const h = makeHarness(mkFetch(S), opts);
    h.App.navigate = () => {};
    h.DevicesPage.render(el(h, 'content'));
    await settle();
    return h;
};
const K = (id) => id.replace(/[^a-z0-9]/gi, '');
const invokes = (S, cmd) => S.calls.filter(c => c.p === '/api/v1/hub/invoke' && c.body && c.body.command === cmd);

(async () => {

{
    const S = freshState(), h = makeHarness(mkFetch(S));
    check('devices 页已注册', !!h.App.pages['devices']);
}

// ======================= 显示的值来自设备 =======================
{
    const S = freshState(), h = await open(S);
    const list = el(h, 'dv-list').innerHTML;
    check('名字 / 房间 / 型号来自设备', /Living room light/.test(list) && /Living room ·/.test(list) && /E11-G13/.test(list));
    check('读数来自设备（开、亮度 100%、电量 45%）', /On · Brightness 100%/.test(list) && /Battery 45%/.test(list));
    check('没起名的：Unnamed + 类型', /Unnamed Motion sensor/.test(list), list.slice(0, 200));
    check('离线的标红、从没收到过的写明', /Offline/.test(list) && /not seen since the hub started/.test(list));
    check('Thread 成员只读（没有改名输入框）', /Thread · sleepy_end_device/.test(list) && !h.els.has('dv-name-' + K(THREAD.id)));
    check('设备给的字符串都转义（名字 / 房间 / 型号）', !/<img src=x/.test(list) && !/<script>/.test(list) && !/<b>/.test(list) && /&lt;img src=x/.test(list));
    check('onclick 里只有 [a-z0-9] 的 key，没有原样的设备 id', !/onclick="[^"]*zb-/.test(list) && /DevicesPage\._ctl\('zbb0ce181403586d1a','turn_on'\)/.test(list));
    check('推送开关只给门窗 / 人体类（人体有、灯没有）', h.els.has('dv-notify-' + K(MOTION.id)) && !h.els.has('dv-notify-' + K(LIGHT.id)));
    check('推送开关回填设备的值', el(h, 'dv-notify-' + K(MOTION.id)).checked === true);
    check('可控的灯有开 / 关和亮度；不可控的没有', h.els.has('dv-on-' + K(LIGHT.id)) && h.els.has('dv-lvl-' + K(LIGHT.id)) && !h.els.has('dv-on-' + K(MOTION.id)));
    check('轮询 2 s 一次（门开 / 有人经过几秒内就看得到；2026-10-24 前是 10 s）', h.timers.some(x => x.ms === 2000 && !x.cleared));
}
{   // 上一次还没读完：这一拍跳过、不叠请求；读了几页就等几秒（每页一个读令牌，REST 读桶每秒 2 个）
    const S = freshState(), h = await open(S);
    const poll = h.timers.find(x => x.ms === 2000 && !x.cleared);
    S.calls.length = 0;
    h.DevicesPage._loading = true;
    poll.fn(); await settle();
    check('还在读：这一拍不发请求', S.calls.filter(c => c.p.startsWith('/api/v1/hub/devices')).length === 0);
    h.DevicesPage._loading = false; h.DevicesPage._loadT = -1e9;   // 上次读是很久以前（单调时钟 performance.now）
    poll.fn(); await settle();
    check('读完了：下一拍照常读', S.calls.filter(c => c.p.startsWith('/api/v1/hub/devices')).length === 1 && h.DevicesPage._loading === false);
    S.calls.length = 0;
    h.DevicesPage._loadPages = 5; h.DevicesPage._loadT = performance.now();
    poll.fn(); await settle();
    check('上次读了 5 页：5 s 内不再读', S.calls.filter(c => c.p.startsWith('/api/v1/hub/devices')).length === 0);
    S.pageSize = 2;                                            // 4 台、每页 2 台
    h.DevicesPage._loadT = -1e9;
    poll.fn(); await settle();
    check('分页读：记下读了几页（2）', h.DevicesPage._loadPages === 2 && S.calls.filter(c => c.p.startsWith('/api/v1/hub/devices')).length === 2);
}
{   // 2 s 一拍：没变的不重画（卡片下的结果行、按下没抬起的点击都不被冲掉），真变了这一拍就画；后台标签页不读
    const S = freshState(), h = await open(S);
    const poll = h.timers.find(x => x.ms === 2000 && !x.cleared);
    const tick = async () => { h.DevicesPage._loadT = -1e9; poll.fn(); await settle(); };
    h.DevicesPage._msg('dv-res-' + K(LIGHT.id), false, 'No answer from the hub yet.');
    const card0 = el(h, 'dv-on-' + K(LIGHT.id));
    S.devs[0].last_seen_s = 99;                                // 只有"多久以前"变了
    await tick();
    check('只有时间变了：不重画（同一个按钮元素、结果行还在）', el(h, 'dv-on-' + K(LIGHT.id)) === card0 && /No answer/.test(el(h, 'dv-res-' + K(LIGHT.id)).innerHTML));
    S.devs[0].state = { onoff: false, level_pct: 100 };       // 灯被别人关了
    await tick();
    check('读数变了：这一拍就重画', /Off/.test(el(h, 'dv-list').innerHTML) && el(h, 'dv-on-' + K(LIGHT.id)) !== card0);   // DOM 桩不解析子元素内容：看列表的 HTML
    check('重画后卡片下的结果行还在', /No answer/.test(el(h, 'dv-res-' + K(LIGHT.id)).innerHTML));
    h.DevicesPage._paintT = -1e9;                              // 30 s 没画过：时间也该刷新了
    S.devs[0].last_seen_s = 140;
    const card1 = el(h, 'dv-on-' + K(LIGHT.id));
    await tick();
    check('30 s 没重画过：照样重画（"多久以前"别停住）', el(h, 'dv-on-' + K(LIGHT.id)) !== card1);
    h.document.hidden = true;
    S.calls.length = 0;
    await tick();
    check('后台标签页：不读（不占 REST 读桶）', S.calls.filter(c => c.p.startsWith('/api/v1/hub/devices')).length === 0);
}

// ======================= Matter 设备（M7 第四步）：可管（改名 / 房间 / 敏感），没订阅之前不说在线 / 离线 =======================
{
    const S = freshState(); S.devs = [JSON.parse(JSON.stringify(MATTER))];
    const h = await open(S);
    const list = el(h, 'dv-list').innerHTML;
    check('Matter 设备可管：有改名输入框、型号来自设备；不可控的（controllable:false）不给"限制 Muse 控制"', h.els.has('dv-name-' + K(MATTER.id)) && !h.els.has('dv-sens-' + K(MATTER.id)) && /Eve Energy 20EBU4101/.test(list));
    check('Matter 设备不当成 Thread 成员画，也不说在线 / 离线（订阅之前不知道）', !/Thread ·/.test(list) && !/Online|Offline/.test(list) && /Matter/.test(list), list.slice(0, 300));
    check('controllable:false 的 Matter 设备没有开 / 关按钮', !h.els.has('dv-on-' + K(MATTER.id)));
    check('协议筛选里有 Matter', /<option value="matter">Matter<\/option>/.test(dom(h)));
}

// ======================= 读数的键名与固件一致（M7 第五步发现：门开着显示"关闭"、照度从来不显示） =======================
{
    const S = freshState();
    S.devs = [{ id: 'mt-0000000000000005', protocol: 'matter', name: 'Door', room: '', kind: 'contact_sensor', capabilities: ['contact', 'battery'],
                controllable: false, online: true, last_seen_s: 5, state: { contact: 'open', battery_pct: 100 }, notify: false, sensitive: false },
              { id: 'mt-0000000000000006', protocol: 'matter', name: 'Plug', room: '', kind: 'plug', capabilities: ['onoff', 'power', 'energy'],
                controllable: false, online: true, last_seen_s: 1, state: { onoff: true, power_w: 3.4, energy_kwh: 0.125 }, notify: false, sensitive: false },
              { id: 'zb-0000000000000007', protocol: 'zigbee', name: 'Lux', room: '', kind: 'light_sensor', capabilities: ['illuminance'],
                controllable: false, online: true, last_seen_s: 1, state: { illuminance_lux: 320 }, notify: false, sensitive: false }];
    const h = await open(S);
    const list = el(h, 'dv-list').innerHTML;
    check('门磁 contact:"open" 显示"打开"（不是按真假判成"关闭"）', /Open/.test(list) && !/Closed/.test(list), list.slice(0, 300));
    check('功率 / 电能照固件的键显示', /3\.4 W/.test(list) && /0\.125 kWh/.test(list));
    check('照度读 illuminance_lux', /320 lx/.test(list));
    check('Matter 设备知道在线后照常显示在线', /Online/.test(list) && !/>Matter</.test(list));
    S.devs[0].state.contact = 'closed';
    await h.DevicesPage._load();
    await settle();
    const l2 = el(h, 'dv-list').innerHTML;
    check('contact:"closed" 显示"关闭"', /Closed/.test(l2) && !/>Open</.test(l2) && !/ Open /.test(l2));
}

// ======================= Matter 设备可控（M7 第六步）：controllable 跟着能力走，按钮发的是同一个 hub.device.control =======================
{
    const S = freshState();
    S.devs = [{ id: 'mt-0000000000000006', protocol: 'matter', name: 'Plug', room: '', kind: 'plug', capabilities: ['onoff', 'power'],
                controllable: true, online: true, last_seen_s: 1, state: { onoff: true, power_w: 0.0 }, notify: false, sensitive: false }];
    const h = await open(S);
    check('可控的 Matter 插座有开 / 关按钮', h.els.has('dv-on-' + K('mt-0000000000000006')) && h.els.has('dv-off-' + K('mt-0000000000000006')));
    S.calls.length = 0;
    await h.DevicesPage._ctl(K('mt-0000000000000006'), 'turn_off');
    const c = invokes(S, 'hub.device.control')[0];
    check('点"关"：hub.device.control {id:"mt-…", action:"turn_off"}', c && c.body.params.id === 'mt-0000000000000006' && c.body.params.action === 'turn_off', JSON.stringify(c && c.body));
}

// ======================= 添加 / 移除 Matter 设备（M7 第七步）：都是作业，显示网关说的结论 =======================
{
    const S = freshState();
    S.devs = [JSON.parse(JSON.stringify(MATTER)), JSON.parse(JSON.stringify(LIGHT))];
    S.jobs = { '7': [{ state: 'running' }, { state: 'done', result: { id: 'mt-0000000000000008' } }] };
    const h = await open(S);
    check('Matter 卡片有"从网关移除"，Zigbee 卡片没有', h.els.has('dv-rm-' + K(MATTER.id)) && !h.els.has('dv-rm-' + K(LIGHT.id)));
    S.calls.length = 0;
    await h.DevicesPage._mtAdd();
    check('没填配对码：提示、不发请求', invokes(S, 'hub.matter.commission').length === 0 && /pairing code first/.test(el(h, 'dv-mt-msg').innerHTML));
    el(h, 'dv-mt-code').value = '  MT:Y.K90AFN00KA0648G00  ';
    el(h, 'dv-mt-name').value = 'Desk lamp';
    await h.DevicesPage._mtAdd();
    const c = invokes(S, 'hub.matter.commission')[0];
    check('添加：hub.matter.commission {setup_code（去掉首尾空白）, name}', c && c.body.params.setup_code === 'MT:Y.K90AFN00KA0648G00' && c.body.params.name === 'Desk lamp', JSON.stringify(c && c.body));
    const jt = h.timers.findIndex(x => x.ms === 3000 && !x.cleared);
    check('受理（202 {job}）后每 3 s 查一次作业', jt >= 0 && /Adding the device/.test(el(h, 'dv-mt-msg').innerHTML));
    await h.DevicesPage._mtAdd();
    check('作业没完之前再点：不再发', invokes(S, 'hub.matter.commission').length === 1);
    await h.timers[jt].fn(); await settle();
    check('作业还在跑：继续等', !h.timers[jt].cleared);
    S.calls.length = 0;
    await h.timers[jt].fn(); await settle();
    check('作业完成：显示"已添加"、停止轮询、清掉配对码、重读列表', h.timers[jt].cleared && /Added\./.test(el(h, 'dv-mt-msg').innerHTML)
          && el(h, 'dv-mt-code').value === '' && S.calls.some(x => x.p.startsWith('/api/v1/hub/devices')));
    el(h, 'dv-mt-code').value = 'bad<b>';
    await h.DevicesPage._mtAdd();
    const m = el(h, 'dv-mt-msg').innerHTML;
    check('码不对：显示网关给的原因（转义过）', /not a valid Matter pairing code/.test(m) && !/<b>/.test(m) && /&lt;b&gt;/.test(m), m);
}
{
    const S = freshState();
    S.devs = [JSON.parse(JSON.stringify(MATTER))];
    S.jobs = { '8': [{ state: 'failed', error: 'unreachable', message: 'The device did not answer, so it still has access from this hub.' }], '9': [{ state: 'done', result: { id: MATTER.id } }] };
    const h = await open(S);
    S.calls.length = 0;
    await h.DevicesPage._mtRemove(K(MATTER.id));
    const rm = S.calls.filter(x => x.p === '/api/v1/hub/device/remove');
    check('移除：先问；POST {id, confirm:"REMOVE"}，不带 force', h.confirms.length === 1 && rm.length === 1 && rm[0].body.id === MATTER.id && rm[0].body.confirm === 'REMOVE' && !('force' in rm[0].body));
    let jt = h.timers.findIndex(x => x.ms === 3000 && !x.cleared);
    await h.timers[jt].fn(); await settle();
    const rm2 = S.calls.filter(x => x.p === '/api/v1/hub/device/remove');
    check('设备没应答：照实说，再问一次"仍然移除"，同意了才带 force 重发', h.confirms.length === 2 && rm2.length === 2 && rm2[1].body.force === true);
    jt = h.timers.findIndex(x => x.ms === 3000 && !x.cleared);
    await h.timers[jt].fn(); await settle();
    check('强制移除完成：页面级提示"已移除"（卡片随列表重读消失）、停止轮询', h.timers[jt].cleared && /Removed\./.test(el(h, 'dv-rm-msg').innerHTML));
}
{
    const S = freshState();
    S.devs = [];
    S.jobs = { '7': [{ state: 'failed', error: 'not_found', message: 'No device is waiting <img src=x onerror=alert(1)>' }] };
    const h = await open(S);
    el(h, 'dv-mt-code').value = '3497-011-2332';
    await h.DevicesPage._mtAdd();
    const jt = h.timers.findIndex(x => x.ms === 3000 && !x.cleared);
    await h.timers[jt].fn(); await settle();
    const m = el(h, 'dv-mt-msg').innerHTML;
    check('作业失败：显示网关的句子（转义过）、停止轮询、可以再试', h.timers[jt].cleared && /No device is waiting/.test(m) && !/<img/.test(m) && /&lt;img/.test(m), m);
}
// 审计（第七步）：双击、迟到的轮询、各种当场拒的原因、作业查不到、网络抖动
{
    const S = freshState();
    S.devs = [JSON.parse(JSON.stringify(MATTER))];
    S.jobs = { '7': [{ state: 'running' }] };
    const h = await open(S);
    el(h, 'dv-mt-code').value = '3497-011-2332';
    S.calls.length = 0;
    const p1 = h.DevicesPage._mtAdd();
    await h.DevicesPage._mtAdd();
    await p1; await settle();
    check('双击"添加"：请求在途时第二下不发', invokes(S, 'hub.matter.commission').length === 1 && /Adding the device/.test(el(h, 'dv-mt-msg').innerHTML));
    await h.DevicesPage._mtRemove(K(MATTER.id));
    check('添加进行中点"移除"：说明在忙、不发', !S.calls.some(x => x.p === '/api/v1/hub/device/remove') && /still adding or removing/.test(el(h, 'dv-rm-msg').innerHTML));
    S.jobNet = { '7': 2 };
    const jt = h.timers.findIndex(x => x.ms === 3000 && !x.cleared);
    await h.timers[jt].fn(); await settle();
    await h.timers[jt].fn(); await settle();
    check('网络抖两次：继续等', !h.timers[jt].cleared);
    S.jobs['7'] = [];
    await h.timers[jt].fn(); await settle();
    check('作业查不到（404）：专门的句子、停止轮询、重读列表', h.timers[jt].cleared && /no longer reports how this ended/.test(el(h, 'dv-mt-msg').innerHTML));
    el(h, 'dv-mt-code').value = 'ble';
    await h.DevicesPage._mtAdd();
    check('只能蓝牙的码：显示固件的原因（不只是"做不了"）', /added over Bluetooth first/.test(el(h, 'dv-mt-msg').innerHTML), el(h, 'dv-mt-msg').innerHTML);
}
{
    const S = freshState();
    S.devs = [JSON.parse(JSON.stringify(MATTER))];
    S.jobs = { '8': [{ state: 'failed', error: 'unreachable', message: 'no answer' }], '9': [{ state: 'running' }] };
    const h = await open(S);
    await h.DevicesPage._mtRemove(K(MATTER.id));
    const old = h.timers.findIndex(x => x.ms === 3000 && !x.cleared);
    await h.timers[old].fn(); await settle();
    const nu = h.timers.findIndex(x => x.ms === 3000 && !x.cleared);
    const confirms = h.confirms.length;
    await h.timers[old].fn(); await settle();                // 旧计时器迟到的一次（回包慢于 3 s 时会这样）
    check('强制移除开始后，旧轮询迟到的回包不认：不再弹确认、不掐掉新轮询', nu >= 0 && nu !== old && h.confirms.length === confirms && !h.timers[nu].cleared);
    S.jobs['9'] = [{ state: 'done', result: { id: MATTER.id, detail: 'Removed from the hub only; reset it in its own app.' } }];
    await h.timers[nu].fn(); await settle();
    check('强制移除完成：带上固件的说明（去它自己的 App 里复位）', /reset it in its own app/.test(el(h, 'dv-rm-msg').innerHTML));
}
{
    const S = freshState();
    S.devs = [JSON.parse(JSON.stringify(MATTER))];
    const h = await open(S, { confirm: false });
    S.calls.length = 0;
    await h.DevicesPage._mtRemove(K(MATTER.id));
    check('移除：不同意就不发', h.confirms.length === 1 && !S.calls.some(x => x.p === '/api/v1/hub/device/remove'));
}

// ======================= Matter 控制器的两个本地操作（10.1）：测试设备开关、身份丢了才出现的重置 =======================
{
    const S = freshState();
    S.devs = [];
    const h = await open(S, { confirm: false });
    check('测试设备开关显示网关回报的值（默认关）；身份没丢不显示重置', el(h, 'dv-mt-test').checked === false && el(h, 'dv-mt-reset').style.display === 'none');
    el(h, 'dv-mt-test').checked = true;
    await h.DevicesPage._mtTest(true);
    check('打开前先问；不同意：不发、勾选框退回', h.confirms.length === 1 && !S.calls.some(c => c.p === '/api/v1/matter/allow_test_devices') && el(h, 'dv-mt-test').checked === false);
}
{
    const S = freshState();
    S.devs = [];
    const h = await open(S);
    await h.DevicesPage._mtTest(true);
    const c = S.calls.filter(x => x.p === '/api/v1/matter/allow_test_devices');
    check('同意了：POST {allow:true}，回读后勾上、说明已允许', c.length === 1 && c[0].body.allow === true && el(h, 'dv-mt-test').checked === true && /allowed/.test(el(h, 'dv-mt-test-msg').innerHTML));
    const n = h.confirms.length;
    await h.DevicesPage._mtTest(false);
    const c2 = S.calls.filter(x => x.p === '/api/v1/matter/allow_test_devices');
    check('关掉：不用问，POST {allow:false}，回读后不勾', h.confirms.length === n && c2.length === 2 && c2[1].body.allow === false && el(h, 'dv-mt-test').checked === false);
    await h.DevicesPage._mtReset();
    check('身份没丢时（没出现重置框）硬调：没输 RESET 不发', !S.calls.some(x => x.p === '/api/v1/matter/reset') && /Type RESET/.test(el(h, 'dv-mt-reset-msg').innerHTML));
}
{
    const S = freshState();
    S.devs = [];
    S.password = false;
    const h = await open(S);
    await h.DevicesPage._mtTest(true);
    check('没设密码：如实提示、勾选框照网关的值（没打开）', /admin password/i.test(el(h, 'dv-mt-test-msg').innerHTML) && el(h, 'dv-mt-test').checked === false, el(h, 'dv-mt-test-msg').innerHTML);
}
{
    const S = freshState();
    S.devs = [];
    S.mstatusFail = true;
    const h = await open(S);
    check('状态读不回来：勾选框一直禁用（不猜开关的值）', el(h, 'dv-mt-test').disabled === true);
}
{
    const S = freshState();
    S.devs = [];
    const h = await open(S);
    S.password = false;
    S.mstatusFail = true;
    el(h, 'dv-mt-test').checked = true;                      // 用户点了
    await h.DevicesPage._mtTest(true);
    check('保存失败、读回也失败：勾选框留在网关上次报的值（关），不留着刚点的', el(h, 'dv-mt-test').checked === false && /admin password/i.test(el(h, 'dv-mt-test-msg').innerHTML));
}
{
    const S = freshState();
    S.devs = [];
    S.mstatus = { state: 'failed', fail_step: 'ident_lost' };
    const h = await open(S);
    check('身份丢了：显示重置框', el(h, 'dv-mt-reset').style.display === '');
    el(h, 'dv-mt-reset-txt').value = 'reset';
    await h.DevicesPage._mtReset();
    check('输的不是 RESET（大小写也算）：不发', !S.calls.some(x => x.p === '/api/v1/matter/reset'));
    el(h, 'dv-mt-reset-txt').value = 'RESET';
    await h.DevicesPage._mtReset();
    const r = S.calls.filter(x => x.p === '/api/v1/matter/reset');
    check('输了 RESET 并确认：POST {confirm:"RESET"}、提示正在重启', r.length === 1 && r[0].body.confirm === 'RESET' && /restarting/.test(el(h, 'dv-mt-reset-msg').innerHTML));
}

// ======================= "限制 Muse 控制"（敏感标记）：只在能控制的设备上给，下面一行按 Muse 页的权限说清楚现在的效果 =======================
{
    const S = freshState();
    S.musePerm = { control: true, manage: true, sensitive: false };
    S.devs = [JSON.parse(JSON.stringify(LIGHT)), JSON.parse(JSON.stringify(MOTION)),
              Object.assign(JSON.parse(JSON.stringify(LIGHT)), { id: 'zb-00000000000000aa', name: 'Garage', sensitive: true }),
              Object.assign(JSON.parse(JSON.stringify(MOTION)), { id: 'zb-00000000000000bb', sensitive: true })];
    const h = await open(S);
    const L = el(h, 'dv-list').innerHTML;
    check('能控制的灯有这个选项，传感器没有', h.els.has('dv-sens-' + K(LIGHT.id)) && !h.els.has('dv-sens-' + K(MOTION.id)));
    check('选项写明是"限制 Muse 控制"和适用的设备', /Restrict Muse control/.test(el(h, 'dv-list').innerHTML) && /garage doors/.test(el(h, 'dv-list').innerHTML));
    check('没勾：现在 Muse 可以控制', /Now: Muse can control it\./.test(L));
    check('勾了、Muse 页"敏感操作"没开：现在 Muse 不能控制、网页照常', /Muse cannot control it/.test(L) && /still control it here/.test(L));
    check('传感器以前被勾过：照样显示（好取消），并说明勾不勾都一样', h.els.has('dv-sens-' + K('zb-00000000000000bb')) && /no effect/.test(L));
}
{
    const S = freshState();
    S.musePerm = { control: true, manage: true, sensitive: true };
    S.devs = [Object.assign(JSON.parse(JSON.stringify(LIGHT)), { sensitive: true })];
    const h = await open(S);
    check('勾了、"敏感操作"开着：现在 Muse 仍然可以控制', /still control it, because/.test(el(h, 'dv-list').innerHTML) && !/cannot control it/.test(el(h, 'dv-list').innerHTML));
}
{
    const S = freshState();
    S.musePerm = { control: false, manage: false, sensitive: false };
    S.devs = [JSON.parse(JSON.stringify(LIGHT))];
    const h = await open(S);
    check('Muse 的"控制设备"没开：说明勾不勾都一样', /not allowed to control any device/.test(el(h, 'dv-list').innerHTML));
}
{
    const S = freshState();
    S.devs = [JSON.parse(JSON.stringify(LIGHT))];
    const h = await open(S);
    check('Muse 权限读不到：不猜（不写现在的效果）', !/Now: Muse/.test(el(h, 'dv-list').innerHTML) && h.els.has('dv-sens-' + K(LIGHT.id)));
}

// ======================= 控制请求符合契约、显示回包里的状态 =======================
{
    const S = freshState(), h = await open(S);
    S.calls.length = 0;
    await h.DevicesPage._ctl(K(LIGHT.id), 'turn_off');
    const c = invokes(S, 'hub.device.control')[0];
    check('关：hub.device.control {id, action:"turn_off"}，没有多余字段', c && c.body.params.id === LIGHT.id && c.body.params.action === 'turn_off' && !('value' in c.body.params),
          JSON.stringify(c && c.body));
    check('关之后显示回包里的状态（Off）', /Off/.test(el(h, 'dv-state-' + K(LIGHT.id)).innerHTML), el(h, 'dv-state-' + K(LIGHT.id)).innerHTML);
    await h.DevicesPage._ctl(K(LIGHT.id), 'set_brightness', 40);
    const b = invokes(S, 'hub.device.control')[1];
    check('亮度：value 是字符串 "40"（hub_params 只认字符串）', b && b.body.params.value === '40' && typeof b.body.params.value === 'string', JSON.stringify(b && b.body));
    S.ctlState = { onoff: false, level_pct: 100 };            // 设备回了确认、可状态没变
    await h.DevicesPage._ctl(K(LIGHT.id), 'turn_on');
    check('显示的是网关回的状态（Off），不是刚发出去的"开"', /Off/.test(el(h, 'dv-state-' + K(LIGHT.id)).innerHTML) && !/Brightness 40/.test(el(h, 'dv-state-' + K(LIGHT.id)).innerHTML));
    check('结论文字来自回包 detail', /confirmed the command/.test(el(h, 'dv-res-' + K(LIGHT.id)).innerHTML));
}

// ======================= 一台设备同一时刻只有一条控制在途（2026-10-24：连点十几次把网关看门狗逼到整机重启） =======================
{
    const S = freshState(), h = await open(S);
    let release; S.ctlGate = new Promise(r => { release = r; });
    S.calls.length = 0;
    const first = h.DevicesPage._ctl(K(LIGHT.id), 'turn_off');
    await settle();
    check('在途时开 / 关 / 亮度都不可点，写明在等设备', el(h, 'dv-on-' + K(LIGHT.id)).disabled && el(h, 'dv-off-' + K(LIGHT.id)).disabled && el(h, 'dv-lvl-' + K(LIGHT.id)).disabled
          && /waiting for the device/.test(el(h, 'dv-res-' + K(LIGHT.id)).innerHTML));
    h.DevicesPage._ctl(K(LIGHT.id), 'turn_on');                 // 按钮灰了也可能被调到（onchange、旧的 DOM）：函数自己挡。不 await：挡不住时它会跟着等闸门
    await settle();
    check('在途时再点：不发第二条，说明还在等上一条', invokes(S, 'hub.device.control').length === 1 && /Still waiting/.test(el(h, 'dv-res-' + K(LIGHT.id)).innerHTML));
    h.DevicesPage._paint();                                    // 轮询重画列表（_paint；render 才清在途表）
    check('在途时重画列表：按钮仍是灰的', el(h, 'dv-on-' + K(LIGHT.id)).disabled === true, el(h, 'dv-list').innerHTML.slice(0, 120));
    release(); await first; await settle();
    check('回包到了：按钮恢复可点', !el(h, 'dv-on-' + K(LIGHT.id)).disabled && !el(h, 'dv-lvl-' + K(LIGHT.id)).disabled);
    S.ctlGate = null; S.ctlFail = true;
    await h.DevicesPage._ctl(K(LIGHT.id), 'turn_on');
    check('失败了也恢复可点（下一条能发）', !el(h, 'dv-on-' + K(LIGHT.id)).disabled && invokes(S, 'hub.device.control').length === 2);
    S.ctlFail = false;
    await h.DevicesPage._ctl(K(LIGHT.id), 'turn_on');
    check('失败之后的下一条照常发出', invokes(S, 'hub.device.control').length === 3);
}
{
    const S = freshState(), h = await open(S);                 // 请求一直不回（网络静默断开）：30 s 后按钮放开、说明网关没回应（审计 ⚠️）
    S.ctlGate = new Promise(() => {});
    S.calls.length = 0;
    h.DevicesPage._ctl(K(LIGHT.id), 'turn_off');
    await settle();
    const late = h.timeouts.find(x => x.ms === 30000);
    check('控制在途时挂了 30 s 的兜底', !!late && el(h, 'dv-on-' + K(LIGHT.id)).disabled);
    if (late) late.fn();
    await settle();
    check('30 s 没回：按钮放开、说明网关没回应', !el(h, 'dv-on-' + K(LIGHT.id)).disabled && /No answer from the hub yet/.test(el(h, 'dv-res-' + K(LIGHT.id)).innerHTML));
    S.ctlGate = null;
    await h.DevicesPage._ctl(K(LIGHT.id), 'turn_on');
    check('兜底之后下一条照常发出', invokes(S, 'hub.device.control').length === 2);
}
{
    const S = freshState(), h = await open(S);                 // 页面整页重画（render）时旧命令的收尾不能放掉新命令的"在途"
    let r1; S.ctlGate = new Promise(r => { r1 = r; });
    const a = h.DevicesPage._ctl(K(LIGHT.id), 'turn_off');
    await settle();
    h.DevicesPage.render(el(h, 'content')); await settle();
    let r2; S.ctlGate = new Promise(r => { r2 = r; });
    const b = h.DevicesPage._ctl(K(LIGHT.id), 'turn_on');
    await settle();
    r1(); await a; await settle();
    const n = invokes(S, 'hub.device.control').length;
    h.DevicesPage._ctl(K(LIGHT.id), 'turn_off');                // 不 await：挡不住时它会跟着等闸门
    await settle();
    check('旧命令收尾不放掉新命令的在途：第三条被挡', invokes(S, 'hub.device.control').length === n && el(h, 'dv-on-' + K(LIGHT.id)).disabled, String(n));
    r2(); await b; await settle();
}

// ======================= 改名 / 房间：只发改了的，改完重读 =======================
{
    const S = freshState(), h = await open(S);
    const fill = () => {                                       // 浏览器会照 value 属性填输入框，DOM 桩不会：照设备当前值填
        for (const d of S.devs) { if (h.els.has('dv-name-' + K(d.id))) { el(h, 'dv-name-' + K(d.id)).value = d.name || ''; el(h, 'dv-room-' + K(d.id)).value = d.room || ''; } }
    };
    fill();
    el(h, 'dv-name-' + K(LIGHT.id)).value = 'Sofa lamp';
    S.calls.length = 0;
    await h.DevicesPage._save(K(LIGHT.id));
    const u = invokes(S, 'hub.device.update')[0];
    check('改名：只发 name（房间没改不发）', u && u.body.params.name === 'Sofa lamp' && !('room' in u.body.params) && !('notify' in u.body.params), JSON.stringify(u && u.body));
    check('改完重新读列表', S.calls.some(c => c.p.startsWith('/api/v1/hub/devices')));
    check('显示重读后的名字', /Sofa lamp/.test(el(h, 'dv-list').innerHTML));
    fill();
    S.calls.length = 0;
    await h.DevicesPage._save(K(LIGHT.id));
    check('什么都没改：不发请求', invokes(S, 'hub.device.update').length === 0 && /Nothing changed/.test(el(h, 'dv-res-' + K(LIGHT.id)).innerHTML));
}

// ======================= 推送开关：显示改完之后网关报的值 =======================
{
    const S = freshState(); S.notifyReject = true;
    const h = await open(S);
    S.calls.length = 0;
    el(h, 'dv-notify-' + K(MOTION.id)).checked = false;
    await h.DevicesPage._notify(K(MOTION.id), false);
    const u = invokes(S, 'hub.device.update')[0];
    check('推送开关：发 {id, notify:false}（布尔）', u && u.body.params.notify === false && Object.keys(u.body.params).length === 2, JSON.stringify(u && u.body));
    check('网关没改成：勾选弹回网关的值（还开着）', el(h, 'dv-notify-' + K(MOTION.id)).checked === true);
}

// ======================= 敏感标记：只在本地、取消先确认 =======================
{
    const S = freshState(); S.devs[0].sensitive = true;
    const h = await open(S, { confirm: false });
    S.calls.length = 0;
    await h.DevicesPage._sensitive(K(LIGHT.id), false);
    check('取消敏感：先问（不同意就不发）', h.confirms.length === 1 && !S.calls.some(c => c.p === '/api/v1/hub/device/sensitive'));
    check('不同意：勾选回到网关的值（还是敏感）', el(h, 'dv-sens-' + K(LIGHT.id)).checked === true);
    const h2 = await open(S, { confirm: true });
    S.calls.length = 0;
    await h2.DevicesPage._sensitive(K(LIGHT.id), false);
    const c = S.calls.find(x => x.p === '/api/v1/hub/device/sensitive');
    check('同意：POST /api/v1/hub/device/sensitive {id, sensitive:false}', c && c.body.id === LIGHT.id && c.body.sensitive === false, JSON.stringify(c && c.body));
    check('标上敏感不用确认', (await (async () => { const n = h2.confirms.length; await h2.DevicesPage._sensitive(K(LIGHT.id), true); return h2.confirms.length === n; })()));
    check('敏感标记不经 hub.device.update（Muse 能调的那条命令不带它）', !invokes(S, 'hub.device.update').some(x => 'sensitive' in x.body.params));
}

// ======================= 没设管理密码 =======================
{
    const S = freshState(); S.password = false;
    const h = await open(S);
    el(h, 'dv-name-' + K(LIGHT.id)).value = 'Sofa lamp';
    el(h, 'dv-room-' + K(LIGHT.id)).value = 'Living room';
    await h.DevicesPage._save(K(LIGHT.id));
    check('没密码：保存后提示留在页面上（重读列表重画卡片之后再写，审计 ❌）', /admin password/.test(el(h, 'dv-res-' + K(LIGHT.id)).innerHTML),
          el(h, 'dv-res-' + K(LIGHT.id)).innerHTML);
    await h.DevicesPage._ctl(K(LIGHT.id), 'turn_off');
    check('没密码：如实提示去设管理密码', /admin password/.test(el(h, 'dv-res-' + K(LIGHT.id)).innerHTML), el(h, 'dv-res-' + K(LIGHT.id)).innerHTML);
    await h.DevicesPage._pair(120);
    check('没密码：添加设备也提示', /admin password/.test(el(h, 'dv-pair-msg').innerHTML));
}

// ======================= 名字按 UTF-8 字节限长（固件按字节，审计 ❌） =======================
{
    const S = freshState(), h = await open(S);
    el(h, 'dv-name-' + K(LIGHT.id)).value = '客厅里的大吊灯好亮好亮的';   // 12 个汉字 = 36 字节
    el(h, 'dv-room-' + K(LIGHT.id)).value = 'Living room';
    S.calls.length = 0;
    await h.DevicesPage._save(K(LIGHT.id));
    check('名字超过 32 字节：本地就拦下、说清原因、不发请求', invokes(S, 'hub.device.update').length === 0 && /32 bytes/.test(el(h, 'dv-res-' + K(LIGHT.id)).innerHTML));
    el(h, 'dv-name-' + K(LIGHT.id)).value = '客厅大吊灯';                 // 5 个汉字 = 15 字节
    await h.DevicesPage._save(K(LIGHT.id));
    check('中文名字没超：照发', invokes(S, 'hub.device.update').length === 1 && invokes(S, 'hub.device.update')[0].body.params.name === '客厅大吊灯');
    check('保存成功的提示留在页面上', /Saved/.test(el(h, 'dv-res-' + K(LIGHT.id)).innerHTML), el(h, 'dv-res-' + K(LIGHT.id)).innerHTML);
}

// ======================= 分页 / 筛选 =======================
{
    const S = freshState(); S.pageSize = 2;
    const h = await open(S);
    check('分页：跟着 next_cursor 读完', S.calls.filter(c => c.p.startsWith('/api/v1/hub/devices')).length === 2 &&
          S.calls.some(c => /cursor=2/.test(c.p)) && /Thread ·/.test(el(h, 'dv-list').innerHTML));
    el(h, 'dv-proto').value = 'thread';
    h.DevicesPage._filter();
    check('协议筛选：只剩 Thread', /Thread ·/.test(el(h, 'dv-list').innerHTML) && !/Living room light/.test(el(h, 'dv-list').innerHTML));
    el(h, 'dv-proto').value = '';
    el(h, 'dv-room').value = 'Living room';
    h.DevicesPage._filter();
    check('房间筛选：只剩客厅的灯', /Living room light/.test(el(h, 'dv-list').innerHTML) && !/Motion sensor/.test(el(h, 'dv-list').innerHTML));
    check('房间下拉来自设备（转义）', /Living room/.test(el(h, 'dv-room').innerHTML) && /K&quot;itchen&lt;b&gt;/.test(el(h, 'dv-room').innerHTML));
}

// ======================= 添加 Zigbee 设备 =======================
{
    const S = freshState(), h = await open(S);
    S.calls.length = 0;
    await h.DevicesPage._pair(120);
    const c = invokes(S, 'hub.zigbee.permit_join')[0];
    check('添加：permit_join {seconds:120}（数字）', c && c.body.params.seconds === 120, JSON.stringify(c && c.body));
    check('显示倒计时（来自回包 seconds_left）', /120 s left/.test(el(h, 'dv-pair-msg').innerHTML));
    S.win = { state: 'open', left_s: 87 };
    await h.DevicesPage._pollWindow();
    check('倒计时跟着 /zigbee/status 的 join_window.left_s', /87 s left/.test(el(h, 'dv-pair-msg').innerHTML));
    await h.DevicesPage._pair(0);
    check('停止：permit_join {seconds:0}、显示关了', invokes(S, 'hub.zigbee.permit_join')[1].body.params.seconds === 0 && /closed/i.test(el(h, 'dv-pair-msg').innerHTML));
}

{
    const S = freshState(); S.pjOpening = true;
    const h = await open(S);
    await h.DevicesPage._pair(120);
    check('窗口正在打开（opening）：照样开始轮询，不当成关了（审计 ❌）', h.timers.some(x => x.ms === 2000 && !x.cleared) && /being opened/.test(el(h, 'dv-pair-msg').innerHTML));
    await h.DevicesPage._pollWindow();
    check('落定之后显示倒计时', /120 s left/.test(el(h, 'dv-pair-msg').innerHTML));
}

// ======================= 离开页面 =======================
{
    const S = freshState(), h = await open(S);
    h.DevicesPage.destroy();
    check('离开页面：轮询停掉', h.timers.every(x => x.cleared));
}

console.log(`\nResult: ${PASS} passed, ${FAIL} failed`);
DONE = true;
process.exit(FAIL === 0 ? 0 : 1);
})();
HARNESS

node "$RUN"
