#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Rebuild the current loader in a pinned image, retaining every other payload."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import urllib.parse
import zipfile
import zlib
from build_api32_image import build_loader


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-image', type=Path, required=True)
    parser.add_argument('--base-catalog', type=Path, required=True)
    parser.add_argument('--ndk', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--url', required=True)
    args = parser.parse_args()
    url = urllib.parse.urlsplit(args.url)
    if url.scheme != 'https' or not url.hostname or url.username or url.password or url.fragment:
        parser.error('The image requires a public HTTPS URL')
    if not re.fullmatch(r'[0-9]+(?:\.[0-9]+)*', args.version):
        parser.error('Invalid image version')
    catalog = json.loads(args.base_catalog.read_text())
    images = [image for image in catalog['images'] if image['sdk'] == 32]
    if catalog['schema'] != 1 or len(images) != 1:
        parser.error('Expected one API32 image in the base catalog')
    base = images[0]
    if args.base_image.stat().st_size != base['bytes'] or digest(args.base_image) != base['sha256']:
        parser.error('Base archive differs from its pinned catalog')
    if base['experimental'] is not True or base['manifest']['experimental'] is not True:
        parser.error('Expected an experimental API32 image')
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    target = out / ('pocket-api32-arm64-' + args.version + '.zip')
    if target == args.base_image.resolve():
        parser.error('Output must not replace the preserved base image')
    loader = build_loader(args.ndk.resolve(), out).read_bytes()
    loader_name = 'system/bin/pocket-loader'
    manifest = json.loads(json.dumps(base['manifest']))
    manifest['version'] = args.version
    manifest['entries'][loader_name] = dict(size=len(loader), crc32=zlib.crc32(loader),
                                          sha256=hashlib.sha256(loader).hexdigest(), mode=0o755)
    raw = (json.dumps(manifest, sort_keys=True, separators=(',', ':')) + '\n').encode()
    temporary = target.with_suffix('.zip.partial')
    with zipfile.ZipFile(args.base_image) as source, zipfile.ZipFile(temporary, 'w') as destination:
        names = source.namelist()
        assert len(names) == len(set(names))
        assert set(names) == set(base['manifest']['entries']) | {'pocket-image.json'}
        old_raw = source.read('pocket-image.json')
        assert hashlib.sha256(old_raw).hexdigest() == base['manifest_sha256']
        assert json.loads(old_raw) == base['manifest']
        for name, entry in sorted(base['manifest']['entries'].items()):
            info = source.getinfo(name)
            data = source.read(name)
            assert len(data) == entry['size'] and zlib.crc32(data) == entry['crc32'], name
            assert hashlib.sha256(data).hexdigest() == entry['sha256'], name
            assert (info.external_attr >> 16) & 0o7777 == entry['mode'], name
            replacement = loader if name == loader_name else data
            member = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            member.compress_type = zipfile.ZIP_DEFLATED
            member.external_attr = (0o100000 | manifest['entries'][name]['mode']) << 16
            destination.writestr(member, replacement, compresslevel=1)
        destination.writestr('pocket-image.json', raw, compress_type=zipfile.ZIP_DEFLATED, compresslevel=1)
    # Independently verify the complete outgoing archive before publishing its catalog.
    with zipfile.ZipFile(temporary) as archive:
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
                   changedPayloads=[loader_name], retainedPayloads=len(manifest['entries'])-1,
                   allPayloadsVerified=True, experimental=True, fullLifecycleQualified=False,
                   loaderSourceBuild=json.loads((out / 'loader-build.json').read_text()))
    (out / 'image-build.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print('LOADER_IMAGE_UPDATE_PASS', target, image['sha256'], flush=True)


if __name__ == '__main__':
    main()
