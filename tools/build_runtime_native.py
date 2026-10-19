#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Source-build the GPL runtime, ARM64 Boost, talloc and patched PRoot.

All generated files stay in out/. No LineageOS source/output is modified.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "out/ananbox-native"

def run(command, cwd=None, env=None):
    print("+ " + " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), cwd=cwd, env=env, check=True)

def download(lock, filename):
    path = WORK / filename
    if not path.exists():
        temporary = path.with_suffix(path.suffix + ".part")
        urllib.request.urlretrieve(lock["url"], temporary)
        temporary.rename(path)
    if hashlib.file_digest(path.open("rb"), "sha256").hexdigest() != lock["sha256"]:
        raise SystemExit(f"Unexpected dependency archive: {path}")
    return path

def extract(path, directory):
    if directory.exists():
        return
    with tarfile.open(path) as archive:
        archive.extractall(WORK, filter="data")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ndk", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    lock = json.loads((ROOT / "sources.lock.json").read_text())
    for name in ("anbox", "proot"):
        source = ROOT / lock[name]["path"]
        revision = subprocess.check_output(["git", "-C", source, "rev-parse", "HEAD"], text=True).strip()
        dirty = subprocess.check_output(["git", "-C", source, "status", "--porcelain"], text=True)
        if revision != lock[name]["commit"] or dirty:
            raise SystemExit(f"{name} source differs from sources.lock.json")
    llvm = args.ndk.resolve() / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    cc = llvm / "aarch64-linux-android28-clang"
    if not cc.is_file():
        raise SystemExit(f"Missing Android NDK compiler: {cc}")
    WORK.mkdir(parents=True, exist_ok=True)
    # The previous exploratory build used this filename for the bzip2 archive.
    boost_archive = WORK / "boost.tar.xz"
    if boost_archive.exists() and not (WORK / "boost.tar.bz2").exists():
        boost_archive.rename(WORK / "boost.tar.bz2")
    extract(download(lock["boost"], "boost.tar.bz2"), WORK / "boost_1_83_0")
    boost = WORK / "boost_1_83_0"
    prefix = WORK / "boost-prefix"
    user = WORK / "boost-user.jam"
    user.write_text(f'using clang : android : {cc}++ : <archiver>{llvm / "llvm-ar"} <ranlib>{llvm / "llvm-ranlib"} <compileflags>-fPIC ;\n')
    if not (prefix / "lib/libboost_log_setup.a").exists():
        run(["./bootstrap.sh", "--with-toolset=clang"], cwd=boost)
        components = ("filesystem", "system", "thread", "serialization", "program_options", "log", "chrono", "atomic", "date_time")
        run(["./b2", f"--user-config={user}", "toolset=clang-android", "target-os=android",
             "architecture=arm", "address-model=64", "link=static", "threading=multi", "cxxstd=14",
             "variant=release", *["--with-" + c for c in components], f"-j{args.jobs}",
             f"--prefix={prefix}", "install"], cwd=boost)
    extract(download(lock["talloc"], "talloc.tar.gz"), WORK / "talloc-2.4.1")
    talloc = WORK / "talloc-2.4.1"
    tprefix = WORK / "talloc-prefix"
    if not (tprefix / "lib/libtalloc.a").exists():
        run(["./configure", f"CC={cc}", f"--prefix={tprefix}", "--disable-rpath", "--disable-python",
             "--without-gettext", "--cross-compile", f"--cross-answers={ROOT / 'runtime/talloc.cross'}"], cwd=talloc)
        run(["make", f"-j{args.jobs}"], cwd=talloc)
        (tprefix / "include").mkdir(parents=True, exist_ok=True)
        (tprefix / "lib").mkdir(parents=True, exist_ok=True)
        run([llvm / "llvm-ar", "rcs", tprefix / "lib/libtalloc.a", *talloc.glob("bin/default/talloc.c*.o")])
        shutil.copy2(talloc / "talloc.h", tprefix / "include/talloc.h")
    proot = WORK / "proot-build-src"
    patch = ROOT / "runtime/proot-android.patch"
    recipe = lock["proot"]["commit"] + hashlib.sha256(patch.read_bytes()).hexdigest() + str(llvm)
    stamp = proot / ".pocket-source"
    if not stamp.exists() or stamp.read_text() != recipe:
        if proot.exists():
            shutil.rmtree(proot)
        shutil.copytree(ROOT / "vendor/proot", proot, ignore=shutil.ignore_patterns(".git"))
        with patch.open() as source:
            subprocess.run(["patch", "-p1"], cwd=proot, stdin=source, check=True)
        stamp.write_text(recipe)
    run(["make", "-C", proot / "src", f"-j{args.jobs}", "HAS_SWIG=", "HAS_PYTHON_CONFIG=",
         f"GIT_VERSION=pocket", f"GIT_COMMIT={lock['proot']['commit'][:8]}",
         f"CC={cc}", f"LD={cc}", f"STRIP={llvm / 'llvm-strip'}", f"OBJCOPY={llvm / 'llvm-objcopy'}",
         f"OBJDUMP={llvm / 'llvm-objdump'}", f"CFLAGS=-O2 -I{tprefix / 'include'} -D_GNU_SOURCE",
         f"LDFLAGS={tprefix / 'lib/libtalloc.a'} -Wl,-z,max-page-size=16384"])
    output = ROOT / "out/runtime-native"
    output.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / "vendor/anbox/src/anbox/graphics/emugl/Renderer.cpp", output / "Renderer.cpp")
    with (ROOT / "runtime/anbox-renderer.patch").open() as source:
        subprocess.run(["patch", "-p1"], cwd=output, stdin=source, check=True)
    shutil.copy2(ROOT / "vendor/anbox/src/anbox/input/device.cpp", output / "Device.cpp")
    with (ROOT / "runtime/anbox-input.patch").open() as source:
        subprocess.run(["patch", "-p1"], cwd=output, stdin=source, check=True)
    run(["cmake", "-S", ROOT / "runtime/native", "-B", output, "-G", "Ninja",
         f"-DCMAKE_TOOLCHAIN_FILE={args.ndk.resolve() / 'build/cmake/android.toolchain.cmake'}",
         "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-28", "-DANDROID_STL=c++_static",
         "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
         "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH", "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH",
         f"-DBoost_DIR={prefix / 'lib/cmake/Boost-1.83.0'}"])
    run(["cmake", "--build", output, "--target", "pocket_runtime", f"-j{args.jobs}"])
    # Both grouped Make outputs have now been checked against the inputs.
    # Preserving the old PRoot mtime retriggered this recipe on every build.
    shutil.copyfile(proot / "src/proot", output / "libproot.so")
    (output / "libpocket_runtime.so").touch()

if __name__ == "__main__":
    main()
