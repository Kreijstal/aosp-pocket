#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the controlled syscall ABI; no PRoot, proprietary code or new loader."""
import argparse
from pathlib import Path
import shutil
import subprocess
from prepare_api28 import prepare, ROOT

WRAPS = ("open", "openat", "stat", "lstat", "fstatat", "readlink", "readlinkat",
         "mkdir", "mkdirat", "unlink", "unlinkat", "rmdir", "symlink", "symlinkat",
         "chdir", "fchdir", "getcwd", "execve")


def compile_probe(compiler, output, android=False):
    output.parent.mkdir(parents=True, exist_ok=True)
    command = [str(compiler), "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
               "-fPIE", "-pie", "-Wl,-z,max-page-size=16384", "-Wl,--no-undefined"]
    command += [str(ROOT / "runtime/direct" / name) for name in ("guest_paths.c", "probe.c")]
    command += ["-Wl,--wrap=" + name for name in WRAPS]
    if android:
        # Reserve space for an app-private PT_INTERP at extraction time. Android
        # 9's linker cannot load a program through a command-line invocation.
        command += ["-Wl,--dynamic-linker=/pocket/" + "x" * 504]
    command += ["-ldl", "-pthread", "-o", str(output)]
    subprocess.run(command, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path)
    parser.add_argument("--host", action="store_true")
    args = parser.parse_args()
    out = ROOT / "out/direct"
    if args.host:
        compile_probe("cc", out / "host-probe")
        return
    if not args.ndk:
        parser.error("--ndk or --host required")
    reference = prepare(art=True)
    stage = out / "rootfs"
    if stage.exists():
        shutil.rmtree(stage)
    (stage / "system/bin").mkdir(parents=True)
    (stage / "system/lib64").mkdir()
    llvm = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    binary = stage / "system/bin/pocket-direct-probe"
    compile_probe(llvm / "aarch64-linux-android28-clang", binary, android=True)
    for name in ("libc.so", "libm.so", "libdl.so"):
        shutil.copy2(reference / "system/lib64" / name, stage / "system/lib64" / name)
    shutil.copy2(reference / "system/bin/linker64", stage / "system/bin/linker64")
    shutil.copy2(reference / "system/build.prop", stage / "system/build.prop")
    dynamic = subprocess.check_output([llvm / "llvm-readelf", "-d", binary], text=True)
    assert "libc.so" in dynamic and "libdl.so" in dynamic and "libc++" not in dynamic
    (out / "build.stamp").touch()
    print("PASS: ARM64 controlled guest ABI built; pinned Android 9 linker/bionic staged")


if __name__ == "__main__":
    main()
