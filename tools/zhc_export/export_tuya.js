#!/usr/bin/env node
/* tools/zhc_export/export_tuya.js —— 从社区设备库 zigbee-herdsman-converters（Zigbee2MQTT 用的那个，MIT 许可）导出涂鸦设备的数据点表，
 * 生成 components/zhub/zh_tuya_db.c（直连型 zhub 用；docs/15 设计 8.8 注记"社区设备库"）。生成物提交进仓库：编固件不需要 node。
 *
 *   mkdir -p /tmp/zhc && (cd /tmp/zhc && npm init -y >/dev/null && npm install --ignore-scripts zigbee-herdsman-converters@<版本>)
 *   node tools/zhc_export/export_tuya.js /tmp/zhc/node_modules/zigbee-herdsman-converters components/zhub/zh_tuya_db.c
 *
 * 做法：**不读转换器的代码，只喂报文看输出**（库里涂鸦设备有两种写法：新式的 meta.tuyaDatapoints 表，和老式的手写 switch，
 * 如最常见的温湿度计 _TZE200_bjawzodf；两种都在 fromZigbee 里挂在 manuSpecificTuya 簇上）。对每条指纹、每个数据点 1–255：
 *  - 先用几种类型（数值 / 枚举 / 布尔）各喂一两个值，看输出里有没有 zhub 认得的属性（FIELDS）；
 *  - 有就细推：数值类用 4 字节数值喂一组样例，拟合 y = k·x + b（换成目标单位的整数乘数 / 偏移）；再喂 0x0000FFF6 看是不是
 *    "16 位补码装在 4 字节里"的老设备（ZH_TDB_LIN16）；开关类用枚举 0–7 逐个判"算不算激活"得到位掩码，枚举分不开再试布尔。
 * "激活"优先按定义自己 exposes 里 binary 的 value_on 判，没有才用 FIELDS 的词表。推断不出的那一条不导（计数打印，ZHC_DEBUG=1 列出）。
 * 转换器会按厂商名分支（如 _TZE200_bjawzodf 的湿度要 ÷10），所以逐条指纹单独喂；推断出一样的行共用一条。
 * 同一个"厂商名 + 型号"出现在多个定义里：先出现的算。只写了 zigbeeModel 的定义按型号认（厂商名记空串）。
 * 第三个参数给了就另写一份"金样例"（同一批报文喂库得到的期望值），test_host 拿它逐条对照固件的解码 + 换算；同目录再写一份写方向的
 * zh_tuya_gold_ctl.inc（库的 toZigbee 对开 / 关 / 亮度发出的数据点）。写方向的推法见 tzSet / inferCtl 上面的注释（M5 第九步）。
 * 输出与库的版本一一对应、不带时间戳：同一版本重跑逐字节一致。 */
'use strict';
const fs = require('fs');
const path = require('path');
const sim = require('./zhc_sim');
const { linear, mask, binaries, fakeDevice, DT_BOOL, DT_NUM, DT_ENUM } = sim;

const PKG = process.argv[2];
const OUT = process.argv[3];
const GOLD = process.argv[4];
if (!PKG || !OUT) {
    console.error('用法：node export_tuya.js <zigbee-herdsman-converters 包目录> <输出 .c> [金样例 .inc]');
    process.exit(2);
}
/* 库自己说"这个数据点不认识"的那次输出不算：老式的 tuya_dimmer 对任何数据点都吐 brightness，但会先记一条 unexpected Tuya DataPoint */
const { zhc, pkg, license, say } = sim.init(PKG, /unexpected Tuya DataPoint|Unrecognized DP|not defined for/i);
sim.guardExit();
const CATCHALL_DPS = 8;   /* 同一属性出现在这么多个数据点上 = 兜底分支（不管哪个数据点都吐），这台设备不导这个属性 */
const FIELDS = sim.FIELDS;
const PROP2FIELD = sim.prop2field(['VOLTAGE']);   /* 涂鸦的 voltage 是插座的市电伏特，不是电池毫伏 */

/* ---- 模拟 z2m 收到一条涂鸦报文 ---- */
const setCur = (manufacturerName, modelID) => sim.setCur(manufacturerName, modelID);
/* 喂一条只带一个数据点的 dataReport，返回所有涂鸦簇转换器的输出（含 publish 出去的）合起来。每次新建 device / meta：有的转换器往里写状态 */
function simDp(d, fzs, dp, dt, raw) {
    let data;
    if (dt === DT_BOOL) data = Buffer.from([raw ? 1 : 0]);
    else if (dt === DT_ENUM) data = Buffer.from([raw & 0xff]);
    else { data = Buffer.alloc(4); data.writeInt32BE(raw | 0); }
    const dev = fakeDevice();
    const out = {};
    for (const c of fzs) {
        const got = {};
        const publish = p => { if (p && typeof p === 'object') Object.assign(got, p); };
        const msg = { data: { seq: 1, dpValues: [{ dp, datatype: dt, data }] }, type: c.msgType, cluster: 'manuSpecificTuya',
                      device: dev, endpoint: dev.endpoints[0], meta: {}, linkquality: 100, groupID: 0 };
        const meta = { state: {}, device: dev, deviceExposesChanged: () => {}, options: {} };
        sim.clearUnexpected();
        try {
            const r = c.convert(d, msg, publish, {}, meta);
            if (r && typeof r === 'object' && typeof r.then !== 'function') Object.assign(got, r);
        } catch (e) { continue; /* 这个类型 / 值它不收 */ }
        if (!sim.unexpected()) Object.assign(out, got);
    }
    return out;
}
function tuyaFzs(d) {
    const out = [];
    for (const c of d.fromZigbee || []) {
        if (!c || typeof c.convert !== 'function') continue;
        const cl = Array.isArray(c.cluster) ? c.cluster : [c.cluster];
        if (!cl.includes('manuSpecificTuya')) continue;
        const ty = Array.isArray(c.type) ? c.type : [c.type];
        const t = ['commandDataReport', 'commandDataResponse', 'commandActiveStatusReport', 'commandActiveStatusReportAlt'].find(x => ty.includes(x));
        if (t) out.push({ convert: c.convert, msgType: t });
    }
    return out;
}

const defs = sim.loadDefs(PKG);
const strStat = { skipped: 0 };
const devStr = s => sim.devStr(s, strStat);

const devs = [], devIdx = new Map();   /* 行表（JSON）→ devs 下标：不同定义 / 指纹推断出一样的行就共用一条 */
const fps = new Map();                   /* "厂商名\0型号" → devs 下标；厂商名为空 = 只按型号认（定义只写了 zigbeeModel） */
const gold = [];                         /* 金样例：[厂商名, 型号, dp, 类型, 原始值, 字段, 期望] */
let defsSeen = 0, defsLegacy = 0, dupFp = 0, asBinary = 0;
const bad = new Map();                   /* "定义|dp|属性" → 属性名：推断不出的（同一定义多条指纹只计一次） */
const PROBES = [[DT_NUM, 1], [DT_NUM, 100], [DT_ENUM, 0], [DT_ENUM, 1], [DT_ENUM, 2], [DT_BOOL, 0], [DT_BOOL, 1]];

function inferRows(d, fzs, bins, tag) {
    const rows = [], samples = [];
    let fields = 0;
    const hits = new Map();   /* dp → 属性集合 */
    const perProp = {};
    for (let dp = 1; dp <= 255; dp++) {
        const props = new Set();
        for (const [dt, raw] of PROBES) for (const [k, v] of Object.entries(simDp(d, fzs, dp, dt, raw))) if (v !== undefined && PROP2FIELD.has(k)) props.add(k);
        if (props.size) hits.set(dp, props);
        for (const p of props) perProp[p] = (perProp[p] || 0) + 1;
    }
    for (const [dp, props] of hits) {
        for (const prop of [...props].sort()) {
            if (perProp[prop] > CATCHALL_DPS) {
                const k = d.model + '|*|' + prop;
                if (!bad.has(k)) { bad.set(k, prop + '(兜底)'); if (process.env.ZHC_DEBUG) process.stderr.write('兜底分支：' + tag + ' ' + prop + ' 出现在 ' + perProp[prop] + ' 个数据点上\n'); }
                continue;
            }
            const fi = PROP2FIELD.get(prop), F = FIELDS[fi];
            /* 有的转换器把数值 toFixed 成字符串发布（"21.5"），按数值算 */
            const get = (dt, raw) => { const y = simDp(d, fzs, dp, dt, raw)[prop]; return typeof y === 'string' && /^-?\d+(\.\d+)?$/.test(y) ? Number(y) : y; };
            let row = null, smp = [];
            if (F.kind === 'lin') {
                const l = linear(x => get(DT_NUM, x), F.unit);
                if (l) {
                    row = { dp, field: fi, kind: l.w16 ? 3 : 1, mask: 0, mul: l.mul, off: l.off };
                    for (const x of [37, 215, 999].concat(l.w16 ? [0xfff6] : [])) {
                        const y = get(DT_NUM, x);
                        if (typeof y === 'number') smp.push([DT_NUM, x, Math.round(y * F.unit)]);
                    }
                }
            } else {
                const be = bins.get(prop);
                const useBin = be && be.value_on !== undefined && be.value_on !== null;
                /* exposes 说 value_on 是 true、转换器却吐 "True" 这类库里自相矛盾的，退回词表 */
                let act = useBin ? (y => y === be.value_on) : F.active;
                let mk = mask(get, act);
                if (!mk && useBin) { act = F.active; mk = mask(get, act); }
                if (mk) {
                    row = { dp, field: fi, kind: 2, mask: mk.m, mul: 0, off: 0 };
                    if (act !== F.active) asBinary++;
                    for (let i = 0; i < (mk.dt === DT_BOOL ? 2 : 4); i++) {
                        const y = get(mk.dt, i);
                        if (y !== undefined) smp.push([mk.dt, i, act(y) ? 1 : 0]);
                    }
                }
            }
            if (!row) {
                const k = d.model + '|' + dp + '|' + prop;
                if (!bad.has(k)) {
                    bad.set(k, prop);
                    if (process.env.ZHC_DEBUG) process.stderr.write('推断不出：' + tag + ' dp' + dp + ' ' + prop + '  ' + JSON.stringify([0, 1, 2, 100].map(x => get(DT_NUM, x))) + ' ' + JSON.stringify([0, 1, 2, 3].map(x => get(DT_ENUM, x))) + '\n');
                }
                continue;
            }
            if (rows.some(x => x.dp === row.dp && x.field === row.field)) continue;   /* 同一个数据点两个别名（如 occupancy / presence） */
            rows.push(row);
            fields |= 1 << fi;
            for (const [dt, raw, y] of smp) samples.push([dp, dt, raw, fi, y]);
        }
    }
    rows.sort((a, b) => a.dp - b.dp || a.field - b.field);
    return { rows, fields, samples };
}

/* ---- 写方向（M5 第九步）：喂 toZigbee，看它往 0xEF00 发什么 ----
 * 查转换器照 z2m 的派发：toZigbee 里第一个"没写 key 或 key 含这个属性"的（新式的数据点转换器不写 key）。只认恰好发出一条 manuSpecificTuya 命令、
 * 里面恰好一个数据点的：开 / 关各喂一次（同一个 dp / 类型 / 命令、值不同才算数）；亮度按百分比喂（brightness = round(p × 254 / 100)，同 z2m 的量程），
 * 拟合 原始值 = p × mul / 10000 + off。命令号：dataRequest 0x00 / sendData 0x04（zigbee-herdsman cluster.js:7157、7168；定义的 meta.tuyaSendCommand 选哪个）。 */
const TUYA_CMD = { dataRequest: 0x00, sendData: 0x04 };
let tzTimeouts = 0;   /* 3 s 没完的 toZigbee 调用（当没输出） */
async function tzSet(d, key, value) {
    const c = (d.toZigbee || []).find(t => t && typeof t.convertSet === 'function' && (!t.key || (Array.isArray(t.key) && t.key.includes(key))));
    if (!c) return null;
    const dev = fakeDevice(), ep = dev.endpoints[0], sent = [];
    ep.command = async (cl, cmd, payload) => { sent.push({ cl, cmd, payload }); };
    const meta = { state: { state: 'ON', brightness: 254 }, device: dev, mapped: d, message: { [key]: value }, options: {}, endpoint_name: undefined };
    sim.step(d.model + ' toZigbee ' + key + '=' + value);
    try { if ((await sim.withTimeout(c.convertSet(ep, key, value, meta), 3000)) === sim.TIMEOUT) { tzTimeouts++; return null; } } catch (e) { return null; }
    if (sent.length !== 1 || sent[0].cl !== 'manuSpecificTuya') return null;
    const cmd = TUYA_CMD[sent[0].cmd];
    const dvs = sent[0].payload && sent[0].payload.dpValues;
    if (cmd === undefined || !Array.isArray(dvs) || dvs.length !== 1) return null;
    const v = dvs[0], data = Array.from(v.data || []);
    let raw;
    if ((v.datatype === DT_BOOL || v.datatype === DT_ENUM) && data.length === 1) raw = data[0];
    else if (v.datatype === DT_NUM && data.length === 4) raw = Buffer.from(data).readInt32BE(0);
    else return null;
    if (typeof v.dp !== 'number' || v.dp < 1 || v.dp > 255) return null;
    return { cmd, dp: v.dp, dt: v.datatype, raw };
}
/* 亮度 1–100 % 逐个喂一遍（样例少了最小二乘会被两次取整带偏：0–1000 的那种在 10 % 处差 4），先拿全量程拟合，不成再只拿 10–100 %（有的把下限钳住）；
   上下限取库实际发过的最小 / 最大原始值（拟合会把 100 % 算到 1001） */
const LVL_FROM = [1, 10];
async function inferCtl(d) {
    const on = await tzSet(d, 'state', 'ON'), off = await tzSet(d, 'state', 'OFF');
    let onoff = null;
    if (on && off && on.dp === off.dp && on.dt === off.dt && on.cmd === off.cmd && on.raw !== off.raw) onoff = { cmd: on.cmd, dp: on.dp, dt: on.dt, on: on.raw, off: off.raw };
    let level = null;
    const w = [];
    const first = await tzSet(d, 'brightness', 254);
    if (first && first.dt === DT_NUM) for (let p = 1; p <= 100; p++) w[p] = await tzSet(d, 'brightness', Math.round(p * 254 / 100));
    for (const p0 of first && first.dt === DT_NUM ? LVL_FROM : []) {
        const ps = [];
        for (let p = p0; p <= 100; p++) ps.push(p);
        if (!ps.every(p => w[p] && w[p].dt === DT_NUM && w[p].dp === first.dp && w[p].cmd === first.cmd)) continue;
        const n = ps.length, sx = ps.reduce((a, x) => a + x, 0), sy = ps.reduce((a, x) => a + w[x].raw, 0);
        const sxx = ps.reduce((a, x) => a + x * x, 0), sxy = ps.reduce((a, x) => a + x * w[x].raw, 0);
        const k = (n * sxy - sx * sy) / (n * sxx - sx * sx), b = (sy - k * sx) / n;
        if (!(k > 0)) continue;                    /* 亮度越大原始值越大，别的不认 */
        const tol = 0.5 * k * 100 / 254 + 1.01;    /* 先把百分比取整成 0–254、库再取整一次：各差半格 */
        if (!ps.every(x => Math.abs(k * x + b - w[x].raw) <= tol)) continue;
        const raws = [];
        for (let p = 1; p <= 100; p++) if (w[p] && w[p].dp === first.dp && w[p].dt === DT_NUM) raws.push(w[p].raw);
        level = { cmd: first.cmd, dp: first.dp, mul: Math.round(k * 10000), off: Math.round(b), lo: Math.min(...raws), hi: Math.max(...raws),
                  tol: Math.ceil(tol), samples: [1, 5, 10, 25, 50, 75, 99, 100].filter(x => x >= p0).map(x => [x, w[x].raw]) };
        break;
    }
    if (onoff && level && (onoff.cmd !== level.cmd || onoff.dp === level.dp)) level = null;   /* 一台只记一个命令号；开关与亮度同一个 dp 的不认 */
    if (!onoff && !level) return null;
    return { cmd: onoff ? onoff.cmd : level.cmd, onoff, level };
}

/* 第一遍（同步）：每条指纹的读方向。多个定义认同一条指纹：先出现的算（哪怕它的行推不出来 —— 别拿后面另一个定义的去顶） */
const perFp = [], seenKey = new Set();
for (const d0 of defs) {
    let d;
    try { d = zhc.prepareDefinition(d0); } catch (e) { continue; }
    const fzs = tuyaFzs(d);
    if (!fzs.length) continue;
    defsSeen++;
    if (!(d.meta && d.meta.tuyaDatapoints)) defsLegacy++;
    const keys = [];
    for (const f of d.fingerprint || []) {
        const mfr = devStr(f.manufacturerName);
        if (!mfr) continue;
        /* 型号：没写 / 截到 0 字节后是空串（如 _TZE200_f1pvdgoh 的 "\u0000B"：固件读回的就是空串）→ 空串；写了但不合格（超长 / 非 ASCII）→ 整条跳过，别放宽成"任意型号" */
        const m0 = typeof f.modelID === 'string' ? f.modelID.split('\0')[0] : '';
        const model = m0 === '' ? '' : devStr(m0);
        if (model === null) continue;
        keys.push([mfr, model]);
    }
    for (const zm of d.zigbeeModel || []) { const m = devStr(zm); if (m) keys.push(['', m]); }
    for (const [mfr, model] of keys) {
        const key = mfr + '\0' + model;
        if (seenKey.has(key)) { dupFp++; continue; }
        seenKey.add(key);
        setCur(mfr, model);
        perFp.push({ key, mfr, model, d, r: inferRows(d, fzs, binaries(d), d.model + '(' + (mfr || model) + ')') });
    }
}

const ctls = [], ctlIdx = new Map(), goldCtl = [];
let ctlFps = 0, ctlDropped = 0;
/* 主流程的错误必须冒出来：全局的 unhandledRejection 是给库里漏网的 Promise 用的，会把这里的错也吞掉、只写了一半的文件看着像成功 */
(async () => {
for (const p of perFp) {
    setCur(p.mfr, p.model);
    p.c = await inferCtl(p.d);
}
for (const p of perFp) {
    const r = p.r;
    let c = p.c;
    /* 只导"同一个 dp 也读得回来"的写法：确认要看设备回报这个 dp，列表里的能力（按读方向推）也才和"可控"一致 */
    if (c) {
        /* 1 = ONOFF：读方向那一行要把写的"开"值判成开、"关"值判成关（切换按读数取反、确认比对值都靠它；审计 ⚠️） */
        const rowOn = c.onoff && r.rows.find(x => x.field === 1 && x.dp === c.onoff.dp && x.kind === 2);
        const active = v => v >= 0 && v <= 7 && ((rowOn.mask >> v) & 1) === 1;
        const onoff = rowOn && active(c.onoff.on) && !active(c.onoff.off) ? c.onoff : null;
        const level = c.level && r.rows.some(x => x.field === 2 && x.dp === c.level.dp) ? c.level : null;   /* 2 = LEVEL */
        if ((c.onoff && !onoff) || (c.level && !level)) ctlDropped++;
        c = onoff || level ? { cmd: c.cmd, onoff, level } : null;
    }
    p.c = c;
    if (!r.rows.length && !c) continue;
    let ci = 0xffff;
    if (c) {
        const ck = JSON.stringify([c.cmd, c.onoff, c.level && [c.level.dp, c.level.mul, c.level.off, c.level.lo, c.level.hi]]);
        ci = ctlIdx.get(ck);
        if (ci === undefined) { ci = ctls.length; ctls.push(c); ctlIdx.set(ck, ci); }
        ctlFps++;
    }
    const k = JSON.stringify([r.rows, ci]);
    let di = devIdx.get(k);
    if (di === undefined) {
        di = devs.length;
        devs.push({ rows: r.rows, fields: r.fields, ctl: ci });
        devIdx.set(k, di);
        for (const s of r.samples) gold.push([p.mfr, p.model].concat(s));   /* 每种组合留第一条指纹的样例 */
        if (c) {                                   /* 写方向的金样例：[厂商名, 型号, 动作 0 开 / 1 关 / 2 亮度, 百分比, 命令号, dp, 类型, 原始值, 容差] */
            if (c.onoff) { goldCtl.push([p.mfr, p.model, 0, 0, c.cmd, c.onoff.dp, c.onoff.dt, c.onoff.on, 0]); goldCtl.push([p.mfr, p.model, 1, 0, c.cmd, c.onoff.dp, c.onoff.dt, c.onoff.off, 0]); }
            if (c.level) for (const [pc, raw] of c.level.samples) goldCtl.push([p.mfr, p.model, 2, pc, c.cmd, c.level.dp, DT_NUM, raw, c.level.tol]);
        }
    }
    fps.set(p.key, di);
}
emit();
sim.done();
})().catch(e => { process.stderr.write('❌ 导出失败：' + (e && e.stack || e) + '\n'); process.exit(1); });

function emit() {
const rowsKept = devs.reduce((a, d) => a + d.rows.length, 0), rowsBad = bad.size;
const badByProp = {};
for (const p of bad.values()) badByProp[p] = (badByProp[p] || 0) + 1;

/* 字符串池：厂商名 / 型号去重 */
const pool = new sim.StrPool();
const fpList = [...fps.entries()].map(([k, di]) => { const [mfr, model] = k.split('\0'); return { mfr, model, di }; });
fpList.sort((a, b) => (a.mfr < b.mfr ? -1 : a.mfr > b.mfr ? 1 : a.model < b.model ? -1 : a.model > b.model ? 1 : 0));
for (const f of fpList) { f.mo = pool.add(f.mfr); f.oo = pool.add(f.model); }
/* 表的字段宽度（zh_tuya_db.h）：字符串池偏移 / 行下标 / 设备下标 u16，每台的行数 u8（池超 64 KB 由 StrPool 自己报） */
if (rowsKept > 0xffff || devs.length > 0xffff || ctls.length >= 0xffff || devs.some(d => d.rows.length > 0xff)) throw new Error('表超出 zh_tuya_db.h 的字段宽度');
const DP_FIELDS_MAX = 4;   /* = zh_tuya_db.h 的 ZH_TUYA_DP_FIELDS_MAX */
if (devs.some(d => d.rows.some(r => d.rows.filter(x => x.dp === r.dp).length > DP_FIELDS_MAX))) throw new Error('同一个 dp 超过 ' + DP_FIELDS_MAX + ' 行（zh_dev 只开这么大的数组）');

const esc = sim.esc;
let rowOff = 0;
const L = [];
L.push('/* 自动生成，别手改：node tools/zhc_export/export_tuya.js（见该文件头）。');
L.push(' * 来源：zigbee-herdsman-converters ' + pkg.version + '（' + pkg.license + '），涂鸦设备（manuSpecificTuya 簇）的数据点映射：读方向只留 zhub 解得开读数的字段，写方向只有开关 / 亮度。');
L.push(' * 统计：看过涂鸦设备定义 ' + defsSeen + ' 个（其中老式手写的 ' + defsLegacy + ' 个）；导出指纹 ' + fpList.length + ' 条、不同的数据点组合 ' + devs.length + ' 种共 ' + rowsKept + ' 条（推断不出换算没导的 ' + rowsBad + ' 条）。');
L.push(' *');
L.push(...sim.licenseLines(license));
L.push(' */');
L.push('#include "zh_tuya_db.h"');
L.push('');
L.push('const char zh_tdb_source[] = "zigbee-herdsman-converters ' + esc(pkg.version) + '";');
L.push('');
L.push('const zh_tdb_row_t zh_tdb_rows[] = {');
for (const dv of devs) for (const r of dv.rows) L.push('    { ' + r.dp + ', ' + r.field + ', ' + r.kind + ', 0x' + r.mask.toString(16).padStart(2, '0') + ', ' + r.mul + ', ' + r.off + ' },');
L.push('};');
L.push('const zh_tdb_dev_t zh_tdb_devs[] = {');
for (const dv of devs) { L.push('    { ' + rowOff + ', ' + dv.rows.length + ', 0x' + dv.fields.toString(16) + 'u, ' + (dv.ctl === 0xffff ? '0xFFFF' : dv.ctl) + ' },'); rowOff += dv.rows.length; }
L.push('};');
L.push('const zh_tdb_ctl_t zh_tdb_ctls[] = {     /* 写方向：命令号、开关 dp / 类型 / 开 / 关、亮度 dp / mul / off / 下限 / 上限（dp 0 = 没有） */');
for (const c of ctls) L.push('    { 0x' + c.cmd.toString(16).padStart(2, '0') + ', ' + (c.onoff ? c.onoff.dp + ', ' + c.onoff.dt + ', ' + c.onoff.on + ', ' + c.onoff.off : '0, 0, 0, 0') + ', ' +
                             (c.level ? c.level.dp + ', ' + c.level.mul + ', ' + c.level.off + ', ' + c.level.lo + ', ' + c.level.hi : '0, 0, 0, 0, 0') + ' },');
if (!ctls.length) L.push('    { 0 },');
L.push('};');
L.push('const zh_tdb_fp_t zh_tdb_fps[] = {     /* 按（厂商名，型号）排序：二分查找 */');
for (const f of fpList) L.push('    { ' + f.mo + ', ' + f.oo + ', ' + f.di + ' },');
L.push('};');
L.push(...pool.lines('zh_tdb_str'));
L.push('const unsigned zh_tdb_n_rows = sizeof(zh_tdb_rows) / sizeof(zh_tdb_rows[0]);');
L.push('const unsigned zh_tdb_n_devs = sizeof(zh_tdb_devs) / sizeof(zh_tdb_devs[0]);');
L.push('const unsigned zh_tdb_n_fps = sizeof(zh_tdb_fps) / sizeof(zh_tdb_fps[0]);');
L.push('const unsigned zh_tdb_str_len = sizeof(zh_tdb_str);');
L.push('const unsigned zh_tdb_n_ctls = ' + ctls.length + ';');
fs.writeFileSync(OUT, L.join('\n') + '\n');
say(`zigbee-herdsman-converters ${pkg.version}: 定义 ${defsSeen}（老式 ${defsLegacy}）、指纹 ${fpList.length}（重复的 ${dupFp} 条按先出现的、字符串不合格没导的 ${strStat.skipped} 条）、组合 ${devs.length}、数据点 ${rowsKept}（推断不出 ${rowsBad}；开关类按 exposes 判的 ${asBinary}）、字符串池 ${pool.len} B、金样例 ${gold.length} 条`);
say(`写方向：能控制的指纹 ${ctlFps} 条（开关 ${perFp.filter(p => p.c && p.c.onoff).length}、亮度 ${perFp.filter(p => p.c && p.c.level).length}；读不回同一个 dp 没导的 ${ctlDropped} 条；3 s 没完的 toZigbee ${tzTimeouts} 次）、不同写法 ${ctls.length} 种、金样例 ${goldCtl.length} 条`);
say('推断不出的（按属性）：' + Object.entries(badByProp).sort((a, b) => b[1] - a[1]).map(([k, v]) => k + ':' + v).join(' '));

if (GOLD) {
    const G = ['/* 自动生成，别手改：export_tuya.js 的第三个输出。zigbee-herdsman-converters ' + pkg.version + ' 对同一批报文给出的期望值（目标单位），',
               ' * zh_tuya_test.c 逐条对照固件的查表 + 解码 + 换算。列：厂商名、型号、dp、类型（1 布尔 / 2 数值 / 4 枚举）、原始值、字段、期望 */'];
    for (const g of gold) G.push('{ "' + esc(g[0]) + '", "' + esc(g[1]) + '", ' + g.slice(2).join(', ') + ' },');
    fs.writeFileSync(GOLD, G.join('\n') + '\n');
    /* 写方向的金样例：同一目录下的 zh_tuya_gold_ctl.inc */
    const GC = ['/* 自动生成，别手改：export_tuya.js 的第三个输出旁边那份。zigbee-herdsman-converters ' + pkg.version + ' 的 toZigbee 对开 / 关 / 亮度发出的数据点，',
                ' * zh_tuya_test.c 逐条对照固件编出的负载。列：厂商名、型号、动作（0 开 / 1 关 / 2 亮度）、百分比、命令号、dp、类型、原始值、原始值容差 */'];
    for (const g of goldCtl) GC.push('{ "' + esc(g[0]) + '", "' + esc(g[1]) + '", ' + g.slice(2).join(', ') + ' },');
    fs.writeFileSync(path.join(path.dirname(GOLD), 'zh_tuya_gold_ctl.inc'), GC.join('\n') + '\n');
}
}
