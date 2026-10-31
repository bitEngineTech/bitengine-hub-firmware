const ThreadPage = {
    _timer: null,
    _state: 'unknown',

    render(el) {
        this._tlvShown = false;
        el.innerHTML = `
        <div class="page-title">${t('Thread Border Router')}</div>

        <div class="section-label">${t('Status')}</div>
        <div class="card-grid">
            <div class="card"><div class="card-label">${t('Role')}</div><div class="card-value" id="t-state">--</div></div>
            <div class="card"><div class="card-label">${t('Network Name')}</div><div class="card-value" id="t-netname">--</div></div>
            <div class="card"><div class="card-label">${t('Channel')}</div><div class="card-value" id="t-channel">--</div></div>
            <div class="card"><div class="card-label">${t('PAN ID')}</div><div class="card-value" id="t-panid">--</div></div>
            <div class="card"><div class="card-label">RLOC16</div><div class="card-value" id="t-rloc16">--</div></div>
            <div class="card"><div class="card-label">${t('Ext PAN ID')}</div><div class="card-value" id="t-extpanid" style="font-size:12px">--</div></div>
            <div class="card"><div class="card-label">${t('Routers')}</div><div class="card-value" id="t-routers">--</div></div>
            <div class="card"><div class="card-label">${t('Leader')}</div><div class="card-value" id="t-leader">--</div></div>
        </div>

        <div class="section-label">${t('Add to Home Assistant')}</div>
        <div class="card">
            <div style="font-size:13px;line-height:1.7">
                ${t("Home Assistant's <strong>OTBR integration has no auto-discovery</strong> (its manifest carries no zeroconf), so the address has to be entered by hand. Copy this line into it:")}
                <div style="margin:8px 0;padding:8px 10px;background:rgba(0,0,0,.25);
                     border-radius:4px;font-family:monospace;font-size:14px;
                     display:flex;align-items:center;gap:8px">
                    <span id="t-otbr-url" style="flex:1;word-break:break-all">--</span>
                    <button id="t-otbr-copy" class="btn-sm" style="flex:none">${t('Copy')}</button>
                </div>
                <div class="hint">
                    ${t('The <strong>Thread</strong> integration, by contrast, discovers this device on its own (via <code>_meshcop._udp</code>) — just confirm the card it offers; nothing to fill in. Install both: <code>otbr</code> reads the Thread network credentials out and hands them to <code>thread</code> to store, which is what commissioning a Matter device needs. Installing <code>otbr</code> pulls <code>thread</code> in automatically.')}
                    <br><br>
                    ⚠️ ${t('If "OTBR / read-only API authentication" is switched on in Settings, the line above becomes <code>http://otbr:&lt;device password&gt;@…</code>, and the password must be plain ASCII without <code>/ ? #</code>.')}
                    <strong>${t('No OTBR entry can have its URL edited afterwards — the integration has no options or reconfigure flow at all. Changing the address or the token means deleting the entry and adding it again.')}</strong>
                </div>
            </div>
        </div>

        <div class="section-label">${t('Node Info')}</div>
        <div class="card">
            <table class="table" id="t-info"><tr><td class="text-muted">${t('Loading...')}</td></tr></table>
        </div>

        <div class="section-label">${t('Network Topology')}</div>
        <div class="card">
            <div id="t-topo" style="display:flex;align-items:center;justify-content:center;min-height:100px;flex-wrap:wrap;gap:12px"></div>
        </div>

        <div class="section-label">${t('Child Devices')}</div>
        <div class="card">
            <table class="table" id="t-children"><tr><td class="text-muted">${t('Loading...')}</td></tr></table>
        </div>

        <div class="section-label">${t('Neighbors')}</div>
        <div class="card">
            <table class="table" id="t-neighbors"><tr><td class="text-muted">${t('Loading...')}</td></tr></table>
        </div>

        <div class="section-label">${t('Border Routing')}</div>
        <div class="card">
            <table class="table" id="t-br"><tr><td class="text-muted">${t('Loading...')}</td></tr></table>
        </div>

        <div class="section-label">${t('IPv6 Addresses')}</div>
        <div class="card">
            <div id="t-ipaddrs" class="text-muted">${t('Loading...')}</div>
        </div>

        <div class="section-label">${t('Active Dataset (Network Credentials)')}</div>
        <div class="card">
            <table class="table" id="t-dataset"><tr><td class="text-muted">${t('Loading...')}</td></tr></table>
            <div style="margin-top:10px;display:flex;gap:8px;flex-wrap:wrap;align-items:center">
                <button class="btn btn-sm" id="t-reveal-btn" onclick="ThreadPage._toggleReveal()">${t('Show Secrets')}</button>
                <button class="btn btn-sm" onclick="ThreadPage._copyDataset()">${t('Copy JSON')}</button>
                <button class="btn btn-sm" onclick="ThreadPage._downloadBackup()">${t('Download Backup')}</button>
                <button class="btn btn-sm" id="t-tlv-btn" onclick="ThreadPage._toggleTlvs()">${t('Show Backup String')}</button>
                <span id="t-backup-msg"></span>
            </div>
            <textarea id="t-tlvs" class="input" readonly rows="3" style="display:none;margin-top:8px;width:100%;font-family:monospace;font-size:12px;word-break:break-all"></textarea>
            <div class="text-muted" style="margin-top:8px;font-size:12px">
                ${t('The backup is the dataset as hex TLVs, the form Home Assistant, ot-ctl and other border routers accept, so this exact network can be brought back. It contains the network key: keep it like a password. "Copy JSON" is for reading only and cannot be restored.')}
            </div>
        </div>

        <div class="section-label">${t('802.15.4 MAC Counters')}</div>
        <div class="card">
            <div class="card-grid" style="grid-template-columns:1fr 1fr" id="t-counters">
                <div class="text-muted">${t('Loading...')}</div>
            </div>
        </div>

        <div class="section-label">${t('Diagnostics')}</div>
        <div class="card">
            <table class="table" id="t-diag"><tr><td class="text-muted">${t('Loading...')}</td></tr></table>
        </div>

        <div class="section-label">${t('Actions')}</div>
        <div class="card">
            <div style="display:flex;gap:8px;flex-wrap:wrap">
                <button class="btn btn-sm" id="t-toggle-btn" onclick="ThreadPage._toggleState()">${t('Disable Thread')}</button>
                <button class="btn btn-danger" onclick="ThreadPage._resetNetwork()">${t('Reset Network')}</button>
            </div>
            <div id="t-action-msg" style="margin-top:8px"></div>
        </div>`;

        /* Same as logs.js: render() can be called twice in a row with no
           destroy() in between, and without clearing first the previous
           interval leaks forever (see the comment in logs.js). */
        this.destroy();
        this._secretsVisible = false;
        this._datasetJson = null;
        this._state = 'unknown';
        /* The OTBR address to paste into Home Assistant. It defaults to the
           browser's current host — whichever address the user came in on, so
           that a dual-uplink device does not hand out the wrong one.
           ⚠️ But it must not be handed out **unconditionally**: all 25 OTBR
           routes sit behind the cross-origin guard, and the user may well have
           arrived via a custom name handed out by their router (those names
           read fine but cannot write). Pasting that into HA makes
           python-otbr-api receive a 421 → OTBRError → ConfigEntryNotReady →
           the integration never loads, with nothing telling the user the
           address was the problem.
           So probe one **guarded** route first and fall back to the device IP
           if it fails (the 421 body carries a `use` field, which the firmware
           takes from getsockname as this connection's local address). */
        (async () => {
            const u = document.getElementById('t-otbr-url');
            const b = document.getElementById('t-otbr-copy');
            let host = location.host;
            /* T9.37 (2026-10-07): prefer the service address (/status svc_ip).
               The OTBR entry in HA cannot be edited later, so it must be the one
               address the gateway keeps reachable across cable pulls, re-plugs
               and reboots — not whichever address this browser happened to use.
               Older firmware has no svc_ip: keep the probe below. */
            let svc = '';
            try {
                const st = await API.status();
                if (st && st.svc_ip && st.svc_ip !== '0.0.0.0') svc = st.svc_ip;
            } catch (e) { /* fall through to the probe */ }
            if (svc) host = svc;
            else try {
                const r = await fetch('/node/state');
                if (r.status === 421) {
                    const d = await r.json().catch(() => null);
                    if (d && d.use) host = d.use.replace(/^https?:\/\//, '').replace(/\/$/, '');
                }
            } catch (e) { /* if the probe fails, keep the current host */ }
            if (u) u.textContent = `http://${host}`;
            if (b) b.onclick = () => {
                navigator.clipboard?.writeText(u.textContent).then(
                    () => { b.textContent = t('Copied'); setTimeout(() => b.textContent = t('Copy'), 1500); },
                    () => { b.textContent = t('Copy failed'); setTimeout(() => b.textContent = t('Copy'), 1500); });
            };
        })();

        this._load();
        this._timer = setInterval(() => this._loadNode(), 10000);
    },

    destroy() {
        if (this._timer) { clearInterval(this._timer); this._timer = null; }
    },

    async _load() {
        await Promise.all([
            this._loadNode(), this._loadDataset(), this._loadDiag(),
            this._loadChildren(), this._loadNeighbors(), this._loadIpAddrs(),
            this._loadBorderRouting(), this._loadCounters()
        ]);
        this._renderTopo();
    },

    /* Role names are OpenThread's own state words (leader/router/child/…);
       they are capitalized for display but deliberately not translated — they
       match what HA's Thread panel and the OT CLI print. */
    _roleBadge(state) {
        const stateColors = { leader: 'green', router: 'blue', child: 'yellow', disabled: 'red', detached: 'red' };
        return `<span class="badge badge-${stateColors[state] || 'yellow'}">${state.charAt(0).toUpperCase() + state.slice(1)}</span>`;
    },

    async _loadNode() {
        const d = await API.threadNode();
        const $ = id => document.getElementById(id);
        if (!$('t-state')) return;                                  // navigated away
        if (!d) {
            $('t-state').innerHTML = `<span class="badge badge-red">${t('Unavailable')}</span>`;
            return;
        }

        const state = d.State || 'unknown';
        this._state = state;
        $('t-state').innerHTML = this._roleBadge(state);
        $('t-netname').textContent = d.NetworkName || '--';
        $('t-rloc16').textContent = d.Rloc16 !== undefined ? '0x' + d.Rloc16.toString(16).toUpperCase().padStart(4, '0') : '--';
        $('t-extpanid').textContent = d.ExtPanId || '--';
        $('t-routers').textContent = d.NumOfRouter !== undefined ? d.NumOfRouter : '--';

        if (d.LeaderData) {
            $('t-leader').innerHTML = `${t('Partition:')} ${d.LeaderData.PartitionId}<br><small>${t('Router ID:')} ${d.LeaderData.LeaderRouterId}</small>`;
        }

        // Node info table
        const info = [
            [t('State'), state],
            [t('Network Name'), d.NetworkName],
            [t('Extended PAN ID'), d.ExtPanId],
            ['RLOC16', '0x' + (d.Rloc16 || 0).toString(16).toUpperCase().padStart(4, '0')],
            ['RLOC (IPv6)', d.Rloc],
            [t('Extended Address'), d.ExtAddress],
            [t('Border Agent ID'), d.BorderAgentId],
        ];
        $('t-info').innerHTML = info.map(([k, v]) =>
            `<tr><td style="color:var(--muted);width:160px">${k}</td><td>${v || '--'}</td></tr>`).join('');

        /* The toggle button follows the **state**, never its own label: the
           label is translated, so testing it for the word "Disable" would break
           in every language but English. */
        const btn = $('t-toggle-btn');
        if (btn) btn.textContent = (state === 'disabled') ? t('Enable Thread') : t('Disable Thread');
    },

    async _loadDataset() {
        const d = await API.threadDataset();
        if (!d || !document.getElementById('t-dataset')) return;
        this._datasetJson = d;

        const $ = id => document.getElementById(id);
        // Channel from dataset is initial config, actual channel comes from _loadDiag
        $('t-panid').textContent = d.PanId !== undefined ? '0x' + d.PanId.toString(16).toUpperCase().padStart(4, '0') : '--';

        const mask = v => this._secretsVisible ? v : (v ? v.slice(0, 4) + '...' : '--');
        const rows = [
            [t('Channel'), d.Channel],
            [t('Network Name'), d.NetworkName],
            [t('PAN ID'), '0x' + (d.PanId || 0).toString(16).toUpperCase().padStart(4, '0')],
            [t('Extended PAN ID'), d.ExtPanId],
            [t('Mesh Local Prefix'), d.MeshLocalPrefix],
            [t('Network Key'), mask(d.NetworkKey)],
            ['PSKc', mask(d.PSKc)],
            [t('Security Policy'), d.SecurityPolicy ? t('Rotation: {n}h', { n: d.SecurityPolicy.RotationTime }) : '--'],
        ];
        $('t-dataset').innerHTML = rows.map(([k, v]) =>
            `<tr><td style="color:var(--muted);width:160px">${k}</td><td style="font-family:monospace;font-size:12px">${v || '--'}</td></tr>`).join('');
    },

    async _loadDiag() {
        const d = await API.threadDiagnostics();
        const tbl = document.getElementById('t-diag');
        if (!tbl) return;
        if (!d || !d.length) {
            tbl.innerHTML = `<tr><td>${t('No diagnostic data')}</td></tr>`;
            return;
        }
        // Diagnostics has the actual running channel
        const ch = d[0].Channel;
        if (ch) document.getElementById('t-channel').textContent = 'CH ' + ch;
        tbl.innerHTML =
            `<tr><th>${t('Type')}</th><th>${t('State')}</th><th>RLOC16</th><th>${t('Network')}</th><th>${t('Channel')}</th></tr>` +
            d.map(r => `<tr>
                <td>${r.Type || '--'}</td>
                <td>${r.State || '--'}</td>
                <td>0x${(r.Rloc16 || 0).toString(16).toUpperCase().padStart(4, '0')}</td>
                <td>${r.NetworkName || '--'}</td>
                <td>${r.Channel || '--'}</td>
            </tr>`).join('');
    },

    _toggleReveal() {
        this._secretsVisible = !this._secretsVisible;
        document.getElementById('t-reveal-btn').textContent = this._secretsVisible ? t('Hide Secrets') : t('Show Secrets');
        this._loadDataset();
    },

    _copyDataset() {
        if (!this._datasetJson) return;
        const msg = document.getElementById('t-action-msg');
        /* navigator.clipboard only exists in secure contexts (https or
           localhost). On plain http://<ip> it is undefined and the old code
           threw a TypeError before reaching .catch — no message at all. */
        if (!navigator.clipboard) {
            msg.innerHTML = `<span class="badge badge-red">${t('Copy failed (the clipboard needs HTTPS or localhost)')}</span>`;
            return;
        }
        navigator.clipboard.writeText(JSON.stringify(this._datasetJson, null, 2))
            .then(() => {
                msg.innerHTML = `<span class="badge badge-green">${t('Dataset copied to clipboard')}</span>`;
                setTimeout(() => { msg.innerHTML = ''; }, 3000);
            })
            .catch(() => {
                msg.innerHTML = `<span class="badge badge-red">${t('Copy failed (the clipboard needs HTTPS or localhost)')}</span>`;
            });
    },

    async _toggleState() {
        const isDisable = this._state !== 'disabled';
        const action = isDisable ? 'disable' : 'enable';
        if (isDisable && !confirm(t('Disable Thread? All Thread devices will disconnect.'))) return;

        const msg = document.getElementById('t-action-msg');
        const ok = await API.threadSetState(action);
        msg.innerHTML = ok
            ? `<span class="badge badge-green">${isDisable ? t('Thread disabled') : t('Thread enabled')}</span>`
            : `<span class="badge badge-red">${t('Failed')}</span>`;
        setTimeout(() => this._loadNode(), 1000);
    },

    async _loadChildren() {
        const d = await API.threadChildren();
        const tbl = document.getElementById('t-children');
        if (!tbl) return;
        if (!d || !d.length) {
            tbl.innerHTML = `<tr><td class="text-muted">${t('No child devices')}</td></tr>`;
            return;
        }
        tbl.innerHTML = `<tr><th>RLOC16</th><th>${t('Type')}</th><th>RSSI</th><th>LQI</th><th>${t('Age')}</th><th>${t('Timeout')}</th><th>${t('Queued')}</th></tr>` +
            d.map(c => {
                const type = c.ftd ? 'FTD' : (c.rx_on_idle ? 'MED' : 'SED');
                const typeBadge = `<span class="badge badge-${c.ftd ? 'blue' : 'yellow'}">${type}</span>`;
                return `<tr>
                    <td style="font-family:monospace">${c.rloc16}</td>
                    <td>${typeBadge}</td>
                    <td>${c.rssi_avg} / ${c.rssi_last} dBm</td>
                    <td>${c.lqi}</td>
                    <td>${c.age}s</td>
                    <td>${c.timeout}s</td>
                    <td>${c.queued_msgs}</td>
                </tr>`;
            }).join('');
    },

    async _loadNeighbors() {
        const d = await API.threadNeighbors();
        const tbl = document.getElementById('t-neighbors');
        if (!tbl) return;
        if (!d || !d.length) {
            tbl.innerHTML = `<tr><td class="text-muted">${t('No neighbors')}</td></tr>`;
            return;
        }
        tbl.innerHTML = `<tr><th>RLOC16</th><th>${t('Type')}</th><th>RSSI</th><th>LQI</th><th>${t('Margin')}</th><th>${t('Age')}</th></tr>` +
            d.map(n => {
                const type = n.is_child ? t('Child') : (n.ftd ? t('Router') : 'MED');
                const color = n.is_child ? 'yellow' : 'blue';
                return `<tr>
                    <td style="font-family:monospace">${n.rloc16}</td>
                    <td><span class="badge badge-${color}">${type}</span></td>
                    <td>${n.rssi_avg} / ${n.rssi_last} dBm</td>
                    <td>${n.lqi}</td>
                    <td>${n.link_margin} dB</td>
                    <td>${n.age}s</td>
                </tr>`;
            }).join('');
    },

    async _loadIpAddrs() {
        const d = await API.threadIpAddrs();
        const el = document.getElementById('t-ipaddrs');
        if (!el) return;
        if (!d || !d.length) {
            el.innerHTML = `<span class="text-muted">${t('No addresses')}</span>`;
            return;
        }
        el.innerHTML = d.map(a => `<div style="font-family:monospace;font-size:12px;padding:2px 0">${a}</div>`).join('');
    },

    async _loadBorderRouting() {
        const d = await API.threadBorderRouting();
        const tbl = document.getElementById('t-br');
        if (!tbl) return;
        if (!d) { tbl.innerHTML = `<tr><td class="text-muted">${t('Unavailable')}</td></tr>`; return; }
        const stateColor = d.state === 'running' ? 'green' : d.state === 'stopped' ? 'yellow' : 'red';
        tbl.innerHTML = [
            [t('State'), `<span class="badge badge-${stateColor}">${d.state}</span>`],
            [t('OMR Prefix'), d.omr_prefix || '--'],
            [t('On-Link Prefix'), d.onlink_prefix || '--'],
        ].map(([k, v]) => `<tr><td style="color:var(--muted);width:140px">${k}</td><td style="font-family:monospace;font-size:12px">${v}</td></tr>`).join('');
    },

    async _loadCounters() {
        const d = await API.threadCounters();
        const el = document.getElementById('t-counters');
        if (!el) return;
        if (!d) { el.innerHTML = `<div class="text-muted">${t('Unavailable')}</div>`; return; }
        const ccaFail = d.tx_err_cca || 0;
        const txTotal = d.tx_total || 1;
        const ccaPct = (ccaFail / txTotal * 100).toFixed(1);
        el.innerHTML = `
            <div>
                <div class="card-label">TX</div>
                <div>${t('Total:')} <strong>${d.tx_total}</strong></div>
                <div>${t('Unicast:')} ${d.tx_unicast} / ${t('Broadcast:')} ${d.tx_broadcast}</div>
                <div>${t("ACK'd:")} ${d.tx_acked} / ${d.tx_ack_req}</div>
                <div>${t('CCA Fail:')} <strong style="color:${ccaFail > 100 ? 'var(--red)' : 'var(--text)'}">${ccaFail}</strong> (${ccaPct}%)</div>
                <div>${t('Abort:')} ${d.tx_err_abort}</div>
            </div>
            <div>
                <div class="card-label">RX</div>
                <div>${t('Total:')} <strong>${d.rx_total}</strong></div>
                <div>${t('Unicast:')} ${d.rx_unicast} / ${t('Broadcast:')} ${d.rx_broadcast}</div>
                <div>${t('FCS Err:')} ${d.rx_err_fcs}</div>
                <div>${t('Security Err:')} ${d.rx_err_sec}</div>
                <div>${t('Duplicated:')} ${d.rx_duplicated}</div>
            </div>`;
    },

    _renderTopo() {
        const el = document.getElementById('t-topo');
        if (!el) return;
        // Build topology: BR in center, children around it
        const nodeEl = (label, badge, color) =>
            `<div style="text-align:center;padding:8px 12px;background:var(--bg);border:2px solid var(--${color});border-radius:8px;min-width:80px">
                <div style="font-size:11px;color:var(--muted)">${badge}</div>
                <div style="font-size:13px;font-weight:600">${label}</div>
            </div>`;
        const lineEl = () => `<div style="width:40px;height:2px;background:var(--border)"></div>`;

        /* Label the centre node with the role we actually hold; it used to say
           "Leader" unconditionally, which was wrong on every network where
           another router is the leader (this one, for instance). */
        const role = this._state && this._state !== 'unknown'
            ? this._state.charAt(0).toUpperCase() + this._state.slice(1) : '--';
        let html = nodeEl(t('BR (Self)'), role, 'accent');

        // Get children from table
        const childRows = document.getElementById('t-children')?.querySelectorAll('tr:not(:first-child)') || [];
        if (childRows.length > 0) {
            html += lineEl();
            html += '<div style="display:flex;flex-wrap:wrap;gap:8px">';
            childRows.forEach(row => {
                const cells = row.querySelectorAll('td');
                if (cells.length >= 2) {
                    const rloc = cells[0].textContent;
                    const type = cells[1].textContent;
                    html += nodeEl(rloc, type, type.includes('FTD') ? 'blue' : 'yellow');
                }
            });
            html += '</div>';
        } else {
            html += `<div class="text-muted" style="margin-left:16px">${t('No children')}</div>`;
        }
        el.innerHTML = html;
    },

    async _downloadBackup() {
        const msg = document.getElementById('t-backup-msg');
        const tlvs = await API.threadDatasetTlvs();
        if (!tlvs) {
            msg.innerHTML = `<span class="badge badge-red">${t('Could not read the dataset')}</span>`;
            return;
        }
        /* Blob + download attribute works on plain http://<ip>, unlike the clipboard.
           The file holds the TLVs and nothing else, so `ot-ctl dataset set active $(cat f)`
           and HA take it as is. ExtPanId names the file: it tells two networks apart. */
        const ext = this._datasetJson && /^[0-9a-fA-F]{16}$/.test(this._datasetJson.ExtPanId || '')
            ? this._datasetJson.ExtPanId.toLowerCase() : 'thread';
        const a = document.createElement('a');
        a.href = URL.createObjectURL(new Blob([tlvs + '\n'], { type: 'text/plain' }));
        a.download = `thread-dataset-${ext}.txt`;
        document.body.appendChild(a);
        a.click();
        a.remove();
        setTimeout(() => URL.revokeObjectURL(a.href), 1000);
        msg.innerHTML = `<span class="badge badge-green">${t('Backup downloaded')}</span>`;
    },

    async _toggleTlvs() {
        const ta = document.getElementById('t-tlvs');
        const btn = document.getElementById('t-tlv-btn');
        if (!ta || !btn) return;
        /* Track visibility in a flag, not by reading style.display back: the inline
           display:none from the template is only reflected in style in real browsers. */
        if (this._tlvShown) {
            this._tlvShown = false;
            ta.style.display = 'none';
            ta.value = '';                       /* don't leave the key sitting in the DOM */
            btn.textContent = t('Show Backup String');
            return;
        }
        const tlvs = await API.threadDatasetTlvs();
        if (!tlvs) {
            document.getElementById('t-backup-msg').innerHTML =
                `<span class="badge badge-red">${t('Could not read the dataset')}</span>`;
            return;
        }
        ta.value = tlvs;
        ta.style.display = '';
        btn.textContent = t('Hide Backup String');
        this._tlvShown = true;
    },

    async _resetNetwork() {
        if (!confirm(t('Reset Thread network? This will disconnect ALL Thread devices and erase the dataset.'))) return;
        if (!confirm(t('This cannot be undone. Are you sure?'))) return;

        const msg = document.getElementById('t-action-msg');
        const ok = await API.threadDeleteDataset();
        msg.innerHTML = ok
            ? `<span class="badge badge-green">${t('Network reset. Rebooting...')}</span>`
            : `<span class="badge badge-red">${t('Failed')}</span>`;
    }
};

App.registerPage('thread', ThreadPage);
