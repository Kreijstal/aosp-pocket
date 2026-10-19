#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Focused packaging/ABI checks for the independent API 28 integration APK."""
import argparse
import io
import json
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk", type=Path)
    parser.add_argument("--ndk", type=Path, required=True)
    args = parser.parse_args()
    readelf = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
    with zipfile.ZipFile(args.apk) as apk, tempfile.TemporaryDirectory() as temp:
        names = apk.namelist()
        assert len(names) == len(set(names)), "Duplicate APK entries"
        assert "dev.kreijstal.aosppocket.api28probe".encode("utf-16le") in apk.read("AndroidManifest.xml")
        lock = json.loads(apk.read("assets/sources.lock.json"))
        assert lock["guest_api28"]["sdk"] == 28
        prefix = "assets/api28-rootfs/"
        assert not any(name.endswith(".bc") for name in names)
        props = apk.read(prefix + "system/build.prop.payload").decode()
        assert "ro.build.version.sdk=28\n" in props
        elf_files = {}
        for name in names:
            if not name.startswith(prefix) or name.endswith("build.prop.payload"):
                continue
            assert name.endswith(".payload"), name
            data = apk.read(name)
            if not data.startswith(b"\x7fELF"):
                guest_name = name[len(prefix):-8]
                if guest_name.endswith(".jar"):
                    assert zipfile.is_zipfile(io.BytesIO(data)), name
                elif guest_name.endswith(".art"):
                    assert data.startswith(b"art\n"), name
                elif guest_name.endswith(".vdex"):
                    assert data.startswith(b"vdex"), name
                elif guest_name.endswith(".art.rel"):
                    assert data, name
                elif guest_name == "system/usr/icu/icudt60l.dat":
                    assert len(data) > 1000000
                elif guest_name == "system/usr/share/zoneinfo/tzdata":
                    assert data.startswith(b"tzdata"), name
                elif guest_name == "data/pocket-art-probe.dex":
                    assert data.startswith(b"dex\n"), name
                elif guest_name.startswith("system/etc/init/") and guest_name.endswith(".rc"):
                    assert data.startswith(b"# SPDX-License-Identifier:"), name
                elif guest_name == "system/etc/event-log-tags":
                    assert b"boot_progress_start" in data, name
                elif guest_name == "system/etc/public.libraries.txt":
                    assert b"libandroid.so" in data, name
                else:
                    raise AssertionError("Unexpected non-ELF guest payload: " + name)
                continue
            file = Path(temp) / Path(name[:-8]).name
            file.write_bytes(data)
            elf = subprocess.check_output([readelf, "-hldWs", file], text=True)
            assert "AArch64" in elf, name
            elf_files[file.name] = elf
        for name, elf in elf_files.items():
            for needed in re.findall(r"Shared library: \[([^]]+)\]", elf):
                assert needed in elf_files, (name, "Missing runtime dependency", needed)
        bridge = elf_files["libpocket_binder_api28.so"]
        assert "_ZN7android12ProcessState16getContextObject" in bridge
        assert "_ZN7android6Parcel19writeInterfaceToken" in bridge
        assert "_ZNK7android6Parcel16enforceInterface" in bridge
        assert "HostBinder" not in bridge and "__ndk1" not in bridge
        assert "pocket-api28-probe" in elf_files
        assert {"pocket-property-service-api28", "pocket-property-probe-api28", "dalvikvm64", "libart.so", "pocket-init-api28", "pocket-init-test-api28"} <= elf_files.keys()
        for name in ("libpocket_binder_api28.so", "pocket-api28-probe"):
            loads = [line for line in elf_files[name].splitlines() if line.strip().startswith("LOAD ")]
            assert loads and all(int(line.split()[-1], 16) >= 16384 for line in loads), loads
        init = elf_files["pocket-init-api28"]
        for symbol in ("_ZN7android4init7Service5StartEv", "_ZN7android4init13ActionManager17ExecuteOneCommandEv",
                       "_ZN7android4init6Parser11ParseConfig"):
            assert symbol in init, ("AOSP init core symbol missing", symbol)
        config = apk.read(prefix + "system/etc/init/init.pocket.rc.payload")
        assert b"socket pocket_init_test" in config and b"on property:" in config
        assert len(apk.read("assets/licenses/android9-NOTICE.xml.gz")) > 1000
    print("PASS: isolated API 28 package, ARM64 Binder/property/ART/init binaries, boot images, complete ELF dependency closure, bridge symbols and license notices")


if __name__ == "__main__":
    main()
