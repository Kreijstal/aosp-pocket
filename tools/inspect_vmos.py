#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Static inventory of an acquired VMOS APK. Never execute or install the target.

Run using the isolated research environment with androguard installed. Outputs
are private research artifacts under out/, not inputs to the application build.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import zipfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("apk", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    from loguru import logger
    logger.remove()
    from androguard.core.apk import APK
    from androguard.core.dex import DEX

    app = APK(str(args.apk))
    manifest = app.get_android_manifest_xml()
    application = manifest.find("application")
    with args.apk.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    info = {
        "apk_file": args.apk.name,
        "bytes": args.apk.stat().st_size,
        "sha256": digest,
        "package": app.get_package(),
        "version_name": app.get_androidversion_name(),
        "version_code": app.get_androidversion_code(),
        "min_sdk": app.get_min_sdk_version(),
        "target_sdk": app.get_target_sdk_version(),
        "application": dict(application.attrib),
        "main_activity": app.get_main_activity(),
        "permissions": sorted(app.get_permissions()),
        "services": app.get_services(),
        "providers": app.get_providers(),
        "receivers": app.get_receivers(),
        "native_methods": [],
        "dex_files": [],
        "entries": [],
    }
    from lxml import etree
    (args.output / "AndroidManifest.xml").write_bytes(etree.tostring(
        manifest, encoding="utf-8", pretty_print=True))
    with zipfile.ZipFile(args.apk) as archive:
        for item in archive.infolist():
            info["entries"].append({"path": item.filename, "bytes": item.file_size,
                                    "compressed_bytes": item.compress_size})
            if (item.filename.startswith("lib/") and item.filename.endswith(".so")) or item.filename in (
                    "assets/rootfs_info", "assets/libprelinker32.so", "assets/libprelinker64.so"):
                target = args.output / "extracted" / item.filename
                if not target.resolve().is_relative_to((args.output / "extracted").resolve()):
                    raise ValueError("Unsafe ZIP path")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(archive.read(item))
        if "assets/rootfs_info" in archive.namelist():
            info["guest_metadata"] = json.loads(archive.read("assets/rootfs_info"))
        for name in archive.namelist():
            if not re.fullmatch(r"classes\d*\.dex", name):
                continue
            print(f"Reading {name}", flush=True)
            dex = DEX(archive.read(name))
            counts = {"name": name, "classes": 0, "methods": 0}
            for cls in dex.get_classes():
                counts["classes"] += 1
                for method in cls.get_methods():
                    counts["methods"] += 1
                    if method.get_access_flags() & 0x100:
                        info["native_methods"].append({"dex": name, "class": cls.get_name(),
                            "method": method.get_name(), "descriptor": method.get_descriptor()})
                    if (cls.get_name() == "Lcom/vmos/virtual/viewmodel/VmActivationViewModel;"
                            and method.get_name() == "u"):
                        instructions = [i.get_name() + " " + i.get_output()
                                        for i in method.get_instructions()]
                        (args.output / "activation-dex.txt").write_text(
                            name + " " + method.get_descriptor() + "\n"
                            + "\n".join(instructions) + "\n")
            info["dex_files"].append(counts)
    (args.output / "inventory.json").write_text(json.dumps(info, indent=2) + "\n")
    print(json.dumps({k: info[k] for k in ("package", "version_name", "version_code",
          "min_sdk", "target_sdk", "main_activity", "dex_files")}, indent=2))
    print(f"Native declarations: {len(info['native_methods'])}")
    print("Largest entries:")
    for item in sorted(info["entries"], key=lambda x: x["bytes"], reverse=True)[:20]:
        print(f"{item['bytes']:12d} {item['path']}")


if __name__ == "__main__":
    main()
