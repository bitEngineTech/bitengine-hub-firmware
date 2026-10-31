/* Direct-profile changes to the app shell (docs/15_muse_direct_design_v1.md
   2.7, 10.2). Loaded right after js/app.js, only in the Direct bundle
   (frontend/direct/build.sh).

   The shared app.js is deliberately not edited: any edit there changes the HA
   www.bin, and the HA product must not change because of the Direct one
   (architecture 5.8). So _renderShell is wrapped instead:
     · no WiFi item — the Direct profile has no WiFi (design 2.2), the page is
       left out of the bundle and /api/v1/wifi/* is not registered;
     · a Muse item right after the dashboard, and a Devices item after it;
     · API.reboot goes to /api/v1/system/reboot (design 10.1). The shared one
       posts to /api2/reboot, the SMLIGHT compatibility layer, which the Direct
       profile does not build: the Settings and Firmware pages would say
       "Rebooting" while the hub stayed up. */
API.reboot = () => API.post('/api/v1/system/reboot', {});

(function () {
    const base = App._renderShell;
    App._renderShell = function () {
        base.call(this);
        const wifi = document.querySelector('.nav-item[data-page="wifi"]');
        if (wifi) wifi.remove();
        const dash = document.querySelector('.nav-item[data-page="dashboard"]');
        if (!dash) return;
        const a = document.createElement('a');
        a.className = 'nav-item';
        a.dataset.page = 'muse';
        a.href = '#muse';
        a.innerHTML = `<span class="nav-icon">&#9673;</span><span class="nav-label">${t('Muse')}</span>`;
        a.addEventListener('click', e => {
            e.preventDefault();
            document.getElementById('sidebar').classList.remove('open');
            App.navigate('muse');
        });
        dash.after(a);
        /* Devices (design 10.2, the device page), right after Muse */
        const dv = document.createElement('a');
        dv.className = 'nav-item';
        dv.dataset.page = 'devices';
        dv.href = '#devices';
        dv.innerHTML = `<span class="nav-icon">&#9783;</span><span class="nav-label">${t('Devices')}</span>`;
        dv.addEventListener('click', e => {
            e.preventDefault();
            document.getElementById('sidebar').classList.remove('open');
            App.navigate('devices');
        });
        a.after(dv);
    };
})();

/* Safe mode banner (docs/15 design appendix E33, decided 2026-10-23).
   After three self-healing restarts within an hour the hub stops in safe
   mode: only the network and this page run. The HA bundle shows nothing here
   because its users see safe mode in Home Assistant; the Direct profile has no
   HA, so it is said here.
   The second button is why this exists: when Muse is what keeps crashing the
   hub, Muse cannot be turned off from inside safe mode (its storage does not
   run there), and a plain exit starts it again and the hub crashes back into
   safe mode. "Exit and keep Muse off" leaves a one-shot note
   ({"muse_off":true}) that the next boot turns into Muse off.
   Device text (the reason) goes in with textContent, never innerHTML. */
const SafeMode = {
    async check() {
        const s = await API.get('/api/v1/status');
        if (!s || s.safe_mode !== true) return;
        const w = await API.get('/api/v1/diag/watchdog');
        this.show(w && typeof w.safe_reason === 'string' ? w.safe_reason : '');
    },

    show(reason) {
        if (document.getElementById('safe-banner')) return;
        const b = document.createElement('div');
        b.id = 'safe-banner';
        b.className = 'banner banner-red';
        b.innerHTML = `<div id="safe-banner-msg"></div>
            <div id="safe-banner-hint"></div>
            <button class="btn btn-sm" id="safe-exit">${t('Exit safe mode')}</button>
            <button class="btn btn-sm" id="safe-exit-muse">${t('Exit and keep Muse off')}</button>`;
        document.body.prepend(b);
        document.getElementById('safe-banner-msg').textContent =
            t('The hub restarted itself three times within an hour and stopped in safe mode. Only the network and this page are running; Zigbee, Thread and Muse are stopped.')
            + (reason ? ' ' + t('Reason: {reason}', { reason }) : '');
        document.getElementById('safe-banner-hint').textContent =
            t('If Muse is what keeps restarting the hub, choose "Exit and keep Muse off", then turn Muse back on from the Muse page once the cause is fixed.');
        document.getElementById('safe-exit').addEventListener('click', () => this.exit(false));
        document.getElementById('safe-exit-muse').addEventListener('click', () => this.exit(true));
    },

    async exit(museOff) {
        const buttons = ['safe-exit', 'safe-exit-muse'].map(id => document.getElementById(id)).filter(Boolean);
        buttons.forEach(x => { x.disabled = true; });
        let r = null;
        try { r = await API.post('/api/v1/diag/safemode/exit', museOff ? { muse_off: true } : {}); }
        catch (e) { r = { error: 'bad_reply' }; }          /* reply was not JSON */
        const msg = document.getElementById('safe-banner-msg');
        if (r === null) {
            /* No reply at all: the hub may already be restarting, or never got the request — say both */
            buttons.forEach(x => { x.disabled = false; });
            msg.textContent = t('No reply from the hub. If it is restarting, reload this page in a minute; otherwise try again.');
            return;
        }
        if (r && r.status === 'ok') {
            /* Trust the reply, not the button: only a reply that says muse_off was recorded promises Muse stays off */
            msg.textContent = r.muse_off === true
                ? t('Restarting. Muse will stay off until you turn it on again on the Muse page.')
                : t('Restarting...');
            return;
        }
        buttons.forEach(x => { x.disabled = false; });
        const code = r && typeof r.error === 'string' ? r.error : '';
        msg.textContent = code === 'muse_hold_failed'
            ? t('Could not record that Muse should stay off, so the hub did not restart. Try again.')
            : code === 'not_in_safe_mode'
                ? t('The hub is no longer in safe mode.')
                : t('Could not exit safe mode. Try again.');
    },
};

(function () {
    const base = App._renderShell;
    App._renderShell = function () {
        base.call(this);
        SafeMode.check();
    };
})();
