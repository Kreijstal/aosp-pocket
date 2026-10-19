#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]
out=ROOT / "out/boot"
with zipfile.ZipFile(out / "base.apk") as base, zipfile.ZipFile(out / "packed.apk","w",zipfile.ZIP_DEFLATED,compresslevel=1) as apk:
    for item in base.infolist(): apk.writestr(item,base.read(item))
    apk.write(ROOT / "out/runtime-native/libpocket_runtime.so","lib/arm64-v8a/libpocket_runtime.so")
    for path in sorted((out / "rootfs").rglob("*")):
        if path.is_file(): apk.write(path,"assets/api28-rootfs/"+path.relative_to(out / "rootfs").as_posix()+".payload")
    apk.write(ROOT / "out/api28/art-probe.dex","assets/api28-rootfs/data/pocket-art-probe.dex.payload")
    apk.write(out / "pocket-zygote.jar","assets/api28-rootfs/system/framework/pocket-zygote.jar.payload")
    apk.write(ROOT / "out/api28/bootclasspath.txt","assets/bootclasspath.txt")
    apk.write(out / "hooks.json","assets/direct-boot-hooks.json")
    apk.write(ROOT / "sources.lock.json","assets/sources.lock.json")
    for source,name in (("runtime/NOTICE","NOTICE"),("runtime/licenses/GPL-3.0-only.txt","runtime.txt"),
                         ("runtime/licenses/talloc.txt","talloc.txt"),("LICENSE","apache-2.0.txt"),
                         ("out/ananbox-native/boost_1_83_0/LICENSE_1_0.txt","boost.txt"),
                         ("out/api28/reference/system/etc/NOTICE.xml.gz","android9-NOTICE.xml.gz")):
        apk.write(ROOT / source,"assets/licenses/"+name)
print("Packed direct platform probe",(out / "packed.apk").stat().st_size,"bytes")
