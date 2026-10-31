const WS = {
    socket: null,
    listeners: {},
    reconnectMs: 3000,
    _timer: null,
    _backoff: 0,
    _probing: false,

    connect() {
        if (this.socket && this.socket.readyState <= 1) return;
        const url = `ws://${location.host}/ws`;
        const sock = new WebSocket(url);
        this.socket = sock;

        /* Set on the first frame. That frame — not onopen — is the only proof
           the firmware accepted this connection: esp_http_server answers 101
           *before* it runs ws_post_handshake_cb (httpd_uri.c:346 → 357), so a
           handshake the auth check is about to reject still fires onopen in the
           browser and is then closed without a single frame. The status feed
           pushes every 2 s, so an accepted socket proves itself quickly. */
        let accepted = false;

        sock.onmessage = e => {
            if (!accepted) { accepted = true; this._backoff = 0; }
            try {
                const msg = JSON.parse(e.data);
                /* The topbar badge is app-wide, not a page concern: without this
                   hook it only ever updated while the Dashboard was open, and a
                   reload landing on any other page showed no status at all. */
                if (msg.type === 'status_update' && typeof App !== 'undefined' && App.updateTopbar) {
                    App.updateTopbar(msg.data);
                }
                const fn = this.listeners[msg.type];
                if (fn) fn(msg.data);
            } catch {}
        };
        sock.onclose = () => {
            if (this.socket !== sock) return;      // superseded by a newer connect()
            /* Exponential backoff, capped at a minute. It used to be reset in
               onopen, which — see `accepted` above — a rejected handshake fires
               too, so for a tab whose session had expired the backoff never
               grew: one attempt every 3 s for as long as the tab stayed open,
               two rejection lines in the device log per attempt, ~2400 lines
               an hour, the whole 600-slot log ring gone in a quarter of an
               hour (2026-09-13: 87% of the ring was this). */
            const wait = this._backoff || this.reconnectMs;
            this._backoff = Math.min(wait * 2, 60000);
            this._timer = setTimeout(() => this.connect(), wait);
            /* Closed without a frame is what a rejected handshake looks like
               from here. The everyday cause is a gateway reboot: sessions live
               in RAM (auth.c), and a tab left on the Dashboard never finds out
               on its own — /status is AUTH_REQUIRED_UNLESS_OPEN and never 401s
               with rest_auth off, and a closed socket looks the same as a
               network blip. So ask a protected endpoint. 401 → login page
               (showLogin() calls disconnect(), which cancels the timer armed
               above). Anything else — device still booting, proxy down,
               cross-origin Host — keeps backing off as before. */
            if (!accepted) this._checkSession();
        };
        sock.onerror = () => sock.close();
    },

    async _checkSession() {
        if (this._probing || typeof API === 'undefined' || typeof App === 'undefined') return;
        this._probing = true;
        try {
            const r = await API.probe(API.AUTH_PROBE);
            if (r.status === 401) App.showLogin();
        } catch (e) {
            /* A gate that fails to render is the login page's own problem
               (app.js leaves a trace); it must not kill the reconnect loop. */
        } finally {
            this._probing = false;
        }
    },

    disconnect() {
        clearTimeout(this._timer);
        this._timer = null;
        this._backoff = 0;
        if (this.socket) { this.socket.onclose = null; this.socket.close(); this.socket = null; }
    },

    on(type, cb) { this.listeners[type] = cb; },
    off(type) { delete this.listeners[type]; },
};
