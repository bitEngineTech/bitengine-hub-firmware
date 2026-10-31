#!/usr/bin/env python3
"""给 managed_components/espressif__esp_matter 打补丁（docs/15_muse_direct_design_v1.md 9.8a 第 7 条、9.8b 第 11 条；M7 第一步）。

── 为什么需要 ────────────────────────────────────────────────────────────
1. browse_base_type —— connectedhomeip 的 ESP32 平台 DNS-SD（src/platform/ESP32/ESP32DnssdImpl.cpp）
   浏览上下文的 mType 只有 kDnssdTypeMaxSize（8）+ 1 字节。按 discriminator 子类型浏览时 CHIP 传进来的是
   "_S2._sub._matterc"，被截成 "_S2._sub"；浏览完成（OnBrowseDone）拿它去查本机代播的服务（mdns_lookup_delegated_service），
   永远查不到。而网关自己就是 Thread 边界路由器：设备的待配网记录正是本机经 SRP 代播的 —— 按配网码配网于是必然
   "Discovery timed out"（G0-5c 上板实测）。改成上下文里存基础类型 "_matterc"：网络查询照旧用带子类型的全名，
   本机代播查回全部待配网服务，按 discriminator 过滤在 CHIP 上层（SetUpCodePairer 比对 TXT 的 D）做。
3. no_test_group_data —— matter_controller_client::setup_commissioner() 无条件把 CHIP 的测试组数据（三个组、三套用公开测试密钥的
   组密钥，GroupTesting::InitData）和公开的默认 IPK（"temporary ipk 01"）写进控制器自己这一侧的 fabric。我们的 IPK 是
   随机的、发给设备的就是它（components/matter_ctrl/mc_issuer.cpp），控制器这一侧必须是同一个值，否则 CASE 建不起来：
   去掉这两段，由 matter_ctrl 在 setup_commissioner 之后用 SetSingleIpkEpochKey 写入（docs/15 设计 9.3；CHECKLIST MX04）。
2. no_operational_advertising —— esp_matter 的 matter_controller_client 把 m_operational_advertising 写死成 true、私有、
   没有 setter。为 true 时 CHIP 宣告控制器自己，EspDnssdPublishService 先 mdns_hostname_set()，上板实测把网关的
   bitengine.local 改成了 3844BE123457.local（9.8a 第 7 条）。client-only 控制器不需要被别人发现，改成 false；
   复测主机名回到 bitengine、Thread 设备的代播记录不受影响。

其余三处 G0-5 撞出来的问题不用改 esp_matter 的文件（9.8a 第 4–6、9 条）：components/matter_ctrl/CMakeLists.txt
（加回 ESP32DnssdImpl.cpp、不编片内 EMAC 驱动、PeerAddress 兼容头）与 components/matter_ctrl/Kconfig（GPIO_RANGE_MAX）。

── 用法 ──────────────────────────────────────────────────────────────────
  python3 tools/patch_esp_matter.py --component-dir managed_components/espressif__esp_matter          # 打（幂等）
  python3 tools/patch_esp_matter.py --component-dir managed_components/espressif__esp_matter --check  # 只查
  python3 tools/patch_esp_matter.py --self-test                                                       # 变异自测
顶层 CMakeLists.txt 在直连型、开了 BITENGINE_ENABLE_MATTER_CTRL 时于构建期调用它（同 patch_mdns.py / patch_mqtt.py：
managed_components/ 被 .gitignore 挡着，改在那里不入库、fullclean 即失）。用原文精确匹配，上游一改就报错停下，不会打歪。
升级 esp_matter（components/matter_ctrl/idf_component.yml 钉着版本）时先读新版的这两处再改这里。
"""
import argparse
import pathlib
import sys
import tempfile

MARK = "BITENGINE_PATCH_M7"

DNSSD = "connectedhomeip/connectedhomeip/src/platform/ESP32/ESP32DnssdImpl.cpp"
CLIENT_H = "components/esp_matter_controller/core/esp_matter_controller_client.h"
CLIENT_CPP = "components/esp_matter_controller/core/esp_matter_controller_client.cpp"

PATCHES = [
    (
        "browse_base_type",
        DNSSD,
        "    BrowseContext * ctx =\n"
        "        chip::Platform::New<BrowseContext>(type, protocol, interface, queryHandle, addressType, callback, context);\n",
        f"    /* {MARK}（tools/patch_esp_matter.py）：上下文的 mType 只有 kDnssdTypeMaxSize + 1 字节，装不下 \"_S2._sub._matterc\"，\n"
        "       被截成 \"_S2._sub\"，OnBrowseDone 查本机代播就永远查不到。存基础类型；按 discriminator 过滤在 CHIP 上层 */\n"
        "    const char * subtypeEnd = strstr(type, \"._sub.\");\n"
        "    BrowseContext * ctx     = chip::Platform::New<BrowseContext>(subtypeEnd ? subtypeEnd + 6 : type, protocol, interface,\n"
        "                                                                 queryHandle, addressType, callback, context);\n",
    ),
    (
        "no_operational_advertising",
        CLIENT_H,
        "    bool m_operational_advertising = true;\n",
        f"    /* {MARK}（tools/patch_esp_matter.py）：宣告控制器自己会改掉网关的 mDNS 主机名（docs/15 设计 9.8a 第 7 条） */\n"
        "    bool m_operational_advertising = false;\n",
    ),
    (
        "no_test_group_data",
        CLIENT_CPP,
        "    ESP_RETURN_ON_FALSE(chip::GroupTesting::InitData(group_data_provider, fabric_index, compressed_fabric_id_span) ==\n"
        "                        CHIP_NO_ERROR,\n"
        "                        ESP_FAIL, TAG, \"Failed to initialize group data\");\n"
        "    chip::ByteSpan default_ipk = chip::GroupTesting::DefaultIpkValue::GetDefaultIpk();\n"
        "    ESP_RETURN_ON_FALSE(chip::Credentials::SetSingleIpkEpochKey(group_data_provider, fabric_index, default_ipk,\n"
        "                                                                compressed_fabric_id_span) == CHIP_NO_ERROR,\n"
        "                        ESP_FAIL, TAG, \"Failed to set ipk for commissioner fabric\");\n",
        f"    /* {MARK}（tools/patch_esp_matter.py）：不写 CHIP 的测试组数据和公开的默认 IPK —— matter_ctrl 在 setup_commissioner 之后\n"
        "       写入网关自己的随机 IPK（发给设备的就是它，components/matter_ctrl/mc_issuer.cpp） */\n"
        "    (void)group_data_provider;\n",
    ),
]


def run(component_dir: pathlib.Path, check_only: bool) -> int:
    files = {}
    for _, rel, _, _ in PATCHES:
        p = component_dir / rel
        if not p.is_file():
            print(f"patch_esp_matter: 找不到 {p}", file=sys.stderr)
            return 1
        files[rel] = p.read_text(encoding="utf-8")

    done = [name for name, rel, _, new in PATCHES if files[rel].count(new) == 1]
    if len(done) == len(PATCHES):
        print(f"patch_esp_matter: 已打（{len(PATCHES)} 处：浏览存基础类型、控制器不宣告自己、不写测试组数据 / 默认 IPK）")
        return 0
    if check_only:
        print(f"patch_esp_matter: 未打或不完整（已打：{done or '无'}）", file=sys.stderr)
        return 1

    # 各处补丁互相独立、每处是一次整段替换：已打的跳过，没打的原文必须恰好匹配一次才补（这里加了新的一处时，
    # 已经打过旧几处的 managed_components 不用删了重下）。哪一处既不是已打、也对不上原文 —— 上游改了或文件被别的东西动过 —— 就停下
    for name, rel, old, new in PATCHES:
        if name in done:
            continue
        n = files[rel].count(old)
        if n != 1:
            print(f"patch_esp_matter: 「{name}」原文在 {rel} 里匹配到 {n} 处（应为 1）—— 上游 esp_matter 改了（或文件被动过），"
                  f"先读新版再改补丁；已打：{done or '无'}", file=sys.stderr)
            return 1
        files[rel] = files[rel].replace(old, new)
    for rel, text in files.items():
        (component_dir / rel).write_text(text, encoding="utf-8")
    print(f"patch_esp_matter: 已打 {len(PATCHES)} 处（浏览存基础类型、控制器不宣告自己、不写测试组数据 / 默认 IPK）")
    return 0


def self_test() -> int:
    """在临时目录里造两份"原文"，验：打一次成功、再打幂等、--check 认得、原文对不上报错、只打了一半报错。"""
    bad = 0

    def make(tmp: pathlib.Path, mutate=None):
        for name, rel, old, _ in PATCHES:
            p = tmp / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            body = f"// head\n{old}// tail\n"
            if mutate and mutate[0] == name:
                body = body.replace(old, mutate[1](old))
            p.write_text(body, encoding="utf-8")

    def expect(what, got, want):
        nonlocal bad
        ok = (got == 0) == want
        print(f"{'✅' if ok else '❌'} 自测：{what}")
        bad += 0 if ok else 1

    with tempfile.TemporaryDirectory() as d:
        t = pathlib.Path(d)
        make(t)
        expect("没打时 --check 报未打", run(t, True), False)
        expect("打一次成功", run(t, False), True)
        expect("再打一次幂等", run(t, False), True)
        expect("打完 --check 通过", run(t, True), True)
        expect("内容确实换了", 0 if all(f"{MARK}" in (t / rel).read_text() for _, rel, _, _ in PATCHES) else 1, True)
    for name, _, _, _ in PATCHES:
        with tempfile.TemporaryDirectory() as d:
            t = pathlib.Path(d)
            make(t, (name, lambda s: s[:10] + "X" + s[10:]))   # 在原文中间插一个字：模拟上游改了这行
            expect(f"「{name}」原文对不上时报错停下", run(t, False), False)
    with tempfile.TemporaryDirectory() as d:
        t = pathlib.Path(d)
        make(t)
        name, rel, old, new = PATCHES[0]
        p = t / rel
        p.write_text(p.read_text().replace(old, new))
        expect("只打了一部分时补齐剩下的", run(t, False), True)
        expect("补齐后 --check 通过", run(t, True), True)
        expect("补齐时已打的那处没被打第二遍", 0 if (t / rel).read_text().count(new) == 1 else 1, True)
    with tempfile.TemporaryDirectory() as d:
        t = pathlib.Path(d)
        make(t)
        name, rel, old, new = PATCHES[0]
        p = t / rel
        p.write_text(p.read_text().replace(old, new))
        name2, rel2, old2, _ = PATCHES[1]
        p2 = t / rel2
        p2.write_text(p2.read_text().replace(old2, old2[:10] + "X" + old2[10:]))
        expect("打了一部分、剩下的原文对不上时报错停下", run(t, False), False)
    with tempfile.TemporaryDirectory() as d:
        expect("组件目录不对时报错", run(pathlib.Path(d), False), False)
    return 0 if bad == 0 else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--component-dir", help="managed_components/espressif__esp_matter 的路径")
    ap.add_argument("--check", action="store_true", help="只检查是否已打，不改文件")
    ap.add_argument("--self-test", action="store_true", help="变异自测")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if not a.component_dir:
        ap.error("要 --component-dir")
    return run(pathlib.Path(a.component_dir), a.check)


if __name__ == "__main__":
    sys.exit(main())
