#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Package the paired DNS and fwmark caller protocol without rebuilding the base Android image."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import urllib.parse
import zipfile
import zlib
from build_boot import reserve_interpreter


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('base-image', 'base-catalog', 'loader', 'core', 'dns', 'netd', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--url', required=True)
    args = parser.parse_args()
    url = urllib.parse.urlsplit(args.url)
    if (url.scheme != 'https' or not url.hostname or url.username or url.password or url.fragment
            or not re.fullmatch(r'[0-9]+(?:\.[0-9]+)*', args.version)):
        parser.error('A numeric version and public HTTPS image URL are required')
    catalog = json.loads(args.base_catalog.read_text())
    images = [image for image in catalog['images'] if image['sdk'] == 32]
    if catalog['schema'] != 1 or len(images) != 1:
        parser.error('Expected one pinned API32 image')
    base = images[0]
    if (args.base_image.stat().st_size != base['bytes'] or digest(args.base_image) != base['sha256']
            or base['experimental'] is not True or base['manifest']['experimental'] is not True):
        parser.error('Expected the unchanged pinned experimental base image')
    core = json.loads((args.core.parent / 'build.json').read_text())
    if core['hardwareLibrarySha256'] != base['manifest']['entries']['system/lib64/libpocket_hidl_api32.so']['sha256']:
        parser.error('Rebuilt core does not match the image HAL adapter')
    if not core.get('optionalInstalldComponent'):
        parser.error('The current runtime requires the embedded installd component')
    out = args.out.resolve();out.mkdir(parents=True, exist_ok=True)
    target = out / ('pocket-api32-arm64-' + args.version + '.zip')
    if target == args.base_image.resolve():
        parser.error('Output must preserve the base image')
    replacements = {'system/bin/pocket-loader': args.loader.read_bytes()}
    for name, source in [('pocket-core-api32', args.core), ('pocket-dns-api32', args.dns), ('pocket-netd-api32', args.netd)]:
        prepared = out / name;shutil.copyfile(source, prepared)
        if not reserve_interpreter(prepared):
            parser.error('Missing ELF interpreter: ' + name)
        replacements['system/bin/' + name] = prepared.read_bytes()
    for name, data in replacements.items():
        if len(data) < 64 or data[:6] != b'\x7fELF\x02\x01' or int.from_bytes(data[18:20], 'little') != 183:
            parser.error('Expected an ARM64 ELF: ' + name)
    marker = 'system/etc/pocket-network-caller32'
    replacements[marker] = b'proc-stat-fd-v1\n'
    manifest = json.loads(json.dumps(base['manifest']));manifest['version'] = args.version
    for name, data in replacements.items():
        manifest['entries'][name] = dict(size=len(data), crc32=zlib.crc32(data),
            sha256=hashlib.sha256(data).hexdigest(), mode=0o644 if name == marker else 0o755)
    raw = (json.dumps(manifest, sort_keys=True, separators=(',', ':')) + '\n').encode()
    temporary = target.with_suffix('.zip.partial')
    with zipfile.ZipFile(args.base_image) as source, zipfile.ZipFile(temporary, 'w') as destination:
        names = source.namelist()
        assert len(names) == len(set(names))
        assert set(names) == set(base['manifest']['entries']) | {'pocket-image.json'}
        old_raw = source.read('pocket-image.json')
        assert hashlib.sha256(old_raw).hexdigest() == base['manifest_sha256']
        assert json.loads(old_raw) == base['manifest']
        for name in sorted(manifest['entries']):
            if name in base['manifest']['entries']:
                data = source.read(name);entry = base['manifest']['entries'][name]
                assert len(data) == entry['size'] and zlib.crc32(data) == entry['crc32'], name
                assert hashlib.sha256(data).hexdigest() == entry['sha256'], name
                assert (source.getinfo(name).external_attr >> 16) & 0o7777 == entry['mode'], name
            else:
                assert name == marker
                data = b''
            member = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0));member.compress_type = zipfile.ZIP_DEFLATED
            member.external_attr = (0o100000 | manifest['entries'][name]['mode']) << 16
            destination.writestr(member, replacements.get(name, data), compresslevel=1)
        destination.writestr('pocket-image.json', raw, compress_type=zipfile.ZIP_DEFLATED, compresslevel=1)
    with zipfile.ZipFile(temporary) as archive:
        assert set(archive.namelist()) == set(manifest['entries']) | {'pocket-image.json'}
        assert archive.read('pocket-image.json') == raw
        for name, entry in manifest['entries'].items():
            data = archive.read(name)
            assert len(data) == entry['size'] and zlib.crc32(data) == entry['crc32'], name
            assert hashlib.sha256(data).hexdigest() == entry['sha256'], name
    temporary.replace(target)
    image = {key: value for key, value in manifest.items() if key not in {'entries', 'schema'}}
    image.update(url=args.url, bytes=target.stat().st_size, sha256=digest(target), manifest=manifest,
        unpacked_bytes=sum(e['size'] for e in manifest['entries'].values()),
        manifest_sha256=hashlib.sha256(raw).hexdigest())
    (out / 'pocket-images.json').write_text(json.dumps(dict(schema=1, images=[image]), sort_keys=True) + '\n')
    receipt = dict(baseImageSha256=base['sha256'], imageSha256=image['sha256'],
        changedPayloads=sorted(set(replacements) & set(base['manifest']['entries'])), addedPayloads=[marker],
        retainedPayloads=len(set(base['manifest']['entries']) - set(replacements)), allPayloadsVerified=True,
        experimental=True, fullLifecycleQualified=False, coreSourceBuild=core)
    (out / 'image-build.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print('NETWORK_IMAGE_UPDATE_PASS', target, image['sha256'], flush=True)


if __name__ == '__main__':
    main()
