const EthernetPage = {
    render(el) {
        el.innerHTML = `
        <div class="page-title">${t('Ethernet Settings')}</div>

        <div class="section-label">${t('Link Status')}</div>
        <div class="card-grid">
            <div class="card"><div class="card-label">${t('Link')}</div><div class="card-value" id="eth-link">--</div></div>
            <div class="card"><div class="card-label">${t('IP Address')}</div><div class="card-value" id="eth-ip-val">--</div></div>
            <div class="card"><div class="card-label">${t('Gateway')}</div><div class="card-value" id="eth-gw-val">--</div></div>
            <div class="card"><div class="card-label">${t('Diagnostics')}</div><div class="card-value" id="eth-diag" style="font-size:12px">--</div></div>
        </div>

        <div class="section-label">${t('IP Configuration')}</div>
        <div class="card">
            <div style="display:flex;align-items:center;gap:12px;margin-bottom:16px">
                <label class="switch"><input type="checkbox" id="eth-dhcp" checked onchange="EthernetPage._toggleDhcp(this.checked)"><span class="slider"></span></label>
                <span>${t('DHCP (automatic)')}</span>
            </div>
            <div id="eth-static" style="display:none">
                <div class="form-row">
                    <div class="form-group" style="flex:1"><label>${t('IP Address')}</label><input id="eth-ip" class="input" placeholder="192.168.1.100"></div>
                    <div class="form-group" style="flex:1"><label>${t('Gateway')}</label><input id="eth-gw" class="input" placeholder="192.168.1.1"></div>
                    <div class="form-group" style="flex:1"><label>${t('Netmask')}</label><input id="eth-mask" class="input" placeholder="255.255.255.0"></div>
                    <div class="form-group" style="flex:1"><label>${t('DNS')}</label><input id="eth-dns" class="input" placeholder="192.168.1.1"></div>
                </div>
            </div>
            <div style="display:flex;align-items:center;gap:12px">
                <button class="btn btn-primary" onclick="EthernetPage._save()">${t('Save')}</button>
                <span id="eth-msg"></span>
            </div>
            <div class="text-muted" style="margin-top:8px;font-size:12px">
                ${t("Takes effect on next reboot. Applying immediately would change this device's own IP and drop the connection serving this page.")}
            </div>
        </div>`;

        API.ethStatus().then(d => {
            if (!d) return;
            const $ = id => document.getElementById(id);
            if (!$('eth-link')) return;                       // navigated away
            $('eth-link').innerHTML = d.link_up
                ? `<span class="badge badge-green">${t('Up')}</span>`
                : `<span class="badge badge-red">${t('Down')}</span>`;
            $('eth-ip-val').textContent = d.has_ip ? d.ip : '--';
            $('eth-gw-val').textContent = d.has_ip ? d.gateway : '--';
            $('eth-diag').textContent = d.diag || '--';
        });

        /* Prefill the **configured** mode, not just the live state. Before this,
           the DHCP switch was always painted ON and the fields blank, so a user
           on a static address saw "DHCP (automatic)" — and pressing Save on that
           screen silently reverted them to DHCP at the next reboot. There is no
           dedicated GET for the ethernet config; /config/export carries it
           (addresses as raw uint32, see IP4.toString). */
        API.configExport().then(c => {
            const e = c && c.ethernet;
            const $ = id => document.getElementById(id);
            if (!e || !$('eth-dhcp')) return;
            $('eth-dhcp').checked = !!e.dhcp;
            this._toggleDhcp(!!e.dhcp);
            const fill = (id, n) => { if (n) $(id).value = IP4.toString(n); };
            fill('eth-ip',   e.static_ip);
            fill('eth-gw',   e.static_gateway);
            fill('eth-mask', e.static_netmask);
            fill('eth-dns',  e.static_dns);
        });
    },

    destroy() {},

    _toggleDhcp(on) { document.getElementById('eth-static').style.display = on ? 'none' : ''; },

    async _save() {
        const $ = id => document.getElementById(id);
        const dhcp = $('eth-dhcp').checked;
        const msg = $('eth-msg');
        const data = { dhcp };
        if (!dhcp) {
            data.static_ip      = $('eth-ip').value.trim();
            data.static_gateway = $('eth-gw').value.trim();
            data.static_netmask = $('eth-mask').value.trim();
            data.static_dns     = $('eth-dns').value.trim();
            if (!data.static_ip) {
                msg.innerHTML = `<span class="badge badge-red">${t('IP address is required for a static configuration')}</span>`;
                return;
            }
        }
        const r = await API.ethConfig(data);
        /* The firmware answers takes_effect:"reboot" — the only place the
           static config is applied is network_mgr_init(). Say so, or the user
           waits for an IP change that never comes. */
        const ERR = { static_ip_required: t('IP address is required for a static configuration'),
                      invalid_json: t('Invalid input') };
        msg.innerHTML = r && r.status === 'ok'
            ? `<span class="badge badge-green">${t('Saved — active after reboot')}</span>`
            : `<span class="badge badge-red">${(r && ERR[r.error]) || (r && r.error) || t('Failed')}</span>`;
    }
};

App.registerPage('ethernet', EthernetPage);
