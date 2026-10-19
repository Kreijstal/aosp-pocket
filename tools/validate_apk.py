#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check that the APK contains actual ARM64 PIEs and the recorded source/license."""
import json
import struct
import sys
import zipfile


def main():
    with zipfile.ZipFile(sys.argv[1]) as apk:
        for name in ("libtoybox_exec.so", "libpocket_probe.so"):
            data = apk.read("lib/arm64-v8a/" + name)
            assert data[:6] == b"\x7fELF\x02\x01", name
            elf_type, machine = struct.unpack_from("<HH", data, 16)
            assert elf_type == 3 and machine == 183, (name, elf_type, machine)
            phoff = struct.unpack_from("<Q", data, 32)[0]
            phsize, phnum = struct.unpack_from("<HH", data, 54)
            headers = [struct.unpack_from("<IIQQQQQQ", data, phoff + i * phsize)
                       for i in range(phnum)]
            # ET_DYN alone could be a JNI shared library: require a real ELF interpreter.
            assert any(h[0] == 3 and b"/system/bin/linker64" in data[h[2]:h[2]+h[5]]
                       for h in headers), name
            assert all(h[7] >= 16384 for h in headers if h[0] == 1), name
            print(f"PASS {name}: ARM64 PIE, Android linker, 16 KiB LOAD alignment")
        lock = json.loads(apk.read("assets/sources.lock.json"))
        assert lock["toybox"]["commit"] == "e21e76d30fa15996188b79affbc25cfd0c13deff"
        assert "Permission to use, copy, modify" in apk.read("assets/licenses/toybox.txt").decode()
        manifest = apk.read("AndroidManifest.xml")
        # Binary XML string pools can encode either UTF-8 or UTF-16. This small
        # permission-free manifest needs neither uses-permission element form.
        assert all("uses-permission".encode(enc) not in manifest
                   for enc in ("utf-8", "utf-16le")), "Unexpected requested permission"
        print("PASS source provenance, Toybox license, no requested permissions")


if __name__ == "__main__":
    main()
