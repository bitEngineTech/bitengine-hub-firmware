#!/bin/bash
# frontend/test_thread_backup.sh —— Thread 页「下载备份」导出的确实是能恢复的那种数据集（不用板子，只要 node）
#
# 为什么要有这个文件（2026-10-06）：Thread 页原来只有「复制 JSON」，而那份 JSON 恢复不回去 ——
# 固件对 JSON 格式的 PUT 走 otDatasetCreateNewNetwork()，会重新生成网络密钥，等于新建一张网
# （otbr_api.c handle_dataset_put）。能原样恢复的只有十六进制 TLV（HA、ot-ctl、其他厂商的
# 边界路由都认）。钉住：
#   1. 取备份必须带 Accept: text/plain（不带就拿到 JSON）
#   2. 回包不是合法十六进制时一律当失败，绝不把它当备份下载
#   3. 下载内容恰好是 TLV、文件名带扩展 PAN ID；读取失败不下载
#   4. 隐藏备份字符串时把文本框清空，网络密钥不留在页面里
# 页面上那句"JSON 恢复不回去"依赖的固件事实，在文件末尾的判据里也钉着。
#
# DOM 桩照抄 test_wifi_diag.sh，另加 Blob / URL / document.body 桩。

cd "$(dirname "$0")"
command -v node >/dev/null || { echo "需要 node"; exit 1; }

RUN=$(mktemp /tmp/threadbackuptest.XXXX.cjs)
trap 'rm -f "$RUN"' EXIT

cat > "$RUN" << 'HARNESS'
const fs = require('fs');
const FILES = ['js/i18n.js', 'js/i18n/de.js', 'js/i18n/zh.js',
               'js/api.js', 'js/tz.js', 'js/ws.js', 'js/app.js', 'js/pages/thread.js'];
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
        createElement: (tag) => {
            const e = mkEl('tmp' + Math.random());
            e.tag = tag; e.clicked = 0;
            e.click = () => { e.clicked++; downloads.push({ href: e.href, download: e.download }); };
            e.remove = () => {};
            return e;
        },
        body: { appendChild() {} },
    };
    const downloads = [], blobs = [];
    function Blob(parts, opts) { this.parts = parts; this.type = opts && opts.type; blobs.push(this); }
    const URL = { createObjectURL: (b) => 'blob:' + blobs.indexOf(b), revokeObjectURL() {} };
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
        'Blob', 'URL',
        SRC + '\n; return { App, API, WS, ThreadPage, Lang };');
    const ctx = factory(window, document, location, navigator, fetchImpl, WebSocket,
                        setTimeout, clearTimeout, setInterval, clearInterval, console, TextEncoder, Intl,
                        Blob, URL);
    return Object.assign(ctx, { document, els, timers, downloads, blobs });
}

const reply = (status, body, text) => ({ ok: status >= 200 && status < 300, status,
    json: async () => body, text: async () => (text !== undefined ? text : JSON.stringify(body)) });
let PASS = 0, FAIL = 0;
const check = (name, cond, detail) => {
    if (cond) { console.log(`✅ ${name}`); PASS++; }
    else { console.log(`❌ ${name}${detail ? ' — ' + detail : ''}`); FAIL++; }
};
const el = (h, id) => h.document.getElementById(id) ||
    { value: '', textContent: '(元素不存在)', checked: false, style: {}, innerHTML: '(元素不存在)', _html: '(元素不存在)' };
const settle = async () => { for (let i = 0; i < 8; i++) await new Promise(r => setTimeout(r, 0)); };
process.on('unhandledRejection', () => { /* 其他卡片的 404 桩会让无关代码路径抛错，与本测试无关 */ });

const TLVS = '0e080000000000010000000300000f35060004001fffe0020811111111222222220708fdaa0bbbcccc0000';
const DS_JSON = { Channel: 15, NetworkName: 'OpenThread-x', PanId: 4660, ExtPanId: '1111111122222222',
                  NetworkKey: '00112233445566778899aabbccddeeff', PSKc: 'aa' };
const calls = [];
const mkFetch = (tlvReply) => async (p, init) => {
    calls.push({ p, init });
    if (p === '/node/dataset/active') {
        const acc = init && init.headers && init.headers.Accept;
        if (acc === 'text/plain') return tlvReply();
        return reply(200, DS_JSON);
    }
    return reply(404, { error: 'not_found' });
};
const okTlv = () => reply(200, null, TLVS + '\n');
const page = async (tlvReply) => {
    const h = makeHarness(mkFetch(tlvReply));
    h.ThreadPage.render(el(h, 'content')); await settle();
    return h;
};

(async () => { try {
    // ---- 1. 取 TLV 必须带 Accept: text/plain（不带的话固件回 JSON，那份恢复不回去）----
    let h = await page(okTlv);
    calls.length = 0;
    let got = await h.API.threadDatasetTlvs();
    const c = calls.find(x => x.p === '/node/dataset/active');
    check('取备份打的是 /node/dataset/active', !!c);
    check('取备份带 Accept: text/plain', c && c.init && c.init.headers && c.init.headers.Accept === 'text/plain', JSON.stringify(c && c.init));
    check('返回去掉换行的十六进制 TLV', got === TLVS, JSON.stringify(got));

    // ---- 2. 不是合法十六进制的回包一律不当备份 ----
    h = await page(() => reply(200, null, JSON.stringify(DS_JSON)));
    check('回包是 JSON → 不当备份（null）', (await h.API.threadDatasetTlvs()) === null);
    h = await page(() => reply(200, null, 'abc'));
    check('回包长度为奇数 → 不当备份', (await h.API.threadDatasetTlvs()) === null);
    h = await page(() => reply(200, null, ''));
    check('回包为空 → 不当备份', (await h.API.threadDatasetTlvs()) === null);
    h = await page(() => reply(404, { error: 'no_active_dataset' }));
    check('没有数据集（404）→ null', (await h.API.threadDatasetTlvs()) === null);
    h = await page(() => reply(401, { error: 'unauthorized' }));
    let loginShown = 0; h.App.showLogin = () => { loginShown++; };
    check('会话过期（401）→ null', (await h.API.threadDatasetTlvs()) === null);
    check('会话过期（401）→ 跳登录页', loginShown === 1);

    // ---- 2b. 粘贴给 HA 的 OTBR 地址（T9.37）：有 svc_ip 用它，老固件走原来的探测 ----
    {
        const st = { svc_ip: '192.168.1.36', svc_holder: 'eth' };
        const h1 = makeHarness(async (p, i) => p === '/api/v1/status' ? reply(200, st) : mkFetch(okTlv)(p, i));
        h1.ThreadPage.render(el(h1, 'content')); await settle();
        check('OTBR 地址：有 svc_ip 时就是它（不是浏览器当前地址）', el(h1, 't-otbr-url').textContent === 'http://192.168.1.36',
              el(h1, 't-otbr-url').textContent);
        const h2 = await page(okTlv);     /* 桩对 /api/v1/status 回 404：老固件 */
        check('OTBR 地址：老固件（没有 svc_ip）退回浏览器当前地址', el(h2, 't-otbr-url').textContent === 'http://127.0.0.1',
              el(h2, 't-otbr-url').textContent);
    }

    // ---- 3. 下载备份 ----
    h = await page(okTlv);
    await h.ThreadPage._downloadBackup(); await settle();
    check('点了下载：触发一次下载', h.downloads.length === 1, JSON.stringify(h.downloads));
    check('下载文件内容恰好是 TLV（加一个换行，没有别的）',
          h.blobs.length === 1 && h.blobs[0].parts.join('') === TLVS + '\n', JSON.stringify(h.blobs[0] && h.blobs[0].parts));
    check('文件名带扩展 PAN ID', h.downloads[0] && h.downloads[0].download === 'thread-dataset-1111111122222222.txt',
          JSON.stringify(h.downloads[0]));
    check('提示"备份已下载"', /Backup downloaded/.test(el(h, 't-backup-msg')._html));

    h = await page(() => reply(404, { error: 'no_active_dataset' }));
    await h.ThreadPage._downloadBackup(); await settle();
    check('读不到数据集：不下载任何东西', h.downloads.length === 0 && h.blobs.length === 0);
    check('读不到数据集：报错', /Could not read the dataset/.test(el(h, 't-backup-msg')._html));

    // ---- 4. 显示 / 隐藏备份字符串 ----
    h = await page(okTlv);
    await h.ThreadPage._toggleTlvs(); await settle();
    check('显示：文本框里是 TLV', el(h, 't-tlvs').value === TLVS && el(h, 't-tlvs').style.display === '',
          JSON.stringify({ v: el(h, 't-tlvs').value, d: el(h, 't-tlvs').style.display }));
    check('按钮变成"隐藏"', el(h, 't-tlv-btn').textContent === 'Hide Backup String');
    await h.ThreadPage._toggleTlvs(); await settle();
    check('隐藏：文本框收起且清空（密钥不留在页面里）', el(h, 't-tlvs').value === '' && el(h, 't-tlvs').style.display === 'none');

    console.log(`\nResult: ${PASS} passed, ${FAIL} failed`);
    process.exit(FAIL ? 1 : 0);
} catch (e) { console.log('❌ harness 抛错：' + (e && e.stack || e)); console.log('\nResult: 0 passed, 1 failed'); process.exit(1); } })();
HARNESS

node "$RUN"; RC=$?

# ---- 判据：页面说明依赖的固件事实 + 用户手册（7.6 规矩 3/6）----
D_PASS=0; D_FAIL=0
dcheck() { if eval "$2" >/dev/null 2>&1; then echo "✅ $1"; D_PASS=$((D_PASS+1)); else echo "❌ $1"; D_FAIL=$((D_FAIL+1)); fi; }
dcheck "固件 JSON 格式 PUT 仍是新建网络（页面说「JSON 恢复不回去」的依据）" \
       "awk '/^static esp_err_t handle_dataset_put/{f=1} f&&/^}/{exit} f' ../components/webui/otbr_api.c | python3 ../tools/grep_code.py -q 'otDatasetCreateNewNetwork\\(' /dev/stdin"
dcheck "固件 GET 在 Accept: text/plain 时回 TLV" \
       "awk '/^static esp_err_t handle_dataset_active_get/{f=1} f&&/^}/{exit} f' ../components/webui/otbr_api.c | python3 ../tools/grep_code.py -q 'accepts_text_plain\\(req\\)' /dev/stdin"
dcheck "用户手册写了怎么备份 Thread 网络（文件名与下载的一致）" "grep -q 'thread-dataset-<扩展 PAN ID>.txt' ../docs/guide/Hub使用手册.md && grep -qF 'thread-dataset-\${ext}.txt' js/pages/thread.js"
echo "Docs: $D_PASS passed, $D_FAIL failed"

[ $RC -eq 0 ] && [ $D_FAIL -eq 0 ]
