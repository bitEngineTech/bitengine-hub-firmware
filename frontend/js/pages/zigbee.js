const ZigbeePage = {
    /* Pasted by the user verbatim, so it is a plain constant (no t()) and carries no
       HTML. Z2M only: ZHA needs nothing pasted any more — the chip answers its
       tx-power query by country (docs/14). Key verified 2026-09-10 against
       zigbee-herdsman's ember adapter: advanced.transmit_power. 19 is the radio's
       highest PA step (what the chip reports as the FCC cap); anything above the
       region cap is limited on the chip, so the number can never exceed the law. */
    SNIPPET_Z2M: 'advanced:\n  transmit_power: 19   # US/CA (FCC). EU/UK: 10 — the gateway caps it by region',

    render(el) {
        el.innerHTML = `
        <div class="page-title">${t('Zigbee Management')}</div>

        <div id="zb-setup-hint" style="display:none;margin:0 0 16px;padding:12px 14px;
             border-left:3px solid #2563eb;background:rgba(37,99,235,.10);
             border-radius:4px;font-size:13px;line-height:1.6"></div>

        <div class="section-label">${t('Status')}</div>
        <div class="card-grid">
            <div class="card"><div class="card-label">${t('NCP Status')}</div><div class="card-value" id="zb-alive">--</div></div>
            <div class="card"><div class="card-label">${t('Channel')}</div><div class="card-value" id="zb-ch">--</div><div class="hint" id="zb-ch-check"></div></div>
            <div class="card"><div class="card-label">${t('NCP Resets')}</div><div class="card-value" id="zb-resets">--</div></div>
            <div class="card"><div class="card-label">${t('Bridge Mode')}</div><div class="card-value" id="zb-mode">--</div></div>
        </div>

        <div class="section-label">${t('Connection Address')}</div>
        <div class="card">
            <div style="font-size:13px;line-height:1.7">
                ${t('Enter this address in Zigbee2MQTT / ZHA:')}
                <div style="margin:8px 0;padding:8px 10px;background:rgba(0,0,0,.25);
                     border-radius:4px;font-family:monospace;font-size:15px">
                    <span id="zb-conn-ip">--</span>
                </div>
                <div class="hint">
                    <strong>⚠️ ${t('ZHA users: do this one-time setup first')}</strong>${t(', or after roughly every other gateway reboot ZHA never reconnects and only a Home Assistant restart brings it back. This is a ZHA defect (socket coordinators only, still unfixed in the latest release). Add three lines to <code>configuration.yaml</code> and <strong>fully restart HA</strong> to cure it for good:')}
                    <div style="margin:8px 0;padding:8px 10px;background:rgba(0,0,0,.25);
                         border-radius:4px;font-family:monospace;font-size:12px;white-space:pre">zha:
  zigpy_config:
    use_thread: false</div>
                    ${t('With that in place the gateway heals itself about 40 seconds after a reboot, with no manual step (measured).')}
                    <br><br>
                    <span style="color:#f59e0b">⚠️ ${t('This is the gateway\'s device address: enter it as-is, you won\'t need to change it.')}</span>
                    ${t('When the cable is unplugged or plugged in, even across a reboot, the gateway moves this address onto whichever link works, so ZHA / Z2M keeps the same address. Exceptions: see the troubleshooting section of the user manual.')}
                    <br>${t('(Z2M uses the <code>tcp://</code> prefix, ZHA uses <code>socket://</code>)')}
                    <br><br>
                    <strong>${t('Don\'t add a DHCP reservation for the gateway on your router.')}</strong>
                    ${t('A reservation is tied to one network interface, so it pins the address there and the gateway can no longer move it between Ethernet and WiFi. The gateway keeps its address by renewing the lease itself. Details: user manual, section 4 (Deployment).')}
                </div>
            </div>
        </div>

        <div class="section-label">${t('EZSP Bridge Traffic')}</div>
        <div class="card-grid" style="grid-template-columns:repeat(auto-fill,minmax(160px,1fr))">
            <div class="card"><div class="card-label">${t('UART RX (NCP&rarr;Host)')}</div><div class="card-value" id="zb-rx">--</div></div>
            <div class="card"><div class="card-label">${t('UART TX (Host&rarr;NCP)')}</div><div class="card-value" id="zb-tx-bytes">--</div></div>
        </div>

        <div class="section-label">${t('Connected Clients (Z2M / ZHA)')}</div>
        <div class="card">
            <table class="table" id="zb-clients-table"><tr><td class="text-muted">${t('Loading...')}</td></tr></table>
            <div style="display:flex;align-items:center;gap:12px;margin-top:12px">
                <button class="btn btn-sm" onclick="ZigbeePage._resetStats()">${t('Reset Counters')}</button>
                <span id="zb-clients-msg"></span>
            </div>
        </div>

        <div class="section-label">${t('NCP Hardware')}</div>
        <div class="card">
            <table class="table" id="zb-hw-table"><tr><td class="text-muted">${t('Loading...')}</td></tr></table>
        </div>

        <div class="section-label">${t('Transmit Power')}</div>
        <div class="card">
            <div style="font-size:13px;line-height:1.7">
                <strong>${t('The Zigbee chip caps its own transmit power by region; your coordinator software sets the value below that cap.')}</strong>
                ${t('ZHA asks the chip for the recommended value for the country set in Home Assistant and uses it automatically. Zigbee2MQTT sends its own <code>transmit_power</code>; anything above the cap is limited on the chip, and Z2M keeps showing the number you typed.')}
                <table class="table" style="margin:8px 0;font-size:12px" id="zb-txp-table"><tr><td class="text-muted">${t('Loading...')}</td></tr></table>
                <div class="hint" id="zb-txp-hint"></div>
                <table class="table" style="margin:8px 0;font-size:12px">
                    <tr><td style="width:150px">${t('USA / Canada (FCC 15.247)')}</td><td><b>19 dBm</b> ${t('— the highest power step of this radio; up to 30 dBm conducted is allowed.')}</td></tr>
                    <tr><td>${t('EU / UK (EN 300 328)')}</td><td><b>10 dBm</b> ${t('— 10 dBm/MHz spectral density caps a Zigbee channel far below 20, and 10 dBm e.i.r.p. or more adds listen-before-talk / duty-cycle test items.')}</td></tr>
                    <tr><td>${t('Elsewhere')}</td><td><b>8 dBm</b> ${t('— the value the ecosystem treats as safe everywhere.')}</td></tr>
                </table>
                ${t('Set the region on the Settings page. The gateway region and the Home Assistant country both count; the stricter one wins.')}
                <div style="margin-top:6px"><strong>Zigbee2MQTT</strong> ${t('(<code>configuration.yaml</code>, optional — only if you want more than its 5 dBm default; the chip caps it):')}</div>
                <div style="display:flex;align-items:flex-start;gap:8px;margin:6px 0">
                    <div id="zb-txp-z2m" style="flex:1;padding:8px 10px;background:rgba(0,0,0,.25);
                         border-radius:4px;font-family:monospace;font-size:12px;white-space:pre">${ZigbeePage.SNIPPET_Z2M}</div>
                    <button class="btn btn-sm" id="zb-txp-z2m-copy" style="flex:none">${t('Copy')}</button>
                </div>
                <div class="hint">
                    ${t('The table above is a snapshot the gateway takes when it boots or when you apply a region; it cannot watch the chip while a coordinator is connected. Higher power only helps the coordinator be heard — battery devices still answer at their own power, so it does not fix a weak link on its own.')}
                </div>
            </div>
        </div>

        <div class="section-label">${t('Actions')}</div>
        <div class="card">
            <div style="display:flex;align-items:center;gap:12px">
                <button class="btn btn-danger" onclick="ZigbeePage._restart()">${t('Restart NCP (MG24)')}</button>
                <span id="zb-restart-msg"></span>
            </div>
            <div class="hint" style="margin-top:8px" id="zb-restart-hint">${t('Loading...')}</div>
        </div>`;

        this._refresh();
        this._loadHwInfo();
        this._wireCopy('zb-txp-z2m-copy', 'zb-txp-z2m');
        WS.on('status_update', d => this._updateFromWS(d));
    },

    _wireCopy(btnId, srcId) {
        const b = document.getElementById(btnId), src = document.getElementById(srcId);
        if (!b || !src) return;
        b.onclick = () => {
            /* clipboard API is unavailable outside https / localhost; say so instead of silently doing nothing */
            const p = navigator.clipboard?.writeText(src.textContent);
            const done = (txt) => { b.textContent = txt; setTimeout(() => b.textContent = t('Copy'), 1500); };
            if (!p) { done(t('Copy failed')); return; }
            p.then(() => done(t('Copied')), () => done(t('Copy failed')));
        };
    },

    destroy() { WS.off('status_update'); },

    async _refresh() {
        const d = await API.status();
        if (d) this._updateFromWS(d);
        this._loadClients();
    },

    /* Connection-address card. The real hostname is not used here on purpose —
       what you see is what you paste, and an IP is what the user can verify.
       ⚠️ 2026-08-21 second correction: this file once claimed "hostnames throw
       gaierror and zigpy does not retry them" — **that was wrong**. gaierror and
       a refused connection take the same ConfigEntryNotReady infinite-retry
       path (docs/guide/ZHA*.md, the ZHA-does-not-self-heal guide, section 7,
       with in-container evidence). The
       real culprit behind "hostname healed 0/3 times" was the bellows unload
       deadlock, unrelated to how the address was written. Showing the IP is
       merely for readability; it is no longer claimed to heal better. */
    _renderConnAddr(d) {
        const ipEl = document.getElementById('zb-conn-ip');
        if (!ipEl || !d) return;
        /* T9.37 (2026-10-07): the address to paste is the service address
           (/status svc_ip) — the one mDNS announces and the gateway keeps
           reachable across cable pulls, re-plugs and reboots. It is not always
           the active interface's own address: while a newly plugged cable is
           still proving itself, or when Ethernet sits on another subnet, the
           active interface can hold a different lease. Older firmware has no
           svc_ip (or reports 0.0.0.0): fall back to the active interface. */
        const svc = d.svc_ip && d.svc_ip !== '0.0.0.0' ? d.svc_ip : '';
        const ip = svc || (d.active_interface === 'eth' ? d.eth_ip : d.wifi_ip) || '--';
        ipEl.textContent = `socket://${ip}:6638`;
        /* The two MACs that used to sit here were for DHCP reservations, which the manual has told users
           NOT to make since T9.37 (2026-10-07): a reservation pins the address to one interface and the
           device address can no longer float between Ethernet and WiFi (user manual in docs/guide/, section 4). */
    },

    /* Channel advice **before** the network is formed — the only moment we can
       intervene ahead of a conflict. The Zigbee channel is chosen by Z2M/ZHA's
       own config (Z2M defaults to ch11, ZHA to ch15, neither scans), the
       firmware cannot influence that decision; all it can do is put the range
       to avoid in front of the user before they configure it. Once the NCP has
       a network (channel_source != 'unknown') this hint disappears. */
    _renderSetupHint(d) {
        const el = document.getElementById('zb-setup-hint');
        if (!el) return;
        if (!d || d.channel_source !== 'unknown') { el.style.display = 'none'; return; }

        /* No channel judgement is made here: both the range to avoid and the
           usable list come from the firmware (thread_br_list_safe_zigbee_channels),
           the same threshold the channel picker and the coexistence warning use.
           Recomputing it here would be a second copy of the rule that drifts. */
        const a = d.channel_advice;
        const avoid = a
            ? t('This gateway\'s Thread network is on <strong>CH {th}</strong>, so avoid <strong>CH {lo}–{hi}</strong> (both radios share one board and suppress each other when too close). Usable: CH {ok}.',
                { th: d.thread_channel, lo: a.avoid_lo, hi: a.avoid_hi, ok: a.ok.join(', ') })
            : t('The Thread channel cannot be read right now; after configuring, check the Dashboard for a coexistence warning.');

        el.innerHTML =
            `<strong>${t('The gateway has not been told which Zigbee channel is in use')}</strong><br>` +
            t('The channel is decided by Z2M / ZHA (Z2M: <code>advanced.channel</code> in <code>configuration.yaml</code>, default 11; ZHA asks in its setup wizard, default 15). The firmware <b>cannot read</b> it — reading requires resetting the NCP, after which the stack needs the host to reconfigure it, i.e. a second Zigbee host inside the gateway. Please declare it here so the coexistence warning has something to work with.') + '<br>' +
            `${avoid}<br>` +
            `<div style="margin-top:10px">${t('Declare the current Zigbee channel:')} ` +
            `<input id="zb-ch-in" type="number" min="11" max="26" style="width:70px" placeholder="11-26">` +
            `<button class="btn" id="zb-ch-save" style="margin-left:8px">${t('Save')}</button>` +
            `<span id="zb-ch-msg" style="margin-left:10px;opacity:.8"></span></div>`;
        el.style.display = '';

        document.getElementById('zb-ch-save').onclick = async () => {
            const v = parseInt(document.getElementById('zb-ch-in').value, 10);
            const msg = document.getElementById('zb-ch-msg');
            if (!(v >= 11 && v <= 26)) { msg.textContent = t('Enter 11-26'); return; }
            const r = await API.post('/api/v1/zigbee/channel', { channel: v });
            msg.textContent = r ? t('Saved') : t('Save failed');
            if (r) this._loadClients();
        };
    },

    /* Validation state of the declared value. `ok` is shown too — the user
       should see that the value is being watched, otherwise they have no reason
       to trust it will not silently go stale. */
    _renderCheck(s) {
        const el = document.getElementById('zb-ch-check');
        if (!el || s.zigbee_channel_check === undefined) return;
        if (this._chEditing) return;   // never clobber a half-typed value
        const c = s.zigbee_channel_check;
        let txt;
        if (!c)                          txt = t('Not verified yet');
        else if (c.state === 'ok')       txt = t('Verified · PAN {pan} · {rssi} dBm', { pan: c.pan_id, rssi: c.rssi });
        else if (c.state === 'mismatch') txt = t('⚠ Measured on CH {n}', { n: c.detected });
        else                             txt = t('⚠ Network not seen in scan');

        /* The declaration input in _renderSetupHint disappears for good once a
           value is stored (channel_source becomes "declared" and never goes
           back). Until 2026-09-15 that left NO way to correct or re-measure it:
           the Dashboard's "Update to CH n" button only exists for `mismatch`,
           and a network re-created with a new PAN reports `not_found` instead —
           the firmware cannot know where it moved to. So keep a way out here
           whenever a value is declared, which doubles as the manual re-check
           for a `not_found` that is really just radio contention. */
        if (s.zigbee_channel == null) { el.innerHTML = `<span>${txt}</span>`; return; }
        const label = c && c.state === 'ok' ? t('Re-check') : t('Re-declare');
        el.innerHTML = `<span>${txt}</span>` +
            `<button class="btn btn-sm" id="zb-ch-redecl" style="margin-left:8px">${label}</button>`;
        document.getElementById('zb-ch-redecl').onclick = () => this._editChannel(s.zigbee_channel);
    },

    /* Re-posting the declared channel re-runs the same probe the periodic check
       does and refreshes the verdict immediately (thread_br_zigbee_check_seed),
       so the banner cannot sit on a stale `not_found` for a whole cycle. */
    _editChannel(current) {
        const el = document.getElementById('zb-ch-check');
        if (!el) return;
        this._chEditing = true;
        el.innerHTML =
            `<input id="zb-ch-re" type="number" min="11" max="26" style="width:64px">` +
            `<button class="btn btn-sm" id="zb-ch-re-save" style="margin-left:6px">${t('Save')}</button>` +
            `<button class="btn btn-sm" id="zb-ch-re-cancel" style="margin-left:4px">${t('Cancel')}</button>` +
            `<span id="zb-ch-re-msg" style="margin-left:8px;opacity:.8"></span>`;
        document.getElementById('zb-ch-re').value = current;
        document.getElementById('zb-ch-re-cancel').onclick = () => {
            this._chEditing = false;
            this._loadClients();
        };
        document.getElementById('zb-ch-re-save').onclick = async () => {
            const v = parseInt(document.getElementById('zb-ch-re').value, 10);
            const msg = document.getElementById('zb-ch-re-msg');
            if (!(v >= 11 && v <= 26)) { msg.textContent = t('Enter 11-26'); return; }
            msg.textContent = t('Measuring (a few seconds)...');
            const r = await API.post('/api/v1/zigbee/channel', { channel: v });
            this._chEditing = false;
            if (!r) { msg.textContent = t('Save failed'); return; }
            this._loadClients();
        };
    },

    async _loadClients() {
        const d = await API.zigbeeStatus();
        this._renderSetupHint(d);
        /* The connection-address card needs active_interface / eth_ip / wifi_ip /
           mac / eth_mac, which live in /api/v1/status — not in /zigbee/status.
           Feeding it the zigbee payload rendered "socket://--:6638" and "--" for
           both MACs on every load (found 2026-09-05 in a browser pass). */
        API.status().then(st => { if (st) { this._renderCheck(st); this._renderConnAddr(st); } });
        if (!d || !d.clients) return;
        const tbl = document.getElementById('zb-clients-table');
        if (!tbl) return;
        if (!d.clients.length) {
            tbl.innerHTML = `<tr><td class="text-muted">${t('No clients connected')}</td></tr>`;
            return;
        }
        tbl.innerHTML = `<tr><th>${t('IP Address')}</th><th>${t('Connected')}</th><th>RX</th><th>TX</th><th></th></tr>` +
            d.clients.map(c => `<tr>
                <td>${c.ip}</td>
                <td>${this._fmtUptime(c.uptime)}</td>
                <td>${this._fmtBytes(c.rx)}</td>
                <td>${this._fmtBytes(c.tx)}</td>
                <td><button class="btn btn-sm" onclick="ZigbeePage._kick('${c.ip}')">${t('Disconnect')}</button></td>
            </tr>`).join('');
    },

    /* After a gateway reboot ZHA can wedge and never recover; the only remedy
       used to be restarting all of HA. Kicking the stale connection so it
       reconnects on its own is far lighter — exactly what disconnect_client is
       for. */
    async _kick(ip) {
        const msg = document.getElementById('zb-clients-msg');
        msg.innerHTML = `<span class="text-muted">${t('Disconnecting...')}</span>`;
        const r = await API.clientDisconnect(ip);
        msg.innerHTML = r && r.status === 'ok'
            ? `<span class="badge badge-green">${t('Closed {n} connection(s) — client should reconnect', { n: r.closed })}</span>`
            : `<span class="badge badge-red">${r?.error || t('Failed')}</span>`;
        setTimeout(() => this._loadClients(), 1500);
    },

    async _resetStats() {
        const msg = document.getElementById('zb-clients-msg');
        const r = await API.statsReset();
        msg.innerHTML = r && r.status === 'ok'
            ? `<span class="badge badge-green">${t('Counters cleared')}</span>`
            : `<span class="badge badge-red">${r?.error || t('Failed')}</span>`;
        setTimeout(() => this._loadClients(), 500);
    },

    _updateFromWS(d) {
        const $ = id => document.getElementById(id);
        if (!$('zb-alive')) return;

        // Same approach as dashboard.js (regression 02b79cb fixed only the
        // Dashboard and missed this page): REST /api/v1/status is complete, the
        // WS status_update carries only the dynamic fields (see websocket.c —
        // no zigbee_channel / mg24_resets / bridge_mode).
        // Consuming the WS payload directly turned those cards into
        // "CH undefined", and bridge_mode.toUpperCase() threw — swallowed by the
        // catch{} in ws.js, so the UART traffic cards after that line stopped
        // updating. Merging into the previous snapshot makes both sources safe,
        // and new static fields are immune by default.
        const s = this._last = Object.assign(this._last || {}, d);

        $('zb-alive').innerHTML = s.mg24_alive
            ? `<span class="badge badge-green">${t('Online')}</span>`
            : `<span class="badge badge-red">${t('Offline')}</span>`;
        // The channel is the user's declared value; the backend returns null when undeclared
        $('zb-ch').textContent = s.zigbee_channel != null ? 'CH ' + s.zigbee_channel : t('Not declared');
        $('zb-resets').textContent = s.mg24_resets !== undefined ? s.mg24_resets : '--';
        $('zb-mode').innerHTML = s.bridge_mode
            ? `<span class="badge badge-${s.bridge_mode === 'lan' ? 'blue' : 'green'}">${s.bridge_mode.toUpperCase()}</span>`
            : '--';
        $('zb-rx').textContent = this._fmtBytes(s.uart_rx_bytes || 0);
        $('zb-tx-bytes').textContent = this._fmtBytes(s.uart_tx_bytes || 0);
        /* zigbee_tx_power is deliberately NOT rendered. Until 2026-09-10 a
           "Turbo Mode" switch here showed "(20 dBm Turbo)" from a value that
           only ever lived in NVS and was never sent to the NCP (docs/
           feature_gap_audit.md A7). The power in use is whatever Z2M/ZHA wrote
           last — see the Transmit Power card. */
    },

    /* /zigbee/status.tx_power — the NCP's region / cap snapshot (docs/14 §5.2).
       Every value is displayed as received; the page computes nothing, so a wrong
       number here is a firmware bug, not a display bug. */
    _renderTxPower(zb) {
        const tbl = document.getElementById('zb-txp-table');
        const hint = document.getElementById('zb-txp-hint');
        if (!tbl) return;
        const tp = zb && zb.tx_power;
        const cls = c => ({
            0: t('not declared'), 1: t('other (8 dBm)'), 2: t('EU / ETSI (10 dBm)'), 3: t('US / Canada (19 dBm)'),
        })[c] || '--';
        if (!tp) {
            tbl.innerHTML = `<tr><td class="text-muted">${t('unknown')}</td></tr>`;
            return;
        }
        if (!tp.ncp_xncp) {
            tbl.innerHTML = `<tr><td class="text-muted">${t('The Zigbee chip firmware does not support the region cap yet')}</td></tr>`;
            if (hint) hint.textContent = t('Update the Zigbee firmware on the Update page; until then the region only applies to Thread.');
            return;
        }
        const rows = [
            [t('Gateway region'),          tp.gw_region   ? `${tp.gw_region} (${cls(tp.gw_class)})`     : t('not set')],
            [t('Home Assistant country'),  tp.host_region ? `${tp.host_region} (${cls(tp.host_class)})` : t('not declared')],
            [t('Effective cap'),           `<b>${tp.cap_dbm} dBm</b> (${cls(tp.class)})`],
            [t('Requested by coordinator'), `${tp.requested_dbm} dBm`],
            [t('Capped so far'),           String(tp.clamped_count)],
        ];
        tbl.innerHTML = rows
            .map(([k, v]) => `<tr><td style="color:var(--muted);width:180px">${k}</td><td>${v}</td></tr>`)
            .join('');
        /* cached: the gateway restarted on its own and did not re-read the Zigbee chip
           (that would stop a running Zigbee network), so this is the previous boot's snapshot. */
        if (hint) hint.textContent = tp.cached
            ? t('Snapshot from an earlier start: the Zigbee chip kept running when the gateway restarted, so it was not read again.')
            : t('Snapshot taken {s} s after boot.', { s: tp.read_uptime_s });
    },

    async _loadHwInfo() {
        const [info, zb] = await Promise.all([API.get('/api2/info'), API.zigbeeStatus()]);
        /* The power snapshot comes from /zigbee/status alone; do not let a failed
           /api2/info (older firmware, transient error) hide it. */
        this._renderTxPower(zb);
        if (!info || !document.getElementById('zb-hw-table')) return;

        // Two identities answer two different questions; do not merge them:
        //   Firmware = the EmberZNet version the chip reports (identical for
        //              every image built from the same SDK)
        //   Image    = sha256 of the .gbl this gateway itself flashed, matching
        //              a line in SHA256SUMS
        // A board flashed only on the production line with J-Link has no
        // Image entry and reports "factory (J-Link)".
        const rows = [
            [t('NCP Chip'), info.zb_chip],
            [t('Firmware'), info.zb_version],
            [t('MAC Address'), info.mac],
        ];
        if (zb) {
            rows.push([t('EZSP Protocol'),
                zb.ncp_version_known ? '0x' + (zb.ncp_ezsp_protocol || 0).toString(16) : t('unknown')]);
            rows.push([t('Image'), zb.image_source === 'gateway_ota'
                ? `<code>${(zb.image_sha256 || '').slice(0, 16)}…</code>` +
                  ` <span class="text-muted">(${this._fmtBytes(zb.image_size)}` +
                  `${zb.image_flashed_at ? ', ' + new Date(zb.image_flashed_at * 1000).toLocaleString() : ''})</span>`
                : `<span class="text-muted">${t('factory (J-Link) — not flashed by this gateway')}</span>`]);
        }
        document.getElementById('zb-hw-table').innerHTML = rows
            .map(([k, v]) => `<tr><td style="color:var(--muted);width:140px">${k}</td><td>${v || '--'}</td></tr>`)
            .join('');

        // The actual effect of a reset is reported by the firmware
        // (restart_resets_board), never hardcoded: on the AM1 board IO23 pulls
        // the C5's EN down through R113/R58, so pressing it **reboots the whole
        // gateway**; only after the AM2 diode fix does it become "Zigbee only".
        // Hardcoded wording would keep lying after the board revision.
        this._resetsBoard = !!(zb && zb.restart_resets_board);
        // AM1: that whole-board reset is a chip-level reset, and with Ethernet up IO28 (PHY_INT = BOOT strap)
        // is low, so the chip usually lands in download mode and needs a power cycle (hardware defect X6).
        const hint = document.getElementById('zb-restart-hint');
        if (hint) {
            hint.innerHTML = this._resetsBoard
                ? `<b style="color:var(--danger,#e35)">${t('On this board revision this reboots the ENTIRE gateway')}</b>, ` +
                  t('not just Zigbee — the MG24 reset line is tied to the supervisor (R113/R58). WebUI, Thread and Ethernet all drop. With the Ethernet cable connected it will most likely not come back on its own: you then have to unplug its power (or the PoE cable) and plug it back in. Use it only when the NCP is wedged.')
                : t('Resets the MG24 only. Zigbee devices reconnect within about a minute; the gateway itself keeps running.');
        }
    },

    async _restart() {
        // The confirmation uses the same single source of truth, so the prompt
        // and the actual consequence cannot disagree
        const msg = this._resetsBoard
            ? t('Restart the MG24 NCP?') + '\n\n' +
              t('WARNING: on this board revision the reset line also pulls the supervisor down — the ENTIRE gateway reboots (WebUI, Thread, Ethernet included), not just Zigbee. With the Ethernet cable connected it will most likely stay down until you unplug its power (or the PoE cable) and plug it back in.') + '\n\n' +
              t('There is a 5-minute cooldown before it can be repeated.')
            : t('Restart the MG24 NCP?') + '\n\n' +
              t('Zigbee devices will reconnect within about a minute. The gateway itself keeps running.');
        if (!confirm(msg)) return;
        const r = await API.zigbeeRestart();
        document.getElementById('zb-restart-msg').innerHTML = r && r.status === 'ok'
            ? `<span class="badge badge-green">${t('Restarting...')}</span>`
            : `<span class="badge badge-red">${r?.error || t('Failed')}</span>`;
    },

    _fmtUptime(s) {
        if (s > 86400) return Math.floor(s/86400) + 'd ' + Math.floor((s%86400)/3600) + 'h';
        if (s > 3600) return Math.floor(s/3600) + 'h ' + Math.floor((s%3600)/60) + 'm';
        return Math.floor(s/60) + 'm ' + (s%60) + 's';
    },
    _fmtBytes(b) {
        if (b > 1048576) return (b / 1048576).toFixed(1) + ' MB';
        if (b > 1024) return (b / 1024).toFixed(1) + ' KB';
        return b + ' B';
    }
};

App.registerPage('zigbee', ZigbeePage);
