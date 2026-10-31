#!/bin/bash
# frontend/direct/test_i18n.sh —— 直连型页面的翻译表与源码键一致（不用板子，只要 node）
#
#   bash frontend/direct/test_i18n.sh
#
# 直连型的中文放在 frontend/direct/i18n/zh.js，合并进共用的 zh 表（共用的 js/i18n/zh.js 属于 HA 型的打包，
# 不能为直连型改，见那个文件头）。共用部分的一致性由 ../test_i18n.sh 管；这里管直连型这一份：
#   · direct/js 里每个 t() 键（含 MUSE_*_MAP / MUSE_ERR 表里经 t() 输出的值）在合并后的 zh 表里都有译文
#   · direct/i18n/zh.js 里没有源码不存在的键（笔误 / 过期），也不重复共用表已有的键（两处都有时谁覆盖谁全看加载顺序）
#   · 每条译文的 {占位符} 与键一致、没有空译文
#   · direct 源码不含 CJK 字面量
#   · 打出来的 direct/dist/index.html 里 direct/i18n/zh.js 排在 js/i18n/zh.js 之后（否则 Object.assign 的对象还不存在）

set -u
cd "$(dirname "$0")"
node - <<'EOF'
const fs = require('fs'), path = require('path');
let PASS = 0, FAIL = 0;
const ok  = m => { console.log('✅ ' + m); PASS++; };
const bad = (m, d) => { console.log('❌ ' + m + (d ? '\n     ' + d : '')); FAIL++; };

const base = ['../js/i18n.js', ...fs.readdirSync('../js/i18n').map(f => '../js/i18n/' + f)];
const load = (files) => new Function('ctx', `with (ctx) { ${files.map(f => fs.readFileSync(f, 'utf8')).join('\n')}; return { I18N, Lang, t }; }`)(
    { console, navigator: undefined, document: undefined, localStorage: undefined });
let shared, merged;
try {
    shared = Object.assign({}, load(base).I18N.zh);
    merged = load([...base, 'i18n/zh.js']);
    ok('shared tables + direct/i18n/zh.js parse and merge');
} catch (e) { bad('i18n sources failed to evaluate', e.message); process.exit(1); }
const direct = (() => {           // the direct table on its own: evaluate it against an empty zh table
    const ctx = { I18N: { zh: {} } };
    new Function('ctx', `with (ctx) { ${fs.readFileSync('i18n/zh.js', 'utf8')} }`)(ctx);
    return ctx.I18N.zh;
})();

const files = fs.readdirSync('js').filter(f => f.endsWith('.js')).map(f => 'js/' + f);
const re = /\bt\(\s*(['"])((?:\\.|(?!\1).)*)\1/g;
const keys = new Map();
for (const f of files) {
    const s = fs.readFileSync(f, 'utf8');
    let m;
    while ((m = re.exec(s))) {
        const k = m[1] === "'" ? m[2].replace(/\\'/g, "'") : m[2].replace(/\\"/g, '"');
        if (!keys.has(k)) keys.set(k, path.basename(f));
    }
    for (const b of s.match(/(?:_MSG|ERR|MAP|M)\s*=\s*\{[\s\S]*?\n\s*\}/g) || []) {
        for (const v of b.match(/:\s*'((?:[^'\\]|\\.)*)'/g) || []) {
            const k = v.replace(/^:\s*'/, '').replace(/'$/, '').replace(/\\'/g, "'");
            if (!keys.has(k)) keys.set(k, path.basename(f) + ' (table)');
        }
    }
}
keys.size > 50 ? ok(`${keys.size} translation keys found in direct/js`) : bad(`only ${keys.size} keys found — is the key scan blind?`);

const zh = merged.I18N.zh;
const missing = [...keys.keys()].filter(k => !(k in zh));
missing.length === 0 ? ok('every direct key has a zh translation (shared or direct table)')
                     : bad(`${missing.length} direct key(s) without translation`, missing.slice(0, 5).map(k => JSON.stringify(k)).join('\n     '));
const stale = Object.keys(direct).filter(k => !keys.has(k));
stale.length === 0 ? ok('direct table has no stale or misspelled keys')
                   : bad(`${stale.length} key(s) in direct/i18n/zh.js not used by direct/js`, stale.slice(0, 5).map(k => JSON.stringify(k)).join('\n     '));
const dup = Object.keys(direct).filter(k => k in shared);
dup.length === 0 ? ok('direct table repeats no key of the shared zh table')
                 : bad(`${dup.length} key(s) already in js/i18n/zh.js — drop them here`, dup.slice(0, 5).map(k => JSON.stringify(k)).join('\n     '));
const ph = k => (k.match(/\{[a-z_]+\}/g) || []).sort().join(',');
const mism = Object.entries(direct).filter(([k, v]) => ph(k) !== ph(v)).map(([k]) => k);
mism.length === 0 ? ok('placeholders match in every direct entry')
                  : bad(`${mism.length} entr(ies) with mismatched {placeholders}`, mism.slice(0, 5).map(k => JSON.stringify(k)).join('\n     '));
const empty = Object.entries(direct).filter(([, v]) => typeof v !== 'string' || !v.trim()).map(([k]) => k);
empty.length === 0 ? ok('no empty translations') : bad(`${empty.length} empty translation(s)`, empty.slice(0, 5).join('\n     '));

merged.Lang._cur = 'zh';
merged.t('Pair with Muse') !== 'Pair with Muse' && merged.t('Dashboard') !== 'Dashboard'
    ? ok('zh: t() translates both direct and shared keys after the merge') : bad('zh: merge lost one of the tables');

const cjk = /[㐀-鿿豈-﫿]/;
const offenders = [];
for (const f of files) fs.readFileSync(f, 'utf8').split('\n').forEach((l, i) => { if (cjk.test(l)) offenders.push(`${f}:${i + 1}`); });
offenders.length === 0 ? ok('no CJK text in direct/js') : bad(`${offenders.length} line(s) with CJK text`, offenders.slice(0, 8).join('\n     '));

if (fs.existsSync('dist/index.html')) {
    const secs = [...fs.readFileSync('dist/index.html', 'utf8').matchAll(/^\/\/ --- (\S+) ---$/gm)].map(m => m[1]);
    const a = secs.indexOf('js/i18n/zh.js'), b = secs.indexOf('direct/i18n/zh.js');
    a >= 0 && b > a ? ok('bundle: direct/i18n/zh.js comes after js/i18n/zh.js') : bad('bundle order wrong', secs.join(' '));
} else {
    console.log('⏭  dist/index.html not built (run frontend/direct/build.sh) — bundle order not checked');
}

console.log(`\nResult: ${PASS} passed, ${FAIL} failed`);
process.exit(FAIL === 0 ? 0 : 1);
EOF
