const API = {
    /* 421 = the cross-origin guard rejected this Host (webui_cross_origin_check
       in the firmware's webui_server.c). This must be intercepted **globally**
       and the way out shown, because the symptom is unusually treacherous: the
       page opens perfectly normally (read-only endpoints carry no guard and
       still return 200) and only writes plus a few sensitive reads fail
       silently, while each caller does nothing but drop the error field into a
       red label — "unknown_host" gives the user no way to help themselves, and
       does not even hint that the problem is the **address** they used.
       The `use` field in the response body is that way out (the firmware takes
       it from getsockname, i.e. the local address of this very connection).
       Returns true meaning "already handled; caller should treat as failure". */
    async _hostGate(res) {
        if (res.status !== 421) return false;
        let d = null;
        try { d = await res.json(); } catch (e) { /* not JSON */ }
        App.showHostBanner(d && d.use);
        return true;
    },

    /* A network-level failure (device rebooting, proxy down, WiFi blip) turns
       into null — the same value every caller already treats as "no data"
       and paints as "--" or "Failed". Left as a thrown TypeError it surfaced
       as an unhandled rejection from whichever poll happened to be running
       (seen from the Logs page's 2 s poll during a proxy restart, 2026-09-05),
       while no page ever caught it. probe()/login() keep their own handling. */
    async _fetch(path, init) {
        try { return await fetch(path, init); }
        catch (e) { return null; }
    },

    async get(path) {
        const res = await API._fetch(path);
        if (!res) return null;
        if (res.status === 401) { App.showLogin(); return null; }
        if (await API._hostGate(res)) return null;
        if (!res.ok) return null;
        return res.json();
    },

    /* Differs from get() by surfacing the body on non-2xx responses too.
       The spectrum endpoints answer 503 with a structured body explaining *why*
       there is no data (how many channels coexistence rejected, how many more
       samples are needed for a recommendation); get() would throw all of that
       away and leave a blank page. */
    async getSoft(path) {
        const res = await API._fetch(path);
        if (!res) return null;
        if (res.status === 401) { App.showLogin(); return null; }
        if (await API._hostGate(res)) return null;
        let body = null;
        try { body = await res.json(); } catch (e) { /* non-JSON response */ }
        if (body && typeof body === 'object') body.__status = res.status;
        return body;
    },

    async post(path, data) {
        const res = await API._fetch(path, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(data)
        });
        if (!res) return null;
        if (res.status === 401) { App.showLogin(); return null; }
        if (await API._hostGate(res)) return null;
        return res.json();
    },

    /* Status code only, bypassing the 401 interceptor above. Used at startup to
       decide whether a login is needed.
       Why not /api/v1/auth/status: that endpoint reports whether the device has
       ever had a password set (auth.c:193-200), not whether *this session* is
       logged in. Using it as the gate bounces an already-authenticated user
       back to the login page on every refresh. The only valid test is "does a
       protected endpoint 401?". */
    /* The endpoint that answers "is this session still valid?". Two callers:
       App.init() at page load, and ws.js after a handshake the firmware closed
       without sending a frame. It must be **unconditionally** AUTH_REQUIRED —
       the reasoning, and the 2026-08-23 bug a toggle-governed endpoint caused,
       is at the App.init() call site. One constant so both callers move
       together; test_login.sh T1c goes red if it is ever pointed at an
       UNLESS_OPEN endpoint. */
    AUTH_PROBE: '/api/v1/ntp',

    async probe(path) {
        try {
            const res = await fetch(path);
            return { ok: res.ok, status: res.status };
        } catch (e) {
            return { ok: false, status: 0 };       // device unreachable, not unauthorized
        }
    },

    /* Login must bypass the 401 interceptor as well: the backend also answers
       401 for a wrong password (auth.c:236), so going through the interceptor
       would repaint the login form while the user is still typing, and the
       caller would receive null with no way to tell "wrong password" from
       "session expired". Hand the status code back untouched. */
    async login(pw) {
        try {
            const res = await fetch('/api/v1/auth/login', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ password: pw })
            });
            let body = null;
            try { body = await res.json(); } catch (e) { /* non-JSON response */ }
            return { ok: res.ok, status: res.status, body };
        } catch (e) {
            return null;                           // device unreachable
        }
    },

    upload(path, file, onProgress) {
        return new Promise((resolve, reject) => {
            const xhr = new XMLHttpRequest();
            xhr.open('POST', path);
            xhr.setRequestHeader('Content-Type', 'application/octet-stream');
            xhr.upload.onprogress = e => { if (e.lengthComputable) onProgress?.(e.loaded, e.total); };
            xhr.onload = () => {
                /* upload uses XHR rather than fetch, so _hostGate does not
                   apply and the check is repeated here. The OTA page is the
                   worst possible place to let a user find out only after the
                   transfer that nothing took effect. */
                if (xhr.status === 421) {
                    let d = null;
                    try { d = JSON.parse(xhr.responseText); } catch (e) {}
                    App.showHostBanner(d && d.use);
                    resolve({ status: 'error', error: 'unknown_host' });
                    return;
                }
                try { resolve(JSON.parse(xhr.responseText)); }
                catch { resolve({ status: xhr.status >= 200 && xhr.status < 300 ? 'ok' : 'error' }); }
            };
            xhr.onerror = () => reject(new Error('Upload failed'));
            xhr.send(file);
        });
    },

    status:            () => API.get('/api/v1/status'),
    authStatus:        () => API.get('/api/v1/auth/status'),
    // login() / probe() are the methods above — they must not go through the
    // 401 interceptor; the reasoning is documented at each of them.
    logout:            () => API.get('/api/v1/auth/logout'),
    wifiScan:          () => API.get('/api/v1/wifi/scan'),
    wifiConfig:   (s,p) => API.post('/api/v1/wifi/config', { ssid: s, password: p }),
    ethStatus:         () => API.get('/api/v1/ethernet/status'),
    ethConfig:      d  => API.post('/api/v1/ethernet/config', d),
    zigbeeStatus:      () => API.get('/api/v1/zigbee/status'),
    /* Radio region (docs/14): GET {region|null, declared, menu[], zigbee{}, thread_tx_dbm};
       POST {region: 'US'|null} saves and applies at once (Zigbee drops ~40 s). */
    radioRegionGet:    () => API.get('/api/v1/radio/region'),
    radioRegionSet:   r  => API.post('/api/v1/radio/region', { region: r }),
    zigbeeRestart:     () => API.post('/api/v1/zigbee/restart', {}),
    /* Clients are disconnected by ip, not by index: index is a tcp_server slot
       number, and in the window between reading it and sending it back the slot
       may already have been reused by someone else. The firmware's ip path
       matches and closes while holding the lock. */
    clientDisconnect: ip => API.post('/api/v1/zigbee/clients/disconnect', { ip }),
    statsReset:        () => API.post('/api/v1/zigbee/stats/reset', {}),
    /* Extra Host values allowed past the cross-origin guard. GET returns
       {hostname, extra_host, current, accepted[]}, where `current` is the Host
       used for this very request — the settings page uses it for its
       "fill in the current address" button. */
    /* OTBR-only token. Both GET and POST return a **complete, pasteable URL** —
       the Home Assistant otbr integration has no reconfigure step, so getting
       it wrong once means deleting the entry and starting over. The fewer
       people involved in assembling that string, the better. */
    otbrTokenGet:      () => API.get('/api/v1/otbr/token'),
    otbrTokenSet:   a  => API.post('/api/v1/otbr/token', { action: a }),
    accessHostGet:     () => API.get('/api/v1/net/access-host'),
    accessHostSet:  h  => API.post('/api/v1/net/access-host', { extra_host: h }),
    softapGet:         () => API.get('/api/v1/network/softap'),
    softapSet:      e  => API.post('/api/v1/network/softap', { enabled: e }),
    wifiStatic:     d  => API.post('/api/v1/wifi/static', d),
    /* getSoft: the 503 body carries *why* there is no data, and must not be
       thrown away. */
    spectrumScan:      () => API.getSoft('/api/v1/spectrum/scan'),
    spectrumBeacon:    () => API.getSoft('/api/v1/spectrum/beacon'),
    spectrumRecommend: () => API.getSoft('/api/v1/spectrum/recommend'),
    configExport:      () => API.get('/api/v1/config/export'),
    configImport:   j  => API.post('/api/v1/config/import', j),
    /* wipeThread defaults to false = keep the Thread network. The firmware
       defaults the same way, so an empty body from an older frontend still
       takes the safe path. */
    factoryReset:      (wipeThread) => API.post('/api/v1/config/factory',
                                                { wipe_thread: !!wipeThread }),
    otaUpload:    (f,p) => API.upload('/api/v1/ota/esp32', f, p),
    wwwUpload:    (f,p) => API.upload('/api/v1/ota/www', f, p),
    // MG24 NCP: the upload returns 202 and the device runs XMODEM in the
    // background, so progress has to be polled separately.
    zigbeeOtaUpload: (f,p) => API.upload('/api/v1/ota/zigbee', f, p),
    zigbeeOtaStatus: () => API.get('/api/v1/ota/zigbee/status'),
    /* Developer unlock for unsigned .gbl: arm opens a 30 s window that the
       physical button (held 2-10 s, released) confirms; lock cancels. */
    zigbeeUnlockStatus: () => API.get('/api/v1/ota/zigbee/unlock'),
    zigbeeUnlockArm:    () => API.post('/api/v1/ota/zigbee/unlock', {}),
    zigbeeUnlockLock:   () => API.post('/api/v1/ota/zigbee/unlock', { lock: true }),
    /* Cloud update. On the device, otaCheck fetches the manifest
       **synchronously** and can take up to about 6s (webui is a single httpd
       task, so the entire UI is frozen meanwhile) — which is why it may only
       ever be triggered by a user clicking a button, and never placed in any
       polling or page-load path. */
    otaCheck:          () => API.get('/api/v1/ota/check'),
    otaCloudInstall:   () => API.post('/api/v1/ota/cloud', {}),
    otaCloudInstallNcp:() => API.post('/api/v1/ota/cloud/ncp', {}),
    otaCloudStatus:    () => API.get('/api/v1/ota/cloud/status'),
    setPassword:    p  => API.post('/api/v1/auth/password', { new_password: p }),
    reboot:            () => API.post('/api2/reboot', {}),

    /* LED test: the backend supplies the LED position table; the frontend never
       hardcodes an index→name mapping (the order is decided by the PCB
       traces, so a board revision only touches that one table in the
       firmware). */
    ledTestInfo:       () => API.get('/api/v1/led/test'),
    ledTest:    (i,c,s) => API.post('/api/v1/led/test',
                                    { index: i, color: c, hold_s: s }),
    ntpGet:            () => API.get('/api/v1/ntp'),
    ntpSet:         d  => API.post('/api/v1/ntp', d),
    otbrAuthGet:       () => API.get('/api/v1/otbr/auth'),
    otbrAuthSet:    e  => API.post('/api/v1/otbr/auth', { enabled: !!e }),
    whitelistGet:      () => API.get('/api/v1/whitelist'),
    whitelistSet:   d  => API.post('/api/v1/whitelist', d),
    ledTiming:         () => API.get('/api/v1/led/timing'),
    ledSetTiming: (a,b) => API.post('/api/v1/led/timing', { t0h_ns: a, t1h_ns: b }),

    /* LED brightness / night mode. The GET was only added on 2026-08-17 —
       before that, reading the current state meant POSTing an empty object,
       i.e. disguising a read as a write and burning an NVS write for nothing.
       ⚠️ The firmware applies **partial updates**: an absent field keeps its
       current value, so sending only the changed fields is safe. */
    ledConfigGet:      () => API.get('/api/v1/led/config'),
    ledConfigSet:   d  => API.post('/api/v1/led/config', d),
    ledNightmode:   e  => API.post('/api/v1/led/nightmode', { enabled: !!e }),

    /* MQTT / Webhook. These endpoints were wired to REST back on 2026-08-12,
       but the frontend had no entry point for them, which meant two P1 selling
       points did not exist for ordinary users (feature_gap_audit A1).
       ⚠️ The MQTT password is never echoed back; status only exposes a
       password_set boolean. Omitting the password field means "keep the
       current password"; sending an empty string is what clears it. */
    mqttStatus:        () => API.get('/api/v1/mqtt/status'),
    mqttConfig:     d  => API.post('/api/v1/mqtt/config', d),
    webhookGet:        () => API.get('/api/v1/webhook/config'),
    webhookSet:     d  => API.post('/api/v1/webhook/config', d),

    // OTBR Thread endpoints
    threadNode:        () => API.get('/node'),
    // threadState has no callers: the Thread page reads its state out of the
    // threadNode() JSON. It is kept because it is a standard ot-br-posix
    // endpoint that third-party tooling will want later.
    threadState:       () => fetch('/node/state').then(r => r.ok ? r.text() : null),
    threadDataset:     () => API.get('/node/dataset/active'),
    threadDiagnostics: () => API.get('/diagnostics'),
    threadChildren:    () => API.get('/node/children'),
    threadNeighbors:   () => API.get('/node/neighbors'),
    threadIpAddrs:     () => API.get('/node/ipaddrs'),
    threadBorderRouting: () => API.get('/node/border-routing'),
    threadCounters:    () => API.get('/node/counters'),
    /* These two use bare fetch (a non-JSON body / no response body), so the 401
       interceptor inside get()/post() cannot reach them — they have to handle
       it themselves. Since authentication was added to the OTBR routes on
       2026-08-15, skipping this would show the user "operation failed" on an
       expired session instead of taking them to the login page. */
    async threadSetState(state) {
        const r = await fetch('/node/state', {
            method: 'PUT',
            headers: { 'Content-Type': 'application/json' },
            body: `"${state}"`
        });
        if (r.status === 401) { App.showLogin(); return false; }
        return r.ok;
    },
    /* The active dataset as hex TLVs — the only form that restores this exact network:
       Home Assistant, `ot-ctl dataset set active <tlvs>` and other vendors' border routers all
       take it. The JSON from threadDataset() cannot be fed back: a JSON PUT creates a NEW
       network with fresh keys (handle_dataset_put in the firmware's otbr_api.c). Bare fetch
       because the body is text, so 401/421 are handled here like threadSetState. Anything
       that is not an even-length hex string is treated as a failure, never as a backup. */
    async threadDatasetTlvs() {
        const r = await API._fetch('/node/dataset/active', { headers: { Accept: 'text/plain' } });
        if (!r) return null;
        if (r.status === 401) { App.showLogin(); return null; }
        if (await API._hostGate(r)) return null;
        if (!r.ok) return null;
        const s = (await r.text()).trim();
        return /^[0-9a-fA-F]+$/.test(s) && s.length % 2 === 0 ? s : null;
    },
    async threadDeleteDataset() {
        const r = await fetch('/node/dataset/active', { method: 'DELETE' });
        if (r.status === 401) { App.showLogin(); return false; }
        return r.ok;
    },
};
