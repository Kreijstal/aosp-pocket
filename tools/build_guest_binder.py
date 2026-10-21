#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build only the guest Binder-bootstrap fix, against the reference guest ABI.

This does not rebuild the AOSP guest. Its existing libbinder/libutils/libc++
provide the ABI used by this small, source-built interposer.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import urllib.request
from target import ROOT, selected

def run(command, **kwargs):
    subprocess.run(list(map(str, command)), check=True, **kwargs)

def prepare_image(lock):
    destination = ROOT / "out/ananbox-unpacked"
    if (destination / "rootfs/system/lib64/libbinder.so").is_file():
        return destination / "rootfs"
    archive = ROOT / "out/ananbox-downloads/rootfs_arm64.7z"
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        temporary = archive.with_suffix(".part")
        urllib.request.urlretrieve(lock["reference_image_url"], temporary)
        temporary.rename(archive)
    with archive.open("rb") as source:
        if hashlib.file_digest(source, "sha256").hexdigest() != lock["reference_image_sha256"]:
            raise SystemExit("Reference guest archive differs from sources.lock.json")
    extracted = subprocess.run(["7z", "x", "-y", "-o" + str(destination), str(archive)],
        capture_output=True, text=True)
    expected = {"ERROR: Dangerous link path was ignored : rootfs/sbin/ueventd : ../init",
                "ERROR: Dangerous link path was ignored : rootfs/sbin/watchdogd : ../init"}
    errors = set(extracted.stderr.strip().splitlines())
    if extracted.returncode and (extracted.returncode != 2 or not errors or not errors <= expected):
        raise SystemExit(extracted.stdout + extracted.stderr)
    for name in ("ueventd", "watchdogd"):
        path = destination / "rootfs/sbin" / name
        if not path.is_symlink():
            if path.exists():
                if path.stat().st_size: raise SystemExit(f"Unexpected init link placeholder: {path}")
                path.unlink()
            path.symlink_to("../init")
    return destination / "rootfs"

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ndk", type=Path, required=True)
    args = parser.parse_args()
    if selected() != "arm64":
        # The Ananbox 7.1.1 guest is an arm64 rootfs; x86_64 uses the direct backend instead.
        raise SystemExit("PRoot guest build is arm64-only; use the direct backend for x86_64")
    lock = json.loads((ROOT / "sources.lock.json").read_text())
    for dependency in lock["guest_build_headers"]:
        source = ROOT / "out" / dependency["cache"]
        if not source.exists():
            run(["git", "clone", "--depth", "1", "--filter=blob:none", "--sparse", "-b",
                 dependency["branch"], dependency["url"], source])
        revision = subprocess.check_output(["git", "-C", source, "rev-parse", "HEAD"], text=True).strip()
        if revision != dependency["commit"]:
            # Branches can advance: always fetch and select the pinned commit.
            run(["git", "-C", source, "fetch", "--depth", "1", "origin", dependency["commit"]])
            run(["git", "-C", source, "checkout", "--detach", dependency["commit"]])
        run(["git", "-C", source, "sparse-checkout", "set", *dependency["paths"]])
        if subprocess.check_output(["git", "-C", source, "status", "--porcelain"], text=True):
            raise SystemExit(f"Guest build headers have local changes: {source}")
    guest = prepare_image(lock["guest_framework"])
    llvm = args.ndk.resolve() / "toolchains/llvm/prebuilt/linux-x86_64"
    headers = ROOT / "out/guest-headers"
    (headers / "linux").mkdir(parents=True, exist_ok=True)
    (headers / "linux/binder.h").write_text("#include <linux/android/binder.h>\n")
    # The system guest uses std::__1; the NDK defaults to std::__ndk1. Keep
    # this generated configuration override local and link --no-undefined
    # against the guest's actual libraries to reject incompatible symbols.
    config = (llvm / "sysroot/usr/include/c++/v1/__config_site").read_text()
    if "#define _LIBCPP_ABI_NAMESPACE __ndk1" not in config:
        raise SystemExit("Unsupported NDK C++ namespace configuration")
    (headers / "__config_site").write_text(config.replace(
        "#define _LIBCPP_ABI_NAMESPACE __ndk1", "#define _LIBCPP_ABI_NAMESPACE __1"))
    output = ROOT / "out/runtime-native/libpocket_binder_bootstrap.so"
    output.parent.mkdir(parents=True, exist_ok=True)
    run([llvm / "bin/aarch64-linux-android25-clang++", "-std=c++14", "-O2", "-fno-rtti",
         "-fno-exceptions", "-fPIC", "-shared", "-nostdlib++", "-I" + str(headers),
         "-I" + str(ROOT / "out/ananbox-frameworks-native/include"),
         "-I" + str(ROOT / "out/ananbox-system-core/include"),
         "-I" + str(ROOT / "out/guest-libnativehelper/include"),
         ROOT / "runtime/guest-binder-bootstrap.cpp", "-L" + str(guest / "system/lib64"),
         "-lbinder", "-lutils", "-lc++", "-llog", "-Wl,--no-undefined",
         "-Wl,-soname,libpocket_binder_bootstrap.so", "-Wl,-z,max-page-size=16384", "-o", output])
    print("PASS: guest Binder bootstrap linked against the reference guest ABI (--no-undefined)")

if __name__ == "__main__":
    main()
