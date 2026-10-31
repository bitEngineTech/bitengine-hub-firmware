const SpectrumPage = {
    render(el) {
        el.innerHTML = `
        <div class="page-title">${t('Spectrum Analysis')}</div>

        <div class="section-label">${t('802.15.4 Channel Energy')}</div>
        <div class="card">
            <div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:12px">
                <span class="text-muted">${t('Energy level per channel (dBm). Green = low interference, Red = high.')}</span>
                <button class="btn btn-sm" onclick="SpectrumPage._scan()" id="spec-btn">${t('Scan Now')}</button>
            </div>
            <div id="spec-chart" style="height:180px;display:flex;align-items:flex-end;gap:3px"></div>
            <div id="spec-note" class="text-muted" style="font-size:11px;margin-top:10px"></div>
        </div>

        <div class="section-label">${t('Recommendation')}</div>
        <div class="card">
            <div id="spec-recommend" class="card-value text-muted">${t('Click Scan to analyze')}</div>
        </div>

        <div class="section-label">${t('Neighbor Zigbee Networks')}</div>
        <div class="card">
            <table class="table" id="spec-neighbors"><tr><td class="text-muted">${t('Click Scan to discover')}</td></tr></table>
        </div>`;
    },

    destroy() {},

    async _scan() {
        const btn = document.getElementById('spec-btn');
        btn.disabled = true; btn.textContent = t('Scanning...');
        /* try/finally: if any fetch along the way throws, the button must come
           back, or the page is stuck on "Scanning..." until a reload. */
        try { await this._doScan(); }
        finally { btn.disabled = false; btn.textContent = t('Scan Now'); }
    },

    async _doScan() {
        const data = await API.spectrumScan();
        if (data && data.channels) {
            this._renderChart(data.channels, data.valid_count);
        } else {
            /* Not a single channel was measured (503
               energy_scan_blocked_by_coex and friends). Show the reason rather
               than leaving an empty chart that reads as a broken page. */
            document.getElementById('spec-chart').innerHTML =
                `<div class="text-muted" style="align-self:center;font-size:12px">${t('No spectrum data')}</div>`;
            const note = document.getElementById('spec-note');
            if (note) note.textContent = data && data.error
                ? t('Scan failed: {err}', { err: data.error }) : t('Scan failed.');
        }

        /* Since 2026-09-13 the firmware answers an object
           {completed, dwell_ms, count, networks:[{pan_id,channel,rssi,lqi,self}]}
           (it used to be a bare array, and 501 whenever Thread was running).
           getSoft keeps the 503 body so the *reason* can be shown: with the
           radio owned by OpenThread, discovery needs the border router running. */
        const beacons = await API.spectrumBeacon();
        if (beacons && Array.isArray(beacons.networks)) this._renderNeighbors(beacons);
        else {
            const err = beacons && beacons.error;
            const el = document.getElementById('spec-neighbors');
            if (el) el.innerHTML = `<tr><td class="text-muted">${
                err === 'thread_not_running_or_busy'
                    ? t('Neighbor discovery needs the Thread border router running — it drives the 802.15.4 radio. Start Thread, or retry if another scan is in progress.')
                    : err === 'spectrum_busy'
                        ? t('Another spectrum scan is in progress — retry in a moment.')
                        : (err ? t('Neighbor scan failed: {err}', { err }) : t('Neighbor scan failed.'))}</td></tr>`;
        }

        const rec = await API.spectrumRecommend();
        const recEl = document.getElementById('spec-recommend');
        if (rec && rec.recommended_channel != null) {
            recEl.innerHTML =
                `${t('Recommended:')} <strong style="font-size:20px">CH ${rec.recommended_channel}</strong>` +
                (rec.thread_channel ? ` <span class="text-muted">&middot; ${t('Thread is on CH {n}', { n: rec.thread_channel })}</span>` : '');
        } else {
            /* Insufficient data has to be stated, never passed over in
               silence or left showing the previous verdict — this code used to
               recommend an unmeasured channel as "the cleanest one".
               The firmware's error codes must also be reported separately:
               "Thread is not running at all" and "not enough channels were
               measured" are entirely different problems, and blurring them into
               one sentence makes neither diagnosable. */
            const err = rec && rec.error;
            let msg;
            if (err === 'thread_not_running')          msg = t('Thread border router is not running');
            else if (err === 'energy_scan_unsupported') msg = t('Energy scan not supported by this build');
            else if (err === 'insufficient_spectrum_data')
                msg = t('Not enough channels measured to recommend ({have}/{need})', { have: rec.valid_count, need: rec.required });
            else if (err)                               msg = t('Cannot recommend: {err}', { err });
            else                                        msg = t('Cannot recommend — scan failed');
            recEl.innerHTML = `<span class="text-muted">${msg}</span>`;
        }
    },

    _renderChart(channels, validCount) {
        const el = document.getElementById('spec-chart');
        const min = -110, max = 0;
        el.innerHTML = channels.map(c => {
            /* Unmeasured channels get a full-height faint hatch plus "n/a".
               Neither obvious alternative works: drawing them at -100 makes the
               shortest green bar, which reads as "the cleanest"; drawing a
               solid full bar reads as "the dirtiest" in a chart where height
               means energy. A very faint hatch with n/a reads as a placeholder
               rather than a reading. */
            if (c.energy === null || c.energy === undefined) {
                return `<div style="flex:1;text-align:center;display:flex;flex-direction:column;justify-content:flex-end;height:100%" title="${t('Blocked by Wi-Fi coexistence — not measured')}">
                    <div style="height:100%;border-radius:3px 3px 0 0;opacity:.22;background:repeating-linear-gradient(45deg,var(--muted) 0 3px,transparent 3px 6px)"></div>
                    <div style="font-size:10px;padding:4px 0;color:var(--text)">${c.ch}</div>
                    <div style="font-size:9px;color:var(--muted)">n/a</div>
                </div>`;
            }
            const pct = Math.max(8, ((c.energy - min) / (max - min)) * 100);
            const color = c.energy > -60 ? '#f44' : c.energy > -75 ? '#fa0' : '#4c4';
            return `<div style="flex:1;text-align:center;display:flex;flex-direction:column;justify-content:flex-end;height:100%">
                <div style="background:${color};height:${pct}%;border-radius:3px 3px 0 0;min-height:4px;transition:height .3s"></div>
                <div style="font-size:10px;padding:4px 0;color:var(--text)">${c.ch}</div>
                <div style="font-size:9px;color:var(--muted)">${c.energy}</div>
            </div>`;
        }).join('');

        const note = document.getElementById('spec-note');
        if (note) {
            const missing = channels.length - (validCount != null ? validCount : channels.length);
            note.textContent = missing > 0
                ? t('{missing} of {total} channels could not be measured (Wi-Fi coexistence blocked the energy detect).',
                    { missing, total: channels.length })
                : '';
        }
    },

    _renderNeighbors(res) {
        const el = document.getElementById('spec-neighbors');
        const list = res.networks;
        /* A scan cut short by the firmware's time budget must say so: an
           empty ch26 row and an unscanned ch26 look identical otherwise. */
        const partial = res.completed === false
            ? `<tr><td colspan="4" class="text-muted">${t('Scan ran out of time — some channels may be missing.')}</td></tr>` : '';
        if (!list.length) { el.innerHTML = `<tr><td class="text-muted">${t('No neighbor networks found')}</td></tr>${partial}`; return; }
        el.innerHTML = `<tr><th>${t('PAN ID')}</th><th>${t('Channel')}</th><th>RSSI</th><th>LQI</th></tr>` +
            list.map(n => `<tr>
                <td style="font-family:monospace">0x${n.pan_id.toString(16).toUpperCase().padStart(4,'0')}${
                    n.self ? ` <span class="text-muted" style="font-family:inherit">(${t('this gateway')})</span>` : ''}</td>
                <td>CH ${n.channel}</td>
                <td>${n.rssi} dBm</td>
                <td>${n.lqi}</td>
            </tr>`).join('') + partial;
    }
};

App.registerPage('spectrum', SpectrumPage);
