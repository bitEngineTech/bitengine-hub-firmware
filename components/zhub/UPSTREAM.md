# zhub 与 Silicon Labs SDK 的关系（来源登记）

zhub 的协议语义以本机 Simplicity SDK 为准；SDK 源码是 **Silicon Labs MSLA**，不拷进本仓库。

## 用到的 SDK

- Simplicity SDK **2025.6.3**（`~/src/mg24-fw/sdk/simplicity_sdk.slcs` 的 `sdk_version`；本机检出提交 `b41bec3f`），与 `mg24/` 构建 NCP 用的同一份。
- 路径可用 `ZHUB_SDK` 改；`test_host/run.sh` 开头核对 `sdk_version` 是 2025.6.3，不对就失败（别的版本行为可能不同，互通结论就不成立）。

| 文件（相对 SDK 根） | sha256 | 怎么用 |
|---|---|---|
| `protocol/zigbee/app/ezsp-host/ash/ash-host.c` | `cf63d072a2292869870f3b40a103139e3c8985a39f571c9d81f3ecb7d4112c4a` | 主机参考实现：`zh_ash_core.c` 的语义正本（不编、不拷） |
| `platform/service/legacy_common_ash/src/ash-common.c` | `b00546bc2ad05a889088d3f5c0907fb63a3de0e8f9e850ca3569bbba3d78f78e` | 测试时从本机路径编进 `libsdkncp.so`（编解码互通 + NCP 侧） |
| `platform/service/legacy_common_ash/inc/ash-protocol.h` | `7e7bd662d4305546dc0d3c8ae2b49e7528da434a55c7feeabb5dfc1b316e36ce` | 常量出处（`zh_ash_core.h` 每个常量带行号）；测试编译时引用 |
| `platform/service/legacy_ncp_ash/src/ash-ncp.c` | `dc1dd3fbedfdd2ad80ba73de96d2bc31ca13a6c88ba0c04e950d21ad6a138c4b` | 测试时从本机路径编进 `libsdkncp.so`：真 NCP 侧状态机 |
| `platform/service/legacy_ncp_ash/inc/ash-ncp.h` | `5d33a8d74ebf0780bf9c9986a79a9e963ad3094d2b51ab14304a98841b66e995` | NCP 侧参数出处；测试编译时引用 |
| `platform/service/legacy_hal/src/crc.c` | `e305212f7bf292be5edb39dcf9b6b150b55627ac2b87f5f4dc96a832f022e89a` | 测试时编进 `libsdkncp.so`：SDK 的 CRC，与我们的独立实现对照 |
| `protocol/zigbee/app/util/ezsp/ezsp.c` | `270afd697ff0bae1dab8aa1043ea25d285a53f2d6a55dc8f70f744be2d338037` | `zh_ezsp_core.c` 的语义正本（头格式、回包校验）；测试时编进 EZSP 预言机 |
| `protocol/zigbee/app/util/ezsp/command-functions.h` | `1396f043d33093fc21539972cecfbad23aaaa67fc4426dfadfd06a949a84ea87` | 每条命令的 append / fetch 顺序、回调派发；被 `ezsp.c` 包含，随它编进预言机 |
| `protocol/zigbee/app/util/ezsp/ezsp-frame-utilities.c` | `136c791d402c2b6911262a9e5468b0fb378d766e79dea4be0eeba0599d1d42c5` | 结构体序列化；测试时编进预言机 |
| `protocol/zigbee/app/util/ezsp/ezsp-frame-utilities.h` | `82343bc89591e1d3a21dd3047c9b538f4a1ad44f8b710b6d4549a074bfb08952` | 同上（`append_sl_zigbee_network_parameters_t` 是这里的宏） |
| `protocol/zigbee/app/util/ezsp/ezsp-callbacks.c` | `612e8d0088d387e43f870feab60a34c1e46acf83f700a826b8e5b15636c52dbb` | 回调的默认实现；测试时编进预言机（要截的回调在那里是 WEAK） |
| `protocol/zigbee/app/util/ezsp/ezsp-protocol.h` | `9676e822511a912e87ca22cfdf6d8aeea52fb4b8ee4b557c34094dfe2b565b77` | 帧控制位、头长度的出处（`zh_ezsp_core.h` 带行号） |
| `protocol/zigbee/app/util/ezsp/ezsp-enum.h` | `aac4254b8abfc4a444c7df18fd93f51f29d9d51fa5c3bcb1c7d64db86ad73ff5` | 帧 ID 的出处（每个 `ZH_EZSP_ID_*` 带行号，`run.sh` 逐个对着它核） |
| `protocol/zigbee/app/util/ezsp/ezsp.h`、`serial-interface.h`、`command-prototypes.h` | `8bd14e23e4f2963036c1468c92e8db0761b0ed99132f741ddb311f11ca40e880`、`aa4c81d3f131117b5145aecad61268fb7b750cc5d351405b55d5240b2531a95c`、`7dc7d9a9c4a457826cebee9b09448ebf68cbb953af5cb6da9e46a39db1035285` | 预言机实现的串口层与回调替身照它们的签名写；测试编译时引用 |
| `protocol/zigbee/app/util/ezsp/serial-interface-uart.c` | `449e7f6fff80c758283122fd8b290526b8ea1b378ff4de4427296eb20ea905d8` | 只作证据引用（设计附录 E 的 E13：主机何时写 `UART_SYNCH_CALLBACKS`），不编 |
| `protocol/zigbee/stack/include/network-formation.h` | `94f4c3103e3278c6ffb25a9df9398633102413a29f7b97f62614c88567d9acfb` | 只作证据引用（`permitJoining(0xFF)` = 永久开放，`zh_ezsp_core.h` 带行号；`networkInit` 回 `NOT_JOINED` 的语义，`zh_host_core.h`），不编 |
| `platform/common/inc/sl_status.h` | `3cfc83987b05f3f12aec3180bb1df82dee44bacbafe317aec62b7049b6bdf5bb` | 只作证据引用（`zh_host_core.h` 的 `ZH_SL_*` 带行号，`run.sh` 逐个对着它核），不编 |
| `protocol/zigbee/stack/include/sl_zigbee_types.h` | `e4d50099b987be0e1cab5be123536572dd2786f33240bee480855cd7edf78111` | 只作证据引用（`networkInit` 位图，`zh_host_core.h` 带行号、`run.sh` 核），不编 |
| `protocol/zigbee/app/framework/util/af-common.c` | `66f3c38e5a63283de934d5a3e3e5ee4bcaddfd772237061e81b435200ad0f58b` | 只作证据引用（哪些 stackStatus 算网络起来了，`zh_host_core.h` 带行号），不编 |

## 本仓库里的东西与 SDK 的关系

- **`zh_ash_core.c` / `zh_ash_core.h`**：按 `ash-host.c` 的结构逐段对照写成的（标志位、帧号变量、收帧与发送优先级的分支顺序都与它对应，
  这是有意的：语义正本就是它，逐段对得上才好审）。代码是重写的，没有复制粘贴，但**结构上是衍生的**。
  私有仓库阶段没有分发问题；**开源前**要么由法务确认 MSLA 是否允许（MSLA 的源码条款通常限定与 Silicon Labs 器件配合使用、以目标码分发），
  要么依公开规范 UG101（ASH 协议说明）重写 —— 记在开源前敏感内容清单里。
- **`zh_ezsp_core.c` / `zh_ezsp_core.h`**：字段顺序、帧控制位、头格式是**协议事实**（EZSP Reference Guide UG100 公开写着），代码自写，
  不按 `ezsp.c` 的结构走（它是全局读写指针 + 每条命令一个函数，我们是读写器 + 严格长度），常量带 SDK 行号只为可核对。
  回包校验有意比 `ezsp.c` 严的几类写在头文件开头，预言机把能复现的逐格单独断言。
- **`zh_ezsp_ses.c` / `zh_ezsp_ses.h`**：自写的异步会话状态机（`ezsp.c` 是同步的 sendCommand 死等，结构完全不同）；只引用 SDK 的行号作超时推导的出处
  （`ash-common.c:238-239`、`ash-ncp.c:404-406 / 483-490`、`ash-ncp.h:237-242`，都已在上表登记的文件里）。
- **`zh_host_core.c` / `zh_host_core.h`、`zhub.c`**：自写的编排层与任务外壳；只引用 SDK 的行号作状态码 / 位图 / "哪些状态算网络起来了"的出处（上表）。
- **`test_host/sdk_interop/ses_interop.c`、`host_interop.c`**：复用同一个 `libsdkncp.so`（上表的 ASH 三个文件 + `ncp_stub.c`）；EZSP 应用层替身是自写的。
  `ncp_stub.c` 的"同一块缓冲重复入队就报错"是我们加的检测，不是 SDK 行为。
- **`test_host/sdk_interop/ezsp_stub/`**：SDK 编 `ezsp.c` 时要的三个生成头（`hal/hal.h`、`zap-type.h`、`sl_component_catalog.h`）的替身，
  各 1–3 行声明，没有 SDK 代码。`ezsp_oracle.c` 只按 SDK 的函数名与签名实现串口层和回调（截参数），同上。
- **`test_host/sdk_interop/stub/` 与 `ncp_stub.c`**：只按 SDK 的函数名与签名重新声明接口（约 30 行）、自写缓冲 / 队列 / 串口 / 计时的替身语义，没有 SDK 代码。
- **`test_host/run.sh`**：构建时从本机 SDK 路径编 ASH 的三个 `.c` 和 EZSP 的三个 `.c`，产物在临时目录、跑完就删。开源仓库里这组互通测试要改成可选（没有 SDK 时跳过并提示），
  私有仓库里找不到 SDK 就失败。

## 社区设备库：zigbee-herdsman-converters（M5 第八步）

涂鸦私有簇 0xEF00 的数据点表**不自己写**，从 Zigbee2MQTT 用的社区设备库导出：

- **zigbee-herdsman-converters 26.123.0**（npm；依赖 zigbee-herdsman 11.1.1），**MIT** 许可，Copyright (c) 2018 Koen Kanters。
- **只导数据、不拷代码**：`tools/zhc_export/export_tuya.js` 在电脑上加载这个包，对每条涂鸦设备指纹、每个数据点喂报文、看它的转换器输出什么，
  推出"数据点 → 字段 + 换算"写成 C 表 `zh_tuya_db.c`（生成物提交进仓库，编固件不需要 node）。表里是事实性的映射数据；按 MIT 的要求，
  生成物文件头带原库许可证全文与版权声明。
- 同一次导出还写 `test_host/zh_tuya_gold.inc`：同一批报文喂库得到的期望值，`test_host/zh_tuya_test.c` 逐条对照固件的解码与换算。
- 写方向（M5 第九步）：同样不读代码，用库的 toZigbee 转换器喂 `state` / `brightness`，假端点截获它发出的 0xEF00 命令，推出写表 `zh_tdb_ctls`；
  写方向金样例在 `test_host/zh_tuya_gold_ctl.inc`。
- 小米 / Aqara（M5 第十步）：`tools/zhc_export/export_lumi.js` 喂 TLV / 0xFCC0 属性 / 标准簇 / IAS 报警，生成 `zh_lumi_db.c` 与金样例 `test_host/zh_lumi_gold.inc`
  （两个导出器共用 `tools/zhc_export/zhc_sim.js`）。电量曲线的参数照抄定义里的 `meta.battery.voltageToPercentage`（数据，不是代码）。
  TLV 格式的出处在 `include/zh_lumi.h`（herdsman foundation.js:45、buffaloZcl.js:642；zhc lib/lumi.js buffer2DataObject）。
- 重新生成 / 核对：`bash tools/zhc_export/regen.sh`（装 `ZHC_VERSION` 那一版到临时目录再生成）、`bash tools/zhc_export/regen.sh --check`（逐字节核对）；
  已经装好的包用 `ZHC_PKG=<包目录>` 传进去，不联网。升级版本：改 `regen.sh` 的 `ZHC_VERSION`，重跑，看打印的统计与 `git diff`，跑 `test_host/run.sh` 与 `VERIFY.sh`。
- 线上格式的出处（`include/zh_tuya.h` 带行号）：zigbee-herdsman `dist/zspec/zcl/definition/cluster.js`（命令表）、`dist/zspec/zcl/buffaloZcl.js`（数据点列表）；
  zigbee-herdsman-converters `dist/lib/tuya.js`（类型、getDataValue、configureMagicPacket / configureQuery）。
- 开源时：表与金样例可以照常随代码发布（MIT 只要求保留声明，已在文件头）。
