/* MQTT / Webhook configuration page.
 *
 * Added 2026-08-17. Both endpoint groups had been fully implemented on the REST
 * side with test_api.sh assertions, but the frontend had **no entry point** —
 * the only place the letters "MQTT" appeared anywhere was the factory-reset
 * warning. With both shipping enabled=false, two P1 selling points simply did
 * not exist for ordinary users. See docs/feature_gap_audit.md A1.
 *
 * Three semantics that must be preserved (all firmware-side contracts; read
 * docs/06 and rest_api.c before touching this file):
 *
 *  1. **Partial updates.** An absent field keeps its current value. So when the
 *     password box is empty we must **not** send password:"" — that means
 *     "clear the password", not "leave it alone". Empty box = omit the field.
 *     Clearing is therefore an explicit checkbox, never an empty field.
 *  2. **The MQTT password is never echoed back.** /mqtt/status only exposes a
 *     password_set boolean; the page shows a hint from it and never tries to
 *     put the password back into the input.
 *  3. **Over-long input is rejected by the firmware, not truncated.** A
 *     truncated broker/URL is still syntactically valid but points elsewhere,
 *     i.e. the request would be silently rerouted. So a 400 must be shown to
 *     the user as-is, never swallowed.
 */
const MqttWebhookPage = {
    _timer: null,

    render(el) {
        el.innerHTML = `
        <div class="page-title">${t('MQTT & Webhook')}</div>

        <div class="section-label">MQTT</div>
        <div class="card">
            <div style="display:flex;align-items:center;gap:12px;margin-bottom:16px">
                <label class="switch"><input type="checkbox" id="mq-en"><span class="slider"></span></label>
                <span>${t('Enable MQTT')}</span>
                <span id="mq-conn" style="margin-left:auto"></span>
            </div>

            <div class="form-row">
                <div class="form-group" style="flex:2"><label>${t('Broker')}</label>
                    <input id="mq-broker" class="input" placeholder="192.168.1.10 or mqtt.example.com"></div>
                <div class="form-group" style="flex:1"><label>${t('Port')}</label>
                    <input id="mq-port" class="input" type="number" min="1" max="65535" placeholder="1883"></div>
            </div>
            <div class="form-row">
                <div class="form-group" style="flex:1"><label>${t('Username')}</label>
                    <input id="mq-user" class="input" autocomplete="off"></div>
                <div class="form-group" style="flex:1"><label>${t('Password')}</label>
                    <input id="mq-pass" class="input" type="password" autocomplete="new-password"
                           placeholder="${t('(unchanged)')}"></div>
            </div>
            <div class="form-row">
                <div class="form-group" style="flex:2"><label>${t('Topic prefix')}</label>
                    <input id="mq-prefix" class="input" placeholder="bitengine"></div>
                <div class="form-group" style="flex:1"><label>${t('Publish interval (s)')}</label>
                    <input id="mq-int" class="input" type="number" min="10" max="65535" placeholder="60"></div>
            </div>
            <div style="display:flex;align-items:center;gap:12px;margin-bottom:16px">
                <label class="switch"><input type="checkbox" id="mq-tls"><span class="slider"></span></label>
                <span>${t('TLS (mqtts://)')}</span>
            </div>

            <div class="hint" id="mq-passhint" style="margin-bottom:6px"></div>
            <!-- Clearing is a deliberate act. An empty password box means "keep",
                 so the only way to remove a stored password is this checkbox. -->
            <label id="mq-clear-wrap" style="display:none;align-items:center;gap:8px;margin-bottom:12px;font-size:13px">
                <input type="checkbox" id="mq-clearpw"> ${t('Clear the stored password')}
            </label>

            <div style="display:flex;align-items:center;gap:12px">
                <button class="btn btn-primary" onclick="MqttWebhookPage._saveMqtt()">${t('Save MQTT')}</button>
                <span id="mq-msg"></span>
            </div>
            <div class="hint" style="margin-top:12px">
                ${t('When enabled, the gateway publishes 6 Home Assistant discovery entities: temperature, free memory, WiFi RSSI, uptime, Zigbee chip alive, firmware version. It cannot be enabled while Broker is empty.')}
            </div>
        </div>

        <div class="section-label">Webhook</div>
        <div class="card">
            <div style="display:flex;align-items:center;gap:12px;margin-bottom:16px">
                <label class="switch"><input type="checkbox" id="wh-en"><span class="slider"></span></label>
                <span>${t('Enable Webhook')}</span>
            </div>

            <div class="form-group"><label>URL</label>
                <input id="wh-url" class="input" placeholder="https://example.com/hook"></div>
            <div class="hint" style="margin-bottom:16px">
                ${t('Must start with <code>http://</code> or <code>https://</code> — leaving the scheme open would let external input decide which protocol the gateway reaches out with.')}
            </div>

            <div class="section-label" style="margin-top:0">${t('Events')}</div>
            <div id="wh-events" style="display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:10px;margin-bottom:16px"></div>

            <div class="form-group" style="max-width:280px"><label>${t('Auto backup interval (s)')}</label>
                <input id="wh-backup" class="input" type="number" min="0" max="2592000" placeholder="0 = off"></div>
            <div class="hint" style="margin-bottom:16px">
                ${t('0 = off; otherwise at least 300 s (anything lower amounts to DoS-ing your own webhook endpoint), up to 30 days.')}<br>
                ⚠️ ${t('<b>This currently sends a reminder, not a backup</b>: a single <code>{"hostname":"…","note":"use /api/v1/config/export for full backup"}</code> — the receiving end gets no recoverable data. For a real backup use Export on the Settings page. See feature_gap_audit A16.')}
            </div>

            <div style="display:flex;align-items:center;gap:12px">
                <button class="btn btn-primary" onclick="MqttWebhookPage._saveWebhook()">${t('Save Webhook')}</button>
                <span id="wh-msg"></span>
            </div>
        </div>`;

        this._renderEvents();
        this._load();
        /* The MQTT connection state changes on its own (broker down / back), so
           refresh it periodically while the page is open. Only the status badge
           is refreshed — the inputs are not refilled, or whatever the user is
           typing would be overwritten. */
        this._timer = setInterval(() => this._refreshConn(), 10000);
    },

    destroy() {
        /* Test against !== null rather than truthiness: 0 is a legal timer
           handle, and `if (this._timer)` would silently skip cleanup for it,
           leaving the poll running forever. Browsers happen to start at 1, so
           this never shows on a real device — until the implementation or the
           runtime changes. After leaving the page it would hit the API every
           10 s, and every 401 would bounce the user back to the login page. */
        if (this._timer !== null) { clearInterval(this._timer); this._timer = null; }
    },

    /* Keys are the firmware's field names; labels are looked up at render time
       so a language switch re-renders them. */
    _EVENTS: [
        ['event_zigbee_crash', 'Zigbee or gateway crash'],
        ['event_network_down', 'Network down'],
        ['event_interference', 'RF interference alert'],
        ['event_ota',          'Firmware update available / completed'],
        ['event_temp_high',    'Temperature too high'],
        ['event_memory_low',   'Low memory'],
    ],

    _renderEvents() {
        document.getElementById('wh-events').innerHTML = this._EVENTS.map(([k, label]) => `
            <label style="display:flex;align-items:center;gap:10px">
                <input type="checkbox" id="wh-${k}">
                <span>${t(label)}</span>
            </label>`).join('');
    },

    _passHint(passwordSet) {
        return passwordSet
            ? t('A password is stored. Leave the field blank to keep it; tick "Clear the stored password" to remove it.')
            : t('No password is set.');
    },

    _showClear(passwordSet) {
        const wrap = document.getElementById('mq-clear-wrap');
        const cb   = document.getElementById('mq-clearpw');
        if (cb) cb.checked = false;
        if (wrap) wrap.style.display = passwordSet ? 'flex' : 'none';
    },

    async _load() {
        const $ = id => document.getElementById(id);

        const m = await API.mqttStatus();
        if (m) {
            $('mq-en').checked   = !!m.enabled;
            $('mq-broker').value = m.broker || '';
            $('mq-port').value   = m.port || '';
            $('mq-user').value   = m.username || '';
            $('mq-prefix').value = m.topic_prefix || '';
            $('mq-int').value    = m.publish_interval || '';
            $('mq-tls').checked  = !!m.tls_enabled;
            /* Never echoed (the firmware only gives password_set). Spell out
               that "blank = keep", or users will read an empty box as "no
               password" and wipe it while changing the port. */
            $('mq-passhint').textContent = this._passHint(m.password_set);
            this._showClear(!!m.password_set);
            this._paintConn(m.connected, m.enabled);
        }

        const w = await API.webhookGet();
        if (w) {
            $('wh-en').checked  = !!w.enabled;
            $('wh-url').value   = w.url || '';
            $('wh-backup').value = w.backup_interval || 0;
            this._EVENTS.forEach(([k]) => { $('wh-' + k).checked = !!w[k]; });
        }
    },

    async _refreshConn() {
        const m = await API.mqttStatus();
        if (m) this._paintConn(m.connected, m.enabled);
    },

    _paintConn(connected, enabled) {
        const el = document.getElementById('mq-conn');
        if (!el) return;   // page already switched away
        el.innerHTML = !enabled
            ? `<span class="badge">${t('Disabled')}</span>`
            : (connected ? `<span class="badge badge-green">${t('Connected')}</span>`
                         : `<span class="badge badge-red">${t('Disconnected')}</span>`);
    },

    _msg(id, r, okText) {
        document.getElementById(id).innerHTML = (r && !r.error)
            ? `<span class="badge badge-green">${okText}</span>`
            : `<span class="badge badge-red">${(r && r.error) || t('Failed')}</span>`;
    },

    async _saveMqtt() {
        const $ = id => document.getElementById(id);
        const d = {
            enabled:      $('mq-en').checked,
            broker:       $('mq-broker').value.trim(),
            topic_prefix: $('mq-prefix').value.trim(),
            username:     $('mq-user').value,
            tls_enabled:  $('mq-tls').checked,
        };
        /* An empty number box has value "", and parseInt gives NaN. NaN turns
           into null in JSON.stringify, the firmware's json_opt_int rejects the
           type, and the whole request is a 400. Absent already means "keep", so
           simply do not send the field when the box is empty. */
        const port = parseInt($('mq-port').value, 10);
        if (!isNaN(port)) d.port = port;
        const iv = parseInt($('mq-int').value, 10);
        if (!isNaN(iv)) d.publish_interval = iv;

        /* Password: blank = do not send the field = keep. Never send "" by
           accident — that clears it. Clearing is the explicit checkbox. */
        const clear = $('mq-clearpw') && $('mq-clearpw').checked;
        const pw = $('mq-pass').value;
        if (clear)          d.password = '';
        else if (pw !== '') d.password = pw;

        const r = await API.mqttConfig(d);
        this._msg('mq-msg', r, t('Saved'));
        if (r && !r.error) {
            $('mq-pass').value = '';         // do not leave the plaintext in the DOM
            this._paintConn(r.connected, r.enabled);
            $('mq-passhint').textContent = this._passHint(r.password_set);
            this._showClear(!!r.password_set);
        }
    },

    async _saveWebhook() {
        const $ = id => document.getElementById(id);
        const d = {
            enabled: $('wh-en').checked,
            url:     $('wh-url').value.trim(),
        };
        const b = parseInt($('wh-backup').value, 10);
        if (!isNaN(b)) d.backup_interval = b;
        this._EVENTS.forEach(([k]) => { d[k] = $('wh-' + k).checked; });

        const r = await API.webhookSet(d);
        this._msg('wh-msg', r, t('Saved'));
    }
};

App.registerPage('mqtt', MqttWebhookPage);
