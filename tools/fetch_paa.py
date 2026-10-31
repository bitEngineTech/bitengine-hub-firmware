#!/usr/bin/env python3
"""从 CSA DCL 主网取 PAA（产品认证根）快照，生成 matter_ctrl 内置的信任库（docs/15 设计 9.7；TODO T9.53，M7 第三步）。

── 为什么内置 ────────────────────────────────────────────────────────────
设备认证（DAC → PAI → PAA）要一份 PAA 根证书集合。esp-matter 有三种来源：测试库（只有 CHIP 的测试 PAA）、SPIFFS 分区
（#159 单开 paa_cert 分区，只能 USB 线刷更新 —— 本板没有 BOOT 键，装出去就再也换不了）、配网时在线查 DCL（要外网与 TLS、
把设备的 SKID 发给 DCL，外网断时配不了）。用户 10-24 按默认定：**内置快照**，PAA 进 app、随 OTA 更新（设计 9.7）。
新厂商的 PAA 要等下一版固件：发版前跑一次本脚本刷新（components/matter_ctrl/paa_snapshot.json 的差异要审）。

── 做什么 ────────────────────────────────────────────────────────────────
  1. GET https://on.dcl.csa-iot.org/dcl/pki/root-certificates → 已批准的根（subject, subjectKeyId）
  2. 逐个 GET /dcl/pki/certificates?subjectKeyId=… → pemCert（同一 SKID 有多张时取第一张已批准的）
  3. openssl 转 DER、核对：是 CA、SKID 与 DCL 报的一致、自签；不过的丢掉并报出来
  4. 写 components/matter_ctrl/paa_snapshot.c（C 数组）与 paa_snapshot.json（SKID、SHA-256、主题，审阅差异用）

  python3 tools/fetch_paa.py            # 联网取、重写两个文件
  python3 tools/fetch_paa.py --check    # 不联网：核对 .c 里的证书与 .json 清单逐个对得上（VERIFY 用）
"""
import argparse
import datetime
import hashlib
import json
import pathlib
import subprocess
import sys
import urllib.parse
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT_C = ROOT / "components" / "matter_ctrl" / "paa_snapshot.c"
OUT_J = ROOT / "components" / "matter_ctrl" / "paa_snapshot.json"
DCL = "https://on.dcl.csa-iot.org"


def get(path: str) -> dict:
    with urllib.request.urlopen(DCL + path, timeout=30) as r:
        return json.load(r)


def openssl(args, data: bytes) -> bytes:
    return subprocess.run(["openssl"] + args, input=data, capture_output=True, check=True).stdout


def der_of(pem: str) -> bytes:
    return openssl(["x509", "-outform", "DER"], pem.encode())


def info_of(der: bytes) -> str:
    return openssl(["x509", "-inform", "DER", "-noout", "-text"], der).decode("utf-8", "replace")


def skid_of(text: str) -> str:
    lines = text.splitlines()
    for i, ln in enumerate(lines):
        if "Subject Key Identifier" in ln and i + 1 < len(lines):
            return lines[i + 1].strip().upper()
    return ""


def subject_of(der: bytes) -> str:
    return openssl(["x509", "-inform", "DER", "-noout", "-subject", "-nameopt", "RFC2253"], der).decode().strip().removeprefix("subject=")


def fetch() -> int:
    roots = get("/dcl/pki/root-certificates")["approvedRootCertificates"]["certs"]
    got, dropped = [], []
    for r in roots:
        skid = r["subjectKeyId"].upper()
        q = urllib.parse.quote(skid, safe="")
        d = get(f"/dcl/pki/certificates?subjectKeyId={q}")
        certs = [c for e in d.get("approvedCertificates", []) for c in e.get("certs", [])]
        pem = next((c["pemCert"] for c in certs if c.get("pemCert") and c.get("isRoot", True)), None)
        if not pem:
            dropped.append((skid, "DCL 没给证书"))
            continue
        try:
            der = der_of(pem)
            text = info_of(der)
        except subprocess.CalledProcessError as e:
            dropped.append((skid, f"openssl 解不开：{e.stderr[:80]!r}"))
            continue
        why = None
        if "CA:TRUE" not in text:
            why = "不是 CA"
        elif skid_of(text) != skid:
            why = f"证书里的 SKID {skid_of(text)} 与 DCL 报的不一致"
        elif subject_of(der) != openssl(["x509", "-inform", "DER", "-noout", "-issuer", "-nameopt", "RFC2253"], der).decode().strip().removeprefix("issuer="):
            why = "不是自签（主题与签发者不同）"
        if why:
            dropped.append((skid, why))
            continue
        got.append({"skid": skid, "sha256": hashlib.sha256(der).hexdigest(), "subject": subject_of(der), "der": der})
    got.sort(key=lambda x: x["skid"])
    today = datetime.date.today().isoformat()
    manifest = {"source": DCL + "/dcl/pki/root-certificates", "fetched": today, "count": len(got),
                "dropped": [{"skid": s, "why": w} for s, w in dropped],
                "paa": [{k: v for k, v in g.items() if k != "der"} for g in got]}
    OUT_J.write_text(json.dumps(manifest, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    lines = ["/* paa_snapshot.c —— 内置的 PAA（Matter 产品认证根）信任库。⚠️ 自动生成：python3 tools/fetch_paa.py，别手改。",
             f" * 来源 {DCL}（CSA DCL 主网已批准的根），{today} 取，{len(got)} 张；清单与 SHA-256 在 paa_snapshot.json（审阅差异用）。",
             " * 不含 CHIP 的测试 PAA：测试设备默认拒绝（设计 9.7）。 */",
             '#include "mc_paa.h"', ""]
    for i, g in enumerate(got):
        hexs = ", ".join(f"0x{b:02x}" for b in g["der"])
        lines.append(f"/* {g['subject'][:100]} */")
        lines.append(f"static const uint8_t k_paa_{i}[] = {{ {hexs} }};")
    lines.append("")
    lines.append("const mc_paa_cert_t mc_paa_certs[] = {")
    for i in range(len(got)):
        lines.append(f"    {{ k_paa_{i}, sizeof(k_paa_{i}) }},")
    lines.append("};")
    lines.append(f"const unsigned mc_paa_count = {len(got)};")
    lines.append(f'const char mc_paa_snapshot_date[] = "{today}";')
    OUT_C.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"fetch_paa: {len(got)} 张写进 {OUT_C.relative_to(ROOT)}；丢掉 {len(dropped)} 张")
    for s, w in dropped:
        print(f"  ⚠️ {s}：{w}")
    return 0


def check() -> int:
    if not OUT_C.is_file() or not OUT_J.is_file():
        print("fetch_paa --check：快照文件不在（先联网跑一次 tools/fetch_paa.py）", file=sys.stderr)
        return 1
    import re
    src = OUT_C.read_text(encoding="utf-8")
    man = json.loads(OUT_J.read_text(encoding="utf-8"))
    arrays = re.findall(r"static const uint8_t k_paa_(\d+)\[\] = \{ ([^}]*) \};", src)
    ders = [bytes(int(x, 16) for x in body.split(", ")) for _, body in arrays]
    ok = len(ders) == man["count"] == len(man["paa"]) and f"mc_paa_count = {len(ders)};" in src
    for d, m in zip(ders, man["paa"]):
        ok = ok and hashlib.sha256(d).hexdigest() == m["sha256"]
    if ok and len(ders) >= 50:
        print(f"fetch_paa --check：{len(ders)} 张与清单逐个对得上（{man['fetched']} 取）")
        return 0
    print(f"fetch_paa --check：不一致（.c {len(ders)} 张、清单 {man.get('count')} 张，或 SHA-256 对不上，或少于 50 张）", file=sys.stderr)
    return 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    sys.exit(check() if a.check else fetch())
