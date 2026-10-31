/* tools/zhc_export/zhc_sim.js —— 社区设备库导出器（export_tuya.js / export_lumi.js）共用的"喂报文看输出"那一套。
 *
 * 都不读转换器的代码：在电脑上加载 zigbee-herdsman-converters，造假设备 / 假端点 / 假报文喂它的 fromZigbee / toZigbee，看输出，
 * 推出"数据 → zhub 认得的字段 + 换算"。字段（FIELDS）与 components/zhub/include/zh_tuya_db.h 的 ZH_TF_* 一一对应（顺序 = 编号，改了两边一起改；
 * zhub/VERIFY.sh 对一遍）。线性 / 开关两种换算的推法见 linear / mask 上面的注释。 */
'use strict';
const fs = require('fs');
const path = require('path');

/* 字段：与 components/zhub/include/zh_tuya_db.h 的 ZH_TF_* 一一对应（顺序 = 编号，改了两边一起改） */
const FIELDS = [
    null,                                                                   /* 0 不用 */
    { name: 'ONOFF',       kind: 'mask', props: ['state'],                                  active: y => y === true || y === 'ON' },
    { name: 'LEVEL',       kind: 'lin',  props: ['brightness'],             unit: 100 / 254 },   /* z2m 亮度 0–254 → 0–100 % */
    { name: 'BATTERY_PCT', kind: 'lin',  props: ['battery'],                unit: 1 },
    { name: 'BATTERY_LOW', kind: 'mask', props: ['battery_low', 'battery_state'],               active: y => y === true || String(y).toLowerCase() === 'low' },
    { name: 'TEMP',        kind: 'lin',  props: ['temperature', 'local_temperature'], unit: 100 },   /* °C → 0.01 °C */
    { name: 'HUMIDITY',    kind: 'lin',  props: ['humidity'],               unit: 100 },   /* % → 0.01 % */
    { name: 'LUX',         kind: 'lin',  props: ['illuminance', 'illuminance_lux'], unit: 1 },
    { name: 'OCCUPANCY',   kind: 'mask', props: ['occupancy', 'presence', 'presence_state'],
      active: y => y === true || ['presence', 'present', 'occupied', 'motion', 'people', 'presence_state', 'move', 'moving', 'small_move', 'large_move', 'detected', 'true'].includes(String(y).toLowerCase()) },
    { name: 'CONTACT',     kind: 'mask', props: ['contact'],                                active: y => y === false },   /* z2m：contact = true 是关着；我们记"开着" */
    { name: 'WATER',       kind: 'mask', props: ['water_leak'],                             active: y => y === true },
    { name: 'SMOKE',       kind: 'mask', props: ['smoke'],                                  active: y => y === true },
    { name: 'GAS',         kind: 'mask', props: ['gas'],                                    active: y => y === true },
    { name: 'CO',          kind: 'mask', props: ['carbon_monoxide'],                        active: y => y === true },
    { name: 'TAMPER',      kind: 'mask', props: ['tamper'],                                 active: y => y === true },
    { name: 'VIBRATION',   kind: 'mask', props: ['vibration'],                              active: y => y === true },
    { name: 'VOLTAGE',     kind: 'lin',  props: ['voltage'],                unit: 1 },     /* 电池电压 mV（只给电池设备：插座的 voltage 是市电伏特，导出器自己挡） */
    { name: 'PRESSURE',    kind: 'lin',  props: ['pressure'],               unit: 10 },    /* hPa → 0.1 hPa */
];
/* 属性名 → 字段编号；skip 里的字段不收（各导出器自己定：例如涂鸦插座的 voltage 是市电伏特，不是电池毫伏） */
function prop2field(skip) {
    const m = new Map();
    FIELDS.forEach((f, i) => f && !(skip || []).includes(f.name) && f.props.forEach(p => m.set(p, i)));
    return m;
}
const FIELD = Object.fromEntries(FIELDS.map((f, i) => [f ? f.name : 'NONE', i]));

/* ---- 库的加载与噪声 ---- */
let UNEXPECTED = false;
let ZHC = null;
function init(PKG, unexpectedRe) {
    const zhc = require(PKG);
    ZHC = zhc;
    /* 库自己说"这个数据点 / 属性不认识"的那次输出不算（unexpectedRe 由导出器给） */
    const note = m => { if (unexpectedRe && unexpectedRe.test(String(m))) UNEXPECTED = true; };
    zhc.setLogger({ debug: note, info: note, warning: note, error: note });
    /* 有的转换器直接 console.log、有的顺手异步发命令（假端点上的 command 不做事，漏掉的 Promise 拒绝也吞掉）；导出器自己的输出走 say()。
       ⚠️ 全局吞 unhandledRejection 会把导出器主流程的错也吞掉 —— 主流程必须自己 .catch 后非零退出 */
    const say = console.log.bind(console);
    for (const k of ['log', 'info', 'warn', 'debug', 'error']) console[k] = () => {};
    process.on('unhandledRejection', () => {});
    const pkg = JSON.parse(fs.readFileSync(path.join(PKG, 'package.json'), 'utf8'));
    const license = fs.readFileSync(path.join(PKG, 'LICENSE'), 'utf8').trim();
    return { zhc, pkg, license, say };
}
/* 主流程没跑完事件循环就空了 = 某个转换器 await 了一个永远不 resolve 的 Promise：node 会静默以 0 退出、文件一个都不写（10-23 栽过）。
   导出器开头调 guardExit()，跑完调 done()；beforeExit 时还没 done 就报卡在哪一步、非零退出 */
let DONE = false, STEP = '';
function guardExit() {
    process.on('beforeExit', () => {
        if (DONE) return;
        process.stderr.write('❌ 导出没跑完事件循环就空了（有转换器在等一个永远不来的 Promise）；最后一步：' + STEP + '\n');
        process.exit(1);
    });
}
function step(s) { STEP = s; }
function done() { DONE = true; }
/* 跑一个可能挂住的转换器调用：超过 ms 没完就当没输出（库里也有 await sleep 的，给足时间） */
function withTimeout(p, ms) {
    let t;
    return Promise.race([Promise.resolve(p), new Promise(res => { t = setTimeout(() => res(TIMEOUT), ms); })]).finally(() => clearTimeout(t));
}
const TIMEOUT = Symbol('timeout');
function unexpected() { return UNEXPECTED; }
function clearUnexpected() { UNEXPECTED = false; }

function loadDefs(PKG) {
    const dir = path.join(PKG, 'dist/devices');
    const defs = [];
    for (const f of fs.readdirSync(dir).filter(f => f.endsWith('.js')).sort()) {
        let m;
        try { m = require(path.join(dir, f)); } catch (e) { continue; }
        if (Array.isArray(m.definitions)) defs.push(...m.definitions);
    }
    return defs;
}

/* ---- 假设备 ---- */
let CUR = { manufacturerName: '', modelID: '' };   /* 正在推断的那条指纹（转换器会按厂商名 / 型号分支） */
function setCur(manufacturerName, modelID) { CUR = { manufacturerName, modelID }; }
/* 类名要叫 Device / Endpoint：库的 globalStore 按 constructor.name 认实体（lib/utils.js isDevice / isEndpoint），普通对象会抛 "Invalid entity type"
   —— 小米人体的 occupancy_with_timeout、照度转换器都靠它存状态 */
class Device {}
class Endpoint {}
function fakeDevice() {
    /* 每次新造设备先清库的 globalStore：同一个 ieeeAddr 的状态会在探测之间 / 定义之间串（审计 ⚠️：结果依赖顺序，--check 照样逐字节通过） */
    if (ZHC && typeof ZHC.clearGlobalStore === 'function') ZHC.clearGlobalStore();
    const dev = Object.assign(new Device(), { ieeeAddr: '0x00124b0000000001', manufacturerName: CUR.manufacturerName, modelID: CUR.modelID, meta: {}, endpoints: [] });
    const nop = async () => {};
    const ep = Object.assign(new Endpoint(), { ID: 1, deviceIeeeAddress: dev.ieeeAddr, getDevice: () => dev, clusters: {}, inputClusters: [], outputClusters: [],
                 command: nop, read: nop, write: nop, bind: nop, configureReporting: nop, defaultResponse: nop });
    dev.endpoints.push(ep);
    dev.getEndpoint = id => (id === 1 ? ep : undefined);
    return dev;
}

/* 开关类的"激活"优先按定义自己的 exposes 判：binary 的 value_on（z2m 的约定里 contact 的 value_on = false 即门开，其余 value_on = 开着 / 告警），
 * 找不到（enum 类、动态 exposes 调不起来）才用 FIELDS 里的词表 */
function binaries(d) {
    let ex = d.exposes;
    if (typeof ex === 'function') {
        try { ex = ex(fakeDevice(), {}); } catch (e) { ex = []; }
    }
    const out = new Map();
    const walk = a => {
        for (const e of a || []) {
            if (!e) continue;
            if (e.type === 'binary' && typeof e.property === 'string' && !out.has(e.property)) out.set(e.property, e);
            if (Array.isArray(e.features)) walk(e.features);
        }
    };
    walk(Array.isArray(ex) ? ex : []);
    return out;
}

/* 数值类：y = k·x + b。按样例组依次试，第一组落在同一条直线上的算数（最小二乘拟合）。sets / probe 不给就用下面的默认（0–1000 的原始值）；
 * 取值范围小的属性（uint8 的电压、电量）由调用方给自己的样例组 —— 库的转换器对越界值不出数（genPowerCfg 只收电压 < 255）：
 *  - 输出全是整数的（库里先换算再取整的，如亮度 0–1000 → 0–254），容差 0.51；否则只容浮点误差；
 *  - 先试宽的样例组（斜率准），带 0 的不行再试不带 0 的（有的转换器把下限钳在 10），最后试小范围（钳在 100 的电量）。
 * 再喂 0x0000FFF6：落在直线的 x = −10 附近（老式转换器 value > 0x2000 ? value − 0xffff : value 差 1 也算）= 16 位补码，记 w16。 */
const LIN_SETS = [[0, 250, 500, 750, 1000], [100, 250, 500, 750, 1000], [0, 10, 20, 50, 100]];
const LIN_PROBE = [0, 1, 2, 5, 10, 20, 37, 50, 61, 99, 100, 101, 150, 215, 250, 333, 500, 750, 999, 1000];
function linear(get, unit, sets, probe) {
    const LIN_PROBE_ = probe || LIN_PROBE;
    for (const xs of sets || LIN_SETS) {
        const ys = xs.map(get);
        if (!ys.every(y => typeof y === 'number' && Number.isFinite(y))) continue;
        const n = xs.length, sx = xs.reduce((a, x) => a + x, 0), sy = ys.reduce((a, y) => a + y, 0);
        const sxx = xs.reduce((a, x) => a + x * x, 0), sxy = xs.reduce((a, x, i) => a + x * ys[i], 0);
        const k = (n * sxy - sx * sy) / (n * sxx - sx * sx), b = (sy - k * sx) / n;
        const rounded = ys.every(Number.isInteger);
        const ok = (x, y) => Math.abs(k * x + b - y) <= (rounded ? 0.51 : 1e-6 * Math.max(1, Math.abs(y)));
        if (k === 0 || !xs.every((x, i) => ok(x, ys[i]))) continue;
        /* 再拿一串探测点验：样例范围内必须都在直线上；范围外的要么也在直线上，要么等于那一侧最外的值（钳住了，如电量封在 100）。
           分段的转换器（AE-720K 的温度 v > 60 ? v / 10 : v，新旧固件两种报法）在这里被拒 —— 宁可不导，别把 26 °C 解成 2.6 °C */
        const lo = xs[0], hi = xs[xs.length - 1], yLo = get(LIN_PROBE_[0]), yHi = get(LIN_PROBE_[LIN_PROBE_.length - 1]);
        if (!LIN_PROBE_.every(x => {
            const y = get(x);
            if (typeof y !== 'number' || !Number.isFinite(y)) return false;
            if (Math.abs(k * x + b - y) <= (rounded ? 1.01 : 1e-6 * Math.max(1, Math.abs(y)))) return true;   /* 取整的：直线本身带半格误差，再加取整半格 */
            return (x < lo && y === yLo) || (x > hi && y === yHi);
        })) continue;
        const mul = Math.round(k * unit * 10000), off = Math.round(b * unit);
        if (mul === 0 || Math.abs(mul) > 0x7fffffff || Math.abs(off) > 0x7fffffff) continue;
        const yw = get(0xfff6);
        const w16 = typeof yw === 'number' && !ok(0xfff6, yw) && Math.abs(k * -10 + b - yw) <= Math.abs(k) * 1.01 + (rounded ? 0.51 : 1e-6);
        return { mul, off, w16 };
    }
    return null;
}

/* 开关类：原始值 0–7 各算不算"激活"。get(DT_ENUM, i) 按数值喂；全是同一个结果、get(DT_BOOL, 0 / 1) 按布尔喂却分得开就用布尔。
 * onlyActive：只有激活值有输出、别的值什么都不吐的也认（小米人体只报"有人"，没人靠超时）—— 没输出的算不激活 */
const DT_BOOL = 1, DT_NUM = 2, DT_ENUM = 4;
function mask(get, active, onlyActive) {
    let m = 0;
    const seen = new Set();
    for (let i = 0; i < 8; i++) {
        const y = get(DT_ENUM, i);
        if (y === undefined) continue;
        seen.add(active(y));
        if (active(y)) m |= 1 << i;
    }
    if (seen.size >= 2) return { m, dt: DT_ENUM };
    if (onlyActive && seen.size === 1 && seen.has(true)) return m === 0xff ? { m, dt: DT_ENUM, any: true } : { m, dt: DT_ENUM };   /* any：值多少都激活，调用方再验大值 */
    const yf = get(DT_BOOL, 0), yt = get(DT_BOOL, 1);
    if (yf !== undefined && yt !== undefined && active(yf) !== active(yt)) return { m: (active(yf) ? 1 : 0) | (active(yt) ? 2 : 0), dt: DT_BOOL };
    return null;
}

/* 指纹里的字符串按固件读回的样子比：zh_dev.c 的 copy_str 遇到 0 字节截断、最多 23 字节（有的指纹真带 \u0000，如 _TYST11_8daqwrsj 的型号）。
 * 截完不是可打印 ASCII 的不导（C 的 strcmp 与 JS 的排序只在 ASCII 上一致，二分查找靠这个）；null = 不导。stat.skipped 计数 */
function devStr(s, stat) {
    if (typeof s !== 'string') return null;
    const z = s.indexOf('\0');
    const t = z >= 0 ? s.slice(0, z) : s;
    if (!t.length || t.length > 23 || !/^[\x20-\x7e]*$/.test(t)) { if (t.length && stat) stat.skipped++; return null; }
    return t;
}

/* ---- 写 C ---- */
const esc = s => s.replace(/\\/g, '\\\\').replace(/"/g, '\\"').replace(/[^\x20-\x7e]/g, c => '\\x' + c.charCodeAt(0).toString(16).padStart(2, '0') + '""');
/* 字符串池：去重；偏移 u16 */
class StrPool {
    constructor() { this.list = []; this.idx = new Map(); this.len = 0; }
    add(s) {
        if (this.idx.has(s)) return this.idx.get(s);
        const off = this.len;
        this.list.push(s);
        this.idx.set(s, off);
        this.len += Buffer.byteLength(s, 'latin1') + 1;
        if (this.len > 0xffff) throw new Error('字符串池超过 64 KB');
        return off;
    }
    /* C 初始化串：三位八进制 \000（\0 后面紧跟数字会被编译器并进同一个八进制转义） */
    lines(name) {
        const L = ['const char ' + name + '[] ='];
        let cur = '';
        for (const s of this.list) {
            const piece = esc(s) + '\\000';
            if ((cur + piece).length > 110) { L.push('    "' + cur + '"'); cur = ''; }
            cur += piece;
        }
        if (cur) L.push('    "' + cur + '"');
        L.push('    ;');
        return L;
    }
}
function licenseLines(license) {
    return [' * 原库许可证全文：'].concat(license.split('\n').map(line => ' *   ' + line.replace(/\*\//g, '* /')));
}

module.exports = { FIELDS, FIELD, prop2field, init, guardExit, step, done, withTimeout, TIMEOUT, unexpected, clearUnexpected, loadDefs, setCur, fakeDevice, binaries,
                   LIN_SETS, LIN_PROBE, linear, DT_BOOL, DT_NUM, DT_ENUM, mask, devStr, esc, StrPool, licenseLines };
