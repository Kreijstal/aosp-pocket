#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build a separate Android 9 Binder bridge and two-process integration probe."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
from prepare_api28 import prepare
from target import ROOT, clang, info, selected


def build(ndk):
    arch = selected()
    reference = prepare(art=True, arch=arch)
    out = ROOT / "out/api28"
    native = out / "native"
    native.mkdir(parents=True, exist_ok=True)
    llvm = ndk.resolve() / "toolchains/llvm/prebuilt/linux-x86_64"
    headers = out / "headers"
    headers.mkdir(exist_ok=True)
    (headers / "sys").mkdir(exist_ok=True)
    shutil.copy2(out / "bionic/libc/include/sys/_system_properties.h", headers / "sys/_system_properties.h")
    includes = [headers, out / "frameworks-native/libs/binder/include"]
    includes += [out / "system-core" / (part + "/include") for part in
                 ("libutils", "libcutils", "liblog", "libsystem", "base")]
    common = [str(clang(llvm / "bin", 28, cxx=True, name=arch)), "-DPOCKET_GUEST_API=28",
              # Match Android 9 libc++ ABI instead of the modern NDK headers.
              "-nostdinc++", "-isystem", str(out / "libcxx/include"),
              "-std=c++14", "-O2", "-fno-rtti", "-fno-exceptions", "-nostdlib++", "-Wall", "-Wextra", "-Werror"]
    common += ["-I" + str(path) for path in includes]
    libs = reference / "system/lib64"
    link = ["-L" + str(libs), "-Wl,-rpath-link," + str(libs), "-lbinder", "-lutils", "-lc++", "-llog",
            "-Wl,--no-undefined", "-Wl,-z,max-page-size=16384"]
    bridge = native / "libpocket_binder_api28.so"
    probe = native / "pocket-api28-probe"
    property_service = native / "pocket-property-service-api28"
    property_probe = native / "pocket-property-probe-api28"
    init = native / "pocket-init-api28"
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
    # Build the actual pinned AOSP init core with the reviewed rootless patch.
    init_source = out / "rootless-init"
    if init_source.exists():
        shutil.rmtree(init_source)
    shutil.copytree(out / "system-core/init", init_source)
    subprocess.run(["patch", "--batch", "-p1", "-i", str(ROOT / "runtime/api28/init-rootless.patch")],
                   cwd=init_source, check=True)
    init_sources = [init_source / (name + ".cpp") for name in
                    ("action", "action_manager", "action_parser", "parser", "tokenizer",
                     "import_parser", "service", "rlimit_parser", "descriptors", "util")]
    subprocess.run(common + ["-std=c++17", "-fPIE", "-pie", "-DPOCKET_INIT_INTEGRATION",
                   "-Wno-unused-parameter", "-Wno-missing-field-initializers",
                   "-I" + str(init_source), "-I" + str(ROOT / "runtime/api28")]
                   + property_includes + list(map(str, init_sources + serializer_sources)) +
                   [str(parser / "property_info_parser.cpp"), str(ROOT / "runtime/api28/property_info.cpp"),
                    str(ROOT / "runtime/api28/property_service.cpp"), str(ROOT / "runtime/api28/init_main.cpp"),
                    str(libs / "libc.so"), "-lbase", "-lcutils"] + link + ["-o", str(init)], check=True)
    init_test = native / "pocket-init-test-api28"
    subprocess.run(common + ["-fPIE", "-pie", str(ROOT / "runtime/api28/init_test.cpp")]
                   + link + ["-o", str(init_test)], check=True)
    # AOSP app_process and native zygote fork hooks, separately adapted for
    # the rootless profile. The Java server remains the actual platform code.
    zygote_source = out / "rootless-frameworks-base"
    if zygote_source.exists():
        shutil.rmtree(zygote_source)
    shutil.copytree(out / "frameworks-base", zygote_source, ignore=shutil.ignore_patterns(".git"))
    subprocess.run(["patch", "--batch", "-p1", "-i", str(ROOT / "runtime/api28/zygote-rootless.patch")],
                   cwd=zygote_source, check=True)
    jni = zygote_source / "core/jni"
    zygote_includes = ["-I" + str(jni), "-I" + str(jni / "include"),
                       "-I" + str(out / "libnativehelper/include"),
                       "-I" + str(out / "libnativehelper/include_jni"),
                       "-I" + str(out / "libnativehelper/header_only_include"),
                       "-I" + str(out / "art/openjdkjvmti/include")]
    zygote_flags = common + ['-DPOCKET_GUEST_ISA="' + info(arch)["framework"] + '"',
                            "-std=c++17", "-Wno-unused-parameter", "-Wno-unused-function",
                            "-Wno-writable-strings"] + zygote_includes
    zygote = native / "libpocket_zygote_api28.so"
    app_process = native / "pocket-app-process-api28"
    subprocess.run(zygote_flags + ["-fPIC", "-shared", str(jni / "com_android_internal_os_Zygote.cpp"),
                   str(jni / "fd_utils.cpp"), str(ROOT / "runtime/api28/zygote_bootstrap.cpp"),
                   str(libs / "libc.so"), "-landroid_runtime", "-lnativehelper", "-lcutils", "-lbase"] + link +
                   ["-o", str(zygote)], check=True)
    subprocess.run(zygote_flags + ["-fPIE", "-pie", str(zygote_source / "cmds/app_process/app_main.cpp"),
                   str(out / "art/sigchainlib/sigchain.cc"), "-DART_TARGET_ANDROID",
                   "-Wl,--export-dynamic", "-Wl,--version-script=" + str(out / "art/sigchainlib/version-script64.txt"),
                   "-landroid_runtime", "-lcutils", "-lwilhelm", "-ldl"] + link + ["-o", str(app_process)], check=True)
    zygote_client = native / "pocket-zygote-client-api28"
    subprocess.run(common + ["-fPIE", "-pie", str(ROOT / "runtime/api28/zygote_client.cpp")]
                   + link + ["-o", str(zygote_client)], check=True)
    log_writer = native / "pocket-log-writer-api28"
    subprocess.run(common + ["-fPIE", "-pie", str(ROOT / "runtime/api28/log_writer.cpp")]
                   + link + ["-o", str(log_writer)], check=True)
    # Include exactly the dependency closure, not the entire reference image.
    stage = out / "probe-rootfs"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir()
    queue = [(bridge, "system/lib64/" + bridge.name), (probe, "system/bin/" + probe.name),
             (property_service, "system/bin/" + property_service.name),
             (property_probe, "system/bin/" + property_probe.name),
             (reference / "system/bin/linker64", "system/bin/linker64")]
    queue.append((reference / "system/bin/dalvikvm64", "system/bin/dalvikvm64"))
    queue.append((init, "system/bin/" + init.name))
    queue.append((init_test, "system/bin/" + init_test.name))
    queue.append((zygote, "system/lib64/" + zygote.name))
    queue.append((app_process, "system/bin/" + app_process.name))
    queue.append((zygote_client, "system/bin/" + zygote_client.name))
    queue.append((log_writer, "system/bin/" + log_writer.name))
    # ART and core JNI libraries are loaded dynamically, beyond DT_NEEDED.
    for name in ("libart.so", "libjavacore.so", "libopenjdk.so", "libopenjdkjvm.so", "libopenjdkjvmti.so"):
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
    data_files.add("system/etc/preloaded-classes")
    data_files.update(path.relative_to(reference).as_posix() for path in
                      (reference / "system/framework" / info(arch)["framework"]).glob("boot*"))
    data_files.update(path.relative_to(reference).as_posix() for path in (reference / "system/usr").rglob("*") if path.is_file())
    for name in sorted(data_files):
        destination = stage / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(reference / name, destination)
    for path in (ROOT / "runtime/api28").glob("*.rc"):
        destination = stage / "system/etc/init" / path.name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(path.read_text().replace("@BOOTCLASSPATH@", bootclasspath)
                               .replace("--abi-list=arm64-v8a", "--abi-list=" + info(arch)["abi"]))
        data_files.add(destination.relative_to(stage).as_posix())
    (stage / "tmp").mkdir(exist_ok=True)
    elf = subprocess.check_output([readelf, "-hlsWd", bridge], text=True)
    assert info(arch)["elf_machine"] in elf and "__ndk1" not in elf
    assert "_ZN7android12ProcessState16getContextObject" in elf
    assert "HostBinder" not in elf, "API 25 class ABI must not leak into API 28"
    print(f"PASS: API 28 bridge + probe linked --no-undefined; {len(done)} ELF files staged in {stage}")
    (out / "build.json").write_text(json.dumps({"sdk": 28, "status": "native_build_passed",
        "full_guest_boot_verified": False, "files": sorted(done), "data_files": sorted(data_files)}, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path, required=True)
    build(parser.parse_args().ndk)
