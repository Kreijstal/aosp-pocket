#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import zipfile
from build_boot import headers, TARGETS

parser=argparse.ArgumentParser()
parser.add_argument("apk",type=Path);parser.add_argument("--ndk",type=Path,required=True)
args=parser.parse_args()
llvm=args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin"
with zipfile.ZipFile(args.apk) as apk,tempfile.TemporaryDirectory() as tmp:
    assert len(apk.namelist())==len(set(apk.namelist()))
    assert not any("libproot" in n for n in apk.namelist())
    assert "dev.kreijstal.aosppocket.bootprobe".encode("utf-16le") in apk.read("AndroidManifest.xml")
    manifest=json.loads(apk.read("assets/direct-boot-hooks.json"))
    assert set(manifest["libc_hooks"])==set(TARGETS)
    assert manifest["property_state"]["size"]==0x450 and manifest["runtime_verification_required"] is True
    for name in manifest["executables"]:
        image=apk.read("assets/api28-rootfs/"+name+".payload")
        entry=next(p for _,p in headers(image) if p[0]==3)
        assert image[entry[2]:entry[2]+entry[5]]==b"/pocket/"+b"x"*504+b"\0"
    binary=Path(tmp)/"boot.so"
    binary.write_bytes(apk.read("assets/api28-rootfs/system/lib64/libpocket_direct_boot.so.payload"))
    elf=subprocess.check_output([llvm/"llvm-readelf","-hldW",binary],text=True)
    assert "AArch64" in elf and "libc++.so" not in elf
print("PASS: isolated direct platform APK, no PRoot, pinned hook manifest and executable interpreter slots")
