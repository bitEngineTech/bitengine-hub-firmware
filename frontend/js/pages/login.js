/* Login page.
 *
 * This file simply did not exist before 2026-08-15 — `App.showLogin()` looked
 * up `this.pages.login`, got undefined, and silently did nothing. The
 * consequence was not "no login protection" but **login protection bricking the
 * WebUI**: set a password → refresh → blank page, with no way back in short of
 * a factory reset. Which is why the password feature, complete as it was on the
 * backend, was never something anyone dared to switch on.
 *
 * Unlike the other pages, this one is not rendered through App.navigate(); it
 * is handed all of #app by App.showLogin() (there is no sidebar or topbar to
 * speak of while logged out). So it needs no destroy(), and it must **not** go
 * into App.pages — if it did, `#login` would become a navigable route, the
 * login card would be painted into #content inside the main UI, and after a
 * successful login the same hash would navigate straight back to it, with no
 * way out. It hangs off App.loginPage instead; see the end of this file.
 */
const LoginPage = {
    /* Backend error code → human wording. Never drop a server-supplied string
       into innerHTML (project rule S1: anything arriving over the network is
       untrusted). _say() uses textContent. */
    _MSG: {
        wrong_password:   'Wrong password',
        missing_password: 'Enter the password',
        empty_body:       'Enter the password',
        /* The login endpoint's body buffer has only 127 usable bytes; anything
           longer is truncated into broken JSON. Since 2026-08-15 both sides cap
           it at 63 bytes (PASSWORD_MAX_BYTES in webui.h), so this is normally
           unreachable — it is kept for devices whose over-long password was set
           before that cap existed. */
        invalid_json:     'Password too long for this device',
    },

    _busy: false,

    render(el) {
        el.innerHTML = `
        <div class="login-wrap">
            <div class="login-card">
                <div class="login-brand">
                    <span class="logo">&#9670;</span>
                    <span class="title">bitEngine Hub</span>
                </div>
                <div class="form-group">
                    <label for="login-pw">${t('Password')}</label>
                    ${PwField.html('login-pw', t('Device password'))}
                </div>
                <button class="btn btn-primary" id="login-btn" style="width:100%"
                        onclick="LoginPage._submit()">${t('Sign In')}</button>
                <span class="login-msg" id="login-msg"></span>
                <div class="hint" style="margin-top:14px">
                    ${t('Forgot the password? It can only be cleared by a factory reset (hold the button for 10 seconds).')}
                </div>
                <!-- The language picker has to be repeated on this gate: it
                     replaces all of #app, topbar included, and this is exactly
                     the screen where someone who cannot read the UI is stuck. -->
                <div class="login-lang">${Lang.selectHtml('login-lang-select')}</div>
            </div>
        </div>`;

        const input = document.getElementById('login-pw');
        /* Submit on Enter: pressing Enter in a password field is muscle memory,
           and without a <form> we have to wire it up ourselves. */
        input.addEventListener('keydown', e => {
            if (e.key === 'Enter') LoginPage._submit();
        });
        input.focus();
    },

    async _submit() {
        if (this._busy) return;                    // double click / Enter mashing

        const input = document.getElementById('login-pw');
        const btn   = document.getElementById('login-btn');
        const msg   = document.getElementById('login-msg');
        if (!input) return;

        const pw = input.value;
        if (!pw) { this._say(msg, t('Enter the password'), 'var(--yellow)'); input.focus(); return; }

        /* One more check, on the **byte** count. All three places that set or
           use a password share the same test — see PwField.passwordTooLong();
           an inconsistency here produces the "can set it, cannot log back in"
           situation. */
        if (PwField.passwordTooLong(pw)) {
            this._say(msg, t('Password too long for this device'), 'var(--red)');
            input.focus();
            return;
        }

        this._busy = true;
        if (btn) btn.disabled = true;
        this._say(msg, t('Signing in...'), 'var(--muted)');

        const r = await API.login(pw);

        this._busy = false;
        if (btn) btn.disabled = false;

        if (!r) {
            this._say(msg, t('Device unreachable'), 'var(--red)');
            return;
        }

        if (r.ok) {
            /* Clear immediately on success: if SHOW was pressed the password is
               still legible on screen, and _enter() is about to repaint all of
               #app — miss it here and there is no later chance. */
            PwField.reset('login-pw');
            this._say(msg, '', 'var(--muted)');
            await App._enter();
            return;
        }

        /* Look the key up first: t(undefined) would return undefined, which
           happens to fall through correctly but only by accident. */
        const known = this._MSG[r.body && r.body.error];
        this._say(msg, known ? t(known) : t('Login failed (HTTP {status})', { status: r.status }),
                  'var(--red)');
        PwField.reset('login-pw');
        input.focus();
    },

    _say(el, text, color) {
        if (!el) return;
        el.textContent = text;                     // not innerHTML
        el.style.color = color;
    },
};

/* Not registerPage — see the header of this file for why */
App.loginPage = LoginPage;
