# muse_pair / muse_link / noise_core 的上游引入登记

上游：<https://github.com/facebookincubator/muse-gadget-sdk>（Apache-2.0，三个组件目录各放一份上游 `LICENSE`）。
规矩出处：`docs/15_muse_direct_design_v1.md` 附录 C。本文件登记三样东西：**引入的提交**、**逐文件改动**、**每次跟进上游的 diff 结论**。

核对：`bash components/muse_link/check_upstream_files.sh [上游检出目录]`
（不带参数只查本地：三个组件下每个文件都已登记、引入文件哈希与清单一致；带上游目录时再与固定提交逐字节比对，
并列出上游远端 `origin/HEAD` 自固定提交以来动过的、我们引入了的文件 —— 先 `git -C <上游目录> fetch`）。

## 固定提交

`e6fe5174092702ffddae6f90759a38d55a4ebae1`（2026-10-21 16:03 -0700，#242）。
附录 C 原写 `693cde9`（复核到 `fa07304`）；M2 开工按"以当时的上游 HEAD 重新固定"取 `e6fe517`。

## 逐文件改动

**除 `noise_core/CMakeLists.txt` 外全部原样，内容零改动。** muse_pair / muse_link 唯一的位置变化：头文件从上游的
`esp32/main/` 放进组件的 `include/`（本项目组件规范）。noise_core 上游本来就是独立组件，目录结构原样。

| 本地 | 上游 | 处理 |
|---|---|---|
| `muse_pair/link_pairing.c`、`pairing_transcript.c`、`pairing_signer_policy.c` | `esp32/main/` 同名 | 原样 |
| `muse_pair/include/*.h`（3 个） | `esp32/main/` 同名 | 原样，只换目录 |
| `muse_link/vm_api.c` | `esp32/main/vm_api.c` | 原样（附录 C 原计划"小改 base URL"——上游已有 `vm_api_set_base_url()` / `vm_api_set_sdk_token()`，由调用方从 hubdb 喂，不用改源码） |
| `muse_link/include/vm_api.h`、`vm_connect.h` | `esp32/main/` 同名 | 原样，只换目录 |
| `*/test_host/upstream_tests/**` | `esp32/tests/` 同名 | 原样，上游测试一行不改 |
| `muse_link/test_host/upstream_tests/muse_account/muse_account_api.{c,h}` | `esp32/components/muse/` | 原样。**固件不引入**（附录 C）；只因上游 `link_vm_api_harness.c` 顺带测了它，不带上它 harness 就得改 |
| `noise_core/include/**`、`noise_core/src/**` | `esp32/components/noise_core/` 同名 | 原样（M3 第一步，2026-10-22）。上游的 `MbedtlsCryptoBackend.cpp` 也原样带着，但上游 CMakeLists 就不编它，我们同样不编 |
| `noise_core/CMakeLists.txt` | `esp32/components/noise_core/CMakeLists.txt` | **modified**：源文件只在 `CONFIG_BITENGINE_ENABLE_MUSE_LINK` 下编（HA 型不编，R-3）；SRCS 为空时 IDF 建 INTERFACE 库、不能加 PRIVATE / PUBLIC 选项，所以 `cxx_std_17`、`-fno-exceptions`、`-fno-rtti` 两条一并放进条件。源文件列表、依赖、编译选项照上游 |
| `muse_link/include/noise_upgrade.h`、`ws_mask.h` | `esp32/main/` 同名 | 原样，只换目录（M3 第四步）。WS 升级请求的构造与应答分类、WS 掩码，会话层（`muse_session.cpp`）直接用 |
| `muse_link/test_host/upstream_tests/{link_noise_upgrade_harness.c, ws_mask_harness.c, test_ws_mask.py, test_link_noise_session.py}` | `esp32/tests/` 同名 | 原样。两份 .py 里只跑能独立运行的那条（`test_word_masking_matches_the_byte_loop`、`test_upgrade_request_and_classification`）：其余几条检查的是上游 `noise_control.cpp` / `app.c` 的源码形状，那两份我们不引入 |
| `noise_core/test_host/linux_sdk/noise_xx.py` | `linux/src/musegadget/noise/noise_xx.py` | 原样。**固件不引入**：只给自加的互通测试当对端（Linux SDK 的 Noise XX，Python + cryptography） |

上游 Kconfig 符号的处理：`CONFIG_HOMEHUB_PAIRING_AUTH_EPOCH` 在 `muse_pair/CMakeLists.txt` 里定为 1（它只对厂商 eFuse
认证有意义）；`CONFIG_HOMEHUB_PAIRING_EFUSE_AUTH` 不定义 = 只用社区模式。

### 调用方义务（引入文件原样不动，下面这些由本项目的调用方和构建保证）

| 义务 | 原因（上游代码里的事实） | 谁保证 |
|---|---|---|
| `vm_api.c`、`link_pairing.c` 编译期按文件定义 `LOG_LOCAL_LEVEL=ESP_LOG_WARN` | 两处以 INFO 打 SDK token 前 12 个字符（`vm_api.c:425`、`link_pairing.c:1058`）；本项目日志进 log_store、网页可看可导出。不用运行时 `esp_log_level_set`：任何 `esp_log_level_set("*", …)` 都会清掉按 TAG 设的级别 | 两个 CMakeLists；两个 VERIFY 查编译命令与目标文件字符串 |
| `vm_api_set_sdk_token()` / `vm_api_set_base_url()` 同一时刻只在一个任务里调、且就是调 vm_api 的那个任务 | 写静态缓冲不加锁（`vm_api.c:103-114`），读在 `:235`；网页改 token 时跨任务调会读到半截 | 配对剖面：muse_pair 的 prov 任务（M2，`muse_pair.c`）；正常开机：会话任务（M3） |
| 配对下发的 `api_url_v2` 先验 `https://` 再交给 `vm_api_set_base_url()` | setter 什么 scheme 都收，`http://` 会让令牌和 SDK token 明文出站 | `muse_pair.c` 的 `pair_effective_api_url()`（非 https 当作没下发） |
| `link_pairing_init(..., sdk_token)` 传的缓冲在配对窗口期间静态且不变 | 只借指针不拷贝（`link_pairing.c:84,686`） | `muse_pair.c` 的 `s_sdk_token`（静态缓冲，开机读一次、收尾才清零） |
| vm_api 只在专门的任务里调：配对剖面里是 muse_pair 的 prov 任务（8 KB 内部栈），正常开机是 `muse_sess`；不在 NimBLE host 任务、httpd、事件循环、esp_timer 里调 | 一次调用栈帧约 2.4 KB 加 TLS 握手；单次最坏约 63.5 s（4 次 × 15 s + 退避），刷新两段约 127 s，DNS 不受超时约束（`:38-44,135,186-220`） | M2 `muse_pair.c`、M3 muse_session（看门狗时长见设计 4.6） |
| `handle_client_hello` 的 ECDH、解密与 cJSON 照上游在 NimBLE host 任务里做 | 上游 host 栈 4096、没在本构建上量过 | 直连型把 `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE` 调到 7168；关窗时日志打高水位、`/muse/status` 的 `blew.host_stack_free`，上板实测后定 |
| pair_ble 对明文帧和解密后的帧都做 JSON 深度检查（≤ 32） | `CONFIG_CJSON_NESTING_LIMIT=1000`；上游 `ble_server.c:287-311` 的 `command_json_depth_valid` 不在引入范围里 | `pair_ble.c`（`pair_json_depth_valid`，VERIFY 钉着） |
| hubdb 里 access token ≤ 2040 字节、refresh token ≤ 2026 字节 | 鉴权头缓冲 2048（`vm_api.c:272,394,447`），减去 `Bearer ` / `Bearer hatch_refresh:` 前缀 | M2 provision_v2 校验、M3 |

#### noise_core 的调用方义务（M3 `muse_session` 接入时兑现；2026-10-22 第二轮审计）

| 义务 | 原因（上游代码里的事实） |
|---|---|
| 每次断线都销毁 `ClientSession` 对象，不复用 | 会话失败后 `transport_` 里的密钥要等析构才清零（上游靠作用域做到） |
| 收到超出接收缓冲的帧（`ResourceExhausted`）时自己断开重连 | `ClientSession` 只回错误、不把会话判失败；接着收会让 nonce 错位 |
| `ClientSession`（512 B）与 `PsaCryptoBackend` 放在会话任务的栈上（PSRAM 栈），不做成静态全局 | 静态全局会落进内部 `.bss`，占 4.12 内部常驻预算（≤ 2 KB）的四分之一 |
| 缓冲只从 PSRAM 分配，分配失败就不连，不退回内部内存 | 上游 `noise_control.cpp:2046-2068` 有退回内部内存的写法，守不住 4.12 |
| msg3 载荷（VM bearer）所在的调用方缓冲用完清零 | 它在调用方的缓冲里，noise_core 不管 |
| 小于 64 KB 的发送缓冲要在板上测一次 | `ClientSession.cpp:255-260` 说允许，`ClientSession.h:115` 的注释说至少 `kMaxOutboundWebSocketPayloadSize`，两处矛盾 |

⚠️ 会话任务崩溃时 PSRAM 栈上的 bearer / 会话密钥可能进 coredump（`/api/v1/diag/coredump` 可下载），与设计第十三节"秘密不进 GET"冲突，
M3 写威胁模型时一并处理。

### 测试侧的适配（不改上游文件）

- `muse_pair/test_host/run.sh`：上游 `test_link_pairing_handshake.py` 写死 `libp256m.a`，IDF v6.0.2 的目标名是 `p256-m`
  （编出 `libp256-m.a`）。只在临时副本里替换，且只在 IDF 的 CMakeLists 确实是这个目标名时替换。上游按 IDF v6.0.1 写。
- 本仓库自加 `muse_pair/test_host/kdf_vectors_harness.c` + `test_kdf_vectors.py`：上游的握手测试是自洽的（"手机端"
  直接拿固件算出的会话密钥），固件的 HKDF 盐、标签、会话 ID、AAD 格式改错了照样全绿，只有向量测试的 Python 部分复算了密钥。
  自加测试把向量灌进固件状态、调固件自己的 `derive_session_keys()` / `aes_gcm_decrypt()`。2026-10-22 变异测试：盐顺序、
  盐长、方向标签、AAD 分隔符四处**只有它抓得到**。

- `noise_core/test_host/run.sh`：上游 `test_noise_core*.py` 要主机的 PSA Crypto 开发库（pkg-config `mbedcrypto` 或
  `-lmbedcrypto`），缺了会**静默跳过**。run.sh 在临时目录用 IDF 自带的 tf-psa-crypto 编出来、合成一个 `libmbedcrypto.a`，
  经 `CXX` 环境变量带 `-I` / `-L`（上游把 `CXX` 按 shell 拆词）；任何一条跳过都判不过。
- 本仓库自加 `noise_core/test_host/noise_interop_harness.cpp` + `test_noise_interop.py`：上游三条 noise_core 测试只测了
  原语和 Transport 自己跟自己收发，**从没拿发起方和别的实现握过手**。自加测试让固件的 `InitiatorHandshake` + `Transport`
  对 Linux SDK 的 `NoiseXXResponder` 走完 XX、核对双方载荷 / 握手哈希 / 对端静态公钥，再双向各发 5 条密文（含一条高计数，
  钉 nonce 高字节的字节序），外加篡改 msg2、篡改密文标签两条反例。2026-10-22 变异：协议名改一个字符、nonce 高字节移位、
  Finalize 收发密钥对调、msg3 载荷改一个字节，四处**上游测试都全绿，只有它抓得到**。

### 已知覆盖缺口（上游测试没测、我们暂未补）

- noise_core 的 `ClientSession`、`OrderedTransportFramer`、`ServiceCodec` 没有和 Linux SDK 的 `framing.py` / `envelope.py` 做过互通；
  几个解码器没有畸形输入用例。输入只来自已过 AEAD 校验的服务端，风险低；M3 移植会话测试时补。

- ~~`vm_connect.h` 的 `vm_derive_id()` 缓冲区边界：把 `id_len + 1 > out_cap` 改成 `id_len > out_cap`（写越界 1 字节）
  上游两个 harness 都不报~~ 已兑现（M3 第四步）：`muse_cfg_core_test.c` 的 `test_vm_url_label` 用 `MUSE_VM_ID_MAX + 1` 的缓冲
  喂 64 / 65 字节标签（ASan 下越界 1 字节即报）。
- `vm_api.c` 内部的响应缓冲、cJSON 树、带 refresh token 的请求体释放前**不清零**（原样引入不改）。本项目自己持有的令牌、VM 凭据
  在 `muse_link.c` 里包一层清零（`tokens_free` / `vms_free`）；上游内部这几处留着，跟进上游时看有没有补。
- AES-GCM nonce：向量只有 m2d、计数 0（nonce 全零），d2m 与计数 > 0 的构造只被自洽的握手测试覆盖。
- ~~上游的凭据 lint 只扫临时布局的 `main/`，眼下那里只有 vm_api~~ 已兑现（M3 第四步）：`run.sh` 把 `muse_session.cpp`、`muse_link.c`、
  `muse_proto.c` 也链进 `main/`。

## 跟进记录

| 日期 | 从 → 到 | 结论 |
|---|---|---|
| 2026-10-22 | noise_core（首次引入）→ `e6fe517` | 29 个文件（组件 23 + 上游测试 5 + Linux SDK 的 `noise_xx.py`）；除登记为 modified 的 CMakeLists 外逐字节等于上游。主机测试 6/6（上游 3 + 自加互通 3） |
| 2026-10-22 | （首次引入）→ `e6fe517` | 61 个文件逐字节等于上游；主机测试：muse_pair 5/5（含自加 1 条）、muse_link 3/3。设计附录 C 的分析做到 `fa07304`：`fa07304..e6fe517` 之间清单内的协议文件零改动，只动了 3 个测试替身头（`link_fakes/esp_app_desc.h`、`esp_ota_ops.h`、`esp_partition.h`，共 +30 行），附录 C 的结论不变 |

## 本仓库自写的文件（相对 components/）

三个组件目录（muse_pair、muse_link、noise_core）下的**每个文件**都必须登记：要么在这里（自写），要么在下面的上游清单里。
两边都没有 = check 红 —— 防止"改了引入文件、顺手把它从清单里删掉"就绕过核对（2026-10-22 审计）。
加自写文件（如 `pair_ble.c`）时在这里加一行。

<!-- check_upstream_files.sh 读这一段 -->
```local
muse_pair/CMakeLists.txt
muse_pair/VERIFY.sh
muse_pair/test_host/run.sh
muse_pair/test_host/kdf_vectors_harness.c
muse_pair/test_host/test_kdf_vectors.py
muse_link/CMakeLists.txt
muse_link/VERIFY.sh
muse_link/UPSTREAM.md
muse_link/check_upstream_files.sh
muse_link/test_host/run.sh
muse_link/include/muse_cfg.h
muse_link/include/muse_cfg_core.h
muse_link/muse_cfg.c
muse_link/muse_cfg_core.c
muse_link/include/muse_hold.h
muse_link/muse_hold.c
muse_link/test_host/muse_cfg_core_test.c
muse_link/test_host/muse_cfg_store_test.c
muse_link/test_host/stub/device_identity.h
muse_link/test_host/stub/esp_err.h
muse_link/test_host/stub/esp_log.h
muse_link/test_host/stub/esp_mac.h
muse_link/test_host/stub/mbedtls/platform_util.h
muse_link/test_host/stub/sdkconfig.h
muse_link/muse_proto.h
muse_link/muse_proto.c
muse_link/test_host/muse_proto_test.c
muse_link/include/muse_link_core.h
muse_link/muse_frame.h
muse_link/muse_frame.c
muse_link/test_host/muse_frame_test.c
muse_link/muse_push_core.h
muse_link/muse_push_core.c
muse_link/test_host/muse_push_core_test.c
muse_link/muse_session.h
muse_link/muse_session.cpp
muse_link/muse_link_priv.h
muse_link/muse_link.c
muse_link/include/muse_link.h
muse_link/muse_link_core.c
muse_link/test_host/muse_link_core_test.c
muse_pair/include/muse_pair.h
muse_pair/muse_pair.c
muse_pair/pair_core.h
muse_pair/pair_core.c
muse_pair/pair_ble.h
muse_pair/pair_ble.c
muse_pair/test_host/pair_core_test.c
noise_core/VERIFY.sh
noise_core/test_host/run.sh
noise_core/test_host/noise_interop_harness.cpp
noise_core/test_host/test_noise_interop.py
```

`muse_pair/pair_ble.c` 是**改写**自上游 `esp32/main/ble_server.c`（@ e6fe517），不是原样：保留 Meta 的 Apache-2.0 文件头，
照抄的部分与改动逐条写在它的文件头里；UUID 那 14 行由程序从上游原样拷贝，`muse_pair/VERIFY.sh` 从 UUID 字符串独立验算字节。
跟进上游时 `ble_server.c` 的改动要人看一遍、决定要不要跟（它不在上游清单里，`check_upstream_files.sh` 不会报）。

## 文件清单（sha256  本地路径（相对 components/）  上游路径  [modified]）

<!-- check_upstream_files.sh 读这一段；改了任何引入文件都必须同步这里，并在上面登记改动。
     有本地改动的文件在行末加第四列 modified：本地哈希照查，带上游目录核对时不再要求与上游逐字节相同 -->
```manifest
90c105ae35784b046d0354e031113756e37571d9372a2eec812a2cfa6f4a97b5  muse_pair/link_pairing.c  esp32/main/link_pairing.c
5839265d65d9c0a21903f8fe66c453d3a1dfb9d6b273650dfa5a341b6f1144c6  muse_pair/pairing_transcript.c  esp32/main/pairing_transcript.c
e2f9ebc97f30aa5349554bf9a6d29651b170b32ef1f91d43caef8024bcd3e63d  muse_pair/pairing_signer_policy.c  esp32/main/pairing_signer_policy.c
e5c3dc1dab1a5fa9f545d91c65aa537fd6f5801735a9b2d746d82be613e46b84  muse_pair/include/link_pairing.h  esp32/main/link_pairing.h
61db7f85fa4a55344b4ee1c3b86232f487ad90525fa130799434836939b9196d  muse_pair/include/pairing_transcript.h  esp32/main/pairing_transcript.h
1afd12c44d550e24b9d8525a1968546f1c702b68bde9ad49e71b29c43b3d67f1  muse_pair/include/pairing_signer_policy.h  esp32/main/pairing_signer_policy.h
0353a8ae950232a70f0379a4b8a10b661b2f3e3a598e9411332fc0d4f98947fa  muse_pair/LICENSE  LICENSE
40db4dca2a13e9f4d873da3c32dbf818f9f22c4af52d0287f89b98488aa2bae5  muse_link/vm_api.c  esp32/main/vm_api.c
a02f5e1d5d475d4899750bac373a95d53e93db75e3921fe3c56c5bc1824cc105  muse_link/include/vm_api.h  esp32/main/vm_api.h
2da0fe3b2e1b615d4bc004837345a7deb6e9feb9f527abc08e1ebeb28aa36bb9  muse_link/include/vm_connect.h  esp32/main/vm_connect.h
0353a8ae950232a70f0379a4b8a10b661b2f3e3a598e9411332fc0d4f98947fa  muse_link/LICENSE  LICENSE
68dab3311d0217b6876a82c1fe03822f8c2b038374b6fe21bf440d30de5e23fa  muse_pair/test_host/upstream_tests/link_pairing_handshake_harness.c  esp32/tests/link_pairing_handshake_harness.c
b0b80b2976e8e73e6cde5f64c9bdf59c13f9d6c1095ae57828617d154b6fd325  muse_pair/test_host/upstream_tests/link_pairing_signer_policy_harness.c  esp32/tests/link_pairing_signer_policy_harness.c
48211dc05b81e1676d678718f8b18cbaea438d64da26fb8d10956d057e183806  muse_pair/test_host/upstream_tests/link_pairing_transcript_harness.c  esp32/tests/link_pairing_transcript_harness.c
850d6907fdeb34b57c47fcd16e5a98d06295d41a909a1af0aa34897eb434804e  muse_pair/test_host/upstream_tests/pairing_fakes/esp_efuse.h  esp32/tests/pairing_fakes/esp_efuse.h
df40b6d96f463389e83ddf9458b2be9e603f970781001b8994e3d2bee946233c  muse_pair/test_host/upstream_tests/pairing_fakes/esp_log.h  esp32/tests/pairing_fakes/esp_log.h
f49212ec5c8e4f9820be9c8e980943cfd2622c59fdac145bf7e576f3f543d439  muse_pair/test_host/upstream_tests/pairing_fakes/esp_random.h  esp32/tests/pairing_fakes/esp_random.h
5bcad341ecadc85f2e6d7c1d3806ecf9acd9fd3e00cefdbe4c1c1a9b266f6f82  muse_pair/test_host/upstream_tests/pairing_fakes/esp_timer.h  esp32/tests/pairing_fakes/esp_timer.h
69b7334eee3a9e5f72ab36235e6a22550bb77bd502f00e0b46b7960a486ce071  muse_pair/test_host/upstream_tests/pairing_fakes/freertos/FreeRTOS.h  esp32/tests/pairing_fakes/freertos/FreeRTOS.h
bd6409e17fd334852f6545681fa37d05cf03504317ab32a668d2c4ed81a4e480  muse_pair/test_host/upstream_tests/pairing_fakes/freertos/semphr.h  esp32/tests/pairing_fakes/freertos/semphr.h
7decc9ab33bd20597efb147b87277af59e6e5c6d42a42a8089c4b758daa1ccc0  muse_pair/test_host/upstream_tests/pairing_fakes/psa_crypto_driver_esp_ecdsa_contexts.h  esp32/tests/pairing_fakes/psa_crypto_driver_esp_ecdsa_contexts.h
a93dcd6320201bb5d48a1a6febf33ebe47e1ab0ad9e6cce4780da568fddd2927  muse_pair/test_host/upstream_tests/pairing_fakes/psa_crypto_driver_esp_ecdsa.h  esp32/tests/pairing_fakes/psa_crypto_driver_esp_ecdsa.h
4499dcc7d3a8388029f6da4d377083212979f246d339d5d3a5fa985322245c42  muse_pair/test_host/upstream_tests/test_link_pairing_handshake.py  esp32/tests/test_link_pairing_handshake.py
2c0f2a02c69ca0d17afe585dd1a23e7d60910b5bb5c53c5a243680bf5f18910a  muse_pair/test_host/upstream_tests/test_link_pairing_signer_policy.py  esp32/tests/test_link_pairing_signer_policy.py
b4b813933a118ebf4082ac2a48262d37fb141194fbe2e1e4acc98cc0045f7205  muse_pair/test_host/upstream_tests/test_link_pairing_transcript.py  esp32/tests/test_link_pairing_transcript.py
4fac87ed05ad0c9a76c198a92b95bb0c2a478629e2c986aad8231480841fa12c  muse_pair/test_host/upstream_tests/vectors/link_pairing_v5.json  esp32/tests/vectors/link_pairing_v5.json
1921a62dfc122643c586456acded95e000e755e004c0cf37543bf1d1611d641f  muse_link/test_host/upstream_tests/host_compat.h  esp32/tests/host_compat.h
9dd2351fc10adfd0b86825146eba18beb02432a570dd9f545b2bd831ee9fb7a8  muse_link/test_host/upstream_tests/link_fakes/cJSON.c  esp32/tests/link_fakes/cJSON.c
1b4c5181b648949803677d0f54764ce5c50f805d26e3ea5f6049e504df0b21dc  muse_link/test_host/upstream_tests/link_fakes/cJSON.h  esp32/tests/link_fakes/cJSON.h
df64d1253a31880ad0fcba36de5e79c926ff06acbf6816ca204aa5ae2ad52366  muse_link/test_host/upstream_tests/link_fakes/esp_app_desc.h  esp32/tests/link_fakes/esp_app_desc.h
0570c043ae0c97681ed46c440b11f92f208cc03495e5695515f3e363123d0d51  muse_link/test_host/upstream_tests/link_fakes/esp_crt_bundle.h  esp32/tests/link_fakes/esp_crt_bundle.h
79142755ed9fced8a4d1abf26030ce0a7da1f7f126adadda62c1fef32b49a28a  muse_link/test_host/upstream_tests/link_fakes/esp_efuse.h  esp32/tests/link_fakes/esp_efuse.h
c71534f9cd14c4a9741eb35038a9d08f2a73063a7fcda7d473398b40aed5aecc  muse_link/test_host/upstream_tests/link_fakes/esp_efuse_table.h  esp32/tests/link_fakes/esp_efuse_table.h
fa7261644caf1d02073e9fea42582f548c3670d96dc5ef9146ab5e74595efd99  muse_link/test_host/upstream_tests/link_fakes/esp_heap_caps.h  esp32/tests/link_fakes/esp_heap_caps.h
f838150a0bc845b4af58aa34129e2c798c6aec94aa814d5edc152bbbfd203d9e  muse_link/test_host/upstream_tests/link_fakes/esp_http_client.h  esp32/tests/link_fakes/esp_http_client.h
a4c4ef644bf3b14eff81a7796ad668e52982d842deeae3fbd7d3c6568e083c35  muse_link/test_host/upstream_tests/link_fakes/esp_https_ota.h  esp32/tests/link_fakes/esp_https_ota.h
a376dafe7c7e7956a4529f5a2a651d8815cfa43dfb9bb20003568c2101189dc7  muse_link/test_host/upstream_tests/link_fakes/esp_log.h  esp32/tests/link_fakes/esp_log.h
2a5c826c7fff9d2239fecd12b02ca4b4add7b07d2d741f026546b5b49111874c  muse_link/test_host/upstream_tests/link_fakes/esp_log_write.h  esp32/tests/link_fakes/esp_log_write.h
878c2cb75be5ae407cc304793d4b275610ca7398100aabf1632b217a885834cc  muse_link/test_host/upstream_tests/link_fakes/esp_netif.h  esp32/tests/link_fakes/esp_netif.h
157c2ee1057d5942625ec65e633956e7184978d8d4337b4feeab1c92fbb9dd90  muse_link/test_host/upstream_tests/link_fakes/esp_ota_ops.h  esp32/tests/link_fakes/esp_ota_ops.h
452f7881cb452d95ccf64d2483acf3cd7b21de7256aa1a2122f980804782db1f  muse_link/test_host/upstream_tests/link_fakes/esp_partition.h  esp32/tests/link_fakes/esp_partition.h
77aee85d1eceaaf562c58ccc79e7794411c1215f8af4520410ee10bed26740a0  muse_link/test_host/upstream_tests/link_fakes/esp_system.h  esp32/tests/link_fakes/esp_system.h
de198e56182434a75d1850a0a34b0f215ae990598d839b239728424d3e50a27a  muse_link/test_host/upstream_tests/link_fakes/esp_timer.h  esp32/tests/link_fakes/esp_timer.h
283521b245580f3e8557294a6e55aab3ee590c36a4f390014505f3d1843bcb21  muse_link/test_host/upstream_tests/link_fakes/esp_tls.h  esp32/tests/link_fakes/esp_tls.h
9d61dd768fcdc87c1640a72af74c90826aa167704bb72f6ba8cb18ee9d3b612a  muse_link/test_host/upstream_tests/link_fakes/esp_wifi.h  esp32/tests/link_fakes/esp_wifi.h
e343e2cad5abae38117795e41cf5c5f9616f1cd489da9b931b594e46782fa943  muse_link/test_host/upstream_tests/link_fakes/freertos/FreeRTOS.h  esp32/tests/link_fakes/freertos/FreeRTOS.h
d84a277c1d3918bc6c5b5684caaabba0e17eb5d8b338d5f3b42d5afe2f3ce051  muse_link/test_host/upstream_tests/link_fakes/freertos/queue.h  esp32/tests/link_fakes/freertos/queue.h
39768fb476d404d9fca2171eae92e6da42e463ac0ba85252559bd4ffb3dd0df6  muse_link/test_host/upstream_tests/link_fakes/freertos/semphr.h  esp32/tests/link_fakes/freertos/semphr.h
0330ef7086c7618f78e30ac990508ebf54d0e1e9f64fcc61f1239c00ce0fffd1  muse_link/test_host/upstream_tests/link_fakes/freertos/task.h  esp32/tests/link_fakes/freertos/task.h
06d18e9ca84d42ed797553f691da5f320af467aeb191d89cbf7f64f588d17073  muse_link/test_host/upstream_tests/link_fakes/lwip/ip4_addr.h  esp32/tests/link_fakes/lwip/ip4_addr.h
e18b767d739fb453f5e974034ef07b45968eb0fe666e3ecbcf4be16240ceb25a  muse_link/test_host/upstream_tests/link_fakes/lwip/sockets.h  esp32/tests/link_fakes/lwip/sockets.h
aae8e02345b92321f0b69143ee1c06c64d5f254f12878e187c9f16923fd82430  muse_link/test_host/upstream_tests/link_fakes/mbedtls/base64.h  esp32/tests/link_fakes/mbedtls/base64.h
9d66677d8235056f38b5a2799bcc7af5915f7b8d4d3cd5d819821b406bdf5d71  muse_link/test_host/upstream_tests/link_fakes/nvs_flash.h  esp32/tests/link_fakes/nvs_flash.h
73ec2c14c798690959808c4e6262bcacbb7f3d33064035cc22dac004fd14d113  muse_link/test_host/upstream_tests/link_fakes/nvs.h  esp32/tests/link_fakes/nvs.h
ef6822a53a5e88d505190b78201feb8c6a799255fe16dec791e480abbc902c52  muse_link/test_host/upstream_tests/link_fakes/stack_monitor.c  esp32/tests/link_fakes/stack_monitor.c
04b2b02c0a5a7f58f80e98900bd2e85200cfd2f4c01101a6bfbbb5f622e18543  muse_link/test_host/upstream_tests/link_vm_api_harness.c  esp32/tests/link_vm_api_harness.c
84834834f6d93cafdf6785b9a0c7771f01f81864ed33616e6259047cad81757a  muse_link/test_host/upstream_tests/link_vm_connect_harness.c  esp32/tests/link_vm_connect_harness.c
0b9174e2d9fa6d9d68431ef83a4cadf106c3de0b10ae1d3b2db9ca7b6469d523  muse_link/test_host/upstream_tests/muse_account/muse_account_api.c  esp32/components/muse/muse_account_api.c
5b9bb5953598f47ff3621fd87a915078af66cc2b2572558e37f84568a82d8f0a  muse_link/test_host/upstream_tests/muse_account/muse_account_api.h  esp32/components/muse/muse_account_api.h
3a2fb502012722d38f550dbe551b780d9d517b032831dc8cfc431e34b950874e  muse_link/test_host/upstream_tests/test_link_vm_api.py  esp32/tests/test_link_vm_api.py
1e515808837146d116d00f3719f3cd2fcc38a1b01bb01aa0bde443942d37d4cc  muse_link/test_host/upstream_tests/test_link_vm_auth_token_contract.py  esp32/tests/test_link_vm_auth_token_contract.py
972feaf8559f9326786916376e82110aa2ca55bfa487c420b8da3a63e0907930  noise_core/include/xplat/noise/core/BufferWriter.h  esp32/components/noise_core/include/xplat/noise/core/BufferWriter.h
2ae7ee4c2bcce91570099e9487e3d6bd19be352c1ef858149b244ba0cf4db633  noise_core/include/xplat/noise/core/ClientSession.h  esp32/components/noise_core/include/xplat/noise/core/ClientSession.h
c28b06fa0815087a6427f20a51d5ad148d08756f7c793ad663a9d2e313d2f171  noise_core/include/xplat/noise/core/CryptoBackend.h  esp32/components/noise_core/include/xplat/noise/core/CryptoBackend.h
74f2928e6142a9ede3759cb9137b0692cfe6d672dd2a767eebd424807075895c  noise_core/include/xplat/noise/core/InitiatorHandshake.h  esp32/components/noise_core/include/xplat/noise/core/InitiatorHandshake.h
0b196889acc2f71f19f48953f2ee3dad850e090f86bf3439f8c5683a2d6bb94a  noise_core/include/xplat/noise/core/MbedtlsCryptoBackend.h  esp32/components/noise_core/include/xplat/noise/core/MbedtlsCryptoBackend.h
4bc2d47c4ea2198fc38eabb912d962f0776fe7351de81716e3c6dfc612b8a65c  noise_core/include/xplat/noise/core/PsaCryptoBackend.h  esp32/components/noise_core/include/xplat/noise/core/PsaCryptoBackend.h
b056e9ce05525c62db03d3d01c79d6548521a4e797e9fc8f9e036af668609022  noise_core/include/xplat/noise/core/Secret.h  esp32/components/noise_core/include/xplat/noise/core/Secret.h
835063608b2224ff44fc89c3746bf9e4d1112c3548865478ffb617455f3e8cd4  noise_core/include/xplat/noise/core/ServiceCodec.h  esp32/components/noise_core/include/xplat/noise/core/ServiceCodec.h
f5f6d856319a4ce62f2df4d648ff07215add04ea9c303e9b972cd13b27a814dc  noise_core/include/xplat/noise/core/Span.h  esp32/components/noise_core/include/xplat/noise/core/Span.h
a7395c45e10fe4cf0dfa04b53ae5802ab9a20dff9ada235d2a342405f46cbacb  noise_core/include/xplat/noise/core/Status.h  esp32/components/noise_core/include/xplat/noise/core/Status.h
4670068ad88f4907f4f920613cb9f4fa22412305ebff2d72229cb3d997921bcd  noise_core/include/xplat/noise/core/StringView.h  esp32/components/noise_core/include/xplat/noise/core/StringView.h
f781865686e673ee32aa1b871652401ee83ca29182d8a5705faf128a299b3fa2  noise_core/include/xplat/noise/core/TransportFrameCodec.h  esp32/components/noise_core/include/xplat/noise/core/TransportFrameCodec.h
b87336afce047afbb9382231589ed8f30c6ba9bdff27ed7923e10fae0a3eeb9d  noise_core/include/xplat/noise/core/Transport.h  esp32/components/noise_core/include/xplat/noise/core/Transport.h
d2d7d6fe28b1c28138aec40aae0d0be41363c4e1c650af0c1a85e7d7f7c8eafd  noise_core/src/ClientSession.cpp  esp32/components/noise_core/src/ClientSession.cpp
2b4bf08d0e6f165f36511c6672e267bca522c79c5ab52ac0ff99350fb1885ee7  noise_core/src/InitiatorHandshake.cpp  esp32/components/noise_core/src/InitiatorHandshake.cpp
141325e55a4b5bbe04c72275a1777ac6fd08c0ed06bc95a95e427fd389f47ac5  noise_core/src/MbedtlsCryptoBackend.cpp  esp32/components/noise_core/src/MbedtlsCryptoBackend.cpp
ff131937a9fa50501edf949cc03a328cdc5f91c7f00b38f90b758ed4f4d763c3  noise_core/src/PsaCryptoBackend.cpp  esp32/components/noise_core/src/PsaCryptoBackend.cpp
de7e6db82085e1c3da8112400759b984b1c9407abb4f8c8431ac4c638822cb7d  noise_core/src/ServiceCodec.cpp  esp32/components/noise_core/src/ServiceCodec.cpp
f41b83599b6da82b12ee84ad74e1a7c7516251aed4ba198b4f1045e32b266988  noise_core/src/Status.cpp  esp32/components/noise_core/src/Status.cpp
83a16b0ea55e21ed14d6a33e150f4717bbdb7f1a4fda48a43a0a375de414197c  noise_core/src/Transport.cpp  esp32/components/noise_core/src/Transport.cpp
336a34d0d32c8e7503b40a5460ac4b7d473071e8e654e4307c92f44a4066d837  noise_core/src/TransportFrameCodec.cpp  esp32/components/noise_core/src/TransportFrameCodec.cpp
a9bc0cc3803906c60883e582f617506199d758227b71e9205db056afbdea66a7  noise_core/CMakeLists.txt  esp32/components/noise_core/CMakeLists.txt  modified
0353a8ae950232a70f0379a4b8a10b661b2f3e3a598e9411332fc0d4f98947fa  noise_core/LICENSE  LICENSE
2d382ddfd67c0011b0e2bb0cb359190d8ad9a1a7330ec65b1a170760bebf8905  noise_core/test_host/upstream_tests/noise_core_harness.cpp  esp32/tests/noise_core_harness.cpp
28bc11558cd2eb4ce2cec965e940b553a9c9c3bd00006db8dca504c66203d8fe  noise_core/test_host/upstream_tests/noise_core_inplace_harness.cpp  esp32/tests/noise_core_inplace_harness.cpp
3b4e8231c9203909c9a57dde5e946bc5de0296b8a5a69c5ac45481854291a9fe  noise_core/test_host/upstream_tests/noise_session_inplace_harness.cpp  esp32/tests/noise_session_inplace_harness.cpp
d4f49941ab274fee786b076b3edab89e33b24d3ebe03ecb9a23347873d4ba4ba  noise_core/test_host/upstream_tests/test_noise_core_inplace.py  esp32/tests/test_noise_core_inplace.py
a7b09500dd8982f61e70d1159e8d91698a546fded7d3c629080c37dc3eb17bb7  noise_core/test_host/upstream_tests/test_noise_core.py  esp32/tests/test_noise_core.py
e90e79cf87f8e44b3b6da095e70dbb1c66bd441eeef753cd1c426acecc7d5157  noise_core/test_host/linux_sdk/noise_xx.py  linux/src/musegadget/noise/noise_xx.py
bdeefc12dba2332d31bd580cdc5d24cc59c34e9881200aeb54f47abe089fd32a  muse_link/include/noise_upgrade.h  esp32/main/noise_upgrade.h
4041138a7add267b03a8bdad2355c37d1392bd1ffabf800189e0182f3ead5c29  muse_link/include/ws_mask.h  esp32/main/ws_mask.h
443a67f064fa64d3d4611fbd896226deded235f6a0756fcd57785b06c28a5617  muse_link/test_host/upstream_tests/link_noise_upgrade_harness.c  esp32/tests/link_noise_upgrade_harness.c
d184b2ccfc6e19515879118d3fef350f4e5806110d0b05edf9a831888ae679d0  muse_link/test_host/upstream_tests/ws_mask_harness.c  esp32/tests/ws_mask_harness.c
53fc7363b27d2c688fd870f0612a54a5078c8f62afb9d494ec374ed42279cc5f  muse_link/test_host/upstream_tests/test_ws_mask.py  esp32/tests/test_ws_mask.py
632035dda3db788ad1bf1c7cea0a53ff67eec720b22c3ba0182bd453786ea134  muse_link/test_host/upstream_tests/test_link_noise_session.py  esp32/tests/test_link_noise_session.py
```
