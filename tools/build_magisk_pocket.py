#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build real Magisk for Pocket's Android 9 userspace and prepare guest assets."""
import argparse
import io
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
COMMIT = 'e8a58776f1d7bdf852072ad0baa6eceb9a1e4aac'
ONDK = 'r29.5'
PACKAGES = {
    'busybox': 'pool/main/b/busybox/busybox_1.38.0-1_aarch64.deb',
    'selinux': 'pool/main/liba/libandroid-selinux/libandroid-selinux_14.0.0.11-1_aarch64.deb',
    'pcre2': 'pool/main/p/pcre2/pcre2_10.49_aarch64.deb',
}


def run(*args, **kwargs):
    subprocess.run(list(map(str, args)), check=True, **kwargs)


def download(url, path):
    if path.is_file() and path.stat().st_size:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix+'.part')
    run('wget', '-q', '--timeout=30', '--tries=2', '-O', temporary, url)
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-apk', type=Path, required=True)
    parser.add_argument('--ndk', type=Path, required=True, help='Project NDK for the guest libc shim')
    parser.add_argument('--package-version', help='Optionally build a complete numbered Manager APK, e.g. 0.23')
    args = parser.parse_args()
    if args.package_version and not re.fullmatch(r'0\.[1-9][0-9]*', args.package_version):
        parser.error('Expected a numeric Manager version such as 0.23')
    out = ROOT/'out/magisk-pocket'
    out.mkdir(parents=True, exist_ok=True)
    source = ROOT/'out/magisk-source'
    if not (source/'.git').exists():
        run('git', 'clone', '--depth', '1', '--branch', 'v30.7',
            'https://github.com/topjohnwu/Magisk.git', source)
    head = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    based_on_pinned = subprocess.run(['git', '-C', str(source), 'merge-base', '--is-ancestor', COMMIT, head],
                                    capture_output=True).returncode == 0
    if head != COMMIT and not based_on_pinned:
        raise RuntimeError('Unexpected Magisk source revision: '+head)
    run('git', 'submodule', 'update', '--init', '--depth', '1', *[
        'native/src/external/'+name for name in ('selinux', 'libcxx', 'cxx-rs', 'lsplt', 'system_properties')], cwd=source)
    patch = ROOT/'runtime/magisk/magisk-pocket.patch'
    applied = subprocess.run(['git', 'apply', '--reverse', '--check', str(patch)],
                             cwd=source, capture_output=True).returncode == 0
    if not applied:
        run('git', 'apply', '--check', patch, cwd=source)
        run('git', 'apply', patch, cwd=source)
    sdk = out/'toolchain'
    ndk = sdk/'ndk'/('ondk-'+ONDK)
    if not (ndk/'toolchains/rust/bin/rustc').is_file():
        archive = out/('ondk-'+ONDK+'-linux.tar.xz')
        download('https://github.com/topjohnwu/ondk/releases/download/'+ONDK+'/'+archive.name, archive)
        with tarfile.open(archive) as tar:
            tar.extractall(sdk/'ndk', filter='data')
    alias = sdk/'ndk/magisk'
    if not alias.exists():
        alias.symlink_to(ndk.name)
    (source/'config.prop').write_text('version=30.7\nabiList=arm64-v8a\n')
    env = dict(os.environ, ANDROID_HOME=str(sdk))
    run('python3', 'build.py', '-v', '-r', 'native', 'magisk', cwd=source, env=env)
    run('python3', ROOT/'tools/build_exec_runtime.py', '--base-apk', args.base_apk.resolve(),
        '--out-dir', out/'exec-runtime', '--ndk', args.ndk)

    guest = out/'rootfs'
    payload = guest/'system/etc/pocket-magisk'
    libs = guest/'system/lib64/pocket-magisk'
    payload.mkdir(parents=True, exist_ok=True)
    libs.mkdir(parents=True, exist_ok=True)
    prefix = 'data/data/com.termux/files/usr/'
    notices = payload/'licenses'
    notices.mkdir(exist_ok=True)
    wanted = {
        prefix+'bin/busybox': payload/'busybox',
        prefix+'lib/libbusybox.so.1.38.0': libs/'libbusybox.so.1.38.0',
        prefix+'lib/libandroid-selinux.so': libs/'libandroid-selinux.so',
        prefix+'lib/libpcre2-8.so': libs/'libpcre2-8.so',
        prefix+'share/doc/pcre2/COPYING': notices/'pcre2-COPYING',
    }
    for name, url in PACKAGES.items():
        package = out/('termux-'+name+'.deb')
        # Preserve the existing busybox artifact name as well as fresh builds.
        if name == 'busybox' and (out/'termux-busybox.deb').is_file():
            package = out/'termux-busybox.deb'
        download('https://packages.termux.dev/apt/termux-main/'+url, package)
        data = subprocess.check_output(['bsdtar', '-xf', str(package), '-O', 'data.tar.xz'])
        with tarfile.open(fileobj=io.BytesIO(data), mode='r:xz') as tar:
            for member in tar:
                path = member.name.removeprefix('./')
                if member.isfile() and path in wanted:
                    wanted[path].write_bytes(tar.extractfile(member).read())
    for path in wanted.values():
        if not path.is_file():
            raise RuntimeError('Package payload missing: '+str(path))
    run('patchelf', '--set-rpath', '/system/lib64/pocket-magisk', payload/'busybox')
    shutil.copyfile(source/'native/out/arm64-v8a/magisk', payload/'magisk')
    shutil.copyfile(source/'LICENSE', notices/'Magisk-LICENSE')
    manager = out/'Magisk-v30.7.apk'
    download('https://github.com/topjohnwu/Magisk/releases/download/v30.7/Magisk-v30.7.apk', manager)
    app = guest/'system/app/Magisk'
    app.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(manager, app/'Magisk.apk')
    download('https://www.gnu.org/licenses/old-licenses/gpl-2.0.txt', notices/'BusyBox-GPL-2.0.txt')
    (notices/'libandroid-selinux.txt').write_text('Termux libandroid-selinux 14.0.0.11-1\n'
        'https://github.com/termux/termux-packages/tree/master/packages/libandroid-selinux\n'
        'Package copyright: Public Domain. Upstream: https://selinuxproject.org\n')
    for name in ('setup.sh', 'stage.sh'):
        shutil.copyfile(ROOT/'runtime/magisk'/name, payload/name)
    util = (source/'scripts/util_functions.sh').read_text().replace('#MAGISK_VERSION_STUB',
        'MAGISK_VER=30.7\nMAGISK_VER_CODE=30700')
    (payload/'util_functions.sh').write_text(util)
    shutil.copyfile(source/'scripts/module_installer.sh', payload/'module_installer.sh')
    compiler = args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang'
    for src, target in [(ROOT/'runtime/magisk/getprop.c', payload/'getprop'),
                        (ROOT/'runtime/tests/magisk/drop_uid.c', out/'drop-uid'),
                        (ROOT/'runtime/tests/magisk/paths.c', out/'paths')]:
        run(compiler, '-D_GNU_SOURCE', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', src, '-o', target)
    with zipfile.ZipFile(out/'fixture.zip', 'w', zipfile.ZIP_DEFLATED) as fixture:
        for path in (ROOT/'runtime/tests/magisk/module').iterdir():
            fixture.write(path, path.name)
    with zipfile.ZipFile(args.base_apk) as apk:
        rc = apk.read('assets/api28-rootfs/system/etc/init/init.manager.rc.payload').decode()
    old = 'on property:pocket.data.ready=1\n    class_start pocket-zygote'
    if old not in rc:
        raise RuntimeError('Unexpected baseline init data/zygote ordering')
    rc = rc.replace(old, '# Data/zygote ordering is provided by init.magisk.rc', 1)
    rc = 'import /system/etc/init/init.magisk.rc\n'+rc
    init = guest/'system/etc/init'
    init.mkdir(exist_ok=True)
    (init/'init.manager.rc').write_text(rc)
    shutil.copyfile(ROOT/'runtime/magisk/init.magisk.rc', init/'init.magisk.rc')
    shutil.copyfile(out/'exec-runtime/libpocket_direct_boot.so', guest/'system/lib64/libpocket_direct_boot.so')
    (out/'provenance.json').write_text(json.dumps({'magiskCommit': COMMIT, 'ondk': ONDK,
        'patch': 'runtime/magisk/magisk-pocket.patch', 'termuxPackages': PACKAGES,
        'hostKernelModified': False, 'hostRootRequired': False}, indent=2)+'\n')
    print('PASS: real Magisk ARM64 userspace and module boot assets prepared:', guest)
    if args.package_version:
        command = ['python3', ROOT/'tools/build_manager_update.py', '--base-apk', args.base_apk.resolve(),
                   '--out', ROOT/('out/manager-'+args.package_version), '--version-name', args.package_version,
                   '--version-code', args.package_version.split('.')[1]]
        for path in sorted(guest.rglob('*')):
            if path.is_file():
                command += ['--guest-file', path.relative_to(guest).as_posix()+'='+str(path)]
        run(*command)


if __name__ == '__main__':
    main()
