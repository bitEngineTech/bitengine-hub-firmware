#!/usr/bin/env python3
"""HA 型 → 直连型换固件（docs/15_muse_direct_design_v1.md 8.12），以及按整片备份原样恢复。

    source ~/esp-idf-v6.0.2/export.sh
    python3 tools/migrate_profile.py to-direct -p /dev/ttyACM0      # 先整片备份，再换刷直连型
    python3 tools/migrate_profile.py restore   -p /dev/ttyACM0 --from <备份目录>/full_8mb.bin
    python3 tools/migrate_profile.py --self-test                     # 离线：合成镜像验判定逻辑，不碰板子

这是服务操作，不是用户自助步骤（设计 8.12）。两种配置的分区表不同，只能 USB 线刷：

| 区域 | HA 型 | 直连型 | 这里怎么做 |
|---|---|---|---|
| fctry（出厂身份） | 0x6B0000 | 0x7F0000 | **搬**：从备份里取 16 KB、解出序列号才往下走，随固件一起写到新位置，回读逐字节比对。旧位置落在直连型 ota_1 中间，不搬的话第一次 OTA 就把身份永久覆盖（红线 2） |
| 主 nvs | 0x9000，24 KB | 0x9000，64 KB | 前 24 KB **一个字节都不碰**（配置、管理密码、Thread 数据集都在里面），回读比对 |
| 0xF000–0x30000 | 旧 otadata / phy_init、ota_0 开头 | nvs 后 40 KB、otadata、phy、matter | **整段先擦**。设计表只写了擦 0xF000–0x19000；但直连型 matter（0x1C000–0x2C000）正压在 HA 型 ota_0（0x20000 起）的程序代码上，不擦就是一个塞满垃圾的 NVS 分区，CHIP 怎么对待它是未知数（E28） |
| hubdb / coredump | 无（HA 型这一段没用） | 0x7B0000–0x7F0000 | 擦掉，首次启动由 hub_store 初始化 |
| 其余 | —— | —— | 按 build-direct/flash_args 刷 bootloader、分区表、otadata、app、www |
| MG24 的 NVM3 | —— | —— | 不碰，Zigbee 网络还在 |

⚠️ 复位：整个过程只进**一次**下载模式（第一条 esptool 命令 `--after no-reset`，之后全部
`--before no-reset`），收尾也不复位，由操作员**完全断电重上电**（PoE 网线和 USB 都拔）。
AM1 上每一次芯片级复位都是 IO28 的抽奖（CLAUDE.md IO28 一节），刷完的硬复位正好撞上以太网在线；
理由与 2026-09-13 的两次实测见 generate_nvs.py 的「复位次数」一节。

⚠️ 写到一半失败（esptool 报错、USB 断了）：本板没有 BOOT 键，app 区残缺时 esptool 可能再也同步不上
（memory: board-no-boot-button），这时用 C5 内置的 USB-JTAG 把整片备份写回，不依赖下载模式：
    openocd -f board/esp32c5-builtin.cfg -c "init; reset halt; flash write_image erase <备份>/full_8mb.bin 0x0; reset run; exit"
写完照样完全断电重上电。

⚠️ 备份里有出厂身份、管理密码哈希与 Wi-Fi 密码：默认放 ~/bitengine-backups/，**不进仓库**。
恢复（restore）把整片 8 MB 原样写回，包括 HA 型分区表与 fctry 原位置 —— 换配置的实验做完就用它回 HA 型。

H1（esptool 5.x 的语义，2026-10-21 按 esptool 5.3.1 核对）：
 - read-flash / erase-region / write-flash 的地址与长度都是字节；erase-region 两者都要 4 KB 对齐；
 - write-flash 会先擦它要写的扇区，写完按 MD5 校验（stub 模式），不需要另擦；
 - `--flash-mode/--flash-freq/--flash-size keep`：不改写镜像头里的 Flash 参数（整片恢复时尤其要紧，
   0x2000 处的 bootloader 头原样写回）。
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import generate_nvs as gn  # noqa: E402  —— 分区表现算、序列号规则、MAC 规整只在那边一份

ROOT = gn.ROOT
FLASH_SIZE = 0x800000                       # ESP32-C5-WROOM-1U N8R8
DIRECT_PROJECT = b"bitengine-direct"         # 防串刷认的工程名（架构 5.8 R-9）
SIGBLOCK_SIZE = 4096                         # 签名档：镜像后面追加的 4 KB 签名块
SIGBLOCK_MAGIC = b"\xe7\x02"


class MigrateError(Exception):
    """判定不通过。CLI 打印后退出 1；--self-test 用它断言"该拒的拒了"。"""


# ── 分区表（二进制，0x8000 处） ─────────────────────────────────────────────

def parse_partition_table(blob: bytes) -> dict[str, tuple[int, int]]:
    """解 ESP-IDF 二进制分区表：每条 32 字节 = magic 0x50AA、type、subtype、offset、size、label[16]、flags。
    0xEBEB 是 MD5 条目，0xFFFF 是结束。解不出任何分区 → 空字典（调用方当作"认不出"）。"""
    parts: dict[str, tuple[int, int]] = {}
    for i in range(0, min(len(blob), 0xC00), 32):
        ent = blob[i:i + 32]
        if len(ent) < 32:
            break
        magic = struct.unpack_from("<H", ent, 0)[0]
        if magic == 0xEBEB:
            continue
        if magic != 0x50AA:
            break
        offset, size = struct.unpack_from("<II", ent, 4)
        label = ent[12:28].split(b"\0")[0].decode("ascii", "replace")
        parts[label] = (offset, size)
    return parts


def expected_layout(csv_name: str, sdkconfig_name: str) -> dict[str, tuple[int, int]]:
    """按 CSV 现算（与 generate_nvs.py 同一个函数）。HA 型没有 matter / hubdb，那几项就不出现。"""
    text = (ROOT / csv_name).read_text()
    out = {}
    for label in ("nvs", "otadata", "ota_0", "ota_1", "www", "coredump", "fctry", "matter", "hubdb"):
        if re.search(rf"^\s*{label}\s*,", text, re.M):
            out[label] = gn.partition_geometry(label, csv_name, sdkconfig_name)
    return out


def classify(table: dict[str, tuple[int, int]], ha: dict, direct: dict) -> str:
    """'ha' / 'direct' / 'unknown'。只认 fctry 与 nvs 的位置都对得上的。"""
    for name, ref in (("ha", ha), ("direct", direct)):
        if table.get("fctry") == ref["fctry"] and table.get("nvs") == ref["nvs"]:
            return name
    return "unknown"


# ── fctry ─────────────────────────────────────────────────────────────────

def nvs_tool() -> str:
    idf = os.environ.get("IDF_PATH") or str(Path.home() / "esp-idf-v6.0.2")
    p = Path(idf) / "components" / "nvs_flash" / "nvs_partition_tool" / "nvs_tool.py"
    if not p.exists():
        raise MigrateError(f"找不到 {p} —— 先 source ~/esp-idf-v6.0.2/export.sh")
    return str(p)


def fctry_serial(img: bytes) -> str:
    """从 fctry 分区镜像里解出 factory/serial_number，并按固件同一套规则校验。解不出就拒绝 ——
    搬一份解不出序列号的"身份"过去，等于在新位置放一块垃圾，而旧位置第一次 OTA 就没了。"""
    if img.count(0xFF) == len(img):
        raise MigrateError("fctry 是空的（全 0xFF）：这块板没写过出厂身份")
    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / "fctry.bin"
        p.write_bytes(img)
        r = subprocess.run([sys.executable, nvs_tool(), "-d", "minimal", "-f", "json", str(p)],
                           capture_output=True, text=True, timeout=60)
    try:
        entries = json.loads(r.stdout)
    except json.JSONDecodeError:
        raise MigrateError(f"fctry 解析失败（nvs_tool 输出不是 JSON）：{r.stdout[:200]} {r.stderr[:200]}")
    sns = [e.get("data") for e in entries
           if e.get("namespace") == gn.NVS_NAMESPACE and e.get("key") == "serial_number"
           and e.get("state", "Written") == "Written"]
    if len(sns) != 1 or not isinstance(sns[0], str):
        raise MigrateError(f"fctry 里找不到唯一的 {gn.NVS_NAMESPACE}/serial_number（找到 {len(sns)} 条）")
    sn = sns[0]
    if not sn or len(sn) >= 32 or not gn.SERIAL_RE.match(sn):
        raise MigrateError(f"fctry 里的序列号形状非法：{sn!r}")
    return sn


# ── 直连型构建产物 ─────────────────────────────────────────────────────────

def read_flash_args(build_dir: Path) -> list[tuple[int, Path]]:
    fa = build_dir / "flash_args"
    if not fa.exists():
        raise MigrateError(f"找不到 {fa} —— 先 bash tools/build_profiles.sh")
    out = []
    for line in fa.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("--"):
            continue
        off, rel = line.split(None, 1)
        out.append((int(off, 0), build_dir / rel))
    return out


def check_build(build_dir: Path, direct: dict) -> list[tuple[int, Path]]:
    """要刷的东西必须确实是直连型、签过名、分区表与 partitions_direct.csv 一致。"""
    items = read_flash_args(build_dir)
    for off, p in items:
        if not p.exists():
            raise MigrateError(f"flash_args 里的 {p} 不存在")
        if "unsigned" in p.name:
            raise MigrateError(f"flash_args 指向未签名镜像 {p.name}（红线 3：绝不能刷）")
    by_off = dict(items)

    pt = by_off.get(0x8000)
    if not pt:
        raise MigrateError("flash_args 里没有 0x8000 的分区表")
    table = parse_partition_table(pt.read_bytes())
    for label in ("nvs", "otadata", "ota_0", "fctry", "hubdb", "matter"):
        if table.get(label) != direct.get(label):
            raise MigrateError(f"构建出的分区表里 {label} = {table.get(label)}，"
                               f"partitions_direct.csv 算出 {direct.get(label)} —— 不是同一张表")

    app = by_off.get(direct["ota_0"][0])
    if not app:
        raise MigrateError(f"flash_args 里没有写到 ota_0（{direct['ota_0'][0]:#x}）的 app")
    img = app.read_bytes()
    if len(img) < 112 or img[0] != 0xE9 or struct.unpack_from("<I", img, 32)[0] != 0xABCD5432:
        raise MigrateError(f"{app.name} 不像 ESP 应用镜像（头 0xE9 / app_desc magic 对不上）")
    proj = img[80:112].split(b"\0")[0]
    if proj != DIRECT_PROJECT:
        raise MigrateError(f"{app.name} 的工程名是 {proj!r}，不是 {DIRECT_PROJECT!r} —— 拿错了构建目录？")
    if len(img) <= SIGBLOCK_SIZE or img[-SIGBLOCK_SIZE:-SIGBLOCK_SIZE + 2] != SIGBLOCK_MAGIC:
        raise MigrateError(f"{app.name} 末尾没有签名块 —— 未签名的镜像以后 OTA 不进来（红线 3）")
    return items


# ── 计划（纯函数：给定整片备份与构建产物，算出擦哪里、写什么） ───────────────────

def plan_to_direct(full: bytes, ha: dict, direct: dict, build_items: list[tuple[int, Path]]) -> dict:
    if len(full) != FLASH_SIZE:
        raise MigrateError(f"整片备份长度 {len(full):#x}，应为 {FLASH_SIZE:#x}")
    table = parse_partition_table(full[0x8000:0x9000])
    kind = classify(table, ha, direct)
    if kind == "direct":
        raise MigrateError("板上已经是直连型的分区表了 —— 不用再迁；要回 HA 型用 restore")
    if kind != "ha":
        raise MigrateError(f"认不出板上的分区表（{sorted(table)}）—— 不是 HA 型，不迁")

    fo, fs = ha["fctry"]
    fctry = full[fo:fo + fs]
    sn = fctry_serial(fctry)

    to, ts = direct["fctry"]
    if ts != fs:
        raise MigrateError(f"两张表的 fctry 大小不同（{fs:#x} vs {ts:#x}）")
    target = full[to:to + ts]
    if target.count(0xFF) != len(target) and target != fctry:
        raise MigrateError(f"直连型 fctry 的位置 {to:#x} 上已有别的数据，而且不是同一份身份 —— 不覆盖")

    nvs_o, nvs_s = ha["nvs"]
    dnvs_o, dnvs_s = direct["nvs"]
    if dnvs_o != nvs_o or dnvs_s < nvs_s:
        raise MigrateError("直连型 nvs 不再从 HA 型 nvs 的起点开始、或者变小了 —— 前 24 KB 原样保留的前提不成立")
    keep = (nvs_o, nvs_o + nvs_s)

    erases = [(keep[1], direct["ota_0"][0]),                 # 0xF000–0x30000
              (direct["hubdb"][0], direct["fctry"][0])]      # 0x7B0000–0x7F0000
    writes = [(off, p) for off, p in build_items] + [(to, None)]   # None = fctry（从备份取）

    # 不变量：任何擦写都不碰 HA 型 nvs 的那 24 KB；fctry 只写一次、写在直连型的位置
    for a, b in erases:
        if a % 0x1000 or b % 0x1000 or a >= b:
            raise MigrateError(f"擦除区 {a:#x}–{b:#x} 没有 4 KB 对齐或为空")
        if a < keep[1] and b > keep[0]:
            raise MigrateError(f"擦除区 {a:#x}–{b:#x} 压到了要保留的 nvs")
    for off, p in writes:
        size = fs if p is None else p.stat().st_size
        if off < keep[1] and off + size > keep[0]:
            raise MigrateError(f"写入 {off:#x}（{size} 字节）压到了要保留的 nvs")
    return {"serial": sn, "fctry": fctry, "fctry_at": to, "keep": keep, "erases": erases, "writes": writes}


# ── esptool（一次下载模式会话） ─────────────────────────────────────────────

READ_CHUNK = 0x40000          # 整片备份分段读（每段一次 esptool，同一个下载模式会话）
READ_BLOCK = 0x1000           # 某段失败时逐块重读
READ_PIECE = 0x400            # 某块还失败时再拆小


class Session:
    """一次下载模式会话（见文件头"复位"一节）。

    read() 分段读并带退路（2026-10-22 上板发现）：esptool 5.3.1 经 C5 内置的 USB-Serial/JTAG 读某些 4 KB 块时
    确定性地断（"No more data to read from the serial port"），换成 1 KB 读同一块就好 —— 跟块的内容有关，
    推测是转义后长度落在 64 字节整数倍、缺一个零长度包。同一块按 4 KB 读必失败，前后块与失败后的下一次读都正常，
    stub 也还活着。所以：先按 256 KB 一段读；某段失败就逐 4 KB 重读这一段；某块还失败就拆成 1 KB。"""

    def __init__(self, port: str):
        self.port = port
        self.entered = False
        self.workdir = Path(tempfile.mkdtemp(prefix="migrate_rd_"))

    def run(self, *args: str, timeout: int = 900) -> str:
        before = [] if not self.entered else ["--before", "no-reset"]
        cmd = ["esptool", "--chip", "esp32c5", "-p", self.port, *before, "--after", "no-reset", *args]
        print("   $ " + " ".join(cmd), flush=True)
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        except FileNotFoundError:
            raise MigrateError("找不到 esptool —— 先 source ~/esp-idf-v6.0.2/export.sh")
        if r.returncode != 0:
            raise MigrateError(f"esptool 失败（{r.returncode}）：\n{r.stdout[-1500:]}\n{r.stderr[-800:]}")
        self.entered = True
        return r.stdout

    def read_once(self, off: int, size: int) -> bytes:
        tmp = self.workdir / f"rd_{off:x}_{size:x}.bin"
        self.run("read-flash", hex(off), hex(size), str(tmp))
        data = tmp.read_bytes()
        tmp.unlink()
        if len(data) != size:
            raise MigrateError(f"读回长度 {len(data)}，应为 {size}")
        return data

    def read(self, off: int, size: int, out: Path) -> bytes:
        data = chunked_read(self.read_once, off, size)
        out.write_bytes(data)
        return data


def chunked_read(read_once, off: int, size: int) -> bytes:
    """按 READ_CHUNK 分段读；失败的段逐 READ_BLOCK 重读，失败的块拆成 READ_PIECE。最小的一片也失败才放弃。"""
    def tiered(o: int, n: int, step: int) -> bytes:
        parts = []
        for p in range(o, o + n, step):
            m = min(step, o + n - p)
            try:
                parts.append(read_once(p, m))
            except MigrateError:
                if step == READ_CHUNK:
                    print(f"   ↳ {p:#x}+{m:#x} 读失败，逐 4 KB 重读这一段", flush=True)
                    parts.append(tiered(p, m, READ_BLOCK))
                elif step == READ_BLOCK:
                    print(f"   ↳ {p:#x} 这一块按 4 KB 读不出来，拆成 1 KB", flush=True)
                    parts.append(tiered(p, m, READ_PIECE))
                else:
                    raise
        return b"".join(parts)
    data = tiered(off, size, READ_CHUNK)
    if len(data) != size:
        raise MigrateError(f"读回长度 {len(data)}，应为 {size}")
    return data


def read_mac(s: Session) -> str:
    m = re.search(r"MAC:\s*((?:[0-9a-fA-F]{2}:){5,7}[0-9a-fA-F]{2})", s.run("read-mac"))
    if not m:
        raise MigrateError("从 esptool 输出里认不出 MAC")
    return gn.normalize_mac(m.group(1))


def flash_size_ok(out: str) -> bool:
    m = re.search(r"Detected flash size:\s*(\d+)MB", out)
    return bool(m) and int(m.group(1)) * 1024 * 1024 == FLASH_SIZE


def power_cycle_hint() -> None:
    print("\n⚠️  现在**完全断电重上电**：PoE 网线和 USB 都拔掉，等 10 秒再插回。")
    print("    板子此刻停在下载模式（故意不复位，见脚本开头），不断电它不会自己起来。")


def cmd_to_direct(args) -> int:
    ha = expected_layout("partitions.csv", "sdkconfig")
    direct = expected_layout("partitions_direct.csv", "sdkconfig.direct")
    build_items = check_build(Path(args.build_dir).resolve(), direct)
    print(f"✅ 构建产物：{args.build_dir}（直连型、已签名、分区表与 partitions_direct.csv 一致）")

    s = Session(args.port)
    mac = read_mac(s)
    if not flash_size_ok(s.run("flash-id")):
        raise MigrateError("Flash 容量不是 8 MB —— 不是这块板？")
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    bdir = Path(args.backup_dir or Path.home() / "bitengine-backups" /
                f"migrate-{mac.replace(':', '')}-{stamp}").expanduser()
    bdir.mkdir(parents=True, exist_ok=True, mode=0o700)
    full_p = bdir / "full_8mb.bin"
    print(f"── ① 整片备份 → {full_p}（8 MB，约 1–2 分钟）")
    full = s.read(0, FLASH_SIZE, full_p)
    (bdir / "meta.json").write_text(json.dumps({
        "mac": mac, "when": stamp, "sha256": hashlib.sha256(full).hexdigest(), "size": len(full)}, indent=2))

    print("── ② 判定")
    plan = plan_to_direct(full, ha, direct, build_items)
    fctry_p = bdir / "fctry_from_ha.bin"
    fctry_p.write_bytes(plan["fctry"])
    print(f"   板上是 HA 型；出厂身份 {plan['serial']}（MAC {mac}），将搬到 {plan['fctry_at']:#x}")
    if args.dry_run:
        print("   --dry-run：到此为止，板子没写过任何东西")
        power_cycle_hint()
        return 0

    print("── ③ 擦除")
    for a, b in plan["erases"]:
        s.run("erase-region", hex(a), hex(b - a))
    print("── ④ 刷直连型 + 写 fctry")
    pairs = []
    for off, p in plan["writes"]:
        pairs += [hex(off), str(fctry_p if p is None else p)]
    s.run("write-flash", "--flash-mode", "keep", "--flash-freq", "keep", "--flash-size", "keep", *pairs)

    print("── ⑤ 回读校验")
    vdir = bdir / "verify"
    vdir.mkdir(exist_ok=True)
    k0, k1 = plan["keep"]
    if s.read(k0, k1 - k0, vdir / "nvs_kept.bin") != full[k0:k1]:
        raise MigrateError("主 nvs 前 24 KB 读回与备份不一致！用 restore 写回备份")
    fo = plan["fctry_at"]
    if s.read(fo, len(plan["fctry"]), vdir / "fctry_new.bin") != plan["fctry"]:
        raise MigrateError("新位置的 fctry 读回不一致！用 restore 写回备份")
    pt = parse_partition_table(s.read(0x8000, 0x1000, vdir / "pt.bin"))
    if classify(pt, ha, direct) != "direct":
        raise MigrateError("读回的分区表不是直连型的")
    for label in ("matter",):
        mo, ms = direct[label]
        if s.read(mo, ms, vdir / f"{label}.bin").count(0xFF) != ms:
            raise MigrateError(f"{label} 分区擦过了却不是全 0xFF")
    print(f"✅ 迁移完成：nvs 保留、fctry {plan['serial']} 已在 {fo:#x}、分区表是直连型、matter 是空的")
    print(f"   回 HA 型：python3 tools/migrate_profile.py restore -p {args.port} --from {full_p}")
    power_cycle_hint()
    return 0


def cmd_restore(args) -> int:
    src = Path(args.src).expanduser()
    full = src.read_bytes()
    if len(full) != FLASH_SIZE:
        raise MigrateError(f"{src} 长度 {len(full):#x}，不是整片 8 MB 备份")
    meta = src.with_name("meta.json")
    if meta.exists():
        m = json.loads(meta.read_text())
        if m.get("sha256") != hashlib.sha256(full).hexdigest():
            raise MigrateError("备份文件与 meta.json 里的 sha256 不符 —— 备份被改过或不完整")
    s = Session(args.port)
    mac = read_mac(s)
    if meta.exists() and json.loads(meta.read_text()).get("mac") not in (None, mac):
        raise MigrateError(f"这份备份来自另一块板（{json.loads(meta.read_text())['mac']}），本板是 {mac} —— "
                           "写回去会把那块板的出厂身份复制过来")
    print(f"── 整片写回 {src}（8 MB，约 2–3 分钟）")
    s.run("write-flash", "--flash-mode", "keep", "--flash-freq", "keep", "--flash-size", "keep", "0x0", str(src))
    print("✅ 已写回（write-flash 按 MD5 校验过）")
    power_cycle_hint()
    return 0


# ── 自测（离线） ───────────────────────────────────────────────────────────

def self_test() -> int:
    ha = expected_layout("partitions.csv", "sdkconfig")
    direct = expected_layout("partitions_direct.csv", "sdkconfig.direct")
    td = Path(tempfile.mkdtemp(prefix="migrate_selftest_"))
    ok = fail = 0

    def check(name, cond):
        nonlocal ok, fail
        print(("✅ " if cond else "❌ ") + name)
        ok, fail = (ok + 1, fail) if cond else (ok, fail + 1)

    def refused(name, fn, needle):
        try:
            fn()
        except MigrateError as e:
            check(f"{name}（{str(e)[:60]}）", needle in str(e))
            return
        check(f"{name} —— 应当拒绝却放行了", False)

    gen = shutil.which("python3") or sys.executable
    idf = os.environ.get("IDF_PATH") or str(Path.home() / "esp-idf-v6.0.2")
    part_tool = Path(idf) / "components" / "partition_table" / "gen_esp32part.py"

    def pt_bin(csv: str) -> bytes:
        out = td / (csv + ".bin")
        subprocess.run([gen, str(part_tool), "--flash-size", "8MB", str(ROOT / csv), str(out)],
                       check=True, capture_output=True)
        return out.read_bytes()

    def fctry_img(sn: str) -> bytes:
        out = td / f"fctry_{abs(hash(sn))}.bin"
        gn.build_image(sn, "AM1", "2026-10-21", ha["fctry"][1], out)
        return out.read_bytes()

    def full_img(pt: bytes, fctry: bytes | None, fctry_at: int, extra: dict | None = None) -> bytes:
        b = bytearray(b"\xff" * FLASH_SIZE)
        b[0x8000:0x8000 + len(pt)] = pt
        b[0x9000:0xF000] = bytes((i * 7) & 0xFF for i in range(0x6000))   # 冒充有内容的主 nvs
        if fctry is not None:
            b[fctry_at:fctry_at + len(fctry)] = fctry
        for off, data in (extra or {}).items():
            b[off:off + len(data)] = data
        return bytes(b)

    # 构建产物：真实 build-direct 存在就用它，否则跳过构建相关用例
    bd = ROOT / "build-direct"
    try:
        items = check_build(bd, direct)
        check("真实 build-direct 通过构建检查", True)
    except MigrateError as e:
        print(f"⏭  build-direct 不可用（{e}），构建相关用例跳过")
        items = []

    pt_ha, pt_direct = pt_bin("partitions.csv"), pt_bin("partitions_direct.csv")
    good = fctry_img("BEH-000001")
    full = full_img(pt_ha, good, ha["fctry"][0])

    check("解析 HA 型分区表：fctry 0x6B0000", parse_partition_table(pt_ha).get("fctry") == ha["fctry"])
    check("解析直连型分区表：fctry 0x7F0000", parse_partition_table(pt_direct).get("fctry") == (0x7F0000, 0x4000))
    check("分类：HA / direct", classify(parse_partition_table(pt_ha), ha, direct) == "ha"
          and classify(parse_partition_table(pt_direct), ha, direct) == "direct")
    check("fctry 解出序列号", fctry_serial(good) == "BEH-000001")

    p = plan_to_direct(full, ha, direct, items)
    check("计划：序列号与新位置", p["serial"] == "BEH-000001" and p["fctry_at"] == 0x7F0000)
    check("计划：擦 0xF000–0x30000 与 0x7B0000–0x7F0000",
          p["erases"] == [(0xF000, 0x30000), (0x7B0000, 0x7F0000)])
    check("计划：保留 0x9000–0xF000", p["keep"] == (0x9000, 0xF000))
    check("计划：fctry 写在 0x7F0000 且只写一次", [o for o, f in p["writes"] if f is None] == [0x7F0000])
    check("计划：matter 落在擦除区里", any(a <= direct["matter"][0] and direct["matter"][0] + direct["matter"][1] <= b
                                         for a, b in p["erases"]))

    refused("fctry 全空 → 拒绝", lambda: plan_to_direct(full_img(pt_ha, None, 0), ha, direct, items), "全 0xFF")
    refused("fctry 解不出序列号 → 拒绝", lambda: fctry_serial(b"\x00" * len(good)), "fctry")
    refused("序列号形状非法 → 拒绝", lambda: fctry_serial(_patch_sn(good, "BEH-000001", "BEH_000001")), "形状非法")
    refused("已经是直连型 → 拒绝", lambda: plan_to_direct(full_img(pt_direct, good, 0x7F0000), ha, direct, items),
            "已经是直连型")
    refused("认不出的分区表 → 拒绝", lambda: plan_to_direct(full_img(b"\xff" * 0x1000, good, ha["fctry"][0]),
                                                           ha, direct, items), "认不出")
    refused("目标位置有别的数据 → 拒绝",
            lambda: plan_to_direct(full_img(pt_ha, good, ha["fctry"][0], {0x7F0000: b"\x00" * 16}), ha, direct, items),
            "已有别的数据")
    p2 = plan_to_direct(full_img(pt_ha, good, ha["fctry"][0], {0x7F0000: good}), ha, direct, items)
    check("目标位置已是同一份身份 → 放行（重跑）", p2["serial"] == "BEH-000001")
    refused("备份长度不对 → 拒绝", lambda: plan_to_direct(full[:-4096], ha, direct, items), "整片备份长度")
    refused("直连型 nvs 起点变了 → 拒绝",
            lambda: plan_to_direct(full, ha, {**direct, "nvs": (0xA000, 0x10000)}, items), "前 24 KB")
    refused("擦除区压到 nvs → 拒绝",
            lambda: plan_to_direct(full, ha, {**direct, "hubdb": (0x9000, 0x30000)}, items),
            "压到了要保留的 nvs")

    if items:
        # 构建产物的拒绝用例：在临时目录里拼一个改坏的 build 目录
        def fake_build(mutate) -> Path:
            fb = td / f"build_{mutate.__name__}"
            if fb.exists():
                shutil.rmtree(fb)
            fb.mkdir()
            lines = []
            for off, src in items:
                dst = fb / src.name
                shutil.copy(src, dst)
                lines.append(f"{off:#x} {src.name}")
            (fb / "flash_args").write_text("--flash-mode dio --flash-freq 80m --flash-size keep\n" + "\n".join(lines) + "\n")
            mutate(fb)
            return fb

        def m_unsigned(fb):
            app = fb / "bitengine-direct.bin"
            app.rename(fb / "bitengine-direct-unsigned.bin")
            (fb / "flash_args").write_text((fb / "flash_args").read_text().replace("bitengine-direct.bin",
                                                                                  "bitengine-direct-unsigned.bin"))

        def m_ha_name(fb):
            app = fb / "bitengine-direct.bin"
            b = bytearray(app.read_bytes())
            b[80:112] = b"bitengine".ljust(32, b"\0")
            app.write_bytes(bytes(b))

        def m_nosig(fb):
            app = fb / "bitengine-direct.bin"
            app.write_bytes(app.read_bytes()[:-SIGBLOCK_SIZE])

        def m_ha_table(fb):
            (fb / "partition-table.bin").write_bytes(pt_ha)

        refused("flash_args 指向未签名镜像 → 拒绝", lambda: check_build(fake_build(m_unsigned), direct), "未签名")
        refused("app 工程名是 bitengine → 拒绝", lambda: check_build(fake_build(m_ha_name), direct), "工程名")
        refused("app 没签名块 → 拒绝", lambda: check_build(fake_build(m_nosig), direct), "签名块")
        refused("分区表是 HA 型的 → 拒绝", lambda: check_build(fake_build(m_ha_table), direct), "不是同一张表")

    # 分段读的退路：模拟"0xf9000 那块按 ≥ 4 KB 读必失败、1 KB 能读"，以及"某 1 KB 怎么都读不出来"
    flash = bytes((i * 31 + (i >> 12)) & 0xFF for i in range(0x100000))
    bad = 0xf9000
    def fake(o, n, worst=False):
        if o <= bad < o + n and n > READ_PIECE:
            raise MigrateError("No more data to read from the serial port")
        if worst and o <= bad < o + n:
            raise MigrateError("No more data to read from the serial port")
        return flash[o:o + n]
    calls = []
    def counted(o, n):
        calls.append((o, n))
        return fake(o, n)
    try:
        got = chunked_read(counted, 0, len(flash))
    except MigrateError:
        got = None
    check("分段读：坏块按 4 KB 读失败时拆 1 KB，数据逐字节一致", got == flash)
    check("分段读：只有出事那一段才逐块重读（调用次数有界）", len(calls) < 0x100000 // READ_CHUNK + 64 + 8)
    refused("分段读：1 KB 也读不出来 → 放弃，不交一份残缺备份",
            lambda: chunked_read(lambda o, n: fake(o, n, worst=True), 0, len(flash)), "No more data")

    shutil.rmtree(td, ignore_errors=True)
    print(f"\nResult: {ok} passed, {fail} failed")
    return 0 if fail == 0 else 1


def _patch_sn(img: bytes, old: str, new: str) -> bytes:
    """把镜像里的序列号字符串原地换成等长的另一个（NVS 字符串条目带 CRC，换完 nvs_tool 仍会读出来，
    只是那条 CRC 不对 —— minimal 输出照样给数据，正好用来测"形状非法"）。"""
    assert len(old) == len(new)
    i = img.find(old.encode())
    assert i >= 0
    return img[:i] + new.encode() + img[i + len(old):]


def main() -> int:
    ap = argparse.ArgumentParser(description="HA 型 → 直连型换固件 / 按整片备份恢复（docs/15 设计 8.12）")
    ap.add_argument("--self-test", action="store_true", help="离线自测，不碰板子")
    sub = ap.add_subparsers(dest="cmd")
    a = sub.add_parser("to-direct", help="整片备份后换刷直连型、搬 fctry")
    a.add_argument("-p", "--port", required=True)
    a.add_argument("--build-dir", default=str(ROOT / "build-direct"))
    a.add_argument("--backup-dir", help="默认 ~/bitengine-backups/migrate-<MAC>-<时间>/")
    a.add_argument("--dry-run", action="store_true", help="只备份和判定，不擦不写")
    r = sub.add_parser("restore", help="把整片 8 MB 备份原样写回")
    r.add_argument("-p", "--port", required=True)
    r.add_argument("--from", dest="src", required=True)
    args = ap.parse_args()
    try:
        if args.self_test:
            return self_test()
        if args.cmd == "to-direct":
            return cmd_to_direct(args)
        if args.cmd == "restore":
            return cmd_restore(args)
        ap.print_help()
        return 2
    except MigrateError as e:
        print(f"❌ {e}", file=sys.stderr)
        if getattr(args, "cmd", None) in ("to-direct", "restore"):
            print("   esptool 连不上了的话：用 OpenOCD 经 USB-JTAG 把整片备份写回（命令见本脚本文件头）", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
