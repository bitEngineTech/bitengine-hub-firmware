/* Derive a POSIX TZ string from the browser.
 *
 * Why this lives in the frontend: the browser is the **authoritative** source
 * for the timezone (the user's system setting plus a tzdata that updates with
 * the system), and it is present at exactly the moment the configuration is
 * made. The firmware understands only POSIX TZ strings, which keeps it simple;
 * the conversion happens once at configuration time and costs nothing at
 * runtime. It also spares us shipping a tzdata table in the firmware, and
 * spares us an OTA every time a DST rule changes — when the browser updates, we
 * follow automatically.
 *
 * ⚠️ The sign of a POSIX TZ offset is the opposite of intuition:
 * `PST8PDT` = UTC-8, `CST-8` = UTC+8. The number in the specification is "the
 * hours you must add to local time to obtain UTC", i.e. **positive west of
 * Greenwich**. JavaScript's getTimezoneOffset() happens to use that same sign
 * convention (positive west), so the two map across directly — this is the one
 * place where no negation is needed. Make sure that sentence is understood
 * before editing any of this.
 */
const TZ = {
    /* UTC offset at a given instant (minutes, positive west, same sign as POSIX) */
    _offAt(d) { return new Date(d).getTimezoneOffset(); },

    /* Minutes → the POSIX [+-]h[:mm[:ss]] form. Positive is west, and the plus
       sign is omitted. */
    _fmtOff(min) {
        const sign = min < 0 ? '-' : '';
        const a = Math.abs(min);
        const h = Math.floor(a / 60), m = a % 60;
        return sign + h + (m ? ':' + String(m).padStart(2, '0') : '');
    },

    /* Binary-search the millisecond within [lo,hi] at which the offset jumps;
       returns the transition instant (epoch ms). */
    _findTransition(lo, hi) {
        const o0 = this._offAt(lo);
        while (hi - lo > 1000) {
            const mid = lo + Math.floor((hi - lo) / 2);
            if (this._offAt(mid) === o0) lo = mid; else hi = mid;
        }
        return hi;
    },

    /* Transition instant → the POSIX M<month>.<week>.<weekday>/<hour> form.
       Week: 1-4 means the nth such weekday of the month, 5 means "the last one"
       (the POSIX convention).
     *
     * ⚠️ The hour must be computed on the wall clock of the timezone **before**
     * the transition. `t` is the first instant after it, so a plain getHours()
     * returns the post-jump reading: in the US, spring 2:00 PST jumping to 3:00
     * PDT reads as 3, and autumn 2:00 PDT falling back to 1:00 PST reads as 1,
     * while the specification calls both of them 2. So convert the UTC instant
     * back to wall-clock time using the pre-transition offset, then read the
     * fields off that. */
    _rule(t) {
        const oldOff = this._offAt(t - 1000);
        const w = new Date(t - oldOff * 60000);   // wall clock in the pre-change zone
        const month = w.getUTCMonth() + 1;
        const dow   = w.getUTCDay();
        const dom   = w.getUTCDate();
        let week = Math.floor((dom - 1) / 7) + 1;
        // One more week would cross into the next month → this is "the last
        // one", which POSIX writes as 5 and which survives year changes better.
        const last = new Date(Date.UTC(w.getUTCFullYear(), w.getUTCMonth() + 1, 0)).getUTCDate();
        if (dom + 7 > last) week = 5;
        const h = w.getUTCHours();
        return `M${month}.${week}.${dow}` + (h === 2 ? '' : `/${h}`);   // POSIX defaults to 2:00
    },

    /* Timezone abbreviation (PST/PDT/CST…), purely decorative: POSIX computes
       time from the offset alone and the name is only used for %Z.
       For many zones Intl only offers a form like "GMT+8", which is not a legal
       POSIX name. In that case fall back to the LST/LDT placeholders — not
       "UTC", because `UTC-8` reads as "UTC minus 8" while actually meaning
       UTC+8 (the POSIX sign is inverted), which is the easiest thing here to
       misread. */
    _abbr(t, isDst) {
        try {
            const s = new Intl.DateTimeFormat('en-US', { timeZoneName: 'short' })
                .formatToParts(new Date(t)).find(p => p.type === 'timeZoneName');
            if (s && /^[A-Za-z]{3,}$/.test(s.value)) return s.value;
        } catch (e) { /* old browser */ }
        return isDst ? 'LDT' : 'LST';
    },

    /* Main entry point: returns {tz, iana, dst}, where tz can be POSTed to
       /api/v1/ntp as-is. */
    detect() {
        let iana = '';
        try { iana = Intl.DateTimeFormat().resolvedOptions().timeZone || ''; } catch (e) {}

        const y = new Date().getFullYear();
        // Sample the offset month by month and take the minimum (DST) and the
        // maximum (standard time). Positive is west → standard time is larger.
        const offs = [];
        for (let m = 0; m < 12; m++) offs.push(this._offAt(new Date(y, m, 15)));
        const stdOff = Math.max(...offs);
        const dstOff = Math.min(...offs);

        const stdName = this._abbr(new Date(y, offs.indexOf(stdOff), 15).getTime(), false);

        if (stdOff === dstOff) {
            // No DST: `<name><offset>`
            return { tz: stdName + this._fmtOff(stdOff), iana, dst: false };
        }

        // Find the two transitions. Scan month by month to bracket them, then
        // binary-search down to the second.
        const edges = [];
        for (let m = 0; m < 12; m++) {
            const a = new Date(y, m, 1).getTime();
            const b = new Date(y, m + 1, 1).getTime();
            if (this._offAt(a) !== this._offAt(b - 1000)) {
                edges.push(this._findTransition(a, b));
            }
        }
        if (edges.length !== 2) {
            // Rules too unusual (more than two transitions in a year, etc.) —
            // do not guess; fall back to a fixed offset with no DST.
            return { tz: stdName + this._fmtOff(stdOff), iana, dst: false };
        }

        // Whether the earlier transition is "entering DST" or "leaving it"
        // depends on whether the offset shrinks or grows there. In the southern
        // hemisphere DST ends before it begins, and this swaps the two.
        let toDst = edges[0], toStd = edges[1];
        if (this._offAt(edges[0] + 1000) > this._offAt(edges[0] - 1000)) {
            toDst = edges[1]; toStd = edges[0];
        }
        const dstName = this._abbr(toDst, true);

        return {
            tz: stdName + this._fmtOff(stdOff) + dstName +
                /* When the DST offset is omitted, POSIX defaults it to
                   "standard time minus one hour", which is exactly the case
                   almost everywhere — omitting it is shorter and harder to get
                   wrong. */
                (dstOff === stdOff - 60 ? '' : this._fmtOff(dstOff)) +
                ',' + this._rule(toDst) + ',' + this._rule(toStd),
            iana, dst: true,
        };
    },
};
