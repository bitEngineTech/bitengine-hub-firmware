#!/usr/bin/env python3
"""防串刷真值表的变异测试：把 ../ota_profile.c / .h 各改坏一处，profile_test 必须变红（CLAUDE.md 7.4、docs/15 设计 2.7）。

  python3 components/ota_cloud/test_host/mutate_profile.py

在临时目录里改副本、用 gcc 编，不碰仓库里的文件。tools/build_profiles.sh 每次都跑它。
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC_DIR = os.path.dirname(HERE)
CC = os.environ.get("CC", "gcc")

MUTATIONS = [  # (说明, 文件, 原文, 改成)
    ("长度不先比（前缀相同算相等）", "ota_profile.c",
     "    if (rn != fn || memcmp(running, field, rn) != 0) return OTA_PROFILE_MISMATCH;",
     "    if (memcmp(running, field, rn) != 0) return OTA_PROFILE_MISMATCH;"),
    ("空名不拒", "ota_profile.c", "    if (rn == 0 || fn == 0) return OTA_PROFILE_NO_NAME;\n", ""),
    ("截断判据差一", "ota_profile.c",
     "    if (len < OTA_PROFILE_MIN_IMAGE_LEN) return OTA_PROFILE_TRUNCATED;",
     "    if (len + 1 < OTA_PROFILE_MIN_IMAGE_LEN) return OTA_PROFILE_TRUNCATED;"),
    ("运行中名字按 33 截", "ota_profile.c",
     "    size_t rn = bounded_len(running, OTA_PROFILE_NAME_LEN);",
     "    size_t rn = bounded_len(running, OTA_PROFILE_NAME_LEN + 1);"),
    ("不验描述 magic", "ota_profile.c",
     "    if (rd_le32(img + OTA_PROFILE_DESC_OFFSET) != OTA_PROFILE_DESC_MAGIC) return OTA_PROFILE_BAD_DESC_MAGIC;\n", ""),
    ("不验头 magic", "ota_profile.c", "    if (img[0] != OTA_PROFILE_IMAGE_MAGIC) return OTA_PROFILE_BAD_IMAGE_MAGIC;\n", ""),
    ("错误串改名", "ota_profile.c",
     '    case OTA_PROFILE_MISMATCH:        return "profile_mismatch";',
     '    case OTA_PROFILE_MISMATCH:        return "mismatch";'),
    ("check_image 不先验 running", "ota_profile.c",
     "    if (!running) return OTA_PROFILE_INVALID_ARG;   /* 参数先验：别让镜像的毛病盖住调用方的错 */\n", ""),
    ("工程名偏移写错", "ota_profile.h",
     "#define OTA_PROFILE_NAME_OFFSET     48u", "#define OTA_PROFILE_NAME_OFFSET     16u"),
]


def main() -> int:
    fails = 0
    with tempfile.TemporaryDirectory() as td:
        os.makedirs(os.path.join(td, "test_host"))
        for f in ("ota_profile.c", "ota_profile.h"):
            shutil.copy(os.path.join(SRC_DIR, f), td)
        shutil.copy(os.path.join(HERE, "profile_test.c"), os.path.join(td, "test_host"))
        exe = os.path.join(td, "t")

        def build_and_run() -> int:
            r = subprocess.run([CC, "-std=c11", "-w", "-I" + td, "-o", exe,
                                os.path.join(td, "test_host", "profile_test.c"), os.path.join(td, "ota_profile.c")],
                               capture_output=True, text=True)
            if r.returncode != 0:
                return 2
            # profile_test 读 ../../../build/bitengine.bin（相对 test_host/），在真目录里跑
            return subprocess.run([exe], cwd=HERE, capture_output=True, text=True).returncode

        if build_and_run() != 0:
            print("❌ 原样的 profile_test 就不过，先修再变异")
            return 1
        for why, fname, old, new in MUTATIONS:
            path = os.path.join(td, fname)
            orig = open(path, encoding="utf-8").read()
            if orig.count(old) != 1:
                print(f"⚠️ {why}：锚点出现 {orig.count(old)} 次，变异没做成（源码改了？同步改这里）")
                fails += 1
                continue
            open(path, "w", encoding="utf-8").write(orig.replace(old, new))
            red = build_and_run() != 0
            open(path, "w", encoding="utf-8").write(orig)
            print(("✅ 抓到 " if red else "❌ 漏了 ") + why)
            fails += 0 if red else 1
    print(f"\n变异：{'全部抓到' if not fails else f'{fails} 处没抓到'}（{len(MUTATIONS)} 个）")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
