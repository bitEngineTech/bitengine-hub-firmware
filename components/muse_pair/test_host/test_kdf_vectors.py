"""固件侧密钥派生对上与 App 共享的 v5 向量（本仓库自加；缺口与原理见 kdf_vectors_harness.c 文件头）。

由 run.sh 复制到上游布局的 tests/ 下运行，目录约定与上游 test_link_pairing_handshake 相同。
crypto 的编法照抄上游那份（IDF 自带 tf-psa-crypto，纯软件实现）。
"""
import json
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VECTORS = json.loads((ROOT / "tests/vectors/link_pairing_v5.json").read_text())["vectors"]


class KdfVectorsTest(unittest.TestCase):
    def test_firmware_derivation_matches_shared_vectors(self) -> None:
        # 向量文件结构一变、变成 0 条，下面的循环就空过了
        self.assertGreaterEqual(len(VECTORS), 3, "v5 向量少于 3 条：上游改了向量文件结构？")
        idf_path = os.environ.get("IDF_PATH")
        # 不许跳过：跳过就等于这条缺口又没人看
        self.assertTrue(idf_path, "IDF_PATH 未设置")
        mbedtls = Path(idf_path).resolve() / "components/mbedtls/mbedtls"
        crypto = mbedtls / "tf-psa-crypto"
        cjson = (ROOT / "managed_components/espressif__cjson/cJSON").resolve()
        cc = shlex.split(os.environ.get("CC", "cc"))
        cmake = shutil.which("cmake")
        self.assertTrue(cmake and cc and shutil.which(cc[0]), "需要主机 C 编译器和 CMake")

        def run(command, cwd):
            r = subprocess.run(command, cwd=cwd, text=True, capture_output=True)
            self.assertEqual(r.returncode, 0, r.stdout[-8000:] + r.stderr[-8000:])
            return r.stdout

        with tempfile.TemporaryDirectory(prefix="kdf-vectors-") as d:
            temp = Path(d)
            compat = temp / "compat"
            (compat / "mbedtls").mkdir(parents=True)
            for name in ("bignum", "ecp"):
                (compat / "mbedtls" / f"{name}.h").write_text(
                    '#pragma once\n#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS\n'
                    f'#include "mbedtls/private/{name}.h"\n')
            (compat / "sdkconfig.h").write_text("")
            build = temp / "crypto-build"
            run([cmake, "-S", str(mbedtls), "-B", str(build),
                 "-DENABLE_TESTING=OFF", "-DENABLE_PROGRAMS=OFF", "-DGEN_FILES=OFF",
                 "-DMBEDTLS_FATAL_WARNINGS=OFF", "-DDISABLE_PACKAGE_CONFIG_AND_INSTALL=ON",
                 "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_C_COMPILER={cc[0]}",
                 "-DCMAKE_C_FLAGS=-I" + shlex.quote(str(compat))], temp)
            run([cmake, "--build", str(build), "--target", "tfpsacrypto", "-j", "4"], temp)
            built = build / "tf-psa-crypto"
            libs = [built / "core/libtfpsacrypto.a"] + sorted((built / "drivers").glob("*/lib*.a"))
            exe = temp / "kdf_vectors"
            cmd = [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-function", "-Wno-unused-variable"]
            for inc in (ROOT / "tests/pairing_fakes", compat, ROOT / "main", cjson,
                        crypto / "include", crypto / "drivers/builtin/include", crypto / "core",
                        crypto / "drivers/builtin/src", built / "core"):
                cmd += ["-I", str(inc)]
            cmd += [str(ROOT / "tests/kdf_vectors_harness.c"),
                    str(ROOT / "main/pairing_transcript.c"),
                    str(ROOT / "main/pairing_signer_policy.c"), str(cjson / "cJSON.c"),
                    "-Wl,--start-group", *map(str, libs), "-Wl,--end-group", "-o", str(exe)]
            run(cmd, temp)

            for v in VECTORS:
                with self.subTest(vector=v["name"]):
                    out = run([str(exe), v["device_private_scalar_hex"], v["mobile_pub"],
                               v["mobile_nonce"], v["device_nonce"], v["transcript"],
                               v["client_finished_ciphertext"], v["client_finished_tag"]], temp)
                    got = dict(kv.split("=", 1) for kv in out.rstrip("\n").split(" ", 3))
                    # 固件收 = 手机发，固件发 = 手机收
                    self.assertEqual(got["rx"], v["mobile_tx_key_hex"])
                    self.assertEqual(got["tx"], v["mobile_rx_key_hex"])
                    self.assertEqual(got["sid"], v["session_id"])
                    self.assertEqual(got["plain"], v["client_finished_plaintext"])


if __name__ == "__main__":
    unittest.main()
