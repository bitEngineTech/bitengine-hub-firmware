#!/usr/bin/env node
/* tools/zhc_export/export_lumi.js —— 从社区设备库 zigbee-herdsman-converters（MIT）导出小米 / Aqara（"lumi"）设备表，
 * 生成 components/zhub/zh_lumi_db.c（直连型 zhub 用；docs/15 设计 8.8 注记"社区设备库"，M5 第十步）。生成物提交进仓库：编固件不需要 node。
 *
 *   node tools/zhc_export/export_lumi.js <包目录> components/zhub/zh_lumi_db.c [components/zhub/test_host/zh_lumi_gold.inc]
 *   （平常用 tools/zhc_export/regen.sh，它两个导出器都跑）
 *
 * 同涂鸦那个一样**不读转换器的代码，只喂报文看输出**（共用 zhc_sim.js）。小米的读数有三路，逐个型号（zigbeeModel 串）喂：
 *  - TLV：Basic 簇 0xFF01（herdsman 解成 {标签: 值} 交给 lumi_basic）与 0xFCC0 簇 0x00F7（字节串，lumi_specific 自己按 TLV 解）。
 *    标签 0–255 各喂几种值，看输出里有没有 zhub 认得的字段，再按涂鸦那套推线性 / 开关换算（行的 cl = 0xFFFF）；
 *  - 0xFCC0 簇的其它属性（id 256–0x5FF，numericAttributes2Payload 认数字键）：同上（cl = 0xFCC0）；
 *  - 标准簇的改写：门磁拿 OnOff 簇报开合、灯把 Occupancy 上报丢掉、照度直接是 lux……对 zhub 通用解析认的那几个属性喂样例，
 *    推出来和通用解析不一样才记一行（cl = 簇号；字段 NONE = 这一路丢掉）；一样的不记，但字段照样算进这台的 fields（hub_zcap 定能力用）。
 * 电量：电池设备的 voltage（毫伏）记 VOLTAGE；电量 % 不拟合（库按 meta.battery.voltageToPercentage 从电压算，带钳位）—— 照抄那份参数，固件同样算。
 * 人体清零：定义的 options 里有 occupancy_timeout 的（小米人体只报"有人"，z2m 到时自动报没人），记 90 s（z2m 的默认）。
 * 第三个参数给了就另写金样例（同一批 TLV / 属性喂库的输出；电量 % 也按库算），test_host/zh_lumi_test.c 逐条对照固件。
 * 输出与库的版本一一对应、不带时间戳：同一版本重跑逐字节一致。 */
'use strict';
const fs = require('fs');
const sim = require('./zhc_sim');
const { linear, mask, binaries, fakeDevice, FIELDS, FIELD, DT_BOOL, DT_ENUM } = sim;

const PKG = process.argv[2];
const OUT = process.argv[3];
const GOLD = process.argv[4];
if (!PKG || !OUT) {
    console.error('用法：node export_lumi.js <zigbee-herdsman-converters 包目录> <输出 .c> [金样例 .inc]');
    process.exit(2);
}
const { zhc, pkg, license, say } = sim.init(PKG, null);
sim.guardExit();
const CL_TLV = 0xffff, CL_LUMI = 0xfcc0;
const OCC_TIMEOUT_S = 90;   /* z2m occupancy_with_timeout 的默认（options.occupancy_timeout 没设时） */

/* 标准簇：zhub 通用解析（zh_dev.c 的 st_apply）认的那几个属性，和它们在通用解析下的换算（目标单位，同 zh_tbl.h）。
   照度的通用解析是对数（10000·log10(lux)+1），没法写成直线：喂出来是直线就一定不一样 */
const U8_SETS = [[0, 50, 100, 150, 200], [0, 10, 20, 50, 100]], U8_PROBE = [0, 1, 2, 5, 10, 20, 37, 50, 61, 99, 100, 101, 150, 200, 254];
const MID_SETS = [[600, 1000, 1400, 1900]], MID_PROBE = [501, 600, 750, 1000, 1013, 1250, 1500, 1999];   /* lumi_pressure 只收 (500, 2000) 开区间 */
const STD = [
    { cl: 0x0006, id: 0x0000, cluster: 'genOnOff',                 attr: 'onOff',                      gen: { field: FIELD.ONOFF, kind: 2, mask: 0x02 } },
    { cl: 0x0406, id: 0x0000, cluster: 'msOccupancySensing',       attr: 'occupancy',                  gen: { field: FIELD.OCCUPANCY, kind: 2, mask: 0xaa } },
    { cl: 0x0400, id: 0x0000, cluster: 'msIlluminanceMeasurement', attr: 'measuredValue',              gen: { field: FIELD.LUX, kind: 9 } },
    { cl: 0x0402, id: 0x0000, cluster: 'msTemperatureMeasurement', attr: 'measuredValue',              gen: { field: FIELD.TEMP, kind: 1, mul: 10000, off: 0 } },
    { cl: 0x0405, id: 0x0000, cluster: 'msRelativeHumidity',       attr: 'measuredValue',              gen: { field: FIELD.HUMIDITY, kind: 1, mul: 10000, off: 0 } },
    { cl: 0x0403, id: 0x0000, cluster: 'msPressureMeasurement',    attr: 'measuredValue',              gen: { field: FIELD.PRESSURE, kind: 1, mul: 100000, off: 0 } },
    { cl: 0x0001, id: 0x0020, cluster: 'genPowerCfg',              attr: 'batteryVoltage',             gen: { field: FIELD.VOLTAGE, kind: 1, mul: 1000000, off: 0 }, u8: true },
    { cl: 0x0001, id: 0x0021, cluster: 'genPowerCfg',              attr: 'batteryPercentageRemaining', gen: { field: FIELD.BATTERY_PCT, kind: 1, mul: 5000, off: 0 }, u8: true },
];

function isLumi(d) {
    return d.vendor === 'Xiaomi' || d.vendor === 'Aqara' || (d.zigbeeModel || []).some(m => /^lumi\./.test(m));
}
function fzsOf(d, cluster, type) {
    const ty = type || 'attributeReport';
    return (d.fromZigbee || []).filter(c => c && typeof c.convert === 'function' && (Array.isArray(c.cluster) ? c.cluster.includes(cluster) : c.cluster === cluster)
                                          && (Array.isArray(c.type) ? c.type.includes(ty) : c.type === ty));
}
/* 喂一条属性上报（或 type 给的别的命令），返回这个簇所有转换器的输出合起来（同步的、异步的都等完；publish 出去的也收） */
async function simAttr(d, fzs, cluster, data, type) {
    const dev = fakeDevice();
    const out = {};
    for (const c of fzs) {
        const got = {};
        const publish = p => { if (p && typeof p === 'object') Object.assign(got, p); };
        const msg = { data, type: type || 'attributeReport', cluster, device: dev, endpoint: dev.endpoints[0], meta: {}, linkquality: 100, groupID: 0 };
        const meta = { state: {}, device: dev, deviceExposesChanged: () => {}, options: {} };
        try {
            const r = await sim.withTimeout(c.convert(d, msg, publish, {}, meta), 3000);
            if (r === sim.TIMEOUT) { fzTimeouts++; continue; }
            if (r && typeof r === 'object') Object.assign(got, r);
        } catch (e) { continue; }
        Object.assign(out, got);
    }
    return out;
}
/* TLV 的一条：[标签][类型][值，小端]；数值按 int32（0x2B）编，布尔按 0x10 —— 都是 buffer2DataObject 认的 */
function tlvBuf(tag, v) {
    if (typeof v === 'boolean') return Buffer.from([tag, 0x10, v ? 1 : 0]);
    const b = Buffer.alloc(6);
    b[0] = tag; b[1] = 0x2b; b.writeInt32LE(v | 0, 2);
    return b;
}

const defs = sim.loadDefs(PKG);
const strStat = { skipped: 0 };
const devs = [], devIdx = new Map(), models = new Map(), gold = [];
let defsSeen = 0, dupModel = 0, rowsBad = 0, battCurves = 0, occDevs = 0, catchAll = 0, fzTimeouts = 0;
const CATCHALL_TAGS = 8;
const badByProp = {};
const PROBES = [[DT_ENUM, 1], [DT_ENUM, 100], [DT_ENUM, 1000], [DT_ENUM, 0], [DT_ENUM, 2], [DT_BOOL, 0], [DT_BOOL, 1]];   /* 1000：只收一段的（气压 500–2000 hPa）也探得到 */
const val = (dt, raw) => (dt === DT_BOOL ? raw === 1 : raw);

/* 一路数据（get(dt, raw) → 输出对象）里出现的、zhub 认得的属性；skip = 这台不收的字段。EXPOSED 不空时只认定义 exposes 里声明了的属性：
   库对没列名的型号会走默认分支多吐键（老门磁的 TLV 100 吐 state，它的 exposes 里只有 contact） */
let EXPOSED = new Set();
async function probeProps(get, skip) {
    const props = new Set();
    for (const [dt, raw] of PROBES) for (const [k, v] of Object.entries(await get(dt, raw)))
        if (v !== undefined && PROP2FIELD.has(k) && !skip.includes(PROP2FIELD.get(k)) && (!EXPOSED.size || EXPOSED.has(k))) props.add(k);
    return props;
}
function exposedProps(d) {
    let ex = d.exposes;
    if (typeof ex === 'function') { try { ex = ex(fakeDevice(), {}); } catch (e) { ex = []; } }
    const out = new Set();
    const walk = a => { for (const e of a || []) { if (!e) continue; if (typeof e.property === 'string') out.add(e.property); if (Array.isArray(e.features)) walk(e.features); } };
    walk(Array.isArray(ex) ? ex : []);
    return out;
}
async function inferOne(get, bins, skip, tag, u8) {
    return inferProps(get, await probeProps(get, skip), bins, tag, u8);
}
/* 对探到的属性逐个推换算 */
async function inferProps(get, props, bins, tag, u8) {
    const rows = [];
    for (const prop of [...props].sort()) {
        const fi = PROP2FIELD.get(prop), F = FIELDS[fi];
        /* 拟合要同步取值：先把要用的点都喂一遍存起来 */
        const cache = new Map();
        const need = [];
        for (const x of (u8 ? U8_SETS.flat().concat(U8_PROBE, [37, 150, 200]) : sim.LIN_SETS.flat().concat(sim.LIN_PROBE, MID_SETS.flat(), MID_PROBE, [0xfff6, 37, 215, 999]))) need.push([DT_ENUM, x]);
        for (let i = 0; i < 8; i++) need.push([DT_ENUM, i]);
        need.push([DT_BOOL, 0], [DT_BOOL, 1]);
        for (const [dt, raw] of need) {
            const k = dt + ':' + raw;
            if (cache.has(k)) continue;
            let y = (await get(dt, raw))[prop];
            if (typeof y === 'string' && /^-?\d+(\.\d+)?$/.test(y)) y = Number(y);
            cache.set(k, y);
        }
        const g = (dt, raw) => cache.get(dt + ':' + raw);
        let row = null;
        const smp = [];
        if (F.kind === 'lin') {
            const l = u8 ? linear(x => g(DT_ENUM, x), F.unit, U8_SETS, U8_PROBE)
                         : (linear(x => g(DT_ENUM, x), F.unit) || linear(x => g(DT_ENUM, x), F.unit, MID_SETS, MID_PROBE));   /* 只收一段的（气压 500–2000 hPa） */
            if (l && fi === FIELD.VOLTAGE && !((l.mul === 10000 || l.mul === 1000000) && l.off === 0)) {
                rowsBad++;                         /* 不是毫伏型的 voltage（TLV 150 = 市电 ×0.1 V，审计 ⚠️）：不当电池电压 */
                badByProp['voltage(非毫伏)'] = (badByProp['voltage(非毫伏)'] || 0) + 1;
                continue;
            } else if (l) {
                row = { field: fi, kind: l.w16 && !u8 ? 3 : 1, mask: 0, mul: l.mul, off: l.off };
                for (const x of (u8 ? [37, 150, 200] : [37, 215, 999, 1013, 1500])) { const y = g(DT_ENUM, x); if (typeof y === 'number') smp.push([x, Math.round(y * F.unit)]); }
            }
        } else {
            const be = bins.get(prop);
            const useBin = be && be.value_on !== undefined && be.value_on !== null;
            let act = useBin ? (y => y === be.value_on) : F.active;
            let mk = mask(g, act, true);
            if (!mk && useBin) { act = F.active; mk = mask(g, act, true); }
            if (mk && mk.any) {                    /* 只要来一条就算激活（新式 Aqara 人体：0xFCC0 的 illuminance 每报一次就是有人）：大值也要激活才认 */
                if ([100, 250, 1000].every(x => { const y = g(DT_ENUM, x); return y !== undefined && act(y); })) {
                    row = { field: fi, kind: 4, mask: 0, mul: 0, off: 0 };
                    smp.push([1000, 1]);
                }
            } else if (mk) {
                row = { field: fi, kind: 2, mask: mk.m, mul: 0, off: 0 };
                for (let i = 0; i < 2; i++) { const y = g(mk.dt, i); if (y !== undefined) smp.push([i, act(y) ? 1 : 0]); }
            }
        }
        if (!row) {
            rowsBad++;
            badByProp[prop] = (badByProp[prop] || 0) + 1;
            if (process.env.ZHC_DEBUG) process.stderr.write('推断不出：' + tag + ' ' + prop + '\n');
            continue;
        }
        row.smp = smp;
        rows.push(row);
    }
    return rows;
}
const PROP2FIELD = sim.prop2field([]);

/* 0xFCC0 的属性名：跑一遍定义的 configure / onEvent(start)，截获 device.addCustomCluster('manuSpecificLumi', {attributes}) */
async function lumiAttrNames(d) {
    const out = [];
    const dev = fakeDevice();
    dev.addCustomCluster = (name, def) => {
        if (name !== 'manuSpecificLumi' || !def || !def.attributes) return;
        for (const a of Object.values(def.attributes)) if (a && typeof a.name === 'string' && Number.isInteger(a.ID)) out.push([a.name, a.ID]);
    };
    const coord = dev.endpoints[0];
    try { if (typeof d.configure === 'function') await sim.withTimeout(d.configure(dev, coord, d), 3000); } catch (e) { /* configure 别的步骤在假设备上会失败，名字已经截到 */ }
    try { if (typeof d.onEvent === 'function') await sim.withTimeout(d.onEvent({ type: 'start', data: { device: dev }, device: dev, deviceExposesChanged: () => {} }), 3000); } catch (e) { }
    const seen = new Set();
    return out.filter(([n]) => !seen.has(n) && seen.add(n)).sort((a, b) => a[1] - b[1]);
}

async function inferModel(d) {
    const bins = binaries(d);
    EXPOSED = exposedProps(d);
    const battMeta = d.meta && d.meta.battery && d.meta.battery.voltageToPercentage;
    const isBattery = !!battMeta || (Array.isArray(d.exposes) && d.exposes.some(e => e && e.name === 'battery'));
    /* 电量 % 由电压按曲线算（固件同样算），不拟合；不是电池设备的 voltage 是市电伏特，不收 */
    const skip = [];
    if (battMeta) skip.push(FIELD.BATTERY_PCT);
    if (!isBattery) skip.push(FIELD.VOLTAGE);
    const fzB = fzsOf(d, 'genBasic'), fzL = fzsOf(d, 'manuSpecificLumi');
    const rows = [];
    let fields = 0;
    /* ① TLV：两路一起喂，输出合起来。先全部标签探一遍：同一属性出现在 CATCHALL_TAGS 个以上标签上 = 兜底分支（灯的 lumi_basic 对任何标签都吐 state），
          这台不导这个属性 */
    if (fzB.length || fzL.length) {
        const getOf = tag => async (dt, raw) => {
            const v = val(dt, raw);
            const a = fzB.length ? await simAttr(d, fzB, 'genBasic', { 65281: { [tag]: v } }) : {};
            const b = fzL.length ? await simAttr(d, fzL, 'manuSpecificLumi', { 247: tlvBuf(tag, v) }) : {};
            return Object.assign({}, a, b);
        };
        const hit = [], perProp = {};
        for (let tag = 0; tag <= 255; tag++) {
            const p = await probeProps(getOf(tag), skip);
            if (p.size) hit.push([tag, p]);
            for (const k of p) perProp[k] = (perProp[k] || 0) + 1;
        }
        for (const [k, n] of Object.entries(perProp)) if (n > CATCHALL_TAGS) { catchAll++; if (process.env.ZHC_DEBUG) process.stderr.write('兜底分支：' + d.model + ' ' + k + ' 出现在 ' + n + ' 个标签上\n'); }
        for (const [tag, p] of hit) {
            const keep = new Set([...p].filter(k => perProp[k] <= CATCHALL_TAGS));
            if (!keep.size) continue;
            for (const r of await inferProps(getOf(tag), keep, bins, d.model + ' tlv' + tag)) { rows.push(Object.assign({ cl: CL_TLV, id: tag }, r)); fields |= 1 << r.field; }
        }
    }
    /* ② 0xFCC0 的其它属性：数字键（numericAttributes2Payload 认的）与具名属性（新式 Aqara：addManuSpecificLumiCluster 给 0xFCC0 起的名字，
          herdsman 收到时按名字交给转换器，如 P1 人体的 illuminance = 0x0112）。先各喂一次看有没有动静，有才细推 */
    if (fzL.length) {
        const named = await lumiAttrNames(d);
        const keys = [];
        for (let id = 256; id <= 0x5ff; id++) keys.push([id, id]);
        for (const [name, id] of named) keys.push([name, id]);
        for (const [key, id] of keys) {
            const get = async (dt, raw) => simAttr(d, fzL, 'manuSpecificLumi', { [key]: val(dt, raw) });
            const quick = Object.assign({}, await get(DT_ENUM, 1), await get(DT_BOOL, 1));
            if (!Object.keys(quick).some(k => PROP2FIELD.has(k))) continue;
            for (const r of await inferOne(get, bins, skip, d.model + ' attr' + key)) { rows.push(Object.assign({ cl: CL_LUMI, id }, r)); fields |= 1 << r.field; }
        }
    }
    /* ③ 标准簇的改写 */
    for (const s of STD) {
        const fz = fzsOf(d, s.cluster);
        if (!fz.length) continue;
        const get = async (dt, raw) => simAttr(d, fz, s.cluster, { [s.attr]: val(dt, raw) });
        const got = await inferOne(get, bins, skip, d.model + ' ' + s.cluster, s.u8);
        /* "库把这一路丢了"要在一串宽范围的值上都不出数才算（Aqara T1 的气压只收 500–2000 hPa，只喂 0 / 1 / 100 会被误判成丢掉 —— 审计 ❌） */
        let quiet = true;
        for (const x of (s.u8 ? [0, 1, 2, 10, 30, 100, 200, 254] : [0, 1, 2, 10, 100, 250, 500, 1000, 1013, 1500, 2000, 2500, 5000, 10000, 20000, 30000])) {
            if (Object.keys(await get(DT_ENUM, x)).length) { quiet = false; break; }
        }
        if (!got.length) {
            if (quiet) rows.push({ cl: s.cl, id: s.id, field: FIELD.NONE, kind: 1, mask: 0, mul: 0, off: 0, smp: [] });   /* 库把这一路丢了：我们也丢 */
            continue;
        }
        for (const r of got) {
            fields |= 1 << r.field;
            const gen = s.gen;
            const same = r.field === gen.field && r.kind === gen.kind &&
                         (r.kind === 2 ? (r.mask & 0x03) === (gen.mask & 0x03) : r.mul === gen.mul && r.off === gen.off);
            if (!same || got.length > 1) rows.push(Object.assign({ cl: s.cl, id: s.id }, r));
        }
    }
    /* ④ IAS：小米走不完访谈、读不到 ZoneType，漏水 / 烟雾这类靠表知道 alarm1 是什么 —— 喂 Zone Status Change（alarm1 置 / 清），
          两次输出里翻了的那个报警属性就是它（只记字段位，解码照走通用 IAS） */
    const fzI = fzsOf(d, 'ssIasZone', 'commandStatusChangeNotification');
    if (fzI.length) {
        const on = await simAttr(d, fzI, 'ssIasZone', { zonestatus: 1, extendedstatus: 0 }, 'commandStatusChangeNotification');
        const off = await simAttr(d, fzI, 'ssIasZone', { zonestatus: 0, extendedstatus: 0 }, 'commandStatusChangeNotification');
        for (const k of Object.keys(on)) {
            const fi = PROP2FIELD.get(k);
            if (fi === undefined || FIELDS[fi].kind !== 'mask' || (EXPOSED.size && !EXPOSED.has(k))) continue;
            if ([FIELD.WATER, FIELD.SMOKE, FIELD.GAS, FIELD.CO, FIELD.CONTACT, FIELD.VIBRATION, FIELD.OCCUPANCY].includes(fi) && FIELDS[fi].active(on[k]) && !FIELDS[fi].active(off[k])) fields |= 1 << fi;
        }
    }
    /* 电量曲线（照抄 meta；固件同 zhc utils.batteryVoltageToPercentage） */
    let batt = { type: 0, min: 0, max: 0, voff: 0 };
    if (battMeta && typeof battMeta === 'object' && Number.isFinite(battMeta.min) && Number.isFinite(battMeta.max)) batt = { type: 1, min: battMeta.min, max: battMeta.max, voff: battMeta.vOffset || 0 };
    else if (battMeta === '3V_2100') batt = { type: 2, min: 0, max: 0, voff: 0 };
    else if (battMeta === '3V_1500_2800') batt = { type: 3, min: 0, max: 0, voff: 0 };
    if (batt.type) fields |= 1 << FIELD.BATTERY_PCT;
    const occ = (d.options || []).some(o => o && o.name === 'occupancy_timeout') ? OCC_TIMEOUT_S : 0;
    rows.sort((a, b) => a.cl - b.cl || a.id - b.id || a.field - b.field);
    const uniq = rows.filter((r, i) => !i || !(r.cl === rows[i - 1].cl && r.id === rows[i - 1].id && r.field === rows[i - 1].field));
    for (const v of [batt.min, batt.max, batt.voff]) if (v < -32768 || v > 32767) throw new Error(d.model + ' 的电量曲线参数超出 int16：' + JSON.stringify(batt));
    return { rows: uniq, fields, batt, occ };
}

(async () => {
    for (const d0 of defs) {
        if (!isLumi(d0)) continue;
        let d;
        try { d = zhc.prepareDefinition(d0); } catch (e) { continue; }
        defsSeen++;
        const keys = [];
        for (const zm of d.zigbeeModel || []) { const m = sim.devStr(zm, strStat); if (m) keys.push(m); }
        for (const f of d.fingerprint || []) { const m = sim.devStr(f.modelID, strStat); if (m) keys.push(m); }
        const todo = [...new Set(keys)].filter(m => { if (models.has(m)) { dupModel++; return false; } return true; });
        if (!todo.length) continue;
        sim.setCur('LUMI', todo[0]);
        sim.step(d.model);
        const r = await inferModel(d);
        if (!r.rows.length && !r.batt.type && !r.occ && !r.fields) { for (const m of todo) models.set(m, -1); continue; }
        const k = JSON.stringify([r.rows.map(x => [x.cl, x.id, x.field, x.kind, x.mask, x.mul, x.off]), r.fields, r.batt, r.occ]);
        let di = devIdx.get(k);
        if (di === undefined) {
            di = devs.length;
            devs.push(r);
            devIdx.set(k, di);
            if (r.batt.type) battCurves++;
            if (r.occ) occDevs++;
            /* 金样例：每条行的样例 + 电量曲线（喂标签 1 的电压，看库算出的电量 %） */
            for (const row of r.rows) for (const [x, y] of row.smp) gold.push([todo[0], row.cl, row.id, x, row.field, y]);
            if (r.batt.type) {
                const fzB = fzsOf(d, 'genBasic'), fzL = fzsOf(d, 'manuSpecificLumi');
                for (const mv of [2400, 2850, 2900, 2950, 3000, 3100, 3300, 3700, 4200]) {
                    const a = fzB.length ? await simAttr(d, fzB, 'genBasic', { 65281: { 1: mv } }) : await simAttr(d, fzL, 'manuSpecificLumi', { 247: tlvBuf(1, mv) });
                    if (typeof a.battery === 'number') gold.push([todo[0], 0, 0, mv, -1, a.battery]);   /* 字段 −1 = 电量曲线 */
                }
            }
        }
        for (const m of todo) models.set(m, di);
    }
    emit();
    sim.done();
})().catch(e => { process.stderr.write('❌ 导出失败：' + (e && e.stack || e) + '\n'); process.exit(1); });

function emit() {
    const list = [...models.entries()].filter(([, di]) => di >= 0).map(([m, di]) => ({ m, di })).sort((a, b) => (a.m < b.m ? -1 : a.m > b.m ? 1 : 0));
    const pool = new sim.StrPool();
    for (const f of list) f.mo = pool.add(f.m);
    const nRows = devs.reduce((a, d) => a + d.rows.length, 0);
    for (const dv of devs) for (const r of dv.rows) for (const k of ['cl', 'id', 'field', 'kind', 'mask', 'mul', 'off']) if (!Number.isInteger(r[k])) throw new Error('行里 ' + k + ' 不是整数：' + JSON.stringify(r));
    if (nRows > 0xffff || devs.length > 0xffff || devs.some(d => d.rows.length > 0xff)) throw new Error('表超出 zh_lumi_db.h 的字段宽度');
    const L = [];
    L.push('/* 自动生成，别手改：node tools/zhc_export/export_lumi.js（见该文件头；平常用 tools/zhc_export/regen.sh）。');
    L.push(' * 来源：zigbee-herdsman-converters ' + pkg.version + '（' + pkg.license + '），小米 / Aqara 设备：TLV（0xFF01 / 0x00F7）、0xFCC0 属性、标准簇的改写、电量曲线、人体清零。');
    L.push(' * 统计：看过小米 / Aqara 定义 ' + defsSeen + ' 个；导出型号 ' + list.length + ' 个、不同组合 ' + devs.length + ' 种共 ' + nRows + ' 行（推断不出没导的 ' + rowsBad + ' 条）。');
    L.push(' *');
    L.push(...sim.licenseLines(license));
    L.push(' */');
    L.push('#include "zh_lumi_db.h"');
    L.push('');
    L.push('const char zh_ldb_source[] = "zigbee-herdsman-converters ' + sim.esc(pkg.version) + '";');
    L.push('');
    L.push('const zh_ldb_row_t zh_ldb_rows[] = {     /* 簇（0xFFFF = TLV 标签）、id、字段、种类、掩码、mul、off */');
    for (const dv of devs) for (const r of dv.rows) L.push('    { 0x' + r.cl.toString(16).toUpperCase().padStart(4, '0') + ', ' + r.id + ', ' + r.field + ', ' + r.kind + ', 0x' + r.mask.toString(16).padStart(2, '0') + ', ' + r.mul + ', ' + r.off + ' },');
    if (!nRows) L.push('    { 0 },');
    L.push('};');
    L.push('const zh_ldb_dev_t zh_ldb_devs[] = {     /* 首行、行数、字段位、电量曲线（种类、min、max、vOffset）、人体清零秒数 */');
    let off = 0;
    for (const dv of devs) { L.push('    { ' + off + ', ' + dv.rows.length + ', 0x' + dv.fields.toString(16) + 'u, ' + dv.batt.type + ', ' + dv.batt.min + ', ' + dv.batt.max + ', ' + dv.batt.voff + ', ' + dv.occ + ' },'); off += dv.rows.length; }
    L.push('};');
    L.push('const zh_ldb_fp_t zh_ldb_fps[] = {     /* 按型号串排序：二分查找 */');
    for (const f of list) L.push('    { ' + f.mo + ', ' + f.di + ' },');
    L.push('};');
    L.push(...pool.lines('zh_ldb_str'));
    L.push('const unsigned zh_ldb_n_rows = ' + nRows + ';');
    L.push('const unsigned zh_ldb_n_devs = sizeof(zh_ldb_devs) / sizeof(zh_ldb_devs[0]);');
    L.push('const unsigned zh_ldb_n_fps = sizeof(zh_ldb_fps) / sizeof(zh_ldb_fps[0]);');
    L.push('const unsigned zh_ldb_str_len = sizeof(zh_ldb_str);');
    fs.writeFileSync(OUT, L.join('\n') + '\n');
    say(`小米 / Aqara：定义 ${defsSeen}、型号 ${list.length}（重复的 ${dupModel} 个按先出现的）、组合 ${devs.length}、行 ${nRows}（推断不出 ${rowsBad}；兜底分支丢掉的 ${catchAll} 个）、电量曲线 ${battCurves} 种、人体清零 ${occDevs} 种、字符串池 ${pool.len} B、金样例 ${gold.length} 条、3 s 没完的转换器 ${fzTimeouts} 次`);
    say('推断不出的（按属性）：' + Object.entries(badByProp).sort((a, b) => b[1] - a[1]).map(([k, v]) => k + ':' + v).join(' '));
    if (GOLD) {
        const G = ['/* 自动生成，别手改：export_lumi.js 的第三个输出。zigbee-herdsman-converters ' + pkg.version + ' 对同一批 TLV / 属性给出的期望值（目标单位），',
                   ' * zh_lumi_test.c 逐条对照固件。列：型号、簇（0xFFFF = TLV 标签；字段 −1 那行是电量曲线：原始值 = 毫伏、期望 = 电量 %）、id、原始值、字段、期望 */'];
        for (const g of gold) G.push('{ "' + sim.esc(g[0]) + '", 0x' + g[1].toString(16).toUpperCase().padStart(4, '0') + ', ' + g.slice(2).join(', ') + ' },');
        fs.writeFileSync(GOLD, G.join('\n') + '\n');
    }
}
