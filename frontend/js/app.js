/* Password input plus a show/hide toggle. Three callers share it: the WiFi
   passphrase field, and the new/confirm password fields on the Settings page —
   hence one implementation rather than three copies.

   The toggle says SHOW / HIDE instead of an eye icon: every other icon in this
   UI is a monochrome HTML entity (&#9632; and friends), while an eye is only
   available as a color emoji, which renders inconsistently across phone fonts.
   Text also carries the state directly ("SHOW" means "clicking reveals").
   Only input.type changes; the password never enters DOM text or the logs. */
const PwField = {
    /* Build the whole field, toggle included. The button id is always
       `<id>-toggle`.

       maxlength defaults to 63, but the limit comes from a **different** place
       for each of the three callers — do not conflate them:
         · WiFi passphrase — 63 characters is the WPA2 PSK maximum, and the
           backend's wifi.password is a PASSWORD_MAX_LEN=64 buffer
           (config_mgr.h).
         · Device password (login page / first-run setup page) — unrelated to
           PASSWORD_MAX_LEN. The device password never lands in a 64-byte
           buffer; config_mgr_set_password() hashes it straight to SHA256 hex.
           The real limit comes from the two body buffers in auth.c — see
           passwordTooLong() below and PASSWORD_MAX_BYTES in webui.h.
       maxlength is only the coarse first pass: it counts UTF-16 code units, so
       63 CJK characters are 189 bytes. */
    html(id, placeholder = '', maxlength = 63) {
        return `<div class="input-pw">
            <input id="${id}" type="password" class="input" maxlength="${maxlength}"
                   placeholder="${placeholder}">
            <button type="button" class="pw-toggle" id="${id}-toggle"
                    aria-label="${t('Show password')}" onclick="PwField.toggle('${id}')">SHOW</button>
        </div>`;
    },

    /* Does the password exceed the number of **bytes** the device accepts? All
       three places that set a password must use this: the login page, the
       first-run setup page and the settings page — miss any one of them and a
       user can set a key that locks themselves out.

       63 is the same number as PASSWORD_MAX_BYTES in webui.h; the derivation
       lives there. In short: the change-password endpoint's body buffer is
       twice the size of the login one, so the range in between is "can be
       set, cannot log back in"; and the HTTP Basic path (used by the Home
       Assistant OTBR integration) has an even tighter decoded[96].
       The backend is the real gate — curl and config import bypass the
       frontend — this check only exists to say so in plain words, on the spot.

       ⚠️ It counts bytes, not characters: 63 CJK characters are 189 bytes.
       maxlength counts UTF-16 code units and cannot catch that, so both
       checks are needed. */
    passwordTooLong(pw) {
        return new TextEncoder().encode(pw).length > 63;
    },

    toggle(id) {
        const input = document.getElementById(id);
        const btn   = document.getElementById(id + '-toggle');
        if (!input || !btn) return;
        const reveal = input.type === 'password';
        input.type = reveal ? 'text' : 'password';
        btn.textContent = reveal ? 'HIDE' : 'SHOW';
        btn.setAttribute('aria-label', reveal ? t('Hide password') : t('Show password'));
        input.focus();
    },

    /* Clear and reset to hidden. Mandatory after switching APs or changing a
       password: if the previous visit clicked SHOW and we do not reset, the
       next entry is typed in cleartext on screen. */
    reset(...ids) {
        for (const id of ids) {
            const input = document.getElementById(id);
            const btn   = document.getElementById(id + '-toggle');
            if (input) { input.value = ''; input.type = 'password'; }
            if (btn) { btn.textContent = 'SHOW'; btn.setAttribute('aria-label', t('Show password')); }
        }
    }
};

/* IPv4 as the firmware stores it: lwip's ip4_addr_t.addr, a uint32 in network
   byte order read on a little-endian core — so the first octet is the low
   byte. /config/export emits static_ip and friends in exactly this form, and
   parse_ip_field() in rest_api.c accepts both this number and dotted text. */
const IP4 = {
    toString(n) {
        n = Number(n) >>> 0;
        return [n & 255, (n >>> 8) & 255, (n >>> 16) & 255, (n >>> 24) & 255].join('.');
    },
};

const App = {
    currentPage: null,

    /* The "gate" currently occupying all of #app: null | 'login' | 'setup'.
       Two purposes:
       ① Idempotence — when a session expires, every in-flight request on the
          page gets its own 401 and each one calls showLogin(); without this
          guard the form is repainted over and over while the user types.
       ② Blocking navigate() — while a gate is up there is no #content, so
          changing the hash (the browser Back button, say) would write
          innerHTML onto null and take the whole script down.
       One field rather than two booleans: two booleans can both be true, and
       that state has no meaning. */
    _gate: null,

    /* Whether the device has a password set. Drives the Logout button and
       whether we force the set-password flow. */
    _hasPassword: false,

    /* These two pages are **not** in the pages table. If they were, `#login`
       would become a navigable route: hitting #login while logged in would
       paint the login card into #content inside the main UI, and after a
       successful login _enter() would navigate right back to the same hash —
       an inescapable loop. */
    loginPage: null,
    setupPage: null,

    /* Object.create(null) rather than {}: otherwise a hash like `#toString` or
       `#constructor` resolves to the same-named member on Object.prototype,
       slips past every `if (!page)` check below, and dies on
       "page.render is not a function". */
    pages: Object.create(null),

    registerPage(name, page) {
        this.pages[name] = page;
    },

    navigate(name) {
        const page = this.pages[name];
        if (!page) return;
        if (this.currentPage && this.currentPage.destroy) this.currentPage.destroy();
        this.currentPage = page;
        const el = document.getElementById('content');
        el.innerHTML = '';
        page.render(el);
        document.querySelectorAll('.nav-item').forEach(n =>
            n.classList.toggle('active', n.dataset.page === name));
        location.hash = name;
    },

    /* Repaint everything after a language change. Every page builds its DOM
       inside render(), so re-running the current route is enough — there is no
       per-node text binding to update. */
    relocalize() {
        if (this._gate) {
            const name = this._gate;
            const page = name === 'login' ? this.loginPage : this.setupPage;
            this._gate = null;              // force _showGate past its idempotence guard
            this._showGate(name, page, name === 'login'
                ? t('Login required, but the login page')
                : t('The password setup page'));
            return;
        }
        if (!this.currentPage) return;
        const name = Object.keys(this.pages).find(k => this.pages[k] === this.currentPage);
        this._renderShell();
        if (name) this.navigate(name);
    },

    /* Hand all of #app over to a gate (login / set password). Both tear down
       the same way, so it is merged here rather than duplicated — otherwise
       one gets fixed and the other does not. */
    _showGate(name, page, missingHint) {
        if (this._gate === name) return;
        this._gate = name;

        /* The current page probably holds polling timers (dashboard, thread and
           logs all do). Without destroy() it keeps hitting the API every few
           seconds, each response another 401 leading back here — the
           idempotence flag stops the repaint but not this endless spinning. */
        if (this.currentPage && this.currentPage.destroy) this.currentPage.destroy();
        this.currentPage = null;

        /* The WS handshake is authenticated (websocket.c:218) and the server
           drops unauthorized ones. ws.js keeps reconnecting after onclose (with
           backoff), so without an explicit disconnect the rejected handshake is
           retried for as long as the tab stays open. */
        WS.disconnect();

        const app = document.getElementById('app');
        if (!page) {
            /* A missing gate must leave a trace. This used to be `if (page)`
               followed by nothing: white screen, no error, nothing to go on —
               which is how the password feature stayed broken for months. */
            app.innerHTML = `<div class="login-wrap"><div class="login-card">
                <div class="login-brand"><span class="logo">&#9670;</span>
                <span class="title">bitEngine Hub</span></div>
                <div class="hint">${t('{what} failed to load. Reload the page; if it persists, re-flash the www partition.', { what: missingHint })}</div>
                <div class="login-lang">${Lang.selectHtml('gate-lang-missing')}</div>
            </div></div>`;
            return;
        }
        app.innerHTML = '';
        page.render(app);
    },

    /* Every 401 anywhere ends up here (the interceptor in api.js). */
    showLogin() { this._showGate('login', this.loginPage, t('Login required, but the login page')); },

    /* The cross-origin guard (webui_server.c in the firmware) decided the
       current Host is not on the allow list and returned 421. This is **not**
       an auth problem, so do not show the login page — the password is fine,
       the **address** is not.
       Typical cause: the router gave the device a custom name (bitengine.mynet,
       say), or there is a reverse proxy in front. The page loads normally
       (read-only endpoints carry no guard) but every save fails, and nothing
       tells the user why.
       The banner is idempotent: one page load produces many simultaneous 421s,
       and we insert exactly one. */
    showHostBanner(use) {
        if (document.getElementById('host-banner')) return;
        const b = document.createElement('div');
        b.id = 'host-banner';
        b.className = 'banner banner-red';
        /* textContent, not innerHTML: `use` comes from the device and
           location.host from the address bar; neither belongs in an HTML
           parser. */
        b.textContent = t('This address ({host}) is not on the allow list, so saving anything will be rejected.', { host: location.host })
                      + ' '
                      + (use ? t('Use {addr} instead, or open Settings → Extra access addresses from that address and add {host} there.', { addr: use, host: location.host })
                             : t('Use the device IP address instead.'));
        document.body.prepend(b);
    },

    /* Forced onboarding when the device ships without a password. docs/00:145
       lists password protection as P0, and docs/01:578 requires "no auth on
       first boot without a password, WebUI forces one to be set". */
    showSetup() { this._showGate('setup', this.setupPage, t('The password setup page')); },

    async init() {
        Lang.init();

        /* The gate test is "does a protected endpoint 401?", not
           /auth/status's password_required — that one reports whether the
           device has ever had a password set, which says nothing about whether
           *this session* is logged in. Using it meant the login page showed
           forever once a password existed, successful login included.

           ⚠️ The probe endpoint (API.AUTH_PROBE in api.js — ws.js probes the
           same one after a rejected handshake) must be **unconditionally**
           AUTH_REQUIRED. It used to be /api/v1/status, which was changed to
           AUTH_REQUIRED_UNLESS_OPEN in 2026-08 — thread.rest_auth ships off
           ⇒ it never 401s ⇒ on a device with a password, a logged-out browser
           still reaches the main UI and sees the full dashboard, and only gets
           bounced to login when opening some other page. Hit for real in a
           browser on 2026-08-23: the dashboard was fully populated, one click
           on Settings replaced the entire DOM via showLogin(), and the console
           was left with two "Cannot set properties of null" errors — the
           settings page's async callbacks landing on a DOM that no longer
           existed.
           /api/v1/ntp is a GET, has no side effects, returns a small body, and
           is governed by no toggle at all (bare AUTH_REQUIRED). Before
           switching probes, confirm the target has not been turned into
           UNLESS_OPEN, or this bug comes back verbatim. */
        const probe = await API.probe(API.AUTH_PROBE);
        if (probe.status === 401) { this.showLogin(); return; }
        /* status 0 (device unreachable) still enters the main UI: each page
           shows "--" on its own, which makes it far easier to tell a network
           problem from an auth problem than being stuck on a fake login page. */
        await this._enter();
    },

    /* Enter the main UI once authenticated (or once we know the device has no
       password). A successful login lands here too, which is why no
       location.reload() is needed — reloading would discard the navigation
       intent the user just expressed. */
    async _enter() {
        /* A failing authStatus must not deadlock entry. probe() deliberately
           catches "device unreachable" (api.js); rethrowing that handling here
           would undo it — the page would sit on index.html's "Loading..."
           forever, with no way to tell a network problem from an auth problem.
           If we cannot read it, keep the previous verdict (right after a
           successful login it is already true). */
        let auth = null;
        try { auth = await API.authStatus(); } catch (e) { /* device unreachable */ }
        this._hasPassword = auth ? !!auth.password_required : this._hasPassword;

        /* Shipped without a password → force the setup page, no main UI.
           ⚠️ The condition must be "**known** to have no password" (auth was
           read and is false), never `!this._hasPassword` — that would throw a
           user who does have a password into the setup page on any network
           hiccup (auth === null). */
        if (auth && !auth.password_required) { this.showSetup(); return; }

        /* Clear the flag immediately before rendering: put it above those
           awaits and a failure there leaves the gate on screen with the
           anti-crash guard in onhashchange already switched off. */
        this._gate = null;
        this._renderShell();
        WS.connect();
        /* .catch: a device that cannot be reached must not surface as an
           unhandled rejection here — the page still has to enter the main UI
           (see the reasoning on authStatus above). */
        API.status().then(d => this.updateTopbar(d)).catch(() => {});
        const hash = location.hash.slice(1);
        this.navigate(this.pages[hash] ? hash : 'dashboard');
    },

    /* Uplink badge in the topbar. Fed by every WS status_update (ws.js) and
       once from REST on entry, so it is correct on whichever page the user
       lands. "Online" means at least one uplink is up — a device on Ethernet
       only must not be painted red just because WiFi is disconnected (the old
       Dashboard-only code looked at wifi_connected alone). */
    updateTopbar(s) {
        const tb = document.getElementById('topbar-status');
        if (!tb || !s) return;
        const up = (s.active_interface && s.active_interface !== 'none')
                || !!s.wifi_connected || !!s.eth_link_up;
        tb.innerHTML = `<span class="badge badge-${up ? 'green' : 'red'}">${up ? t('Online') : t('Offline')}</span>`;
    },

    async logout() {
        /* Go back to the login page even if the request fails: otherwise the
           user clicks Logout, stays on the main UI, and believes they are
           logged out. */
        try { await API.logout(); } catch (e) { /* offline goes through too */ }
        this.showLogin();
    },

    _renderShell() {
        const navItems = [
            { id: 'dashboard', icon: '&#9632;', label: t('Dashboard') },
            { id: 'wifi',      icon: '&#8634;', label: t('WiFi') },
            { id: 'ethernet',  icon: '&#9783;', label: t('Ethernet') },
            { id: 'zigbee',    icon: '&#9881;', label: t('Zigbee') },
            { id: 'thread',    icon: '&#9670;', label: t('Thread') },
            { id: 'spectrum',  icon: '&#9776;', label: t('Spectrum') },
            { id: 'led',       icon: '&#9679;', label: t('LED') },
            { id: 'mqtt',      icon: '&#8644;', label: t('MQTT / Webhook') },
            { id: 'ota',       icon: '&#8679;', label: t('Firmware') },
            { id: 'logs',      icon: '&#9776;', label: t('Logs') },
            { id: 'settings',  icon: '&#9881;', label: t('Settings') },
        ];
        document.getElementById('app').innerHTML = `
            <nav id="sidebar">
                <div class="nav-header">
                    <span class="logo">&#9670;</span>
                    <span class="title">bitEngine Hub</span>
                </div>
                ${navItems.map(n => `
                    <a class="nav-item" data-page="${n.id}" href="#${n.id}">
                        <span class="nav-icon">${n.icon}</span>
                        <span class="nav-label">${n.label}</span>
                    </a>`).join('')}
            </nav>
            <main id="main">
                <header id="topbar">
                    <button id="menu-btn" onclick="document.getElementById('sidebar').classList.toggle('open')">&#9776;</button>
                    <span id="topbar-title">bitEngine Hub</span>
                    <span id="topbar-status"></span>
                    ${Lang.selectHtml()}
                    ${this._hasPassword ? `<button class="btn btn-sm" id="logout-btn"
                            onclick="App.logout()">${t('Logout')}</button>` : ''}
                </header>
                <div id="content"></div>
            </main>`;

        document.querySelectorAll('.nav-item').forEach(a => {
            a.addEventListener('click', e => {
                e.preventDefault();
                document.getElementById('sidebar').classList.remove('open');
                this.navigate(a.dataset.page);
            });
        });
    }
};

window.onload = () => App.init();
window.onhashchange = () => {
    /* While a gate is up there is no #content, and navigate() would write
       innerHTML onto null. One press of the browser Back button after a
       session expires is enough to trigger it. */
    if (App._gate) return;
    const h = location.hash.slice(1);
    if (h && App.pages[h]) App.navigate(h);
};
