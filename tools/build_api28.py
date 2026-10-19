#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build a separate Android 9 Binder bridge and two-process integration probe."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
from prepare_api28 import prepare, ROOT


def build(ndk):
    reference = prepare(art=True)
    out = ROOT / "out/api28"
    native = out / "native"
    native.mkdir(parents=True, exist_ok=True)
    llvm = ndk.resolve() / "toolchains/llvm/prebuilt/linux-x86_64"
    headers = out / "headers"
    headers.mkdir(exist_ok=True)
    (headers / "sys").mkdir(exist_ok=True)
    shutil.copy2(out / "bionic/libc/include/sys/_system_properties.h", headers / "sys/_system_properties.h")
    config = (llvm / "sysroot/usr/include/c++/v1/__config_site").read_text()
    if "#define _LIBCPP_ABI_NAMESPACE __ndk1" not in config:
        raise SystemExit("Unsupported NDK C++ namespace configuration")
    (headers / "__config_site").write_text(config.replace(
        "#define _LIBCPP_ABI_NAMESPACE __ndk1", "#define _LIBCPP_ABI_NAMESPACE __1"))
    includes = [headers, out / "frameworks-native/libs/binder/include"]
    includes += [out / "system-core" / (part + "/include") for part in
                 ("libutils", "libcutils", "liblog", "libsystem", "base")]
    common = [str(llvm / "bin/aarch64-linux-android28-clang++"), "-DPOCKET_GUEST_API=28",
              # API 28 libc++ predates the NDK's verbose-abort symbol. With
              # exceptions disabled, use the header's supported abort override.
              "-D_LIBCPP_VERBOSE_ABORT(...)=__builtin_abort()",
              "-std=c++14", "-O2", "-fno-rtti", "-fno-exceptions", "-nostdlib++", "-Wall", "-Wextra", "-Werror"]
    common += ["-I" + str(path) for path in includes]
    libs = reference / "system/lib64"
    link = ["-L" + str(libs), "-Wl,-rpath-link," + str(libs), "-lbinder", "-lutils", "-lc++", "-llog",
            "-Wl,--no-undefined", "-Wl,-z,max-page-size=16384"]
    bridge = native / "libpocket_binder_api28.so"
    probe = native / "pocket-api28-probe"
    property_service = native / "pocket-property-service-api28"
    property_probe = native / "pocket-property-probe-api28"
    subprocess.run(common + ["-fPIC", "-shared", str(ROOT / "runtime/api28/binder_bridge.cpp"),
                             str(ROOT / "runtime/api28/guest_credentials.cpp")]
                   + link + ["-Wl,-soname," + bridge.name, "-o", str(bridge)], check=True)
    subprocess.run(common + ["-fPIE", "-pie", str(ROOT / "runtime/api28/service_probe.cpp"),
                            "-L" + str(native), "-lpocket_binder_api28"] + link + ["-o", str(probe)], check=True)
    property_root = out / "system-core/property_service"
    serializer = property_root / "libpropertyinfoserializer"
    parser = property_root / "libpropertyinfoparser"
    property_includes = ["-I" + str(serializer / "include"), "-I" + str(parser / "include")]
    serializer_sources = [serializer / name for name in ("property_info_serializer.cpp", "trie_builder.cpp", "trie_serializer.cpp")]
    subprocess.run(common + ["-std=c++17", "-fPIE", "-pie"] + property_includes + list(map(str, serializer_sources)) +
                   [str(parser / "property_info_parser.cpp"), str(ROOT / "runtime/api28/property_info.cpp"),
                    str(ROOT / "runtime/api28/property_service.cpp"), str(libs / "libc.so"), "-lbase"] + link +
                   ["-o", str(property_service)], check=True)
    subprocess.run(common + ["-fPIE", "-pie", str(ROOT / "runtime/api28/property_probe.cpp")]
                   + link + ["-o", str(property_probe)], check=True)
    # Include exactly the dependency closure, not the entire reference image.
    stage = out / "probe-rootfs"
    stage.mkdir(exist_ok=True)
    queue = [(bridge, "system/lib64/" + bridge.name), (probe, "system/bin/" + probe.name),
             (property_service, "system/bin/" + property_service.name),
             (property_probe, "system/bin/" + property_probe.name),
             (reference / "system/bin/linker64", "system/bin/linker64")]
    queue.append((reference / "system/bin/dalvikvm64", "system/bin/dalvikvm64"))
    # ART and core JNI libraries are loaded dynamically, beyond DT_NEEDED.
    for name in ("libart.so", "libjavacore.so", "libopenjdk.so", "libopenjdkjvm.so"):
        queue.append((libs / name, "system/lib64/" + name))
    # Android 9 NativeLoader preloads its public namespace before the Java
    # entry point. Keep its actual list and supply the corresponding libraries.
    for line in (reference / "system/etc/public.libraries.txt").read_text().splitlines():
        parts = line.split("#", 1)[0].split()
        if parts and "32" not in parts[1:] and "nopreload" not in parts[1:]:
            queue.append((libs / parts[0], "system/lib64/" + parts[0]))
    done = set()
    readelf = llvm / "bin/llvm-readelf"
    while queue:
        source, target = queue.pop()
        if target in done:
            continue
        done.add(target)
        destination = stage / target
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        elf = subprocess.check_output([readelf, "-d", source], text=True)
        for name in re.findall(r"Shared library: \[([^]]+)\]", elf):
            dependency = native / name if (native / name).is_file() else libs / name
            if not dependency.is_file():
                raise SystemExit(f"Missing API 28 dependency: {name}")
            queue.append((dependency, "system/lib64/" + name))
    shutil.copy2(reference / "system/build.prop", stage / "system/build.prop")
    environment = (reference / "init.environ.rc").read_text()
    bootclasspath = re.search(r"export BOOTCLASSPATH (\S+)", environment)[1]
    (out / "bootclasspath.txt").write_text(bootclasspath + "\n")
    data_files = set(path.lstrip("/") for path in bootclasspath.split(":"))
    data_files.add("system/etc/event-log-tags")
    data_files.add("system/etc/public.libraries.txt")
    data_files.update(path.relative_to(reference).as_posix() for path in
                      (reference / "system/framework/arm64").glob("boot*"))
    data_files.update(path.relative_to(reference).as_posix() for path in (reference / "system/usr").rglob("*") if path.is_file())
    for name in sorted(data_files):
        destination = stage / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(reference / name, destination)
    (stage / "tmp").mkdir(exist_ok=True)
    elf = subprocess.check_output([readelf, "-hlsWd", bridge], text=True)
    assert "AArch64" in elf and "__ndk1" not in elf
    assert "_ZN7android12ProcessState16getContextObject" in elf
    assert "HostBinder" not in elf, "API 25 class ABI must not leak into API 28"
    print(f"PASS: API 28 bridge + probe linked --no-undefined; {len(done)} ELF files staged in {stage}")
    (out / "build.json").write_text(json.dumps({"sdk": 28, "status": "native_build_passed",
        "full_guest_boot_verified": False, "files": sorted(done), "data_files": sorted(data_files)}, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path, required=True)
    build(parser.parse_args().ndk)
