#!/bin/bash
# frontend/test_i18n.sh —— 翻译表与源码键的一致性（不用板子，只要 node）
#
#   bash frontend/test_i18n.sh
#
# 为什么需要这个：t('...') 用英文原文当键，改一个英文字符串就会**静默**让它的
# 译文失效（回落到英文，不报错）；反过来翻译表里的一个笔误键永远不会被命中。
# 两种情况页面都"正常"，只有逐键比对才看得见。2026-09-05 首次填满 zh 表时
# 520 个键就是靠这个对齐的。
#
# 断言：
#   · 源码里每个 t() 键（含 MAP/ERR/_MSG 表里经 t() 输出的值）在 zh 表里都有译文
#   · zh 表里没有源码不存在的键（笔误 / 过期）
#   · 每条译文的 {占位符} 与键完全一致
#   · 每个注册的语言表都是合法 JS，且 t() 对未知键回落到键本身
#   · 页面源码（js/i18n/ 之外）不含 CJK 字面量 —— 英文是唯一源语言

set -u
cd "$(dirname "$0")"
node - <<'EOF'
const fs = require('fs'), path = require('path');
let PASS = 0, FAIL = 0;
const ok  = m => { console.log('✅ ' + m); PASS++; };
const bad = (m, d) => { console.log('❌ ' + m + (d ? '\n     ' + d : '')); FAIL++; };

// ---- load i18n.js + tables the way the browser does ----
const ctx = { console, navigator: undefined, document: undefined, localStorage: undefined };
const src = ['js/i18n.js', ...fs.readdirSync('js/i18n').map(f => 'js/i18n/' + f)]
    .map(f => fs.readFileSync(f, 'utf8')).join('\n');
let sandbox;
try {
    sandbox = new Function('ctx', `with (ctx) { ${src}; return { I18N, Lang, t }; }`)(ctx);
    ok('i18n.js and every language table parse and register');
} catch (e) { bad('i18n sources failed to evaluate', e.message); process.exit(1); }
const { I18N, Lang, t } = sandbox;

// ---- collect source keys ----
const files = ['js/app.js', 'js/api.js', 'js/tz.js', 'js/ws.js',
               ...fs.readdirSync('js/pages').map(f => 'js/pages/' + f)];
const re = /\bt\(\s*(['"])((?:\\.|(?!\1).)*)\1/g;
const keys = new Map();
for (const f of files) {
    const s = fs.readFileSync(f, 'utf8');
    let m;
    while ((m = re.exec(s))) {
        const k = m[1] === "'" ? m[2].replace(/\\'/g, "'") : m[2].replace(/\\"/g, '"');
        if (!keys.has(k)) keys.set(k, path.basename(f));
    }
    // Lookup tables whose *values* are passed through t() later
    for (const b of s.match(/(?:_MSG|ERR|MAP|M)\s*=\s*\{[\s\S]*?\n\s*\}/g) || []) {
        for (const v of b.match(/:\s*'((?:[^'\\]|\\.)*)'/g) || []) {
            const k = v.replace(/^:\s*'/, '').replace(/'$/, '').replace(/\\'/g, "'");
            if (!keys.has(k)) keys.set(k, path.basename(f) + ' (table)');
        }
    }
}
// mqtt_webhook.js keeps its event labels in an array and t()s them at render time
for (const k of ['Zigbee or gateway crash', 'Network down', 'RF interference alert',
                 'Firmware update available / completed', 'Temperature too high', 'Low memory'])
    if (!keys.has(k)) keys.set(k, 'mqtt_webhook.js (table)');
ok(`${keys.size} translation keys found in the sources`);

// ---- per-language checks (only tables that have entries are offered) ----
for (const code of Object.keys(I18N)) {
    const table = I18N[code];
    const n = Object.keys(table).length;
    if (n === 0) { ok(`${code}: table is empty and is therefore hidden from the picker`); continue; }
    const missing = [...keys.keys()].filter(k => !(k in table));
    const extra   = Object.keys(table).filter(k => !keys.has(k));
    missing.length === 0 ? ok(`${code}: every source key has a translation (${n})`)
                         : bad(`${code}: ${missing.length} source key(s) without translation`, missing.slice(0, 5).map(k => JSON.stringify(k)).join('\n     '));
    extra.length === 0   ? ok(`${code}: no stale or misspelled keys`)
                         : bad(`${code}: ${extra.length} key(s) not present in the sources (typo or stale)`, extra.slice(0, 5).map(k => JSON.stringify(k)).join('\n     '));
    const ph = k => (k.match(/\{[a-z_]+\}/g) || []).sort().join(',');
    const mism = Object.entries(table).filter(([k, v]) => ph(k) !== ph(v)).map(([k]) => k);
    mism.length === 0 ? ok(`${code}: placeholders match in every entry`)
                      : bad(`${code}: ${mism.length} entr(ies) with mismatched {placeholders}`, mism.slice(0, 5).map(k => JSON.stringify(k)).join('\n     '));
    const empty = Object.entries(table).filter(([, v]) => typeof v !== 'string' || !v.trim()).map(([k]) => k);
    empty.length === 0 ? ok(`${code}: no empty translations`) : bad(`${code}: ${empty.length} empty translation(s)`, empty.slice(0, 5).join('\n     '));
}

// ---- t() semantics ----
Lang._cur = 'en';
t('Dashboard') === 'Dashboard' ? ok('t() returns the key itself in English') : bad('t() altered an English key');
t('no such key {n}', { n: 7 }) === 'no such key 7' ? ok('t() substitutes placeholders even for untranslated keys') : bad('placeholder substitution broken for fallback');
if (I18N.zh && Object.keys(I18N.zh).length) {
    Lang._cur = 'zh';
    t('Dashboard') !== 'Dashboard' ? ok('zh: t() returns the translation') : bad('zh: t() did not translate "Dashboard"');
    /{n}/.test(t('{n} connected', { n: 3 })) === false && /3/.test(t('{n} connected', { n: 3 }))
        ? ok('zh: placeholders are substituted after lookup') : bad('zh: placeholder left unsubstituted');
}
const offered = Lang.available.map(l => l.code);
offered.includes('en') ? ok('picker always offers English') : bad('picker lost English');
const emptyOffered = offered.filter(c => c !== 'en' && !(I18N[c] && Object.keys(I18N[c]).length));
emptyOffered.length === 0 ? ok('picker never offers a language with an empty table') : bad('picker offers empty table(s)', emptyOffered.join(','));

// ---- English-only sources ----
const cjk = /[㐀-鿿豈-﫿]/;
const offenders = [];
for (const f of files) {
    const lines = fs.readFileSync(f, 'utf8').split('\n');
    lines.forEach((l, i) => { if (cjk.test(l)) offenders.push(`${f}:${i + 1}`); });
}
offenders.length === 0 ? ok('no CJK text outside js/i18n/ (English is the only source language)')
                       : bad(`${offenders.length} line(s) with CJK text outside js/i18n/`, offenders.slice(0, 8).join('\n     '));

console.log(`\nResult: ${PASS} passed, ${FAIL} failed`);
process.exit(FAIL === 0 ? 0 : 1);
EOF
