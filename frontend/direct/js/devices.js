/* Devices page — Direct profile only (docs/15_muse_direct_design_v1.md 10.2, the device page).
   Bundled by frontend/direct/build.sh; the HA bundle never contains it (A-29).

   Endpoints (components/webui/hub_api.c, hub_core/hub_cmds.c):
     GET  /api/v1/hub/devices[?cursor=N]       = hub.devices.list; pages via truncated / next_cursor
     POST /api/v1/hub/invoke  hub.device.update   {id, name?, room?, notify?}
                              hub.device.control  {id, action, value?}   value is a STRING ("40")
                              hub.zigbee.permit_join {seconds}           30–254, 0 closes
     POST /api/v1/hub/device/sensitive {id, sensitive}   local only: Muse can never change it (design 6.2)
     GET  /api/v1/zigbee/status                hub.join_window.left_s for the pairing countdown
     POST /api/v1/hub/invoke  hub.matter.commission {setup_code, name?}  → 202 {job}; GET /api/v1/hub/job?id= until not running
     POST /api/v1/hub/device/remove {id, confirm:"REMOVE", force?}     Matter only → 202 {job} (same polling)
     GET  /api/v1/matter/status                allow_test_devices; fail_step "ident_lost" shows the reset
     POST /api/v1/matter/allow_test_devices {allow}   POST /api/v1/matter/reset {confirm:"RESET"}   local only (design 10.1)

   Contracts that frontend/direct/test_hub_devices.sh pins (design 10.2, new frontend tests):
     · Everything shown comes from the hub's answer; after a change the page shows what the hub reports
       afterwards (the control result's state, a re-read list), never the value it just sent.
     · Requests match the firmware's contracts (only the changed fields; brightness as a string).
     · Device-provided strings (names, rooms, models) are escaped before they reach innerHTML.
     · Writes without an admin password answer 409 password_required: say so instead of "Failed".
     · Removing the "restrict Muse control" (sensitive) mark asks first: it lets Muse control the device again.
     · That mark is offered only on devices Muse could control (or where it is already set), and the line under it says
       what is true now from the Muse permissions (GET /api/v1/muse/config perm).
   Re-interview (design 10.2) is not offered yet. Removing a Matter device asks first, and offers "remove anyway" only after the
   device did not answer (the hub then forgets it; it must be reset in its own app). */

const DEV_ERR = {
    password_required: 'Set an admin password first (Settings).',
    rate_limited: 'Too many requests. Wait a few seconds and try again.',
    not_found: 'The hub no longer knows this device.',
    unsupported: 'This device cannot do that yet.',
    unavailable: 'The hub could not do that right now. Try again in a minute.',
    busy: 'The hub is busy. Try again.',
    invalid_params: 'The hub rejected the request.',
    limit_reached: 'The hub cannot take more devices of this kind.',
    conflict: 'Not possible right now.',
    forbidden: 'Not allowed.',
};

const DEV_KIND_MAP = {
    dimmable_light: 'Dimmable light', light: 'Light', color_light: 'Color light', switch: 'Switch', plug: 'Plug',
    climate_sensor: 'Climate sensor', motion_sensor: 'Motion sensor', contact_sensor: 'Contact sensor',
    leak_sensor: 'Leak sensor', smoke_sensor: 'Smoke sensor', remote: 'Remote', cover: 'Cover', lock: 'Lock',
    thermostat: 'Thermostat', sensor: 'Sensor',
};

const DevicesPage = {
    _gen: 0,             // bumped on render/destroy: a reply that lands after leaving the page must not paint
    _timer: null,
    _winTimer: null,
    _jobTimer: null,     // one Matter add / remove at a time (the hub does one too); polls GET /api/v1/hub/job
    _jobBusy: false,     // set before the POST, cleared when the job ends or the request is refused
    _mtAllow: false,     // what the hub last reported for "allow test devices"
    _musePerm: null,     // Muse permissions {control, manage, sensitive} from the Muse page; null = unknown
    _ctlBusy: {},        // device key → a control command is in flight (one at a time per device)
    _loading: false,     // a list load is running (the 2 s poll skips a tick instead of stacking loads)
    _loadT: 0,           // when the last load started (Date.now())
    _loadPages: 0,       // how many pages the last load read
    _paintSig: '',       // what the list showed last time, minus the ages that tick every poll (see _paint)
    _paintT: 0,
    _roomsHtml: '',      // the room filter's options as last written (rebuilt only when they change)
    _devs: [],
    _proto: '',
    _room: '',

    _esc(s) {
        return String(s == null ? '' : s).replace(/[&<>"']/g, c =>
            ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
    },

    _errText(r) {
        if (!r) return t('No response from the hub.');
        const code = r.error || '';
        const base = Object.prototype.hasOwnProperty.call(DEV_ERR, code) ? t(DEV_ERR[code]) : this._esc(code || t('Failed'));
        /* invalid_params carries the firmware's reason (e.g. "name is at most 32 bytes in UTF-8"): show it, escaped */
        return code === 'invalid_params' && r.message ? `${base} ${this._esc(r.message)}` : base;
    },

    _bytes(v) { return new TextEncoder().encode(String(v)).length; },   // the firmware limits names in UTF-8 bytes, not characters

    _msg(id, ok, text) {
        const el = document.getElementById(id);
        if (el) el.innerHTML = text ? `<span class="badge badge-${ok ? 'green' : 'red'}">${text}</span>` : '';
    },

    render(el) {
        this._gen++;
        this._devs = [];
        this._ctlBusy = {};
        el.innerHTML = `
        <div class="page-title">${t('Devices')}</div>
        <div class="card" style="margin-bottom:12px">
            <button class="btn btn-primary" id="dv-pair" onclick="DevicesPage._pair(120)">${t('Add Zigbee device')}</button>
            <button class="btn btn-sm" id="dv-pair-stop" onclick="DevicesPage._pair(0)">${t('Stop adding')}</button>
            <span id="dv-pair-msg"></span>
            <div class="hint" id="dv-pair-hint">${t('Opens the Zigbee network for 2 minutes. Then put the device into pairing mode (see its manual).')}</div>
        </div>
        <div class="card" style="margin-bottom:12px">
            <div class="form-group" style="display:flex;gap:8px;flex-wrap:wrap;align-items:center">
                <input class="input" id="dv-mt-code" maxlength="64" placeholder="${t('Matter pairing code')}">
                <input class="input" id="dv-mt-name" maxlength="32" placeholder="${t('Name (optional)')}">
                <button class="btn btn-primary" id="dv-mt-add" onclick="DevicesPage._mtAdd()">${t('Add Matter device')}</button>
            </div>
            <span id="dv-mt-msg"></span>
            <div class="hint">${t('The device must already work in another app (Apple Home, Google Home, Home Assistant…). In that app, turn on pairing mode for it to get a pairing code, then enter the code here. Adding takes up to two minutes.')}</div>
            <label style="display:flex;align-items:center;gap:8px;margin-top:8px"><input type="checkbox" id="dv-mt-test" disabled onchange="DevicesPage._mtTest(this.checked)">
                ${t('Allow uncertified test devices (developer boards, DIY)')}</label>
            <span id="dv-mt-test-msg"></span>
            <div id="dv-mt-reset" style="display:none;margin-top:8px">
                <div style="color:var(--red)">${t('The hub lost its Matter identity. Matter devices added earlier can no longer be reached from this hub. Resetting creates a new identity; then share each device to the hub again from its own app.')}</div>
                <input class="input" id="dv-mt-reset-txt" maxlength="8" placeholder="${t('Type RESET')}">
                <button class="btn btn-sm" id="dv-mt-reset-btn" onclick="DevicesPage._mtReset()">${t('Reset the Matter controller')}</button>
                <span id="dv-mt-reset-msg"></span>
            </div>
        </div>
        <div style="display:flex;gap:12px;align-items:center;margin-bottom:12px;flex-wrap:wrap">
            <label>${t('Protocol')}
                <select class="input" id="dv-proto" onchange="DevicesPage._filter()">
                    <option value="">${t('All')}</option><option value="zigbee">Zigbee</option><option value="matter">Matter</option><option value="thread">Thread</option>
                </select></label>
            <label>${t('Room')} <select class="input" id="dv-room" onchange="DevicesPage._filter()"></select></label>
            <span id="dv-msg"></span>
            <span id="dv-rm-msg"></span>
        </div>
        <div id="dv-list"><div class="text-muted">${t('Loading…')}</div></div>`;
        this._load();
        this._mtStatus();
        this._museLoad();
        // 2 s: a door opening or someone walking past shows up within a couple of seconds (2026-10-24: was 10 s). Each page of the
        // list is one read from the REST read bucket (2 per second, burst 5; design 6.5 note): a tick is skipped while a load is
        // still running, and a long list (several pages) waits about a second per page so clicks on the page still get through
        this._timer = setInterval(() => this._poll(), 2000);
    },

    destroy() {
        this._gen++;
        if (this._timer != null) clearInterval(this._timer);       // != null: a timer handle can be 0
        if (this._winTimer != null) clearInterval(this._winTimer);
        if (this._jobTimer != null) clearInterval(this._jobTimer);
        this._timer = this._winTimer = this._jobTimer = null;
        this._jobBusy = false;
    },

    _poll() {
        if (document.hidden) return;                               // a background tab does not spend the read bucket
        if (this._loading || performance.now() - this._loadT < this._loadPages * 1000) return;
        this._load(true);
    },

    // fromPoll: the 2 s poll (may skip an unchanged repaint); everything else — opening the page, after the user changed
    // something — repaints, so a control snaps back to what the hub reports
    async _load(fromPoll) {
        const gen = this._gen;
        this._loading = true;
        this._loadT = performance.now();                           // monotonic: a clock change must not stop the poll
        try { this._loadPages = await this._loadPaged(gen, fromPoll); } finally { if (gen === this._gen) this._loading = false; }
    },

    // returns how many pages it read (the next poll waits about a second per page, see render)
    async _loadPaged(gen, fromPoll) {
        const all = [];
        let cursor = '', pages = 0;
        for (let page = 0; page < 8; page++) {
            const r = await API.getSoft('/api/v1/hub/devices' + (cursor ? '?cursor=' + encodeURIComponent(cursor) : ''));
            pages++;
            if (gen !== this._gen) return pages;
            if (!r || r.__status !== 200 || !r.ok || !r.result) {
                this._msg('dv-msg', false, this._errText(r));
                return pages;
            }
            all.push(...(r.result.devices || []));
            if (!r.result.truncated || !r.result.next_cursor) break;
            cursor = String(r.result.next_cursor);
        }
        this._msg('dv-msg', true, '');
        this._devs = all;
        if (!this._editing()) this._paint(!fromPoll);   // don't wipe what the user is typing; the next poll paints
        return pages;
    },

    _editing() {
        const a = document.activeElement;
        return !!(a && a.id && /^dv-(name|room|lvl)-|^dv-(room|proto)$/.test(a.id));   // also a dragged slider, an open filter list
    },

    _filter() {
        this._proto = (document.getElementById('dv-proto') || {}).value || '';
        this._room = (document.getElementById('dv-room') || {}).value || '';
        this._paint(true);
    },

    /* The list polls every 2 s. Repainting every time would wipe the result line under a card ("Sending…", "No answer…") and eat a
       click that lands between press and release, so: repaint only when something shown changed — the ages ("12 s ago") alone
       refresh at most every 30 s — and carry the result lines over a repaint. */
    _paint(force) {
        const sig = JSON.stringify([this._proto, this._room, this._musePerm,
                                    this._devs.map(d => Object.assign({}, d, { last_seen_s: undefined, state_age_s: undefined }))]);
        const now = performance.now();
        if (!force && sig === this._paintSig && now - this._paintT < 30000 && document.getElementById('dv-list') === this._paintEl) return;
        this._paintSig = sig;
        this._paintT = now;
        const sel = document.getElementById('dv-room');
        if (sel) {
            const rooms = [...new Set(this._devs.map(d => d.room).filter(Boolean))].sort();
            if (this._room && !rooms.includes(this._room)) this._room = '';
            const rh = `<option value="">${t('All')}</option>` +
                rooms.map(r => `<option value="${this._esc(r)}"${r === this._room ? ' selected' : ''}>${this._esc(r)}</option>`).join('');
            if (rh !== this._roomsHtml || sel !== this._roomsEl) { sel.innerHTML = rh; this._roomsHtml = rh; this._roomsEl = sel; }
        }
        const list = document.getElementById('dv-list');
        this._paintEl = list;
        if (!list) return;
        const shown = this._devs.filter(d => (!this._proto || d.protocol === this._proto) && (!this._room || d.room === this._room));
        if (!shown.length) {
            list.innerHTML = `<div class="text-muted">${this._devs.length ? t('No devices match the filter.') : t('No devices yet.')}</div>`;
            return;
        }
        const keep = {};
        for (const d of shown) {
            const k = this._keyOf(d.id), e = document.getElementById('dv-res-' + k);
            if (e && e.innerHTML) keep[k] = e.innerHTML;
        }
        list.innerHTML = `<div class="card-grid">${shown.map(d => this._card(d)).join('')}</div>`;
        for (const k in keep) {
            const e = document.getElementById('dv-res-' + k);
            if (e) e.innerHTML = keep[k];
        }
    },

    _fmtState(st) {
        if (!st) return '';
        const out = [];
        if ('onoff' in st) out.push(st.onoff ? t('On') : t('Off'));
        if ('level_pct' in st) out.push(`${t('Brightness')} ${st.level_pct}%`);
        if ('temperature_c' in st) out.push(`${st.temperature_c} °C`);
        if ('humidity_pct' in st) out.push(`${st.humidity_pct}% RH`);
        if ('pressure_hpa' in st) out.push(`${st.pressure_hpa} hPa`);
        if ('motion' in st) out.push(st.motion ? t('Motion') : t('No motion'));
        // contact is the string "open" / "closed" (hub_zigbee.c / hub_matter.c write_state): a truthy check showed an open door as "Closed"
        if ('contact' in st) out.push(st.contact === 'open' ? t('Open') : t('Closed'));
        if ('water_leak' in st) out.push(st.water_leak ? t('Leak!') : t('Dry'));
        if ('smoke' in st) out.push(st.smoke ? t('Smoke!') : t('No smoke'));
        if ('illuminance_lux' in st) out.push(`${st.illuminance_lux} lx`);   // the firmware key (was read as "lux" and never shown)
        if ('power_w' in st) out.push(`${st.power_w} W`);
        if ('energy_kwh' in st) out.push(`${st.energy_kwh} kWh`);
        return out.map(s => this._esc(s)).join(' · ');
    },

    _ago(s) {
        if (s == null) return t('not seen since the hub started');
        s = Number(s);
        if (s < 60) return t('{n} s ago').replace('{n}', s);
        if (s < 3600) return t('{n} min ago').replace('{n}', Math.floor(s / 60));
        return t('{n} h ago').replace('{n}', Math.floor(s / 3600));
    },

    _card(d) {
        const id = this._esc(d.id);
        const key = this._keyOf(d.id);   // handlers get only [a-z0-9]: device ids never land inside an onclick string
        if (d.protocol === 'thread') {   // Thread members: read only (not managed by this hub, appendix A). Matter devices on the hub's own fabric are managed (mt-, design 7.3)
            return `<div class="card"><div class="card-label">Thread · ${this._esc(d.role || '')}</div>
                <div class="card-value" style="font-size:1rem">${id}</div>
                <div class="text-muted">${d.online ? t('Online') : t('Offline')} · ${this._ago(d.last_seen_s)}</div></div>`;
        }
        const st = d.state || {};
        const caps = d.capabilities || [];
        const kind = Object.prototype.hasOwnProperty.call(DEV_KIND_MAP, d.kind) ? t(DEV_KIND_MAP[d.kind]) : this._esc(d.kind || t('Device'));
        const name = d.name ? this._esc(d.name) : `<span class="text-muted">${t('Unnamed')} ${kind}</span>`;
        const batt = 'battery_pct' in st ? ` · ${t('Battery')} ${Number(st.battery_pct)}%` : '';
        const link = d.link ? ` · LQI ${this._esc(d.link.lqi)} / ${this._esc(d.link.rssi)} dBm` : '';
        // Matter devices report no availability until subscriptions arrive (M7 step 5): say "Matter", do not claim online / offline
        const seen = d.protocol === 'matter' && d.online === undefined ? 'Matter'
            : `${d.online === false ? `<span style="color:var(--red)">${t('Offline')}</span>` : t('Online')} · ${this._ago(d.last_seen_s)}`;
        const notifiable = ['contact', 'motion', 'vibration'].some(c => caps.includes(c));   // the notify bit only gates door / window / motion pushes (design 6.7)
        let ctl = '';
        const busy = this._ctlBusy[key] ? ' disabled' : '';     // a repaint during a command keeps its buttons disabled
        if (d.controllable && d.online !== false) {
            if (caps.includes('onoff'))
                ctl += `<button class="btn btn-sm" id="dv-on-${key}"${busy} onclick="DevicesPage._ctl('${key}','turn_on')">${t('On')}</button>
                        <button class="btn btn-sm" id="dv-off-${key}"${busy} onclick="DevicesPage._ctl('${key}','turn_off')">${t('Off')}</button>`;
            if (caps.includes('level'))
                ctl += ` <input type="range" min="1" max="100" id="dv-lvl-${key}"${busy} value="${Number(st.level_pct) || 100}"
                         onchange="DevicesPage._ctl('${key}','set_brightness',this.value)">`;
        }
        return `<div class="card">
            <div class="card-label">${kind}${d.model ? ' · ' + this._esc(d.model) : ''}</div>
            <div class="card-value" style="font-size:1rem">${name}</div>
            <div class="text-muted">${d.room ? this._esc(d.room) + ' · ' : ''}${seen}${batt}${link}</div>
            <div id="dv-state-${key}" style="margin:6px 0">${this._fmtState(st) || '<span class="text-muted">' + t('No reading yet') + '</span>'}</div>
            <div>${ctl}</div>
            <div class="form-group" style="margin-top:8px">
                <input class="input" id="dv-name-${key}" maxlength="32" placeholder="${t('Name')}" value="${this._esc(d.name || '')}">
                <input class="input" id="dv-room-${key}" maxlength="24" placeholder="${t('Room')}" value="${this._esc(d.room || '')}">
                <button class="btn btn-sm" onclick="DevicesPage._save('${key}')">${t('Save')}</button>
            </div>
            ${notifiable ? `<label style="display:flex;align-items:center;gap:8px"><input type="checkbox" id="dv-notify-${key}"${d.notify ? ' checked' : ''}
                onchange="DevicesPage._notify('${key}', this.checked)"> ${t('Tell Muse when it changes (doors, windows, motion)')}</label>` : ''}
            ${this._sensBlock(d, key)}
            ${d.protocol === 'matter' ? `<button class="btn btn-sm" id="dv-rm-${key}" onclick="DevicesPage._mtRemove('${key}')">${t('Remove from hub')}</button>` : ''}
            <span id="dv-res-${key}"></span>
        </div>`;
    },

    /* "Restrict Muse control" (the device's sensitive mark, design 6.2; local only). Shown only where it changes something — devices Muse
       could control — plus wherever it is already set, so it can be removed. The line under it says what is true right now, from the
       Muse permissions on the Muse page (GET /api/v1/muse/config perm), instead of asking the user to work it out. */
    _sensBlock(d, key) {
        if (!d.controllable && !d.sensitive) return '';
        const box = `<label style="display:flex;align-items:center;gap:8px"><input type="checkbox" id="dv-sens-${key}"${d.sensitive ? ' checked' : ''}
                onchange="DevicesPage._sensitive('${key}', this.checked)"> ${t('Restrict Muse control (for locks, garage doors, gates, heaters)')}</label>`;
        return `${box}<div class="hint" id="dv-sens-now-${key}">${this._sensNow(d)}</div>`;
    },

    _sensNow(d) {
        const p = this._musePerm;
        if (!d.controllable) return t('This device cannot be controlled, so the mark has no effect here.');
        if (!p) return '';                                       // Muse not set up / permissions unknown: say nothing rather than guess
        if (!p.control) return t('Muse is not allowed to control any device (Muse page), so this makes no difference now.');
        if (!d.sensitive) return t('Now: Muse can control it.');
        return p.sensitive ? t('Now: Muse can still control it, because "Sensitive actions" is on (Muse page).')
                           : `<span style="color:var(--red)">${t('Now: Muse cannot control it ("Sensitive actions" is off on the Muse page). You can still control it here.')}</span>`;
    },

    async _museLoad() {
        const gen = this._gen;
        const r = await API.getSoft('/api/v1/muse/config');
        if (gen !== this._gen) return;
        this._musePerm = r && r.__status === 200 && r.perm ? r.perm : null;
        if (!this._editing()) this._paint();
    },

    _keyOf(id) { return String(id).replace(/[^a-z0-9]/gi, ''); },
    _byKey(key) { return this._devs.find(x => this._keyOf(x.id) === key) || null; },

    async _invoke(command, params) {
        return API.post('/api/v1/hub/invoke', { command, params });
    },

    /* One command per device at a time: the hub waits for the device's answer (up to ~10 s when it does not answer), and a burst of
       clicks on a device that has dropped off the network kept the hub's web server busy for minutes (2026-10-24: its watchdog
       restarted the hub). Its buttons stay disabled until the answer is back. */
    _ctlButtons(key, off) {
        for (const p of ['dv-on-', 'dv-off-', 'dv-lvl-']) {
            const e = document.getElementById(p + key);
            if (e) e.disabled = off;
        }
    },

    async _ctl(key, action, value) {
        const dev = this._byKey(key);
        if (!dev) return;
        if (this._ctlBusy[key]) { this._msg('dv-res-' + key, false, t('Still waiting for the device to answer the last command.')); return; }
        const tok = {};                                           // a repaint resets _ctlBusy: an old command's finally must not free a newer one
        this._ctlBusy[key] = tok;
        this._ctlButtons(key, true);
        this._msg('dv-res-' + key, true, t('Sending… waiting for the device'));
        let timer = null;                                         // the hub answers within ~10 s even when the device does not; a request
        try {                                                     // that hangs (the network went quiet) must not grey the buttons out for good
            const late = new Promise(r => { timer = setTimeout(() => r('late'), 30000); });
            if (await Promise.race([this._ctlSend(key, dev.id, action, value), late]) === 'late')
                this._msg('dv-res-' + key, false, t('No answer from the hub yet. Check the connection and try again.'));
        } finally {
            clearTimeout(timer);
            if (this._ctlBusy[key] === tok) {
                delete this._ctlBusy[key];
                this._ctlButtons(key, false);
            }
        }
    },

    async _ctlSend(key, id, action, value) {
        const params = { id, action };
        if (value !== undefined) params.value = String(value);   // hub_params: value must be a string
        const r = await this._invoke('hub.device.control', params);
        if (!r || !r.ok) { this._msg('dv-res-' + key, false, this._errText(r)); return; }
        const res = r.result || {};
        const ok = res.result === 'confirmed' || res.result === 'delivered' || res.result === 'pending';
        this._msg('dv-res-' + key, ok, this._esc(res.detail || res.result || ''));
        const d = this._byKey(key);
        if (d && res.state) d.state = res.state;            // what the hub reports after the command, not what was sent
        const sEl = document.getElementById('dv-state-' + key);
        if (sEl && d) sEl.innerHTML = this._fmtState(d.state);
    },

    async _save(key) {
        const gen = this._gen;
        const d = this._byKey(key);
        if (!d) return;
        const id = d.id;
        const name = (document.getElementById('dv-name-' + key) || {}).value;
        const room = (document.getElementById('dv-room-' + key) || {}).value;
        const params = { id };
        if (name !== undefined && name !== (d.name || '')) params.name = name;
        if (room !== undefined && room !== (d.room || '')) params.room = room;
        if (Object.keys(params).length === 1) { this._msg('dv-res-' + key, true, t('Nothing changed')); return; }
        if (this._bytes(params.name || '') > 32) { this._msg('dv-res-' + key, false, t('The name is at most 32 bytes (about 10 Chinese characters).')); return; }
        if (this._bytes(params.room || '') > 24) { this._msg('dv-res-' + key, false, t('The room is at most 24 bytes (about 8 Chinese characters).')); return; }
        const r = await this._invoke('hub.device.update', params);
        if (gen !== this._gen) return;
        const a = document.activeElement;
        if (a && a.id && /^dv-(name|room)-/.test(a.id) && a.blur) a.blur();   // only our own inputs: let the repaint show the saved value
        await this._load();
        this._msg('dv-res-' + key, !!(r && r.ok), r && r.ok ? t('Saved') : this._errText(r));   // after the repaint, or it is wiped at once
    },

    async _notify(key, on) {
        const gen = this._gen;
        const d = this._byKey(key);
        if (!d) return;
        const r = await this._invoke('hub.device.update', { id: d.id, notify: !!on });
        if (gen !== this._gen) return;
        await this._load();                                        // the box shows the hub's value after the change
        if (!r || !r.ok) this._msg('dv-res-' + key, false, this._errText(r));
        else if (on && r.result && r.result.notify_active === false)   // the "doors, windows and motion" category is off on the Muse page
            this._msg('dv-res-' + key, false, t('Saved, but door / window / motion notifications are off on the Muse page.'));
    },

    async _sensitive(key, on) {
        const gen = this._gen;
        const d = this._byKey(key);
        if (!d) return;
        if (!on && !confirm(t('Stop restricting Muse? Muse will then be able to control this device with its ordinary control permission.'))) {
            await this._load();
            return;
        }
        const r = await API.post('/api/v1/hub/device/sensitive', { id: d.id, sensitive: !!on });
        if (gen !== this._gen) return;
        await this._load();
        if (!r || !r.ok) this._msg('dv-res-' + key, false, this._errText(r));
    },

    async _pair(seconds) {
        const gen = this._gen;
        const r = await this._invoke('hub.zigbee.permit_join', { seconds });
        if (gen !== this._gen) return;                         // left the page meanwhile: don't start a poll nobody sees
        if (!r || !r.ok) { this._msg('dv-pair-msg', false, this._errText(r)); return; }
        const res = r.result || {};
        if (res.opening) {                                     // the hub has not settled within 1 s (hub_cmds.c): poll until it has
            this._msg('dv-pair-msg', true, this._esc(res.message || t('Opening…')));
            if (this._winTimer != null) clearInterval(this._winTimer);
            this._winTimer = setInterval(() => this._pollWindow(), 2000);
            return;
        }
        if (!res.open) {
            if (this._winTimer != null) clearInterval(this._winTimer);
            this._winTimer = null;
            this._msg('dv-pair-msg', true, this._esc(res.message || t('Closed')));
            return;
        }
        this._showLeft(res.seconds_left);
        if (this._winTimer != null) clearInterval(this._winTimer);
        this._winTimer = setInterval(() => this._pollWindow(), 2000);
    },

    /* ---- Matter: add (hub.matter.commission) and remove (POST /api/v1/hub/device/remove); both are jobs (202 {job}) ---- */
    _mtBusy() { return this._jobBusy || this._jobTimer != null; },   // a request in flight counts too (double click)

    /* the firmware's sentence for any refusal (escaped): a BLE-only code, a full fabric or a stopped controller each say what to do */
    _whyText(r) {
        const base = this._errText(r);
        return r && r.message && r.error !== 'invalid_params' ? `${base} ${this._esc(r.message)}` : base;
    },

    async _mtAdd() {
        const code = String((document.getElementById('dv-mt-code') || {}).value || '').trim();
        const name = String((document.getElementById('dv-mt-name') || {}).value || '').trim();
        if (!code) { this._msg('dv-mt-msg', false, t('Enter the pairing code first.')); return; }
        if (this._bytes(name) > 32) { this._msg('dv-mt-msg', false, t('The name is at most 32 bytes (about 10 Chinese characters).')); return; }
        if (this._mtBusy()) { this._msg('dv-mt-msg', false, t('The hub is still adding or removing a Matter device.')); return; }
        const gen = this._gen;
        const params = { setup_code: code };
        if (name) params.name = name;
        this._jobBusy = true;
        const r = await this._invoke('hub.matter.commission', params);
        if (gen !== this._gen) return;
        if (!r || !r.ok || !r.result || !r.result.job) {
            this._jobBusy = false;
            this._msg('dv-mt-msg', false, this._whyText(r));
            return;
        }
        this._msg('dv-mt-msg', true, t('Adding the device…'));
        this._watchJob(String(r.result.job), 'dv-mt-msg', (res) => {
            const el = document.getElementById('dv-mt-code');
            if (el) el.value = '';
            return t('Added.') + (res && res.detail ? ' ' + this._esc(res.detail) : '');
        });
    },

    async _mtRemove(key, force) {
        const d = this._byKey(key);
        if (!d || d.protocol !== 'matter') return;
        if (this._mtBusy()) { this._msg('dv-rm-msg', false, t('The hub is still adding or removing a Matter device.')); return; }
        if (!force && !confirm(t('Remove this Matter device from the hub? It stays in its other apps; to add it here again you need a new pairing code.'))) return;
        const gen = this._gen;
        const body = { id: d.id, confirm: 'REMOVE' };
        if (force) body.force = true;
        this._jobBusy = true;
        const r = await API.post('/api/v1/hub/device/remove', body);
        if (gen !== this._gen) return;
        /* page-level message: once removed, the card (and anything shown on it) is gone after the list is re-read */
        if (!r || !r.ok || !r.result || !r.result.job) {
            this._jobBusy = false;
            this._msg('dv-rm-msg', false, this._whyText(r));
            return;
        }
        this._msg('dv-rm-msg', true, t('Removing…'));
        this._watchJob(String(r.result.job), 'dv-rm-msg', (res) => t('Removed.') + (res && res.detail ? ' ' + this._esc(res.detail) : ''), (err) => {
            /* the device did not answer: offer to remove it from the hub only (it then has to be reset in its own app) */
            if (err === 'unreachable' && !force && confirm(t('The device did not answer. Remove it from the hub anyway? Then reset it in its own app.'))) {
                this._mtRemove(key, true);
            }
        });
    },

    /* Poll a job every 3 s until it is no longer running; show what the hub reports (escaped), never an assumed outcome.
       The hub's single httpd task can take > 3 s to answer: a tick is skipped while the last poll is still in flight, and an answer
       that lands after its timer was replaced (a forced remove started meanwhile) is ignored. */
    _watchJob(job, msgId, okText, onFail) {
        const gen = this._gen;
        let misses = 0, inflight = false;
        if (this._jobTimer != null) clearInterval(this._jobTimer);
        const h = setInterval(async () => {
            if (inflight) return;
            inflight = true;
            const r = await API.getSoft('/api/v1/hub/job?id=' + encodeURIComponent(job));
            inflight = false;
            if (gen !== this._gen || this._jobTimer !== h) return;
            if (!r || !r.ok || !r.result) {
                if (r == null && ++misses < 3) return;             // a network blip: try again before giving up
                this._stopJob();
                this._msg(msgId, false, t('The hub no longer reports how this ended; check the device list.'));
                this._load();
                return;
            }
            misses = 0;
            const j = r.result;
            if (j.state === 'running') return;
            this._stopJob();
            if (j.state === 'done') this._msg(msgId, true, okText(j.result));
            else {
                this._msg(msgId, false, this._esc(j.message || j.error || t('Failed')));
                if (onFail) onFail(j.error);
            }
            this._load();
        }, 3000);
        this._jobTimer = h;
    },

    _stopJob() {
        if (this._jobTimer != null) clearInterval(this._jobTimer);
        this._jobTimer = null;
        this._jobBusy = false;
    },

    /* ---- Matter controller settings (design 10.1, local only): the test-device switch, and the reset that only appears when the
       hub lost its Matter identity (ident_lost). The checkbox always shows what the hub reports, never what was just clicked. ---- */
    async _mtStatus() {
        const gen = this._gen;
        const r = await API.getSoft('/api/v1/matter/status');
        if (gen !== this._gen || !r || r.__status !== 200) return;
        const cb = document.getElementById('dv-mt-test');
        this._mtAllow = r.allow_test_devices === true;
        if (cb) { cb.checked = this._mtAllow; cb.disabled = false; }   // enabled only once the hub has said what it is
        const box = document.getElementById('dv-mt-reset');
        if (box) box.style.display = r.fail_step === 'ident_lost' ? '' : 'none';
    },

    async _mtTest(on) {
        if (on && !confirm(t('Allow test devices? The hub will accept devices whose certificates are only for development, so it can no longer tell whether such a device is genuine. Turn it off again after adding the device.'))) {
            const cb = document.getElementById('dv-mt-test');
            if (cb) cb.checked = false;
            return;
        }
        const cb = document.getElementById('dv-mt-test');
        if (cb) cb.checked = !!this._mtAllow;                  // until the hub answers, show what it last reported (a failed read must not leave the click)
        const gen = this._gen;
        const r = await API.post('/api/v1/matter/allow_test_devices', { allow: !!on });
        if (gen !== this._gen) return;
        if (!r || typeof r.allow !== 'boolean') this._msg('dv-mt-test-msg', false, this._whyText(r));
        else this._msg('dv-mt-test-msg', true, (r.allow ? t('Test devices are allowed.') : t('Test devices are refused (default).'))
                                               + (r.message ? ' ' + this._esc(r.message) : ''));
        await this._mtStatus();
    },

    async _mtReset() {
        const txt = String((document.getElementById('dv-mt-reset-txt') || {}).value || '').trim();
        if (txt !== 'RESET') { this._msg('dv-mt-reset-msg', false, t('Type RESET to confirm.')); return; }
        if (!confirm(t('Reset the Matter controller? The hub restarts with a new Matter identity.'))) return;
        const r = await API.post('/api/v1/matter/reset', { confirm: 'RESET' });
        if (!r || r.status !== 'ok') { this._msg('dv-mt-reset-msg', false, this._whyText(r)); return; }
        this._msg('dv-mt-reset-msg', true, t('The hub is restarting…'));
    },

    _showLeft(s) {
        this._msg('dv-pair-msg', true, t('Open: {n} s left').replace('{n}', Number(s) || 0));
    },

    async _pollWindow() {
        const gen = this._gen;
        const z = await API.get('/api/v1/zigbee/status');
        if (gen !== this._gen) return;
        if (!z || !z.hub || !z.hub.join_window) {              // no answer (network error, login lost): stop polling rather than retry every 2 s
            clearInterval(this._winTimer);
            this._winTimer = null;
            return;
        }
        const w = z.hub.join_window;
        if (w.state === 'open') { this._showLeft(w.left_s); return; }
        clearInterval(this._winTimer);
        this._winTimer = null;
        this._msg('dv-pair-msg', true, t('Closed'));
        this._load();
    },
};

App.registerPage('devices', DevicesPage);
