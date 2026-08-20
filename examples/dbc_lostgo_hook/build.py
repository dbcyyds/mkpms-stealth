#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
一键编译 dbc_lostgo_hook：

  1) CMake 配置（NDK + Ninja）
  2) 编译 libdbc.so
  3) Python 把 so 转成内存数组 launcher/so_embed.c
  4) 编译可执行文件 dbc（内嵌 so）

用法（在项目根目录）:
  python build.py
  python build.py --clean
  python build.py --jobs 16
  python build.py --ndk "D:/SDK/ndk/28.2.13676358"
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SCRIPTS = ROOT / "scripts"
OUT_DIR = ROOT / "outputs" / "arm64-v8a"
BUILD_DIR = ROOT / "cmake-build-release-ndk28"
LIBDBC = OUT_DIR / "libdbc.so"
DBC_BIN = OUT_DIR / "dbc"
SO_EMBED_C = ROOT / "launcher" / "so_embed.c"
SO_EMBED_H = ROOT / "launcher" / "so_embed.h"

DEFAULT_NDK = Path(os.environ.get("ANDROID_NDK", r"D:\SDK\ndk\28.2.13676358"))
DEFAULT_CMAKE = Path(
    os.environ.get(
        "CMAKE",
        r"D:\Program Files\JetBrains\CLion 2026.1.1\bin\cmake\win\x64\bin\cmake.exe",
    )
)
DEFAULT_NINJA = Path(
    os.environ.get(
        "NINJA",
        r"D:\Program Files\JetBrains\CLion 2026.1.1\bin\ninja\win\x64\ninja.exe",
    )
)


def log(msg: str) -> None:
    print(msg, flush=True)


def run(cmd: list[str], cwd: Path | None = None) -> None:
    log(f"\n$ {' '.join(str(c) for c in cmd)}")
    r = subprocess.run(cmd, cwd=str(cwd or ROOT))
    if r.returncode != 0:
        raise SystemExit(r.returncode)


def which_or(path: Path, names: list[str]) -> Path:
    if path.is_file():
        return path
    for n in names:
        p = shutil.which(n)
        if p:
            return Path(p)
    return path


def embed_so() -> None:
    # 直接 import 同目录逻辑，避免子进程路径问题
    sys.path.insert(0, str(SCRIPTS))
    from embed_so import embed_so as do_embed  # type: ignore

    code = do_embed(LIBDBC, SO_EMBED_C, SO_EMBED_H)
    if code != 0:
        raise SystemExit(code)


def configure(cmake: Path, ninja: Path, ndk: Path) -> None:
    toolchain = ndk / "build" / "cmake" / "android.toolchain.cmake"
    if not toolchain.is_file():
        log(f"[!] NDK toolchain missing: {toolchain}")
        raise SystemExit(2)

    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    cache = BUILD_DIR / "CMakeCache.txt"
    need = True
    if cache.is_file():
        # 已配置过则跳过，除非 NDK 路径变了
        text = cache.read_text(encoding="utf-8", errors="ignore")
        if str(ndk).replace("\\", "/") in text.replace("\\", "/"):
            need = False
            log("[*] CMake already configured, skip")

    if not need:
        return

    run(
        [
            str(cmake),
            "-S",
            str(ROOT),
            "-B",
            str(BUILD_DIR),
            "-G",
            "Ninja",
            f"-DCMAKE_MAKE_PROGRAM={ninja}",
            f"-DNDK_PATH={ndk.as_posix()}",
            f"-DCMAKE_TOOLCHAIN_FILE={toolchain.as_posix()}",
            "-DANDROID_ABI=arm64-v8a",
            "-DANDROID_PLATFORM=android-30",
            "-DANDROID_STL=c++_static",
            "-DCMAKE_BUILD_TYPE=Release",
        ]
    )


def build_target(cmake: Path, target: str, jobs: int) -> None:
    run(
        [
            str(cmake),
            "--build",
            str(BUILD_DIR),
            "--target",
            target,
            "-j",
            str(jobs),
        ]
    )


def main() -> int:
    ap = argparse.ArgumentParser(description="Build libdbc.so + embed + dbc")
    ap.add_argument("--ndk", type=Path, default=DEFAULT_NDK)
    ap.add_argument("--cmake", type=Path, default=DEFAULT_CMAKE)
    ap.add_argument("--ninja", type=Path, default=DEFAULT_NINJA)
    ap.add_argument("--jobs", "-j", type=int, default=max(os.cpu_count() or 8, 4))
    ap.add_argument("--clean", action="store_true", help="delete build dir first")
    ap.add_argument(
        "--embed-only",
        action="store_true",
        help="only convert existing libdbc.so to so_embed.c",
    )
    ap.add_argument(
        "--so-only",
        action="store_true",
        help="only build libdbc.so (no embed/dbc)",
    )
    args = ap.parse_args()

    cmake = which_or(args.cmake, ["cmake"])
    ninja = which_or(args.ninja, ["ninja"])
    ndk = args.ndk

    log("=" * 56)
    log(" dbc_lostgo_hook one-shot build")
    log("=" * 56)
    log(f"  ROOT   = {ROOT}")
    log(f"  NDK    = {ndk}")
    log(f"  CMAKE  = {cmake}")
    log(f"  NINJA  = {ninja}")
    log(f"  JOBS   = {args.jobs}")

    if not cmake.is_file() and not shutil.which("cmake"):
        log("[!] cmake not found")
        return 2
    if not ndk.is_dir():
        log(f"[!] NDK not found: {ndk}")
        return 2

    t0 = time.time()

    if args.embed_only:
        return embed_so() or 0

    if args.clean and BUILD_DIR.exists():
        log(f"[*] clean {BUILD_DIR}")
        shutil.rmtree(BUILD_DIR)

    # ---- 1) configure ----
    log("\n[1/4] CMake configure")
    configure(cmake, ninja, ndk)

    # ---- 2) build libdbc.so ----
    log("\n[2/4] Build libdbc.so (target dbc_so)")
    build_target(cmake, "dbc_so", args.jobs)
    if not LIBDBC.is_file():
        log(f"[!] missing {LIBDBC}")
        return 3
    log(f"[+] {LIBDBC}  ({LIBDBC.stat().st_size} bytes)")

    if args.so_only:
        log("\n[--so-only] done")
        return 0

    # ---- 3) so -> C array ----
    log("\n[3/4] Embed SO -> launcher/so_embed.c (byte array)")
    embed_so()

    # ---- 4) build dbc ----
    log("\n[4/4] Build dbc executable (target dbc_bin)")
    build_target(cmake, "dbc_bin", args.jobs)
    if not DBC_BIN.is_file():
        log(f"[!] missing {DBC_BIN}")
        return 4

    dt = time.time() - t0
    log("\n" + "=" * 56)
    log(" BUILD OK")
    log("=" * 56)
    log(f"  libdbc.so : {LIBDBC}  ({LIBDBC.stat().st_size:,} bytes)")
    log(f"  so_embed  : {SO_EMBED_C}  ({SO_EMBED_C.stat().st_size:,} bytes)")
    log(f"  dbc       : {DBC_BIN}  ({DBC_BIN.stat().st_size:,} bytes)")
    log(f"  time      : {dt:.1f}s")
    log("\nDeploy:")
    log("  adb push outputs\\arm64-v8a\\dbc /data/local/tmp/dbc")
    log("  adb shell su -c /data/local/tmp/dbc")
    log("  adb logcat -s DbcHK")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\n[!] aborted", file=sys.stderr)
        raise SystemExit(130)
