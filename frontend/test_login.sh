#!/bin/bash
# frontend/test_login.sh —— 验证登录 / 强制设密码流程的接线（不用板子，只要 node）
#
# 为什么要有这个文件：登录页在 2026-08-15 之前根本不存在，而
# `App.showLogin()` 取不到页面时**静默返回**。后果是设了密码就白屏、再也进
# 不去，所以密码功能虽然后端完整却从没人敢开，藏了几个月没人发现。
# grep 类检查对这种"函数在、但什么也不做"天然失明（见 CLAUDE.md 7.5），
# 所以这里真的把 app.js 跑起来，断言**行为**。
#
# 用最小 DOM 桩而不是 jsdom：只需要 innerHTML / getElementById / 事件监听
# 这几样，装个依赖不值当，也免得 CI 上还要 npm install。
#
# ⚠️ 这个桩**不做**的事（下面的断言不要指望它们）：
#   - 不解析 HTML，只用正则从 innerHTML 里扫 id="..."；标签写错照样"存在"
#   - 不执行内联 onclick 属性（所以用 grep 断言属性名 + 函数存在来兜）
#   - 不套 CSS，布局/可见性一概测不到（那部分只能靠人眼或真浏览器）
#   - 不发真请求，fetch 全是桩

cd "$(dirname "$0")"
SHELL_PASS=0; SHELL_FAIL=0

command -v node >/dev/null || { echo "需要 node"; exit 1; }

# ---------- 1. 结构检查：两份脚本清单必须一致，且**顺序一致** ----------
# index.html 是开发时直接打开用的，build.sh 是出货的。两边漏了谁，开发环境
# 就少个功能而构建产物照样是好的（tz.js / logs.js 就这么漏过）。
# 顺序同样要比：build.sh 把所有 js 拼进**一个** <script>，把 login.js 排到
# app.js 前面就是 ReferenceError，后面每个页面的注册全部中断。
BUILD_LIST=$(grep '^for f in' build.sh | grep -o 'js/[^ ]*\.js')
HTML_LIST=$(grep -o 'src="js/[^"]*"' index.html | sed 's/src="//; s/"$//')
if [ "$BUILD_LIST" == "$HTML_LIST" ]; then
    echo "✅ build.sh 与 index.html 的脚本清单一致（含顺序）"; SHELL_PASS=$((SHELL_PASS+1))
else
    echo "❌ build.sh 与 index.html 的脚本清单不一致（含顺序）："
    diff <(echo "$BUILD_LIST") <(echo "$HTML_LIST") | sed 's/^/     /'
    SHELL_FAIL=$((SHELL_FAIL+1))
fi

# 两份清单一致还不够：把 login.js/setup.js 一起排到 app.js 前面，上面那条检查
# 照样绿，而 build.sh 是拼成**一个** <script>，运行时 `App.loginPage = LoginPage`
# 会撞上 const App 的暂时性死区，login.js 当场中断、App.loginPage 永远是 null。
APP_POS=$(echo "$BUILD_LIST" | grep -n '^js/app\.js$' | cut -d: -f1)
BAD_ORDER=""
for p in js/pages/login.js js/pages/setup.js; do
    POS=$(echo "$BUILD_LIST" | grep -n "^$p$" | cut -d: -f1)
    if [ -n "$POS" ] && [ -n "$APP_POS" ] && [ "$POS" -lt "$APP_POS" ]; then
        BAD_ORDER="$BAD_ORDER $p"
    fi
done
if [ -z "$BAD_ORDER" ] && [ -n "$APP_POS" ]; then
    echo "✅ app.js 排在 login.js / setup.js 之前"; SHELL_PASS=$((SHELL_PASS+1))
else
    echo "❌ 脚本顺序错误：$BAD_ORDER 排在 js/app.js 之前（会撞 const App 的暂时性死区）"
    SHELL_FAIL=$((SHELL_FAIL+1))
fi

# ---------- 2. 行为检查 ----------
RUN=$(mktemp /tmp/loginrun.XXXX.cjs)
trap 'rm -f "$RUN"' EXIT

cat > "$RUN" << 'HARNESS'
const fs = require('fs');

/* settings.js 也在列：它是 PwField 的第三个消费者，也是**已有用户改密码**的
   地方。2026-08-15 的复核发现字节上限只加在了新写的两个文件上，偏偏漏了它
   —— 漏的正是最容易把人锁在门外的那一个。 */
const FILES = ['js/i18n.js', 'js/i18n/de.js', 'js/i18n/zh.js',
               'js/api.js', 'js/ws.js', 'js/app.js', 'js/pages/login.js',
               'js/pages/setup.js', 'js/pages/settings.js'];
/* 缺文件是本测试要抓的头号场景（登录页整个不存在过）——
   得报成一条断言失败，不是 ENOENT 崩栈。 */
const missing = FILES.filter(f => !fs.existsSync(f));
if (missing.length) {
    console.log(`❌ 源文件缺失：${missing.join(', ')}`);
    console.log('\nResult: 0 passed, 1 failed');
    process.exit(1);
}
const SRC = FILES.map(f => fs.readFileSync(f, 'utf8')).join('\n');

/* ---- 最小 DOM 桩 ----
   innerHTML 被写入时扫出其中所有 id="..."，登记成可 getElementById 的元素，
   并记下它归谁所有。重写同一个元素的 innerHTML 会**连带移除**它名下的 id
   —— 不这么做的话 "某元素已经不在了" 这类断言全是假的。 */
function makeHarness(fetchImpl, timers) {
    const els   = new Map();
    const owner = new Map();      // id -> 拥有它的元素 id

    const mkEl = (id, ownerId) => {
        const el = {
            id, value: '', type: 'password', textContent: '', disabled: false, checked: false,
            style: {}, _html: '', _listeners: {},
            get innerHTML() { return this._html; },
            set innerHTML(v) { this._html = v; dropOwned(this.id); scan(v, this.id); },
            addEventListener(type, fn) { (this._listeners[type] ||= []).push(fn); },
            focus() {}, setAttribute() {},
            classList: { toggle() {}, add() {}, remove() {} },
        };
        els.set(id, el);
        if (ownerId !== undefined) owner.set(id, ownerId);
        return el;
    };
    const dropOwned = (ownerId) => {
        for (const [id, o] of [...owner]) {
            if (o === ownerId) { dropOwned(id); owner.delete(id); els.delete(id); }
        }
    };
    const scan = (html, ownerId) => {
        for (const m of String(html).matchAll(/id="([^"]+)"/g)) {
            if (!els.has(m[1])) mkEl(m[1], ownerId);
        }
    };
    mkEl('app');                  // 无 owner，永不被移除

    const document = {
        getElementById: (id) => els.get(id) || null,
        querySelector: () => null,
        querySelectorAll: () => [],
    };
    /* 浏览器会把赋给 location.hash 的值补上前导 '#'。不模拟的话
       `location.hash = 'dashboard'` 之后再 slice(1) 会读成 'ashboard'。 */
    const location = {
        _h: '', host: '127.0.0.1',
        get hash() { return this._h; },
        set hash(v) { this._h = v ? (String(v).startsWith('#') ? String(v) : '#' + v) : ''; },
    };
    const window = {};
    const sockets = [];
    const WebSocket = function () {
        this.readyState = 0; this.closed = false;
        this.close = () => { this.closed = true; };
        sockets.push(this);
    };

    const factory = new Function(
        'window', 'document', 'location', 'fetch', 'WebSocket',
        'setTimeout', 'clearTimeout', 'console', 'TextEncoder',
        SRC + '\n; return { App, API, WS, LoginPage, SetupPage, SettingsPage, PwField };');
    /* timers：可注入的 setTimeout/clearTimeout（见 fakeTimers）。ws.js 的退避
       全靠 setTimeout 的延时表达，用真定时器得等 3+6+12… 秒才看得出趋势。 */
    const ctx = factory(window, document, location, fetchImpl, WebSocket,
                        timers ? timers.setTimeout : setTimeout,
                        timers ? timers.clearTimeout : clearTimeout,
                        console, TextEncoder);

    /* 主界面的落点：让 navigate() 真的有东西可渲染。不注册的话
       navigate() 会在 `if (!page) return` 处直接返回，于是所有
       "进主界面" 的断言其实只证明了骨架 HTML 里有个空的 #content。 */
    let destroyed = 0;
    ctx.App.registerPage('dashboard', {
        render(el) { el.innerHTML = '<div id="dash-marker"></div>'; },
        destroy() { destroyed++; },
    });

    const fire = (el, type, ev) => (el._listeners[type] || []).forEach(f => f(ev));
    return Object.assign(ctx, {
        document, location, window, els, sockets, fire,
        destroyCount: () => destroyed,
    });
}

const reply = (status, body) => ({
    ok: status >= 200 && status < 300,
    status,
    json: async () => body,
});

/* 假定时器：记下每次 setTimeout 的延时、以及它有没有被 clearTimeout 掉。
   fire(t) 手动到点 —— 被取消过的不会跑。 */
const fakeTimers = () => {
    const armed = []; let n = 0;
    return {
        armed,
        setTimeout(fn, ms) { const t = { id: ++n, fn, ms, cleared: false }; armed.push(t); return t.id; },
        clearTimeout(id) { const t = armed.find(x => x.id === id); if (t) t.cleared = true; },
        fire(t) { if (t && !t.cleared) t.fn(); },
    };
};
const tick = () => new Promise(r => setImmediate(r));
/* 模拟浏览器眼里"被固件拒掉的握手"：httpd 先回 101（浏览器 onopen），
   ws_post_handshake_cb 返回 ESP_FAIL 后连接被关（onclose），中间一帧都没有。
   readyState 置 3 是为了让下一次 connect() 过得了"已有活连接"的守卫。 */
const rejectHandshake = async (h) => {
    const w = h.WS.socket;
    if (w.onopen) w.onopen();
    w.readyState = 3;
    w.onclose();
    await tick(); await tick();
};

let PASS = 0, FAIL = 0;
const check = (name, cond, detail) => {
    if (cond) { console.log(`✅ ${name}`); PASS++; }
    else      { console.log(`❌ ${name}${detail ? ' — ' + detail : ''}`); FAIL++; }
};

/* 元素取不到时给个哑对象：接线断了的时候，后面的断言应该继续跑完并各报各的，
   而不是在 null.value 上崩掉、把剩下几十条检查一起带走。 */
const el = (h, id) => h.document.getElementById(id) ||
                      { value: '', textContent: '(元素不存在)', style: {}, _listeners: {} };

/* 模拟固件的认证相关端点。状态码照抄 components/webui/auth.c。 */
const stubFetch = (state) => async (p, opts) => {
    state.calls.push(p);
    state.methods.push((opts && opts.method) || 'GET');
    if (p === '/api/v1/auth/status') return reply(200, { password_required: state.hasPw });
    if (p === '/api/v1/auth/password') {
        const np = JSON.parse(opts.body).new_password;
        if (!np || np.length < 4) return reply(400, { error: 'password_too_short' });
        if (state.setPwFails)     return reply(500, { error: 'save_failed' });
        state.hasPw = true; state.pw = np;          // 设完设备就有密码了
        return reply(200, { status: 'ok' });
    }
    if (p === '/api/v1/auth/login') {
        if (state.delayMs) await new Promise(r => setTimeout(r, state.delayMs));
        if (state.loginFails) return reply(500, { error: 'boom' });
        const pw = JSON.parse(opts.body).password;
        if (!state.hasPw || pw === state.pw) { state.authed = true; return reply(200, { status: 'ok' }); }
        return reply(401, { error: 'wrong_password' });
    }
    if (p === '/api/v1/otbr/auth') {
        if (!opts || opts.method !== 'POST') return reply(200, { enabled: !!state.otbrAuth });
        if (state.otbrFails) return reply(500, { error: 'save_failed' });
        const want = JSON.parse(opts.body).enabled;
        if (want && !state.hasPw) return reply(400, { error: 'set_device_password_first' });
        state.otbrAuth = want;
        return reply(200, { enabled: want });
    }
    if (p === '/api/v1/auth/logout') {
        if (!state.hasPw || state.authed) { state.authed = false; return reply(200, { status: 'ok' }); }
        return reply(401, { error: 'login_required' });
    }
    /* ⚠️ 这几个端点在固件里是 AUTH_REQUIRED_UNLESS_OPEN（webui.h）：
       thread.rest_auth 出厂**关**，此时它们**没有凭据也回 200**。
       桩必须照抄这个行为，否则"门禁探针探的是不是一个真受保护的端点"这条判据
       是恒真的 —— 兜底对一切未认证请求回 401，探针路径改成 /status 也照样绿，
       而那正是 2026-08-23 在真浏览器里撞到的 bug（未登录能看到完整 Dashboard，
       一点设置页才被弹回登录）。 */
    if (!state.otbrAuth &&
        ['/api/v1/status', '/api/v1/led/config', '/api2/info', '/api2/state'].includes(p)) {
        return reply(200, { hostname: 'hub' });
    }
    if (!state.hasPw || state.authed) return reply(200, { hostname: 'hub' });
    return reply(401, { error: 'login_required' });
};
const st = (o = {}) => Object.assign(
    { calls: [], methods: [], hasPw: true, pw: 'hunter2', authed: false,
      otbrAuth: false }, o);
const methodOf = (s, path) => s.methods[s.calls.indexOf(path)];
const nLogin = (s) => s.calls.filter(p => p === '/api/v1/auth/login').length;
const nSetPw = (s) => s.calls.filter(p => p === '/api/v1/auth/password').length;

(async () => {

// ══════════════════════ 登录 ══════════════════════

// ===== T1: 受保护端点 401 → 登录表单必须真的出现（原 bug：静默白屏）=====
{
    const s = st(); const h = makeHarness(stubFetch(s));
    await h.App.init();
    check('401 → 登录表单出现在 DOM 里',
          h.App._gate === 'login' && !!h.document.getElementById('login-pw'),
          `gate=${h.App._gate} pwField=${!!h.document.getElementById('login-pw')}`);
    check('401 → 不渲染主界面', !h.document.getElementById('dash-marker'));
    check('登录页的 Sign In 按钮指向真实存在的函数',
          /onclick="LoginPage\._submit\(\)"/.test(h.document.getElementById('app').innerHTML) &&
          typeof h.LoginPage._submit === 'function');
}

// ===== T1c: 门禁探针必须探一个**无条件** AUTH_REQUIRED 的端点 =====
//   原 bug：探的是 /api/v1/status，而它被改成了 AUTH_REQUIRED_UNLESS_OPEN，
//   rest_auth 出厂关 ⇒ 永不 401 ⇒ 设了密码的设备上未登录也能进主界面。
//   上面的桩已经把这批端点建模成"无凭据也 200"，所以探针一旦改回它们，本条即红。
{
    const s = st({ hasPw: true, authed: false, otbrAuth: false });
    const h = makeHarness(stubFetch(s));
    await h.App.init();
    check('设了密码 + 未登录 → 必须停在登录页',
          h.App._gate === 'login', `gate=${h.App._gate}`);
    const probed = s.calls.find(p => p.startsWith('/api/v1/'));
    check('探针不是 UNLESS_OPEN 那批端点',
          !['/api/v1/status', '/api/v1/led/config', '/api2/info', '/api2/state'].includes(probed),
          `probed=${probed}`);
}

// ===== T1b: 启动接线 =====
//   所有用例都直接调 App.init()，所以 window.onload 那行被删掉也全绿 ——
//   而那正是"整个 App 永远不启动"的样子。
{
    const s = st({ authed: true }); const h = makeHarness(stubFetch(s));
    check('window.onload 挂了启动函数', typeof h.window.onload === 'function');
    await h.window.onload();
    check('走 window.onload 也能进主界面', !!h.document.getElementById('dash-marker'));
}

// ===== T2: 已登录（受保护端点 200）→ 直接进主界面 =====
//   即使 /auth/status 说 password_required:true。原来拿它当门禁，
//   登录成功后刷新照样被弹回登录页。
{
    const s = st({ authed: true }); const h = makeHarness(stubFetch(s));
    await h.App.init();
    check('已登录 + 设备有密码 → 进主界面（不看 auth/status）',
          h.App._gate === null && !!h.document.getElementById('dash-marker'),
          `gate=${h.App._gate} marker=${!!h.document.getElementById('dash-marker')}`);
    check('设备有密码 → 显示 Logout', !!h.document.getElementById('logout-btn'));
}

// ===== T3: 密码错不能把表单冲掉 =====
//   后端密码错也发 401（auth.c:236）。login 要是走了 api.js 的 401 拦截器，
//   调用方就只拿到 null，分不清"密码错"和"会话过期"。
{
    const s = st(); const h = makeHarness(stubFetch(s));
    await h.App.init();
    const pwEl = el(h, 'login-pw');
    pwEl.value = 'nope';
    await h.LoginPage._submit();

    /* 断言的是**元素同一性**，不是"有个同名 id 存在"。重画会产生一个新元素，
       存在性检查看不出区别 —— 这条断言曾经因此对该缺陷完全失明。 */
    check('密码错 → 表单是同一个元素（没被重画）',
          h.document.getElementById('login-pw') === pwEl);
    check('密码错 → 显示 "Wrong password"',
          el(h, 'login-msg').textContent === 'Wrong password',
          `msg="${el(h, 'login-msg').textContent}"`);
    check('密码错 → 输入框已清空（不把明文留在 DOM 里）', pwEl.value === '');
    check('密码错 → 按钮重新可用（_busy 没卡住）',
          el(h, 'login-btn').disabled === false && h.LoginPage._busy === false);
}

// ===== T4: 会话过期时多个请求同时 401，登录页只画一次 =====
{
    const s = st(); const h = makeHarness(stubFetch(s));
    let renders = 0;
    const orig = h.LoginPage.render.bind(h.LoginPage);
    h.LoginPage.render = (e) => { renders++; return orig(e); };
    await Promise.all(Array.from({ length: 6 }, () => h.App.showLogin()));
    check('6 个并发 401 → 登录页只渲染 1 次', renders === 1, `renders=${renders}`);
}

// ===== T5: 登录成功 → 主界面，且清掉明文 =====
{
    const s = st(); const h = makeHarness(stubFetch(s));
    await h.App.init();
    const pwEl = el(h, 'login-pw');
    pwEl.value = 'hunter2';
    /* 先按 SHOW 把明文亮出来，reset() 的复位才有东西可撤 —— 桩里 type 默认就是
       'password'，不先 toggle 的话"复位为 password 型"这条断言无法失败。 */
    h.PwField.toggle('login-pw');
    check('SHOW 之后 type 变成 text（下一条断言的前提）', pwEl.type === 'text');
    await h.LoginPage._submit();
    check('登录成功 → SHOW 状态被复位（明文不留在屏幕上）', pwEl.type === 'password');
    check('登录成功 → 进主界面',
          h.App._gate === null && !!h.document.getElementById('dash-marker'),
          `gate=${h.App._gate}`);
    check('登录成功 → 输入框已清空', pwEl.value === '');
}

// ===== T6: 回车提交 =====
{
    const s = st(); const h = makeHarness(stubFetch(s));
    await h.App.init();
    el(h, 'login-pw').value = 'hunter2';
    h.fire(el(h, 'login-pw'), 'keydown', { key: 'Enter' });
    await new Promise(r => setTimeout(r, 20));
    check('密码框里按回车能提交', nLogin(s) === 1, `login calls=${nLogin(s)}`);
}

// ===== T7: 退出登录 =====
{
    const s = st({ authed: true }); const h = makeHarness(stubFetch(s));
    await h.App.init();
    await h.App.logout();
    check('Logout → 回到登录页', h.App._gate === 'login' &&
                                  !!h.document.getElementById('login-pw'));
    check('Logout → 主界面已拆掉', !h.document.getElementById('dash-marker'));
    check('Logout → 调了后端 /auth/logout', s.calls.includes('/api/v1/auth/logout'));
    // auth.c 只注册了 HTTP_GET；发成 POST 会 404，而"回到登录页"照样成立
    check('Logout 用的是 GET（后端只注册了 HTTP_GET）',
          methodOf(s, '/api/v1/auth/logout') === 'GET',
          `method=${methodOf(s, '/api/v1/auth/logout')}`);
}

// ===== T8: Logout 请求失败也必须回登录页 =====
{
    const s = st({ authed: true }); const base = stubFetch(s);
    const h = makeHarness(async (p, o) => {
        if (p === '/api/v1/auth/logout') throw new TypeError('Failed to fetch');
        return base(p, o);
    });
    await h.App.init();
    let threw = null;
    try { await h.App.logout(); } catch (e) { threw = e.message; }
    check('Logout 请求失败 → 不抛异常，仍回登录页',
          threw === null && h.App._gate === 'login' &&
          !!h.document.getElementById('login-pw'), `threw=${threw}`);
}

// ===== T9: 连点 Sign In 只提交一次 =====
//   _busy 守卫。没有它的话每多点一次就多一次密码尝试，
//   而后端登录接口完全没有限速（auth.c:202-252）。
{
    const s = st({ delayMs: 30 }); const h = makeHarness(stubFetch(s));
    await h.App.init();
    el(h, 'login-pw').value = 'hunter2';
    await Promise.all([h.LoginPage._submit(), h.LoginPage._submit()]);
    check('连点两次 Sign In 只发一次登录请求', nLogin(s) === 1, `login calls=${nLogin(s)}`);
}

// ===== T10: showLogin 必须停掉后台活动 =====
{
    const s = st({ authed: true }); const h = makeHarness(stubFetch(s));
    await h.App.init();
    h.App.showLogin();
    check('showLogin → 当前页被 destroy（停掉轮询）',
          h.destroyCount() >= 1 && h.App.currentPage === null,
          `destroyed=${h.destroyCount()} currentPage=${h.App.currentPage}`);
    check('showLogin → WS 断开（不再每 3s 重连一次被拒的握手）',
          h.WS.socket === null && h.sockets.every(w => w.closed),
          `socket=${h.WS.socket} closed=${h.sockets.map(w => w.closed)}`);
    /* 只查 dash-marker 不够：把门画进 #content 而不是 #app 时，侧栏和 topbar
       还在，用户看到的是"主界面里嵌了个登录框"。必须在**外壳已经渲染过**的
       用例里查才有意义 —— 登录态直接启动时 #content 本来就不存在。 */
    check('showLogin → 整个外壳拆掉（门接管 #app，不是画进 #content）',
          !h.document.getElementById('sidebar') && !h.document.getElementById('content'),
          `sidebar=${!!h.document.getElementById('sidebar')} content=${!!h.document.getElementById('content')}`);
    check('showLogin → 登录表单确实画出来了', !!h.document.getElementById('login-pw'));
}

// ===== T11: 有门的时候切 hash 不能崩 =====
{
    const s = st(); const h = makeHarness(stubFetch(s));
    await h.App.init();
    let threw = null;
    try { h.location.hash = '#dashboard'; h.window.onhashchange(); }
    catch (e) { threw = e.message; }
    check('登录态下 hashchange 不抛异常', threw === null, `threw=${threw}`);
    check('登录态下 hashchange 不会把主界面画出来', !h.document.getElementById('dash-marker'));
}

// ===== T12: #login / #setup 都不是可导航路由 =====
//   进了 pages 表的话，已登录时敲 #login 会把登录卡片画进 #content，
//   而且登录成功后 _enter() 又按同一个 hash 导航回去，转不出来。
{
    const s = st({ authed: true }); const h = makeHarness(stubFetch(s));
    h.location.hash = '#login';
    await h.App.init();
    check('#login / #setup 都不在 pages 表里', !h.App.pages.login && !h.App.pages.setup);
    check('#login 回落到 dashboard，登录卡片不进 #content',
          !!h.document.getElementById('dash-marker') && !h.document.getElementById('login-pw'));
}

// ===== T13: 登录页的本地校验 =====
{
    const s = st(); const h = makeHarness(stubFetch(s));
    await h.App.init();
    el(h, 'login-pw').value = '';
    await h.LoginPage._submit();
    check('空密码 → 本地提示，不发请求',
          el(h, 'login-msg').textContent === 'Enter the password' && nLogin(s) === 0);

    // maxlength 数的是 UTF-16 码元。63 个汉字 = 189 字节，照样撑爆后端
    // 那 127 字节的 body 缓冲，而后端只会回一句语焉不详的 invalid_json。
    el(h, 'login-pw').value = '密'.repeat(63);
    await h.LoginPage._submit();
    check('63 个汉字（189 字节）→ 本地拦下，不发请求',
          /too long/i.test(el(h, 'login-msg').textContent) && nLogin(s) === 0,
          `msg="${el(h, 'login-msg').textContent}" calls=${nLogin(s)}`);
}

// ══════════════════════ 首次开机强制设密码 ══════════════════════

// ===== T14: 出厂无密码 → 强制设密码页，**不是**主界面 =====
//   docs/00:145 P0、docs/01:578「WebUI 强制设置密码」，此前从未实现：
//   设备出厂 password_hash 为空，check_auth() 恒真，整个管理接口敞开
//   且没有任何东西提醒用户。
{
    const s = st({ hasPw: false }); const h = makeHarness(stubFetch(s));
    await h.App.init();
    check('无密码 → 显示强制设密码页',
          h.App._gate === 'setup' && !!h.document.getElementById('setup-pw1'),
          `gate=${h.App._gate}`);
    check('无密码 → 不放行主界面', !h.document.getElementById('dash-marker'));
    check('无密码 → 也不是登录页（那样没有出口）', !h.document.getElementById('login-pw'));
    check('设密码页的按钮指向真实存在的函数',
          /onclick="SetupPage\._submit\(\)"/.test(h.document.getElementById('app').innerHTML) &&
          typeof h.SetupPage._submit === 'function');
}

// ===== T15: 设置成功 → 自动登录 → 进主界面 =====
//   config_mgr_set_password() 不建会话，不自动登录一次的话，
//   用户刚设完密码下一个请求就 401，被弹回登录页。
{
    const s = st({ hasPw: false }); const h = makeHarness(stubFetch(s));
    await h.App.init();
    const i1 = el(h, 'setup-pw1'), i2 = el(h, 'setup-pw2');
    i1.value = 'newpass1'; i2.value = 'newpass1';
    await h.SetupPage._submit();
    check('设密码成功 → 自动登录了一次', nSetPw(s) === 1 && nLogin(s) === 1,
          `setPw=${nSetPw(s)} login=${nLogin(s)}`);
    check('设密码成功 → 进主界面',
          h.App._gate === null && !!h.document.getElementById('dash-marker'),
          `gate=${h.App._gate}`);
    check('设密码成功 → 此后显示 Logout', !!h.document.getElementById('logout-btn'));
    check('设密码成功 → 两个框都已清空', i1.value === '' && i2.value === '');
}

// ===== T16: 设置成功但自动登录失败 → 回登录页，不能留在设置页 =====
//   密码**已经设上了**，留在设置页会让用户以为没成功而再设一遍。
{
    const s = st({ hasPw: false, loginFails: true }); const h = makeHarness(stubFetch(s));
    await h.App.init();
    el(h, 'setup-pw1').value = 'newpass1';
    el(h, 'setup-pw2').value = 'newpass1';
    await h.SetupPage._submit();
    check('自动登录失败 → 落到登录页而不是卡在设置页',
          h.App._gate === 'login' && !!h.document.getElementById('login-pw'),
          `gate=${h.App._gate}`);
}

// ===== T17: 设置页的本地校验，一条都不许放过去 =====
{
    const s = st({ hasPw: false }); const h = makeHarness(stubFetch(s));
    await h.App.init();

    el(h, 'setup-pw1').value = 'abc'; el(h, 'setup-pw2').value = 'abc';
    await h.SetupPage._submit();
    check('太短 → 本地拦下，不发请求',
          /4 characters/i.test(el(h, 'setup-msg').textContent) && nSetPw(s) === 0,
          `msg="${el(h, 'setup-msg').textContent}" calls=${nSetPw(s)}`);

    el(h, 'setup-pw1').value = 'abcd1234'; el(h, 'setup-pw2').value = 'abcd9999';
    await h.SetupPage._submit();
    check('两次不一致 → 本地拦下，不发请求',
          /not match/i.test(el(h, 'setup-msg').textContent) && nSetPw(s) === 0,
          `msg="${el(h, 'setup-msg').textContent}" calls=${nSetPw(s)}`);
    check('两次不一致 → 只清确认框，第一个框保留',
          el(h, 'setup-pw1').value === 'abcd1234' && el(h, 'setup-pw2').value === '');

    // ★ 关键：改密码接口的 body 缓冲是 256 字节，登录只有 128。设置页若用
    //   setter 那条宽限制，就能设出一个自己**再也登不进去**的密码。
    const long = '密'.repeat(63);
    el(h, 'setup-pw1').value = long; el(h, 'setup-pw2').value = long;
    await h.SetupPage._submit();
    check('设置页用的是登录那条更紧的字节上限（防止设出登不进去的密码）',
          /too long/i.test(el(h, 'setup-msg').textContent) && nSetPw(s) === 0,
          `msg="${el(h, 'setup-msg').textContent}" calls=${nSetPw(s)}`);
}

// ===== T18: 设备不可达时**不能**误判成"没密码"而弹设置页 =====
//   条件必须是"确知没密码"，写成 !this._hasPassword 的话一次网络抖动
//   就会把已经设过密码的用户扔进设置页。
{
    const h = makeHarness(async () => { throw new TypeError('Failed to fetch'); });
    let threw = null;
    try { await h.App.init(); } catch (e) { threw = e.message; }
    check('设备不可达 → init() 不抛异常', threw === null, `threw=${threw}`);
    check('设备不可达 → 不弹设置密码页', h.App._gate !== 'setup');
    check('设备不可达 → 仍渲染主界面（各页自己显示 --）',
          !!h.document.getElementById('dash-marker'));
}

// ══════════════════════ 设置页改密码 ══════════════════════

// ===== T18b: 三个设密码的入口必须用同一条字节上限 =====
//   2026-08-15 复核抓到的真缺陷：上限只加在新写的 login.js / setup.js 上，
//   偏偏漏了 settings.js —— 而那才是**已有用户**改密码的地方，设出一个
//   登录接口收不下的密码就等于把自己锁在门外，只能恢复出厂。
{
    const s = st({ authed: true }); const h = makeHarness(stubFetch(s));
    await h.App.init();
    // 不渲染整个设置页，只造出 _changePw 需要的三个元素
    h.document.getElementById('content').innerHTML =
        '<div id="set-pw1"></div><div id="set-pw2"></div><div id="set-pw-msg"></div>';

    const long = '密'.repeat(30);          // 90 字节 > 63
    el(h, 'set-pw1').value = long; el(h, 'set-pw2').value = long;
    await h.SettingsPage._changePw();
    // 文案在子节点 set-pw-badge 上（settings.js 用 textContent 写进去的）
    check('设置页改密码：超长被本地拦下，不发请求',
          nSetPw(s) === 0 && /too long/i.test(el(h, 'set-pw-badge').textContent),
          `msg="${el(h, 'set-pw-badge').textContent}" calls=${nSetPw(s)}`);

    el(h, 'set-pw1').value = 'goodpass'; el(h, 'set-pw2').value = 'goodpass';
    await h.SettingsPage._changePw();
    check('设置页改密码：正常长度能发出去', nSetPw(s) === 1, `calls=${nSetPw(s)}`);
}

// ===== T18c: 三处调用点一个都不能少（防止下次再漏一个）=====
{
    const src = FILES.filter(p => /login|setup|settings/.test(p))
                     .map(p => fs.readFileSync(p, 'utf8')).join('\n');
    const n = (src.match(/PwField\.passwordTooLong\(/g) || []).length;
    check('login / setup / settings 三处都调了 passwordTooLong', n >= 3, `找到 ${n} 处`);
}

// ══════════════════ OTBR 认证开关（设置页）══════════════════
// 复核用变异证明过：把整张卡 + 两个方法 + 两个 api 助手全删掉，原来的 56 项
// 依然全绿。开关这类"改配置"功能必须有断言证明它真的作用到系统上（7.5）。
{
    const s = st({ authed: true }); const h = makeHarness(stubFetch(s));
    await h.App.init();
    h.App.navigate('settings');
    await new Promise(r => setTimeout(r, 20));

    check('设置页有 OTBR 认证开关', !!h.document.getElementById('otbr-auth'));
    check('开关按钮绑到真实存在的函数',
          /onchange="SettingsPage\._otbrAuthToggle\(\)"/.test(
              h.document.getElementById('content').innerHTML) &&
          typeof h.SettingsPage._otbrAuthToggle === 'function');

    // 载入时应反映设备真实状态，而不是写死
    s.otbrAuth = true;
    await h.SettingsPage._otbrAuthLoad();
    check('载入时读设备真实状态（开）', el(h, 'otbr-auth').checked === true);
    s.otbrAuth = false;
    await h.SettingsPage._otbrAuthLoad();
    check('载入时读设备真实状态（关）', el(h, 'otbr-auth').checked === false);

    // 拨开：要真的发 POST，且状态跟着服务端回包走
    const n0 = s.calls.filter(p => p === '/api/v1/otbr/auth').length;
    el(h, 'otbr-auth').checked = true;
    await h.SettingsPage._otbrAuthToggle();
    check('拨开会发 POST 到设备',
          s.calls.filter(p => p === '/api/v1/otbr/auth').length > n0 && s.otbrAuth === true);
    check('拨开后提示提到要改 HA', /HA/.test(el(h, 'otbr-auth-badge').textContent),
          `badge="${el(h, 'otbr-auth-badge').textContent}"`);

    // 失败时开关必须拨回去，不能让 UI 和设备说两样话
    s.otbrFails = true;
    el(h, 'otbr-auth').checked = false;
    await h.SettingsPage._otbrAuthToggle();
    check('保存失败 → 开关拨回原位（UI 不谎报）',
          el(h, 'otbr-auth').checked === true && s.otbrAuth === true,
          `checked=${el(h, 'otbr-auth').checked} device=${s.otbrAuth}`);
    s.otbrFails = false;
}

// ===== 无密码设备上开启会被后端拒，UI 要照实拨回并说人话 =====
{
    const s = st({ hasPw: false }); const h = makeHarness(stubFetch(s));
    await h.App.init();          // 无密码 → 强制设密码页，先设一个才能进设置页
    el(h, 'setup-pw1').value = 'newpass1'; el(h, 'setup-pw2').value = 'newpass1';
    await h.SetupPage._submit();
    // 手工把设备退回无密码，模拟"导入了一份无密码备份"
    s.hasPw = false;
    h.App.navigate('settings');
    await new Promise(r => setTimeout(r, 20));
    el(h, 'otbr-auth').checked = true;
    await h.SettingsPage._otbrAuthToggle();
    check('无密码时开启被拒 → 开关拨回且提示"先设密码"',
          el(h, 'otbr-auth').checked === false &&
          /password first/.test(el(h, 'otbr-auth-badge').textContent),
          `checked=${el(h, 'otbr-auth').checked} badge="${el(h, 'otbr-auth-badge').textContent}"`);
}

// ══════════════════════ 兜底 ══════════════════════

// ===== T19: 门缺失时必须留下痕迹，不能静默白屏 =====
//   这正是原 bug 的形状：`if (page) {...}` 后面什么都没有。
{
    const s = st(); const h = makeHarness(stubFetch(s));
    h.App.loginPage = null;
    h.App.showLogin();
    check('登录页缺失 → #app 有可见提示（不是白屏）',
          /failed to\s+load/i.test(h.document.getElementById('app').innerHTML));

    const s2 = st({ hasPw: false }); const h2 = makeHarness(stubFetch(s2));
    h2.App.setupPage = null;
    h2.App.showSetup();
    check('设密码页缺失 → #app 有可见提示（不是白屏）',
          /failed to\s+load/i.test(h2.document.getElementById('app').innerHTML));
}

// ===== T20: 密码框有 maxlength =====
{
    const h = makeHarness(async () => reply(200, {}));
    const html = h.PwField.html('x');
    const m = /maxlength="(\d+)"/.exec(html);
    check('PwField 带 maxlength', !!m, html.slice(0, 90));
    // 63 的依据是 webui.h 的 PASSWORD_MAX_BYTES，不是 PASSWORD_MAX_LEN ——
    // 后者管的是 WiFi/MQTT 密码那些 64 字节缓冲，设备密码根本不落进去
    check('maxlength ≤ 63（对齐 webui.h 的 PASSWORD_MAX_BYTES）',
          !!m && Number(m[1]) <= 63, m ? `maxlength=${m[1]}` : '没有 maxlength 属性');
}

// ===== T21: 被拒的握手必须退避，不能每 3 秒撞一次 =====
//   httpd 先发 101 再跑 ws_post_handshake_cb（httpd_uri.c:346→357），所以被拒的
//   握手在浏览器里同样先 onopen 再 onclose。旧版在 onopen 里把退避清回 3 秒，
//   退避永远长不起来：2026-09-13 真板日志 600 槽里 87% 是它，每 3.01 秒一对。
{
    const tm = fakeTimers();
    const s = st({ authed: true }); const h = makeHarness(stubFetch(s), tm);
    await h.App.init();
    check('进主界面后 WS 已连接', h.sockets.length === 1 && h.WS.socket === h.sockets[0],
          `sockets=${h.sockets.length}`);
    const waits = []; let extra = 0;
    for (let i = 0; i < 6; i++) {
        const before = tm.armed.length;
        await rejectHandshake(h);
        const fresh = tm.armed.slice(before);
        if (fresh.length !== 1) extra++;
        waits.push(fresh.length ? fresh[fresh.length - 1].ms : null);
        tm.fire(fresh[fresh.length - 1]);      // 到点重连 → 新 socket
    }
    check('被拒握手的重连间隔指数增长（3→6→12→24→48→60 秒封顶）',
          JSON.stringify(waits) === JSON.stringify([3000, 6000, 12000, 24000, 48000, 60000]),
          `waits=${JSON.stringify(waits)}`);
    check('每次被拒只挂一个重连定时器、到点只建一条新 socket',
          extra === 0 && h.sockets.length === 7, `extra=${extra} sockets=${h.sockets.length}`);
    check('会话还活着（探针 200）→ 不弹登录页', h.App._gate === null, `gate=${h.App._gate}`);
    /* 收到第一帧才算被接受，退避从这一刻才归零 */
    const w = h.WS.socket;
    w.onmessage({ data: JSON.stringify({ type: 'status_update', data: { wifi_connected: true } }) });
    w.readyState = 3; w.onclose(); await tick();
    check('收到过帧的连接断开 → 退避归零，下次 3 秒重连',
          tm.armed[tm.armed.length - 1].ms === 3000, `ms=${tm.armed[tm.armed.length - 1].ms}`);
}

// ===== T22: 会话没了（网关重启过）的标签必须落到登录页，而不是无限重连 =====
//   停在 Dashboard 的标签只轮询 /status（UNLESS_OPEN，永远不 401），WS 被关又和
//   网络闪断没区别，所以它自己永远发现不了会话已失效（会话表在 RAM，auth.c:25）。
//   一帧都没收到就被关的连接要探一次受保护端点。
{
    const tm = fakeTimers();
    const s = st({ authed: true }); const h = makeHarness(stubFetch(s), tm);
    await h.App.init();
    s.authed = false;                              // 网关重启：RAM 里的会话表清空
    const nCalls = s.calls.length;
    await rejectHandshake(h);
    const probed = s.calls.slice(nCalls);
    check('被拒握手后探了一次、且只探一次受保护端点',
          probed.length === 1 && probed[0] === h.API.AUTH_PROBE, `calls=${JSON.stringify(probed)}`);
    check('WS 探针不是 UNLESS_OPEN 那批端点（同 T1c）',
          !['/api/v1/status', '/api/v1/led/config', '/api2/info', '/api2/state'].includes(probed[0]),
          `probed=${probed[0]}`);
    check('探针 401 → 登录页', h.App._gate === 'login' && !!h.document.getElementById('login-pw'),
          `gate=${h.App._gate}`);
    const reconnect = tm.armed[tm.armed.length - 1];
    check('探针 401 → 重连定时器被取消、socket 置空',
          reconnect.cleared && h.WS.socket === null, `cleared=${reconnect.cleared} socket=${h.WS.socket}`);
    tm.fire(reconnect);
    check('已取消的定时器不会再建 socket', h.sockets.length === 1, `sockets=${h.sockets.length}`);
}
// 设备正在重启（探针打不通）→ 不能误弹登录页，要继续退避重连
{
    const tm = fakeTimers();
    const s = st({ authed: true });
    const base = stubFetch(s);
    let down = false;
    const h = makeHarness(async (p, o) => {
        if (down && p === h.API.AUTH_PROBE) throw new Error('ECONNREFUSED');
        return base(p, o);
    }, tm);
    await h.App.init();
    down = true;
    await rejectHandshake(h);
    const t = tm.armed[tm.armed.length - 1];
    check('探针打不通（设备重启中）→ 不弹登录页，重连定时器仍在',
          h.App._gate === null && !t.cleared && h.WS.socket !== null,
          `gate=${h.App._gate} cleared=${t.cleared}`);
}
// 收到过帧的正常断线（网络闪断）→ 不探针，照常重连
{
    const tm = fakeTimers();
    const s = st({ authed: true }); const h = makeHarness(stubFetch(s), tm);
    await h.App.init();
    const w = h.WS.socket;
    w.onmessage({ data: JSON.stringify({ type: 'status_update', data: {} }) });
    const nCalls = s.calls.length;
    w.readyState = 3; w.onclose(); await tick(); await tick();
    const t = tm.armed[tm.armed.length - 1];
    check('收到过帧的连接断开 → 不探针（网络闪断不该多打一次 API），照常重连',
          s.calls.length === nCalls && h.App._gate === null && !t.cleared,
          `extra calls=${JSON.stringify(s.calls.slice(nCalls))} gate=${h.App._gate}`);
}

console.log('');
console.log(`Result: ${PASS} passed, ${FAIL} failed`);
process.exit(FAIL === 0 ? 0 : 1);

})();
HARNESS

node "$RUN"
NODE_RC=$?

echo ""
if [ $SHELL_FAIL -gt 0 ]; then
    echo "另有 $SHELL_FAIL 项结构检查失败（见开头）"
fi
if [ $NODE_RC -eq 0 ] && [ $SHELL_FAIL -eq 0 ]; then
    echo "全部通过"
    exit 0
fi
echo "有失败项"
exit 1
