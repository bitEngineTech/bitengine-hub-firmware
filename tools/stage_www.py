#!/usr/bin/env python3
"""汇集 www SPIFFS 镜像的内容到构建目录的 staging 目录，供 spiffsgen 打包成 www.bin。

由顶层 CMakeLists.txt 的 www_stage 目标调用，做两件事：

  1. 尽量重新生成前端（跑 frontend/build.sh，产出 www/index.html.gz）
  2. 生成 version.json —— 前端的身份标识，固件挂载 SPIFFS 后读出来，
     通过 /api/v1/status 的 www_version 字段上报

version.json 刻意**不含**构建时间戳，也不含固件版本号：只有前端内容真正变化时
www.bin 才会变。否则固件侧的纯代码改动会让 www 分区在每次 idf.py flash 时被无谓
重刷，烧录包里的 www.bin 也会莫名其妙"变新"而无法判断是否真的更新过。

version 字段格式：<最后改动前端的 git 短 sha>-<index.html.gz 内容 sha256 前 8 位>
后半段能暴露"改了前端但没提交"的情况——git sha 不变而内容 sha 变了。

直连型（--profile direct，docs/15 设计 2.7、A-29）：跑 frontend/direct/build.sh（由 HA 型的打包派生，产物在 www-direct/），
version.json 多一个 "profile":"direct"。HA 型的 git sha 不算 frontend/direct/ 下的提交 —— 否则只改直连型页面也会让 HA 型的
www.bin 变（version.json 在镜像里），违反"HA 型一点都不变"（架构 5.8）。不传 --profile 时产物与以前相同；唯一的差别是 git 段
不再算只动了 frontend/direct/ 的提交（2026-10-22 起；之前没有这个目录，所以对已有的提交算出的值不变）。
"""

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

INDEX = "index.html.gz"
VERSION_FILE = "version.json"


def log(msg):
    print(f"stage_www: {msg}")


def run_frontend_build(project_dir: Path, profile: str = "ha") -> None:
    """跑 frontend/build.sh（直连型：frontend/direct/build.sh）刷新 www/index.html.gz（直连型：www-direct/）。

    失败不致命——下面会退回到已存在的 www/index.html.gz。Windows 上通常没有
    bash/gzip，而按 docs/guide/WINDOWS_烧录指南.md 的约定 Windows 只烧录不构建。
    """
    script = project_dir / "frontend" / ("direct/build.sh" if profile == "direct" else "build.sh")
    if not script.is_file():
        log(f"跳过前端构建：{script} 不存在")
        return
    bash = shutil.which("bash")
    if not bash:
        log("跳过前端构建：环境中没有 bash")
        return
    try:
        subprocess.run([bash, str(script)], cwd=str(script.parent),
                       check=True, stdout=subprocess.DEVNULL)
        log("前端已重新构建")
    except subprocess.CalledProcessError as e:
        log(f"警告：frontend/build.sh 失败（exit {e.returncode}），沿用已有产物")


def frontend_git_ref(project_dir: Path, profile: str = "ha") -> str:
    """最后一次改动 frontend/ 的提交短 sha（HA 型不算 frontend/direct/）。非 git 仓库或无历史时返回 nogit。"""
    paths = ["frontend"] if profile == "direct" else ["frontend", ":(exclude)frontend/direct"]
    try:
        out = subprocess.run(
            ["git", "log", "-1", "--format=%h", "--", *paths],
            cwd=str(project_dir), check=True,
            capture_output=True, text=True).stdout.strip()
        return out or "nogit"
    except (subprocess.CalledProcessError, FileNotFoundError, OSError):
        return "nogit"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--project-dir", required=True, type=Path)
    ap.add_argument("--src", required=True, type=Path,
                    help="前端产物目录（通常是 <project>/www）")
    ap.add_argument("--stage", required=True, type=Path,
                    help="spiffsgen 的输入目录，本脚本会重建它")
    ap.add_argument("--profile", choices=("ha", "direct"), default="ha",
                    help="产品配置（docs/15 设计 2.7）；直连型的前端由 frontend/direct/build.sh 生成")
    args = ap.parse_args()

    # rmtree 的护栏：--stage 传错（比如误填成 www/ 或工程根）会直接删掉源文件
    stage = args.stage.resolve()
    for forbidden, name in ((args.src.resolve(), "--src"),
                            (args.project_dir.resolve(), "--project-dir")):
        if stage == forbidden:
            log(f"错误：--stage 不能等于 {name}（该目录会被清空重建）")
            return 1

    run_frontend_build(args.project_dir, args.profile)

    # 重建 staging 目录：残留的旧文件会被一起打进镜像，必须清干净
    if args.stage.exists():
        shutil.rmtree(args.stage)
    args.stage.mkdir(parents=True)

    index_src = args.src / INDEX
    if index_src.is_file():
        shutil.copy2(index_src, args.stage / INDEX)
        content_sha = hashlib.sha256(index_src.read_bytes()).hexdigest()
    else:
        # 固件侧能优雅降级：static_files.c 挂载失败/无 index 时返回纯文本 API 清单。
        # 但这几乎总是意味着前端没构建出来，所以要吵。
        log(f"警告：{index_src} 不存在，www.bin 将不含前端页面")
        content_sha = ""

    # src 下的其他资源（图标等）一并带上
    if args.src.is_dir():
        for extra in sorted(args.src.iterdir()):
            if extra.is_file() and extra.name not in (INDEX, VERSION_FILE):
                shutil.copy2(extra, args.stage / extra.name)

    git_ref = frontend_git_ref(args.project_dir, args.profile)
    version = f"{git_ref}-{content_sha[:8]}" if content_sha else f"{git_ref}-empty"

    meta = {
        "version": version,
        "git": git_ref,
        "sha256": content_sha,
    }
    if args.profile == "direct":
        meta["profile"] = "direct"         # 没有这个字段即 HA 型（同 /api/v1/status 的约定，设计 10.1）
    (args.stage / VERSION_FILE).write_text(
        json.dumps(meta, separators=(",", ":")) + "\n",
        encoding="utf-8")

    log(f"version={version}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
