const WiFiPage = {
    render(el) {
        el.innerHTML = `
        <div class="page-title">${t('WiFi Settings')}</div>

        <div class="section-label">${t('Current Connection')}</div>
        <div class="card-grid">
            <div class="card"><div class="card-label">${t('Status')}</div><div class="card-value" id="wifi-status">--</div></div>
            <div class="card"><div class="card-label">SSID</div><div class="card-value" id="wifi-ssid">--</div></div>
            <div class="card"><div class="card-label">${t('IP Address')}</div><div class="card-value" id="wifi-ip">--</div></div>
            <div class="card"><div class="card-label">${t('Signal')}</div><div class="card-value" id="wifi-rssi">--</div></div>
            <div class="card"><div class="card-label">${t('Channel')}</div><div class="card-value" id="wifi-ch">--</div></div>
        </div>
        <div class="text-muted" id="wifi-dfs-warn" style="display:none;margin-top:8px;font-size:12px"></div>

        <div class="section-label">${t('Link Health (since boot)')}</div>
        <div class="card-grid">
            <div class="card"><div class="card-label">${t('Disconnects')}</div><div class="card-value" id="wifi-disc">--</div></div>
            <div class="card"><div class="card-label">${t('Current outage')}</div><div class="card-value" id="wifi-down">--</div></div>
            <div class="card"><div class="card-label">${t('Last outage')}</div><div class="card-value" id="wifi-last">--</div></div>
            <div class="card"><div class="card-label">${t('Longest outage')}</div><div class="card-value" id="wifi-long">--</div></div>
        </div>
        <div class="text-muted" style="margin-top:8px;font-size:12px">
            ${t('An outage lasts from the moment WiFi drops until the gateway has an IP address again — that is when Zigbee and Matter can reconnect. Changing WiFi settings also counts as one.')}
        </div>

        <div class="section-label">${t('Available Networks')}</div>
        <div class="card">
            <div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:12px">
                <span class="text-muted">${t('5 GHz networks only')}</span>
                <button class="btn btn-sm" id="wifi-scan-btn" onclick="WiFiPage._scan()">${t('Scan')}</button>
            </div>
            <div id="wifi-list" class="text-muted">${t('Click Scan to search')}</div>
        </div>

        <div class="section-label">${t('IP Configuration')}</div>
        <div class="card">
            <div style="display:flex;align-items:center;gap:12px;margin-bottom:16px">
                <label class="switch"><input type="checkbox" id="wifi-dhcp" checked onchange="WiFiPage._toggleDhcp(this.checked)"><span class="slider"></span></label>
                <span>${t('DHCP (automatic)')}</span>
            </div>
            <div id="wifi-static" style="display:none">
                <div class="form-row">
                    <div class="form-group" style="flex:1"><label>${t('IP Address')}</label><input id="wifi-sip" class="input" placeholder="192.168.1.100"></div>
                    <div class="form-group" style="flex:1"><label>${t('Gateway')}</label><input id="wifi-sgw" class="input" placeholder="192.168.1.1"></div>
                    <div class="form-group" style="flex:1"><label>${t('Netmask')}</label><input id="wifi-smask" class="input" placeholder="255.255.255.0"></div>
                    <div class="form-group" style="flex:1"><label>${t('DNS')}</label><input id="wifi-sdns" class="input" placeholder="192.168.1.1"></div>
                </div>
            </div>
            <div style="display:flex;align-items:center;gap:12px">
                <button class="btn btn-primary" onclick="WiFiPage._saveStatic()">${t('Save')}</button>
                <span id="wifi-static-msg"></span>
            </div>
            <div class="text-muted" style="margin-top:8px;font-size:12px">
                ${t("Takes effect on next reboot. Applying immediately would change this device's own IP and drop the connection serving this page.")}
            </div>
        </div>

        <div class="section-label">${t('Setup Access Point')}</div>
        <div class="card">
            <div style="display:flex;align-items:center;gap:12px">
                <label class="switch"><input type="checkbox" id="wifi-softap" onchange="WiFiPage._toggleSoftap(this.checked)"><span class="slider"></span></label>
                <span>${t('Broadcast 5 GHz setup AP')}</span>
                <span id="wifi-softap-msg"></span>
            </div>
            <div class="text-muted" style="margin-top:8px;font-size:12px">
                ${t('Thread is paused while the setup AP is on &mdash; the radios cannot share the antenna, so Thread devices go offline until it is switched off.')}
            </div>
        </div>

        <div class="section-label">${t('Connect')}</div>
        <div class="card" id="wifi-form-card" style="display:none">
            <div class="form-row">
                <div class="form-group" style="flex:1"><label>${t('SSID')}</label><input id="wifi-input-ssid" class="input" readonly></div>
                <div class="form-group" style="flex:1"><label>${t('Password')}</label>
                    ${PwField.html('wifi-pass', t('Enter password'))}
                </div>
            </div>
            <div style="display:flex;align-items:center;gap:12px">
                <button class="btn btn-primary" onclick="WiFiPage._connect()">${t('Connect')}</button>
                <span id="wifi-msg"></span>
            </div>
        </div>`;

        API.status().then(d => {
            if (!d) return;
            const $ = id => document.getElementById(id);
            $('wifi-status').innerHTML = d.wifi_connected
                ? `<span class="badge badge-green">${t('Connected')}</span>`
                : `<span class="badge badge-red">${t('Disconnected')}</span>`;
            $('wifi-ssid').textContent = d.wifi_ssid || '--';
            $('wifi-ip').textContent = d.wifi_ip || '--';
            const rssi = d.wifi_rssi;
            const bars = rssi > -50 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
            $('wifi-rssi').innerHTML = d.wifi_connected
                ? `${'<span class="bar bar-on"></span>'.repeat(bars)}${'<span class="bar"></span>'.repeat(4 - bars)} <small>${rssi} dBm</small>`
                : '--';
            this._renderLink(d);
        });

        /* Prefill the configured static addresses. The wifi section of
           /config/export has no dhcp flag: static_ip == 0 means DHCP. Without
           this the switch always showed DHCP, and Save on that screen would
           quietly revert a static setup at the next reboot. */
        API.configExport().then(c => {
            const w = c && c.wifi;
            const $ = id => document.getElementById(id);
            if (!w || !$('wifi-dhcp')) return;
            const dhcp = !w.static_ip;
            $('wifi-dhcp').checked = dhcp;
            this._toggleDhcp(dhcp);
            const fill = (id, n) => { if (n) $(id).value = IP4.toString(n); };
            fill('wifi-sip',   w.static_ip);
            fill('wifi-sgw',   w.static_gateway);
            fill('wifi-smask', w.static_netmask);
            fill('wifi-sdns',  w.static_dns);
        });

        API.softapGet().then(d => {
            if (d && typeof d.active === 'boolean') {
                document.getElementById('wifi-softap').checked = d.active;
                if (d.active) document.getElementById('wifi-softap-msg').innerHTML =
                    `<span class="badge badge-yellow">${t('AP on — Thread paused')}</span>`;
            }
        });
    },

    /* WiFi backhaul diagnostics. The fields come from /status (firmware v0.5.0-395+) and are
       defined by wifi_link_stats_t in network_mgr.h: only "connected -> dropped" transitions are
       counted, and an outage lasts until the gateway has an IP again. wifi_down_s is the CURRENT
       outage so far (0 while online), not a running total. last/longest are null until at least
       one outage has ended (0 s is a real possible value). Older firmware lacks the fields: show
       "--" then, never 0, or the page would claim "all fine". Numbers go through Number() before
       touching innerHTML. */
    _renderLink(d) {
        const $ = id => document.getElementById(id);
        if (!$('wifi-ch') || !d) return;
        const has = k => Object.prototype.hasOwnProperty.call(d, k);

        const ch = has('wifi_channel') && d.wifi_channel != null ? Number(d.wifi_channel) : null;
        $('wifi-ch').innerHTML = ch == null ? '--'
            : `${ch}${d.wifi_dfs === true ? ' <span class="badge badge-yellow">DFS</span>' : ''}`;
        const warn = $('wifi-dfs-warn');
        if (d.wifi_dfs === true && ch != null) {
            warn.innerHTML = t('Channel {ch} is a DFS channel: when the router detects radar it must switch channels and stay silent for at least a minute, and the gateway drops off WiFi meanwhile. If you rely on WiFi, set the router to channel 36–48 or 149–165.', { ch });
            warn.style.display = '';
        } else {
            warn.style.display = 'none';
        }

        $('wifi-disc').textContent = has('wifi_disconnects') && d.wifi_disconnects != null
            ? String(Number(d.wifi_disconnects)) : '--';

        const down = has('wifi_down_s') && d.wifi_down_s != null ? Number(d.wifi_down_s) : null;
        if (down == null || (down === 0 && !d.wifi_connected)) {
            $('wifi-down').textContent = '--';          /* no field, or WiFi never connected */
        } else {
            $('wifi-down').innerHTML = down > 0
                ? `<span class="badge badge-red">${this._fmtDur(down)}</span>`
                : `<span class="badge badge-green">${t('None')}</span>`;
        }

        const outage = k => !has(k) ? '--' : d[k] == null ? t('None yet') : this._fmtDur(d[k]);
        $('wifi-last').textContent = outage('wifi_last_outage_s');
        $('wifi-long').textContent = outage('wifi_longest_outage_s');
    },

    _fmtDur(sec) {
        const s = Math.max(0, Math.floor(Number(sec) || 0));
        const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), r = s % 60;
        if (h > 0) return `${h}h ${m}m`;
        if (m > 0) return `${m}m ${r}s`;
        return `${r}s`;
    },

    _toggleDhcp(on) { document.getElementById('wifi-static').style.display = on ? 'none' : ''; },

    async _saveStatic() {
        const dhcp = document.getElementById('wifi-dhcp').checked;
        const msg = document.getElementById('wifi-static-msg');
        const data = { dhcp };
        if (!dhcp) {
            data.static_ip      = document.getElementById('wifi-sip').value;
            data.static_gateway = document.getElementById('wifi-sgw').value;
            data.static_netmask = document.getElementById('wifi-smask').value;
            data.static_dns     = document.getElementById('wifi-sdns').value;
        }
        const r = await API.wifiStatic(data);
        msg.innerHTML = r && r.status === 'ok'
            ? `<span class="badge badge-green">${t('Saved — active after reboot')}</span>`
            : `<span class="badge badge-red">${r?.error || t('Failed')}</span>`;
    },

    async _toggleSoftap(on) {
        const msg = document.getElementById('wifi-softap-msg');
        msg.innerHTML = `<span class="text-muted">${t('Working...')}</span>`;
        const r = await API.softapSet(on);
        if (r && r.status === 'ok') {
            msg.innerHTML = on
                ? `<span class="badge badge-yellow">${t('AP on — Thread paused')}</span>`
                : `<span class="badge badge-green">${t('AP off')}</span>`;
        } else {
            msg.innerHTML = `<span class="badge badge-red">${r?.error || t('Failed')}</span>`;
            /* On failure, flip the switch back, or the UI state stops matching
               the device's actual state. */
            document.getElementById('wifi-softap').checked = !on;
        }
    },

    async _scan() {
        const btn = document.getElementById('wifi-scan-btn');
        const list = document.getElementById('wifi-list');
        btn.disabled = true; btn.textContent = t('Scanning...');
        list.innerHTML = `<div class="text-muted">${t('Scanning...')}</div>`;
        const data = await API.wifiScan();
        btn.disabled = false; btn.textContent = t('Scan');
        if (!data || !data.length) { list.innerHTML = `<div class="text-muted">${t('No networks found')}</div>`; return; }
        // Security-mode names are protocol identifiers, not prose — left
        // untranslated on purpose so they match what the router's own UI says.
        // Values follow ESP-IDF wifi_auth_mode_t: 5 is WPA2-Enterprise (was mislabelled
        // "WPA3"), 6 is WPA3, 7 is the WPA2/WPA3 mixed mode most home routers use.
        const authNames = { 0: 'Open', 1: 'WEP', 2: 'WPA', 3: 'WPA2', 4: 'WPA/WPA2', 5: 'WPA2-Enterprise',
                            6: 'WPA3', 7: 'WPA2/WPA3', 8: 'WAPI', 9: 'OWE', 10: 'WPA3-Enterprise' };
        list.innerHTML = data.map(ap => {
            const bars = ap.rssi > -50 ? 4 : ap.rssi > -65 ? 3 : ap.rssi > -75 ? 2 : 1;
            const signal = '<span class="bar bar-on"></span>'.repeat(bars) + '<span class="bar"></span>'.repeat(4 - bars);
            return `<div class="list-item" onclick="WiFiPage._select('${ap.ssid.replace(/'/g, "\\'")}')">
                <div><strong>${ap.ssid}</strong><br><small class="text-muted">CH ${ap.channel} &middot; ${authNames[ap.auth] || t('Unknown')}</small></div>
                <div style="text-align:right">${signal}<br><small class="text-muted">${ap.rssi} dBm</small></div>
            </div>`;
        }).join('');
    },

    _select(ssid) {
        document.getElementById('wifi-form-card').style.display = '';
        document.getElementById('wifi-input-ssid').value = ssid;
        /* Resetting is mandatory when switching APs: if SHOW was pressed for
           the previous one and we do not reset, the new password is typed in
           cleartext on screen. */
        PwField.reset('wifi-pass');
        document.getElementById('wifi-pass').focus();
    },

    async _connect() {
        const ssid = document.getElementById('wifi-input-ssid').value;
        const pass = document.getElementById('wifi-pass').value;
        const msg = document.getElementById('wifi-msg');
        msg.innerHTML = `<span class="text-muted">${t('Connecting...')}</span>`;
        const r = await API.wifiConfig(ssid, pass);
        if (!r || r.status !== 'ok') {
            msg.innerHTML = `<span class="badge badge-red">${r?.error || t('Failed')}</span>`;
            return;
        }
        // Poll for the real outcome rather than blindly refreshing after 10s.
        // Provisioning on the firmware side is **bounded** (three quick
        // attempts and then it stops), so a verdict is guaranteed to arrive:
        // wifi_provision becomes ok or failed, never staying on connecting.
        this._pollProvision(msg);
    },

    // The gateway is 5 GHz only, and the most common cause of "network not
    // found" is in fact the user picking a 2.4 GHz SSID. The message has to say
    // so, or they will just keep retyping the password.
    _provisionError(code) {
        return {
            auth_failed:  t('Wrong password — check it and try again'),
            ap_not_found: t('Network not found — check the SSID, and that it is on 5 GHz (this device does not support 2.4 GHz)'),
            signal_lost:  t('Signal lost — the router may have restarted, or it is too far away'),
        }[code] || t('Connection failed');
    },

    _pollProvision(msg) {
        clearTimeout(this._provTimer);
        const t0 = Date.now();
        const tick = async () => {
            let d;
            try { d = await API.status(); } catch (e) { d = null; }
            // Once connected, the device moves to the new network and this
            // page (most likely opened over the setup AP) loses contact. A
            // failing request is itself a hint that it probably worked — but do
            // not conclude anything from it; keep waiting.
            if (d && d.wifi_provision === 'ok') {
                msg.innerHTML = `<span class="badge badge-green">${t('Connected to {ssid}, IP {ip}', { ssid: d.wifi_ssid || '', ip: d.wifi_ip || '' })}</span>`;
                return;
            }
            if (d && d.wifi_provision === 'failed') {
                msg.innerHTML = `<span class="badge badge-red">${this._provisionError(d.wifi_last_error)}</span>`
                              + `<div class="hint" style="margin-top:6px">${t('Diagnostic code {code}', { code: d.wifi_last_reason })}</div>`;
                return;
            }
            if (Date.now() - t0 > 90000) {
                msg.innerHTML = `<span class="badge">${t('Submitted, but no result came back — the device may have moved to the new network; try its new address.')}</span>`;
                return;
            }
            msg.innerHTML = `<span class="text-muted">${t('Connecting...')}</span>`;
            this._provTimer = setTimeout(tick, 2000);
        };
        this._provTimer = setTimeout(tick, 1500);
    },

    destroy() {
        // The poll is a self-recursing setTimeout; without clearing it, it
        // keeps hitting the API after the user has left the page.
        clearTimeout(this._provTimer);
        this._provTimer = null;
    }
};

App.registerPage('wifi', WiFiPage);
