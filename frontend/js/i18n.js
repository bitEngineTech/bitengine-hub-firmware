/* Minimal gettext-style i18n.

   The English source string IS the lookup key — `t('Save Settings')` — rather
   than an invented identifier like `settings.save`. Two reasons this fits here:

     · A missing entry falls back to the key itself, which is already correct
       English. A half-finished table therefore ships fine: untranslated items
       render in English instead of going blank or printing `missing.key`.
       That matters because we cannot verify every language ourselves — a
       translator can fill in what they are sure of and leave the rest.
     · There is no build tooling in this project (build.sh is a plain `cat`
       concatenation), so there is nothing to extract keys or check coverage.
       Keys that are the text itself need neither.

   The cost: editing an English string silently orphans its translations, which
   then fall back to English. That is a visible-but-harmless degradation, and
   the alternative (stale translations still showing) is worse.

   Translation values are developer-authored constants, never user input, so
   they are interpolated into HTML templates without escaping — same as any
   other literal in those templates. Do not route device data through t(). */

const I18N = Object.create(null);

/* Language tables register themselves (js/i18n/<code>.js). Kept separate from
   this file so that adding a language is adding one file, and so the
   English-only lint can whitelist exactly those files and nothing else. */
function registerLang(code, table) {
    I18N[code] = table;
}

const Lang = {
    /* Names are deliberately written in their own language. When auto-detection
       guesses wrong, the user is looking at a UI they cannot read — and
       "Language" / "Sprache" / "语言" is precisely the word they cannot search
       for. A row that says "Deutsch" is recognizable regardless of what the
       rest of the page says. */
    _names: [
        { code: 'en', name: 'English' },
        { code: 'de', name: 'Deutsch' },
        { code: 'zh', name: '中文' },
    ],

    /* Only languages with at least one translated string are offered. A
       registered-but-empty table (a translation not started yet) would put a
       "中文" entry in the picker that changes nothing when chosen — worse than
       not offering it, because the user concludes the control is broken. */
    get available() {
        return this._names.filter(l => l.code === 'en' || (I18N[l.code] && Object.keys(I18N[l.code]).length > 0));
    },

    _cur: 'en',

    current() { return this._cur; },

    /* localStorage override wins over browser detection, permanently. Once a
       user has picked a language by hand we must never second-guess it: the
       phone that opens this page during commissioning is often not the phone
       of the person who will use the gateway.

       Every host lookup here is guarded with typeof. These files are also
       eval'd in plain node by the host-side tests (frontend/test_*.sh), where
       navigator, localStorage and document may not exist at all — an unguarded
       reference is a ReferenceError that takes the whole harness down, not a
       missing feature. */
    init() {
        let saved = null;
        try { saved = localStorage.getItem('lang'); } catch (e) { /* private mode, or no host */ }
        this._cur = (saved && this._known(saved)) ? saved : this._detect();
        this._stamp();
    },

    _stamp() {
        if (typeof document !== 'undefined' && document.documentElement) {
            document.documentElement.setAttribute('lang', this._cur);
        }
    },

    _known(code) {
        return code === 'en' || !!(I18N[code] && Object.keys(I18N[code]).length > 0);
    },

    /* navigator.languages (plural) is the ordered preference list; navigator
       .language is only its first entry. Match on the base subtag so that
       de-AT/de-CH land on de and zh-CN/zh-TW on zh. Anything unmatched falls
       through to English rather than to a partially translated table. */
    _detect() {
        if (typeof navigator === 'undefined') return 'en';
        const prefs = navigator.languages && navigator.languages.length
            ? navigator.languages
            : [navigator.language || 'en'];
        for (const p of prefs) {
            const base = String(p).toLowerCase().split('-')[0];
            if (this._known(base)) return base;
        }
        return 'en';
    },

    set(code) {
        if (!this._known(code) || code === this._cur) return;
        this._cur = code;
        try { localStorage.setItem('lang', code); } catch (e) { /* private mode, or no host */ }
        this._stamp();
        /* Re-render whatever is on screen. Every page builds its DOM inside
           render(), so re-running the current route is enough; there is no
           per-node text binding to update. */
        if (typeof App !== 'undefined') App.relocalize();
    },

    /* <select> for the topbar and for the login/setup gates. The gates need
       their own copy because they replace the whole #app subtree, topbar
       included — and those are exactly the screens where a user who cannot
       read the UI is stuck. */
    selectHtml(id = 'lang-select') {
        const opts = this.available.map(l =>
            `<option value="${l.code}"${l.code === this._cur ? ' selected' : ''}>${l.name}</option>`
        ).join('');
        return `<select id="${id}" class="lang-select" aria-label="Language"
                        onchange="Lang.set(this.value)">${opts}</select>`;
    },
};

/* Translate. `vars` substitutes {name} placeholders after lookup, so numbers
   and device values stay out of the translation tables. */
function t(s, vars) {
    const table = I18N[Lang._cur];
    let out = (table && table[s]) || s;
    if (vars) {
        for (const k in vars) out = out.split('{' + k + '}').join(vars[k]);
    }
    return out;
}
