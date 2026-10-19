#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Fetch a pinned ARM64 Android 9 ABI reference and sparse AOSP headers.

Does not start Docker, execute the image, or touch the working API 25 guest.
The reference supplies bionic/platform/ART libraries for isolated bring-up.
"""
import argparse
import fcntl
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def prepare(art=False):
    cache = ROOT / "out/api28"
    cache.mkdir(parents=True, exist_ok=True)
    # Multiple independent backends share this immutable source/reference cache.
    # Serialize Git sparse-checkout updates and archive publication.
    with (cache / ".prepare.lock").open("a") as guard:
        fcntl.flock(guard.fileno(), fcntl.LOCK_EX)
        return _prepare(art)


def _prepare(art=False):
    lock = json.loads((ROOT / "sources.lock.json").read_text())["guest_api28"]
    cache = ROOT / "out/api28"
    for item in lock["headers"]:
        source = cache / item["cache"]
        if not source.exists():
            subprocess.run(["git", "clone", "--depth", "1", "--filter=blob:none", "--sparse",
                            "-b", item["tag"], item["url"], source], check=True)
        revision = subprocess.check_output(["git", "-C", source, "rev-parse", "HEAD"], text=True).strip()
        if revision != item["commit"]:
            raise SystemExit(f"Unexpected API 28 source revision: {source}: {revision}")
        subprocess.run(["git", "-C", source, "sparse-checkout", "set", *item["paths"]], check=True)
        if subprocess.check_output(["git", "-C", source, "status", "--porcelain"], text=True):
            raise SystemExit(f"API 28 reference headers have local changes: {source}")
    image = lock["abi_reference"]
    digest = image["layer_digest"].removeprefix("sha256:")
    archive = cache / "downloads" / (digest + ".tar.gz")
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        token = json.load(urllib.request.urlopen(
            "https://auth.docker.io/token?service=registry.docker.io&scope=repository:redroid/redroid:pull",
            timeout=60))["token"]
        request = urllib.request.Request("https://registry-1.docker.io/v2/redroid/redroid/blobs/sha256:" + digest,
                                         headers={"Authorization": "Bearer " + token})
        temporary = archive.with_suffix(".part")
        with urllib.request.urlopen(request, timeout=60) as src, temporary.open("wb") as dst:
            shutil.copyfileobj(src, dst)
        temporary.rename(archive)
    root = cache / "reference"
    marker = root / ".pocket-reference"
    art_marker = root / ".pocket-art-reference"
    if marker.exists() and marker.read_text().strip() == digest and (root / "system/etc/NOTICE.xml.gz").is_file() and (
            not art or (art_marker.exists() and art_marker.read_text().strip() == digest and
                        (root / "system/etc/event-log-tags").is_file() and
                        (root / "system/etc/public.libraries.txt").is_file() and
                        (root / "system/bin/app_process64").is_file() and
                        (root / "system/etc/preloaded-classes").is_file())):
        return root
    with archive.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != digest:
            raise SystemExit("API 28 reference differs from its pinned OCI digest")
    # Select regular files only, without extracting archive links/devices.
    # This is deliberately not an importable full guest root filesystem.
    links = []
    with tarfile.open(archive) as layer:
        for entry in layer:
            path = PurePosixPath(entry.name)
            if path.is_absolute() or ".." in path.parts:
                raise SystemExit("Unsafe OCI reference path")
            name = path.as_posix()
            selected = name.startswith("system/lib64/") or name in (
                "system/bin/linker64", "system/build.prop", "system/etc/NOTICE.xml.gz")
            if art:
                selected |= name in ("init.environ.rc", "system/bin/dalvikvm64", "system/bin/app_process64", "system/etc/event-log-tags",
                                      "system/etc/public.libraries.txt", "system/etc/preloaded-classes") or (
                    path.parent.as_posix() == "system/framework" and (path.suffix == ".jar" or path.name.startswith("boot"))) or (
                    name.startswith("system/framework/arm64/boot")) or name.startswith("system/usr/icu/") or (
                    name == "system/usr/share/zoneinfo/tzdata")
                if selected and entry.issym():
                    # Flatten only the known ARM64 boot-image VDEX links into
                    # regular payloads. No archive symlink is created on host.
                    target = PurePosixPath(entry.linkname.removeprefix("/"))
                    if not (name.startswith("system/framework/arm64/boot") and name.endswith(".vdex") and
                            target.parent.as_posix() == "system/framework" and target.name.startswith("boot") and
                            target.suffix == ".vdex" and ".." not in target.parts):
                        raise SystemExit(f"Unexpected ART reference link: {name}: {entry.linkname}")
                    links.append((name, target.as_posix()))
            if selected and entry.isfile():
                destination = root / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                with layer.extractfile(entry) as src, destination.open("wb") as dst:
                    shutil.copyfileobj(src, dst)
                destination.chmod(entry.mode & 0o777)
    properties = (root / "system/build.prop").read_text()
    if "ro.build.version.sdk=28\n" not in properties:
        raise SystemExit("Reference image is not API 28")
    marker.write_text(digest + "\n")
    if art:
        for name, target in links:
            shutil.copy2(root / target, root / name)
        art_marker.write_text(digest + "\n")
    return root


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--art", action="store_true", help="also stage the Android 9 ART/boot-classpath reference")
    print("API 28 ABI reference:", prepare(art=parser.parse_args().art))
