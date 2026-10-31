/* 自动生成，别手改：node tools/zhc_export/export_lumi.js（见该文件头；平常用 tools/zhc_export/regen.sh）。
 * 来源：zigbee-herdsman-converters 26.123.0（MIT），小米 / Aqara 设备：TLV（0xFF01 / 0x00F7）、0xFCC0 属性、标准簇的改写、电量曲线、人体清零。
 * 统计：看过小米 / Aqara 定义 186 个；导出型号 153 个、不同组合 45 种共 103 行（推断不出没导的 58 条）。
 *
 * 原库许可证全文：
 *   MIT License
 *   
 *   Copyright (c) 2018 Koen Kanters
 *   
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *   
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *   
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */
#include "zh_lumi_db.h"

const char zh_ldb_source[] = "zigbee-herdsman-converters 26.123.0";

const zh_ldb_row_t zh_ldb_rows[] = {     /* 簇（0xFFFF = TLV 标签）、id、字段、种类、掩码、mul、off */
    { 0x0001, 33, 0, 1, 0x00, 0, 0 },
    { 0xFFFF, 1, 16, 1, 0x00, 10000, 0 },
    { 0xFFFF, 100, 10, 2, 0x02, 0, 0 },
    { 0x0001, 33, 0, 1, 0x00, 0, 0 },
    { 0xFFFF, 1, 16, 1, 0x00, 10000, 0 },
    { 0xFFFF, 100, 9, 2, 0xfe, 0, 0 },
    { 0x0001, 33, 0, 1, 0x00, 0, 0 },
    { 0x0006, 0, 9, 2, 0xfe, 0, 0 },
    { 0xFCC0, 320, 14, 2, 0x02, 0, 0 },
    { 0xFFFF, 1, 16, 1, 0x00, 10000, 0 },
    { 0x0402, 0, 0, 1, 0x00, 0, 0 },
    { 0x0403, 0, 0, 1, 0x00, 0, 0 },
    { 0x0405, 0, 0, 1, 0x00, 0, 0 },
    { 0x0406, 0, 0, 1, 0x00, 0, 0 },
    { 0xFFFF, 101, 2, 1, 0x00, 3937, 0 },
    { 0xFFFF, 1, 16, 1, 0x00, 10000, 0 },
    { 0xFFFF, 100, 1, 2, 0x02, 0, 0 },
    { 0xFFFF, 1, 16, 1, 0x00, 10000, 0 },
    { 0xFFFF, 100, 5, 1, 0x00, 10000, 0 },
    { 0xFFFF, 101, 6, 1, 0x00, 10000, 0 },
    { 0xFFFF, 1, 16, 1, 0x00, 10000, 0 },
    { 0xFFFF, 100, 5, 1, 0x00, 10000, 0 },
    { 0xFFFF, 101, 6, 1, 0x00, 10000, 0 },
    { 0xFFFF, 102, 17, 1, 0x00, 1000, 0 },
    { 0xFFFF, 1, 16, 1, 0x00, 10000, 0 },
    { 0xFFFF, 100, 5, 1, 0x00, 10000, 0 },
    { 0xFFFF, 101, 6, 1, 0x00, 10000, 0 },
    { 0xFFFF, 1, 16, 1, 0x00, 10000, 0 },
    { 0x0400, 0, 7, 1, 0x00, 10000, 0 },
    { 0x0400, 0, 8, 4, 0x00, 0, 0 },
    { 0xFFFF, 1, 16, 1, 0x00, 10000, 0 },
    { 0xFFFF, 11, 7, 1, 0x00, 10000, 0 },
    { 0x0001, 33, 0, 1, 0x00, 0, 0 },
    { 0xFCC0, 274, 7, 1, 0x00, 10000, -65536 },
    { 0xFCC0, 274, 8, 4, 0x00, 0, 0 },
    { 0xFFFF, 1, 16, 1, 0x00, 10000, 0 },
    { 0xFFFF, 101, 7, 1, 0x00, 10000, 0 },
    { 0x0001, 33, 0, 1, 0x00, 0, 0 },
    { 0xFFFF, 1, 16, 1, 0x00, 10000, 0 },
    { 0xFCC0, 322, 8, 2, 0x02, 0, 0 },
    { 0xFFFF, 101, 8, 2, 0x02, 0, 0 },
    { 0xFCC0, 322, 8, 2, 0x02, 0, 0 },
    { 0x0006, 0, 9, 2, 0xfe, 0, 0 },
    { 0xFFFF, 1, 16, 1, 0x00, 10000, 0 },
    { 0x0006, 0, 9, 2, 0xfe, 0, 0 },
    { 0xFFFF, 1, 16, 1, 0x00, 10000, 0 },
    { 0xFFFF, 100, 9, 2, 0xfe, 0, 0 },
    { 0xFFFF, 1, 16, 1, 0x00, 10000, 0 },
    { 0x0400, 0, 0, 1, 0x00, 0, 0 },
    { 0x0406, 0, 0, 1, 0x00, 0, 0 },
    { 0xFFFF, 100, 1, 2, 0x02, 0, 0 },
    { 0x0406, 0, 0, 1, 0x00, 0, 0 },
    { 0xFFFF, 100, 1, 2, 0x02, 0, 0 },
    { 0xFFFF, 1, 16, 1, 0x00, 10000, 0 },
    { 0xFCC0, 313, 1, 2, 0x02, 0, 0 },
    { 0xFCC0, 314, 12, 2, 0x02, 0, 0 },
    { 0xFFFF, 100, 1, 2, 0x02, 0, 0 },
    { 0xFFFF, 160, 12, 2, 0x02, 0, 0 },
    { 0xFFFF, 164, 1, 2, 0x02, 0, 0 },
    { 0x0001, 33, 0, 1, 0x00, 0, 0 },
    { 0xFCC0, 314, 11, 2, 0x02, 0, 0 },
    { 0xFFFF, 1, 16, 1, 0x00, 10000, 0 },
    { 0xFFFF, 160, 11, 2, 0x02, 0, 0 },
    { 0xFFFF, 1, 16, 1, 0x00, 10000, 0 },
    { 0x0006, 0, 9, 2, 0x02, 0, 0 },
    { 0xFFFF, 23, 16, 1, 0x00, 10000, 0 },
    { 0xFFFF, 24, 3, 1, 0x00, 10000, 0 },
    { 0xFFFF, 100, 1, 2, 0x02, 0, 0 },
    { 0xFFFF, 101, 3, 1, 0x00, 10000, 0 },
    { 0xFCC0, 1034, 3, 1, 0x00, 10000, 0 },
    { 0xFFFF, 101, 3, 1, 0x00, 10000, 0 },
    { 0xFCC0, 1035, 16, 1, 0x00, 10000, 0 },
    { 0xFCC0, 1065, 7, 1, 0x00, 500000, 0 },
    { 0xFFFF, 1, 16, 1, 0x00, 10000, 0 },
    { 0xFFFF, 101, 3, 1, 0x00, 5000, 0 },
    { 0x0001, 32, 0, 1, 0x00, 0, 0 },
    { 0x0001, 33, 0, 1, 0x00, 0, 0 },
    { 0xFFFF, 1, 16, 1, 0x00, 10000, 0 },
    { 0x0001, 33, 0, 1, 0x00, 0, 0 },
    { 0xFFFF, 1, 16, 1, 0x00, 10000, 0 },
    { 0x0001, 33, 0, 1, 0x00, 0, 0 },
    { 0xFFFF, 1, 16, 1, 0x00, 10000, 0 },
    { 0xFFFF, 100, 7, 1, 0x00, 10000, 0 },
    { 0x0001, 33, 0, 1, 0x00, 0, 0 },
    { 0x0001, 33, 0, 1, 0x00, 0, 0 },
    { 0xFFFF, 1, 16, 1, 0x00, 10000, 0 },
    { 0xFFFF, 100, 5, 1, 0x00, 10000, 0 },
    { 0xFFFF, 101, 6, 1, 0x00, 10000, 0 },
    { 0xFCC0, 1034, 3, 1, 0x00, 10000, 0 },
    { 0xFFFF, 102, 5, 1, 0x00, 10000, 0 },
    { 0xFFFF, 105, 3, 1, 0x00, 10000, 0 },
    { 0x0006, 0, 0, 1, 0x00, 0, 0 },
    { 0x0402, 0, 0, 1, 0x00, 0, 0 },
    { 0x0403, 0, 0, 1, 0x00, 0, 0 },
    { 0x0405, 0, 0, 1, 0x00, 0, 0 },
    { 0x0406, 0, 0, 1, 0x00, 0, 0 },
    { 0x0001, 32, 0, 1, 0x00, 0, 0 },
    { 0x0001, 33, 3, 1, 0x00, 10000, 0 },
    { 0x0402, 0, 5, 1, 0x00, 10000, 0 },
    { 0xFFFF, 102, 3, 1, 0x00, 10000, 0 },
    { 0xFCC0, 322, 8, 2, 0x02, 0, 0 },
    { 0xFFFF, 24, 3, 1, 0x00, 10000, 0 },
    { 0xFCC0, 322, 8, 2, 0x02, 0, 0 },
};
const zh_ldb_dev_t zh_ldb_devs[] = {     /* 首行、行数、字段位、电量曲线（种类、min、max、vOffset）、人体清零秒数 */
    { 0, 0, 0x2u, 0, 0, 0, 0, 0 },
    { 0, 3, 0x10408u, 1, 2850, 3000, 0, 0 },
    { 3, 0, 0x60u, 0, 0, 0, 0, 0 },
    { 3, 3, 0x10208u, 1, 2850, 3000, 0, 0 },
    { 6, 4, 0x14208u, 1, 2850, 3000, 0, 0 },
    { 10, 5, 0x6u, 0, 0, 0, 0, 0 },
    { 15, 1, 0x10008u, 1, 2850, 3000, 0, 0 },
    { 16, 1, 0x2u, 0, 0, 0, 0, 0 },
    { 17, 3, 0x10068u, 1, 2850, 3000, 0, 0 },
    { 20, 4, 0x30068u, 1, 2850, 3000, 0, 0 },
    { 24, 3, 0x30068u, 1, 2850, 3000, 0, 0 },
    { 27, 1, 0x10108u, 1, 2850, 3000, 0, 90 },
    { 28, 4, 0x10188u, 1, 2850, 3000, 0, 90 },
    { 32, 5, 0x10188u, 1, 2850, 3000, 0, 90 },
    { 37, 2, 0x10108u, 1, 2850, 3000, 0, 90 },
    { 39, 2, 0x100u, 0, 0, 0, 0, 0 },
    { 41, 1, 0x100u, 0, 0, 0, 0, 0 },
    { 42, 2, 0x10208u, 1, 2850, 3000, 0, 0 },
    { 44, 3, 0x10208u, 1, 2850, 3000, 0, 0 },
    { 47, 1, 0x10408u, 1, 2850, 3000, 0, 0 },
    { 48, 3, 0x2u, 0, 0, 0, 0, 0 },
    { 51, 2, 0x2u, 0, 0, 0, 0, 0 },
    { 53, 1, 0x10808u, 1, 2850, 3000, 0, 0 },
    { 54, 0, 0x1000u, 0, 0, 0, 0, 0 },
    { 54, 5, 0x1002u, 0, 0, 0, 0, 0 },
    { 59, 4, 0x10808u, 1, 2475, 3000, 0, 0 },
    { 63, 1, 0x10000u, 0, 0, 0, 0, 0 },
    { 64, 3, 0x10208u, 0, 0, 0, 0, 0 },
    { 67, 2, 0xau, 0, 0, 0, 0, 0 },
    { 69, 2, 0x8u, 0, 0, 0, 0, 0 },
    { 71, 4, 0x10088u, 0, 0, 0, 0, 0 },
    { 75, 3, 0x10008u, 1, 3000, 4200, 0, 0 },
    { 78, 2, 0x10008u, 1, 2850, 3000, 0, 0 },
    { 80, 3, 0x10088u, 1, 2850, 3000, 0, 0 },
    { 83, 1, 0x8u, 1, 2850, 3000, 0, 0 },
    { 84, 4, 0x10068u, 1, 2850, 3000, 0, 0 },
    { 88, 3, 0x10028u, 0, 0, 0, 0, 0 },
    { 91, 1, 0x8u, 1, 2850, 3000, 0, 0 },
    { 92, 4, 0x0u, 0, 0, 0, 0, 0 },
    { 96, 2, 0xau, 0, 0, 0, 0, 0 },
    { 98, 2, 0x68u, 0, 0, 0, 0, 0 },
    { 100, 0, 0x40u, 0, 0, 0, 0, 0 },
    { 100, 2, 0x168u, 0, 0, 0, 0, 0 },
    { 102, 1, 0x160u, 0, 0, 0, 0, 0 },
    { 103, 0, 0x10068u, 0, 0, 0, 0, 0 },
};
const zh_ldb_fp_t zh_ldb_fps[] = {     /* 按型号串排序：二分查找 */
    { 0, 44 },
    { 14, 44 },
    { 27, 44 },
    { 41, 44 },
    { 54, 2 },
    { 71, 35 },
    { 93, 41 },
    { 112, 36 },
    { 131, 0 },
    { 153, 7 },
    { 170, 7 },
    { 191, 7 },
    { 205, 7 },
    { 223, 0 },
    { 242, 7 },
    { 255, 29 },
    { 275, 30 },
    { 295, 30 },
    { 315, 7 },
    { 332, 28 },
    { 352, 7 },
    { 372, 7 },
    { 392, 5 },
    { 411, 5 },
    { 430, 5 },
    { 449, 1 },
    { 467, 19 },
    { 484, 5 },
    { 502, 5 },
    { 520, 0 },
    { 538, 5 },
    { 556, 5 },
    { 574, 5 },
    { 592, 5 },
    { 610, 5 },
    { 628, 38 },
    { 646, 38 },
    { 664, 0 },
    { 682, 5 },
    { 700, 0 },
    { 718, 5 },
    { 736, 5 },
    { 754, 5 },
    { 772, 5 },
    { 790, 5 },
    { 808, 5 },
    { 826, 5 },
    { 844, 5 },
    { 862, 5 },
    { 880, 5 },
    { 898, 5 },
    { 916, 5 },
    { 934, 0 },
    { 952, 5 },
    { 970, 5 },
    { 988, 5 },
    { 1008, 5 },
    { 1028, 5 },
    { 1048, 5 },
    { 1068, 5 },
    { 1088, 0 },
    { 1106, 31 },
    { 1122, 4 },
    { 1139, 3 },
    { 1158, 17 },
    { 1176, 15 },
    { 1193, 13 },
    { 1210, 13 },
    { 1229, 13 },
    { 1247, 14 },
    { 1265, 20 },
    { 1275, 7 },
    { 1292, 21 },
    { 1306, 7 },
    { 1323, 7 },
    { 1340, 20 },
    { 1357, 20 },
    { 1374, 20 },
    { 1391, 7 },
    { 1408, 6 },
    { 1427, 6 },
    { 1446, 32 },
    { 1465, 32 },
    { 1484, 32 },
    { 1503, 6 },
    { 1525, 6 },
    { 1547, 34 },
    { 1569, 6 },
    { 1588, 6 },
    { 1608, 32 },
    { 1628, 6 },
    { 1650, 6 },
    { 1672, 6 },
    { 1694, 32 },
    { 1717, 32 },
    { 1736, 32 },
    { 1759, 32 },
    { 1782, 37 },
    { 1801, 26 },
    { 1820, 32 },
    { 1839, 33 },
    { 1858, 8 },
    { 1868, 6 },
    { 1886, 6 },
    { 1904, 6 },
    { 1926, 6 },
    { 1943, 6 },
    { 1967, 24 },
    { 1989, 8 },
    { 2004, 40 },
    { 2026, 10 },
    { 2047, 17 },
    { 2066, 18 },
    { 2089, 11 },
    { 2108, 12 },
    { 2131, 23 },
    { 2150, 43 },
    { 2174, 16 },
    { 2198, 42 },
    { 2222, 22 },
    { 2240, 25 },
    { 2264, 6 },
    { 2281, 6 },
    { 2300, 6 },
    { 2323, 6 },
    { 2346, 19 },
    { 2368, 7 },
    { 2387, 7 },
    { 2406, 7 },
    { 2425, 7 },
    { 2444, 5 },
    { 2463, 7 },
    { 2482, 7 },
    { 2503, 0 },
    { 2524, 7 },
    { 2545, 7 },
    { 2564, 7 },
    { 2585, 7 },
    { 2606, 7 },
    { 2627, 7 },
    { 2646, 7 },
    { 2665, 7 },
    { 2684, 7 },
    { 2703, 7 },
    { 2722, 7 },
    { 2741, 7 },
    { 2760, 7 },
    { 2779, 7 },
    { 2798, 39 },
    { 2816, 27 },
    { 2838, 26 },
    { 2859, 6 },
    { 2878, 9 },
};
const char zh_ldb_str[] =
    "LYWSD03MMC-bz\000LYWSD03MMC-z\000MJWSD06MMC-bz\000MJWSD06MMC-z\000lumi.airm.fhac01\000"
    "lumi.airmonitor.acn01\000lumi.airrtc.aeu001\000lumi.airrtc.agl001\000lumi.bhf_light.acn001\000"
    "lumi.ctrl_86plug\000lumi.ctrl_86plug.aq1\000lumi.ctrl_ln1\000lumi.ctrl_ln1.aq1\000lumi.ctrl_neutral1\000"
    "lumi.curtain\000lumi.curtain.acn002\000lumi.curtain.acn003\000lumi.curtain.agl001\000lumi.curtain.aq2\000"
    "lumi.curtain.hagl04\000lumi.curtain.hagl07\000lumi.curtain.vagl02\000lumi.dimmer.acn003\000"
    "lumi.dimmer.acn004\000lumi.dimmer.acn005\000lumi.flood.acn001\000lumi.flood.agl02\000lumi.light.acn003\000"
    "lumi.light.acn004\000lumi.light.acn006\000lumi.light.acn014\000lumi.light.acn023\000lumi.light.acn024\000"
    "lumi.light.acn025\000lumi.light.acn026\000lumi.light.acn031\000lumi.light.acn032\000lumi.light.acn033\000"
    "lumi.light.acn036\000lumi.light.acn037\000lumi.light.acn040\000lumi.light.acn128\000lumi.light.acn132\000"
    "lumi.light.agl001\000lumi.light.agl002\000lumi.light.agl003\000lumi.light.agl004\000lumi.light.agl005\000"
    "lumi.light.agl006\000lumi.light.agl007\000lumi.light.agl008\000lumi.light.aqcn02\000lumi.light.cbacn1\000"
    "lumi.light.cwac02\000lumi.light.cwacn1\000lumi.light.cwjwcn01\000lumi.light.cwjwcn02\000"
    "lumi.light.cwopcn01\000lumi.light.cwopcn02\000lumi.light.cwopcn03\000lumi.light.rgbac1\000lumi.lock.acn02\000"
    "lumi.magnet.ac01\000lumi.magnet.acn001\000lumi.magnet.agl02\000lumi.motion.ac01\000lumi.motion.ac02\000"
    "lumi.motion.acn001\000lumi.motion.agl02\000lumi.motion.agl04\000lumi.plug\000lumi.plug.aeu001\000"
    "lumi.plug.aq1\000lumi.plug.macn01\000lumi.plug.maeu01\000lumi.plug.maus01\000lumi.plug.mitw01\000"
    "lumi.plug.mmeu01\000lumi.plug.sacn02\000lumi.remote.acn003\000lumi.remote.acn004\000lumi.remote.acn007\000"
    "lumi.remote.acn008\000lumi.remote.acn009\000lumi.remote.b186acn01\000lumi.remote.b186acn02\000"
    "lumi.remote.b186acn03\000lumi.remote.b18ac1\000lumi.remote.b1acn01\000lumi.remote.b1acn02\000"
    "lumi.remote.b286acn01\000lumi.remote.b286acn02\000lumi.remote.b286acn03\000lumi.remote.b286opcn01\000"
    "lumi.remote.b28ac1\000lumi.remote.b486opcn01\000lumi.remote.b686opcn01\000lumi.remote.cagl02\000"
    "lumi.remote.rkba01\000lumi.sen_ill.agl01\000lumi.sen_ill.mgl01\000lumi.sens\000lumi.sensor_86sw1\000"
    "lumi.sensor_86sw2\000lumi.sensor_86sw2.es1\000lumi.sensor_cube\000lumi.sensor_cube.aqgl01\000"
    "lumi.sensor_gas.acn02\000lumi.sensor_ht\000lumi.sensor_ht.agl001\000lumi.sensor_ht.agl02\000"
    "lumi.sensor_magnet\000lumi.sensor_magnet.aq2\000lumi.sensor_motion\000lumi.sensor_motion.aq2\000"
    "lumi.sensor_natgas\000lumi.sensor_occupy.acn1\000lumi.sensor_occupy.agl1\000lumi.sensor_occupy.agl8\000"
    "lumi.sensor_smoke\000lumi.sensor_smoke.acn03\000lumi.sensor_swit\000lumi.sensor_switch\000"
    "lumi.sensor_switch.aq2\000lumi.sensor_switch.aq3\000lumi.sensor_wleak.aq1\000lumi.switch.acn029\000"
    "lumi.switch.acn048\000lumi.switch.acn056\000lumi.switch.acn061\000lumi.switch.agl007\000lumi.switch.agl011\000"
    "lumi.switch.b1lacn01\000lumi.switch.b1lacn02\000lumi.switch.b1laus01\000lumi.switch.b1lc04\000"
    "lumi.switch.b1nacn01\000lumi.switch.b1nacn02\000lumi.switch.b1naus01\000lumi.switch.b1nc01\000"
    "lumi.switch.l0acn1\000lumi.switch.l0agl1\000lumi.switch.l1acn1\000lumi.switch.l1aeu1\000lumi.switch.n0acn2\000"
    "lumi.switch.n0agl1\000lumi.switch.n1acn1\000lumi.switch.n1aeu1\000lumi.valve.agl001\000"
    "lumi.vibration.agl002\000lumi.vibration.agl01\000lumi.vibration.aq1\000lumi.weather\000"
    ;
const unsigned zh_ldb_n_rows = 103;
const unsigned zh_ldb_n_devs = sizeof(zh_ldb_devs) / sizeof(zh_ldb_devs[0]);
const unsigned zh_ldb_n_fps = sizeof(zh_ldb_fps) / sizeof(zh_ldb_fps[0]);
const unsigned zh_ldb_str_len = sizeof(zh_ldb_str);
