const Dashboard = {
    /* Generation counter for _loadThread(). It awaits /node before it arms the
       15 s poll, so a destroy() that lands inside that await had nothing to
       clear yet — the interval was armed *after* destroy() and lived on as an
       orphan. Not a corner case: entering the UI renders the Dashboard twice
       in a row (navigate() sets location.hash, onhashchange navigates again),
       so every visit used to leave one orphan polling /node every 15 s, still
       running after the tab had fallen back to the login page (seen in the
       proxy log, 2026-09-13). render() and destroy() both bump it; a
       _loadThread() that wakes up in a different generation just returns. */
    _gen: 0,

    render(el) {
        el.innerHTML = `
        <div class="page-title">${t('Dashboard')}</div>
        <div id="v-nvs-warn" style="display:none"></div>

        <div id="zb-host-warn" style="display:none;margin:0 0 16px;padding:10px 14px;
             border-left:3px solid #dc2626;background:rgba(220,38,38,.12);
             border-radius:4px;font-size:13px;line-height:1.6"></div>

        <div id="zb-check-warn" style="display:none;margin:0 0 16px;padding:10px 14px;
             border-left:3px solid #2563eb;background:rgba(37,99,235,.10);
             border-radius:4px;font-size:13px;line-height:1.6"></div>

        <div id="coex-warn" style="display:none;margin:0 0 16px;padding:10px 14px;
             border-left:3px solid #d97706;background:rgba(217,119,6,.12);
             border-radius:4px;font-size:13px;line-height:1.5"></div>

        <div class="section-label">${t('System')}</div>
        <div class="card-grid">
            <div class="card"><div class="card-label">${t('Uptime')}</div><div class="card-value" id="v-uptime">--</div></div>
            <div class="card" id="c-temp"><div class="card-label">${t('Temperature')}</div><div class="card-value" id="v-temp">--</div></div>
            <div class="card"><div class="card-label">${t('Free Memory')}</div><div class="card-value" id="v-heap">--</div></div>
            <div class="card"><div class="card-label">${t('Firmware')}</div><div class="card-value" id="v-fw">--</div></div>
            <div class="card"><div class="card-label">${t('Chip Revision')}</div><div class="card-value" id="v-chip">--</div></div>
            <div class="card"><div class="card-label">${t('Serial Number')}</div><div class="card-value" id="v-sn" style="font-family:monospace">--</div></div>
            <div class="card"><div class="card-label">MAC</div><div class="card-value" id="v-mac" style="font-family:monospace">--</div></div>
        </div>

        <div class="section-label">${t('Network')}</div>
        <div class="card-grid">
            <div class="card"><div class="card-label">${t('Active Interface')}</div><div class="card-value" id="v-net">--</div></div>
            <div class="card"><div class="card-label">${t('WiFi Signal')}</div><div class="card-value" id="v-rssi">--</div></div>
            <div class="card"><div class="card-label">${t('Ethernet')}</div><div class="card-value" id="v-eth">--</div></div>
            <div class="card"><div class="card-label">${t('Power Source')}</div><div class="card-value" id="v-power">--</div></div>
        </div>

        <div class="section-label">${t('Zigbee')}</div>
        <div class="card-grid">
            <div class="card"><div class="card-label">${t('Bridge Mode')}</div><div class="card-value" id="v-mode">--</div></div>
            <div class="card"><div class="card-label">${t('Zigbee Channel')}</div><div class="card-value" id="v-zigbee">--</div></div>
            <!-- Counts ZHA/Z2M TCP connections to port 6638, NOT Zigbee devices.
                 Named to match the "Coordinator connections" sensor in the Home
                 Assistant integration: one number, one name. The old label
                 ("Zigbee clients") was read as a device count often enough to
                 cost real debugging time. -->
            <div class="card" id="c-clients"><div class="card-label">${t('Coordinator Connections')}</div><div class="card-value" id="v-clients">--</div></div>
            <div class="card"><div class="card-label">${t('EZSP Traffic')}</div><div class="card-value" id="v-traffic">--</div></div>
            <div class="card"><div class="card-label">${t('MG24 NCP')}</div><div class="card-value" id="v-mg24">--</div></div>
        </div>

        <div class="section-label">${t('Thread / Matter')}</div>
        <div class="card-grid">
            <div class="card"><div class="card-label">${t('Thread BR Role')}</div><div class="card-value" id="v-thread-state">--</div></div>
            <div class="card"><div class="card-label">${t('Thread Network')}</div><div class="card-value" id="v-thread-net">--</div></div>
            <div class="card"><div class="card-label">${t('Thread Devices')}</div><div class="card-value" id="v-thread-children">--</div></div>
        </div>`;

        WS.on('status_update', d => this._update(d));
        API.status().then(d => { if (d) this._update(d); });
        this._loadThread(++this._gen);
    },

    destroy() {
        WS.off('status_update');
        // Drop the snapshot: otherwise, on re-entering the page, a WS push that
        // arrives before API.status() returns renders the values from the last
        // visit as if they were current.
        this._last = null;
        this._gen++;                       // any _loadThread() still awaiting must not arm its poll
        if (this._threadTimer) { clearInterval(this._threadTimer); this._threadTimer = null; }
    },

    _update(d) {
        const $ = id => document.getElementById(id);
        if (!$('v-uptime')) return;

        // The two data sources carry different fields: REST /api/v1/status is
        // complete, while the WS status_update carries only the dynamic ones
        // (see websocket.c). Consuming the WS payload directly turns
        // fw_version, bridge_mode and friends into undefined — and the
        // exception thrown by bridge_mode.toUpperCase() is swallowed silently
        // by the catch{} in ws.js, so every card after that line stops
        // updating. Merging into the previous snapshot makes both sources safe
        // to consume, and any static field added later is immune by default.
        const s = this._last = Object.assign(this._last || {}, d);

        // System
        $('v-uptime').textContent = this._fmtUptime(s.uptime);
        $('v-temp').innerHTML = `${s.temperature.toFixed(1)} <small>&deg;C</small>`;
        $('c-temp').className = 'card' + (s.temperature > 70 ? ' card-red' : s.temperature > 55 ? ' card-yellow' : '');
        $('v-heap').textContent = this._fmtBytes(s.free_heap);
        $('v-fw').textContent = s.fw_version || '--';
        $('v-chip').textContent = s.chip_revision || '--';
        $('v-sn').textContent = s.serial_number || '--';
        $('v-mac').textContent = s.mac || '--';   /* no serial written → users report this instead (user manual, section 12) */

        // Network
        const iface = s.active_interface;
        const ip = iface === 'wifi' ? (s.wifi_ip || '--')
                                    : (s.eth_ip && s.eth_ip !== '0.0.0.0' ? s.eth_ip : '--');
        $('v-net').innerHTML = `<span class="badge badge-${iface === 'wifi' ? 'blue' : 'green'}">${iface.toUpperCase()}</span> <span style="font-size:14px">${ip}</span>`;

        const rssi = s.wifi_rssi;
        const bars = rssi > -50 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
        $('v-rssi').innerHTML = s.wifi_connected
            ? `${'<span class="bar bar-on"></span>'.repeat(bars)}${'<span class="bar"></span>'.repeat(4 - bars)} <small>${rssi} dBm &middot; ${s.wifi_ssid}</small>`
            : `<span class="badge badge-red">${t('Disconnected')}</span>`;

        $('v-eth').innerHTML = s.eth_link_up
            ? `<span class="badge badge-green">${t('Link Up')}</span> ${s.eth_ip || ''}`
            : `<span class="text-muted">${t('No link')}</span>`;

        /* Power source. On a PoE-powered unit, unplugging the network cable
           also cuts power — worth showing at a glance.
           ⚠️ Tested against undefined, not truthiness: older firmware has no
           such field in /status, and rendering it directly would turn
           "unknown" into "DC / USB", which is making something up. */
        /* Config-save failures. This fault is otherwise completely silent to
           the user: REST setters answer 200 well before the 2s debounce, so
           they see "saved" and only discover on the next reboot that the
           setting reverted. The counter only grows (reset on reboot), so it is
           evidence that it happened, not a current state. */
        const nvsFail = s.nvs_write_failures;
        const nvsBanner = $('v-nvs-warn');
        if (nvsBanner) {
            if (nvsFail) {
                nvsBanner.innerHTML =
                    `<div style="margin:0 0 16px;padding:12px 14px;border-left:3px solid #ef4444;` +
                    `background:rgba(239,68,68,.10);border-radius:4px;font-size:13px;line-height:1.6">` +
                    `<strong>⚠️ ${t('Settings failed to save to flash')}</strong> ` +
                    t('({n} times since boot). A setting you changed may have reverted after a reboot. If this keeps happening the flash may be worn out — export your configuration and contact support.', { n: nvsFail }) +
                    `</div>`;
                nvsBanner.style.display = '';
            } else {
                nvsBanner.style.display = 'none';
            }
        }

        if (s.poe_present !== undefined) {
            $('v-power').innerHTML = s.poe_present
                ? `<span class="badge badge-blue">PoE</span> <small>${t('pulling the cable cuts power')}</small>`
                : `<span class="badge badge-green">${t('DC / USB')}</span>`;
        }

        // Zigbee
        const bm = s.bridge_mode;
        $('v-mode').innerHTML = bm
            ? `<span class="badge badge-${bm === 'lan' ? 'blue' : bm === 'usb' ? 'green' : 'yellow'}">${bm.toUpperCase()}</span>`
            : '<span class="text-muted">--</span>';
        // != null rather than !== undefined: the channel is now read from the
        // MG24, and the backend explicitly returns null when it cannot be read
        // or the NCP has not formed a network yet (it no longer substitutes the
        // locally configured value).
        $('v-zigbee').textContent = s.zigbee_channel != null
            ? `CH ${s.zigbee_channel}` : (s.zigbee_channel_source === 'unknown' ? t('Not declared') : '--');
        this._renderZbHost(s);
        this._renderZbCheck(s);
        this._renderCoexWarning(s);
        this._renderThreadChildren(s);
        $('v-clients').innerHTML = s.tcp_clients > 0
            ? `<span class="badge badge-green">${t('{n} connected', { n: s.tcp_clients })}</span>`
            : '<span class="text-muted">0</span>';
        $('c-clients').className = 'card' + (s.tcp_clients > 0 ? ' card-green' : '');
        $('v-traffic').innerHTML = `&darr; ${this._fmtBytes(s.uart_rx_bytes)} &nbsp; &uarr; ${this._fmtBytes(s.uart_tx_bytes)}`;
        $('v-mg24').innerHTML = s.mg24_alive
            ? `<span class="badge badge-green">${t('Online')}</span>${s.mg24_resets > 0 ? ` <small>${t('resets: {n}', { n: s.mg24_resets })}</small>` : ''}`
            : `<span class="badge badge-red">${t('Offline')}</span>`;

        // The topbar badge is handled app-wide (App.updateTopbar via ws.js).
    },

    async _loadThread(gen) {
        const node = await API.threadNode();
        if (gen !== this._gen) return;     // destroyed or re-rendered while awaiting
        const $ = id => document.getElementById(id);
        if (!node || !$('v-thread-state')) return;

        const stateColors = { leader: 'green', router: 'blue', child: 'yellow', disabled: 'red', detached: 'red' };
        const state = node.State || 'unknown';
        $('v-thread-state').innerHTML = `<span class="badge badge-${stateColors[state] || 'yellow'}">${state.charAt(0).toUpperCase() + state.slice(1)}</span>`;
        $('v-thread-net').innerHTML = `${node.NetworkName || '--'} <small style="color:var(--muted)">&middot; ${t('{n} routers', { n: node.NumOfRouter || 0 })}</small>`;

        // The child count is driven by thread_children / thread_children_total
        // from /api/v1/status instead (see _renderThreadChildren):
        // /node/children contains entries restored from NVS whose devices have
        // not actually come back yet, so counting its length makes "all
        // offline" look like "all online".

        // Refresh every 15s. Never two at once: clear before arming.
        if (this._threadTimer) clearInterval(this._threadTimer);
        this._threadTimer = setInterval(async () => {
            const n = await API.threadNode();
            if (!n || !$('v-thread-state')) return;
            const s = n.State || 'unknown';
            $('v-thread-state').innerHTML = `<span class="badge badge-${stateColors[s] || 'yellow'}">${s.charAt(0).toUpperCase() + s.slice(1)}</span>`;
        }, 15000);
    },

    /*
     * Thread child-node recovery progress.
     *
     * After the gateway reboots, OpenThread restores the child list from NVS,
     * but sleepy devices measurably take 4-6 minutes to actually rejoin. During
     * that window they all show as offline in Home Assistant, and it is very
     * easy to conclude the gateway is broken. So this must show "online/total"
     * plus "reconnecting" rather than a single number that hides the problem.
     */
    // Staleness detection for the declared value. The firmware rescans every 6h
    // using an exact PAN match; `state` comes from the backend and the frontend
    // makes no judgement of its own. undefined = this push did not carry the
    // field (WS sends dynamic fields only), so keep whatever is displayed;
    // null = the backend says the scan has not run yet.
    /*
     * No Zigbee coordinator connected — the network is actually down.
     *
     * This is not a cosmetic warning. The gateway is a pure byte-stream bridge
     * (design decision 1): it answers no EZSP request of its own. With no
     * coordinator attached, every Zigbee entity in Home Assistant is
     * unavailable, however healthy the radio chip is.
     *
     * The mesh itself keeps running — measured 2026-09-15, the coordinator
     * still beacons at -4 dBm with no host connected. So this is about the
     * host link being gone, not about the radio being down.
     *
     * `mg24_alive` keeps reporting true throughout, because the chip really is
     * alive — it answers on the wire. It is a different question from "is the
     * Zigbee network serving anything", and reading it as the latter is exactly
     * how an 8-hour outage went unnoticed in 2026-08 (a device was lost).
     * The honest signal is whether a coordinator is connected, so show that.
     *
     * Threshold, not instant: ZHA reconnects on its own within a couple of
     * minutes after a gateway reboot or an HA restart, and flagging that would
     * train the user to ignore the banner.
     */
    _renderZbHost(s) {
        const el = document.getElementById('zb-host-warn');
        if (!el || s.zigbee_host_absent_s === undefined) return;
        const secs = s.zigbee_host_absent_s;
        if (!secs || secs < 300) { el.style.display = 'none'; return; }

        const mins = Math.floor(secs / 60);
        el.innerHTML =
            `<strong>${t('No Zigbee coordinator has been connected for {n} minutes', { n: mins })}</strong><br>` +
            t('Your Zigbee devices stay unavailable in Home Assistant for as long as this lasts — the gateway only relays bytes, so nothing answers it while no coordinator is attached. Check that the integration is running; after a gateway restart ZHA sometimes fails to reconnect on its own.') +
            `<br><span style="opacity:.75">${t('The "Zigbee chip" card above reports the chip itself, which is a different question — it stays online while this warning is showing.')}</span>`;
        el.style.display = '';
    },

    _renderZbCheck(s) {
        const el = document.getElementById('zb-check-warn');
        if (!el || s.zigbee_channel_check === undefined) return;

        const c = s.zigbee_channel_check;
        if (!c || c.state === 'ok') { el.style.display = 'none'; return; }

        if (c.state === 'mismatch') {
            el.innerHTML =
                `<strong>${t('The declared Zigbee channel is out of date')}</strong><br>` +
                t('You declared <strong>CH {declared}</strong>, but your network (PAN {pan}, {rssi} dBm) was just measured on <strong>CH {detected}</strong>. Coexistence checks are running against that stale value; updating it is recommended.',
                  { declared: c.declared, pan: c.pan_id, rssi: c.rssi, detected: c.detected }) +
                `<div style="margin-top:8px">` +
                `<button class="btn" id="zb-check-fix">${t('Update to CH {n}', { n: c.detected })}</button>` +
                `<span id="zb-check-msg" style="margin-left:10px;opacity:.8"></span></div>`;
            el.style.display = '';
            document.getElementById('zb-check-fix').onclick = async () => {
                const msg = document.getElementById('zb-check-msg');
                msg.textContent = t('Updating (a confirming scan takes a few seconds)...');
                const r = await API.post('/api/v1/zigbee/channel', { channel: c.detected });
                msg.textContent = r ? t('Updated') : t('Update failed');
            };
        } else {   // not_found
            el.innerHTML =
                `<strong>${t('Your Zigbee network was not found')}</strong><br>` +
                t('No beacon for PAN {pan} was seen on any channel. The Zigbee coordinator may be stopped, the network may have been re-created (a new PAN), or we simply missed its beacon window.',
                  { pan: c.pan_id }) +
                `<span style="opacity:.75">${t('This does not affect anything that currently works; it only means the channel cannot be re-validated automatically for now.')}</span>`;
            el.style.display = '';
        }
    },

    // Coexistence warning. The backend produces an object only when it has read
    // a measured channel on both sides and the separation is at or below the
    // threshold; otherwise it is null. No judgement is made here — the rule
    // exists in exactly one place, in the firmware.
    // The WS status_update does not carry coex_warning, so undefined means
    // "this push had no such field" and the display must be left alone; only an
    // explicit null means "the backend says there is no problem".
    _renderCoexWarning(s) {
        const el = document.getElementById('coex-warn');
        if (!el || s.coex_warning === undefined) return;

        const c = s.coex_warning;
        if (!c) { el.style.display = 'none'; return; }

        el.innerHTML =
            `<strong>${t('Zigbee and Thread channels are too close')}</strong> — ` +
            t('Zigbee CH {zb} / Thread CH {th}, separation {sep} ({limit} or less counts as too close). Both radios sit on one board and will suppress each other: expect dropouts, packet loss and more retries. The Zigbee channel is chosen by Z2M/ZHA, so change it there; the Thread channel can be migrated from the Thread integration in Home Assistant.',
              { zb: c.zigbee_channel, th: c.thread_channel, sep: c.separation, limit: c.limit }) +
            `<br><span style="opacity:.75">` +
            t('The Zigbee channel used here is the value you declared in Settings; the firmware cannot read it directly (see ncp_version.h for why). Remember to update it after changing the channel in Z2M/ZHA.') +
            `</span>`;
        el.style.display = '';
    },

    _renderThreadChildren(s) {
        const el = document.getElementById('v-thread-children');
        if (!el || s.thread_children_total === undefined) return;

        const total  = s.thread_children_total;
        const online = s.thread_children;

        if (total === 0) {
            el.textContent = '0';
        } else if (online < total) {
            el.innerHTML = `${online}/${total} ` +
                `<span class="badge badge-yellow">${t('reconnecting')}</span>` +
                `<div><small style="color:var(--muted)">${t('Sleepy devices take several minutes to rejoin; this is normal.')}</small></div>`;
        } else {
            el.textContent = `${online}`;
        }
    },

    _fmtUptime(s) {
        const d = Math.floor(s / 86400);
        const h = Math.floor((s % 86400) / 3600);
        const m = Math.floor((s % 3600) / 60);
        if (d > 0) return `${d}d ${h}h ${m}m`;
        if (h > 0) return `${h}h ${m}m`;
        return `${m}m ${s % 60}s`;
    },

    _fmtBytes(b) {
        if (b > 1048576) return (b / 1048576).toFixed(1) + ' MB';
        if (b > 1024) return (b / 1024).toFixed(1) + ' KB';
        return b + ' B';
    }
};

App.registerPage('dashboard', Dashboard);
