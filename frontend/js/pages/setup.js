/* Forced set-password page on first boot.
 *
 * docs/00:145 lists "password protection" as P0, and docs/01:578 spells it out
 * as "no authentication on first boot while no password is set; the WebUI
 * forces one to be set". That requirement went unimplemented for a long time —
 * a factory device has an empty `password_hash` (config_defaults.c) and
 * check_auth() returns true whenever there is no password (auth.c), leaving the
 * entire management interface open to the local network with nothing telling
 * the user about it.
 *
 * This gate appears only when the device is **known** to have no password (the
 * test is in App._enter), and it stops browsers only. The REST API is
 * unaffected: the gate lives in the frontend, so curl automation such as
 * tools/factory_test.sh keeps working — a deliberate trade-off, because a
 * production line should not be blocked by an interactive prompt.
 * In other words it does not defend against an attacker; it defends against
 * "the user never got around to setting a password".
 *
 * After setting one it **logs in automatically** with the new password before
 * entering the main UI: config_mgr_set_password() creates no session, and
 * without that step the very next request 401s and the user is bounced to the
 * login page the instant they finish.
 */
const SetupPage = {
    _busy: false,

    render(el) {
        el.innerHTML = `
        <div class="login-wrap">
            <div class="login-card">
                <div class="login-brand">
                    <span class="logo">&#9670;</span>
                    <span class="title">bitEngine Hub</span>
                </div>

                <div class="setup-lead">
                    ${t('This gateway has <b>no password</b>. Anyone on your network can read its Thread network key and change its settings. Set a password to continue.')}
                </div>

                <div class="form-group">
                    <label for="setup-pw1">${t('New Password')}</label>
                    ${PwField.html('setup-pw1', t('At least 4 characters'))}
                </div>
                <div class="form-group">
                    <label for="setup-pw2">${t('Confirm Password')}</label>
                    ${PwField.html('setup-pw2', t('Repeat password'))}
                </div>

                <button class="btn btn-primary" id="setup-btn" style="width:100%"
                        onclick="SetupPage._submit()">${t('Set Password')}</button>
                <span class="login-msg" id="setup-msg"></span>

                <div class="hint" style="margin-top:14px">
                    ${t('Write it down. There is no password reset &mdash; recovering a forgotten password means a factory reset (hold the button for 10 seconds), which erases WiFi and all other settings.')}
                </div>
                <!-- Same reasoning as the login gate: this screen replaces all
                     of #app, so it needs its own language picker. -->
                <div class="login-lang">${Lang.selectHtml('setup-lang-select')}</div>
            </div>
        </div>`;

        const p1 = document.getElementById('setup-pw1');
        const p2 = document.getElementById('setup-pw2');
        /* Enter submits from either field: the second one is the last box, and
           pressing Enter in the first is a common habit too. Wiring up both
           saves the user from thinking nothing happened. */
        [p1, p2].forEach(i => i.addEventListener('keydown', e => {
            if (e.key === 'Enter') SetupPage._submit();
        }));
        p1.focus();
    },

    async _submit() {
        if (this._busy) return;

        const i1  = document.getElementById('setup-pw1');
        const i2  = document.getElementById('setup-pw2');
        const btn = document.getElementById('setup-btn');
        const msg = document.getElementById('setup-msg');
        if (!i1 || !i2) return;

        const pw = i1.value;
        /* The order matters: empty first, then matching, then length. Being
           told "the two do not match" and only afterwards discovering "it was
           too long anyway" is genuinely infuriating. */
        if (pw.length < 4) {
            this._say(msg, t('At least 4 characters'), 'var(--yellow)'); i1.focus(); return;
        }
        if (pw !== i2.value) {
            this._say(msg, t('The two passwords do not match'), 'var(--red)');
            PwField.reset('setup-pw2'); i2.focus(); return;
        }
        /* Shares the byte limit with the login page — not sharing it is how you
           end up setting a password you cannot log in with. */
        if (PwField.passwordTooLong(pw)) {
            this._say(msg, t('Password too long for this device'), 'var(--red)');
            i1.focus(); return;
        }

        this._busy = true;
        if (btn) btn.disabled = true;
        this._say(msg, t('Saving...'), 'var(--muted)');

        const r = await API.setPassword(pw);
        if (!r || r.status !== 'ok') {
            this._busy = false;
            if (btn) btn.disabled = false;
            const code = r && r.error;
            this._say(msg, code === 'password_too_short' ? t('At least 4 characters')
                                                         : t('Could not save the password'),
                      'var(--red)');
            return;
        }

        /* The password is live, but this browser has no session yet — trade it
           for one immediately, or every request after authStatus() in _enter()
           comes back 401. */
        this._say(msg, t('Signing in...'), 'var(--muted)');
        const login = await API.login(pw);

        PwField.reset('setup-pw1', 'setup-pw2');
        this._busy = false;
        if (btn) btn.disabled = false;

        if (login && login.ok) {
            await App._enter();
        } else {
            /* Auto-login failed, but the password **is** set. Fall back to the
               login page so the user types it once by hand; never stay on the
               setup page, or they will assume it did not work and set it again. */
            App.showLogin();
        }
    },

    _say(el, text, color) {
        if (!el) return;
        el.textContent = text;              // not innerHTML
        el.style.color = color;
    },
};

/* Not registerPage — see the header of this file and the loginPage comment in
   app.js */
App.setupPage = SetupPage;
