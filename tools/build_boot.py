#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Isolated direct platform build using only pinned, byte-checked guest objects."""
import argparse
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
from prepare_api28 import ROOT, prepare
from prepare_boot_resources import prepare_resources
from build_boot_zygote import build_zygote

TARGETS = {
    "inotify_add_watch":"__wrap_inotify_add_watch",
    "timerfd_create":"boot_timerfd_create", "settimeofday":"boot_settimeofday",
    "getxattr":"__wrap_getxattr", "lgetxattr":"__wrap_lgetxattr",
    "setxattr":"__wrap_setxattr", "lsetxattr":"__wrap_lsetxattr",
    "listxattr":"__wrap_listxattr", "llistxattr":"__wrap_llistxattr",
    "removexattr":"__wrap_removexattr", "lremovexattr":"__wrap_lremovexattr",
    "__openat": "__wrap_openat", "__getcwd": "boot_getcwd", "__connect": "__wrap_connect",
    "fchown":"boot_fchown", "fstatat": "boot_fstatat", "fstat": "boot_fstat", "readlinkat": "__wrap_readlinkat",
    "mkdirat": "__wrap_mkdirat", "unlinkat": "__wrap_unlinkat", "renameat": "__wrap_renameat",
    "linkat": "__wrap_linkat", "fchownat": "boot_chown", "symlinkat": "__wrap_symlinkat",
    "utimensat": "__wrap_utimensat", "chdir": "__wrap_chdir", "fchdir": "__wrap_fchdir",
    "faccessat": "__wrap_faccessat", "fchmodat": "boot_chmod", "__statfs": "boot_statfs",
    "bind": "__wrap_bind", "getsockname": "__wrap_getsockname", "getpeername": "__wrap_getpeername",
    "execve": "__wrap_execve", "getuid": "boot_getuid", "geteuid": "boot_geteuid",
    "getgid": "boot_getgid", "getegid": "boot_getegid", "getresuid": "boot_getresuid",
    "getresgid": "boot_getresgid", "setresuid": "boot_setresuid", "setresgid": "boot_setresgid",
    "setuid": "boot_setuid", "setgid": "boot_setgid", "setreuid": "boot_setreuid",
    "setregid": "boot_setregid", "setgroups": "boot_setgroups", "getgroups": "boot_getgroups",
}


def headers(data):
    assert data[:6] == b"\x7fELF\x02\x01"
    offset, = struct.unpack_from("<Q", data, 32)
    size, count = struct.unpack_from("<HH", data, 54)
    return [(offset + n * size, struct.unpack_from("<IIQQQQQQ", data, offset+n*size)) for n in range(count)]


def reserve_interpreter(path):
    data = bytearray(path.read_bytes())
    for pos, entry in headers(data):
        if entry[0] != 3:
            continue
        offset = len(data)
        data += b"/pocket/" + b"x" * 504 + b"\0"
        struct.pack_into("<QQQQQ", data, pos+8, offset, 0, 0, 513, 513)
        path.write_bytes(data)
        return True
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path, required=True)
    parser.add_argument("--hidl-gen",type=Path,required=True)
    args = parser.parse_args()
    reference = prepare(art=True)
    llvm = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    out = ROOT / "out/boot"
    out.mkdir(parents=True, exist_ok=True)
    libc = reference / "system/lib64/libc.so"
    symbols = {}
    for line in subprocess.check_output([llvm / "llvm-nm", "-a", "-S", libc], text=True).splitlines():
        match = re.fullmatch(r"([0-9a-f]+) ([0-9a-f]+) \w (\S+)", line.strip())
        if match:
            symbols[match[3]] = (int(match[1], 16), int(match[2], 16))
    image = libc.read_bytes()
    loads = [p for _, p in headers(image) if p[0] == 1]
    lines = ["// Generated exclusively from the pinned guest libc symbol table.", "static Hook hooks[] = {"]
    manifest = {}
    for name, target in TARGETS.items():
        address, size = symbols[name]
        assert size >= 16, (name, size)
        segment = next(p for p in loads if p[3] <= address and address+16 <= p[3]+p[5])
        offset = segment[2] + address - segment[3]
        expected = image[offset:offset+16]
        lines.append('{"%s",0x%x,{%s},(void*)%s},' % (name,address,','.join(str(b) for b in expected),target))
        manifest[name] = {"offset": address, "expected": expected.hex(), "target": target}
    prop_address, prop_size = symbols["_ZL17system_properties"]
    assert prop_size == 0x450, "Review pinned Android 9 SystemProperties layout"
    lines += ["};", f"#define PROPERTY_STATE_OFFSET 0x{prop_address:x}", f"#define PROPERTY_STATE_SIZE {prop_size}"]
    (out / "boot_hooks.h").write_text('\n'.join(lines)+'\n')
    stage = out / "rootfs"
    if stage.exists():
        shutil.rmtree(stage)
    shutil.copytree(ROOT / "out/api28/probe-rootfs", stage)
    platform=prepare_resources()
    # Matching framework resources, stripped-JAR OAT/VDEX and core UI packages.
    for directory in ("system/etc", "system/fonts", "system/framework"):
        shutil.copytree(platform/directory,stage/directory,dirs_exist_ok=True)
    # The matching SystemUI requires the Wi-Fi framework service even when the
    # controller is unavailable. Restore its reference-image feature metadata;
    # do not import unrelated hardware/Vulkan capability declarations.
    permissions=stage/'vendor/etc/permissions';permissions.mkdir(parents=True,exist_ok=True)
    shutil.copy2(platform/'vendor/etc/permissions/android.hardware.wifi.xml',permissions/'android.hardware.wifi.xml')
    applications={
        "app":("ExtShared","KeyChain","LatinIME","CertInstaller","DeskClock"),
        "priv-app":("SystemUI","Launcher3QuickStep","SettingsProvider","Settings","PackageInstaller",
                    "DefaultContainerService","ExtServices","InputDevices","ExternalStorageProvider",
                    "MediaProvider","DownloadProvider","Shell","ProxyHandler","FusedLocation","Provision"),
    }
    for directory,names in applications.items():
        for name in names:
            relative=Path("system")/directory/name
            shutil.copytree(platform/relative,stage/relative,ignore=shutil.ignore_patterns("arm","armeabi-v7a"))
    for jar in (reference/"system/framework").glob("*.jar"):
        shutil.copy2(jar,stage/"system/framework"/jar.name)
    shutil.copy2(ROOT/'out/runtime/guest-test.apk',stage/'system/etc/pocket-smoke.apk')
    for binary in ("sh", "toybox", "surfaceflinger", "installd", "screencap", "keystore", "gatekeeperd"):
        shutil.copy2(platform/"system/bin"/binary,stage/"system/bin"/binary)
    vendor=platform/"vendor/lib64"
    for relative in ("egl/libEGL_swiftshader.so","egl/libGLESv1_CM_swiftshader.so","egl/libGLESv2_swiftshader.so",
                     "hw/android.hardware.graphics.composer@2.1-impl.so","hw/android.hardware.graphics.allocator@2.0-impl.so",
                     "hw/android.hardware.graphics.mapper@2.0-impl.so","hw/gralloc.redroid.so","hw/hwcomposer.redroid.so",
                     "hw/android.hardware.keymaster@3.0-impl.so"):
        destination=stage/"vendor/lib64"/relative;destination.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(vendor/relative,destination)
    libs=reference/"system/lib64"
    shutil.copytree(libs,stage/"system/lib64",dirs_exist_ok=True)
    queue=[libs/"libandroid_servers.so",platform/"system/bin/surfaceflinger",platform/"system/bin/installd"]
    queue+=list((stage/"vendor/lib64").rglob("*.so"))
    seen=set()
    while queue:
        source=queue.pop()
        if source.name in seen: continue
        seen.add(source.name)
        if source.parent==libs: shutil.copy2(source,stage/"system/lib64"/source.name)
        elf=subprocess.check_output([llvm/"llvm-readelf","-d",source],text=True)
        for name in re.findall(r"Shared library: \[([^]]+)\]",elf):
            if (libs/name).is_file(): queue.append(libs/name)
            elif (vendor/name).is_file():
                shutil.copy2(vendor/name,stage/"vendor/lib64"/name);queue.append(vendor/name)
            else:
                vndk=next((directory/name for directory in (libs/'vndk-28',libs/'vndk-sp-28') if (directory/name).is_file()),None)
                if vndk is None:raise SystemExit("Missing platform dependency "+name)
                shutil.copy2(vndk,stage/'vendor/lib64'/name);queue.append(vndk)
    props=stage/"system/build.prop"
    values={"ro.product.cpu.abilist":"arm64-v8a", "ro.product.cpu.abilist64":"arm64-v8a",
            "ro.product.cpu.abilist32":"", "ro.zygote":"zygote64", "ro.hardware.gralloc":"redroid",
            "ro.hardware.hwcomposer":"redroid", "ro.hardware.egl":"swiftshader",
            "dalvik.vm.usejit":"false", "dalvik.vm.usejitprofiles":"false", "dalvik.vm.dexopt.secondary":"false",
            "ro.zygote.disable_gl_preload":"true", "ro.sf.lcd_density":"320", "ro.boot.redroid_width":"480", "ro.boot.redroid_height":"800", "ro.boot.redroid_fps":"30"}
    merged={}
    for source in (platform/"system/etc/prop.default",props):
        for line in source.read_text().splitlines():
            if line.strip().startswith("#") or "=" not in line: continue
            key,value=line.split("=",1);merged[key.strip()]=value.strip()
    # This initial ARM64 profile runs interpreted code and reuses matching OAT.
    # No 32-bit dex2oat process is silently substituted or reported as working.
    values.update({"pm.dexopt."+reason:"verify" for reason in
                  ("first-boot","boot","install","bg-dexopt","ab-ota","inactive","shared")})
    merged.update(values)
    props.write_text("# Direct Android 9 profile; merged pinned defaults and build properties\n"+
                     "".join(k+"="+v+"\n" for k,v in merged.items()))
    rc=(stage/"system/etc/init/init.zygote.rc").read_text()
    rc=rc.replace("# A real Android 9 zygote with lazy preloading; no system_server/launcher yet.",
                  "# Direct Android 9 system-server profile; full UI boot is under development.")
    rc=rc.replace("--enable-lazy-preload", "--enable-lazy-preload start-system-server")
    rc=rc.replace("service pocket-registry /system/bin/pocket-api28-probe manager", "service pocket-registry /system/bin/pocket-hw-services")
    rc=rc.replace("    class_start pocket-zygote", "    start pocket-data\n\non property:pocket.data.ready=1\n    class_start pocket-zygote")
    rc+="    setenv SYSTEMSERVERCLASSPATH /system/framework/services.jar:/system/framework/ethernet-service.jar:/system/framework/wifi-service.jar:/system/framework/com.android.location.provider.jar\n"
    (stage/"system/etc/init/init.system-server.rc").write_text(rc)
    rc+="\nservice pocket-data /system/bin/pocket-boot-probe prepare-data\n    class pocket-bootstrap\n    user 0\n    group 0\n    oneshot\n"
    rc+="\nservice pocket-storage /system/bin/pocket-storage-services\n    class pocket-zygote\n    user 0\n    group 0\n"
    rc+="\nservice pocket-network /system/bin/pocket-netd-offline\n    class pocket-zygote\n    user 0\n    group 0\n"
    rc+="\nservice pocket-keystore /system/bin/keystore /data/misc/keystore\n    class pocket-zygote\n    user 1017\n    group 1017\n"
    rc+="\nservice pocket-audio-verify /system/bin/pocket-audio-verify\n    class pocket-zygote\n    user 1000\n    group 1000\n    oneshot\n"
    rc+="\nservice pocket-gatekeeper /system/bin/gatekeeperd /data/misc/gatekeeper\n    class pocket-zygote\n    user 1000\n    group 1000\n"
    bootclasspath=(ROOT/'out/api28/bootclasspath.txt').read_text().strip()
    rc+="\non property:sys.boot_completed=1\n    start pocket-framework-probe\n"
    rc+="\nservice pocket-framework-probe /system/bin/pocket-app-process-api28 -Xint -Xnorelocate -Xnoimage-dex2oat -Ximage:/system/framework/boot.art -Xplugin:libopenjdkjvmti.so -agentpath:/system/lib64/libpocket_zygote_api28.so -Xms16m -Xmx96m /system/bin dev.kreijstal.aosppocket.guest.FrameworkProbe /system/etc/pocket-smoke.apk\n    class pocket-bootstrap\n    user 1000\n    group 1000\n    oneshot\n    setenv BOOTCLASSPATH "+bootclasspath+"\n"
    rc+="\non property:pocket.storage.ready=1\n    start pocket-storage-verify\n\nservice pocket-storage-verify /system/bin/pocket-storage-services verify\n    class pocket-bootstrap\n    user 0\n    group 0\n    oneshot\n"
    rc+="\nservice pocket-installd /system/bin/installd\n    class pocket-zygote\n    user 0\n    group 0\n\nservice pocket-surfaceflinger /system/bin/surfaceflinger\n    class pocket-zygote\n    user 1000\n    group 1000\n"
    (stage/"system/etc/init/init.system-server.rc").write_text(rc)
    build_zygote(args.ndk,stage,args.hidl_gen)
    compiler = llvm / "aarch64-linux-android28-clang"
    subprocess.run([compiler, "-std=c11", "-O2", "-g", "-fPIC", "-shared", "-Wall", "-Wextra", "-Werror",
                    "-Wno-misleading-indentation", "-DPOCKET_DIRECT_BOOT", "-I"+str(out),
                    ROOT / "runtime/direct/guest_paths.c", ROOT / "runtime/direct/boot_runtime.c",
                    reference / "system/lib64/libc.so", "-ldl", "-Wl,--no-undefined", "-Wl,-z,max-page-size=16384", "-Wl,-soname,libpocket_direct_boot.so",
                    "-o", stage / "system/lib64/libpocket_direct_boot.so"], check=True)
    subprocess.run([compiler,"-std=c11","-O2","-Wall","-Wextra","-Werror",
                    ROOT/"runtime/direct/boot_probe.c","-ldl","-Wl,-z,max-page-size=16384",
                    "-o",stage/"system/bin/pocket-boot-probe"],check=True)
    subprocess.run([compiler,"-std=c11","-O2","-Wall","-Wextra","-Werror",
                    ROOT/"runtime/direct/boot_egl.c","-L"+str(libs),"-lEGL","-lGLESv2",
                    "-Wl,-z,max-page-size=16384","-o",stage/"system/bin/pocket-boot-egl"],check=True)
    subprocess.run([compiler,"-std=c11","-O2","-Wall","-Wextra","-Werror",
                    ROOT/"runtime/direct/boot_hw_main.c","-ldl","-Wl,-z,max-page-size=16384",
                    "-o",stage/"system/bin/pocket-hw-services"],check=True)
    executables = []
    for path in (stage / "system/bin").iterdir():
        if path.is_file() and path.read_bytes()[:4] == b"\x7fELF" and reserve_interpreter(path):
            executables.append(path.relative_to(stage).as_posix())
    (out / "hooks.json").write_text(json.dumps({"libc_hooks": manifest, "property_state": {
        "offset": prop_address, "size": prop_size}, "executables": executables,
        "core_applications": applications, "runtime_verification_required": True}, indent=2)+'\n')
    (out / "build.stamp").touch()
    print(f"PASS: direct platform shim, {len(manifest)} pinned libc hooks, {len(executables)} interpreter reservations")


if __name__ == "__main__":
    main()
