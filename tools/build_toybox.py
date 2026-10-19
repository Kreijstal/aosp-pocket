#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build a small, pinned AOSP Toybox with bionic; keep vendor sources untouched."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ndk", required=True, type=Path)
    args = parser.parse_args()
    lock = json.loads((ROOT / "sources.lock.json").read_text())["toybox"]
    source = ROOT / lock["path"]
    actual = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if actual != lock["commit"]:
        raise SystemExit(f"Toybox revision differs from sources.lock.json: {actual}")
    if subprocess.check_output(["git", "-C", str(source), "status", "--porcelain"], text=True):
        raise SystemExit("Toybox source has local modifications; record a new source revision first")
    llvm = args.ndk.resolve() / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    cc = llvm / "aarch64-linux-android28-clang"
    if not cc.is_file():
        raise SystemExit(f"NDK compiler missing: {cc}")
    dest = ROOT / "out/toybox-src"
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(source, dest, ignore=shutil.ignore_patterns(".git"))
    env = dict(os.environ, CC=str(cc), HOSTCC="cc", CPUS="4",
               CFLAGS="-fPIE", LDFLAGS="-pie -Wl,-z,max-page-size=16384",
               STRIP=str(llvm / "llvm-strip"),
               VERSION=f"aosp-{lock['tag']}-{lock['commit'][:12]}")
    # KCONFIG_ALLCONFIG applies a small explicit seed while disabling everything else.
    subprocess.run(["make", "allnoconfig", f"KCONFIG_ALLCONFIG={ROOT / 'guest/toybox.config'}"],
                   cwd=dest, env=env, check=True)
    subprocess.run(["make", "toybox"], cwd=dest, env=env, check=True)
    output = ROOT / "out/lib/arm64-v8a/libtoybox_exec.so"
    output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(dest / "toybox", output)
    output.chmod(0o755)
    print(f"Built {output.relative_to(ROOT)} ({output.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
