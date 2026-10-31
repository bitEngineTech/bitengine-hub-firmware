# matter_ctrl 交叉需求清单（CLAUDE.md P2，2026-10-23）

> 这是**索引**，不是正本：每一行只写"要做什么 + 正本在哪一节"，具体数值一律看出处（CLAUDE.md 7.6：抄过来的数值迟早和正本对不上）。
> 引用设计文档用**节号 / 表名 / 条号**不用行号（行号一改就错位）。做完的在"状态"列标上提交号；进度的正本仍是设计第十七节的 M7 行（BLE 配网是（M8）那一行）。
> 由扫 `docs/00`、`docs/01`、`docs/15` 三份、CLAUDE.md 与现有代码（`hub_core`、`muse_link`、`thread_br/srp_readvertise.c`、分区表、`sdkconfig.defaults.direct`、`main/Kconfig.projbuild`）生成；
> 全项目的交叉清单是 `docs/15_muse_direct_cross_requirements.md`（A-xx 指它）。缩写：设计 = `docs/15_muse_direct_design_v1.md`，架构 = `docs/15_muse_direct_architecture_v1.md`。
> "G0-5 已核（…）"= 做法在 G0-5 原型上跑通过；原型没入库（设计 9.8b"G0-5 结论"），正式组件里要照做一遍。本表的 MCxx 是清单编号，不是测试用例 TC-MCxx。`components/matter_bridge/` 是 HA 型的 Zigbee-to-Matter 空壳，与本组件无关（架构 5.7、A-55）。

## 需求

| # | 要做什么 | 正本 | 层 | 里程碑 | 状态 |
|---|---|---|---|---|---|
| | **构建与预算** | | | | |
| MC01 | Kconfig `BITENGINE_ENABLE_MATTER_CTRL`、`BITENGINE_MATTER_MAX_NODES` 进直连型菜单，最终都 `depends on` DIRECT；直连型的取值写进 `sdkconfig.defaults.direct` | 设计 2.1、14.2「产品配置」；架构 5.8 R-1 | 构建 | M7 | ✅ M7 第一步（`main/Kconfig.projbuild`；依赖照 2.1 含 MUSE_LINK，去不去见 MX19） |
| MC02 | 引入 esp-matter：版本钉死、按条件依赖只在直连型里解算与链接；9.2 表的选项与自己的 `CHIPProjectConfig.h` 限额在正式构建里逐条核（E16） | 设计 9.2「版本」「关键配置」；附录 E16；`main/idf_component.yml` 头注 | 构建 | M7 | ✅ M7 第一步（`idf_component.yml` 钉 ==1.6.0~1、按直连型条件依赖；直连型锁文件 `dependencies.direct.lock`） |
| MC03 | 构建期补丁的落地：改上游源文件的照 `tools/patch_mdns.py` 的做法（幂等、扛 fullclean、diff 在 git 里、有自测）；在本组件 CMake 里做的留出处注释；每处都有判据钉着、只影响直连型 | 设计 9.8a 第 4–7、9 条，9.8b 第 11 条；`tools/patch_mdns.py` 头注 | 构建 | M7 | ✅ M7 第一步（`tools/patch_esp_matter.py`：原文匹配、幂等、`--check` / `--self-test`；顶层 CMake 在 MATTER_CTRL 下调） |
| MC04 | HA 型产物里没有 `libmatter_ctrl.a`、`libespressif__esp_matter.a`；HA 型不下载 esp-matter，`sdkconfig` 与依赖锁文件不随直连型变 | 架构 5.8 R-3；设计 2.7「检查脚本」 | 构建 | M7 | ✅ M7 第一步（HA 型逐归档字节数不变、sdkconfig 不变、`dependencies.lock` 只变清单哈希；`check_profiles.py` 的 HA 禁止归档原本就列了这两个） |
| MC05 | esp-matter 的 OTA Requestor（第三条 app 写入路径，在 managed_components 里，`check_profile_rules.py` 扫不到）：关掉并加判据，或补 profile 比对再登记 | 设计 2.7「还没做」；架构 5.8 R-9 | 构建 | M7 | ✅ M7 第一步（不带 server 时没编进来；`check_profiles.py` 按产物钉 OTA Requestor / BDX 下载器 / 增量与加密 OTA 库 0 字节，自测三条变异） |
| MC06 | 网页上传 app 的大小上限从写死改成按 OTA 分区大小判（镜像超限之前）；这是 OTA 路径（红线 1），先跑主机侧测试，HA 型行为不变（R-5） | 设计 2.7「还没做」；CLAUDE.md 红线 1 | 构建 | M7 | ✅ M7 第一步（`rest_api.c` 直连型分支按 `update_part->size`、HA 型照旧 3 MB；`frontend/direct/js/ota_slot.js`；上板验了超槽拒、3.3 MB 过大小检查） |
| MC07 | 预算：app 体积与槽余量（每次构建由 `check_profiles.py` 输出）；CHIP 任务栈必须在内部（它写 KVS），其余分配与静态池进 PSRAM；N2 四项硬停线、配网峰值 `alloc_failures` 为 0 | 设计 9.2「任务」、9.8 表；架构 N2、MD-16、4.3 | 内存 | M7 | ⏳（G0-5 三条判据已过，正式代码重量；体积见 MX20） |
| | **启动与存储** | | | | |
| MC08 | 启动：安全模式出口之后、muse_link 之前起，不卡等网；开机没插网线、过一会儿再插也能发现节点（平台 DNS-SD 靠 IP 事件，参照 SNTP 那次） | 设计 2.6 #8 / #13 与"直连型的等网"、十五「安全模式」 | 启动 | M7 | ◐ M7 第一步：启动序列第 13 步（zhub 之后、muse_link 之前，VERIFY 钉次序），不等网、不依赖 thread_br 在跑；"开机没插网线再插"⏳ |
| MC09 | 崩溃循环兜底（本板没有 BOOT 键）：原型的"晚起 + RTC 计数"转不转正；OTA 后首启的 mark valid 不看 matter_ctrl | 设计 9.8a 开头、2.6"直连型的等网"；附录 E33；CLAUDE.md 红线 1 | 启动 | M7 | ✅ M7 第一步按 ② 做：晚起 30 s + RTC 计数，只认崩溃类复位（`wd_boot_reason_get`）；诊断固件两次 panic → held、正常重启后清零（审计提的"正常重启也累计"已修） |
| MC10 | `matter` 分区：谁来初始化（E28）；初始化失败 / 损坏只擦它、状态置位、GATEWAY 事件、网页提示；永不碰主 `nvs` 与 `hubdb`；原型残留怎么处理 | 设计 2.5 规则 5、2.6 #1、9.2「KVS 放哪」；附录 E28；A-53 | 存储 | M7 | ◐ M7 第一、二步：本组件先初始化 / 坏了只擦 matter 分区（E28 关）；第二步起 matter 分区按 hubdb 里的身份绑定，**擦它不丢设备**（控制器用同一个根重签自己的 NOC），第一步审计那条"有设备时别擦"随之不再成立；GATEWAY 事件 / 网页提示 ⏳ |
| MC11 | `hubdb/mctrl` 只经 hub_store 写；`mctrl/nodes` 首字节格式版本、逐版本迁移、读到更新的版本不覆盖写 | 设计 2.5 规则 1 / 4、9.3；A-53 | 存储 | M7 | ⏳ |
| MC12 | 根 CA 私钥、`next_node` 落盘并读回确认之后才拿来签发 / 分配（防抖窗口里断电 = 设备拿着已丢的根、节点号被重用） | 设计 9.3、2.5 规则 1 / 3；附录 E33 的"读回确认"做法 | 存储 | M7 | ✅ M7 第二、三步：身份与节点号都落盘读回确认后才用（身份签证书、节点号去 PairDevice）；VERIFY 钉次序 |
| | **凭据** | | | | |
| MC13 | 自有 fabric：自签 RCAC、不用 ICAC、控制器自己的 NOC、新设备节点号递增、随机 fabric ID —— 要换掉 esp-matter 默认的 example issuer | 设计 9.3；架构 MD-7 | 凭据 | M7 | ✅ M7 第二步（`mc_issuer.cpp`：自签根、无 ICAC、根签 NOC、随机 fabric ID；上板控制器起来 = CHIP 校验过这条链；设备那一侧随第三步） |
| MC14 | Vendor ID 取值（E19） | 设计 9.3；附录 E19 | 凭据 | M7 | ◐ M7 第二步：开发期 0xFFF1（`CONFIG_ESP_MATTER_CONTROLLER_VENDOR_ID=65521`）；发版前定（TODO T9.52） |
| MC15 | IPK 与组数据：esp-matter 写入 CHIP 测试用的默认 IPK 与测试组数据，9.3、十三都没提 | 设计 9.1（不做组）、9.3、十三 | 凭据 | M7 | ✅ M7 第二步（IPK 随机、发给设备与控制器这一侧同一个；补丁第 3 条去掉测试组数据与默认 IPK） |
| | **发现与配网** | | | | |
| MC16 | 配网入口：Muse `hub.matter.commission` 与本地 `/api/v1/hub/invoke` 共用一个 handler；异步作业（REST 回 job、`/hub/job` 轮询），截止早于 VM 超时；限速类别已在注册表里 | 设计 9.4「入口」、6.3、6.5、6.6；附录 A | 配网 | M7 | ◐ M7 第三步：Muse 与本地 `/api/v1/hub/invoke` 同一个处理函数；作业（REST 202 + `/hub/job`，Muse 结论晚到发 link.result）；截止 165 s < 180 s。Muse 出口没上板 |
| MC17 | 配网码解析：`MT:` QR 与手动码，解出 discriminator / passcode / VID·PID / 发现方式；只支持 BLE 的码直接回能照做的句子 | 设计 9.4「配网码解析」；附录 A commission 说明 | 配网 | M7 | ✅ M7 第三步（`mc_code_core.c` 规整 + CHIP 解析器验校验和；只能蓝牙的码当场拒；主机测试） |
| MC18 | 在网配网（9.4 B）：发现 `_matterc` → 经 IP 的 PASE → 认证 → CSR → AddNOC → CASE → CommissioningComplete，不下发 Thread 网络；完成后写节点表、建能力与订阅、`DEVICE_ADDED` + LIFECYCLE 推送、name / room 进 hub_meta | 设计 9.4 B、9.4 A 图末段、6.7；架构 5.4 ⑥ | 配网 | M7 | ◐ M7 第三步：流程走通（上板两台），节点表、name / room 进 hub_meta、LIFECYCLE 事件进环；能力 / 订阅 / `DEVICE_ADDED` 推送随第四、五步 |
| MC19 | 待添加的设备从哪来：本机 SRP 代播（thread_inventory 的 `ready_to_add`，不另加 `matter.commissionables` 命令）与局域网 mDNS；设备在别人的 Thread 网络上时靠那台 BR 的路由（E20） | 设计 9.4 B 第 1 / 4 条、7.3；附录 E20 | 发现 | M7 | ◐ M7 第三步：等本机 SRP 代播的 `_matterc`（discriminator 比对）最多 15 s，等不到交给 CHIP 在网络上浏览；`ready_to_add` 与 mt- 的合并随第四步 |
| MC20 | 运营节点解析（CASE 前把 `<cfid>-<nodeid>` 解析成地址）；若保留 9.2 的"SRP 直查"，thread_inventory 要补服务端口与按实例名解析的接口 | 设计 9.2 集成点 2、9.8b「集成点 2」、第十七节 M3"第六步第二轮审计留给后续的"；附录 E17 | 发现 | M7 | ◐ M7 第三步：CHIP 的平台 DNS-SD 解析本机代播的运营记录（补丁第 1 条之后）—— 配网后 CASE、重启后 CASE 移除都通；首条 CASE 先等网络超时的 3 s 仍在（第 15 条） |
| MC21 | 设备认证：PAA 信任库随 OTA 更新（不单开分区）、体积与放置（E21）；不查吊销列表记为已知限制 | 设计 9.7、2.7（`www` 容量）；附录 E21；A-29 | 配网 | M7 | ✅ M7 第三步（T9.53）：内置 PAA 快照随 OTA 走（`paa_snapshot.c`，74 张，`tools/fetch_paa.py`，`--check` 进 VERIFY）；不查吊销；发版前刷新 |
| MC22 | 测试设备（测试 CD）默认拒绝；"允许测试设备"本地开关：默认关、只在本地可开、记审计 | 设计 9.7、10.1、十三「Matter」、14.2 matter_ctrl 行 | 配网 | M7 | ✅ 2026-10-24（用户定"做"）：本地开关 `/api/v1/matter/allow_test_devices`，默认关、要管理密码、记审计、重启保持；测试 PAA + 测试 CD 密钥只在开着时认（9.7 注记）；test_api MC25–MC27。那台测试设备本身还没试配 |
| MC23 | 配网失败的分类与错误句子（发现超时、PASE 失败、认证失败、失败安全超时、测试设备被拒、限速）：写成用户能照做的话（Muse 会原样转述） | 设计 9.4、6.5；架构第六节"G0-2 实测"观察 3 | 配网 | M7 | ◐ M7 第三步：not_found / code_rejected / attestation_failed / timeout / internal（带阶段名）/ 同步拒的几句，句子 ≤ 199 字节（VERIFY）；测试设备那一类随 MC22 |
| | **访谈与能力** | | | | |
| MC24 | 访谈：读 Basic Information 与 Descriptor（端点、设备类型、簇）；VendorName / ProductName / NodeLabel 按 6.8 净化、只进专门字段 | 设计 9.4 A 图末段、6.8；架构 5.5 | 访谈 | M7 | ✅ M7 第四步（`mc_iv.cpp`；BasicInformation 1–5 + 各端点 Descriptor + PowerSource 特性位；字符串出口按 6.8 净化，只进 `manufacturer` / `model` / `label`；存 hubdb，设计 9.6 注记） |
| MC25 | 簇 → 能力照 9.6 的表，BooleanState 按设备类型分；簇与设备类型 ID 以 Device Library 为准（E22）；kind 词表与 `hub_zcap` 共用一份 | 设计 9.6、6.1、6.2；附录 E22；`components/zhub/CHECKLIST.md` C16 ⑤ | 能力 | M7 | ✅ M7 第四步（`hub_core/hub_mcap.c`，主机测试 21 条；kind 用 hub_zcap 的词表；ColorControl 先按设备类型分、特性位没读 —— 9.6 注记） |
| MC26 | UDM：`mt-<fabric 内节点号>` id、列表项 / 详情照附录 A、`protocol` / `room` / `capability` 过滤认 Matter、`hub.device.get` 兑现说明里承诺的字段 | 设计 6.1、十三「调用输入」、附录 A 列表项；架构第六节"G0-2 实测"观察 2 | 能力 | M7 | ✅ M7 第四步（`hub_matter.c` 的列表项与详情、`protocol` / `room` / `capability` 过滤；`online` / `state` 随订阅 MC28；test_api MC11–MC16） |
| MC27 | 与 thread_inventory 合并：本 fabric 的 `_matter._tcp` 实例按 `mt-` 呈现、不再重复出 `th-`（清单要从 matter_ctrl 拿压缩 fabric ID）；`_matterc` 的 VID / PID 只给配网用 | 设计 7.3、7.5；`hub_ti_core.h` | 能力 | M7 | ✅ M7 第四步（本 fabric 且节点表里配好了的 `th-` 不再出、指向 `mt-`；压缩 fabric ID 从 `matter_ctrl_get_status` 拿，控制器没在跑 / 强制移除 / 配到一半的照旧按 `th-` 列；设计 7.3 注记） |
| | **订阅** | | | | |
| MC28 | 订阅：按能力订属性，普通设备用 9.5 的间隔、ICD 用它自己报的；断了重订；控制器重启后照节点表立即重订 | 设计 9.5「订阅」；9.8b 表「订阅」 | 订阅 | M7 | ✅ M7 第五步（`mc_sub.cpp`：按访谈的簇挑路径、上限 300 s、ICD 同样要（设备报它能做到的）；退避 5 → 30 → 120 s 自己算；开机照表立即重订、同时 2 条在建；设计 9.5 注记） |
| MC29 | 离线判定 → AVAILABILITY / `DEVICE_OFFLINE`，恢复 → ONLINE；`last_seen_s` 与链路 | 设计 9.5「离线判定」、6.7 | 订阅 | M7 | ✅ M7 第五步（订阅没建好且 > 2 × 最大间隔、至少 60 s → `matter_offline`，回来 `matter_online`，都推、不去重；`last_seen_s`；板上没触发过离线） |
| MC30 | CASE 会话池（上限、放 PSRAM、按最近使用淘汰）与订阅占着的会话、CHIP 限额、节点上限对账 | 设计 9.5、9.2 表「限额」、2.1 | 订阅 | M7 | ✅ 2026-10-24 用户定 Matter 设备 32 → 128：会话池 = 节点上限 + 16、定时器 256、交换上下文 32、CASE 客户端 8、同时在建订阅 4（都在 PSRAM；VERIFY 钉住两份 sdkconfig 一致）；板上只有 2 台，128 台没实测 |
| MC31 | ICD：v1 不注册、只被动订阅；check-in 支不支持 | 设计 9.1、9.9 第 4 条 | 订阅 | M7 | ◐ v1 只被动订阅（不注册、不走 CHIP 默认策略里 LIT 的"睡着等 check-in"，照常重订）；check-in 支不支持仍见 MX18 |
| | **命令** | | | | |
| MC32 | `hub.device.control` 的 Matter 列：语义动作 → 簇命令，门锁走定时交互；三道检查与敏感集合；结论 confirmed / delivered / failed，回包带 `state` | 设计 6.2、6.5、9.5「命令」；附录 A control 说明 | 命令 | M7 | ◐ M7 第六步：开 / 关 / 切换 / 亮度（`mc_ctl.cpp`；三道检查与 Zigbee 共用、敏感取自 hub_meta；结论与回包 `state` 同 Zigbee）；门锁（定时交互）、色温 / 颜色 / 窗帘 / 温控随后（回 unsupported） |
| MC33 | 不注册、也不实现原始簇命令（`matter.invoke` / `write` / `read`），VERIFY 钉住 | 架构 MD-17、3.3；设计 6.4、14.2 | 命令 | M7 | ⏳ |
| MC34 | 注册：`current_backends()` 报 `HUB_BACKEND_MCTRL`，控制路径做好才连带 `CONTROL`；matter_ctrl 启停 / 就绪变化时重注册；用户关掉 Thread 时不重注册、commission 照常 | 设计 4.7「什么时候重新注册」、附录 A 注册规则、2.8（A-42）；`hub_registry.h` | 命令 | M7 | ◐ M7 第六步：matter_ctrl 在跑报 `MCTRL | CONTROL`（位一变 resync 重注册，第三步起）；用户关掉 Thread 时的行为没单独验 |
| MC35 | `hub.device.update`、`/api/v1/hub/device/sensitive` 认 `mt-`；`device.health` 的设备数算上 Matter | 设计 6.1 元数据注记、10.1、附录 A | 命令 | M7 | ✅ M7 第四步（`hub.device.update`、`/api/v1/hub/device/sensitive` 认 `mt-`；`device.health` 的 total 扣掉本 fabric 的 Thread 成员、加上配好的 Matter 设备） |
| | **事件与移除** | | | | |
| MC36 | 设备事件：BooleanState / SmokeCoAlarm → ALARM 或 SECURITY（按设备 notify）、PowerSource 低电 → BATTERY、Switch → 按钮事件；句子只用用户起的名字 | 设计 6.7（分类表与注记）、6.8、9.6 | 事件 | M7 | ◐ M7 第五步：门磁 / 人体 SECURITY、漏水 / 烟雾 ALARM、电量 ≤ 15 % BATTERY（每天一条），句子同 Zigbee（`hub_meta_event_name`）；Switch 按钮事件随后（要订事件路径） |
| MC37 | 移除只在本地：`/api/v1/hub/device/remove` 对 `mt-` 先 RemoveFabric 再删节点表与 hub_meta 条目；设备不可达时怎么收尾；不给 Muse | 设计 9.9 第 5 条、10.1、6.1 元数据注记（孤儿条目）、14.2 | 移除 | M7 | ◐ M7 第三步：Matter 先 RemoveFabric、设备确认了才删节点表并清 hub_meta，force 时不答话也删；Zigbee（ZDO Leave）⏳ |
| MC38 | "重置 Matter 控制器"（C+A + 文字确认）；恢复出厂两档对 `mctrl` 与 `matter` 分区的处理、`test_factory_reset.sh` 的断言 | 设计十二；`hub_store.c` 的恢复出厂 | 移除 | M7 | ◐ 2026-10-24：重置只在 `ident_lost` 时给（用户定）：`/api/v1/matter/reset`，先清名字、再清 mctrl、最后删记号（第十二节注记）；恢复出厂两档原样；`test_factory_reset.sh` 的断言随后 |
| MC39 | 换回 HA 型时设备上还留着网关的 fabric：迁移步骤与手册要说先移除 | 设计 8.12 | 移除 | M7 | ⏳（见 MX16） |
| | **接口、前端与灯** | | | | |
| MC40 | 新端点首行 `CROSS_ORIGIN_GUARD` 再 `AUTH_REQUIRED`；写操作没设密码回 `password_required`；记审计 | CLAUDE.md 红线 5；设计 10.1、6.5 注记、6.10 | 接口 | M7 | ⏳ |
| MC41 | Matter 状态读数与"重置控制器"的端点（10.1 只有 `allow_test_devices`）；原型的诊断入口不进出厂 | 设计 10.1、十二、9.8b 标题 | 接口 | M7 | ◐ `/matter/status`（含 `allow_test_devices`）、`/matter/allow_test_devices`、`/matter/reset`（只在 ident_lost）都有了；原型的诊断入口没进正式代码 |
| MC42 | 设备页"添加 Matter 设备"（输入配网码、显示进度）、Matter 卡片的控制与移除；只在 `frontend/direct/`；`test_hub_devices.sh`、i18n、`check_english_only.sh` | 设计 10.2；2.7 A-29 注记 | 前端 | M7 | ◐ M7 第七步：设备页"添加 Matter 设备"（配对码 + 名字，作业进度）、Matter 卡片的开 / 关（第六步）与"从网关移除"（先问；设备不答话再问一次强制移除），`test_hub_devices.sh` 69 条；Muse 页 / 手册随 M6 |
| MC43 | 导出：`/api/v1/hub/export` 带 `mt-` 的名字 / 房间、永不带 fabric 秘密；配置导出不含 Matter | 设计十二、10.1、十三 | 接口 | M7 | ⏳ |
| MC44 | 在网配网占不占 SYS 灯（第十一节那几行按 BLE 窗口写）；改了同步 `check_led_docs.py` 与手册第三节 | 设计第十一节 | 灯 | M7 | ⏳（待用户定，见 MX17） |
| | **线程、看门狗与安全** | | | | |
| MC45 | 看门狗：matter_ctrl 存活检查（往 CHIP 线程投空任务、超时算停转）、占直连型的槽；堆水位标记名别和 `matter_bridge` 现有的那个撞 | 设计十五「watchdog」；A-24；`main/main.c` 的 heap mark | 看门狗 | M7 | ⏳ |
| MC46 | Thread 停摆 L4 会打断订阅：重启后本地立即重订；matter_ctrl 永远不重启 Thread 栈（红线 11）、不调 `otInstanceInit*` / `esp_openthread_init` | 设计十五「watchdog」、14.2；CLAUDE.md 红线 11 | 看门狗 | M7 | ⏳ |
| MC47 | 线程上下文：CHIP 的回调只在 CHIP 任务里；进出 CHIP（作业 → CHIP、CHIP → hub_store / 事件）一律投递；esp_timer 回调不阻塞拿锁（红线 10） | CLAUDE.md S2、红线 10；设计 6.6、6.7 | 线程 | M7 | ⏳ |
| MC48 | 秘密：根 CA 私钥、IPK、NOC 私钥不进日志 / GET / 导出 / Muse；落盘不加密要写进第十三节 | 设计十三「秘密」、9.2「KVS 放哪」；架构 N6、2.5；A-47 | 安全 | M7 | ◐ M7 第二步：身份 blob（根私钥、IPK）只经钩子进 hubdb，不打日志、用完清零（VERIFY）；GET / 导出 / Muse 那一侧随有了相应接口时钉；第十三节的"落盘不加密"已写 |
| MC49 | 出站数据清单（Muse 页逐条比对）：Matter 的 fabric 摘要与节点号；认证若走在线 DCL 要加一行 | 设计十三「出站数据清单」、10.2；`frontend/direct/test_muse.sh` | 安全 | M7 | ⏳（见 MX06） |
| MC50 | CHIP 的 P-256 若走硬件 ECDSA，T9.47 的锁修复同样要覆盖：`idf_fixes` 的 ELF 判据在带 esp-matter 的构建上也成立；红线 7 不动 | `components/idf_fixes/VERIFY.sh`（提交 1c4ca49）；CLAUDE.md 红线 7 | 安全 | M7 | ⏳ |
| | **文档** | | | | |
| MC51 | MD-13（扩到 P3）：matter_ctrl 落地的同一个提交改 `docs/00` 的 Matter 口径（文首作用域、第二节"Matter 的实际路径"、"Matter SDK 一行都没链进来"），`check_scope_claims.py` 跟上 | 架构 MD-13；设计十六 `docs/00` 行；A-01 | 文档 | M7 | ⏳ |
| MC52 | 对外容量：控制器最多管几台与 BR 最多代播几台分开写；本 fabric 让每台设备多占一条代播；`check_capacity_docs.py` 按配置算 | A-07；设计十六 `check_capacity_docs.py` 行、7.5 | 文档 | M7 | ◐ 设计 9.5 注记与 Kconfig 帮助里已分开写（控制器 128 台 / 代播上限 / 每台在本 fabric 多占一条代播）；用户手册与 docs/00 的口径随 MC51 / MC53 |
| MC53 | 用户手册：添加 Matter 设备（先在别的 App 里配好、设备留在原来的 Thread 网络上）、恢复出厂清什么、排障；HA 型手册"网关不提供 Matter 控制器"那段只对 HA 型 | 设计十六、9.4 B ⚠️ 段、第十一节"手册这次不改"注；CLAUDE.md 6.5 | 文档 | M7 | ⏳（写进哪份见 MX15） |
| MC54 | `tools/check_upstream.sh` 盯梢加 esp-matter 的 release 与 #1851；`docs/开发索引.md` 加 matter_ctrl 一行（坑指向 9.8a / 9.8b） | 设计十六；架构第九节 | 文档 | M7 | ⏳ |
| | **测试** | | | | |
| MC55 | 先写 `VERIFY.sh`（P1）：14.2 matter_ctrl 那一行的不变量 + S1 / S2 / S3 自动检查；反向用 `grep_code.py --absent`，每条做变异 | 设计 14.2；CLAUDE.md P1、7.4、编码后第 8 条 | 测试 | M7 | ◐ M7 第一步：`VERIFY.sh` 先写（68 条，13 处变异都抓到）；随后几步加 |
| MC56 | 主机侧测试（14.1 没有 matter_ctrl 一行）：纯逻辑（配网码预检、簇 → 能力、节点表 blob 版本、结论分类、重订策略）拆成 `*_core` 照 zhub 的做法在主机上测 | 设计 14.1 | 测试 | M7 | ◐ 配网码、节点表、身份、访谈记录、簇 → 能力已有（`test_host/`、`hub_core/test_host/hub_mcap_test.c`）；结论分类、重订策略随第五、六步 |
| MC57 | 上板 TC-MC02–06（TC-MC02 两种 Thread 网络各一次；TC-MC03 要门锁）；TC-TI 的板上部分 | 设计 14.3「Matter 控制器」「Thread 清单」、第十七节 M3 第六步注 | 测试 | M7 | ⏳ |
| MC58 | 端到端：E2E-3 的 Matter 那次、E2E-4 用 Thread 门磁、E2E-7；E2E-9 记审批 | 设计 14.4 | 测试 | M7 | ⏳ |
| MC59 | `tools/test_api.sh` 生效断言：配网后列表出现 `mt-`、control 后读回新 state、测试设备开关读回、移除后列表里没有 | 设计 14.5；CLAUDE.md 7.5 | 测试 | M7 | ◐ 列表出现 `mt-` 与字段形状、device.get、改房间读回、能力过滤、device.health 总数（MC11–MC16）；control、测试设备开关、移除后读回随后 |
| | **G0-5 要关 / 要补（设计 9.8a 第 1–10 条、9.8b 第 11–15 条）** | | | | |
| MC60 | `CONFIG_ENABLE_MATTER_OVER_THREAD=n`：CHIP 不起自己的 Thread 栈管理器，经 lwIP 走网关已在路由的 IPv6 | 设计 9.8a 第 1 条、9.2 集成点 1 | 构建 | M7 | ✅ M7 第一步（`sdkconfig.defaults.direct`，VERIFY 逐条钉） |
| MC61 | 不改全局 lwIP 加锁模型（关 `USE_TCPIP_CORE_LOCK_FOR_THREAD_SAFETY`），CHIP 把 lwIP 调用投到 tcpip 任务 | 设计 9.8a 第 2 条 | 构建 | M7 | ✅ M7 第一步（同上） |
| MC62 | 关 Wi-Fi 站点与 #159 的 IPv6 路由钩子 | 设计 9.8a 第 3 条、9.2 表末行 | 构建 | M7 | ✅ M7 第一步（同上） |
| MC63 | 平台 mDNS：不用 CHIP 的最小 mDNS、开 CHIP 的以太网支持、把 `ESP32DnssdImpl.cpp` 加回 esp_matter 的目标 | 设计 9.8a 第 4 条 | 构建 | M7 | ✅ M7 第一步（配置同上；`ESP32DnssdImpl.cpp` 在本组件 CMake 里加回） |
| MC64 | 不带以太网配网驱动、不编它的源文件（片内 EMAC，C5 没有；真跑会和 DM9051 冲突） | 设计 9.8a 第 5 条 | 构建 | M7 | ✅ M7 第一步（本组件 CMake 把它标 HEADER_FILE_ONLY + 配置关掉） |
| MC65 | CHIP 的 `GPIO_RANGE_MAX` 给 C5 补默认值 | 设计 9.8a 第 6 条 | 构建 | M7 | ✅ M7 第一步（本组件 `Kconfig` 再定义一次，不改 esp_matter 的文件；直连型取到 28、HA 型不可见） |
| MC66 | 控制器不宣告自己（否则改掉网关的 mDNS 主机名）；回归看 `bitengine.local` 与 Thread 设备的代播记录 | 设计 9.8a 第 7 条；CLAUDE.md 红线 8 | 发现 | M7 | ✅ M7 第一步（构建期补丁；上板 `bitengine.local` 照旧解析到网关） |
| MC67 | 本组件与 esp_matter 的 `.bss` 都映射进 PSRAM（控制器单例的静态对象落在调用方），加上 CHIP 限额 | 设计 9.8a 第 8 条 | 内存 | M7 | ✅ M7 第一步（`linker.lf` 两个归档；`CHIPProjectConfig.h`） |
| MC68 | esp-matter 那一版的编译缺陷：给配网命令的源文件强制包含兼容头 | 设计 9.8a 第 9 条 | 构建 | M7 | ✅ M7 第一步（兼容头只强制包含进那一个文件） |
| MC69 | CHIP 任务优先级的取值（默认值够不够）；CHIP 日志取 ERROR | 设计 9.8a 第 10 条、9.2「优先级」；附录 E32；A-44 | CPU | M7 | ⏳（见 MX09） |
| MC70 | 浏览上下文 `mType` 截断，本机代播兜底查不到 | 设计 9.8b 第 11 条 | 发现 | M7 | ✅ M7 第一步（构建期补丁）；第三步上板配网验过（按配网码找到本机代播的设备） |
| MC71 | 配网前先等本机出现 discriminator 对得上的 `_matterc` 再发起（CHIP 的浏览是一次性的） | 设计 9.8b 第 12 条 | 配网 | M7 | ✅ M7 第三步（`mc_comm.cpp`：每秒查本机代播、比 discriminator，最多 15 s） |
| MC72 | 发起配网前撤掉上次发现超时残留的配网委托 | 设计 9.8b 第 13 条 | 配网 | M7 | ✅ M7 第三步（发起前撤残留委托；PairDevice 失败自己撤；VERIFY） |
| MC73 | 不用 esp-matter 的 `subscribe_command` / `read_command`，改用 `subscribe::send_request` + 自己的 `ReadClient::Callback` | 设计 9.8b 第 14 条 | 订阅 | M7 | G0-5 已核（设计 9.8b 第 14 条） |
| MC74 | 重启后第一条 CASE 的解析别先等网络超时 | 设计 9.8b 第 15 条 | 发现 | M7 | ⏳（见 MX01） |
| | **M7 出口** | | | | |
| MC75 | 正式实现上带订阅长跑 + 内存泄漏排查（看当前空闲的斜率，不只看 `min_free`；CPU 先量不带 matter_ctrl 的基线；整机内存布局变化一并看）；两种配置都编、HA 侧守护、两种都上板回归；收工前 grep 用户手册 | 设计 9.8、9.8a「内部 SRAM（静态）」、9.8b「G0-5d」、第十七节 M7 行；架构 5.8 R-4 / R-6 / R-7；CLAUDE.md 6.5 | 测试 | M7 | ⏳ |
| | **（M8）BLE 配网（P3+，不在 M7 范围，列出来免得忘）** | | | | |
| MC76 | BLE-Thread 配网（9.4 A）：ble_window 中心角色与扫描、Thread 先恢复再 ConnectNetwork、数据集取本机 OT、失败安全时长（E19）、自检不过的处置 | 设计 9.4 A、3.1–3.3；附录 E19 | BLE | M8 | ⏳ |
| MC77 | `BITENGINE_MATTER_BLE_COMMISSIONING` 只在 G0-4 (b) 通过（或 #1851 有解 / 双 SoC）后才许开，关着时 `CHIPOBLE` 不为 y；CHIP 的 BLE 初始化交给 ble_window（E18），NimBLE 与配对共用 | 设计 2.1、9.2 末两点、14.2；附录 E18；架构 P3+、G0-4 | BLE | M8 | ⏳ |
| MC78 | 打开 BLE 配网时：commission 的说明换成 P3+ 版（编译期常量）、手册的 P3+ 段、SYS 青色语义 | 附录 A 注册规则；设计十六、第十一节 | BLE | M8 | ⏳ |
| MC79 | TC-MC01、TC-BW03；配网成功率按 G0-4 定下的策略再量一遍 | 设计 14.3、9.8「配网成功率」、第十七节（M8）行 | BLE | M8 | ⏳ |

## 冲突与未决

"处置"括号里是默认（拍板项不回复就按默认做）。类：**实测** = G0-5 实测推翻了先前的设计；**矛盾** = 文档之间或文档与上游默认对不上；**拍板** = 要用户定；**缺口** = 哪份都没写。

| # | 类 | 内容 | 处置 |
|---|---|---|---|
| MX01 | 实测 | **运营节点 / 待配网记录的解析**（9.2 集成点 2、E17）：9.2 要给 CHIP 接一个读 thread_inventory 快照的"SRP 直查"后端、查不到再回落 mDNS。9.8b 实测 CHIP 的 ESP32 平台 DNS-SD 本来就会在网络上查不到之后查本机代播表（`mdns_lookup_delegated_service`），补了第 11 条就通；剩下的只是先等网络超时（第 15 条）和浏览一次就结束（第 12 条）。而清单快照是轮询刷新的，跟不上开窗后几秒才注册的 `_matterc` | 定一种做法，9.2 集成点 2 与 E17 按实测改写、关掉（默认：不另做解析后端；在 esp_matter 的补丁里让本机代播先查、查不到再上网络；第 12 条的"等记录"直接轮询本机代播表；MC20 的清单接口不做。补丁打在 esp_matter 里，别打进 `patch_mdns.py` —— 那个两种配置共用，等于改 HA 型） |
| MX02 | 实测 | **重订退避**（9.5）：9.5 写"订阅断了按固定几档退避重订"；9.8b 第 14 条改成重订走 CHIP 的默认退避、不封顶，整条结束 / 连不上之后再自己重新发起。9.5 的离线判定（几倍最大间隔 + CASE 重建失败）也要和 CHIP 的存活超时对上 | ✅ 2026-10-24 M7 第五步定：**取 9.5、没取默认的 9.8b** —— CHIP 的默认策略对 LIT ICD 的 INACTIVE_TIMEOUT 让订阅睡着等 check-in，我们不做 ICD 注册，会永远等下去；所以 OnResubscriptionNeeded 自己排 5 → 30 → 120 s，超时断的重建 CASE（同默认）。整条结束之后同一套退避重建。正本是代码常量（`mc_sub_core.c` 的 `mc_sub_backoff_ms`、`mc_sub_judge`），9.5 注记指过去 |
| MX03 | 矛盾 | **凭据**（9.3）：设计要自签 RCAC、不用 ICAC、私钥在 `hubdb` 的 `mctrl/rcac_key`、fabric ID 随机。esp-matter 默认的 `example_credentials_issuer`（`TEST_OPERATIONAL_CREDS_ISSUER`）包的是 CHIP 的 `ExampleOperationalCredentialsIssuer`：根与中间 CA 的私钥存在 CHIP 的 KVS（即 `matter` 分区），签发带 ICAC；原型用的就是它。9.3 也没说 fabric ID、IPK、控制器自己的 NOC 存哪。M7 第一步的工作区代码先用默认签发器，并把自己的 fabric 记号放在 **`matter` 分区里**一个也叫 `mctrl` 的命名空间 —— 与 2.5"`mctrl` 在 hubdb、只经 hub_store 写"不是一回事，同名两处（7.6） | 选 `CUSTOM_OPERATIONAL_CREDS_ISSUER` 自己实现 9.3，还是改 9.3 接受默认签发器（默认：前者，fabric 根的全部材料放 hubdb 的 `mctrl`，`matter` 分区里的命名空间改个不撞的名字）。连带重看 2.5 规则 5"`matter` 分区坏了 → 设备要重新添加"：根私钥与 fabric ID 留在 hubdb 时，可能重建控制器而不丢设备；✅ 10-24 用户按默认定、第二步实现：自己的签发器，身份放 hubdb `mctrl/ident`；matter 分区里的记号改名 `mc_bind` |
| MX04 | 缺口 | **IPK 与测试组数据**：设备拿到的 IPK 来自凭据签发器，example issuer 给的是 CHIP 测试用的默认值（`GroupTesting::DefaultIpkValue`，公开值）；`setup_commissioner` 又无条件调 `GroupTesting::InitData`、把控制器自己这边的 IPK 设成同一个默认值（两边必须一致）。9.3、第十三节都没提 IPK | H1：核规范对 IPK 的要求（默认：随 MX03 的自有签发器一起做 —— 首次启用时随机生成 IPK、和根私钥同等保管，控制器这边经构建期补丁用同一个值、不写测试组数据）；✅ 10-24 第二步：IPK 随身份随机生成；构建期补丁不写测试组数据（设计 9.3 注记） |
| MX05 | 拍板 | **Vendor ID**（9.3、E19）：esp-matter 的 `ESP_MATTER_CONTROLLER_VENDOR_ID` 默认 4891（0x131B）是**乐鑫自己的厂商号**，不是测试厂商号（0xFFF1–0xFFF4），原型和第一步都用的它；它会写进设备的 fabric 表，别的生态"已连接的服务"里网关可能显示成乐鑫 | 待用户定：① 申请 CSA 厂商号；② 开发期用测试厂商号、发版前再定；③ 沿用乐鑫号（用的是别人的号，不建议）（默认 ②）；◐ 10-24：开发期 0xFFF1；发版前定（TODO T9.52） |
| MX06 | 实测 | **PAA 信任库**：9.2 表写 `SPIFFS_ATTESTATION_TRUST_STORE=y`（从 SPIFFS 分区读，#159 的 `paa_cert`）；9.7 说不单开分区、放 app 或 `www` 随 OTA 走（直连型分区表也没有 `paa_cert`）；原型与第一步实际开的是 `DCL_ATTESTATION_TRUST_STORE=y` —— 配网时**在线**向 CSA 的 DCL 查 PAA，9.8b 的"DCL 信任库认证通过"指的就是它。三处各说各的 | 待用户定：① 离线快照（9.7 原意：`CUSTOM_ATTESTATION_TRUST_STORE`，PAA 进 app，量增量，E21）；② 在线 DCL（配网要外网与 TLS、把设备的 VID / PID 发给 DCL —— 要进第十三节出站清单与 Muse 页，外网断时配不了）；③ 离线优先、查不到再在线（默认 ①）。9.2 表那一行按定下的改；✅ 10-24 用户按默认 ①：内置快照，M7 第三步实现 |
| MX07 | 矛盾 | **测试设备开关**：9.2 表 / 9.7 让 `ESP_MATTER_COMMISSIONER_SUPPORT_TEST_CD=n`，那是编译期的；9.7 / 10.1 又要网页上的"允许测试设备"运行时开关。esp-matter 只在 `setup_commissioner` 里按这个 Kconfig 调一次 CHIP 校验器的 `EnableCdTestKeySupport` | 开关改成运行时调这个接口（默认：Kconfig 不再承担判定，"默认拒绝"由开关的出厂值保证，VERIFY 钉开关默认 false），9.2 表那一行改写 |
| MX08 | 矛盾 | **会话与节点容量**：9.5 的 CASE 会话池上限、9.2 表的 CHIP 限额（安全会话池、同时活跃的 CASE 客户端）、2.1 的 `MATTER_MAX_NODES` 是三个各写各的数，节点上限比会话池大；每条订阅都占着一条 CASE 会话，"按最近使用淘汰"会把订阅踢掉。A-07 / `docs/00` 的 Matter 容量是 BR 代播口径，不是控制器能管几台；⚠️ 2026-10-24 M7 第五步审计补：CHIP 定时器池静态 32 个（`CONFIG_NUM_TIMERS`，没开 `CHIP_SYSTEM_CONFIG_POOL_USE_HEAP`），每条订阅常驻一个，也要一起对账 | ✅ 2026-10-24 用户定：Matter 设备上限 128，三个数按它对齐（9.2 表「限额」注记；正本 `CHIPProjectConfig.h` + `sdkconfig.defaults.direct`） |
| MX09 | 实测 | **CHIP 任务优先级**：9.2 要"低于 OT 与 zhub、高于 hub_jobs"；9.8a 第 10 条实测默认值与 mdns 同级、低于全部业务任务（也就低于 hub_jobs）。E32 问的 Kconfig 名原型里已经有了（`CONFIG_CHIP_TASK_PRIORITY`） | E32 名字那半关掉；取值随 M7 长跑的 CPU 判据定（默认：照 9.2 的相对次序设；hub_jobs 还没实现，落地时一起核） |
| MX10 | 矛盾 | **启动依赖**：2.6 #13 写 `matter_ctrl_start()`"依赖 thread_br 已运行"；2.8 说用户关掉 Thread 时 matter_ctrl 照管 IP 上的 Matter 设备、`hub.matter.commission` 照常可加 IP 设备。开机时 Thread 本来就关着的话，按 2.6 matter_ctrl 不起 | （默认：matter_ctrl 不依赖 thread_br 在跑，只要 lwIP；2.6 #13 改写） |
| MX11 | 拍板 | **崩溃循环兜底**（红线 1）：原型带"晚起 + RTC 计数，连续几次没撑过就不起"；设计只靠安全模式。用户退出安全模式后 matter_ctrl 照起照崩 —— 和 E33（Muse）同一个形状，而 Matter 没有运行时开关可关；OTA 后首启的 mark valid 只看以太网拿到地址，坏了的 matter_ctrl 不会触发回滚 | 待用户定：① 照 E33 加 Matter 启用开关 +"退出并先关掉 Matter"；② 正式实现保留原型的 RTC 计数（只拦 matter_ctrl，不加用户可见的开关）；③ 只靠安全模式（默认 ②；M7 第一步的工作区代码已照 ② 写） |
| MX12 | 拍板 | **原型残留**（9.8b"G0-5 结论"末句）：`matter` 分区里有原型写的 fabric 凭据（两台设备已在网关侧解绑），主 NVS 有原型的 `g05` 命名空间（已清零）；正式固件起 CHIP 会把那份 KVS 当成自己的 | （默认：只擦 `matter` 分区、不碰主 nvs 与 hubdb，`g05` 留着无害。第一步的工作区代码是"没有我们的 fabric 记号就先擦 `matter` 分区"，等于自动按默认处理 —— 但那个记号放在哪受 MX03 影响）；✅ 10-24：第一、二步上板时自动按默认处理（绑定记号对不上 → 擦 matter 分区），`g05` 留着无害 |
| MX13 | 拍板 | **esp-matter 版本**：9.2 写的是范围（`~1.6.0`），原型钉的是 `==1.6.0~1`；1.6.1 在 G0-5 当天刚发，9.8a / 9.8b 的补丁都是对着原型那一版打的 | （默认：M7 继续钉原型那一版，9.2 改成钉死的写法；换新版另作一步、补丁逐条重核） |
| MX14 | 缺口 | **Matter 的状态与重置没有端点**：10.1 只有 `allow_test_devices`；第十二节的"重置 Matter 控制器"没有端点；原型靠诊断入口 `POST /api/v1/diag/matter`（`publish_ota.sh` 拒发 DIAG_*） | 补进 10.1（默认：只读的 `GET /api/v1/matter/status` —— 第一步的工作区代码已加 —— 加 `POST /api/v1/matter/reset` 带文字确认；配网与命令走 `/hub/invoke`） |
| MX15 | 矛盾 | **手册写进哪份**：第十六节把"添加 Matter 设备"等列在 `docs/guide/Hub使用手册.md`；第十一节又说那份是 HA 型手册、直连型手册在 M6（现在还没有）；HA 型手册写着"网关不提供 Matter 控制器"，对 HA 型仍然对 | （默认：Matter 内容进 M6 的直连型手册，M7 收工时它要已经存在；HA 型手册不动；6.5 的 grep 两份都做；第十六节改成这样写） |
| MX16 | 缺口 | **换回 HA 型时的 Matter 设备**：8.12 只写了 Zigbee 与分区；直连型配过的 Matter 设备上留着网关的 fabric（占设备的一个 fabric 槽，HA 那边不受影响） | （默认：迁移步骤与手册写"换回之前先在网页上移除 Matter 设备"；`migrate_profile.py` 只提示、不代做） |
| MX17 | 拍板 | **在网配网占不占灯**：第十一节"SYS 绿 / 红"那几行是按 BLE 窗口写的，在网配网没有 BLE 窗口 | （默认：不占灯，进度看网页与 Muse，理由同"Muse 会话不占灯"；不改就不用动 `check_led_docs.py` 与手册第三节） |
| MX18 | 缺口 | **附录 E 没跟上**：G0-5 已回答、还没关的 E16（9.8a 第 1–5 条 + 原型配置）、E17（MX01）、E28（原型由本组件先初始化 `matter` 分区；CHIP 自己会不会初始化仍没核）、E32（MX09）；9.9 说"全部进附录 E"，可第 4 条（ICD check-in）、第 5 条（移除）在附录 E 里没有对应条 | 在 M7 动手的提交里关掉或补条，改成指针 |
| MX19 | 矛盾 | **Kconfig 依赖**：2.1 让 `BITENGINE_ENABLE_MATTER_CTRL` 依赖 `BITENGINE_ENABLE_MUSE_LINK`；架构第〇节说 Hub 能力（含 Matter 控制器）不依赖 Muse | （默认：去掉对 Muse 的依赖，只留纯以太网；运行时本来就不依赖） |
| MX20 | 小处 | ① 9.8a 与 9.8b 的 app 体积是同一个数，而 9.8a 说当时配网 / 命令 / 订阅的代码还没被引用、完整的会更大，其中一处多半抄错；② `hub_events.h`、`hub_ti_core.h`、设计 6.7 注记里写"M5 matter_ctrl"，里程碑表是 M7；③ 9.8b 顺带发现的代播子类型封顶不归 matter_ctrl（产品化 TODO T9.50，改 `thread_br` 等于改 HA 型），本控制器补了第 11 条后按基础类型浏览，不受它影响 | ① 以 M7 构建的 `check_profiles.py` 输出为准、两格改成指针（7.6）；② 动到时顺手改；③ 按 T9.50 另做 |
| MX21 | 缺口 | **配网的限速扣得太早**（M7 第三步上板发现）：`hub.matter.commission` 限速 1 次 / 60 s，在处理函数之前扣 —— 码打错一位（Muse 从照片读码也可能错）也扣掉这一分钟，用户得干等 | （默认：处理函数当场拒（invalid_params / unsupported）时把令牌还回去；限速的意图是挡反复真配网，不是挡打字错。改 hub_policy 的令牌桶要两个出口一起改、主机测试先行）；✅ 2026-10-24 M7 第七步按默认做：`hub_call_t.rl_refund`，配网处理函数受理之前的每个当场拒都置上，两个出口（hub_api.c、muse_link.c）看到就 `hub_rl_refund`（主机测试 hub_policy；test_api MC24） |
