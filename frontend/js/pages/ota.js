// The two web-updatable partitions: app (bitengine-*.bin → ota_0/ota_1) and
// www (www.bin → the SPIFFS frontend). The bootloader and partition table are
// not among them — they have no A/B copy and no rollback, a bad write bricks
// the device, and they can only be re-flashed over a wire.
const OTA_TARGETS = {
    fw: {
        prefix:  'ota',
        upload:  (f, p) => API.otaUpload(f, p),
        maxSize: 3 * 1024 * 1024,
        maxText: '3 MB',
        hint:    () => t('bitengine-&lt;version&gt;.bin — about 2 MB'),
        btnText: () => t('Upload &amp; Update'),
    },
    www: {
        prefix:  'www',
        upload:  (f, p) => API.wwwUpload(f, p),
        maxSize: 512 * 1024,          // www partition = 0x80000
        maxText: '512 KB',
        hint:    () => t('www.bin — always 512 KB, produced next to the firmware'),
        btnText: () => t('Upload Frontend'),
    },
};

const OTAPage = {
    render(el) {
        el.innerHTML = `
        <div class="page-title">${t('Firmware Update')}</div>

        <div class="section-label">${t('Current Firmware')}</div>
        <div class="card-grid" style="grid-template-columns:repeat(auto-fill,minmax(160px,1fr))">
            <div class="card"><div class="card-label">${t('Version')}</div><div class="card-value" id="ota-version">--</div></div>
            <div class="card"><div class="card-label">${t('Frontend')}</div><div class="card-value" id="ota-www">--</div></div>
            <div class="card"><div class="card-label">${t('Zigbee NCP')}</div><div class="card-value" id="ota-ncp" style="font-size:15px">--</div></div>
            <div class="card"><div class="card-label">${t('Uptime')}</div><div class="card-value" id="ota-uptime">--</div></div>
        </div>

        <div class="section-label">${t('Upload New Firmware')}</div>
        <div class="card">
            <div class="form-group">
                <label>${t('Select firmware file (.bin, max {max})', { max: OTA_TARGETS.fw.maxText })}</label>
                <input type="file" id="ota-file" accept=".bin" class="input" style="padding:8px">
            </div>
            <div style="display:flex;align-items:center;gap:12px">
                <button id="ota-btn" class="btn btn-primary" onclick="OTAPage._upload('fw')">${OTA_TARGETS.fw.btnText()}</button>
                <span id="ota-msg"></span>
            </div>
            <div id="ota-progress" style="display:none;margin-top:12px">
                <div class="progress-bar"><div class="progress-fill" id="ota-fill"></div></div>
                <div id="ota-pct" class="text-center text-muted">0%</div>
            </div>
            <div class="hint" style="margin-top:8px">${OTA_TARGETS.fw.hint()}</div>
        </div>

        <div class="section-label">${t('Update Frontend (WebUI)')}</div>
        <div class="card">
            <div class="form-group">
                <label>${t('Select frontend image (www.bin, max {max})', { max: OTA_TARGETS.www.maxText })}</label>
                <input type="file" id="www-file" accept=".bin" class="input" style="padding:8px">
            </div>
            <div style="display:flex;align-items:center;gap:12px">
                <button id="www-btn" class="btn btn-primary" onclick="OTAPage._upload('www')">${OTA_TARGETS.www.btnText()}</button>
                <span id="www-msg"></span>
            </div>
            <div id="www-progress" style="display:none;margin-top:12px">
                <div class="progress-bar"><div class="progress-fill" id="www-fill"></div></div>
                <div id="www-pct" class="text-center text-muted">0%</div>
            </div>
            <div class="hint" style="margin-top:8px">
                ${OTA_TARGETS.www.hint()}<br>
                ${t('Updates only the web interface — Zigbee and Thread are not affected, but the device still reboots. Reload this page afterwards.')}
            </div>
        </div>

        <div class="section-label">${t('Update Zigbee Firmware (MG24 NCP)')}</div>
        <div class="card">
            <div class="form-group">
                <label>${t('Select NCP image (.gbl, max 1 MB)')}</label>
                <input type="file" id="zb-file" accept=".gbl" class="input" style="padding:8px">
            </div>
            <div style="display:flex;align-items:center;gap:12px">
                <button id="zb-btn" class="btn btn-primary" onclick="OTAPage._uploadZigbee()">${t('Upload &amp; Flash NCP')}</button>
                <span id="zb-msg"></span>
            </div>
            <div id="zb-progress" style="display:none;margin-top:12px">
                <div class="progress-bar"><div class="progress-fill" id="zb-fill"></div></div>
                <div id="zb-pct" class="text-center text-muted">0%</div>
            </div>
            <div id="zb-unlock" style="margin-top:12px;display:flex;align-items:center;gap:12px;flex-wrap:wrap">
                <span id="zb-unlock-state" class="badge badge-green">${t('Unsigned firmware: locked')}</span>
                <button id="zb-unlock-btn" class="btn btn-sm" onclick="OTAPage._unlockNcp()">${t('Allow unsigned firmware (developer)')}</button>
                <button id="zb-lock-btn" class="btn btn-sm" style="display:none" onclick="OTAPage._lockNcp()">${t('Lock again')}</button>
                <span id="zb-unlock-hint" class="text-muted"></span>
            </div>
            <div class="hint" style="margin-top:8px">
                ${t('zigbee_ncp_uart_hw.gbl — about 240 KB. The gateway writes it over UART (XMODEM), so <b>Zigbee goes down for roughly 20&ndash;30 seconds</b> and Z2M/ZHA will reconnect on their own. The device does <b>not</b> reboot.')}<br>
                ${t('Do not start this while pairing a device. A <code>.s37</code> is the SWD/J-Link format and is rejected — this path only takes GBL.')}<br>
                ${t('Only images signed with the official key are accepted; a bad or missing signature is refused before the chip is touched. To flash third-party firmware, press the Allow unsigned firmware button and confirm on the device: hold the button until the light turns solid red (about 2 s), then release before 10 s.')}
            </div>
        </div>

        <div class="section-label">${t('Cloud Update')}</div>
        <div class="card">
            <div style="display:flex;align-items:center;gap:12px;flex-wrap:wrap">
                <button id="cloud-check-btn" class="btn" onclick="OTAPage._check()">${t('Check for updates')}</button>
                <button id="cloud-install-btn" class="btn btn-primary" style="display:none"
                        onclick="OTAPage._cloudInstall()">${t('Install the new version')}</button>
                <span id="cloud-msg" class="text-muted">${t('Not checked yet')}</span>
            </div>
            <div id="cloud-notes" class="hint" style="margin-top:8px;display:none"></div>
            <div id="cloud-ncp" style="display:none;margin-top:10px">
                <span id="cloud-ncp-msg" class="text-muted"></span>
                <button id="cloud-ncp-btn" class="btn btn-sm" style="margin-left:8px"
                        onclick="OTAPage._cloudInstallNcp()">${t('Update Zigbee chip firmware')}</button>
            </div>
            <div id="cloud-progress" style="display:none;margin-top:12px">
                <div class="progress-bar"><div class="progress-fill" id="cloud-fill"></div></div>
                <div id="cloud-pct" class="text-center text-muted">0%</div>
            </div>
            <div class="hint" style="margin-top:8px">
                ${t('Pressing "Check for updates" makes the device fetch the version manifest over the internet; this can take up to about 6 seconds, during which the interface is briefly unresponsive.')}<br>
                ${t('A downloaded image is signature-checked before installation; a mismatching signature is refused (the device cannot be bricked this way).')}<br>
                ${t('An update carries the firmware and the web interface together; one reboot applies both.')}
            </div>
        </div>

        <div class="section-label">${t('Recovery Information')}</div>
        <div class="card">
            <div class="hint">
                ${t('After a successful update, the device will reboot automatically.')}<br>
                ${t('Zigbee and Thread devices should reconnect within 60 seconds.')}<br>
                ${t('If devices remain unavailable, reload the ZHA integration in Home Assistant settings.')}<br>
                ${t('Bootloader and partition table cannot be updated over the web — they require a wired re-flash. The same is true of the MG24 bootloader: only the NCP application can be updated here, and a dead NCP needs J-Link on J7.')}
            </div>
        </div>`;

        this._loadStatus();
        this._loadCloud();
        this._refreshUnlock();
    },

    /* ---- developer unlock for unsigned NCP images (T1.2 step 3) ----
       Three states from GET /ota/zigbee/unlock: locked (default), armed
       (waiting for the physical button, 30 s), unlocked (10 min). While armed
       or unlocked the badge counts down by re-polling every second; the
       device is the only source of truth for the timers. */
    _renderUnlock(u) {
        const st = document.getElementById('zb-unlock-state');
        const hint = document.getElementById('zb-unlock-hint');
        const lockBtn = document.getElementById('zb-lock-btn');
        const unlockBtn = document.getElementById('zb-unlock-btn');
        if (!st || !u) return;
        const show = (el, on) => { el.style.display = on ? '' : 'none'; };
        if (u.unlocked) {
            st.className = 'badge badge-red';
            st.textContent = t('Unsigned firmware: allowed for {m}:{s}', {
                m: Math.floor(u.unlock_remaining_s / 60),
                s: String(u.unlock_remaining_s % 60).padStart(2, '0') });
            hint.textContent = t('Any .gbl will be flashed until the timer runs out or the device reboots.');
            show(lockBtn, true); show(unlockBtn, false);
        } else if (u.armed) {
            st.className = 'badge badge-blue';
            st.textContent = t('Waiting for the button: {n}s', { n: u.arm_remaining_s });
            hint.textContent = t('Hold the button on the device until the light turns solid red (about 2 s), then release. Do not hold past 10 s — that starts a factory reset.');
            show(lockBtn, true); show(unlockBtn, false);
        } else if (u.signature_required === false) {
            st.className = 'badge badge-red';
            st.textContent = t('Unsigned firmware: allowed (developer build)');
            hint.textContent = '';
            show(lockBtn, false); show(unlockBtn, false);
        } else {
            st.className = 'badge badge-green';
            st.textContent = t('Unsigned firmware: locked');
            hint.textContent = '';
            show(lockBtn, false); show(unlockBtn, true);
        }
        clearTimeout(this._unlockTimer);
        this._unlockLive = !!(u.unlocked || u.armed);
        if (this._unlockLive) this._unlockTimer = setTimeout(() => this._refreshUnlock(), 1000);
    },

    /* One lost poll must not end the countdown. API.get() turns any failed
       request (device busy, socket purged, WiFi blip) into null; before
       2026-09-14 that null fell into _renderUnlock's early return and the
       chain silently died — the badge sat on "Waiting for the button" for a
       minute after the device had long unlocked, until the page was re-entered.
       While the last known state was armed/unlocked, keep asking. */
    _refreshUnlock() {
        const retry = () => {
            if (!this._unlockLive || !document.getElementById('zb-unlock-state')) return;
            clearTimeout(this._unlockTimer);
            this._unlockTimer = setTimeout(() => this._refreshUnlock(), 2000);
        };
        API.zigbeeUnlockStatus().then(u => {
            if (!document.getElementById('zb-unlock-state')) return;
            if (u) this._renderUnlock(u); else retry();
        }).catch(retry);
    },

    async _unlockNcp() {
        if (!confirm(t('Allow unsigned Zigbee firmware for 10 minutes?') + '\n\n' +
                     t('The gateway will then flash any .gbl you upload, including firmware nobody has checked. Confirm on the device within 30 seconds: hold the button until the light turns solid red (about 2 s), then release.'))) return;
        try {
            this._renderUnlock(await API.zigbeeUnlockArm());
        } catch (e) {
            alert(t('Could not start the unlock') + ': ' + e.message);
        }
    },

    async _lockNcp() {
        try { this._renderUnlock(await API.zigbeeUnlockLock()); } catch (e) { /* next poll shows the truth */ }
    },

    /* Re-entering the page (navigation, language switch) re-renders it from
       scratch, but the device may be mid-install: pick the cloud state back up
       so the phase text, progress bar and the reboot button reappear instead
       of "Not checked yet". 2026-09-10: a language switch during a download
       made the progress vanish while the device kept installing. */
    _loadCloud() {
        API.otaCloudStatus().then(d => {
            if (!d || d.state === 'idle' || !document.getElementById('cloud-msg')) return;
            this._renderCloud(d);
        }).catch(() => {});
    },

    _loadStatus() {
        API.status().then(d => {
            if (!d || !document.getElementById('ota-version')) return;
            document.getElementById('ota-version').textContent = d.fw_version;
            document.getElementById('ota-www').textContent = d.www_version || t('unknown');
            document.getElementById('ota-ncp').textContent = d.ncp_version || t('unknown');
            const s = d.uptime;
            const dd = Math.floor(s/86400), hh = Math.floor((s%86400)/3600), mm = Math.floor((s%3600)/60);
            document.getElementById('ota-uptime').textContent = dd > 0 ? `${dd}d ${hh}h` : hh > 0 ? `${hh}h ${mm}m` : `${mm}m`;
        });
    },

    destroy() {
        // The poll is a self-recursing setTimeout; without clearing it, it keeps
        // hitting the API after the user has left the page.
        clearTimeout(this._pollTimer);
        clearTimeout(this._unlockTimer);
        this._pollTimer = null;
    },


    // ---- Cloud update ----
    // ⚠️ The backend behind _check() fetches the manifest synchronously, and the
    // whole web interface is stuck for up to 6 s meanwhile (webui is a single
    // httpd task). So it may only ever hang off a user click — never a poll or
    // the page-load path, or every visit to this page stalls and the device
    // looks slow.
    async _check() {
        const btn = document.getElementById('cloud-check-btn');
        const msg = document.getElementById('cloud-msg');
        btn.disabled = true; btn.textContent = t('Checking...');
        msg.textContent = t('Querying the update server, up to about 6 seconds...');
        try {
            const d = await API.otaCheck();
            this._renderCloud(d);
        } catch (e) {
            msg.innerHTML = `<span class="badge badge-red">${t('Check failed: {err}', { err: e.message })}</span>`;
        }
        btn.disabled = false; btn.textContent = t('Check for updates');
    },

    _renderCloud(d) {
        const msg   = document.getElementById('cloud-msg');
        const inst  = document.getElementById('cloud-install-btn');
        const notes = document.getElementById('cloud-notes');
        if (!msg) return;                                   // navigated away
        if (!d) { msg.textContent = t('No response'); return; }

        // The device's error is a short identifier (manifest_http_404 and the
        // like). Unknown ones are shown verbatim rather than translated: the
        // user is going to paste it to us, and a translation would not match
        // the logs.
        const ERR = {
            manifest_url_not_configured: 'No update server address is configured on this device',
            connect_failed:              'Cannot reach the update server (check network/DNS)',
            refused_downgrade:           'Downgrade refused: the server version is not newer than the current one',
            signature_invalid:           'Firmware signature verification failed — installation refused',
            // Same signing key for both bitEngine models, so the device compares the image's
            // project name before writing anything (docs/15 design 2.7). Any other ESP image
            // (a third-party build) lands here too, hence "a different device model".
            profile_mismatch:            'This update is for a different device model — refused, nothing was installed',
            profile_unknown:             'Cannot tell which device model this update is for — refused, nothing was installed',
            image_invalid:               'The downloaded file is not a firmware image — refused, nothing was installed',
        };
        const human = e => ERR[e] ? t(ERR[e]) : e;

        inst.style.display  = 'none';
        notes.style.display = 'none';

        switch (d.state) {
            case 'update_available': {
                /* Firmware and web interface travel together (the device applies
                   both in one go); say which of them this update carries. Older
                   firmware without app_update/www_update falls back to the old line. */
                let text;
                if (d.app_update === undefined) {
                    text = t('New version {v} available', { v: d.latest_version });
                } else {
                    const parts = [];
                    if (d.app_update) parts.push(t('firmware {v}', { v: d.latest_version }));
                    if (d.www_update) parts.push(t('web interface {v}', { v: d.latest_www_version }));
                    text = t('Update available: {list}', { list: parts.join(' + ') });
                }
                msg.innerHTML = `<span class="badge badge-green">${text}</span>`;
                inst.style.display = '';
                if (d.notes) { notes.textContent = d.notes; notes.style.display = ''; }
                break;
            }
            case 'up_to_date':
                msg.innerHTML = `<span class="badge">${t('Already up to date')}</span>`;
                break;
            case 'downloading': {
                const PHASE = {
                    www:   t('Downloading web interface...'),
                    app:   t('Downloading and installing firmware...'),
                    write: t('Writing web interface...'),
                };
                msg.textContent = PHASE[d.phase] || t('Downloading and installing...');
                if (d.progress >= 0) {
                    document.getElementById('cloud-progress').style.display = 'block';
                    document.getElementById('cloud-fill').style.width = d.progress + '%';
                    document.getElementById('cloud-pct').textContent  = d.progress + '%';
                }
                this._pollCloud();
                break;
            }
            case 'success':
                /* The cloud path deliberately does not reboot on its own (the user
                   picks the moment: Zigbee/Thread drop for about a minute), so it
                   must offer the reboot right here — otherwise the badge is a dead end. */
                msg.innerHTML = `<span class="badge badge-green">${t('Installed — takes effect after reboot')}</span>
                    <button class="btn btn-danger btn-sm" style="margin-left:8px" onclick="OTAPage._cloudReboot()">${t('Reboot now')}</button>`;
                break;
            case 'failed':
                /* textContent for the badge: `error` arrives from the network. */
                msg.innerHTML = '<span class="badge badge-red" id="cloud-err"></span>';
                document.getElementById('cloud-err').textContent = human(d.error);
                break;
            default:
                msg.textContent = t('Not checked yet');
        }
        this._renderCloudNcp(d);
    },

    /* The Zigbee chip (MG24) firmware is offered separately from firmware +
       web interface: flashing it drops Zigbee for 15-30 s, so the user picks the
       moment. Progress lives in /ota/zigbee/status once the download is handed
       to the flasher (same path as the manual .gbl upload). */
    _renderCloudNcp(d) {
        const box = document.getElementById('cloud-ncp');
        const msg = document.getElementById('cloud-ncp-msg');
        const btn = document.getElementById('cloud-ncp-btn');
        if (!box || !d || d.ncp_update === undefined) return;
        box.style.display = 'none'; btn.style.display = ''; btn.disabled = false;
        if (d.ncp_state === 'downloading') {
            box.style.display = ''; btn.style.display = 'none';
            msg.textContent = t('Downloading Zigbee chip firmware...');
            this._pollCloudNcp();
        } else if (d.ncp_state === 'flashing') {
            box.style.display = ''; btn.style.display = 'none';
            msg.textContent = t('Writing to NCP...');
            this._pollCloudNcp();
        } else if (d.ncp_state === 'failed') {
            box.style.display = '';
            btn.style.display = d.ncp_update ? '' : 'none';
            // Same table as the manual upload; unknown codes come back verbatim (the user pastes them to us)
            msg.textContent = t('Zigbee chip firmware update failed: {err}', { err: d.ncp_error ? this._zigbeeError(d.ncp_error) : '?' });
        } else if (d.ncp_update) {
            box.style.display = '';
            msg.textContent = t('Zigbee chip firmware {v} available', { v: d.latest_ncp_version });
        }
    },

    async _cloudInstallNcp() {
        if (!confirm(t('Flash the MG24 NCP?') + '\n\n' +
                     t('Zigbee will be unavailable for about 20-30 seconds while the chip is flashed. Do not do this while pairing a device.'))) return;
        const btn = document.getElementById('cloud-ncp-btn');
        const msg = document.getElementById('cloud-ncp-msg');
        btn.disabled = true;
        try {
            await API.otaCloudInstallNcp();
            btn.style.display = 'none';
            msg.textContent = t('Downloading Zigbee chip firmware...');
            this._pollCloudNcp();
        } catch (e) {
            msg.textContent = t('Could not start: {err}', { err: e.message });
            btn.disabled = false;
        }
    },

    _pollCloudNcp() {
        clearTimeout(this._pollTimer);
        const prog = document.getElementById('cloud-progress');
        const fill = document.getElementById('cloud-fill');
        const pct  = document.getElementById('cloud-pct');
        const msg  = document.getElementById('cloud-ncp-msg');
        const tick = async () => {
            let d;
            try { d = await API.otaCloudStatus(); } catch (e) { return; }
            if (!d || !document.getElementById('cloud-ncp')) return;
            if (d.ncp_state === 'downloading') {
                if (d.progress >= 0) {
                    prog.style.display = 'block';
                    fill.style.width = d.progress + '%';
                    pct.textContent  = d.progress + '%';
                }
                this._pollTimer = setTimeout(tick, 1000);
                return;
            }
            if (d.ncp_state === 'failed') { this._renderCloudNcp(d); return; }
            /* handed to the MG24 flasher: same poll as the manual upload card */
            prog.style.display = 'block';
            msg.textContent = t('Writing to NCP...');
            try {
                await this._pollZigbee(fill, pct);
                msg.textContent = t('NCP updated');
                document.getElementById('cloud-ncp-btn').style.display = 'none';
                this._loadStatus();
            } catch (e) {
                msg.textContent = t('Zigbee chip firmware update failed: {err}', { err: e.message });
            }
        };
        this._pollTimer = setTimeout(tick, 500);
    },

    async _cloudReboot() {
        if (!confirm(t('Reboot device?'))) return;
        const msg = document.getElementById('cloud-msg');
        try {
            await API.reboot();
        } catch (e) {
            /* The device drops the connection while restarting; that is not a failure. */
        }
        if (msg) msg.textContent = t('Rebooting. The gateway comes back in about a minute.');
    },

    async _cloudInstall() {
        const inst = document.getElementById('cloud-install-btn');
        const msg  = document.getElementById('cloud-msg');
        inst.disabled = true;
        try {
            await API.otaCloudInstall();
            document.getElementById('cloud-progress').style.display = 'block';
            msg.textContent = t('Downloading and installing...');
            this._pollCloud();
        } catch (e) {
            msg.innerHTML = `<span class="badge badge-red">${t('Could not start: {err}', { err: e.message })}</span>`;
            inst.disabled = false;
        }
    },

    // Polling shares the _pollTimer that destroy() clears — leaving the page
    // stops it, otherwise it would keep hitting the API in the background (the
    // MG24 path set that rule originally).
    _pollCloud() {
        clearTimeout(this._pollTimer);
        const tick = async () => {
            let d;
            try { d = await API.otaCloudStatus(); } catch (e) { return; }
            if (!d || !document.getElementById('cloud-progress')) return;
            if (d.progress >= 0) {
                document.getElementById('cloud-progress').style.display = 'block';
                document.getElementById('cloud-fill').style.width = d.progress + '%';
                document.getElementById('cloud-pct').textContent  = d.progress + '%';
            }
            if (d.state === 'downloading') {
                this._pollTimer = setTimeout(tick, 1000);
            } else {
                document.getElementById('cloud-install-btn').disabled = false;
                this._renderCloud(d);
            }
        };
        this._pollTimer = setTimeout(tick, 500);
    },

    // The MG24 differs from the other two targets: the POST only means
    // "accepted" (202); the actual write runs on the device for ten-odd seconds
    // in the background and progress must be polled from /api/v1/ota/zigbee/status.
    async _uploadZigbee() {
        const $ = id => document.getElementById('zb-' + id);
        const file = $('file').files[0];
        if (!file) { alert(t('Select a .gbl file')); return; }
        // .s37 is the SWD flashing format; the server rejects it too
        // (not_a_gbl_image), this just explains why up front.
        if (!file.name.toLowerCase().endsWith('.gbl')) {
            alert(t('That is not a .gbl file.') + '\n\n' +
                  t('The .s37 image is for J-Link/SWD flashing only — this path takes the GBL container.'));
            return;
        }
        if (file.size > 1024 * 1024) { alert(t('File too large (max 1 MB)')); return; }
        if (!confirm(t('Flash the MG24 NCP?') + '\n\n' +
                     t('Zigbee will be unavailable for about 20-30 seconds and Z2M/ZHA will drop and reconnect. Do not do this while pairing a device.'))) return;

        const btn = $('btn'), prog = $('progress'), fill = $('fill');
        const pct = $('pct'), msg = $('msg');

        btn.disabled = true; btn.textContent = t('Uploading...');
        prog.style.display = 'block'; msg.innerHTML = '';
        fill.style.width = '0%';

        try {
            const r = await API.zigbeeOtaUpload(file, (loaded, total) => {
                const p = Math.round(loaded / total * 100);
                fill.style.width = p + '%';
                pct.textContent = t('uploading {p}%', { p });
            });
            if (!r || r.status !== 'accepted') {
                throw new Error(this._zigbeeError(r?.error));
            }
            btn.textContent = t('Flashing...');
            msg.innerHTML = `<span class="badge badge-blue">${t('Writing to NCP...')}</span>`;
            await this._pollZigbee(fill, pct);
            msg.innerHTML = `<span class="badge badge-green">${t('NCP updated')}</span>`;
            this._loadStatus();
        } catch (e) {
            /* e.message may carry the bootloader's own words (from the network):
               textContent, not innerHTML. */
            msg.innerHTML = '<span class="badge badge-red" id="zb-err"></span>';
            document.getElementById('zb-err').textContent = e.message;
        }
        btn.disabled = false; btn.innerHTML = t('Upload &amp; Flash NCP');
    },

    _zigbeeError(code) {
        const M = {
            not_a_gbl_image:      'Not a valid GBL image (bad magic, end tag or CRC32)',
            ncp_image_malformed:  'Malformed GBL container (signature tag shape or tag chain)',
            ncp_signature_invalid: 'Signature check failed — this .gbl was not signed with the official key, or it was modified',
            ncp_image_unsigned:   'This .gbl carries no signature — the gateway only flashes Zigbee firmware signed with the official key',
            // T9.25: decided before the chip is touched; the developer unlock lifts them for web uploads
            ncp_image_downgrade:  'This Zigbee firmware is older than what this chip has already run — the chip would fail to start with it, so it was refused',
            ncp_image_wipe:       'This is a test build that erases the Zigbee network at every start — refused',
            ncp_image_unreadable: 'Cannot read the build information of this firmware (compressed image) — refused',
            truncated_body:       'Upload was truncated — try again',
            file_too_large:       'Image larger than 1 MB',
            mg24_ota_in_progress: 'An NCP update is already running',
            ota_already_in_progress: 'Another OTA is already running',
            alloc_failed:         'Out of memory on the device',
            ncp_unconfirmed:      'Written, but the Zigbee chip did not answer afterwards. If Zigbee does not come back once Home Assistant reconnects, install the official Zigbee firmware again (Check for updates will offer it).',
        };
        return M[code] ? t(M[code]) : (code || t('Upload rejected'));
    },

    _pollZigbee(fill, pct) {
        return new Promise((resolve, reject) => {
            const started = Date.now();
            const tick = async () => {
                const s = await API.zigbeeOtaStatus();
                if (s) {
                    fill.style.width = (s.percent || 0) + '%';
                    pct.textContent = `${s.state} ${s.percent || 0}%`;
                    if (s.state === 'success') { resolve(); return; }
                    // Written, but the new firmware never answered (T9.19 ⑨): not a success, not a proven failure
                    if (s.state === 'unconfirmed') { reject(new Error(t('Written, but the Zigbee chip did not answer afterwards. If Zigbee does not come back once Home Assistant reconnects, install the official Zigbee firmware again (Check for updates will offer it).'))); return; }
                    if (s.state === 'failed') {
                        // `detail` is the bootloader's own text — far more useful than the error code
                        reject(new Error(s.detail || t('NCP update failed')));
                        return;
                    }
                }
                // Same 180 s cap as the script side: XMODEM itself takes ~15 s,
                // the rest is retry headroom.
                if (Date.now() - started > 180000) {
                    reject(new Error(t('Timed out waiting for the NCP update')));
                    return;
                }
                this._pollTimer = setTimeout(tick, 1000);
            };
            tick();
        });
    },

    // kind: 'fw' | 'www' — both targets share the progress/error handling
    async _upload(kind) {
        const tgt = OTA_TARGETS[kind];
        if (!tgt) return;

        const $ = id => document.getElementById(`${tgt.prefix}-${id}`);
        const file = $('file').files[0];
        if (!file) { alert(t('Select a .bin file')); return; }
        // The firmware and the frontend image are both called .bin and are easy
        // to mix up. Size is the most direct tell: www.bin is always 512 KB, the
        // firmware about 2 MB. The server rejects each on its own too; this
        // only catches it earlier.
        if (file.size > tgt.maxSize) {
            alert(t('File too large (max {max}).', { max: tgt.maxText }) + ' ' +
                  (kind === 'www' ? t('Did you pick the firmware .bin by mistake?')
                                  : t('Is this really a firmware image?')));
            return;
        }

        const btn = $('btn'), prog = $('progress'), fill = $('fill');
        const pct = $('pct'), msg = $('msg');

        btn.disabled = true; btn.textContent = t('Uploading...');
        prog.style.display = 'block'; msg.innerHTML = '';

        try {
            const result = await tgt.upload(file, (loaded, total) => {
                const p = Math.round(loaded / total * 100);
                fill.style.width = p + '%';
                pct.textContent = p + '%';
            });
            if (result && result.status === 'ok') {
                fill.style.width = '100%'; pct.textContent = '100%';
                msg.innerHTML = `<span class="badge badge-green">${t('Update successful! Rebooting...')}</span>`;
            } else {
                // Rejections the user can act on get a sentence; anything else shows the
                // device's code verbatim (they will paste it to us, and it must match the logs).
                // Firmware upload only: the device compares the image's project name before
                // writing anything (same signing key for both bitEngine models, docs/15 design 2.7).
                const FW_UP_ERR = {
                    profile_mismatch: 'This firmware is for a different device model — refused, nothing was written',
                    profile_unknown:  'Cannot tell which device model this firmware is for — refused, nothing was written',
                    image_invalid:    'Not a firmware image (the frontend www.bin picked by mistake?) — refused, nothing was written',
                };
                const code = result?.error || 'unknown';
                const known = kind === 'fw' && Object.prototype.hasOwnProperty.call(FW_UP_ERR, code);
                msg.innerHTML = '<span class="badge badge-red" id="' + tgt.prefix + '-err"></span>';
                document.getElementById(tgt.prefix + '-err').textContent =
                    known ? t(FW_UP_ERR[code]) : t('Failed: {err}', { err: code });
            }
        } catch (e) {
            msg.innerHTML = '<span class="badge badge-red" id="' + tgt.prefix + '-err"></span>';
            document.getElementById(tgt.prefix + '-err').textContent = t('Error: {err}', { err: e.message });
        }
        btn.disabled = false; btn.innerHTML = tgt.btnText();
    }
};

App.registerPage('ota', OTAPage);
