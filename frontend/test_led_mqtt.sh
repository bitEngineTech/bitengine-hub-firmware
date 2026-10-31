#!/bin/bash
# frontend/test_led_mqtt.sh —— LED 页 / MQTT+Webhook 页的**行为**断言（不用板子，只要 node）
#
# 为什么要有这个文件：这两页 2026-08-17 才补上，而它们背后的端点从 2026-08-12
# 起就没有任何 UI 调用点 —— 本仓库反复栽的正是这个跟头（NTP 改了不生效、
# 夜间模式时间窗从没起过作用、LED color_* 存了永不读）。新加的页面如果只做到
# "能渲染"，下一次它就会以另一种形式复发：**发出去的请求语义错了，UI 却报成功**。
#
# 所以这里断言的不是"有没有这个控件"，而是：
#   · 显示的值确实来自设备响应（不是写死的占位值）
#   · 点保存后**真正发出去的 JSON**符合固件契约
#
# 三条最容易错、也最伤人的契约（都在断言里钉死了）：
#   1. MQTT 密码框留空 = **不发 password 字段**（保持原值）。发 "" 是"清空密码"。
#   2. 数字框留空 = **不发该字段**。发 NaN 会被 JSON.stringify 变成 null，
#      固件 json_opt_int 判类型不通过 → 整条 400，用户看到的是"改端口失败"。
#   3. LED 夜间时间窗按**本地时间**算；时区没声明过必须出提示条 ——
#      否则用户在 UTC 基准上设 22:00，实际关灯是本地下午 3 点。
#
# 复用 test_login.sh 的最小 DOM 桩思路，同样的局限也照搬（见那个文件的说明）：
# 不解析 HTML、不执行内联 onclick、不套 CSS。

cd "$(dirname "$0")"
command -v node >/dev/null || { echo "需要 node"; exit 1; }

RUN=$(mktemp /tmp/ledmqtt.XXXX.cjs)
trap 'rm -f "$RUN"' EXIT

cat > "$RUN" << 'HARNESS'
const fs = require('fs');

const FILES = ['js/i18n.js', 'js/i18n/de.js', 'js/i18n/zh.js',
               'js/api.js', 'js/ws.js', 'js/app.js',
               'js/pages/led.js', 'js/pages/mqtt_webhook.js'];
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
            style: {}, _html: '', _listeners: {},
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
    const scan = (html, o) => {
        for (const m of String(html).matchAll(/id="([^"]+)"/g))
            if (!els.has(m[1])) mkEl(m[1], o);
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

    /* setInterval 记下来但不真跑：MqttWebhookPage 会挂一个 10s 的连接状态轮询，
       真跑起来 node 就不退出了。同时也让我们能断言 destroy() 有没有清掉它。 */
    const timers = [];
    const setInterval  = (fn, ms) => { timers.push({ fn, ms, cleared: false }); return timers.length - 1; };
    const clearInterval = (h) => { if (timers[h]) timers[h].cleared = true; };

    const factory = new Function(
        'window', 'document', 'location', 'fetch', 'WebSocket',
        'setTimeout', 'clearTimeout', 'setInterval', 'clearInterval', 'console', 'TextEncoder',
        SRC + '\n; return { App, API, LedPage, MqttWebhookPage };');
    const ctx = factory(window, document, location, fetchImpl, WebSocket,
                        setTimeout, clearTimeout, setInterval, clearInterval, console, TextEncoder);
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

const settle = () => new Promise(r => setTimeout(r, 0));

// ===========================================================================
(async () => {

// ---- 固件端点桩。字段名与 rest_api.c 的回包逐字对齐 ----
const mkFetch = (state) => async (p, opts) => {
    const method = (opts && opts.method) || 'GET';
    state.calls.push({ p, method, body: opts && opts.body ? JSON.parse(opts.body) : null });

    if (p === '/api/v1/mqtt/status')   return reply(200, state.mqtt);
    if (p === '/api/v1/mqtt/config')   { Object.assign(state.mqtt, { enabled: true }); return reply(200, state.mqtt); }
    if (p === '/api/v1/webhook/config') return reply(200, state.webhook);
    if (p === '/api/v1/led/config')    return reply(200, state.led);
    if (p === '/api/v1/ntp')           return reply(200, state.ntp);
    return reply(404, { error: 'not_found' });
};

const freshState = () => ({
    calls: [],
    mqtt: { enabled: true, connected: true, broker: 'mq.lan', port: 8883,
            username: 'hub', password_set: true, topic_prefix: 'bitengine',
            publish_interval: 45, tls_enabled: true },
    webhook: { status: 'ok', enabled: true, url: 'https://hook.example/x',
               backup_interval: 3600, event_zigbee_crash: true, event_network_down: false,
               event_interference: true, event_ota: false, event_temp_high: true,
               event_memory_low: false },
    led: { status: 'ok', brightness: 200, night_mode_enabled: true,
           night_start: '23:30', night_end: '06:15', night_active: true,
           colors_configurable: false },
    ntp: { server: 'pool.ntp.org', timezone: 'PST8PDT,M3.2.0,M11.1.0',
           sync_interval: 3600, tz_declared: true, synced: true,
           local_time: '2026-08-17 09:12:00' },
});

// ======================= 1. 页面注册 =======================
{
    const st = freshState(), h = makeHarness(mkFetch(st));
    check('led 页已注册到 App.pages',  !!h.App.pages['led']);
    check('mqtt 页已注册到 App.pages', !!h.App.pages['mqtt']);
}

// ======================= 2. MQTT：显示的值来自设备 =======================
{
    const st = freshState(), h = makeHarness(mkFetch(st));
    h.MqttWebhookPage.render(el(h, 'content'));
    await settle(); await settle();

    check('MQTT broker 来自设备响应',  el(h,'mq-broker').value === 'mq.lan',
          `实际 "${el(h,'mq-broker').value}"`);
    check('MQTT port 来自设备响应',    String(el(h,'mq-port').value) === '8883');
    check('MQTT username 来自设备响应', el(h,'mq-user').value === 'hub');
    check('MQTT topic_prefix 来自设备', el(h,'mq-prefix').value === 'bitengine');
    check('MQTT publish_interval 来自设备', String(el(h,'mq-int').value) === '45');
    check('MQTT tls_enabled 来自设备', el(h,'mq-tls').checked === true);
    check('MQTT enabled 来自设备',     el(h,'mq-en').checked === true);

    // 密码永不回显 —— 固件只给 password_set 布尔，输入框必须是空的
    check('MQTT 密码框不回显密码', el(h,'mq-pass').value === '');
    check('已设密码时给出「留空=不改」提示',
          /blank to keep/.test(el(h,'mq-passhint').textContent),
          `实际 "${el(h,'mq-passhint').textContent}"`);

    check('MQTT 连接状态显示 Connected', /Connected/.test(el(h,'mq-conn').innerHTML));

    // Webhook 侧
    check('Webhook url 来自设备',      el(h,'wh-url').value === 'https://hook.example/x');
    check('Webhook backup_interval 来自设备', String(el(h,'wh-backup').value) === '3600');
    check('Webhook 事件开关逐个回填（真）', el(h,'wh-event_zigbee_crash').checked === true);
    check('Webhook 事件开关逐个回填（假）', el(h,'wh-event_network_down').checked === false);
    check('六个事件开关都在',
          ['event_zigbee_crash','event_network_down','event_interference',
           'event_ota','event_temp_high','event_memory_low']
          .every(k => h.document.getElementById('wh-' + k)));
}

// ======================= 3. MQTT 保存的请求体语义 =======================
{
    const st = freshState(), h = makeHarness(mkFetch(st));
    h.MqttWebhookPage.render(el(h, 'content'));
    await settle(); await settle();

    // 场景 A：只改端口，密码框不动
    el(h,'mq-port').value = '1883';
    st.calls.length = 0;
    await h.MqttWebhookPage._saveMqtt();
    const a = st.calls.find(c => c.p === '/api/v1/mqtt/config' && c.method === 'POST');
    check('保存 MQTT 发出 POST', !!a);
    check('密码框留空 → 请求体里没有 password 字段（否则等于清空密码）',
          a && !('password' in a.body), a && JSON.stringify(a.body));
    check('改端口真的发出去了', a && a.body.port === 1883);
    check('未改的字段仍原样带上（部分更新也要自洽）',
          a && a.body.broker === 'mq.lan' && a.body.topic_prefix === 'bitengine');

    // 场景 B：填了密码
    el(h,'mq-pass').value = 's3cret';
    st.calls.length = 0;
    await h.MqttWebhookPage._saveMqtt();
    const b = st.calls.find(c => c.p === '/api/v1/mqtt/config' && c.method === 'POST');
    check('填了密码 → 请求体带 password', b && b.body.password === 's3cret');
    check('保存后清空密码框（不把明文留在 DOM 里）', el(h,'mq-pass').value === '');

    // 场景 C：数字框被清空 —— 绝不能发 NaN/null
    el(h,'mq-port').value = '';
    el(h,'mq-int').value  = '';
    st.calls.length = 0;
    await h.MqttWebhookPage._saveMqtt();
    const c = st.calls.find(c => c.p === '/api/v1/mqtt/config' && c.method === 'POST');
    check('端口框清空 → 不发 port 字段（发 null 会让整条请求 400）',
          c && !('port' in c.body), c && JSON.stringify(c.body));
    check('间隔框清空 → 不发 publish_interval 字段',
          c && !('publish_interval' in c.body));

    // 场景 D：勾了「清除已存密码」—— 这是**唯一**能把 password:"" 发出去的路径。
    // 之前的提示文案说"输入一个空格再删掉"，那仍然是空字串、仍然不发字段，
    // 也就是从页面上根本清不掉密码（2026-09-05 浏览器巡检发现）。
    el(h,'mq-pass').value = '';
    el(h,'mq-clearpw').checked = true;
    st.calls.length = 0;
    await h.MqttWebhookPage._saveMqtt();
    const d = st.calls.find(c => c.p === '/api/v1/mqtt/config' && c.method === 'POST');
    check('勾选清除 → 请求体 password 为空字串（固件语义：清空）',
          d && d.body.password === '', d && JSON.stringify(d.body));
    check('请求体里没有任何 null/NaN',
          c && !Object.values(c.body).some(v => v === null || Number.isNaN(v)),
          c && JSON.stringify(c.body));
}

// ======================= 4. LED：显示的值来自设备 =======================
{
    const st = freshState(), h = makeHarness(mkFetch(st));
    h.LedPage.render(el(h, 'content'));
    await settle(); await settle();

    check('LED brightness 来自设备',  String(el(h,'led-bright').value) === '200');
    check('亮度数值同步显示',         String(el(h,'led-bright-val').textContent) === '200');
    check('夜间模式开关来自设备',     el(h,'led-night-en').checked === true);
    check('夜间起始时间来自设备',     el(h,'led-night-start').value === '23:30',
          `实际 "${el(h,'led-night-start').value}"`);
    check('夜间结束时间来自设备',     el(h,'led-night-end').value === '06:15');
    check('night_active shown truthfully',
          /Currently in the night window/.test(el(h,'led-night-active').innerHTML));
    check('显示设备本地时间（时区错没错一眼能看出）',
          el(h,'led-localtime').textContent === '2026-08-17 09:12:00');

    // colors_configurable:false → 绝不能渲染颜色选择器
    const html = el(h,'content').innerHTML;
    check('不渲染颜色选择器（color_* 字段零读取，接了也不生效）',
          !/type="color"/.test(html) && !/id="led-color/.test(html));
}

// ======================= 5. LED：时区没声明过必须警告 =======================
{
    const st = freshState();
    st.ntp.tz_declared = false;
    const h = makeHarness(mkFetch(st));
    h.LedPage.render(el(h, 'content'));
    await settle(); await settle();
    check('tz_declared=false -> banner at the top of the page',
          /factory default/.test(el(h,'led-tzwarn').innerHTML),
          `actual "${el(h,'led-tzwarn').innerHTML.slice(0,60)}"`);
    check('提示条指向 Settings 的时区设置',
          /#settings/.test(el(h,'led-tzwarn').innerHTML));
}
{
    const st = freshState();   // tz_declared = true
    const h = makeHarness(mkFetch(st));
    h.LedPage.render(el(h, 'content'));
    await settle(); await settle();
    check('tz_declared=true → 不打扰用户（无提示条）',
          el(h,'led-tzwarn').innerHTML === '');
}
{
    const st = freshState();
    st.ntp.synced = false;
    const h = makeHarness(mkFetch(st));
    h.LedPage.render(el(h, 'content'));
    await settle(); await settle();
    check('NTP unsynchronized -> says outright that the scheduler will not run night mode',
          /not synchronized/i.test(el(h,'led-synced').innerHTML));
}

// ======================= 6. LED 保存的请求体语义 =======================
{
    const st = freshState(), h = makeHarness(mkFetch(st));
    h.LedPage.render(el(h, 'content'));
    await settle(); await settle();

    el(h,'led-bright').value = '64';
    el(h,'led-night-start').value = '22:05';
    el(h,'led-night-end').value   = '07:30';
    st.calls.length = 0;
    await h.LedPage._save();

    const p = st.calls.find(c => c.p === '/api/v1/led/config' && c.method === 'POST');
    check('保存 LED 发出 POST', !!p);
    check('时间窗被拆成固件要的 hour/min 四个整数',
          p && p.body.night_start_hour === 22 && p.body.night_start_min === 5 &&
               p.body.night_end_hour === 7   && p.body.night_end_min === 30,
          p && JSON.stringify(p.body));
    check('亮度作为整数发出（不是字符串）',
          p && p.body.brightness === 64 && typeof p.body.brightness === 'number');
    check('夜间模式开关一起发出', p && typeof p.body.night_mode_enabled === 'boolean');
}

// ======================= 7. 生命周期：轮询必须被 destroy 清掉 =======================
{
    const st = freshState(), h = makeHarness(mkFetch(st));
    h.MqttWebhookPage.render(el(h, 'content'));
    await settle();
    check('MQTT 页挂了连接状态轮询', h.timers.length === 1);
    h.MqttWebhookPage.destroy();
    check('destroy() 清掉了轮询（否则切走后仍每 10s 打一次接口）',
          h.timers.length === 1 && h.timers[0].cleared === true);
}

console.log(`\nResult: ${PASS} passed, ${FAIL} failed`);
process.exit(FAIL === 0 ? 0 : 1);
})();
HARNESS

node "$RUN"
RC=$?
echo ""
[ $RC -eq 0 ] && echo "全部通过" || echo "有失败项"
exit $RC
