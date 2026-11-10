#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Prepare the pinned AOSP 12L runtime without mounts or host root.

The SDK image contains GPT, dynamic partitions and compressed APEX packages.
Only immutable platform files enter the reference; emulator userdata is excluded.
This preparation step does not establish that Pocket can boot the framework.
"""
import argparse
import base64
import fcntl
import hashlib
import json
import os
from pathlib import Path
import posixpath
import re
import shutil
import stat
import struct
import subprocess
import tempfile
import urllib.request
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / 'out/api32'
MARKER = '.pocket-api32-reference.json'


def specification():
    return json.loads((ROOT / 'sources.lock.json').read_text())['guest_api32']


def verified_download(url, target, digest, size=None):
    target = Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists():
        temporary = target.with_suffix(target.suffix + '.partial')
        try:
            with urllib.request.urlopen(url, timeout=60) as source, temporary.open('wb') as output:
                shutil.copyfileobj(source, output)
            verify_file(temporary, digest, size)
            temporary.replace(target)
        finally:
            temporary.unlink(missing_ok=True)
    verify_file(target, digest, size)
    return target


def verify_file(path, digest, size=None):
    if size is not None and path.stat().st_size != size:
        raise ValueError('Reference size differs from source lock: ' + str(path))
    with path.open('rb') as stream:
        if hashlib.file_digest(stream, 'sha256').hexdigest() != digest:
            raise ValueError('Reference SHA-256 differs from source lock: ' + str(path))


def prepare_host_compiler(name):
    """Use the version-matched AOSP host compiler and its pinned C++ library."""
    spec = specification()[name + '_tool']
    root = CACHE / 'tools'
    for entry in spec['files']:
        target = root / entry['destination']
        if not target.exists():
            target.parent.mkdir(parents=True, exist_ok=True)
            url = spec['source'] + '/+/' + spec['commit'] + '/' + entry['path'] + '?format=TEXT'
            with urllib.request.urlopen(url, timeout=60) as response:
                data = base64.b64decode(response.read(), validate=True)
            temporary = target.with_suffix('.partial')
            try:
                temporary.write_bytes(data)
                verify_file(temporary, entry['sha256'], entry['bytes'])
                temporary.replace(target)
            finally:
                temporary.unlink(missing_ok=True)
        verify_file(target, entry['sha256'], entry['bytes'])
    compiler = root / 'bin' / ('hidl-gen' if name == 'hidl' else name)
    compiler.chmod(0o755)
    return compiler


def prepare_aidl():
    return prepare_host_compiler('aidl')


def prepare_hidl():
    return prepare_host_compiler('hidl')


def gpt_partition(image, name):
    """Return a bounds- and checksum-checked byte range in a GPT disk image."""
    size = Path(image).stat().st_size
    with Path(image).open('rb') as stream:
        stream.seek(512)
        header = bytearray(stream.read(512))
        if len(header) != 512 or header[:8] != b'EFI PART':
            raise ValueError('SDK partition image has no GPT header')
        length, checksum = struct.unpack_from('<II', header, 12)
        if not 92 <= length <= 512:
            raise ValueError('Invalid GPT header length')
        struct.pack_into('<I', header, 16, 0)
        if zlib.crc32(header[:length]) != checksum:
            raise ValueError('Invalid GPT header checksum')
        table, count, stride, checksum = struct.unpack_from('<QIII', header, 72)
        if not 0 < count <= 4096 or not 128 <= stride <= 1024 or stride % 8 or table * 512 + count * stride > size:
            raise ValueError('Invalid GPT partition table bounds')
        stream.seek(table * 512)
        entries = stream.read(count * stride)
        if zlib.crc32(entries) != checksum:
            raise ValueError('Invalid GPT partition table checksum')
        matches = []
        for index in range(count):
            entry = entries[index * stride:(index + 1) * stride]
            if entry[:16] == bytes(16):
                continue
            label = entry[56:128].decode('utf-16le').rstrip('\0')
            first, last = struct.unpack_from('<QQ', entry, 32)
            if first < 34 or last < first or (last + 1) * 512 > size:
                raise ValueError('GPT partition extends outside SDK image')
            if label == name:
                matches.append((first * 512, (last - first + 1) * 512))
        if len(matches) != 1:
            raise ValueError('Expected one GPT partition named ' + name)
        return matches[0]


def copy_partition(image, name, destination):
    offset, remaining = gpt_partition(image, name)
    with Path(image).open('rb') as source, Path(destination).open('wb') as output:
        source.seek(offset)
        while remaining:
            data = source.read(min(remaining, 1024 * 1024))
            if not data:
                raise ValueError('Truncated GPT partition')
            output.write(data)
            remaining -= len(data)


def apex_manifest(data):
    """Read the AOSP 12L ApexManifest protobuf, retaining exported-library lists."""
    cursor = 0

    def varint():
        nonlocal cursor
        value = 0
        for shift in range(0, 70, 7):
            if cursor == len(data):
                raise ValueError('Truncated APEX manifest')
            byte = data[cursor]
            cursor += 1
            if shift == 63 and byte > 1:
                raise ValueError('Oversized APEX manifest integer')
            value |= (byte & 127) << shift
            if not byte & 128:
                return value
        raise ValueError('Oversized APEX manifest integer')

    fields = {}
    while cursor < len(data):
        tag = varint()
        number, wire = tag >> 3, tag & 7
        if not number:
            raise ValueError('Invalid APEX manifest tag')
        if wire == 0:
            value = varint()
        elif wire in (1, 2, 5):
            length = varint() if wire == 2 else 8 if wire == 1 else 4
            if length > len(data) - cursor:
                raise ValueError('Truncated APEX manifest field')
            value = data[cursor:cursor + length]
            cursor += length
        else:
            raise ValueError('Unsupported APEX manifest wire type')
        fields.setdefault(number, []).append(value)
    def text(number, default=b''):
        value = fields.get(number, [default])[-1]
        if not isinstance(value, bytes):
            raise ValueError('Invalid APEX manifest string')
        return value.decode('utf-8')
    name, version = text(1), fields.get(2, [0])[-1]
    if not re.fullmatch(r'[A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)+', name) or not isinstance(version, int) or not 0 < version < 2**63:
        raise ValueError('Invalid APEX module name or version')
    result = {'name': name, 'version': version, 'versionName': text(5)}
    for field, key in [(7, 'provideNativeLibs'), (8, 'requireNativeLibs'), (9, 'jniLibs'), (10, 'requireSharedApexLibs')]:
        result[key] = [value.decode('utf-8') for value in fields.get(field, [])]
    return result


def dump_ext4(image, destination):
    destination = Path(destination).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    # debugfs opens images read-only; it never mounts them or executes their code.
    command = 'rdump / "' + str(destination).replace('\\', '\\\\').replace('"', '\\"') + '"'
    result = subprocess.run(['debugfs', '-R', command, str(image)], capture_output=True, text=True, check=True)
    if not any(destination.iterdir()):
        raise ValueError('Could not extract ext4 payload: ' + result.stderr[-1000:])


def flatten_apex(package, destination, work):
    package, destination, work = Path(package), Path(destination), Path(work)
    with zipfile.ZipFile(package) as archive:
        if package.suffix == '.capex':
            original = work / 'original.apex'
            with archive.open('original_apex') as source, original.open('wb') as output:
                shutil.copyfileobj(source, output)
            package = original
    with zipfile.ZipFile(package) as archive:
        manifest = apex_manifest(archive.read('apex_manifest.pb'))
        payload = work / 'payload.img'
        if archive.getinfo('apex_payload.img').file_size > 2 * 1024**3:
            raise ValueError('APEX payload too large')
        with archive.open('apex_payload.img') as source, payload.open('wb') as output:
            shutil.copyfileobj(source, output)
    target = destination / manifest['name']
    if target.exists():
        raise ValueError('Duplicate APEX module: ' + manifest['name'])
    dump_ext4(payload, target)
    if apex_manifest((target / 'apex_manifest.pb').read_bytes()) != manifest:
        raise ValueError('APEX package and payload manifests differ')
    return manifest


def virtual_path(root, name):
    """Resolve guest absolute and relative symlinks without following host paths."""
    def normalize(value):
        value = posixpath.normpath(value)
        if value.startswith('/') or value == '..' or value.startswith('../'):
            raise ValueError('Guest link escapes reference image: ' + name)
        return value
    name = normalize(name)
    for _ in range(64):
        parts = name.split('/')
        for index in range(len(parts)):
            prefix = '/'.join(parts[:index + 1])
            path = root / prefix
            if path.is_symlink():
                link = os.readlink(path)
                target = link.lstrip('/') if link.startswith('/') else posixpath.join(posixpath.dirname(prefix), link)
                name = normalize(posixpath.join(target, *parts[index + 1:]))
                break
        else:
            return root / name
    raise ValueError('Cyclic guest reference link: ' + name)


def materialize_links(root):
    for _ in range(64):
        links = [path for path in root.rglob('*') if path.is_symlink()]
        if not links:
            return
        progress = False
        for path in links:
            source = virtual_path(root, path.relative_to(root).as_posix())
            if not source.exists():
                # Keep an explicit inventory of absent runtime links rather than
                # letting an absolute guest target accidentally resolve on the host.
                continue
            if source.is_dir() and (path == source or path.is_relative_to(source)):
                raise ValueError('Recursive guest directory alias')
            path.parent.chmod(path.parent.stat().st_mode | stat.S_IWUSR)
            path.unlink()
            if source.is_dir():
                shutil.copytree(source, path, symlinks=True)
            else:
                shutil.copy2(source, path)
            progress = True
        if not progress:
            break
    missing = [p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_symlink()]
    if missing:
        raise ValueError('Unresolved guest runtime links: ' + ', '.join(missing[:20]))


def prepare_sources(names):
    items = {item['cache']: item for item in specification()['headers']}
    if set(names) - items.keys():
        raise ValueError('Unknown Android 12L source checkout')
    CACHE.mkdir(parents=True, exist_ok=True)
    with (CACHE / '.prepare-sources.lock').open('a') as guard:
        fcntl.flock(guard.fileno(), fcntl.LOCK_EX)
        for name in names:
            item, source = items[name], CACHE / name
            if not source.exists():
                subprocess.run(['git', 'clone', '--depth', '1', '--filter=blob:none', '--sparse', '-b', item['tag'], item['url'], str(source)], check=True)
            revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
            if revision != item['commit'] or subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain'], text=True):
                raise ValueError('Unexpected or modified Android 12L source: ' + name)
            subprocess.run(['git', '-C', str(source), 'sparse-checkout', 'set', '--no-cone', *item['paths']], check=True)


def prepare_system_sdk():
    """Pinned compile-only 12L system APIs; these stubs never enter the image."""
    spec=specification()['system_api_stubs'];target=CACHE/'tools/android-system32.jar'
    if target.exists():
        verify_file(target,spec['sha256'],spec['bytes']);return target
    assert spec['format']=='gitiles-base64'
    target.parent.mkdir(parents=True,exist_ok=True);temporary=target.with_suffix('.partial')
    try:
        bound=((spec['bytes']+2)//3)*4+1024
        with urllib.request.urlopen(spec['url'],timeout=60) as source:
            encoded=source.read(bound+1)
        if len(encoded)>bound:raise ValueError('System SDK download exceeds pinned size')
        temporary.write_bytes(base64.b64decode(encoded,validate=True))
        verify_file(temporary,spec['sha256'],spec['bytes']);temporary.replace(target)
    finally:temporary.unlink(missing_ok=True)
    return target


def lpunpack():
    spec = specification()['partition_tools']['lpunpack']
    binary = CACHE / 'tools/lpunpack'
    if not binary.exists():
        archive = verified_download(spec['url'], CACHE / 'downloads/android-tools.pkg.tar.zst', spec['archive_sha256'])
        binary.parent.mkdir(parents=True, exist_ok=True)
        with binary.open('wb') as output:
            subprocess.run(['bsdtar', '-xOf', str(archive), spec['member']], stdout=output, check=True)
    verify_file(binary, spec['binary_sha256'])
    binary.chmod(0o755)
    return binary


def prepare():
    CACHE.mkdir(parents=True, exist_ok=True)
    with (CACHE / '.prepare-image.lock').open('a') as guard:
        fcntl.flock(guard.fileno(), fcntl.LOCK_EX)
        spec = specification()['abi_reference']
        destination = CACHE / 'reference'
        if (destination / MARKER).is_file():
            marker = json.loads((destination / MARKER).read_text())
            if marker.get('archiveSha256') == spec['sha256'] and marker.get('sdk') == 32:
                return destination
        if destination.exists() and any(destination.iterdir()):
            raise ValueError('Refusing to overwrite an unrelated API 32 reference')
        archive = verified_download(spec['url'], CACHE / 'downloads/arm64-v8a-32_r02.zip', spec['sha256'], spec['bytes'])
        with tempfile.TemporaryDirectory(prefix='api32-stage-', dir=CACHE) as temporary:
            temporary = Path(temporary)
            sdk, partitions, root = temporary / 'sdk', temporary / 'partitions', temporary / 'root'
            for directory in [sdk, partitions, root]:
                directory.mkdir()
            with zipfile.ZipFile(archive) as package:
                for name in ['system.img', 'NOTICE.txt']:
                    with package.open('arm64-v8a/' + name) as source, (sdk / name).open('wb') as output:
                        shutil.copyfileobj(source, output)
            copy_partition(sdk / 'system.img', 'super', sdk / 'super.img')
            subprocess.run([str(lpunpack()), str(sdk / 'super.img'), str(partitions)], check=True)
            dump_ext4(partitions / 'system.img', root)
            # The system partition includes empty mount-point directories.
            for name in ['product', 'system_ext', 'vendor']:
                dump_ext4(partitions / (name + '.img'), root / name)
            props = (root / 'system/build.prop').read_text()
            if not re.search(r'^ro.build.version.sdk=32$', props, re.MULTILINE):
                raise ValueError('Reference is not Android 12L/API 32')
            modules = []
            for path in sorted((root / 'system/apex').iterdir()):
                if path.suffix not in ('.apex', '.capex'):
                    continue
                with tempfile.TemporaryDirectory(prefix='apex-', dir=temporary) as work:
                    modules.append(flatten_apex(path, root / 'apex', Path(work)))
            # Preserve only immutable platform roots and their real init exports.
            for path in list(root.iterdir()):
                if path.name not in ['system', 'product', 'system_ext', 'vendor', 'apex', 'init.environ.rc']:
                    if path.is_dir() and not path.is_symlink():
                        path.chmod(path.stat().st_mode | stat.S_IWUSR)
                        shutil.rmtree(path)
                    else:
                        path.unlink()
            materialize_links(root)
            for required in ['system/bin/sh', 'apex/com.android.runtime/bin/linker64', 'apex/com.android.art/bin/dalvikvm64', 'apex/com.android.art/javalib/core-oj.jar']:
                if not (root / required).is_file():
                    raise ValueError('Missing Android 12L runtime payload: ' + required)
            shutil.copy2(sdk / 'NOTICE.txt', root / 'NOTICE.txt')
            report = {'sdk': 32, 'android': '12L', 'architecture': 'arm64', 'archiveSha256': spec['sha256'],
                      'buildId': spec['build_id'], 'apexModules': modules, 'fullGuestBootVerified': False}
            (root / MARKER).write_text(json.dumps(report, indent=2) + '\n')
            if destination.exists():
                destination.rmdir()
            root.rename(destination)
        return destination


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sources', nargs='+', help='Fetch selected pinned platform source checkouts')
    args = parser.parse_args()
    if args.sources:
        prepare_sources(args.sources)
    else:
        print(prepare())
