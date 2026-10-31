#!/usr/bin/env python3
"""tools/make_manifest.py —— 生成云端 OTA 的 manifest.json（先验签，再生成；app 与前端成对）

用法：
    python3 tools/make_manifest.py build/bitengine-v0.5.0-210-geb9299c.bin \
        --notes "一行更新说明" [--out dist/ota] [--base https://updates.bitengine.tech/hub/] \
        [--www build/www.bin | --no-www]

产出 <out>/manifest.json、固件副本、前端副本 www-<ver>.bin；把 <out>/ 整目录放到服务器的 /hub/ 下即可。

两种产品配置（docs/15 设计 2.7，防串刷第 1 层）：--profile ha（默认）/ direct。直连型的文件名是
bitengine-direct-v<ver>.bin、基址默认 /hub-direct/、manifest 带 "profile"。文件名、镜像里的工程名
（esp_app_desc_t.project_name）与 --profile 三者不一致就拒绝生成 —— 两种固件同一把签名钥匙，验签挡不住
把直连型发进 HA 型的 manifest。设备侧还有第 2 层（OTA 写入前比对工程名），但现场还没升级的设备没有它。

为什么要有这个脚本：
  * manifest 手写过一次就会错一次 —— size 抄错设备下载完直接拒装，version 与固件内
    fw_version 不一致设备会"永远有更新"或"永远没更新"。
  * 传错 bitengine-unsigned.bin 设备也会拒装（失败方向安全），但用户只看到"更新失败"。
    所以这里先跑 tools/signing_key.sh verify，未签名镜像直接拒绝生成。
  * 设备认 version / url / size / notes 与 www{version,url,size,sha256}（其余字段忽略）；
    顶层额外写 sha256 供人核对。
  * 版本比较规则见 components/ota_cloud/version_cmp.c：version 必须是 git describe 形态
    vX.Y.Z[-N-gHASH]，且严格大于设备当前版本才会装（防降级，签名拦不住"推旧的合法固件"）。
  * 前端（www.bin）与 app 成对下发（2026-09-10）：app 与前端之间没有兼容性检查，只推 app
    会让用户设备上 app 新、前端旧。默认自动带上固件旁边的 build/www.bin 与
    build/www_image/version.json 里的版本串（设备用它与运行中的 /www/version.json 逐字比较）。
    前端没有签名机制，靠 HTTPS + 这里算的 sha256。
"""
import argparse, hashlib, json, os, re, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# 每种配置：镜像工程名（= 文件名前缀）、默认下载基址（与该配置 sdkconfig 里的 CONFIG_BITENGINE_OTA_CLOUD_URL 同目录）、
# 它的 sdkconfig（读分区表文件名，算 www 分区大小）
PROFILES = {
    "ha":     {"project": "bitengine",        "base": "https://updates.bitengine.tech/hub/",        "sdkconfig": "sdkconfig"},
    "direct": {"project": "bitengine-direct", "base": "https://updates.bitengine.tech/hub-direct/", "sdkconfig": "sdkconfig.direct"},
}
VER_PART = r"(v\d+\.\d+\.\d+(?:-\d+-g[0-9a-f]+)?)"

def die(msg):
    print("❌ " + msg, file=sys.stderr); sys.exit(1)

def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

def image_project_name(path):
    """镜像里的工程名（esp_app_desc_t.project_name，偏移 80、32 字节）—— 与设备侧 components/ota_cloud/ota_profile.h
    同一套偏移；magic 不对就当读不出（返回空串）。"""
    with open(path, "rb") as f:
        d = f.read(112)
    if len(d) < 112 or d[0] != 0xE9 or int.from_bytes(d[32:36], "little") != 0xABCD5432:
        return ""
    return d[80:112].split(b"\0")[0].decode("ascii", "replace")

def partition_csv(profile):
    """该配置用的分区表文件：读它自己的 sdkconfig 里的 CONFIG_PARTITION_TABLE_CUSTOM_FILENAME，读不到退回 partitions.csv。"""
    try:
        for line in open(os.path.join(ROOT, PROFILES[profile]["sdkconfig"]), encoding="utf-8"):
            m = re.match(r'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="(.+)"', line.strip())
            if m:
                return m.group(1)
    except OSError:
        pass
    return "partitions.csv"

def www_partition_size(profile="ha"):
    """从该配置的分区表读 www 分区大小；读不到就用本板已知的 0x80000。"""
    try:
        for line in open(os.path.join(ROOT, partition_csv(profile)), encoding="utf-8"):
            cols = [c.strip() for c in line.split(",")]
            if len(cols) >= 5 and cols[0] == "www":
                return int(cols[4], 0)
    except OSError:
        pass
    return 0x80000

def copy_unless_same(src, dst):
    if os.path.abspath(dst) != os.path.abspath(src):   # 把 manifest 指回已发布过的文件时，源目标同文件
        shutil.copy2(src, dst)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("firmware", help="签名版固件，文件名须为 bitengine-v<ver>.bin（直连型 bitengine-direct-v<ver>.bin）")
    ap.add_argument("--profile", choices=sorted(PROFILES), default="ha",
                    help="产品配置：ha（默认，现有产品）/ direct（直连型）。决定文件名前缀、默认基址、manifest 的 profile 字段")
    ap.add_argument("--notes", default="", help="一行更新说明（设备原样显示，别放 HTML）")
    ap.add_argument("--out", default=None, help="输出目录，默认按 --profile：ha dist/ota、direct dist/ota-direct（分开放，"
                    "免得手工生成直连型时覆盖暂存的 HA 型 manifest）")
    ap.add_argument("--base", default=None, help="下载 URL 基址，默认按 --profile：" +
                    "、".join(f"{k} {v['base']}" for k, v in sorted(PROFILES.items())))
    ap.add_argument("--www", default=None, help="前端镜像 www.bin；默认取固件旁边的 www.bin")
    ap.add_argument("--www-version-json", default=None,
                    help="前端版本文件；默认取固件旁边的 www_image/version.json")
    ap.add_argument("--no-www", action="store_true", help="不带前端（只在明确知道前端没变时用）")
    ap.add_argument("--ncp", default=None, help="Zigbee 芯片固件 .gbl；默认 mg24/release/zigbee_ncp_uart_hw.gbl")
    ap.add_argument("--ncp-version", default=None, help="NCP 版本串；默认读 mg24/release/ncp_version.txt")
    ap.add_argument("--no-ncp", action="store_true", help="不带 Zigbee 芯片固件")
    a = ap.parse_args()

    fw = os.path.abspath(a.firmware)
    name = os.path.basename(fw)
    prof = PROFILES[a.profile]
    if "unsigned" in name:
        die(f"这是 {name}，签名构建的中间产物，设备会拒装")
    # HA 型的正则要求 "bitengine-" 后面紧跟 v，所以 bitengine-direct-v….bin 不会被当成 HA 型
    m = re.match(r"^" + re.escape(prof["project"]) + "-" + VER_PART + r"\.bin$", name)
    if not m:
        other = [k for k, v in PROFILES.items() if k != a.profile and re.match(r"^" + re.escape(v["project"]) + "-v", name)]
        if other:
            die(f"{name} 是 {other[0]} 型的固件，却要发进 --profile {a.profile} 的 manifest —— 两种固件同一把钥匙，设备验签挡不住，"
                f"装进去就是没有 Wi-Fi / 分区对不上，只能拆机（docs/15 设计 2.7）")
        die(f"文件名不是 {prof['project']}-v<ver>.bin 形态：{name}（设备用文件名里的版本比大小）")
    version = m.group(1)
    pname = image_project_name(fw)
    if pname != prof["project"]:
        die(f"镜像里的工程名是「{pname or '读不出'}」，--profile {a.profile} 要的是「{prof['project']}」—— "
            f"文件名对得上而内容是另一种固件（改过名？），不发")
    if a.base is None:
        a.base = prof["base"]
    if a.out is None:
        a.out = "dist/ota" if a.profile == "ha" else f"dist/ota-{a.profile}"
    if not a.base.startswith("https://"):
        die("基址必须是 https://（设备侧硬校验，http 直接拒绝发出请求）")
    if not a.base.endswith("/"):
        a.base += "/"
    # notes 的上限**必须**是设备侧那个缓冲区的大小，所以直接从头文件读，不抄第二份。
    # 2026-09-15 栽过：这里写死 200，而设备是 char notes[160]，一条 164 字节的中文
    # 说明顺利通过发布闸门，到设备上被 strlcpy 从第 159 字节砍断 —— 砍在一个 3 字节
    # 字符中间，/api/v1/ota/check 的回包不再是合法 UTF-8，严格解析器直接报错。
    # 设备侧现在会按字符边界截断（ota_cloud.c 的 utf8_safe_copy），但那是自保；
    # 说明被悄悄截掉半句话仍然是坏事，所以发布前就拦住。
    hdr = os.path.join(ROOT, "components", "ota_cloud", "include", "ota_cloud.h")
    m = re.search(r"char\s+notes\[(\d+)\]", open(hdr, encoding="utf-8").read())
    if not m:
        die(f"在 {hdr} 里找不到 notes[N] —— 字段改名了？上限不能靠猜")
    notes_max = int(m.group(1)) - 1          # 留一个 NUL
    if "\n" in a.notes:
        die("notes 只能一行")
    if len(a.notes.encode()) > notes_max:
        die(f"notes {len(a.notes.encode())} 字节，超过设备缓冲区上限 {notes_max} —— "
            f"设备会截断（中文按字节砍还会切坏字符）。请缩短。")

    # 先验签：用错钥匙或未签名的镜像不进 manifest
    r = subprocess.run(["bash", os.path.join(ROOT, "tools", "signing_key.sh"), "verify", fw],
                       capture_output=True, text=True)
    if r.returncode != 0:
        die("验签不通过，不生成 manifest：\n" + (r.stdout + r.stderr).strip())

    manifest = {
        "profile": a.profile,        # 设备忽略多余字段；给人和发布脚本核对用（防串刷第 1 层）
        "version": version,
        "url": a.base + name,
        "size": os.path.getsize(fw),
        "notes": a.notes,
        "sha256": sha256_of(fw),     # app 的这一项设备忽略（它验签）；给人核对用
    }

    # ---- 前端：默认自动带上 ----
    www = None
    if not a.no_www:
        fw_dir = os.path.dirname(fw)
        www = a.www or os.path.join(fw_dir, "www.bin")
        vjson = a.www_version_json or os.path.join(fw_dir, "www_image", "version.json")
        if not os.path.isfile(www):
            die(f"找不到前端镜像 {www}。要么给 --www，要么明确 --no-www（只在前端确实没变时）")
        if not os.path.isfile(vjson):
            die(f"找不到前端版本文件 {vjson}（idf.py build 会生成 build/www_image/version.json）")
        try:
            vmeta = json.load(open(vjson, encoding="utf-8"))
            www_version = vmeta["version"]
        except (OSError, ValueError, KeyError) as e:
            die(f"读不出前端版本：{vjson}: {e}")
        # 前端也分配置（docs/15 设计 2.7 A-29）：version.json 带 "profile":"direct" 的是直连型前端，没有这个字段是 HA 型。
        # 两种 www 分区一样大，大小检查拦不住串发；设备侧"只换前端"那条路也不比对（串了不会变砖，但界面全错）
        wprof = vmeta.get("profile", "ha")
        if wprof != a.profile:
            die(f"{vjson} 说这是 {wprof} 型的前端，却要发进 --profile {a.profile} 的 manifest —— 换成该配置构建目录里的 www.bin / version.json")
        # version.json 本身也打在 www.bin 里（SPIFFS 小文件落在一个数据页里、字节连续）：对不上说明两者不是同一次构建的
        try:
            vraw = open(vjson, "rb").read().strip()
            wraw = open(www, "rb").read()
        except OSError as e:
            die(f"读不了前端镜像：{e}")
        if vraw not in wraw:
            die(f"{www} 里找不到 {vjson} 的内容 —— 两者不是同一次构建的产物（--www 与 --www-version-json 配错了？）")
        size = os.path.getsize(www)
        part = www_partition_size(a.profile)
        if size != part:
            die(f"www.bin 是 {size} 字节，www 分区是 {part} 字节 —— 不是给这个分区生成的镜像，设备会拒")
        www_name = f"www-{version}.bin"      # 按固件版本命名，内容永不变，可长缓存
        manifest["www"] = {
            "version": www_version,
            "url": a.base + www_name,
            "size": size,
            "sha256": sha256_of(www),
        }

    # ---- Zigbee 芯片固件：默认带上 mg24/release 里的成品 ----
    # 设备按指纹（本网关自己刷过的镜像 sha256）判断要不要提示，相同就是空操作，所以每份
    # manifest 都带着它没有代价；版本串只在指纹缺失（产线 J-Link 首烧）时用来比较，
    # 必须与设备 EZSP 读到的完全一致，如 "8.2.2.0 build 532"。
    ncp = None
    if not a.no_ncp:
        ncp = a.ncp or os.path.join(ROOT, "mg24", "release", "zigbee_ncp_uart_hw.gbl")
        if not os.path.isfile(ncp):
            if a.ncp:
                die(f"找不到 {ncp}")
            ncp = None
    if ncp:
        vfile = os.path.join(os.path.dirname(ncp), "ncp_version.txt")
        ncp_version = a.ncp_version or (open(vfile, encoding="utf-8").read().strip() if os.path.isfile(vfile) else "")
        if not ncp_version:
            die("给 --ncp-version，或在 .gbl 旁边放 ncp_version.txt（设备在指纹缺失时用它比较，须与 /api/v1/status 的 ncp_version 逐字一致）")
        # NCP 镜像两条放行判据，正本在 mg24/ncp_image_check.py（产线套件脚本也调它）：不是擦除版（每次开机都擦 NVM3，
        # 发出去 = 每台网关每次开机丢网）、NVM3 缓存 ≥ 400（缓存只许升不许降：缓存 200 的老 NCP 刷到跑过直连子设备 64 的
        # 芯片上开机即崩，mg24/README.md「直连子设备 64：根因实测」）
        r = subprocess.run([sys.executable, os.path.join(ROOT, "mg24", "ncp_image_check.py"), ncp],
                           capture_output=True, text=True)
        if r.returncode != 0:
            die(f"NCP 镜像不能发布：\n{(r.stderr or r.stdout).strip()}")
        ncp_size = os.path.getsize(ncp)
        if not (128 <= ncp_size <= 1024 * 1024):
            die(f"{ncp} 是 {ncp_size} 字节，超出设备接受范围 [128, 1MB]")
        ncp_sha = sha256_of(ncp)
        sums = os.path.join(os.path.dirname(ncp), "SHA256SUMS")
        if os.path.isfile(sums) and ncp_sha not in open(sums, encoding="utf-8").read():
            die(f"{os.path.basename(ncp)} 的 sha256 不在 {sums} 里 —— release 目录不一致，先查那边")
        # 验签（T1.2，2026-09-14）：设备的云端路径永远只收官方签名的 .gbl，未签名的会在
        # 下载完之后才被拒（ncp_image_unsigned），用户只看到"更新失败"。所以这里先拒。
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "gbl_verify.py"), ncp],
                           capture_output=True, text=True)
        if r.returncode != 0:
            die("NCP 固件签名不过，不生成 manifest：\n" + (r.stdout + r.stderr).strip())
        ncp_name = "zigbee_ncp-" + re.sub(r"[^0-9A-Za-z.]+", "-", ncp_version) + ".gbl"
        # XNCP 构建串（T9.19 ⑦）：产线 J-Link 首烧的整机没有指纹，设备拿 NCP 报的构建串与它比 ——
        # EmberZNet 版本串所有 build 都一样，比不出来。取法与 ncp_image_check.py 同一份（它上面已经放行过这个镜像）。
        sys.path.insert(0, os.path.join(ROOT, "mg24"))
        import ncp_image_check
        mb = re.search(rb"bitEngine-xncp/[0-9.]+[\x20-\x7e]*", ncp_image_check.program_bytes(ncp))
        if not mb:
            die(f"{ncp} 里取不到 bitEngine-xncp 构建串")
        ncp_build = mb.group(0).decode("ascii")
        if "-dirty" in ncp_build:
            die(f"NCP 构建串「{ncp_build}」带 -dirty：不是从已提交的树构建的，构建串不能唯一标识这一版（设备靠它判断要不要提示更新）")
        if len(ncp_build) >= 96 or '"' in ncp_build or "\\" in ncp_build:
            die(f"构建串「{ncp_build}」超长或带引号 / 反斜杠，设备侧收不下（ota_cloud.h 的 ncp_build[96]）")
        manifest["ncp"] = {
            "version": ncp_version,
            "url": a.base + ncp_name,
            "size": ncp_size,
            "sha256": ncp_sha,
            "build": ncp_build,
        }

    body = json.dumps(manifest, ensure_ascii=False, indent=2) + "\n"
    if len(body.encode()) > 2048:
        die("manifest 超过 2KB（设备侧上限），notes 太长了")

    os.makedirs(a.out, exist_ok=True)
    copy_unless_same(fw, os.path.join(a.out, name))
    if www:
        copy_unless_same(www, os.path.join(a.out, www_name))
    if ncp:
        copy_unless_same(ncp, os.path.join(a.out, ncp_name))
    with open(os.path.join(a.out, "manifest.json"), "w", encoding="utf-8") as f:
        f.write(body)
    print(body, end="")
    print(f"✅ 写入 {a.out}/manifest.json + {name}" + (f" + {www_name}" if www else "（不带前端）")
          + (f" + {ncp_name}" if ncp else "（不带 NCP）") + "（验签通过）")
    print(f"   上传后设备侧应能取到：{a.base}manifest.json")

if __name__ == "__main__":
    main()
