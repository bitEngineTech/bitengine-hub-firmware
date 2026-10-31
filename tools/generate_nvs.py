#!/usr/bin/env python3
"""产线出厂身份写入 —— 生成 fctry 分区镜像，可选直接烧进板子。

    # 只生成镜像（离线备料，不连板子）
    python3 tools/generate_nvs.py --serial BEH-000001

    # 连着板子：分配下一个序列号 → 生成 → 烧 → 读回校验（产线工位 2 的一步）
    python3 tools/generate_nvs.py --port /dev/ttyACM0 --flash

    # 指定序列号（返修重写、或换号段）
    python3 tools/generate_nvs.py --port /dev/ttyACM0 --flash --serial BEH-000042

    # 新批次 / 台账丢失后从已知编号接上
    python3 tools/generate_nvs.py --port /dev/ttyACM0 --flash --start 1200

写进去的只有三个字符串：serial_number / hw_revision / mfg_date。

⚠️ **没有 device_secret。** docs/10 §6.2 原设计里有一个 32 字节密钥给云端 OTA 做
HMAC，2026-09-09 决策不做 —— 云端 OTA 靠固件自带签名验证，谁下载都一样，密钥没有
消费者。落一把没人读的密钥进 Flash 是纯负债（开源、日志、配置导出每处都要脱敏）。
因此 docs/10 §7 里「非法设备拒绝」「设备注册」两条验收在本期**不适用**，不是没做到。

⚠️ **不存 MAC。** docs/10:175 把 base_mac 列进了 fctry，但 docs/05:90 又说 MAC 来自
eFuse、"留 fctry 是为了留住 MAC"这句是空的。后者对：MAC 在 eFuse 里本来就是权威的，
再存一份只会多一个可能对不上的副本。

⚠️ **docs/10 §5.2 那段示例代码有两处照抄就会出事，本脚本都绕开了：**

1. 镜像大小写的是 `0x2000`（8KB），而 partitions.csv 里 fctry 是 `0x4000`（16KB）。
   照抄会生成一个只有半个分区大的镜像。本脚本的大小与偏移都**从 partitions.csv
   现算**，不写死，分区表一改就跟着变。
2. 命名空间行写的是 `namespace,namespace,,factory`，但 nvs_partition_gen 的格式是
   **键位置放命名空间名**（`factory,namespace,,`）。照抄会建出一个叫 "namespace" 的
   命名空间，固件打开 "factory" 就一个键都读不到 —— 最像"产线忘了写"的一种故障。
3. 键名 `hardware_revision`（17 字符）和 `manufacture_date`（16 字符）**超过 NVS 的
   15 字符上限**，`nvs_partition_gen.py` 直接报 "Length of key should be <= 15
   characters" 拒绝生成 —— 那份 CSV 从来就写不进去。这里用 `hw_revision`(11) 和
   `mfg_date`(8)，`serial_number`(13) 正好卡在线内可以保留。
   ⚠️ 键名与 device_identity.c 里读的必须逐字一致，改一边就会变成"分区在、能打开、
   一个键都读不到"，很容易被误判成产线没写。
"""

from __future__ import annotations

import argparse
import csv
import datetime
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# ── 序列号前缀 ────────────────────────────────────────────────────────────
#
# ⚠️ **首批量产前必须确认这个值。** 它会印在机壳标签和二维码上，出货之后改不了
#    （已出货的那批就永远是旧前缀）。
#
# 现值 BEH = bitEngine Hub（2026-09-11 用户定）。加上 6 位递增编号就是完整序列号，
# 如 BEH-000001 —— 10 个字符、一个连字符，标签好印、电话里好念。
# 曾经取过 BEH-C5（带芯片型号），两段前缀太啰嗦，已弃。
# docs/10 里写的是 zGW-C5，那是产品定名之前的遗留串，全仓只出现在设计文档里，
# 代码零引用。沿用它会让标签上的品牌和其他所有地方对不上。
#
# 固件侧**不校验前缀**（只校验形状：字母数字分段、连字符分隔、≤31 字符），
# 所以改这里不需要改固件，也不会让已出货设备把自己判成非法。
SERIAL_PREFIX = "BEH"
SERIAL_DIGITS = 6          # BEH-000001 … BEH-999999

FCTRY_LABEL = "fctry"
NVS_NAMESPACE = "factory"          # 与 device_identity.c 的 FCTRY_NAMESPACE 逐字一致
HW_REVISION_DEFAULT = "AM1"        # 当前生产 PCB。AM2 改版后改这里

# 产线台账。每写一台追加一行，用来查重和事后追溯。
# 可用 --ledger 指到别处：test_host/run.sh 跑失败路径时用临时台账，绝不碰真台账。
LOG_CSV = ROOT / "tools" / ".factory_identity_log.csv"

SERIAL_RE = re.compile(r"^[A-Za-z0-9]+(-[A-Za-z0-9]+)+$")


def die(msg: str) -> "NoReturn":  # type: ignore[valid-type]
    print(f"❌ {msg}", file=sys.stderr)
    sys.exit(1)


# ── 分区表 ────────────────────────────────────────────────────────────────

def partition_geometry(label: str, csv_name: str = "partitions.csv",
                       sdkconfig_name: str = "sdkconfig") -> tuple[int, int]:
    """从分区表现算 (offset, size)。默认 HA 型的 partitions.csv；直连型传 partitions_direct.csv /
    sdkconfig.direct（docs/15 设计 2.4：直连型的 fctry 在 0x7F0000）。

    不写死偏移：分区表一改，写死的偏移就会把身份写进隔壁分区（fctry 前面正好是
    coredump），而且不会有任何报错。偏移由 ESP-IDF 按顺序累加得出，起点是
    CONFIG_PARTITION_TABLE_OFFSET 之后的一页。
    """
    csv_path = ROOT / csv_name
    if not csv_path.exists():
        die(f"找不到 {csv_path}")

    # 分区表本身占一页(0x1000)，其后才是第一个分区
    tbl_off = 0x8000
    sdk = ROOT / sdkconfig_name
    if sdk.exists():
        m = re.search(r"^CONFIG_PARTITION_TABLE_OFFSET=(0x[0-9a-fA-F]+|\d+)$",
                      sdk.read_text(), re.M)
        if m:
            tbl_off = int(m.group(1), 0)
    cursor = tbl_off + 0x1000

    def align(v: int, a: int) -> int:
        return (v + a - 1) // a * a

    for raw in csv_path.read_text().splitlines():
        line = raw.split("#")[0].strip()
        if not line:
            continue
        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 5:
            continue
        name, ptype, _subtype, offset, size = parts[:5]
        size_i = int(size, 0)
        if offset:
            cursor = int(offset, 0)
        else:
            # app 分区要 64KB 对齐，data 分区 4KB
            cursor = align(cursor, 0x10000 if ptype == "app" else 0x1000)
        if name == label:
            return cursor, size_i
        cursor += size_i
    die(f"{csv_name} 里没有名为 {label} 的分区")


# ── 序列号 ────────────────────────────────────────────────────────────────

def next_serial() -> str:
    """产线分配下一个序列号：前缀 + 6 位递增编号，如 BEH-000001。

    **序列号是生产时分配的，与 MAC 无关。** 台账 LOG_CSV 就是计数器的状态：
    取里面所有本前缀的号，最大值加一。

    ⚠️ 台账丢了，计数器就从 1 重来，而重号是事后无法修复的（两台设备标签一样）。
       所以：① 台账必须备份（它同时也是保修追溯的正本）；② 万一丢了，用
       `--start N` 从已知的下一个号接上，别让它从头数。
    ⚠️ 同时开两台产线机器会各数各的。要并行就给每台分一段号区（`--start`），
       别指望工具自己协调 —— 它看不见另一台的台账。
    """
    used = 0
    if LOG_CSV.exists():
        with LOG_CSV.open(newline="") as f:
            for row in csv.DictReader(f):
                m = re.fullmatch(rf"{re.escape(SERIAL_PREFIX)}-(\d+)",
                                 (row.get("serial_number") or "").strip())
                if m:
                    used = max(used, int(m.group(1)))
    return f"{SERIAL_PREFIX}-{used + 1:0{SERIAL_DIGITS}d}"


def check_serial(sn: str) -> None:
    """与固件 device_identity.c 的 serial_shape_ok() 同一套规则。

    两处实现必须保持一致：这边放行、固件判非法，产线会在工位 3 才发现，
    而那时身份已经写进 Flash 了。components/device_identity/test_host/ 里有
    一组共用的样例把两边钉在一起。
    """
    if not sn:
        die("序列号为空")
    if len(sn) >= 32:
        die(f"序列号超过 31 字符（固件会判为非法）: {sn!r}")
    if not SERIAL_RE.match(sn):
        die(f"序列号形状非法（要求：字母数字分段、单个连字符分隔、至少两段）: {sn!r}")


# ── esptool ───────────────────────────────────────────────────────────────

def esptool(*args: str) -> str:
    cmd = ["esptool", "--chip", "esp32c5", *args]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    except FileNotFoundError:
        die("找不到 esptool —— 先 source ~/esp-idf-v6.0.2/export.sh")
    if r.returncode != 0:
        die(f"esptool 失败：{' '.join(cmd)}\n{r.stdout}\n{r.stderr}")
    return r.stdout


# ── 复位次数 ─────────────────────────────────────────────────────────────
# read-mac / write-flash / read-flash 三步各自用 esptool 默认的
# "--before default-reset --after hard-reset"，等于让板子进 3 次下载模式、复位 3 次。
# 2026-09-13 真板首跑就栽在第 2→3 步之间：write-flash 结束的硬复位让 USB
# Serial/JTAG 口重新初始化，紧接着的 read-flash 打开串口就 "Write timeout"，
# 读回校验没做成、台账没记；而且这块 AM1 板每一次复位都是 IO28 的抽奖
# （PHY 有挂起中断就停在下载模式，见 docs/guide/产测指南.md §7.1）——当天就中了一次，
# 只能断电重上电。所以三步串成一次会话：第一步进下载模式后 `--after no-reset`
# 留在 stub 里，后两步 `--before no-reset` 直接接上，板子只进一次下载模式。
#
# 收尾**也不复位**（`--after no-reset`），板子确定性地停在下载模式，由操作员断电
# 重上电。原因：esptool 的 hard-reset 只复位 C5，不复位 DM9051 —— 应用刚才跑过、
# PHY 中断已使能，复位瞬间 INT 大概率还挂着，IO28 就被拉低。2026-09-13 两次
# hard-reset 两次都进坑（2/2）。既然横竖都要断电，不如让"写完断电"成为固定动作，
# 而不是"有时能起来、有时看起来像变砖"。AM2 解开 IO28（A3）后可以改回 hard-reset，
# 那条已记在 docs/hardware/AM2_改版工单.md 附一（改法与验收的正本）。
def normalize_mac(raw: str) -> str:
    """esptool 在 ESP32-C5 上打印的是 **EUI-64**（`38:44:be:ff:fe:12:34:56`，
    中间插了 ff:fe），不是熟悉的 6 字节。此前的正则 `{17}` 会把前 17 个字符
    `38:44:be:ff:fe:12` 当成 MAC 记进台账 —— 一个不存在的地址，售后拿设备
    反查永远查不到（2026-09-13 真板首写发现）。这里折回 EUI-48（去掉中间的
    ff:fe），并统一小写，与固件 /api/v1/status、产线自检 `mac=` 报的是同一个值。"""
    parts = raw.lower().split(":")
    if len(parts) == 8 and parts[3:5] == ["ff", "fe"]:
        parts = parts[:3] + parts[5:]
    if len(parts) != 6 or not all(re.fullmatch(r"[0-9a-f]{2}", x) for x in parts):
        die(f"MAC 格式认不出：{raw!r}（期望 6 字节，或中间为 ff:fe 的 EUI-64）")
    return ":".join(parts)


def read_mac(port: str) -> str:
    out = esptool("-p", port, "--after", "no-reset", "read-mac")
    for line in out.splitlines():
        m = re.search(r"MAC:\s*((?:[0-9a-fA-F]{2}:){5,7}[0-9a-fA-F]{2})\b", line)
        if m:
            return normalize_mac(m.group(1))
    die(f"从 esptool 输出里认不出 MAC：\n{out}")


# ── NVS 镜像 ──────────────────────────────────────────────────────────────

def nvs_gen_tool() -> str:
    idf = os.environ.get("IDF_PATH")
    if idf:
        p = Path(idf) / "components" / "nvs_flash" / "nvs_partition_generator" / "nvs_partition_gen.py"
        if p.exists():
            return str(p)
    for guess in (Path.home() / "esp-idf-v6.0.2", Path.home() / "esp-idf"):
        p = guess / "components" / "nvs_flash" / "nvs_partition_generator" / "nvs_partition_gen.py"
        if p.exists():
            return str(p)
    die("找不到 nvs_partition_gen.py —— 先 source ~/esp-idf-v6.0.2/export.sh")


def build_image(sn: str, hw_rev: str, date: str, size: int, out_bin: Path) -> None:
    rows = [
        ("key", "type", "encoding", "value"),
        # ⚠️ 命名空间行的**键**就是命名空间名，value 留空。
        #    docs/10:676 写的是 `namespace,namespace,,factory`，照抄会建出一个
        #    名叫 "namespace" 的命名空间，而固件打开的是 "factory" —— 表现为
        #    "分区在、能打开、一个键都读不到"，最像"产线没写"的一种故障。
        #    2026-09-11 本脚本第一版就是这么错的，被 test_host 的镜像内容检查抓到。
        (NVS_NAMESPACE, "namespace", "", ""),
        ("serial_number", "data", "string", sn),
        ("hw_revision", "data", "string", hw_rev),
        ("mfg_date", "data", "string", date),
    ]
    with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False,
                                     newline="") as f:
        csv.writer(f).writerows(rows)
        csv_path = f.name
    try:
        r = subprocess.run(
            [sys.executable, nvs_gen_tool(), "generate", csv_path,
             str(out_bin), hex(size)],
            capture_output=True, text=True, timeout=120)
        if r.returncode != 0:
            die(f"nvs_partition_gen 失败：\n{r.stdout}\n{r.stderr}")
    finally:
        os.unlink(csv_path)

    actual = out_bin.stat().st_size
    if actual != size:
        die(f"生成的镜像 {actual} 字节，分区是 {size} 字节 —— 对不上")


# ── 台账 ──────────────────────────────────────────────────────────────────

def log_lookup(mac: str, sn: str) -> dict | None:
    """查这台 MAC 或这个 SN 是不是已经写过。

    撞号是事后无法修复的（两台设备标签一样），所以宁可在这里拦住。
    同一块 MAC 重写同一个 SN 是允许的（返修重烧），拦的是"同一个 SN 落到两块
    不同的板子"。
    """
    if not LOG_CSV.exists():
        return None
    with LOG_CSV.open(newline="") as f:
        for row in csv.DictReader(f):
            if row.get("serial_number") == sn and row.get("mac") != mac:
                die(f"序列号 {sn} 已经写给过另一块板 (MAC {row.get('mac')})，拒绝重复使用。"
                    f"这通常意味着台账被回退过或两台产线机器在并行 —— 用 --start 分开号段")
            # mac 为空（纯离线生成）时没法按板查重，只能靠上面那条按号查重
            if mac and row.get("mac") == mac:
                return row
    return None


def log_append(mac: str, sn: str, hw_rev: str, date: str) -> None:
    new = not LOG_CSV.exists()
    with LOG_CSV.open("a", newline="") as f:
        w = csv.writer(f)
        if new:
            w.writerow(["written_at", "mac", "serial_number",
                        "hardware_revision", "manufacture_date"])
        w.writerow([datetime.datetime.now().isoformat(timespec="seconds"),
                    mac, sn, hw_rev, date])


# ── main ──────────────────────────────────────────────────────────────────

def main() -> int:
    global LOG_CSV   # 必须在函数里第一次引用 LOG_CSV 之前声明（--ledger 的 help 就引用它）
    ap = argparse.ArgumentParser(
        description="生成（并可选烧录）出厂身份到 fctry 分区",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="串口，如 /dev/ttyACM0。烧录与读 MAC 都用它")
    ap.add_argument("--mac", help="离线生成时手工给 MAC（只进台账，不参与生成序列号）")
    ap.add_argument("--serial", help="显式指定序列号。默认由台账分配下一个")
    ap.add_argument("--start", type=int, metavar="N",
                    help="把下一个编号定为 N（新批次；或台账丢失后从已知编号接上）")
    ap.add_argument("--hw-rev", default=HW_REVISION_DEFAULT,
                    help=f"硬件版本（默认 {HW_REVISION_DEFAULT}）")
    ap.add_argument("--date", default=datetime.date.today().isoformat(),
                    help="生产日期 YYYY-MM-DD（默认今天）")
    ap.add_argument("--out", default=str(ROOT / "dist" / "fctry"),
                    help="镜像输出目录")
    ap.add_argument("--flash", action="store_true",
                    help="烧进板子并读回校验（需要 --port）")
    ap.add_argument("--force", action="store_true",
                    help="这块板子台账里已有记录时仍然重写")
    ap.add_argument("--partitions", default="partitions.csv",
                    help="分区表（默认 HA 型的 partitions.csv；直连型给 partitions_direct.csv，fctry 在 0x7F0000）")
    ap.add_argument("--ledger", metavar="CSV",
                    help=f"台账文件（默认 {LOG_CSV.relative_to(ROOT)}；主机侧测试用临时文件）")
    args = ap.parse_args()
    if args.ledger:
        LOG_CSV = Path(args.ledger)

    if args.flash and not args.port:
        die("--flash 需要 --port")
    if args.serial and args.start is not None:
        die("--serial 与 --start 互斥：一个是指定这一台的号，一个是定下一个号")

    # 按文件名判直连型：写成 ./partitions_direct.csv 也要配 sdkconfig.direct（审计 2026-10-21）
    offset, size = partition_geometry(FCTRY_LABEL, args.partitions,
                                      "sdkconfig.direct" if Path(args.partitions).name == "partitions_direct.csv"
                                      else "sdkconfig")

    # 序列号：产线分配，**与 MAC 无关**。
    if args.serial:
        sn = args.serial
    elif args.start is not None:
        sn = f"{SERIAL_PREFIX}-{args.start:0{SERIAL_DIGITS}d}"
    else:
        sn = next_serial()
    check_serial(sn)

    # MAC 只进台账，用来事后由设备反查序列号（售后拿到一台板子时，标签可能已磨损，
    # 但 MAC 总能从设备读出来）。它不参与生成序列号。
    mac = args.mac or (read_mac(args.port) if args.port else "")
    # read_mac 之后芯片留在 stub 里，直到操作员断电（见"复位次数"一节）。
    # 记下来，退出时（成败都）提示断电重上电。
    global _stub_live, _port_for_reset
    _stub_live = bool(args.port)
    _port_for_reset = args.port or ""

    prev = log_lookup(mac, sn)
    if prev and not args.force:
        if prev.get("serial_number") == sn:
            print(f"ℹ️  这块板（MAC {mac}）台账里已经写过 {sn}，"
                  f"继续（同一块板重写同一个号是安全的）")
        else:
            die(f"这块板（MAC {mac}）台账里已有序列号 {prev.get('serial_number')}。"
                f"要重新分配请加 --force；要重写原号请 --serial {prev.get('serial_number')}")

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    out_bin = out_dir / f"fctry_{sn}.bin"

    build_image(sn, args.hw_rev, args.date, size, out_bin)
    print(f"✅ 镜像 {out_bin}")
    print(f"   SN={sn}  硬件={args.hw_rev}  产期={args.date}")
    print(f"   分区 {FCTRY_LABEL} @ {hex(offset)}  {size} 字节（现算自 {args.partitions}）")

    if not args.flash:
        print(f"\n   烧录：esptool --chip esp32c5 -p <口> write-flash "
              f"{hex(offset)} {out_bin}")
        print("   ⚠️ 没有 --flash 就**不记台账**，下一次运行还会分到同一个号。"
              "离线备料请自己用 --serial 逐台指定。")
        return 0

    esptool("-p", args.port, "--before", "no-reset", "--after", "no-reset",
            "write-flash", hex(offset), str(out_bin))

    # 读回逐字节比对。"esptool 没报错"不等于"写进去了"——
    # 偏移写错时它同样一声不吭地成功，只是写去了隔壁分区。
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        back = f.name
    try:
        esptool("-p", args.port, "--before", "no-reset", "--after", "no-reset",
                "read-flash", hex(offset), hex(size), back)
        if Path(back).read_bytes() != out_bin.read_bytes():
            die("读回的内容与写入的不一致 —— 别放这块板过站")
    finally:
        os.unlink(back)

    log_append(mac, sn, args.hw_rev, args.date)
    print(f"✅ 已写入并读回校验一致，台账记在 {LOG_CSV.name}")
    print(f"\n   下一步：断电重上电后，GET /api/v1/status 的 serial_number 应为 {sn}，"
          f"产线自检的 identity 项应为 pass")
    return 0


_stub_live = False
_port_for_reset: str = ""


def _power_cycle_hint() -> None:
    """read_mac 之后板子就停在下载模式了，成功与失败都一样（见"复位次数"一节）。
    退出前把这件事说出来，否则失败那条路上操作员只看到一块不上网、串口无输出
    的板子 —— 和 §7.1 的 IO28 故障一个样子。"""
    if _stub_live and _port_for_reset:
        print("\n   ⚠️ 板子现在停在下载模式（不上网、串口无输出，属正常）。"
              "把 PoE 网线和 USB 都拔掉再上电 —— 按复位没用，DM9051 的挂起中断会让"
              "IO28 再把它拉回下载模式。见 docs/guide/产测指南.md §五 / §7.1",
              file=sys.stderr)


if __name__ == "__main__":
    try:
        sys.exit(main())
    finally:
        _power_cycle_hint()
