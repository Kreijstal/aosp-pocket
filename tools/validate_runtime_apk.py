#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import zipfile

def check_host_ui_manifest(data):
    # Verify the encoded attribute, not just the source XML: the shared
    # builder originally lacked this resource ID and silently omitted it.
    offset, resources, values, configs = 8, [], [], []
    while offset < len(data):
        kind, header, size = struct.unpack_from("<HHI", data, offset)
        assert size >= header >= 8 and offset + size <= len(data)
        if kind == 0x0180:  # resource map, indexed by string-pool ID
            resources = list(struct.unpack_from("<" + "I" * ((size-header)//4), data, offset+header))
        elif kind == 0x0102:  # start element
            start, stride, count = struct.unpack_from("<HHH", data, offset+24)
            ids = []
            for index in range(count):
                attribute = offset + 16 + start + stride * index
                name = struct.unpack_from("<I", data, attribute+4)[0]
                ids.append(resources[name] if name < len(resources) else 0)
                if name < len(resources) and resources[name] == 0x010102d3:
                    values.append((data[attribute+15], struct.unpack_from("<I", data, attribute+16)[0]))
                if name < len(resources) and resources[name] == 0x0101001f:
                    configs.append((data[attribute+15], struct.unpack_from("<I", data, attribute+16)[0]))
            assert ids == sorted(ids), "Android requires attributes sorted by resource ID"
        offset += size
    assert values == [(0x12, 0)], "Host controls must explicitly disable HWUI acceleration: " + repr(values)
    assert configs == [(0x10, 7552)] * 2, "Both windows must handle display configuration changes"

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("apk", type=Path)
    parser.add_argument("--ndk", type=Path, required=True)
    args = parser.parse_args()
    readelf = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
    with zipfile.ZipFile(args.apk) as apk, tempfile.TemporaryDirectory() as temporary:
        check_host_ui_manifest(apk.read("AndroidManifest.xml"))
        prefix = 'assets/default-guest/'
        layout = json.loads(apk.read(prefix + 'pocket-layout.json'))
        assert layout['format'] == 1 and layout['abi'] == 'arm64-v8a'
        files = {name for name, spec in layout['entries'].items() if spec['type'] == 'file'}
        stored = set()
        for name in apk.namelist():
            if not name.startswith(prefix): continue
            name = name[len(prefix):]
            if name != 'pocket-layout.json':
                assert name.endswith('.payload'), 'Guest entry must be opaque to Android APK scans'
                name = name[:-len('.payload')]
            stored.add(name)
        assert stored == files | {'pocket-layout.json'}, 'Incomplete bundled guest'
        assert not any(name.endswith('.bc') for name in apk.namelist()), 'Guest RenderScript must not force a 32-bit host ABI'
        assert {'init', 'system/build.prop', 'system/etc/NOTICE.html.gz'} <= files
        for name in ('ueventd', 'watchdogd'):
            assert layout['entries']['sbin/'+name]['target'] == '../init'
        manifest = apk.read('AndroidManifest.xml')
        # Optional host-device grants are now declared for the API28 broker.
        # Calls/SMS must still use the host UI, with no direct-send privileges.
        for permission in ('READ_PRIVILEGED_PHONE_STATE', 'CALL_PHONE', 'SEND_SMS'):
            name = 'android.permission.' + permission
            assert name.encode() not in manifest and name.encode('utf-16le') not in manifest, name
        for name in ("libpocket_runtime.so", "libproot.so"):
            file = Path(temporary) / name
            file.write_bytes(apk.read("lib/arm64-v8a/" + name))
            output = subprocess.check_output([readelf, "-hlWd", file], text=True)
            assert "AArch64" in output, name
            loads = [line for line in output.splitlines() if line.strip().startswith("LOAD ")]
            assert loads and all(int(line.split()[-1], 16) >= 16384 for line in loads), loads
            assert "libc++_shared.so" not in output, output
            assert not re.search(r"Shared library: \[libboost", output), output
            if name == "libproot.so":
                assert "/system/bin/linker64" in output and "DYN" in output
        assert "PROOT_NO_SECCOMP=1" in apk.read("assets/guest.sh").decode()
        file = Path(temporary) / "guest-binder-bootstrap.so"
        file.write_bytes(apk.read("assets/guest-binder-bootstrap.so"))
        guest = subprocess.check_output([readelf, "-hlWds", file], text=True)
        assert "AArch64" in guest and "_ZN7android10HostBinder9getSVMObjEv" in guest
        loads = [line for line in guest.splitlines() if line.strip().startswith("LOAD ")]
        assert loads and all(int(line.split()[-1], 16) >= 16384 for line in loads), loads
        assert "libbinder.so" in guest and "libc++.so" in guest
        assert "__ndk1" not in guest, "Guest must use the reference system C++ namespace"
        assert "killall" not in apk.read("assets/guest.sh").decode()
        for license in ("anbox-and-runtime", "proot", "talloc", "boost"):
            assert len(apk.read("assets/licenses/" + license + ".txt")) > 100
    print("PASS: ARM64 runtime + PRoot, static C++/Boost, 16 KiB LOAD alignment, guest Binder ABI, software host UI, licenses, scoped launch, complete bundled guest")

if __name__ == "__main__":
    main()
