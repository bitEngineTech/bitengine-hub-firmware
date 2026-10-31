const SettingsPage = {
    render(el) {
        el.innerHTML = `
        <div class="page-title">${t('System Settings')}</div>

        <div class="section-label">${t('Device Info')}</div>
        <div class="card-grid" style="grid-template-columns:repeat(auto-fill,minmax(180px,1fr))">
            <div class="card"><div class="card-label">${t('Hostname')}</div><div class="card-value" id="set-hostname">--</div></div>
            <div class="card"><div class="card-label">${t('Serial Number')}</div><div class="card-value" id="set-sn" style="font-family:monospace">--</div></div>
            <div class="card"><div class="card-label">MAC</div><div class="card-value" id="set-mac" style="font-family:monospace">--</div></div>
            <div class="card"><div class="card-label">${t('Firmware')}</div><div class="card-value" id="set-fw">--</div></div>
            <div class="card"><div class="card-label">${t('Frontend')}</div><div class="card-value" id="set-www">--</div></div>
            <div class="card"><div class="card-label">${t('Free Heap')}</div><div class="card-value" id="set-heap">--</div></div>
        </div>

        <div class="section-label">${t('Device Name')}</div>
        <div class="card">
            <div class="form-row">
                <div class="form-group" style="flex:1"><label>${t('Hostname')}</label>
                    <input id="set-hostname-in" maxlength="31" placeholder="bitengine">
                </div>
                <div class="form-group" style="flex:none;align-self:flex-end">
                    <button class="btn" onclick="SettingsPage._saveHostname()">${t('Save')}</button>
                </div>
            </div>
            <div class="hint">
                ${t('Letters, digits and hyphens only; no dots (the <code>.local</code> suffix is added automatically). This is the name in <code>&lt;name&gt;.local</code> and in mDNS discovery.')}
                <br>${t('Renaming takes effect immediately and does not drop existing connections — those use the IP address. Home Assistant entries found via mDNS may need to be re-discovered.')}
            </div>
            <span id="set-hostname-msg"></span>
        </div>

        <div class="section-label">${t('Radio Region')}</div>
        <div class="card">
            <div class="form-row">
                <div class="form-group" style="flex:1"><label>${t('Region')}</label>
                    <select id="set-region"><option value="">${t('Loading...')}</option></select>
                </div>
                <div class="form-group" style="flex:none;align-self:flex-end">
                    <button class="btn" onclick="SettingsPage._saveRegion()">${t('Save and apply')}</button>
                </div>
            </div>
            <div class="hint">
                ${t('Sets the legal transmit power cap for both radios: Zigbee (inside the MG24 chip) and Thread. Not set = 10 dBm, which is legal everywhere.')}
                <br>${t('Applying disconnects Zigbee for about a minute (Z2M / ZHA reconnect on their own) and briefly restarts the Zigbee chip. ZHA users: the country set in Home Assistant counts too, and the stricter of the two wins.')}
            </div>
            <span id="set-region-msg"></span>
            <div id="set-region-result" class="hint" style="margin-top:6px"></div>
        </div>

        <div class="section-label">${t('Security')}</div>
        <div class="card">
            <div class="form-row">
                <div class="form-group" style="flex:1"><label>${t('New Password')}</label>
                    ${PwField.html('set-pw1', t('Min 4 characters'))}
                </div>
                <div class="form-group" style="flex:1"><label>${t('Confirm Password')}</label>
                    ${PwField.html('set-pw2', t('Repeat password'))}
                </div>
            </div>
            <div style="display:flex;align-items:center;gap:12px">
                <button class="btn btn-primary" onclick="SettingsPage._changePw()">${t('Update Password')}</button>
                <span id="set-pw-msg"></span>
            </div>
        </div>

        <div class="section-label">${t('Configuration Backup')}</div>
        <div class="card">
            <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap">
                <button class="btn btn-sm" onclick="SettingsPage._export()">${t('Export Config')}</button>
                <label class="btn btn-sm" style="cursor:pointer">${t('Import Config')}<input type="file" accept=".json" style="display:none" onchange="SettingsPage._import(this.files[0])"></label>
                <span id="set-cfg-msg"></span>
            </div>
            <div class="hint" style="margin-top:8px">${t('Export saves all settings as JSON. Import restores from a previously exported file.')}</div>
        </div>

        <div class="section-label">${t('Time &amp; Timezone')}</div>
        <div class="card">
            <div id="ntp-warn" style="display:none;margin-bottom:12px;padding:10px 12px;
                 border-left:3px solid var(--yellow);background:rgba(255,167,38,.08);font-size:13px">
                <b>${t('The timezone has not been set')}</b> ${t('— the device is on the factory default, UTC. Night mode is evaluated in the device\'s local time, so without a timezone "lights off at 22:00" means 22:00 UTC. Press <b>Detect</b> below.')}
            </div>
            <div class="form-row" style="align-items:flex-end">
                <div class="form-group" style="flex:2;min-width:260px">
                    <label>${t('Timezone (POSIX TZ)')}</label>
                    <div style="display:flex;gap:6px">
                        <input class="input" id="ntp-tz" placeholder="UTC+0">
                        <button class="btn btn-sm" style="white-space:nowrap"
                                onclick="SettingsPage._ntpDetect()">${t('Detect')}</button>
                    </div>
                </div>
                <div class="form-group" style="flex:2;min-width:200px">
                    <label>${t('NTP Server')}</label>
                    <input class="input" id="ntp-server" placeholder="pool.ntp.org">
                </div>
                <div class="form-group" style="flex:0 0 140px;min-width:140px">
                    <label>${t('Sync Interval (s)')}</label>
                    <input class="input" type="number" id="ntp-interval" min="60" max="86400">
                </div>
            </div>
            <div id="ntp-detected" class="hint" style="margin-bottom:8px"></div>
            <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap">
                <button class="btn btn-primary" onclick="SettingsPage._ntpSave()">${t('Save')}</button>
                <span class="hint">${t('Device time:')} <b id="ntp-now">--</b></span>
                <span id="ntp-msg"></span>
            </div>
            <div class="hint" style="margin-top:8px">
                ${t('The sign in a POSIX TZ string is the opposite of intuition: <code>PST8PDT</code> means UTC&minus;8, <code>CST-8</code> means UTC+8. After saving, check the "Device time" above — that is the only reliable verification.')}
            </div>
        </div>

        <div class="section-label">${t('Extra access addresses (cross-site protection)')}</div>
        <div class="card">
            <div class="hint" style="margin-bottom:10px">
                ${t('The firmware only accepts requests addressed to the <b>device IP</b>, <code id="ah-host">-</code> and its <code>.local</code> / <code>.lan</code> style private suffixes; any other name is rejected (<b>421</b>). This blocks DNS rebinding: otherwise any web page could point its own domain at the gateway IP and the browser would treat it as same-origin.')}
                <br>${t('If your router gave the device a different domain name, or there is a reverse proxy in front of it, <b>every save fails when the UI is opened through that address</b> — add it here.')}
            </div>
            <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap">
                <input type="text" id="ah-extra" placeholder="${t('e.g. gw.mynet or gateway.example.com')}" style="flex:1;min-width:200px">
                <button class="btn btn-sm" onclick="SettingsPage._ahFillCurrent()">${t('Use current address')}</button>
                <button class="btn" onclick="SettingsPage._ahSave()">${t('Save')}</button>
                <span id="ah-msg"></span>
            </div>
            <div class="hint" style="margin-top:8px">
                ${t('Currently accepted:')} <code id="ah-accepted">-</code>
                <br>⚠️ ${t('This is a <b>permanent pass</b>: enter a public domain controlled by someone else and this protection is void for that domain. Empty = off.')}
            </div>
        </div>

        <div class="section-label">${t('Thread / OTBR access token')}</div>
        <div class="card">
            <div class="hint" style="margin-bottom:10px">
                ${t('For the Home Assistant <b>OpenThread Border Router</b> integration. Without a token the OTBR routes are wide open to the local network — anyone can read the <b>Thread network key</b>.')}
                <br>${t('With a token set, HA is given a URL that carries the token, so the <b>gateway admin password is not needed there</b> (a leaked admin password is full control of the device; the token can only read and write the Thread dataset).')}
            </div>
            <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:10px">
                <button class="btn" onclick="SettingsPage._otbrTokenGen()">${t('Generate / replace token')}</button>
                <button class="btn btn-sm" onclick="SettingsPage._otbrTokenClear()">${t('Clear')}</button>
                <span id="ot-msg"></span>
            </div>
            <div id="ot-url-box" style="display:none">
                <div class="hint" style="margin-bottom:4px">${t('Paste this whole line into the OTBR integration in HA:')}</div>
                <div style="display:flex;gap:8px;align-items:center">
                    <code id="ot-url" style="flex:1;word-break:break-all">--</code>
                    <button id="ot-copy" class="btn-sm" style="flex:none">${t('Copy')}</button>
                </div>
            </div>
            <div class="hint" style="margin-top:8px">
                ⚠️ ${t('HA\'s OTBR integration has <b>no "edit URL" feature</b> (no OptionsFlow / reconfigure anywhere in the component). So <b>after changing the token, the existing OTBR entry in HA must be deleted and added again</b>.')}
                <br>⚠️ ${t('The address may only contain an <b>IP</b>, not a <code>.local</code> name — the HA container has no mDNS resolver. The line above already uses the gateway\'s device address, which follows the gateway across cable pulls, re-plugs and reboots, so this URL keeps working. Don\'t add a DHCP reservation for the gateway on your router (user manual, section 4). The address only changes if the gateway stays powered off longer than the router\'s lease and the router gives it to another device — then this entry has to be deleted and added again.')}
            </div>
        </div>

        <div class="section-label">${t('Thread / OTBR REST access control')}</div>
        <div class="card">
            <div style="display:flex;align-items:center;gap:10px;margin-bottom:10px">
                <label class="switch"><input type="checkbox" id="otbr-auth" onchange="SettingsPage._otbrAuthToggle()"><span class="slider"></span></label>
                <span>${t('OTBR REST requires authentication')}</span>
                <span id="otbr-auth-msg"></span>
            </div>
            <div class="hint">
                ${t('When off (<b>factory default</b>) the standard OTBR routes such as <code>/node</code> and <code>/diagnostics</code> need no authentication — consistent with the Zigbee bridge on port 6638; this product\'s trust boundary is the local network. The price is that anyone on the LAN can read the <b>Thread network key</b> and rewrite the Thread network.')}
                <br><br>
                ${t('When on, the Home Assistant OTBR integration stops working — <b>both :80 and :8081 are governed by this switch</b>. Change whichever port HA currently uses and enter the URL as <code>http://otbr:&lt;device password&gt;@&lt;device IP&gt;:&lt;port&gt;</code> (any username, only the password is checked). No OTBR entry can have its URL edited afterwards: delete it and add it again. Switching back off restores access immediately.')}
                ${t('⚠️ The password must be <b>ASCII</b> without <code>/ ? #</code>, otherwise HA fails before sending any request. It also means <b>the gateway admin password is stored in clear text in HA\'s configuration</b> — weigh that yourself.')}
                ${t('A device password must be set before this can be switched on.')}
            </div>
        </div>

        <div class="section-label">${t('TCP Whitelist (Zigbee bridge, port 6638)')}</div>
        <div class="card">
            <div style="display:flex;align-items:center;gap:10px;margin-bottom:10px">
                <label class="switch"><input type="checkbox" id="wl-enabled"><span class="slider"></span></label>
                <span>${t('Enable whitelist')}</span>
                <span id="wl-state"></span>
            </div>
            <div class="form-group">
                <label>${t('Allowed IPs (one per line, up to')} <span id="wl-max">8</span>)</label>
                <textarea class="input" id="wl-ips" rows="4"
                          style="font-family:monospace;resize:vertical"
                          placeholder="192.168.1.100&#10;192.168.1.101"></textarea>
            </div>
            <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap">
                <button class="btn btn-primary" onclick="SettingsPage._wlSave()">${t('Save')}</button>
                <span id="wl-msg"></span>
            </div>
            <div class="hint" style="margin-top:8px">
                ${t('Restricts only the Zigbee bridge port 6638 (Z2M/ZHA), never this management UI — a mistake here cannot lock you out.')}
                ${t('Applies to <b>new connections</b> only; established connections are not dropped.')}
                ${t('Enabled with an empty list = no filtering.')}
            </div>
        </div>

        <div class="section-label">${t('LED Test')}</div>
        <div class="card" id="led-test-card">
            <div class="hint" id="led-test-status">${t('Loading…')}</div>
            <div id="led-test-body" style="display:none">
                <div class="form-row" style="align-items:flex-end">
                    <div class="form-group" style="flex:2">
                        <label>${t('Target')}</label>
                        <div id="led-test-targets" style="display:flex;gap:6px;flex-wrap:wrap"></div>
                    </div>
                    <div class="form-group" style="flex:1;min-width:150px">
                        <label>${t('Colour')}</label>
                        <div style="display:flex;gap:6px;align-items:center">
                            <input type="color" id="led-test-color" value="#ff0000"
                                   style="width:42px;height:36px;padding:2px;background:var(--bg);border:1px solid var(--border);border-radius:4px;cursor:pointer">
                            <div id="led-test-presets" style="display:flex;gap:4px"></div>
                        </div>
                    </div>
                    <div class="form-group" style="flex:0 0 110px;min-width:110px">
                        <label>${t('Hold (s)')}</label>
                        <input class="input" type="number" id="led-test-hold" value="10" min="1" max="60">
                    </div>
                </div>
                <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap">
                    <button class="btn btn-primary" onclick="SettingsPage._ledTest()">${t('Light Up')}</button>
                    <button class="btn btn-sm" onclick="SettingsPage._ledRestore()">${t('Restore')}</button>
                    <span id="led-test-msg"></span>
                </div>
                <div class="hint" style="margin-top:8px">
                    ${t('Overrides the normal display (and night mode) for the hold time, then returns to normal on its own. Restore ends it early.')}
                </div>

                <div style="border-top:1px solid var(--border);margin-top:14px;padding-top:12px">
                    <div class="card-label" style="margin-bottom:6px">${t('Bit Timing (level-shifter compensation)')}</div>
                    <div class="hint" style="margin-bottom:10px">
                        ${t('White shows as green / magenta as pure red / the last LED stays dark &rarr; <b>T1L is too short</b>: consecutive 1 bits run together and the following bytes are lost. Lower T1H until all four LEDs are clean white. Changes live in RAM only; a reboot restores the defaults.')}
                    </div>
                    <div class="form-row" style="align-items:flex-end">
                        <div class="form-group" style="flex:0 0 150px;min-width:150px">
                            <label>${t('T0H (ns) &mdash; 0 bit')}</label>
                            <input class="input" type="number" id="led-t0h" step="50" min="50" max="1000">
                        </div>
                        <div class="form-group" style="flex:0 0 150px;min-width:150px">
                            <label>${t('T1H (ns) &mdash; 1 bit')}</label>
                            <input class="input" type="number" id="led-t1h" step="50" min="50" max="1000">
                        </div>
                        <div class="form-group" style="flex:1;min-width:200px">
                            <label>${t('Low time (1200 minus the above)')}</label>
                            <div id="led-tl" class="card-value" style="font-size:14px">--</div>
                        </div>
                    </div>
                    <div style="display:flex;gap:6px;align-items:center;flex-wrap:wrap">
                        <button class="btn btn-primary" onclick="SettingsPage._ledApplyTiming()">${t('Apply + White')}</button>
                        <button class="btn btn-sm" onclick="SettingsPage._ledNudge(-50)">T1H &minus;50</button>
                        <button class="btn btn-sm" onclick="SettingsPage._ledNudge(50)">T1H +50</button>
                        <button class="btn btn-sm" onclick="SettingsPage._ledResetTiming()">${t('Reset to default')}</button>
                        <span id="led-timing-msg"></span>
                    </div>
                </div>
            </div>
        </div>

        <div class="section-label">${t('Danger Zone')}</div>
        <div class="card" style="border-color:var(--red)">
            <div style="display:flex;gap:12px;flex-wrap:wrap">
                <button class="btn btn-danger" onclick="SettingsPage._factory()">${t('Factory Reset')}</button>
                <button class="btn btn-danger" onclick="SettingsPage._reboot()">${t('Reboot Device')}</button>
            </div>
            <div style="display:flex;align-items:center;gap:10px;margin-top:12px">
                <label class="switch"><input type="checkbox" id="fr-wipe-thread"><span class="slider"></span></label>
                <span>${t('Also erase the Thread network')}</span>
            </div>
            <div class="hint" style="margin-top:8px">
                ${t('A factory reset erases every gateway setting (WiFi, password, MQTT, LED, timezone, …) and reboots. It cannot be undone.')}<br>
                <b>${t('The Thread network is kept by default')}</b>${t(': the network key and child records stay, and Thread devices come back on their own after the reboot.')}<br>
                ${t('Only with the switch above is the Thread network erased too — every Thread / Matter device becomes orphaned and must be commissioned again, one by one. Use it when handing the device over, or when the Thread data itself is suspect.')}<br>
                ${t('Neither variant <b>touches Zigbee</b>: the Zigbee network lives inside the MG24 chip, which a factory reset does not reach.')}
            </div>
        </div>`;

        API.status().then(d => {
            if (!d || !document.getElementById('set-hostname')) return;
            document.getElementById('set-hostname').textContent = d.hostname;
            /* Prefill the editor from the device, not from a hardcoded default —
               otherwise a user who opens the page and hits Save without typing
               would silently rename the device to whatever we guessed. */
            const hnIn = document.getElementById('set-hostname-in');
            if (hnIn && !hnIn.value) hnIn.value = d.hostname || '';
            document.getElementById('set-fw').textContent = d.fw_version;
            document.getElementById('set-www').textContent = d.www_version || t('unknown');
            /* Factory identity (fctry). Null on units that were never provisioned —
               show "--" rather than inventing a value; support asks for this
               together with the firmware version (user manual, section 12). */
            document.getElementById('set-sn').textContent = d.serial_number || '--';
            document.getElementById('set-mac').textContent = d.mac || '--';
            document.getElementById('set-heap').textContent = (d.free_heap / 1024).toFixed(0) + ' KB';
        });

        this._ledInit();
        API.ntpGet().then(d => this._ntpShow(d));
        API.radioRegionGet().then(d => this._regionShow(d));
        API.whitelistGet().then(d => this._wlShow(d));
        this._otbrAuthLoad();
        this._ahLoad();
        this._otbrTokenLoad();
    },

    destroy() {},

    /* Badge with **textContent** for the message. Several messages carry a
       server-supplied error code (project rule S1: anything arriving over the
       network is untrusted and must not enter innerHTML). The class is a
       literal from this file, so interpolating it is safe. */
    _badge(holderId, cls, text) {
        const m = document.getElementById(holderId);
        if (!m) return;
        /* Badge id = holder id minus its -msg suffix, plus -badge — the ids the
           node tests (frontend/test_login.sh) assert on: set-pw-badge,
           otbr-auth-badge, ot-badge, ah-badge. */
        const bid = holderId.replace(/-msg$/, '') + '-badge';
        m.innerHTML = `<span class="badge ${cls}" id="${bid}"></span>`;
        const b = document.getElementById(bid);
        if (b) b.textContent = text;
    },

    // ---- OTBR access token ----
    async _saveHostname() {
        const el = document.getElementById('set-hostname-in');
        const msg = document.getElementById('set-hostname-msg');
        const name = (el?.value || '').trim();
        if (!name) { this._badge('set-hostname-msg', 'red', t('Enter a name')); return; }
        const r = await API.post('/api/v1/system/hostname', { hostname: name });
        if (r && r.status === 'ok') {
            this._badge('set-hostname-msg', 'green', t('Renamed to {n}', { n: r.hostname }));
            const card = document.getElementById('set-hostname');
            if (card) card.textContent = r.hostname;
        } else {
            /* The firmware validates; surface its verdict instead of guessing.
               invalid_hostname means the name broke the letter/digit/hyphen rule. */
            this._badge('set-hostname-msg', 'red',
                r?.error === 'invalid_hostname' ? t('Invalid name — letters, digits and hyphens only')
                                                : (r?.error || t('Failed')));
        }
    },

    /* Menu labels by code. The codes themselves come from the firmware (the only
       copy of the menu, config_mgr/radio_region.c); an unknown code is shown as-is
       rather than dropped, so a newer firmware never hides an option. */
    _regionLabel(code) {
        return {
            EU: t('European Union / United Kingdom / other ETSI regions'),
            US: t('United States'),
            CA: t('Canada'),
            ZZ: t('Other region (8 dBm, safe everywhere)'),
        }[code] || code;
    },

    _regionShow(d) {
        const sel = document.getElementById('set-region');
        if (!sel) return;
        if (!d) { sel.innerHTML = `<option value="">${t('unknown')}</option>`; return; }
        const menu = Array.isArray(d.menu) ? d.menu : [];
        sel.innerHTML = [['', t('Not set')], ...menu.map(c => [c, this._regionLabel(c)])]
            .map(([v, l]) => `<option value="${v}">${l}</option>`).join('');
        /* The device is the source of truth for the current value; "" = not set. */
        sel.value = d.region || '';
        this._regionResult(d);
    },

    async _saveRegion() {
        const sel = document.getElementById('set-region');
        const v = (sel && sel.value) || '';
        /* Applying is disruptive (Zigbee drops ~40 s, MG24 soft-resets), so it must
           never happen from a stray click — same rule as the reboot button. */
        if (!confirm(t('Apply the region now? Zigbee disconnects for about a minute and the Zigbee chip restarts.'))) return;
        const r = await API.radioRegionSet(v || null);
        if (!r || r.error) {
            /* The firmware validates against its menu; show its verdict, do not guess */
            this._badge('set-region-msg', 'red', r?.error === 'region_not_in_menu'
                ? t('Region not in the menu') : (r?.error || t('Failed')));
            return;
        }
        this._badge('set-region-msg', 'green', r.applied === 'now'
            ? t('Saved and applied')
            : t('Saved — the Zigbee chip was busy, applies after the next reboot'));
        this._regionResult(r);
    },

    /* What the two radios ended up with. All numbers come from the reply; nothing is
       computed here (the firmware is the only place that knows the caps). */
    _regionResult(r) {
        const out = document.getElementById('set-region-result');
        if (!out || !r) return;
        const z = r.zigbee || {};
        const zig = z.ncp_xncp === false
            ? t('Zigbee: the chip firmware lacks region support — update it on the Update page')
            : (typeof z.cap_dbm === 'number' ? t('Zigbee cap: {n} dBm', { n: z.cap_dbm }) : t('Zigbee: unknown'));
        const th = (typeof r.thread_tx_dbm === 'number')
            ? t('Thread: {n} dBm', { n: r.thread_tx_dbm }) : t('Thread: unknown');
        out.textContent = `${zig} · ${th}`;
    },

    _otbrShowUrl(d) {
        const box = document.getElementById('ot-url-box');
        const u   = document.getElementById('ot-url');
        const b   = document.getElementById('ot-copy');
        if (!box || !u) return;
        if (d && d.enabled && d.url) {
            u.textContent = d.url;
            box.style.display = '';
            if (b) b.onclick = () => {
                navigator.clipboard?.writeText(u.textContent).then(
                    () => { b.textContent = t('Copied'); setTimeout(() => b.textContent = t('Copy'), 1500); },
                    () => { b.textContent = t('Copy failed'); setTimeout(() => b.textContent = t('Copy'), 1500); });
            };
        } else {
            box.style.display = 'none';
        }
    },

    _otbrSay(cls, text) { this._badge('ot-msg', cls, text); },

    async _otbrTokenLoad() {
        let d = null;
        try { d = await API.otbrTokenGet(); } catch (e) { /* offline */ }
        if (!d) return;
        this._otbrShowUrl(d);
        this._otbrSay(d.enabled ? 'badge-green' : 'badge-yellow',
                      d.enabled ? t('Enabled') : t('Not set (OTBR routes are open to the local network)'));
    },

    async _otbrTokenGen() {
        this._otbrSay('badge-blue', t('Generating…'));
        let d = null;
        try { d = await API.otbrTokenSet('generate'); } catch (e) { /* offline */ }
        if (d && d.status === 'ok') {
            this._otbrShowUrl(d);
            /* Spell out the consequence, or the user thinks one click is all —
               in reality the HA entry has to be recreated. */
            this._otbrSay('badge-green', t('Generated — remember to delete and re-add the OTBR entry in HA'));
            return;
        }
        this._otbrSay('badge-red', t('Generation failed'));
    },

    async _otbrTokenClear() {
        this._otbrSay('badge-blue', t('Clearing…'));
        let d = null;
        try { d = await API.otbrTokenSet('clear'); } catch (e) { /* offline */ }
        if (d && d.status === 'ok') {
            this._otbrShowUrl(d);
            this._otbrSay('badge-yellow', t('Cleared — OTBR routes are open to the local network again'));
            return;
        }
        this._otbrSay('badge-red', t('Clearing failed'));
    },

    // ---- Extra access addresses (cross-origin guard allow list) ----
    async _ahLoad() {
        let d = null;
        try { d = await API.accessHostGet(); } catch (e) { /* offline */ }
        if (!d) return;
        const set = (id, text) => { const e = document.getElementById(id); if (e) e.textContent = text; };
        set('ah-host', d.hostname || '-');
        /* `accepted` is rendered by the firmware (the suffix table lives in
           webui_server.c). The frontend keeps no copy — a copy inevitably
           drifts, and drift shows up as "the page says it works, the device
           says 421". */
        set('ah-accepted', (d.accepted || []).join(', ') || '-');
        const inp = document.getElementById('ah-extra');
        if (inp) inp.value = d.extra_host || '';
        this._ahCurrent = d.current || location.host;
    },

    _ahFillCurrent() {
        const inp = document.getElementById('ah-extra');
        /* Strip the port: the guard removes :port from Host before comparing, so
           entering it would only create a value that never matches — and the
           user sees "still 421 after adding it". */
        if (inp) inp.value = String(this._ahCurrent || location.host).replace(/:\d+$/, '');
    },

    async _ahSave() {
        const inp = document.getElementById('ah-extra');
        const say = (cls, text) => this._badge('ah-msg', cls, text);
        say('badge-blue', t('Saving…'));
        let r = null;
        try { r = await API.accessHostSet(inp ? inp.value.trim() : ''); } catch (e) { /* offline */ }
        if (r && r.status === 'ok') { say('badge-green', t('Saved')); this._ahLoad(); return; }
        const MAP = { invalid_host: 'Invalid format: letters, digits, - and . only, no port or slash',
                      save_failed:  'The device could not save it',
                      missing_extra_host: 'Invalid parameter' };
        const code = r && r.error;
        say('badge-red', Object.prototype.hasOwnProperty.call(MAP, code) ? t(MAP[code]) : t('Save failed'));
    },

    // ---- NTP / timezone ----
    _ntpShow(d) {
        if (!d || !document.getElementById('ntp-tz')) return;
        document.getElementById('ntp-tz').value = d.timezone;
        document.getElementById('ntp-server').value = d.server;
        document.getElementById('ntp-interval').value = d.sync_interval;
        document.getElementById('ntp-now').textContent =
            d.local_time + (d.synced ? '' : t(' (not synchronized with NTP)'));
        /* Warn only when it was never set. tz_declared rather than
           timezone=="UTC+0" — a user who really lives in UTC should not be
           nagged forever. */
        document.getElementById('ntp-warn').style.display = d.tz_declared ? 'none' : '';
    },

    _ntpDetect() {
        const r = TZ.detect();
        document.getElementById('ntp-tz').value = r.tz;
        document.getElementById('ntp-detected').innerHTML =
            t('Browser timezone <b>{iana}</b> → <code>{tz}</code>', { iana: r.iana || t('(unknown)'), tz: r.tz }) +
            (r.dst ? t(' (with daylight-saving rules)') : t(' (this timezone has no daylight saving)')) +
            ' ' + t('— press Save to apply, then check the device time.');
    },

    async _ntpSave() {
        const msg = document.getElementById('ntp-msg');
        const tz = document.getElementById('ntp-tz').value.trim();
        const server = document.getElementById('ntp-server').value.trim();
        const interval = parseInt(document.getElementById('ntp-interval').value, 10);
        if (!tz || !server) {
            msg.innerHTML = `<span class="badge badge-red">${t('Timezone and server must not be empty')}</span>`; return;
        }
        if (!Number.isInteger(interval) || interval < 60 || interval > 86400) {
            msg.innerHTML = `<span class="badge badge-red">${t('Sync interval must be 60-86400 s')}</span>`; return;
        }
        const r = await API.ntpSet({ timezone: tz, server, sync_interval: interval });
        if (!r || r.error) {
            this._badge('ntp-msg', 'badge-red', (r && r.error) || t('Failed')); return;
        }
        this._ntpShow(r);
        msg.innerHTML = `<span class="badge badge-green">${t('Saved and applied')}</span>`;
    },

    // ---- TCP whitelist ----
    _wlShow(d) {
        if (!d || !document.getElementById('wl-enabled')) return;
        document.getElementById('wl-enabled').checked = d.enabled;
        document.getElementById('wl-ips').value = (d.ips || []).join('\n');
        document.getElementById('wl-max').textContent = d.max;
        /* enabled and active are two different things: switched on with an
           empty list, tcp_server does not filter. Say so, or the user believes
           something is being blocked. */
        document.getElementById('wl-state').innerHTML = d.active
            ? `<span class="badge badge-green">${t('Active · {n} IP(s)', { n: d.count })}</span>`
            : (d.enabled
                ? `<span class="badge badge-yellow">${t('Enabled but the list is empty = no filtering')}</span>`
                : `<span class="badge badge-blue">${t('Not enabled')}</span>`);
    },

    async _otbrAuthLoad() {
        const cb = document.getElementById('otbr-auth');
        if (!cb) return;
        let d = null;
        try { d = await API.otbrAuthGet(); } catch (e) { /* offline */ }
        if (d && typeof d.enabled === 'boolean') { cb.checked = d.enabled; return; }
        /* If it cannot be read, do not pretend to know: disable the switch and
           say so. Leaving a possibly-wrong state for the user to click means
           one click sets the device to whatever they assumed. */
        cb.disabled = true;
        this._badge('otbr-auth-msg', 'badge-yellow', t('Could not read; reload the page to retry'));
    },

    async _otbrAuthToggle() {
        const cb  = document.getElementById('otbr-auth');
        const say = (cls, text) => this._badge('otbr-auth-msg', cls, text);
        const want = cb.checked;
        cb.disabled = true;                       // disabled in flight: with two quick
        say('badge-blue', t('Saving…'));          // clicks the later response decides what is shown
        let r = null;
        try { r = await API.otbrAuthSet(want); } catch (e) { /* offline */ }
        cb.disabled = false;

        if (r && typeof r.enabled === 'boolean') {
            cb.checked = r.enabled;
            say('badge-green', r.enabled ? t('Enabled — remember to change the OTBR URL in HA') : t('Disabled'));
            return;
        }
        cb.checked = !want;                       // on failure, flip the switch back
        /* Only known codes are mapped. Names on Object.prototype (constructor
           and the like) would hit the prototype chain and paint function source
           into the badge — the current firmware never returns those, but do not
           leave the hole open. */
        const MAP = { set_device_password_first: 'Set a device password first',
                      enabled_must_be_bool: 'Invalid parameter',
                      save_failed: 'The device could not save it' };
        const code = r && r.error;
        say('badge-red', Object.prototype.hasOwnProperty.call(MAP, code) ? t(MAP[code]) : t('Save failed'));
    },

    async _wlSave() {
        const msg = document.getElementById('wl-msg');
        const ips = document.getElementById('wl-ips').value
            .split('\n').map(s => s.trim()).filter(Boolean);
        const r = await API.whitelistSet({
            enabled: document.getElementById('wl-enabled').checked,
            ips,
        });
        if (!r || r.error) {
            this._badge('wl-msg', 'badge-red', (r && r.error) || t('Failed')); return;
        }
        this._wlShow(r);
        msg.innerHTML = `<span class="badge badge-green">${t('Saved')}</span>`;
    },

    // ---- LED test panel ----
    _ledSel: -1,
    _ledHoldMin: 1,
    _ledHoldMax: 60,

    async _ledInit() {
        const status = document.getElementById('led-test-status');
        const info = await API.ledTestInfo();
        if (!status) return;                                  // navigated away
        if (!info) { status.textContent = t('Unavailable.'); return; }
        if (!info.supported) {
            status.innerHTML = `<span class="badge badge-yellow">${t('No WS2812B on this board')}</span>`;
            return;
        }

        /* The target buttons are generated entirely from the table the backend
           sends — the index→LED mapping is decided by the PCB traces
           (schematic sheet 5). Hardcoding it here would point at the wrong LED
           after a board revision, and a test panel pointing at the wrong LED is
           worse than none: people go and debug the wrong subsystem. */
        const targets = info.leds.map(l =>
            `<button class="btn btn-sm" data-idx="${l.index}"
                     onclick="SettingsPage._ledPick(${l.index})">${l.name}</button>`).join('');
        document.getElementById('led-test-targets').innerHTML = targets +
            `<button class="btn btn-sm" data-idx="${info.all_index}"
                     onclick="SettingsPage._ledPick(${info.all_index})">${t('all')}</button>`;

        const presets = ['ff0000', '00ff00', '0000ff', 'ffffff', 'ffaa00', 'ff00ff'];
        document.getElementById('led-test-presets').innerHTML = presets.map(c =>
            `<span onclick="SettingsPage._ledPreset('${c}')" title="#${c}"
                   style="width:18px;height:18px;border-radius:3px;cursor:pointer;
                          border:1px solid var(--border);background:#${c}"></span>`).join('');

        const hold = document.getElementById('led-test-hold');
        this._ledHoldMin = info.hold_s.min;
        this._ledHoldMax = info.hold_s.max;
        hold.min = this._ledHoldMin;
        hold.max = this._ledHoldMax;

        status.style.display = 'none';
        document.getElementById('led-test-body').style.display = '';
        this._ledPick(info.all_index);

        this._ledShowTiming(await API.ledTiming());
    },

    _ledPick(i) {
        this._ledSel = i;
        /* Selection uses inline style rather than a class: .btn-sm comes after
           .btn-primary in the stylesheet, so an added class would have its
           background overridden and the selection would not show. */
        document.querySelectorAll('#led-test-targets button').forEach(b => {
            const on = Number(b.dataset.idx) === i;
            b.style.background = on ? 'var(--accent)' : '';
            b.style.color      = on ? '#000' : '';
        });
    },

    _ledPreset(hex) {
        document.getElementById('led-test-color').value = '#' + hex;
    },

    async _ledTest() {
        const msg = document.getElementById('led-test-msg');
        // <input type="color"> is always #rrggbb — exactly the 6-digit hex the backend wants
        const color = document.getElementById('led-test-color').value.replace('#', '');
        const hold = parseInt(document.getElementById('led-test-hold').value, 10);
        if (!Number.isInteger(hold) || hold < this._ledHoldMin || hold > this._ledHoldMax) {
            msg.innerHTML = `<span class="badge badge-red">${t('Hold must be {min}-{max} s', { min: this._ledHoldMin, max: this._ledHoldMax })}</span>`;
            return;
        }
        const r = await API.ledTest(this._ledSel, color, hold);
        if (r && r.status === 'ok') msg.innerHTML = `<span class="badge badge-green">${t('Lit for {n} s', { n: hold })}</span>`;
        else this._badge('led-test-msg', 'badge-red', (r && r.error) || t('Failed'));
    },

    // ---- bit timing ----
    _ledTimingDefaults: null,

    _ledShowTiming(tm) {
        if (!tm || !document.getElementById('led-t0h')) return;
        document.getElementById('led-t0h').value = tm.t0h_ns;
        document.getElementById('led-t1h').value = tm.t1h_ns;
        /* The low time is the criterion itself: T1L eaten by the level shifter's
           stretching was exactly this fault, so it is computed for the user
           instead of asking them to work out 1200 minus T1H in their head. */
        document.getElementById('led-tl').textContent =
            `T0L ${tm.t0l_ns} ns / T1L ${tm.t1l_ns} ns`;
        if (!this._ledTimingDefaults) {
            this._ledTimingDefaults = { t0h: tm.default_t0h_ns, t1h: tm.default_t1h_ns };
        }
    },

    /* Light white immediately after applying — white needs all three G/R/B
       bytes to survive and is the only colour that reveals "bytes lost after a
       1". A single colour (red/green/blue) always looks right; tuning with it
       would repeat the wrong-timing mistake made before 2026-08-13. */
    async _ledApplyTiming() {
        const msg = document.getElementById('led-timing-msg');
        const t0h = parseInt(document.getElementById('led-t0h').value, 10);
        const t1h = parseInt(document.getElementById('led-t1h').value, 10);
        if (!Number.isInteger(t0h) || !Number.isInteger(t1h)) {
            msg.innerHTML = `<span class="badge badge-red">${t('Numbers required')}</span>`; return;
        }
        if (t0h >= t1h) {
            msg.innerHTML = `<span class="badge badge-red">${t('T0H must be less than T1H')}</span>`; return;
        }
        const r = await API.ledSetTiming(t0h, t1h);
        if (!r || r.error) {
            this._badge('led-timing-msg', 'badge-red', (r && r.error) || t('Failed')); return;
        }
        this._ledShowTiming(r);
        await API.ledTest(-1, 'ffffff', 10);
        msg.innerHTML = `<span class="badge badge-green">${t('Applied · four LEDs white for 10 s')}</span>`;
    },

    async _ledNudge(delta) {
        const el = document.getElementById('led-t1h');
        el.value = Math.max(50, Math.min(1000, (parseInt(el.value, 10) || 0) + delta));
        await this._ledApplyTiming();
    },

    async _ledResetTiming() {
        const d = this._ledTimingDefaults;
        if (!d) return;
        document.getElementById('led-t0h').value = d.t0h;
        document.getElementById('led-t1h').value = d.t1h;
        await this._ledApplyTiming();
    },

    async _ledRestore() {
        /* There is no "cancel test" endpoint, and none is needed: a 1-second
           all-off cuts the test window to its minimum, after which the normal
           display resumes on its own. 1 s is the hold_s floor (led_ctrl_test
           does not accept 0). */
        const r = await API.ledTest(-1, '000000', this._ledHoldMin);
        document.getElementById('led-test-msg').innerHTML =
            r && r.status === 'ok' ? `<span class="badge badge-green">${t('Restored')}</span>` : '';
    },

    async _changePw() {
        const p1 = document.getElementById('set-pw1').value;
        const p2 = document.getElementById('set-pw2').value;
        const bad = (text) => this._badge('set-pw-msg', 'badge-red', text);
        if (p1 !== p2)     { bad(t('Passwords do not match')); return; }
        if (p1.length < 4) { bad(t('Min 4 characters')); return; }
        /* Same byte limit as the login and first-run setup pages. Missing it
           here has the worst consequence: this is where an **existing user
           changes the password**, and setting one the login endpoint cannot
           accept locks them out until a factory reset. The backend (auth.c)
           blocks it too — curl bypasses the frontend — this only lets the user
           see on the spot why it was refused. */
        if (PwField.passwordTooLong(p1)) { bad(t('Password too long for this device')); return; }
        const r = await API.setPassword(p1);
        const ok = r && r.status === 'ok';
        if (ok) {
            this._badge('set-pw-msg', 'badge-green', t('Password updated'));
        } else {
            const MAP = { password_too_short: 'Min 4 characters',
                          password_too_long:  'Password too long for this device' };
            const code = r && r.error;
            bad(Object.prototype.hasOwnProperty.call(MAP, code) ? t(MAP[code]) : t('Failed'));
        }
        /* On success clear the fields and hide the plaintext: the new password
           has no business staying on screen. On failure keep them, so the user
           need not type everything again. */
        if (ok) PwField.reset('set-pw1', 'set-pw2');
    },

    async _export() {
        const data = await API.configExport();
        if (!data) return;
        const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' });
        const a = document.createElement('a');
        a.href = URL.createObjectURL(blob); a.download = 'bitengine-config.json'; a.click();
    },

    async _import(file) {
        if (!file) return;
        try {
            const json = JSON.parse(await file.text());
            const r = await API.configImport(json);
            if (r && r.status === 'ok') document.getElementById('set-cfg-msg').innerHTML = `<span class="badge badge-green">${t('Imported')}</span>`;
            else this._badge('set-cfg-msg', 'badge-red', (r && r.error) || t('Failed'));
        } catch { document.getElementById('set-cfg-msg').innerHTML = `<span class="badge badge-red">${t('Invalid JSON')}</span>`; }
    },

    async _factory() {
        const wipeThread = document.getElementById('fr-wipe-thread').checked;
        /* The confirmation must say what this particular click does — the Thread
           part is irreversible and cannot hide behind a generic "all settings
           will be erased". */
        if (!confirm(wipeThread
            ? t('Factory reset AND erase the Thread network?') + '\n' + t('Every Thread / Matter device becomes orphaned and must be commissioned again, one by one.')
            : t('Factory reset?') + '\n' + t('All gateway settings will be erased (the Thread network is kept, Zigbee is not affected).'))) return;
        if (!confirm(t('This cannot be undone. Continue?'))) return;
        await API.factoryReset(wipeThread);
    },

    async _reboot() {
        if (!confirm(t('Reboot device?'))) return;
        await API.reboot();
    }
};

App.registerPage('settings', SettingsPage);
