#!/bin/bash
# frontend/test_device_info.sh —— 「设备信息」里的序列号确实来自设备（不用板子，只要 node）
#
# 为什么要有这个文件（2026-09-13，交接 #16）：序列号 2026-09-11 起由固件从 fctry 分区
# 读出、在 /api/v1/status 里以 serial_number 上报，但前端**零显示**了两天 —— 用户手册
# 因此一直没法写"报修时怎么查序列号"。显示一个字段是一行代码，而"显示的值确实来自
# 设备、字段名没拼错、没身份时不编一个出来"正是 test_led_mqtt.sh 那类测试存在的理由。
#
# 钉住三件事：
#   1. Dashboard 与设置页都有序列号卡片，值 = /status 的 serial_number
#   2. 设备没写身份（serial_number: null）时显示 "--"，不显示 undefined/null
#   3. 两页用的是同一个字段名 serial_number（fctry 落地时定的契约，别各写各的）
#
# DOM 桩照抄 test_zigbee.sh，同样的局限：不解析 HTML、不执行内联 onclick。

cd "$(dirname "$0")"
command -v node >/dev/null || { echo "需要 node"; exit 1; }

RUN=$(mktemp /tmp/devinfotest.XXXX.cjs)
trap 'rm -f "$RUN"' EXIT

cat > "$RUN" << 'HARNESS'
const fs = require('fs');
const FILES = ['js/i18n.js', 'js/i18n/de.js', 'js/i18n/zh.js',
               'js/api.js', 'js/tz.js', 'js/ws.js', 'js/app.js',
               'js/pages/dashboard.js', 'js/pages/settings.js'];
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
    /* 定时器 id 从 1 起：浏览器也是从 1 起，dashboard.js 用 `if (this._threadTimer)`
       判有没有定时器，id 为 0 会被当成"没有"，桩就会制造一个假红。 */
    const timers = [];
    const setInterval  = (fn, ms) => { timers.push({ fn, ms, cleared: false }); return timers.length; };
    const clearInterval = (id) => { if (timers[id - 1]) timers[id - 1].cleared = true; };
    const Intl_ = Intl;

    const factory = new Function(
        'window', 'document', 'location', 'navigator', 'fetch', 'WebSocket',
        'setTimeout', 'clearTimeout', 'setInterval', 'clearInterval', 'console', 'TextEncoder', 'Intl',
        SRC + '\n; return { App, API, WS, Dashboard, SettingsPage };');
    const ctx = factory(window, document, location, navigator, fetchImpl, WebSocket,
                        setTimeout, clearTimeout, setInterval, clearInterval, console, TextEncoder, Intl_);
    return Object.assign(ctx, { document, els, timers });
}

const reply = (status, body) => ({ ok: status >= 200 && status < 300, status, json: async () => body });
let PASS = 0, FAIL = 0;
const check = (name, cond, detail) => {
    if (cond) { console.log(`✅ ${name}`); PASS++; }
    else { console.log(`❌ ${name}${detail ? ' — ' + detail : ''}`); FAIL++; }
};
const el = (h, id) => h.document.getElementById(id) ||
    { value: '', textContent: '(元素不存在)', checked: false, style: {}, innerHTML: '(元素不存在)' };
const settle = async () => { for (let i = 0; i < 6; i++) await new Promise(r => setTimeout(r, 0)); };
process.on('unhandledRejection', () => { /* 其他卡片的 404 桩会让无关代码路径抛错，与本测试无关 */ });

const baseStatus = {
    uptime: 4242, temperature: 41.5, free_heap: 100000, fw_version: 'v0.5.0-test', chip_revision: 'v1.0',
    hostname: 'bitengine', www_version: 'abc-def', mg24_alive: true, zigbee_channel: 20,
    active_interface: 'eth', eth_ip: '192.168.1.45', wifi_ip: '192.168.1.43', wifi_connected: true,
    eth_link: true, mac: 'aa:bb', bridge_mode: 'lan', tcp_clients: 1, mg24_resets: 0,
    uart_rx_bytes: 1, uart_tx_bytes: 1, poe_present: true, nvs_write_failures: 0, safe_mode: false,
};
const mkFetch = (status) => async (p) => {
    if (p === '/api/v1/status') return reply(200, status);
    return reply(404, { error: 'not_found' });
};

(async () => { try {
    // ---- 1. 有身份的设备：两页都显示同一个序列号 ----
    let h = makeHarness(mkFetch({ ...baseStatus, serial_number: 'BEH-000001' }));
    h.Dashboard.render(el(h, 'content')); await settle();
    check('Dashboard 有序列号卡片', !!h.document.getElementById('v-sn'));
    check('Dashboard 序列号 = /status.serial_number', el(h, 'v-sn').textContent === 'BEH-000001', `实际 ${JSON.stringify(el(h, 'v-sn').textContent)}`);
    h.SettingsPage.render(el(h, 'content')); await settle();
    check('设置页有序列号卡片', !!h.document.getElementById('set-sn'));
    check('设置页序列号 = /status.serial_number', el(h, 'set-sn').textContent === 'BEH-000001', `实际 ${JSON.stringify(el(h, 'set-sn').textContent)}`);
    check('设置页固件版本仍来自 /status（没把别的卡片挤坏）', el(h, 'set-fw').textContent === 'v0.5.0-test');
    // MAC 跟序列号放在同一张卡片里：没写身份时手册让报 MAC（第十二节）。2026-10-22 之前它只在 Zigbee 页
    // "给两张网卡做 DHCP 保留"那句话底下，那句话撤掉后挪到这里。值 = /status.mac（产线台账与自检报的同一个）。
    check('设置页 MAC = /status.mac', el(h, 'set-mac').textContent === 'aa:bb', `实际 ${JSON.stringify(el(h, 'set-mac').textContent)}`);
    h.Dashboard.render(el(h, 'content')); await settle();
    check('Dashboard MAC = /status.mac', el(h, 'v-mac').textContent === 'aa:bb', `实际 ${JSON.stringify(el(h, 'v-mac').textContent)}`);

    // ---- 2. 没写身份的设备：显示 "--"，不是 null/undefined ----
    h = makeHarness(mkFetch({ ...baseStatus, serial_number: null }));
    h.Dashboard.render(el(h, 'content')); await settle();
    check('无身份时 Dashboard 显示 --', el(h, 'v-sn').textContent === '--', `实际 ${JSON.stringify(el(h, 'v-sn').textContent)}`);
    h.SettingsPage.render(el(h, 'content')); await settle();
    check('无身份时设置页显示 --', el(h, 'set-sn').textContent === '--', `实际 ${JSON.stringify(el(h, 'set-sn').textContent)}`);
    check('无身份时设置页照样有 MAC 可报', el(h, 'set-mac').textContent === 'aa:bb');

    // ---- 3. 字段名契约：两页只认 serial_number，不认别的拼法 ----
    h = makeHarness(mkFetch({ ...baseStatus, serialNumber: 'WRONG-0001', sn: 'WRONG-0002' }));
    h.Dashboard.render(el(h, 'content')); await settle();
    check('字段名拼错时不会显示出来（契约是 serial_number）', el(h, 'v-sn').textContent === '--');

    // ---- 4. Dashboard 的 Thread 轮询不能留孤儿定时器 ----
    //   _loadThread() 先 await /node 再 setInterval。进主界面时 Dashboard 会连着 render
    //   两次（navigate 写 hash → onhashchange 再 navigate），第一次的 await 还没回来就被
    //   destroy，回来后照样挂上定时器，此后每 15 秒打一次 /node，标签退到登录页也不停
    //   （2026-09-13 反代日志里看到的）。
    const withNode = (status) => async (p) => {
        if (p === '/node') return reply(200, { State: 'leader', NetworkName: 'ot', NumOfRouter: 1 });
        return mkFetch(status)(p);
    };
    h = makeHarness(withNode(baseStatus));
    h.Dashboard.render(el(h, 'content'));          // 第一次 render：_loadThread 停在 await
    h.Dashboard.destroy();                         // hashchange 触发的第二次 navigate 先 destroy
    h.Dashboard.render(el(h, 'content'));          // 再 render
    await settle();
    const live = () => h.timers.filter(t => !t.cleared).length;
    check('render→destroy→render 后只剩一个 Thread 轮询定时器', live() === 1, `live=${live()} total=${h.timers.length}`);
    h.Dashboard.destroy(); await settle();
    check('destroy 后没有活着的 Thread 轮询定时器', live() === 0, `live=${live()} total=${h.timers.length}`);
    h = makeHarness(withNode(baseStatus));
    h.Dashboard.render(el(h, 'content'));
    h.Dashboard.destroy();                         // await 期间被 destroy，之后不再 render
    await settle();
    check('await 期间被 destroy → 回来后不挂定时器', h.timers.length === 0 || live() === 0, `live=${live()} total=${h.timers.length}`);

    // ---- 无协调器横幅（2026-09-15）----
    // mg24_alive 全程 true 时也必须能看出"Zigbee 实际停服" —— 2026-08 那次
    // 8 小时停服没被发现，正是因为唯一的指示器回答的是另一个问题（芯片在不在）。
    h = makeHarness(mkFetch(baseStatus));
    h.Dashboard.render(el(h, 'content')); await settle();
    const hostWarn = () => el(h, 'zb-host-warn');
    check('有协调器时不出横幅', hostWarn().style.display === 'none' || !hostWarn()._html,
          JSON.stringify(hostWarn().style.display));

    h.Dashboard._renderZbHost({ ...baseStatus, tcp_clients: 0, zigbee_host_absent_s: 120 });
    check('断开 2 分钟不报（ZHA 正常重连的时间窗）', hostWarn().style.display === 'none');

    h.Dashboard._renderZbHost({ ...baseStatus, tcp_clients: 0, zigbee_host_absent_s: 900 });
    check('断开 15 分钟出横幅', hostWarn().style.display === '', JSON.stringify(hostWarn().style.display));
    check('横幅报的是分钟数', /15 minutes/.test(hostWarn()._html), hostWarn()._html);
    check('横幅点明 HA 里设备不可用', /devices stay unavailable in Home Assistant/.test(hostWarn()._html));
    // 2026-09-15 订正：曾写成"射频芯片 idle"，实测无协调器时协调器照样发信标，
    // 所以文案只能说"HA 够不着"，不能说"射频停了"。
    check('横幅不声称射频停了', !/radio chip is idle|network is down/.test(hostWarn()._html));
    // 老固件没有这个字段时必须原样不动，不能当成 0 而误判"一切正常"
    h.Dashboard._renderZbHost({ ...baseStatus, tcp_clients: 0, zigbee_host_absent_s: 900 });
    h.Dashboard._renderZbHost({ ...baseStatus });   // 字段缺失
    check('老固件缺字段时不改动已显示的横幅', hostWarn().style.display === '');
    h.Dashboard._renderZbHost({ ...baseStatus, zigbee_host_absent_s: 0 });
    check('协调器回来后横幅消失', hostWarn().style.display === 'none');

    // null = 这台网关从来没有过协调器（只用 Thread/Matter，或还没装 ZHA）。
    // 那是**正常状态**，报一个用户既无法消除也不该消除的警，比不报更糟。
    h.Dashboard._renderZbHost({ ...baseStatus, tcp_clients: 0, zigbee_host_absent_s: 900 });
    check('先让横幅显示出来', hostWarn().style.display === '');
    h.Dashboard._renderZbHost({ ...baseStatus, tcp_clients: 0, zigbee_host_absent_s: null });
    check('从没配过 Zigbee（null）不报警', hostWarn().style.display === 'none');

    console.log(`\nResult: ${PASS} passed, ${FAIL} failed`);
    process.exit(FAIL ? 1 : 0);
} catch (e) { console.log('❌ harness 抛错：' + (e && e.stack || e)); console.log('\nResult: 0 passed, 1 failed'); process.exit(1); } })();
HARNESS

node "$RUN"; RC=$?

# ---- 文档判据（7.6 规矩 3）----
# 横幅阈值写在两处：dashboard.js 的 `secs < 300` 与用户手册第十节的"5 分钟"。
# 代码与文档两侧都验，任一处改了另一处没跟上就红。
M=../docs/guide/Hub使用手册.md
if grep -q 'secs < 300' js/pages/dashboard.js; then
    echo "✅ 横幅阈值在代码里是 300s"
else
    echo "❌ 横幅阈值不再是 300s —— 用户手册第十节写着「超过 5 分钟」，要同步"; RC=1
fi
if grep -q '超过 5 分钟没连上' "$M"; then
    echo "✅ 用户手册写了同一个阈值"
else
    echo "❌ 用户手册第十节没写横幅的阈值（或写法变了）"; RC=1
fi
if grep -q '没有 Zigbee 协调器连接' "$M"; then
    echo "✅ 用户手册收录了这条横幅"
else
    echo "❌ 用户手册没提这条横幅 —— 用户看得见的东西必须在手册里"; RC=1
fi
exit $RC
