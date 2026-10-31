const LogsPage = {
    _timer: null,
    _since: 0,          // highest seq fetched so far, for incremental pulls
    _paused: false,
    _level: '',
    _tagFilter: '',
    _rows: [],          // entries currently retained (for redraw on filter change)

    /* Maximum number of rows kept in the DOM. Without a cap, leaving the page
       open for a few hours accumulates tens of thousands of nodes and makes
       scrolling and repainting visibly stutter. Overflow drops the oldest. */
    MAX_ROWS: 800,

    render(el) {
        /* ⚠️ Clear the timer before creating a new one. render() can be
           called twice in a row with no destroy() in between (navigate() ends
           by writing location.hash, which fires onhashchange and navigates
           again); _timer only holds the last handle, so the earlier one leaks
           forever — visible as the same `since` being fetched concurrently
           several times. Clearing is idempotent and free. */
        this.destroy();

        el.innerHTML = `
        <div class="page-title">${t('System Logs')}</div>

        <div class="card" style="margin-bottom:12px">
            <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap">
                <div id="log-levels" style="display:flex;gap:4px"></div>
                <input class="input" id="log-tag" placeholder="${t('Filter by tag (empty = all)')}"
                       style="width:200px" oninput="LogsPage._onTagInput(this.value)">
                <button class="btn btn-sm" id="log-pause" onclick="LogsPage._togglePause()">${t('Pause')}</button>
                <button class="btn btn-sm" onclick="LogsPage._clear()">${t('Clear')}</button>
                <button class="btn btn-sm" onclick="LogsPage._copy()">${t('Copy')}</button>
                <span id="log-stat" class="hint"></span>
            </div>
            <div id="log-dropped" style="display:none;margin-top:8px;padding:8px 10px;
                 border-left:3px solid var(--yellow);background:rgba(255,167,38,.08);font-size:13px"></div>
        </div>

        <div class="card" style="padding:0">
            <div id="log-body" style="height:calc(100vh - 260px);min-height:280px;overflow-y:auto;
                 font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12px;line-height:1.55;
                 padding:10px 12px"></div>
        </div>`;

        /* NB: the loop variable is `label`, not `t` — `t` is the global
           translation function, and shadowing it here would break every t()
           call inside this scope. */
        const levels = [['', t('All')], ['E', 'Error'], ['W', 'Warn'], ['I', 'Info'], ['D', 'Debug']];
        document.getElementById('log-levels').innerHTML = levels.map(([v, label]) =>
            `<button class="btn btn-sm" data-lv="${v}" onclick="LogsPage._setLevel('${v}')">${label}</button>`).join('');
        this._syncLevelButtons();

        this._since = 0;
        this._rows = [];
        this._paused = false;
        this._poll(true);
        this._timer = setInterval(() => this._poll(false), 2000);
    },

    destroy() {
        /* Must be cleared, or the timer outlives the page and pointlessly
           hits the API every 2 seconds. */
        if (this._timer) { clearInterval(this._timer); this._timer = null; }
    },

    _syncLevelButtons() {
        document.querySelectorAll('#log-levels button').forEach(b => {
            const on = b.dataset.lv === this._level;
            b.style.background = on ? 'var(--accent)' : '';
            b.style.color      = on ? '#000' : '';
        });
    },

    _setLevel(lv) {
        this._level = lv;
        this._syncLevelButtons();
        /* Changing level requires a refetch: entries the server filtered out
           earlier were never held locally. */
        this._since = 0;
        this._rows = [];
        document.getElementById('log-body').innerHTML = '';
        this._poll(true);
    },

    _onTagInput(v) {
        this._tagFilter = v.trim().toLowerCase();
        this._redraw();     // tag filtering is local; no refetch needed
    },

    _togglePause() {
        this._paused = !this._paused;
        document.getElementById('log-pause').textContent = this._paused ? t('Resume') : t('Pause');
    },

    _clear() {
        this._rows = [];
        document.getElementById('log-body').innerHTML = '';
    },

    async _copy() {
        const txt = this._visible().map(r =>
            `${r.ms} ${r.level} ${r.tag}: ${r.msg}`).join('\n');
        try { await navigator.clipboard.writeText(txt); }
        catch { /* the clipboard API is unavailable outside https; ignore */ }
    },

    _visible() {
        if (!this._tagFilter) return this._rows;
        return this._rows.filter(r => r.tag.toLowerCase().includes(this._tagFilter));
    },

    async _poll(force) {
        if (this._paused && !force) return;

        let url = `/api/v1/logs?since=${this._since}&limit=100`;
        if (this._level) url += `&level=${this._level}`;
        const d = await API.getSoft(url);
        if (!d) return;

        const stat = document.getElementById('log-stat');
        if (!stat) return;                     // already navigated away

        if (d.__status === 503) {
            stat.innerHTML = `<span class="badge badge-yellow">${t('Log retention is disabled (PSRAM allocation failed)')}</span>`;
            return;
        }
        if (d.error) return;

        stat.textContent = t('{stored}/{capacity} in buffer', { stored: d.stored, capacity: d.capacity });

        /* `dropped` has to be surfaced: the ring buffer overflowed, which means
           what you are looking at is not everything. Discarding silently leads
           people to draw wrong conclusions from incomplete logs. */
        const dz = document.getElementById('log-dropped');
        if (d.dropped > 0) {
            dz.style.display = '';
            dz.innerHTML = t('The buffer has overwritten <b>{n}</b> lines — what is shown is <b>not the complete log</b>. When the device is chatty, check more often or filter by level.',
                              { n: d.dropped });
        } else {
            dz.style.display = 'none';
        }

        if (d.entries && d.entries.length) {
            this._since = d.next_seq - 1;
            this._rows.push(...d.entries);
            if (this._rows.length > this.MAX_ROWS) {
                this._rows.splice(0, this._rows.length - this.MAX_ROWS);
                this._redraw();
                return;
            }
            this._append(d.entries);
        }
    },

    _lvColor(l) {
        return l === 'E' ? 'var(--red)'
             : l === 'W' ? 'var(--yellow)'
             : l === 'D' ? 'var(--muted)'
             : 'var(--text)';
    },

    _row(r) {
        /* ⚠️ Everything goes through textContent; never innerHTML.
           Log bodies are arbitrary device-generated text and may well contain
           things like <img onerror=...> (the firmware's JSON escaping only
           guarantees valid JSON, it does not neutralize HTML).
           Using innerHTML here turns the log viewer into an XSS entry point. */
        const line = document.createElement('div');
        line.style.whiteSpace = 'pre-wrap';
        line.style.wordBreak = 'break-word';

        const meta = document.createElement('span');
        meta.style.color = 'var(--muted)';
        meta.textContent = `${String(r.ms).padStart(8)} `;

        const lv = document.createElement('span');
        lv.style.color = this._lvColor(r.level);
        lv.style.fontWeight = '600';
        lv.textContent = `${r.level} `;

        const tag = document.createElement('span');
        tag.style.color = 'var(--accent)';
        tag.textContent = r.tag ? `${r.tag}: ` : '';

        const msg = document.createElement('span');
        msg.style.color = this._lvColor(r.level);
        msg.textContent = r.msg;

        line.append(meta, lv, tag, msg);
        return line;
    },

    _append(entries) {
        const body = document.getElementById('log-body');
        if (!body) return;
        /* Auto-scroll only when already pinned to the bottom. Yanking a user
           back down while they are scrolled up reading is the single most
           irritating thing a log viewer can do. */
        const atBottom = body.scrollHeight - body.scrollTop - body.clientHeight < 40;
        const frag = document.createDocumentFragment();
        for (const r of entries) {
            if (this._tagFilter && !r.tag.toLowerCase().includes(this._tagFilter)) continue;
            frag.appendChild(this._row(r));
        }
        body.appendChild(frag);
        while (body.childElementCount > this.MAX_ROWS) body.removeChild(body.firstChild);
        if (atBottom) body.scrollTop = body.scrollHeight;
    },

    _redraw() {
        const body = document.getElementById('log-body');
        if (!body) return;
        body.innerHTML = '';
        const frag = document.createDocumentFragment();
        for (const r of this._visible()) frag.appendChild(this._row(r));
        body.appendChild(frag);
        body.scrollTop = body.scrollHeight;
    },
};

App.registerPage('logs', LogsPage);
