/* Muse page — Direct profile only (docs/15_muse_direct_design_v1.md 10.2).
   Bundled by frontend/direct/build.sh; the HA bundle never contains it (A-29).

   Endpoints (design 10.1, components/webui/muse_api.c, hub_api.c):
     GET/POST /api/v1/muse/config   enabled, sdk_token (set/unset only), display_name, perm, notify
     GET      /api/v1/muse/status   pairing state, link state, BLE self-check
     POST     /api/v1/muse/pair     202 → the hub restarts into the pairing profile (policy C)
     POST     /api/v1/muse/unpair   {"confirm":"UNPAIR"}
     POST     /api/v1/muse/test_push
     GET      /api/v1/muse/audit    ?limit=50

   Contracts that frontend/direct/test_muse.sh pins:
     · The SDK token is write-only. The firmware only ever says "set"/"unset";
       the token input starts empty, is never filled from a response, and is
       cleared after every save attempt.
     · Every switch shows what the hub reports *after* the change (the page
       re-reads /muse/config), never the value it just sent.
     · Pairing and turning Muse on need an admin password; without one the
       buttons are disabled and say so, instead of failing with a 409.
     · The outbound data list mirrors design section 13 row by row (and 5.3: the
       MAC is also readable over Bluetooth while pairing). The "never sent" column
       leaves out WiFi credentials and the SSID: the Direct profile has no WiFi. */

const MUSE_LINK_MAP = {
    disabled: 'Off',
    unpaired: 'Not paired',
    wait_net: 'Waiting for the network',
    low_mem: 'Paused: low memory',
    fetch_vm: 'Looking up the Muse server',
    connecting: 'Connecting',
    registering: 'Registering',
    online: 'Online',
    backoff: 'Retrying shortly',
    refreshing: 'Renewing credentials',
    protocol_error: 'Stopped: Muse changed its protocol; update the firmware',
    not_running: 'Not running',
};

/* last_error is either one of these reason codes or "<step>: <detail>" text
   the firmware has already sanitized; unknown text is shown escaped. */
const MUSE_REASON_MAP = {
    token_revoked: 'Muse revoked the hub\'s access. Pair again.',
    unpaired_in_app: 'The hub was removed in the Muse app. Pair again.',
    local_unpair: 'Unpaired on this page.',
    protocol_changed: 'Muse changed its protocol. Update the hub firmware.',
    registration_rejected: 'Muse rejected the hub\'s registration.',
    credentials_refused: 'Muse refused the hub\'s credentials. Pair again.',
    ws_auth_rejected: 'Muse refused the connection.',
    network_error: 'Network error; retrying.',
    low_memory: 'Low memory; paused.',
};

const MUSE_RESULT_MAP = {
    ok: 'Paired',
    window_timeout: 'Timed out: no app connected',
    cancelled: 'Cancelled',
    decrypt_failed: 'Failed: the app and the hub could not agree on keys',
    confirm_timeout: 'Timed out: the button was not pressed in time',
    auth_failed: 'Failed: Muse rejected the SDK token',
    error_storage: 'Failed: the pairing could not be saved',
    no_sdk_token: 'Failed: no SDK token',
    api_unreachable: 'Failed: Muse could not be reached',
    bt_failed: 'Failed: Bluetooth did not start',
    no_network: 'Failed: no network',
    internal: 'Failed: internal error',
    interrupted: 'Interrupted: the hub restarted',
};

const MUSE_ERR = {
    password_required: 'Set an admin password first (Settings).',
    sdk_token_required: 'Enter your SDK token first.',
    muse_disabled: 'Turn Muse on first.',
    already_pairing: 'Pairing is already running.',
    safe_mode: 'The hub is in safe mode. Restart it normally first.',
    hubdb_not_ready: 'The hub is still starting. Try again in a few seconds.',
    hubdb_busy: 'Storage is busy. Try again in a few seconds.',
    mg24_ota_in_progress: 'A Zigbee firmware update is running. Wait for it to finish.',
    cloud_ota_in_progress: 'A firmware update is running. Wait for it to finish.',
    ota_in_progress: 'A firmware update is running. Wait for it to finish.',
    firmware_not_confirmed: 'The new firmware is not confirmed yet. Try again in a minute.',
    invalid_sdk_token: 'That is not an SDK token: it starts with mgst_ and is 48 characters long.',
    invalid_display_name: 'The name must be 1 to 32 bytes, without control characters.',
    alarm_cannot_be_disabled: 'Alarms are always sent.',
    muse_not_running: 'Muse is not running on this hub right now.',
    not_online: 'Muse is not online yet.',
    busy_try_again: 'The hub is busy. Try again.',
    save_failed: 'Could not save. Try again.',
    audit_unavailable: 'The audit log is not running (Muse did not start on this boot).',
    audit_busy: 'The audit log is busy. Try again.',
};

const MusePage = {
    _timer: null,
    _gen: 0,             // bumped on render/destroy: a reply that lands after leaving the page must not paint
    _cfg: null,
    _st: null,
    _restartTimer: null,
    _pairing: false,     // a pair request is in flight: a second click must not start a second restart probe

    _esc(s) {
        return String(s == null ? '' : s).replace(/[&<>"']/g, c =>
            ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
    },

    _errText(r) {
        if (!r) return t('No response from the hub.');
        const code = r.error || '';
        return MUSE_ERR[code] ? t(MUSE_ERR[code]) : this._esc(code || t('Failed'));
    },

    _msg(id, ok, text) {
        const el = document.getElementById(id);
        if (el) el.innerHTML = text ? `<span class="badge badge-${ok ? 'green' : 'red'}">${text}</span>` : '';
    },

    render(el) {
        this._gen++;
        this._cfg = null;
        this._st = null;
        el.innerHTML = `
        <div class="page-title">${t('Muse')}</div>
        <div id="mu-banner"></div>
        <div id="mu-banner-deg"></div>

        <div class="section-label">${t('Status')}</div>
        <div class="card-grid">
            <div class="card"><div class="card-label">${t('Connection')}</div><div class="card-value" id="mu-link">--</div>
                <div class="text-muted" id="mu-link-why"></div></div>
            <div class="card"><div class="card-label">${t('Pairing')}</div><div class="card-value" id="mu-paired">--</div>
                <div class="text-muted" id="mu-last"></div></div>
            <div class="card"><div class="card-label">${t('SDK token')}</div><div class="card-value" id="mu-tokstate">--</div></div>
            <div class="card"><div class="card-label">${t('Notifications sent')}</div><div class="card-value" id="mu-pushes">--</div>
                <div class="text-muted" id="mu-pushes-sub"></div></div>
        </div>

        <div class="section-label">${t('Turn on')}</div>
        <div class="card">
            <div style="display:flex;align-items:center;gap:12px;margin-bottom:12px">
                <label class="switch"><input type="checkbox" id="mu-en"><span class="slider"></span></label>
                <span>${t('Let Muse reach this hub')}</span>
                <span id="mu-en-msg"></span>
            </div>
            <div class="hint" id="mu-en-why"></div>
            <div class="hint" style="margin-bottom:8px">${t('While Muse is on, the hub keeps an outbound connection to Meta\'s Muse service. Nothing listens for incoming connections.')}</div>
            <table class="table" id="mu-outbound">
                <tr><th>${t('Sent to Meta')}</th><th>${t('Never sent')}</th></tr>
                <tr><td>${t('Display name, firmware version, model')}</td><td>${t('Zigbee and Thread network keys, PSKc')}</td></tr>
                <tr><td>${t('The hub\'s MAC address (the device ID is hatch-link:&lt;mac&gt;; while pairing, anyone nearby can also read it over Bluetooth), your SDK token (to Muse only)')}</td><td>${t('Your admin password or its hash')}</td></tr>
                <tr><td>${t('Device names, rooms, types, capabilities, state, online status, battery, link quality')}</td><td>${t('LAN IP addresses')}</td></tr>
                <tr><td>${t('Thread network name, channel, PAN ID, role, member count, OMR prefix')}</td><td>${t('Raw Zigbee or Matter messages')}</td></tr>
                <tr><td>${t('Device hardware addresses (in device IDs), Thread RLOC16, SRP host names, remaining leases, names advertised by devices waiting to be added')}</td><td></td></tr>
                <tr><td>${t('Matter fabric summary and node IDs')}</td><td></td></tr>
                <tr><td>${t('Hub health: uptime, uplink, free memory, whether the Zigbee radio answers and its network is up')}</td><td></td></tr>
                <tr><td>${t('The text of notifications')}</td><td></td></tr>
            </table>
            <label style="display:flex;align-items:center;gap:8px;margin-top:12px">
                <input type="checkbox" id="mu-ack"> ${t('I understand what is sent to Meta')}
            </label>
        </div>

        <div class="section-label">${t('Account')}</div>
        <div class="card">
            <div class="form-group">
                <label>${t('SDK token')}</label>
                <input id="mu-token" type="password" class="input" maxlength="48" autocomplete="off" spellcheck="false">
                <div class="hint" id="mu-token-hint"></div>
                <div class="hint">${t('Create one at gadgets.muse.ai &rarr; Account &rarr; SDK tokens. The token is yours: the hub does not come with one, and once saved it is never shown again.')}</div>
            </div>
            <button class="btn btn-primary" id="mu-token-save" onclick="MusePage._saveToken()">${t('Save token')}</button>
            <button class="btn btn-sm" id="mu-token-clear" onclick="MusePage._clearToken()">${t('Remove token')}</button>
            <span id="mu-token-msg"></span>
            <div class="form-group" style="margin-top:16px">
                <label>${t('Display name')}</label>
                <input id="mu-name" class="input" maxlength="32">
                <div class="hint">${t('How the hub is called in the Muse app. Up to 32 bytes.')}</div>
            </div>
            <button class="btn btn-primary" onclick="MusePage._saveName()">${t('Save name')}</button>
            <span id="mu-name-msg"></span>
        </div>

        <div class="section-label">${t('Pairing')}</div>
        <div class="card">
            <div id="mu-pair-why" class="hint" style="margin-bottom:8px"></div>
            <button class="btn btn-primary" id="mu-pair" onclick="MusePage._pair()">${t('Pair with Muse')}</button>
            <button class="btn btn-sm" id="mu-test" onclick="MusePage._testPush()">${t('Send a test notification')}</button>
            <button class="btn btn-danger" id="mu-unpair" onclick="MusePage._unpair()">${t('Unpair')}</button>
            <span id="mu-pair-msg"></span>
            <div id="mu-pairing" style="margin-top:12px"></div>
        </div>

        <div class="section-label">${t('What Muse may do')}</div>
        <div class="card">
            <div class="hint" style="margin-bottom:8px">${t('Reading devices and hub status is always allowed once paired. Changes apply right away; Muse re-reads the hub\'s command list.')}</div>
            <div id="mu-perm-bad"></div>
            <table class="table">
                <tr><td>${t('Control devices: lights, switches, covers, thermostats')}</td>
                    <td><label class="switch"><input type="checkbox" id="mu-perm-control" onchange="MusePage._setPerm('control', this.checked)"><span class="slider"></span></label></td></tr>
                <tr><td>${t('Manage devices: rename, change rooms, turn device notifications on or off, add Zigbee or Matter devices')}</td>
                    <td><label class="switch"><input type="checkbox" id="mu-perm-manage" onchange="MusePage._setPerm('manage', this.checked)"><span class="slider"></span></label></td></tr>
                <tr><td><span style="color:var(--red);font-weight:600">${t('Sensitive actions')}</span><br>
                    <span class="hint" style="color:var(--red)">${t('Unlocking doors, and controlling devices where "Restrict Muse control" is ticked on the Devices page (garage doors, gates). Anyone who can talk to your Muse could then open them.')}</span></td>
                    <td><label class="switch"><input type="checkbox" id="mu-perm-sensitive" onchange="MusePage._setPerm('sensitive', this.checked)"><span class="slider"></span></label></td></tr>
            </table>
            <span id="mu-perm-msg"></span>
        </div>

        <div class="section-label">${t('Notifications')}</div>
        <div class="card">
            <table class="table">
                <tr><td>${t('Alarms: smoke, water leak, tamper, sirens. Always sent.')}</td>
                    <td><label class="switch"><input type="checkbox" id="mu-ntf-alarm" checked disabled><span class="slider"></span></label></td></tr>
                <tr><td>${t('Security: door and motion sensors whose own notification switch is on')}</td>
                    <td><label class="switch"><input type="checkbox" id="mu-ntf-security" onchange="MusePage._setNotify('security', this.checked)"><span class="slider"></span></label></td></tr>
                <tr><td>${t('A device went offline')}</td>
                    <td><label class="switch"><input type="checkbox" id="mu-ntf-availability" onchange="MusePage._setNotify('availability', this.checked)"><span class="slider"></span></label></td></tr>
                <tr><td>${t('A new device joined, or adding one failed')}</td>
                    <td><label class="switch"><input type="checkbox" id="mu-ntf-lifecycle" onchange="MusePage._setNotify('lifecycle', this.checked)"><span class="slider"></span></label></td></tr>
                <tr><td>${t('The hub itself: unexpected restarts, Thread network down, Zigbee chip not responding')}</td>
                    <td><label class="switch"><input type="checkbox" id="mu-ntf-gateway" onchange="MusePage._setNotify('gateway', this.checked)"><span class="slider"></span></label></td></tr>
                <tr><td>${t('Low batteries')}</td>
                    <td><label class="switch"><input type="checkbox" id="mu-ntf-battery" onchange="MusePage._setNotify('battery', this.checked)"><span class="slider"></span></label></td></tr>
            </table>
            <div class="hint">${t('Every event is sent to Muse as it happens: the hub does not merge, drop or limit notifications.')}</div>
            <span id="mu-ntf-msg"></span>
        </div>

        <div class="section-label">${t('Audit log')}</div>
        <div class="card">
            <div class="hint" style="margin-bottom:8px">${t('Requests from Muse and the local API, and changes made on this page, newest first. Repeats within a minute are merged into one row (&times;N). Kept in memory only; a restart clears it.')}
                <button class="btn btn-sm" onclick="MusePage._loadAudit()">${t('Refresh')}</button></div>
            <table class="table" id="mu-audit"><tr><td class="text-muted">${t('Loading...')}</td></tr></table>
        </div>`;

        document.getElementById('mu-en').addEventListener('change', e => this._setEnabled(e.target.checked));
        this._loadConfig();
        this._poll();
        this._loadAudit();
        this._timer = setInterval(() => this._poll(), 3000);
    },

    destroy() {
        this._gen++;
        if (this._timer !== null) clearInterval(this._timer);
        if (this._restartTimer !== null) clearInterval(this._restartTimer);
        this._timer = this._restartTimer = null;
        this._pairing = false;
        const tok = document.getElementById('mu-token');
        if (tok) tok.value = '';
    },

    /* ---------------------------------------------------------------- reads */

    async _loadConfig() {
        const gen = this._gen;
        const c = await API.getSoft('/api/v1/muse/config');
        if (gen !== this._gen) return;
        if (!c || c.__status !== 200) {
            /* 503 = storage not ready (just booted) or busy: say so and retry, rather than showing
               "no token / off" — the firmware itself refuses to guess (muse_api.c hubdb_ready) */
            this._cfg = null;
            document.getElementById('mu-banner').innerHTML =
                `<div class="card card-yellow" style="margin-bottom:12px">${this._errText(c)}</div>`;
            setTimeout(() => { if (gen === this._gen) this._loadConfig(); }, 3000);
            this._paintControls();
            return;
        }
        this._cfg = c;
        document.getElementById('mu-banner').innerHTML = '';
        document.getElementById('mu-perm-bad').innerHTML = c.perm_unreadable
            ? `<div class="card card-yellow" style="margin-bottom:8px">${t('The saved permissions could not be read, so Muse is treated as read-only. Set them again below.')}</div>`
            : '';
        this._paintConfig();
        this._paintControls();
    },

    async _poll() {
        const gen = this._gen;
        const s = await API.get('/api/v1/muse/status');
        if (gen !== this._gen || !s) return;
        /* The hub is in the pairing profile (another tab started it, or this page came back mid-pairing):
           its own progress page is what should be on screen */
        if (s.pair_boot === true) { location.reload(); return; }
        this._st = s;
        this._paintStatus();
        this._paintControls();
    },

    async _loadAudit() {
        const gen = this._gen;
        const a = await API.getSoft('/api/v1/muse/audit?limit=128');
        if (gen !== this._gen) return;
        const tbl = document.getElementById('mu-audit');
        if (!tbl) return;
        if (!a || a.__status !== 200 || !Array.isArray(a.entries)) {
            tbl.innerHTML = `<tr><td class="text-muted">${this._errText(a)}</td></tr>`;
            return;
        }
        if (!a.entries.length) {
            tbl.innerHTML = `<tr><td class="text-muted">${t('No requests yet.')}</td></tr>`;
            return;
        }
        const e = s => this._esc(s);
        tbl.innerHTML = `<tr><th>${t('When')}</th><th>${t('From')}</th><th>${t('Command')}</th><th>${t('Device')}</th>
                <th>${t('Action')}</th><th>${t('Result')}</th><th>ms</th></tr>` +
            a.entries.map(x => `<tr>
                <td>${x.time ? e(x.time) : t('{n} s ago', { n: Number(x.age_s) || 0 })}</td>
                <td>${x.source === 'rest' ? t('Local API') : t('Muse')}</td>
                <td><code>${e(x.command)}</code>${Number(x.repeat) > 1 ? ` &times;${Number(x.repeat)}` : ''}</td><td>${e(x.device || '')}</td><td>${e(x.action || '')}</td>
                <td>${x.result === 'ok' ? `<span class="badge badge-green">ok</span>` : `<span class="badge badge-red">${e(x.result)}</span>`}</td>
                <td>${Number(x.ms) || 0}</td></tr>`).join('') +
            (a.truncated ? `<tr><td colspan="7" class="text-muted">${t('More entries did not fit.')}</td></tr>` : '');
    },

    /* ---------------------------------------------------------------- painting */

    _paintConfig() {
        const c = this._cfg;
        const $ = id => document.getElementById(id);
        $('mu-en').checked = !!c.enabled;
        /* Show the ack box ticked once Muse is on: the user already agreed when turning it on */
        if (c.enabled) $('mu-ack').checked = true;
        $('mu-token-hint').textContent = c.sdk_token === 'set'
            ? t('A token is saved. Enter a new one to replace it.')
            : t('No token saved.');
        $('mu-token').placeholder = c.sdk_token === 'set' ? t('(saved; hidden)') : 'mgst_...';
        if (document.activeElement !== $('mu-name')) $('mu-name').value = c.display_name || '';
        for (const k of ['control', 'manage', 'sensitive'])
            $('mu-perm-' + k).checked = !!(c.perm && c.perm[k]);
        for (const k of ['security', 'availability', 'lifecycle', 'gateway', 'battery'])
            $('mu-ntf-' + k).checked = !!(c.notify && c.notify[k]);
    },

    _paintStatus() {
        const s = this._st, l = s.link || {};
        const $ = id => document.getElementById(id);
        const state = l.state || 'not_running';
        const color = state === 'online' ? 'green'
            : (state === 'disabled' || state === 'unpaired' || state === 'not_running') ? 'blue'
            : (state === 'protocol_error' || state === 'low_mem') ? 'red' : 'yellow';
        $('mu-link').innerHTML = `<span class="badge badge-${color}">${MUSE_LINK_MAP[state] ? t(MUSE_LINK_MAP[state]) : this._esc(state)}</span>`;
        const why = l.last_error || '';
        $('mu-link-why').innerHTML = why ? (MUSE_REASON_MAP[why] ? t(MUSE_REASON_MAP[why]) : this._esc(why))
            : (state === 'online' && l.vm_name ? this._esc(l.vm_name) : '');
        $('mu-paired').textContent = s.paired ? t('Paired') : t('Not paired');
        const r = s.last_result;
        $('mu-last').textContent = r && r !== 'none'
            ? t('Last attempt: {r}', { r: MUSE_RESULT_MAP[r] ? t(MUSE_RESULT_MAP[r]) : r }) : '';
        $('mu-tokstate').textContent = s.sdk_token === 'set' ? t('Saved') : t('Not set');
        const p = l.pushes;
        $('mu-pushes').textContent = p ? String(p.sent) : '--';
        $('mu-pushes-sub').textContent = p ? t('{q} waiting, {d} dropped', { q: p.queued, d: p.dropped }) : '';

        /* Design 3.3 (pushes only state facts, MD-10; the advice lives here): after two degraded self-checks in a row,
           tell the user to power-cycle.
           A plain reset does not help — a CPU reset is exactly what did not clear it. */
        const sc = s.blew && s.blew.selfcheck;
        $('mu-banner-deg').innerHTML = sc && sc.consecutive_degraded >= 2
            ? `<div class="card card-red" style="margin-bottom:12px">
                ${t('Thread has worked worse after the last two pairing windows. Unplug the hub (and PoE) for 10 seconds, then plug it back in. Pressing the reset button is not enough.')}</div>`
            : '';
    },

    /* Buttons depend on both config (password, token, enabled) and status (paired, online) */
    _paintControls() {
        const $ = id => document.getElementById(id);
        const c = this._cfg, s = this._st;
        const known = !!c;
        const pw = known && c.password_set;
        const reasons = [];
        if (known && !pw) reasons.push(t('Set an admin password first (Settings).'));
        if (known && c.sdk_token !== 'set') reasons.push(t('Save your SDK token first.'));
        if (known && !c.enabled) reasons.push(t('Turn Muse on first.'));
        $('mu-pair').disabled = !known || reasons.length > 0 || this._pairing;
        $('mu-pair-why').innerHTML = reasons.join('<br>');
        $('mu-en').disabled = !known || (!pw && !c.enabled);
        $('mu-en-why').textContent = known && !pw && !c.enabled ? t('Set an admin password first (Settings).') : '';
        $('mu-token-save').disabled = !known || !pw;
        $('mu-perm-sensitive').disabled = !known || (!pw && !(c.perm && c.perm.sensitive));
        const paired = !!(s ? s.paired : known && c.paired);
        $('mu-unpair').style.display = paired ? '' : 'none';
        $('mu-pair').textContent = paired ? t('Pair again') : t('Pair with Muse');
        $('mu-test').style.display = s && s.link && s.link.state === 'online' ? '' : 'none';
        for (const id of ['mu-perm-control', 'mu-perm-manage', 'mu-ntf-security', 'mu-ntf-availability',
                          'mu-ntf-lifecycle', 'mu-ntf-gateway', 'mu-ntf-battery', 'mu-name'])
            $(id).disabled = !known;
    },

    /* ---------------------------------------------------------------- writes */

    async _post(body, msgId) {
        const r = await API.post('/api/v1/muse/config', body);
        const ok = !!(r && r.status === 'ok');
        this._msg(msgId, ok, ok ? t('Saved') : this._errText(r));
        await this._loadConfig();          // show what the hub now has, not what we sent
        return ok;
    },

    async _setEnabled(on) {
        const $ = id => document.getElementById(id);
        if (on && !$('mu-ack').checked) {
            $('mu-en').checked = false;
            this._msg('mu-en-msg', false, t('Tick "I understand what is sent to Meta" first.'));
            return;
        }
        await this._post({ enabled: on }, 'mu-en-msg');
    },

    async _saveToken() {
        const inp = document.getElementById('mu-token');
        const tok = inp.value.trim();
        inp.value = '';                    // never keep the token in the DOM, whatever the outcome
        if (!/^mgst_[A-Za-z0-9_-]{43}$/.test(tok)) {
            this._msg('mu-token-msg', false, t(MUSE_ERR.invalid_sdk_token));
            return;
        }
        await this._post({ sdk_token: tok }, 'mu-token-msg');
    },

    async _clearToken() {
        if (!confirm(t('Remove the saved SDK token? Pairing again will need a token.'))) return;
        await this._post({ sdk_token: null }, 'mu-token-msg');
    },

    async _saveName() {
        const name = document.getElementById('mu-name').value.trim();
        if (!name || new TextEncoder().encode(name).length > 32) {
            this._msg('mu-name-msg', false, t(MUSE_ERR.invalid_display_name));
            return;
        }
        await this._post({ display_name: name }, 'mu-name-msg');
    },

    async _setPerm(key, on) {
        if (key === 'sensitive' && on &&
            !confirm(t('Allow Muse to unlock doors and to control devices where "Restrict Muse control" is ticked?') + '\n\n' +
                     t('Anyone who can talk to your Muse could then open them.'))) {
            document.getElementById('mu-perm-sensitive').checked = false;
            return;
        }
        await this._post({ perm: { [key]: on } }, 'mu-perm-msg');
    },

    async _setNotify(key, on) {
        await this._post({ notify: { [key]: on } }, 'mu-ntf-msg');
    },

    async _testPush() {
        const r = await API.post('/api/v1/muse/test_push', {});
        const ok = !!(r && r.status === 'queued');
        this._msg('mu-pair-msg', ok, ok ? t('Queued. It should reach the Muse app within a minute.') : this._errText(r));
    },

    async _unpair() {
        if (!confirm(t('Unpair the hub from Muse?') + '\n\n' +
                     t('The hub forgets its Muse credentials; your SDK token and settings stay. Remove the hub in the Muse app as well.'))) return;
        const r = await API.post('/api/v1/muse/unpair', { confirm: 'UNPAIR' });
        const ok = !!(r && r.status === 'ok');
        this._msg('mu-pair-msg', ok, ok ? t('Unpaired. Remove the hub in the Muse app too.') : this._errText(r));
        this._poll();
    },

    async _pair() {
        if (this._pairing) return;
        /* window_s comes with every /muse/status; before the first one arrives, use the Kconfig default (180 s) */
        const win = Math.max(1, Math.round((this._st && this._st.window_s ? this._st.window_s : 180) / 60));
        if (!confirm(t('Pair with Muse?') + '\n\n' +
                     '- ' + t('The hub restarts into pairing mode and opens a Bluetooth window for about {n} minutes. Zigbee and Thread devices are offline until it restarts back.', { n: win }) + '\n' +
                     '- ' + t('Turn on Developer mode in the Muse app first.') + '\n' +
                     '- ' + t("When the app asks, hold the hub's button for 2 to 10 seconds, then release it.") + '\n' +
                     '  ' + t('A press shorter than 2 seconds does nothing while pairing; 10 seconds or longer resets the hub to factory settings.') + '\n' +
                     '- ' + t('Only press the button when your own Muse app is asking for it.'))) return;
        const gen = this._gen;
        this._pairing = true;
        this._paintControls();
        const r = await API.post('/api/v1/muse/pair', {});
        if (gen !== this._gen) return;
        if (!r || r.status !== 'accepted') {
            this._pairing = false;
            this._paintControls();
            this._msg('mu-pair-msg', false, this._errText(r));
            return;
        }
        this._msg('mu-pair-msg', true, t('Restarting into pairing mode...'));
        this._watchRestart();
    },

    /* After a 202 the hub restarts into the pairing profile, which serves its own small progress page
       (muse_api.c PAIR_PAGE). Wait until /api/v1/status says mode "muse_pair", then reload onto it.
       If it comes back in normal mode instead (pairing ended early, e.g. no network), reload here so
       the result shows. The pairing profile's /api/v1/status never asks for a login; the normal one
       does when "REST authentication" is on — so after an outage a 401 also means "back, in normal
       mode" (the session did not survive the restart; the reload shows the login page). */
    _watchRestart() {
        const gen = this._gen;
        const started = Date.now();
        let wentDown = false;
        if (this._timer !== null) clearInterval(this._timer);
        this._timer = null;
        const box = document.getElementById('mu-pairing');
        box.innerHTML = `<div class="hint">${t('Waiting for the hub to restart. This page switches to the pairing view by itself.')}</div>`;
        this._restartTimer = setInterval(async () => {
            if (gen !== this._gen) return;
            let st = null;
            try {
                const res = await fetch('/api/v1/status', { cache: 'no-store' });
                st = res.ok ? await res.json() : res.status === 401 ? { normal_needs_login: true } : null;
            } catch (e) { st = null; }
            if (gen !== this._gen) return;
            if (!st) wentDown = true;
            if (st && st.mode === 'muse_pair') { location.reload(); return; }
            if (st && wentDown) { location.reload(); return; }
            if (Date.now() - started > 120000) {
                clearInterval(this._restartTimer);
                this._restartTimer = null;
                box.innerHTML = `<div class="hint">${t('The hub has not come back yet. Reload this page in a minute.')}</div>`;
            }
        }, 2000);
    },
};

App.registerPage('muse', MusePage);
