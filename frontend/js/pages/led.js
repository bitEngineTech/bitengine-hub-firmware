/* LED brightness / night mode configuration page.
 *
 * Added 2026-08-17. The backend and renderer chain had been complete all along
 * (led_ctrl really does read brightness, scheduler really does compute the
 * window), but /api/v1/led/nightmode is the only way to set night_mode_enabled
 * true — and it had **zero UI callers**. So the scheduler code that correctly
 * handles crossing midnight and zero-length windows was running for nothing.
 * See docs/feature_gap_audit.md A9.
 *
 * ⚠️ Why this page must also deal with the timezone (docs/12:664 calls this out
 *    explicitly): the timezone defaults to UTC+0 while the night-mode window is
 *    evaluated in **local time**. A user who never sets the timezone and asks
 *    for "lights off at 22:00" gets 22:00 UTC — 3pm in PDT. This is not
 *    theoretical: before 2026-08-13 changing NTP had no effect at all (the
 *    event had no consumer) and the whole feature had never worked end to end.
 *    So this page shows the device's local_time and tz_declared directly, and
 *    puts a banner up when the timezone was never declared, rather than letting
 *    anyone set a window against the wrong time base.
 *
 * ⚠️ Colors are deliberately not offered. The led_ctrl renderer uses fixed
 *    color semantics (green = normal / yellow = attention / red = fault), and
 *    the five color_* fields in the config have never been read by anything;
 *    wiring them up would only store settings that never take effect. The
 *    firmware reports colors_configurable:false and this page honours it —
 *    do not build a control for every field you see.
 */
const LedPage = {
    render(el) {
        el.innerHTML = `
        <div class="page-title">LED</div>

        <div id="led-tzwarn"></div>

        <div class="section-label">Brightness</div>
        <div class="card">
            <div style="display:flex;align-items:center;gap:16px">
                <input type="range" id="led-bright" min="0" max="255" value="128"
                       style="flex:1" oninput="document.getElementById('led-bright-val').textContent=this.value">
                <span id="led-bright-val" style="min-width:3em;text-align:right">128</span>
            </div>
            <div class="hint" style="margin-top:10px">
                ${t('0-255. 0 = fully off. Changes are written to NVS 2 seconds after they stop (so dragging the slider does not wear out the flash), which makes continuous dragging safe.')}
            </div>
        </div>

        <div class="section-label">Night Mode</div>
        <div class="card">
            <div style="display:flex;align-items:center;gap:12px;margin-bottom:16px">
                <label class="switch"><input type="checkbox" id="led-night-en"><span class="slider"></span></label>
                <span>${t('Enable night mode')}</span>
                <span id="led-night-active" style="margin-left:auto"></span>
            </div>

            <div class="form-row">
                <div class="form-group" style="flex:1"><label>${t('Start (local time)')}</label>
                    <input id="led-night-start" class="input" type="time" value="22:00"></div>
                <div class="form-group" style="flex:1"><label>${t('End (local time)')}</label>
                    <input id="led-night-end" class="input" type="time" value="07:00"></div>
            </div>

            <div class="hint" style="margin-bottom:16px">
                ${t('Crossing midnight is supported (22:00 → 07:00). Setting start and end to the <b>same</b> time means zero duration, i.e. the LEDs never dim.')}<br>
                ${t('Device local time:')} <b id="led-localtime">--</b>
                <span id="led-synced"></span>
            </div>

            <div style="display:flex;align-items:center;gap:12px">
                <button class="btn btn-primary" onclick="LedPage._save()">${t('Save')}</button>
                <span id="led-msg"></span>
            </div>
        </div>

        <div class="card" style="margin-top:16px">
            <div class="hint">
                ${t('Colors are not configurable: the four LEDs use fixed color semantics (green = normal / yellow = attention / red = fault).')}<br>
                ${t('Bit-timing adjustment and the per-LED lighting test live in the LED TEST / Bit Timing cards on the <a href="#settings">Settings</a> page.')}
            </div>
        </div>`;

        this._load();
    },

    destroy() {},

    async _load() {
        const $ = id => document.getElementById(id);

        const c = await API.ledConfigGet();
        if (c) {
            $('led-bright').value = c.brightness;
            $('led-bright-val').textContent = c.brightness;
            $('led-night-en').checked = !!c.night_mode_enabled;
            /* The firmware returns "HH:MM" strings (night_start/night_end),
               which is exactly the value format of <input type=time> — no
               assembly required. */
            if (c.night_start) $('led-night-start').value = c.night_start;
            if (c.night_end)   $('led-night-end').value   = c.night_end;
            $('led-night-active').innerHTML = c.night_active
                ? `<span class="badge badge-green">${t('Currently in the night window')}</span>`
                : `<span class="badge">${t('Not currently in the night window')}</span>`;
        }

        /* The timezone half — see the file header. When no timezone was ever
           declared, the banner goes at the very top of the page, because a
           window set against the wrong time base is wrong no matter what. */
        const n = await API.ntpGet();
        if (n) {
            $('led-localtime').textContent = n.local_time || '--';
            $('led-synced').innerHTML = n.synced
                ? ''
                : ` <span class="badge badge-red">${t('NTP not synchronized')}</span>` +
                  `<br>${t('While the clock is unsynchronized the scheduler does not run night mode — configure NTP under Settings first.')}`;
            if (!n.tz_declared) {
                $('led-tzwarn').innerHTML = `
                <div class="card" style="border-left:3px solid #e0a030;margin-bottom:16px">
                    <b>${t('The timezone is still the factory default (UTC+0)')}</b><br>
                    <span class="hint">
                    ${t('Night-mode windows are evaluated in <b>local time</b>. Without setting the timezone first, "lights off at 22:00" means 22:00 UTC — 3pm on the US west coast. Go to <a href="#settings">Settings → Time &amp; Timezone</a> and press Detect once; it derives the complete POSIX TZ string straight from your browser.')}
                    </span>
                </div>`;
            } else {
                $('led-tzwarn').innerHTML = '';
            }
        }
    },

    async _save() {
        const $ = id => document.getElementById(id);
        const [sh, sm] = $('led-night-start').value.split(':').map(Number);
        const [eh, em] = $('led-night-end').value.split(':').map(Number);

        if ([sh, sm, eh, em].some(v => isNaN(v))) {
            $('led-msg').innerHTML = `<span class="badge badge-red">${t('Invalid time format')}</span>`;
            return;
        }

        const r = await API.ledConfigSet({
            brightness:          parseInt($('led-bright').value, 10),
            night_mode_enabled:  $('led-night-en').checked,
            night_start_hour: sh, night_start_min: sm,
            night_end_hour:   eh, night_end_min:   em,
        });

        $('led-msg').innerHTML = (r && !r.error)
            ? `<span class="badge badge-green">${t('Saved')}</span>`
            : `<span class="badge badge-red">${(r && r.error) || t('Failed')}</span>`;

        /* The response carries the state **after** it took effect, so refresh
           from that rather than passing the values we just submitted off as the
           result. A trap this repo has fallen into before: the setter stored
           the value without it reaching the system, and the UI reported
           success anyway. */
        if (r && !r.error) {
            $('led-night-active').innerHTML = r.night_active
                ? `<span class="badge badge-green">${t('Currently in the night window')}</span>`
                : `<span class="badge">${t('Not currently in the night window')}</span>`;
        }
    }
};

App.registerPage('led', LedPage);
