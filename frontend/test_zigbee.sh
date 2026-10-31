#!/bin/bash
# frontend/test_zigbee.sh —— Zigbee 页的行为断言（不用板子，只要 node）
#
# 为什么要有这个文件（2026-09-10，T1.5）：这一页曾经有个「Turbo Mode」开关，
# 把 /status 里的 zigbee_tx_power=20 渲染成 "(20 dBm Turbo)"。那个 20 只在
# NVS 里活过，从未下发给 NCP；同一块板子的 zigbee.db 备份里查到 NCP 实际
# 跑的是 ZHA 写进去的 8 dBm。UI 撒了几个月的谎，没有任何测试看得见。
#
# 发射功率由 Z2M/ZHA 每次启动写进 MG24（zigpy _get_effective_tx_power →
# bellows setRadioPower），网关既不产生、也读不到、也下发不了。所以本页
# 能做的只有：**不发表意见** + 把配置片段和地区限值摆到用户面前。
# 这里钉死的正是这几件事：
#   1. 页面上没有 Turbo 开关，也不再向 /zigbee/turbo 发请求
#   2. 收到带 zigbee_tx_power 的 status 也不渲染任何 dBm 数字（不能悄悄回来）
#   3. 只剩 Z2M 的片段：ZHA 现在向芯片询问本国推荐值、自动设置，不再需要粘贴（docs/14）；
#      快照表显示的网关地区 / HA 国家 / 生效上限 全部来自 /zigbee/status.tx_power
#   4. 片段带地区注释（US/CA 19 / EU-UK 10；19 = 射频最高档，芯片按地区封顶，docs/14）—— 20 dBm 在 FCC 下合法，在
#      EN 300 328 下超标，只给一个数等于教一半用户违规
#   5. 复制按钮复制的是片段原文
#
# DOM 桩照抄 test_led_mqtt.sh，同样的局限：不解析 HTML、不执行内联 onclick。

cd "$(dirname "$0")"
command -v node >/dev/null || { echo "需要 node"; exit 1; }

RUN=$(mktemp /tmp/zbtest.XXXX.cjs)
trap 'rm -f "$RUN"' EXIT

cat > "$RUN" << 'HARNESS'
const fs = require('fs');
const FILES = ['js/i18n.js', 'js/i18n/de.js', 'js/i18n/zh.js',
               'js/api.js', 'js/ws.js', 'js/app.js', 'js/pages/zigbee.js'];
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
            style: {}, _html: '', _listeners: {}, onclick: null,
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
    const dropOwned = (o) => {
        for (const [id, ow] of [...owner])
            if (ow === o) { dropOwned(id); owner.delete(id); els.delete(id); }
    };
    /* 记下每个元素的**文本**：片段是用 ${ZigbeePage.SNIPPET_ZHA} 直接拼进
       innerHTML 的 —— 复制按钮读的是 src.textContent，所以要从 HTML 里把
       <div id=...>…</div> 的内容抠出来喂给 textContent。 */
    const scan = (html, o) => {
        for (const m of String(html).matchAll(/id="([^"]+)"/g))
            if (!els.has(m[1])) mkEl(m[1], o);
        for (const m of String(html).matchAll(/<div id="(zb-txp-[a-z0-9]+)"[^>]*>([\s\S]*?)<\/div>/g)) {
            const e = els.get(m[1]); if (e) e.textContent = m[2];
        }
    };
    mkEl('app'); mkEl('content');

    const document = {
        getElementById: id => els.get(id) || null,
        querySelector: () => null, querySelectorAll: () => [],
    };
    const location = { _h: '', host: '127.0.0.1',
        get hash() { return this._h; },
        set hash(v) { this._h = v ? (String(v).startsWith('#') ? String(v) : '#' + v) : ''; } };
    const window = {};
    const WebSocket = function () { this.readyState = 0; this.close = () => {}; };
    const copied = [];
    const navigator = { clipboard: { writeText: async (s) => { copied.push(s); } } };
    const timers = [];
    const setInterval  = (fn, ms) => { timers.push({ fn, ms }); return timers.length - 1; };
    const clearInterval = () => {};

    const factory = new Function(
        'window', 'document', 'location', 'navigator', 'fetch', 'WebSocket',
        'setTimeout', 'clearTimeout', 'setInterval', 'clearInterval', 'console', 'TextEncoder',
        SRC + '\n; return { App, API, WS, ZigbeePage };');
    const ctx = factory(window, document, location, navigator, fetchImpl, WebSocket,
                        setTimeout, clearTimeout, setInterval, clearInterval, console, TextEncoder);
    return Object.assign(ctx, { document, els, copied });
}

const reply = (status, body) => ({ ok: status >= 200 && status < 300, status, json: async () => body });
let PASS = 0, FAIL = 0;
const check = (name, cond, detail) => {
    if (cond) { console.log(`✅ ${name}`); PASS++; }
    else { console.log(`❌ ${name}${detail ? ' — ' + detail : ''}`); FAIL++; }
};
const el = (h, id) => h.document.getElementById(id) ||
    { value: '', textContent: '(元素不存在)', checked: false, style: {}, innerHTML: '(元素不存在)', onclick: null };
const settle = () => new Promise(r => setTimeout(r, 0));

(async () => {

/* 固件回包桩。故意带上一个 zigbee_tx_power:20 —— 老固件就是这么回的，
   新前端遇到它必须无动于衷。 */
const status = {
    uptime: 10, mg24_alive: true, zigbee_channel: 20, zigbee_channel_source: 'declared',
    zigbee_tx_power: 20, zigbee_tx_power_source: 'host',
    mg24_resets: 0, bridge_mode: 'lan', uart_rx_bytes: 100, uart_tx_bytes: 50,
    active_interface: 'eth', eth_ip: '192.168.1.45', wifi_ip: '192.168.1.43',
    mac: 'aa:bb', eth_mac: 'cc:dd', zigbee_channel_check: null,
};
const zbStatus = { channel: 20, channel_source: 'declared', clients: [], restart_resets_board: true,
    /* docs/14 §5.2：NCP 的地区/功率快照。数值故意各不相同，好证明页面显示的确实是回包里的字段 */
    tx_power: { ncp_xncp: true, class: 2, cap_dbm: 10, requested_dbm: 8, host_class: 3, gw_class: 2,
                host_region: 'US', gw_region: 'EU', clamped_count: 4, read_uptime_s: 123 } };
const calls = [];
const fetchImpl = async (p, opts) => {
    calls.push({ p, method: (opts && opts.method) || 'GET', body: opts && opts.body });
    if (p === '/api/v1/status')        return reply(200, status);
    if (p === '/api/v1/zigbee/status') return reply(200, zbStatus);
    if (p === '/api/v1/api2/info')     return reply(200, { zb_chip: 'MG24', zb_version: '8.2.2', mac: 'aa:bb' });
    return reply(404, { error: 'not_found' });
};

const h = makeHarness(fetchImpl);
check('zigbee 页已注册到 App.pages', !!h.App.pages['zigbee']);
h.ZigbeePage.render(el(h, 'content'));
await settle(); await settle(); await settle();
const html = el(h, 'content').innerHTML;

// ---- 0. 连接地址（T9.37）：有 svc_ip 就用它；老固件退回活动网口的地址 ----
check('连接地址：老固件（没有 svc_ip）退回活动网口的地址', el(h, 'zb-conn-ip').textContent === 'socket://192.168.1.45:6638',
      el(h, 'zb-conn-ip').textContent);
{
    const st2 = Object.assign({}, status, { svc_ip: '192.168.1.36', svc_holder: 'eth', svc_identity: 'wifi' });
    const h2 = makeHarness(async (p, o) => p === '/api/v1/status' ? reply(200, st2) : fetchImpl(p, o));
    h2.ZigbeePage.render(el(h2, 'content'));
    await settle(); await settle(); await settle();
    check('连接地址：有 svc_ip 时用它（哪怕与活动网口的地址不同）', el(h2, 'zb-conn-ip').textContent === 'socket://192.168.1.36:6638',
          el(h2, 'zb-conn-ip').textContent);
    const st3 = Object.assign({}, status, { svc_ip: '0.0.0.0' });
    const h3 = makeHarness(async (p, o) => p === '/api/v1/status' ? reply(200, st3) : fetchImpl(p, o));
    h3.ZigbeePage.render(el(h3, 'content'));
    await settle(); await settle(); await settle();
    check('连接地址：svc_ip 为 0.0.0.0（还没有服务地址）时退回活动网口', el(h3, 'zb-conn-ip').textContent === 'socket://192.168.1.45:6638',
          el(h3, 'zb-conn-ip').textContent);
}

// ---- 1. 开关没了 ----
check('页面上没有 Turbo 开关元素', !h.document.getElementById('zb-turbo'));
check('页面 HTML 不含 "Turbo"', !/turbo/i.test(html));
check('API 层没有 turboToggle', typeof h.API.turboToggle === 'undefined');
check('没有向 /zigbee/turbo 发过请求', !calls.some(c => /zigbee\/turbo/.test(c.p)));

// ---- 2. 收到 zigbee_tx_power 也不渲染 ----
h.ZigbeePage._updateFromWS({ zigbee_tx_power: 20 });
const allText = [...h.els.values()].map(e => e.textContent + ' ' + e._html).join('\n');
check('status 带 zigbee_tx_power:20 时页面无 "20 dBm" 之类的功率读数',
      !/\b(20|8|9) dBm (Turbo|Normal)/.test(allText), (allText.match(/.{0,30}dBm (Turbo|Normal).{0,30}/) || [''])[0]);

// ---- 3. 片段：只剩 Z2M 的（ZHA 现在自动，docs/14）----
const z2m = h.ZigbeePage.SNIPPET_Z2M;
check('ZHA 片段已删（ZHA 不再需要粘贴任何东西）', typeof h.ZigbeePage.SNIPPET_ZHA === 'undefined' && !h.document.getElementById('zb-txp-zha'));
check('Z2M 片段在页面里', !!h.document.getElementById('zb-txp-z2m') && html.includes(z2m));
check('Z2M 片段：advanced.transmit_power = 19（射频最高档 = FCC 上限，芯片按地区封顶）', /^advanced:\n  transmit_power: 19/.test(z2m), JSON.stringify(z2m));
check('Z2M 片段带 EU/UK 的值', /EU\/UK: 10/.test(z2m));
check('片段里没有 HTML 标签（用户是原样粘贴的）', !/<[a-z]/i.test(z2m));
check('页面不再教 ZHA 填 tx_power', !/tx_power: 2/.test(html));

// ---- 4. 地区限值与快照表 ----
check('页面写明 FCC 与 EN 300 328 两套限值', /FCC 15\.247/.test(html) && /EN 300 328/.test(html));
check('FCC 那行写的是 19 dBm（不再是 20）', /FCC 15\.247[\s\S]{0,120}<b>19 dBm<\/b>/.test(html));
check('页面说明这是快照、协调器连着时读不到', /snapshot the gateway takes/.test(html));
const tbl = el(h, 'zb-txp-table')._html || el(h, 'zb-txp-table').innerHTML;
check('快照表：网关地区 = 回包 gw_region + 等级', /EU \(EU \/ ETSI \(10 dBm\)\)/.test(tbl), tbl);
check('快照表：HA 国家 = 回包 host_region + 等级', /US \(US \/ Canada \(19 dBm\)\)/.test(tbl), tbl);
check('快照表：生效上限 = 回包 cap_dbm', /<b>10 dBm<\/b>/.test(tbl), tbl);
check('快照表：协调器请求值 = 回包 requested_dbm', /8 dBm/.test(tbl), tbl);
check('快照表：封顶次数 = 回包 clamped_count', /<td>4<\/td>/.test(tbl), tbl);
check('快照提示带 read_uptime_s', /Snapshot taken 123 s after boot/.test(el(h, 'zb-txp-hint').textContent), el(h, 'zb-txp-hint').textContent);

// T9.31 ②：C5 单独重启时没重读芯片，快照是上次开机的 —— 不能再说"开机后第 N 秒"
h.ZigbeePage._renderTxPower({ tx_power: Object.assign({}, zbStatus.tx_power, { cached: true, read_uptime_s: 0 }) });
check('沿用的快照：提示说明是上次开机的', /earlier start/.test(el(h, 'zb-txp-hint').textContent), el(h, 'zb-txp-hint').textContent);
check('沿用的快照：不说"开机后第 0 秒"', !/Snapshot taken 0 s/.test(el(h, 'zb-txp-hint').textContent));

// 旧 NCP 固件：tx_power.ncp_xncp=false → 提示去升级，别显示假数字
h.ZigbeePage._renderTxPower({ tx_power: { ncp_xncp: false, read_uptime_s: 5 } });
check('NCP 无 XNCP 时表里提示不支持', /does not support the region cap/.test(el(h, 'zb-txp-table')._html));
check('NCP 无 XNCP 时提示去升级页', /Update the Zigbee firmware/.test(el(h, 'zb-txp-hint').textContent));
// 旧 C5 固件：/zigbee/status 根本没有 tx_power → unknown，不崩
h.ZigbeePage._renderTxPower({});
check('回包没有 tx_power 时显示 unknown', /unknown/.test(el(h, 'zb-txp-table')._html));

// ---- 5. 复制按钮 ----
const b2 = el(h, 'zb-txp-z2m-copy');
check('Z2M 复制按钮已接线', typeof b2.onclick === 'function');
if (typeof b2.onclick === 'function') { b2.onclick(); await settle(); }
check('复制的是 Z2M 片段原文', h.copied[0] === z2m, JSON.stringify(h.copied[0]));

// ---- 6. 声明值的出路（2026-09-15）----
// 声明一次之后 channel_source 永远是 "declared"，_renderSetupHint 里那个输入框
// 从此不再出现。在此之前那就是死路：PAN 被重建时只报 not_found，而 not_found
// 没有"更新为 CH n"按钮（固件不知道它搬去哪了），用户改不了也没法让它重测。
h.ZigbeePage._chEditing = false;
h.ZigbeePage._renderCheck({ zigbee_channel: 20,
    zigbee_channel_check: { state: 'not_found', pan_id: '0xDB76', declared: 20, detected: null, rssi: 0 } });
check('not_found 时有重新声明的出路', !!h.document.getElementById('zb-ch-redecl'),
      el(h, 'zb-ch-check')._html);
check('not_found 的按钮是「重新声明」', /Re-declare/.test(el(h, 'zb-ch-check')._html));

h.ZigbeePage._renderCheck({ zigbee_channel: 20,
    zigbee_channel_check: { state: 'ok', pan_id: '0xDB76', declared: 20, detected: null, rssi: -20 } });
check('ok 时按钮是「重新测一次」', /Re-check/.test(el(h, 'zb-ch-check')._html));

// 还没声明时不出按钮 —— 那种情况下 _renderSetupHint 的输入框在，两个入口会打架
h.ZigbeePage._renderCheck({ zigbee_channel: null, zigbee_channel_check: null });
check('未声明时不出这个按钮', !h.document.getElementById('zb-ch-redecl'));

// 点开 → 预填当前值 → 保存 → 真的 POST 出去（而不是只改了页面上的字）
h.ZigbeePage._renderCheck({ zigbee_channel: 20,
    zigbee_channel_check: { state: 'not_found', pan_id: '0xDB76', declared: 20, detected: null, rssi: 0 } });
el(h, 'zb-ch-redecl').onclick(); await settle();
check('点开后有输入框', !!h.document.getElementById('zb-ch-re'));
check('输入框预填当前声明值', el(h, 'zb-ch-re').value == 20, String(el(h, 'zb-ch-re').value));
check('编辑期间不会被刷新覆盖', h.ZigbeePage._chEditing === true);
const before = calls.length;
el(h, 'zb-ch-re').value = '15';
await el(h, 'zb-ch-re-save').onclick(); await settle();
const posted = calls.slice(before).find(c => c.p === '/api/v1/zigbee/channel' && c.method === 'POST');
check('保存真的 POST 了 /api/v1/zigbee/channel', !!posted, JSON.stringify(calls.slice(before)));
check('POST 带的是输入的信道', !!posted && posted.body && JSON.parse(posted.body).channel === 15,
      posted && posted.body);
check('保存后退出编辑态（否则后续刷新永远被挡）', h.ZigbeePage._chEditing === false);

// 超范围不能发出去
h.ZigbeePage._renderCheck({ zigbee_channel: 20,
    zigbee_channel_check: { state: 'not_found', pan_id: '0xDB76', declared: 20, detected: null, rssi: 0 } });
el(h, 'zb-ch-redecl').onclick(); await settle();
const before2 = calls.length;
el(h, 'zb-ch-re').value = '99';
await el(h, 'zb-ch-re-save').onclick(); await settle();
check('信道超范围时不发请求',
      !calls.slice(before2).some(c => c.p === '/api/v1/zigbee/channel' && c.method === 'POST'));

// 不许再建议给网关做 DHCP 保留（2026-10-22）：T9.37 起设备地址在网线和 WiFi 之间双向浮动，保留按网卡绑，
// 会把地址钉在一张网卡上 —— 手册第四节「部署建议」10-07 起就是"别做"，而这一页和设置页一直还在教人做（用户提的）。
// 只认英文原文（页面源码不许有中文，译文在 zh.js），设置页一并查。
{
    const pages = ['js/pages/zigbee.js', 'js/pages/settings.js'].map(f => fs.readFileSync(f, 'utf8')).join('\n');
    check('页面不再建议做 DHCP 保留', !/(?:add|make)\s+DHCP reservations?/i.test(pages));
    check('Zigbee 页明说别做 DHCP 保留', /Don\\'t add a DHCP reservation for the gateway/.test(fs.readFileSync('js/pages/zigbee.js', 'utf8')));
    check('Zigbee 页不再列两张网卡的 MAC（它们只为保留服务）', !/zb-conn-e?mac/.test(fs.readFileSync('js/pages/zigbee.js', 'utf8')));
    check('Zigbee 页不再叫人填"以太网 IP"', !/use the Ethernet IP/i.test(pages));
}

console.log('');
console.log(`Result: ${PASS} passed, ${FAIL} failed`);
process.exit(FAIL === 0 ? 0 : 1);
})();
HARNESS

node "$RUN"
