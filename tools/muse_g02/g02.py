#!/usr/bin/env python3
"""G0-2 协议闸门：在开发电脑上用上游 Linux SDK 走通 Muse 配对 → 注册 → 调用 → 推送（docs/15 架构第六节 G0-2）。

    python3 tools/muse_g02/g02.py show-register --identity a    # 离线：打印要注册的参数（不连网、不读 token 内容）
    python3 tools/muse_g02/g02.py pair --identity a              # 开 BLE 配对窗口（10 分钟），手机 App 里添加设备
    python3 tools/muse_g02/g02.py run  --identity a              # 建 Noise 会话、注册、等 Muse 调命令（Ctrl-C 退出）
    python3 tools/muse_g02/g02.py info                           # 本地身份与配对状态
    python3 tools/muse_g02/g02.py send "测试推送"                 # 经本地套接字给 Muse 发一条消息（run 必须在跑）

规矩（架构 G0-2 一行，2026-10-15 用户定）：
- 从源码跑上游 SDK（默认 ~/src/muse-gadget-sdk/linux，MUSE_SDK_DIR 可改），**不改上游克隆**：本脚本在运行时替换。
- **不装 systemd 服务、不用 sudo**：状态在 ~/.muse-g02/state，本地套接字 ~/.muse-g02/musegadget.sock。
- **内置的 system.run / file.* 一律不注册**：命令表整个换成设计文档附录 A 的九条（运行时从
  docs/15_muse_direct_design_v1.md 解析，不另存副本），每条都是桩：回假数据，每次调用记进 ~/.muse-g02/invokes.jsonl。
- SDK token 从 ~/.muse-sdk-token 读进本进程的环境变量，**从不打印**。websockets 库的 DEBUG 日志会打握手头
  （含 Authorization: Bearer），所以它的日志级别单独压在 INFO。
- 注册身份按 MD-4 两种都试：a = homehub / esp32 / bitengine-hub-c5（首选），b = link / esp-link（上游 ESP32 固件的取值）。

依赖：系统的 python3-dbus / python3-gi（只有发行版包），加上 SDK 锁定的 websockets，装在能看到系统包的虚拟环境里：
    uv venv --python /usr/bin/python3 --system-site-packages ~/.muse-g02/venv
    uv pip install --python ~/.muse-g02/venv/bin/python --require-hashes -r ~/src/muse-gadget-sdk/linux/src/musegadget/data/requirements.lock
本脚本发现当前解释器缺 websockets、而那个虚拟环境在时，自动换到它重跑。
"""
from __future__ import annotations

import argparse
import json
import logging
import os
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DESIGN = ROOT / "docs" / "15_muse_direct_design_v1.md"
SDK = Path(os.environ.get("MUSE_SDK_DIR", "~/src/muse-gadget-sdk/linux")).expanduser()
HOME = Path("~/.muse-g02").expanduser()
TOKEN_FILE = Path("~/.muse-sdk-token").expanduser()

IDENTITIES = {
    "a": {"platform": "esp32", "device_family": "homehub", "model_id": "bitengine-hub-c5"},
    "b": {"platform": "esp32", "device_family": "link", "model_id": "esp-link"},
}


def appendix_a_commands() -> dict:
    """设计文档附录 A 的 commands_v2（唯一正本）。"""
    text = DESIGN.read_text(encoding="utf-8")
    start = text.index("## 附录 A")
    m = re.search(r"```json\n(.*?)\n```", text[start:], re.S)
    if not m:
        sys.exit(f"❌ 在 {DESIGN} 附录 A 里找不到 json 代码块")
    cmds = json.loads(m.group(1))
    bad = [c for c in cmds if c.startswith(("system.", "file.")) or c == "device.ota"]
    if bad:
        sys.exit(f"❌ 附录 A 里出现了不许注册的命令：{bad}")
    return cmds


# ---- 桩：假数据，结构照附录 A 的列表项格式 ----
FAKE_DEVICES = [
    {"id": "zb-00158d0001a2b3c4", "protocol": "zigbee", "name": "Front door", "room": "Hall",
     "kind": "contact_sensor", "capabilities": ["contact", "battery"], "online": True, "last_seen_s": 42,
     "state": {"contact": "closed", "battery_pct": 87}, "link": {"lqi": 176, "rssi": -61}, "notify": True, "sensitive": False},
    {"id": "zb-00158d0002b3c4d5", "protocol": "zigbee", "name": "Living room lamp", "room": "Living room",
     "kind": "dimmable_light", "capabilities": ["onoff", "level"], "online": True, "last_seen_s": 5,
     "state": {"onoff": False, "level_pct": 60}, "link": {"lqi": 210, "rssi": -52}, "notify": False, "sensitive": False},
    {"id": "th-1e2d3c4b5a697887", "protocol": "thread", "name": "", "controllable": False, "online": True,
     "last_seen_s": 12, "role": "sleepy_end_device", "thread": {"parent": "0x6c00", "rssi_avg": -70, "link_margin": 25}},
]
EVENTS = [
    {"t_s_ago": 120, "type": "contact", "id": "zb-00158d0001a2b3c4", "name": "Front door", "value": "closed"},
    {"t_s_ago": 180, "type": "contact", "id": "zb-00158d0001a2b3c4", "name": "Front door", "value": "open"},
]


def stub_run(self, command: str, params: dict, timeout_ms: int | None = None) -> dict:
    t0 = time.time()
    p = params or {}
    if command == "device.health":
        res = {"ok": True, "payload": {"firmware": "g02-stub", "uptime_s": int(t0 - START), "uplink": "ethernet",
                                       "zigbee_radio": "ok", "thread_role": "leader", "devices": len(FAKE_DEVICES),
                                       "internal_free": 82100, "note": "G0-2 stub, not a real hub"}}
    elif command == "hub.devices.list":
        devs = FAKE_DEVICES
        if p.get("protocol") not in (None, "", "all"):
            devs = [d for d in devs if d["protocol"] == p["protocol"]]
        if p.get("room"):
            devs = [d for d in devs if d.get("room", "").lower() == str(p["room"]).lower()]
        res = {"ok": True, "payload": {"devices": devs}}
    elif command == "hub.device.get":
        d = next((d for d in FAKE_DEVICES if d["id"] == p.get("id")), None)
        res = {"ok": True, "payload": d} if d else {"ok": False, "error": "unknown device id"}
    elif command == "hub.thread.network":
        res = {"ok": True, "payload": {"network_name": "OpenThread-2831", "channel": 11, "pan_id": "0x2831",
                                       "role": "leader", "routers": 1, "children": 1, "omr_prefix": "fd16:2b4b:919c:1::/64"}}
    elif command == "hub.events.recent":
        res = {"ok": True, "payload": {"events": EVENTS[: int(p.get("limit") or 20)]}}
    elif command == "hub.device.control":
        d = next((d for d in FAKE_DEVICES if d["id"] == p.get("id")), None)
        if not d or d.get("controllable") is False:
            res = {"ok": False, "error": "device not controllable"}
        else:
            res = {"ok": True, "payload": {"result": "confirmed", "action": p.get("action"), "value": p.get("value")}}
    elif command == "hub.device.update":
        res = {"ok": True, "payload": {"id": p.get("id"), "updated": sorted(k for k in p if k != "id")}}
    elif command == "hub.zigbee.permit_join":
        res = {"ok": True, "payload": {"open_s": int(p.get("seconds") or 120)}}
    elif command == "hub.matter.commission":
        res = {"ok": False, "error": "G0-2 stub: commissioning not available"}
    else:
        res = {"ok": False, "error": f"unsupported command: {command}"}   # 包括服务端发来我们没注册的命令
    rec = {"ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "identity": IDENTITY, "command": command,
           "params": p, "timeout_ms": timeout_ms, "ok": res.get("ok"), "registered": command in COMMANDS}
    with open(HOME / "invokes.jsonl", "a", encoding="utf-8") as f:
        f.write(json.dumps(rec, ensure_ascii=False) + "\n")
    logging.getLogger("g02").info("invoke %s %s → %s", command, json.dumps(p, ensure_ascii=False)[:200],
                                  "ok" if res.get("ok") else res.get("error"))
    return res


START = time.time()
IDENTITY = "a"
COMMANDS: dict = {}


VENV = HOME / "venv"


def ensure_venv() -> None:
    try:
        import websockets  # noqa: F401
    except ImportError:
        py = VENV / "bin" / "python"
        if py.exists() and Path(sys.prefix) != VENV:
            os.execv(str(py), [str(py), __file__, *sys.argv[1:]])
        sys.exit(f"❌ 缺 websockets：先按文件头建 {VENV}")


def main() -> int:
    global IDENTITY, COMMANDS
    ap = argparse.ArgumentParser(description="G0-2：上游 Linux SDK + 附录 A 桩命令")
    ap.add_argument("cmd", choices=["show-register", "pair", "run", "info", "unpair", "send"])
    ap.add_argument("rest", nargs="*", help="send 的消息文本")
    ap.add_argument("--identity", choices=sorted(IDENTITIES), default="a")
    ap.add_argument("--force", action="store_true", help="pair：已配对也重新配")
    ap.add_argument("--timeout", type=int, help="pair：配对窗口秒数（默认 SDK 的 600）")
    a = ap.parse_args()
    os.umask(0o077)                                       # 状态、日志、invoke 记录都只给本人
    IDENTITY = a.identity
    if a.cmd != "show-register":
        ensure_venv()

    if not (SDK / "src" / "musegadget").is_dir():
        sys.exit(f"❌ 找不到上游 SDK：{SDK}（git clone https://github.com/facebookincubator/muse-gadget-sdk ~/src/muse-gadget-sdk）")
    HOME.mkdir(mode=0o700, exist_ok=True)
    (HOME / "state").mkdir(mode=0o700, exist_ok=True)
    (HOME / "logs").mkdir(mode=0o700, exist_ok=True)
    os.environ["MUSEGADGET_STATE_DIR"] = str(HOME / "state")
    os.environ["MUSEGADGET_SOCKET"] = str(HOME / "musegadget.sock")
    if a.cmd in ("pair", "run"):
        if not TOKEN_FILE.exists():
            sys.exit(f"❌ 没有 {TOKEN_FILE}")
        tok = TOKEN_FILE.read_text().strip()
        if not re.fullmatch(r"mgst_[A-Za-z0-9_-]{43}", tok):
            sys.exit("❌ SDK token 形状不对（应为 mgst_ + 43 个 base64url 字符）")
        os.environ["MUSEGADGET_SDK_TOKEN"] = tok          # 只进本进程环境，从不打印

    sys.path.insert(0, str(SDK / "src"))
    import musegadget.cli as cli
    import musegadget.executor as ex
    import musegadget.link_client as lc

    COMMANDS = appendix_a_commands()
    ex.COMMAND_SPECS.clear()                              # 原地换：service.py 导入时就绑定了这个字典对象
    ex.COMMAND_SPECS.update(COMMANDS)
    ex.Executor.run = stub_run
    ident = IDENTITIES[IDENTITY]
    orig = lc.DeviceDescription.register_params

    def register_params(self):
        p = orig(self)
        p.update(ident)
        return p
    lc.DeviceDescription.register_params = register_params

    if a.cmd == "show-register":
        d = lc.DeviceDescription(node_id="homelink-xxxxxx", display_name="(display name)", version="g02", commands=COMMANDS)
        p = d.register_params()
        body = json.dumps(p, separators=(",", ":"))
        print(json.dumps({k: v for k, v in p.items() if k != "commands_v2"}, ensure_ascii=False, indent=1))
        print(f"commands_v2: {len(COMMANDS)} 条 {sorted(COMMANDS)}")
        print(f"注册参数紧凑 JSON {len(body)} 字节（设计要求整条 ≤ 7 KB）")
        assert not any(c.startswith(("system.", "file.")) for c in p["commands_v2"])
        return 0

    stamp = time.strftime("%Y%m%d-%H%M%S")
    logf = HOME / "logs" / f"{stamp}-{a.cmd}-{IDENTITY}.log"
    handler = logging.FileHandler(logf, encoding="utf-8")
    handler.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(name)s: %(message)s"))
    console = logging.StreamHandler()
    console.setLevel(logging.INFO)          # 屏幕只看 INFO；DEBUG 进文件（SDK 的 basicConfig 见到已有处理器就不加了）
    console.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(name)s: %(message)s"))
    logging.getLogger().addHandler(handler)
    logging.getLogger().addHandler(console)
    logging.getLogger().setLevel(logging.DEBUG)
    for noisy in ("websockets", "websockets.client", "websockets.protocol"):
        logging.getLogger(noisy).setLevel(logging.INFO)  # DEBUG 会打握手头里的 Bearer
    print(f"日志 → {logf}（invoke 记录 → {HOME / 'invokes.jsonl'}）", flush=True)

    argv = ["-v", "send-user-msg" if a.cmd == "send" else a.cmd]
    if a.cmd == "pair" and a.force:
        argv.append("--force")
    if a.cmd == "pair" and a.timeout:
        argv += ["--timeout", str(a.timeout)]
    if a.cmd == "run":
        argv += ["--run-as", os.environ.get("USER", "")]
    if a.cmd == "send":
        argv += a.rest
    return cli.main(argv)


if __name__ == "__main__":
    sys.exit(main())
