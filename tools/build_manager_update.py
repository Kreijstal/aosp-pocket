#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Package a full Manager update, retaining native ABI and optionally replacing guest Java."""
import argparse
from pathlib import Path
import subprocess
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-apk', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--version-code', type=int, required=True)
    parser.add_argument('--version-name', required=True)
    parser.add_argument('--guest-control-jar', type=Path,
                        help='Explicit rebuilt guest bootstrap/control JAR; native image stays pinned')
    parser.add_argument('--guest-file', action='append', default=[], metavar='GUEST_PATH=LOCAL_FILE',
                        help='Explicit replacement/addition under guest system/ or vendor/')
    parser.add_argument('--remove-guest-file', action='append', default=[], metavar='GUEST_PATH',
                        help='Omit a retired system/ or vendor/ payload from the new APK')
    args = parser.parse_args()
    if not args.version_name.replace('.', '').isdigit():
        parser.error('Version name must contain only numbers and dots')
    guest_asset = 'assets/api28-rootfs/system/framework/pocket-zygote.jar.payload'
    guest_files = {}
    removed=set()
    for name in args.remove_guest_file:
        if (not name.startswith(('system/', 'vendor/')) or
                any(part in ('', '.', '..') for part in name.split('/')) or '\\' in name):
            parser.error('Invalid removed guest path: '+name)
        removed.add('assets/api28-rootfs/'+name+'.payload')
    for spec in args.guest_file:
        name, separator, local = spec.partition('=')
        if (not separator or not name.startswith(('system/', 'vendor/')) or
                any(part in ('', '.', '..') for part in name.split('/')) or '\\' in name):
            parser.error('Invalid guest-file path: '+name)
        asset = 'assets/api28-rootfs/'+name+'.payload'
        if asset in removed:parser.error('Guest path cannot be replaced and removed: '+name)
        if asset in guest_files or (asset == guest_asset and args.guest_control_jar):
            parser.error('Duplicate guest replacement: '+name)
        path = Path(local)
        if not path.is_file():
            parser.error('Guest replacement is not a file: '+local)
        guest_files[asset] = path
    if args.guest_control_jar:
        with zipfile.ZipFile(args.guest_control_jar) as guest:
            if 'classes.dex' not in guest.namelist():
                parser.error('Guest control JAR must contain classes.dex')
    args.out.mkdir(parents=True, exist_ok=True)
    toolchain = ROOT.parent/'helloworldandroid'
    toolcp = str(toolchain/'build/tools')+':'+str(ROOT/'out/runtime/tools')
    subprocess.run(['make', '-f', 'runtime/Makefile', 'out/runtime/classes.dex',
                    'out/runtime/tools/PocketManifest.class'], cwd=ROOT, check=True)
    manifest = args.out/'manifest-source.xml'
    manifest.write_text((ROOT/'runtime/app/AndroidManifest.xml').read_text())
    subprocess.run(['java', '-cp', toolcp, 'PocketManifest', str(manifest),
                    str(args.out/'AndroidManifest.xml'), '28', '28',
                    str(args.version_code), args.version_name], check=True)
    unsigned = args.out/'packed.apk'
    with zipfile.ZipFile(args.base_apk) as base, zipfile.ZipFile(unsigned, 'w', zipfile.ZIP_DEFLATED, compresslevel=1) as out:
        names = base.namelist()
        assert 'assets/api28-rootfs/system/bin/sh.payload' in names, 'Expected complete Android 9 image'
        assert 'assets/direct-boot-hooks.json' in names, 'Expected pinned direct runtime'
        base_machine = struct.unpack_from('<H', base.read('assets/api28-rootfs/system/lib64/libc.so.payload'), 18)[0]
        for name, path in guest_files.items():
            if name.endswith('.so.payload'):
                data = path.read_bytes()
                assert data[:4] == b'\x7fELF' and struct.unpack_from('<H', data, 18)[0] == base_machine, \
                    'Guest replacement ELF ABI differs from base APK: '+str(path)
        if args.guest_control_jar:
            assert guest_asset in names, 'Expected existing guest bootstrap JAR'
        for entry in base.infolist():
            if entry.filename in removed:continue
            if entry.filename in ('AndroidManifest.xml', 'classes.dex', 'assets/pocketctl.py') or entry.filename.startswith('META-INF/'):
                continue
            if args.guest_control_jar and entry.filename == guest_asset:
                continue
            if entry.filename in guest_files:
                continue
            out.writestr(entry, base.read(entry), compress_type=entry.compress_type, compresslevel=1)
        out.write(args.out/'AndroidManifest.xml', 'AndroidManifest.xml')
        out.write(ROOT/'out/runtime/classes.dex', 'classes.dex')
        out.write(ROOT/'tools/pocketctl.py', 'assets/pocketctl.py')
        if args.guest_control_jar:
            out.write(args.guest_control_jar, guest_asset)
        for name, path in guest_files.items():
            out.write(path, name)
    target = args.out/('aosp-pocket-manager-'+args.version_name+'.apk')
    signer = ['java', '-cp', str(toolchain/'build/apksigner.jar'), 'com.android.apksigner.ApkSignerTool']
    subprocess.run(signer+['sign', '--ks', str(ROOT/'out/debug.keystore'), '--ks-pass', 'pass:android',
                           '--key-pass', 'pass:android', '--out', str(target), str(unsigned)], check=True)
    subprocess.run(signer+['verify', '--verbose', str(target)], check=True)
    print('Built full APK:', target, target.stat().st_size, 'bytes')
    print('Guest images/native libraries retained from:', args.base_apk)
    if args.guest_control_jar:
        print('Guest Java bootstrap/control replaced from:', args.guest_control_jar)
    for name, path in guest_files.items():
        print('Explicit guest payload:', name, '<-', path)


if __name__ == '__main__':
    main()
