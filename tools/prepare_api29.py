#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Stage the pinned Android 10 platform, including flattened runtime APEX paths.

This is an ABI/boot reference for the Pocket port, not a verified bootable image.
Archive links are resolved to private regular payloads; no kernel mounts are used.
"""
import argparse
import fcntl
import hashlib
import json
from pathlib import Path, PurePosixPath
import posixpath
import re
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
ARCHES = {"arm64": ("abi_reference", 183), "x86_64": ("abi_reference_x86_64", 62)}
MARKER = ".pocket-api29-reference.json"


def prepare_sources(names):
    """Fetch only the pinned source checkouts needed for a selected component."""
    lock=json.loads((ROOT/'sources.lock.json').read_text())['guest_api29']
    items={item['cache']:item for item in lock['headers']}
    if set(names)-items.keys():raise ValueError('Unknown Android 10 source checkout')
    cache=ROOT/'out/api29';cache.mkdir(parents=True,exist_ok=True)
    with (cache/'.prepare-sources.lock').open('a') as guard:
        fcntl.flock(guard.fileno(),fcntl.LOCK_EX)
        for name in names:
            item=items[name];source=cache/name
            if not source.exists():
                subprocess.run(['git','clone','--depth','1','--filter=blob:none','--sparse',
                                '-b',item['tag'],item['url'],str(source)],check=True)
            revision=subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()
            if revision!=item['commit']:raise ValueError('Unexpected Android 10 source revision: '+name)
            if subprocess.check_output(['git','-C',str(source),'status','--porcelain'],text=True):
                raise ValueError('Android 10 source checkout has local changes: '+name)
            subprocess.run(['git','-C',str(source),'sparse-checkout','set',*item['paths']],check=True)


def safe_path(name):
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts:
        raise ValueError("Unsafe Android 10 reference path: " + name)
    return path.as_posix()


def link_target(name, target, hard=False):
    value = target.lstrip("/") if target.startswith("/") or hard else posixpath.join(posixpath.dirname(name), target)
    return safe_path(posixpath.normpath(value))


def stage_reference(archive, destination, digest, arch="arm64"):
    archive, destination = Path(archive), Path(destination)
    with archive.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != digest:
            raise ValueError("Android 10 reference differs from its pinned OCI digest")
    if destination.exists() and any(destination.iterdir()) and not (destination / MARKER).is_file():
        raise ValueError("Refusing to replace an unrelated reference directory")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="api29-stage-", dir=destination.parent) as temporary:
        root = Path(temporary) / "root"
        root.mkdir()
        links = []; files = 0; skipped = 0
        with tarfile.open(archive) as layer:
            for entry in layer:
                name = safe_path(entry.name)
                selected = name.startswith(("system/", "vendor/", "product/", "system_ext/", "apex/")) or name in ("init.environ.rc", "default.prop")
                if not selected: continue
                if entry.issym() or entry.islnk():
                    links.append((name, link_target(name, entry.linkname, entry.islnk())))
                    continue
                if not entry.isfile(): continue
                if PurePosixPath(name).name.startswith(".wh."):
                    raise ValueError("Layer whiteouts require an OCI layer merge")
                with layer.extractfile(entry) as source:
                    first = source.read(64)
                    if first.startswith(b"\x7fELF") and (len(first) < 20 or first[4] != 2 or int.from_bytes(first[18:20], "little") != ARCHES[arch][1]):
                        skipped += 1; continue
                    target = root / name; target.parent.mkdir(parents=True, exist_ok=True)
                    with target.open("wb") as output:
                        output.write(first); shutil.copyfileobj(source, output)
                    target.chmod((entry.mode & 0o777) | 0o400)
                    files += 1
        modules = {}
        # Android 10's init normally bind-mounts these directories. Pocket
        # provides the same guest paths in its own filesystem instead.
        for manifest in sorted((root / "system/apex").glob("*/apex_manifest.json")):
            info = json.loads(manifest.read_text())
            name = info["name"]
            if not re.fullmatch(r"[A-Za-z0-9_.]+", name) or name in modules:
                raise ValueError("Invalid or ambiguous flattened APEX module")
            source = manifest.parent; target = root / "apex" / name
            if target.exists(): raise ValueError("APEX alias already exists")
            target.parent.mkdir(exist_ok=True)
            shutil.copytree(source, target)
            relative = source.relative_to(root).as_posix()
            for link, value in list(links):
                if link.startswith(relative + "/"):
                    links.append(("apex/" + name + link[len(relative):], value))
            modules[name] = {"source": relative, "version": info.get("version")}
        unresolved = links
        for _ in range(len(links) + 1):
            next_links = []; progress = False
            for name, value in unresolved:
                source, target = root / value, root / name
                if source.is_file():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, target); progress = True
                elif source.is_dir() and not target.exists():
                    # Directory aliases are copied only when they cannot
                    # recursively include the destination itself.
                    if source == target or source.is_relative_to(target):
                        raise ValueError("Recursive reference directory alias")
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copytree(source, target); progress = True
                else: next_links.append((name, value))
            unresolved = next_links
            if not progress: break
        props = (root / "system/build.prop").read_text()
        if not re.search(r"^ro\.build\.version\.sdk=29$", props, re.MULTILINE):
            raise ValueError("Reference image is not Android 10/API 29")
        environment = (root / "init.environ.rc").read_text()
        match = re.search(r"export BOOTCLASSPATH (\S+)", environment)
        if not match: raise ValueError("Missing Android 10 boot classpath")
        required = ["system/bin/sh", "system/bin/linker64", "system/bin/dalvikvm64", "system/bin/app_process64",
                    "apex/com.android.runtime/lib64/bionic/libc.so", "apex/com.android.runtime/bin/linker64"]
        required += [name.lstrip("/") for name in match[1].split(":")]
        for name in required:
            if not (root / name).is_file(): raise ValueError("Missing Android 10 runtime payload: " + name)
        report = {"sdk": 29, "android": "10", "architecture": arch, "layer_sha256": digest,
                  "regular_files": files, "skipped_other_architecture_elf": skipped,
                  "apex_modules": modules, "unresolved_nonruntime_links": unresolved,
                  "bootclasspath": match[1], "full_guest_boot_verified": False}
        (root / MARKER).write_text(json.dumps(report, indent=2) + "\n")
        if destination.exists(): shutil.rmtree(destination)
        root.rename(destination)
    return destination


def prepare(arch="arm64"):
    cache=ROOT/'out/api29';cache.mkdir(parents=True,exist_ok=True)
    with (cache/'.prepare-image.lock').open('a') as guard:
        fcntl.flock(guard.fileno(),fcntl.LOCK_EX)
        return _prepare(arch)


def _prepare(arch="arm64"):
    lock = json.loads((ROOT / "sources.lock.json").read_text())["guest_api29"]
    image = lock[ARCHES[arch][0]]; digest = image["layer_digest"].removeprefix("sha256:")
    cache = ROOT / "out/api29"; root = cache / ("reference" if arch == "arm64" else "reference-" + arch)
    marker = root / MARKER
    if marker.is_file():
        value = json.loads(marker.read_text())
        if value.get("layer_sha256") == digest and value.get("architecture") == arch:
            return root
    archive = cache / "downloads" / (digest + ".tar.gz"); archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        token = json.load(urllib.request.urlopen("https://auth.docker.io/token?service=registry.docker.io&scope=repository:redroid/redroid:pull", timeout=60))["token"]
        request = urllib.request.Request("https://registry-1.docker.io/v2/redroid/redroid/blobs/sha256:" + digest, headers={"Authorization": "Bearer " + token})
        temporary = archive.with_suffix(".part")
        with urllib.request.urlopen(request, timeout=60) as source, temporary.open("wb") as output:
            shutil.copyfileobj(source, output)
        with temporary.open("rb") as stream:
            if hashlib.file_digest(stream, "sha256").hexdigest() != digest:
                raise ValueError("Downloaded Android 10 layer failed verification")
        temporary.rename(archive)
    return stage_reference(archive, root, digest, arch)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=sorted(ARCHES), default="arm64")
    args = parser.parse_args(); print(prepare(args.arch))
