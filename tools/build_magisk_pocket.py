#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build Pocket compatibility adapters; optionally build a separate Magisk test runtime."""
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


def prepare_support(base_apk, ndk, out):
    guest=out/'rootfs'
    if guest.exists(): shutil.rmtree(guest)
    integration=guest/'system/etc/pocket-magisk'
    integration.mkdir(parents=True)
    (guest/'system/lib64').mkdir(parents=True)
    run('python3',ROOT/'tools/build_exec_runtime.py','--base-apk',base_apk.resolve(),
        '--out-dir',out/'exec-runtime','--ndk',ndk)
    from build_boot_zygote import build_zygote
    build_zygote(ndk,guest,None,zygote_only=True)
    for name in ('setup.sh','stage.sh'):
        shutil.copyfile(ROOT/'runtime/magisk'/name,integration/name)
    compiler=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang'
    run(compiler,'-D_GNU_SOURCE','-std=c11','-O2','-Wall','-Wextra','-Werror',
        ROOT/'runtime/magisk/getprop.c','-o',integration/'getprop')
    with zipfile.ZipFile(base_apk) as apk:
        rc=apk.read('assets/api28-rootfs/system/etc/init/init.manager.rc.payload').decode()
    old='on property:pocket.data.ready=1\n    class_start pocket-zygote'
    if old not in rc: raise RuntimeError('Unexpected baseline init data/zygote ordering')
    rc='import /system/etc/init/init.magisk.rc\n'+rc.replace(old,'# Optional runtime controls data/zygote ordering',1)
    init=guest/'system/etc/init';init.mkdir(exist_ok=True)
    (init/'init.manager.rc').write_text(rc)
    shutil.copyfile(ROOT/'runtime/magisk/init.magisk.rc',init/'init.magisk.rc')
    shutil.copyfile(out/'exec-runtime/libpocket_direct_boot.so',guest/'system/lib64/libpocket_direct_boot.so')
    return guest


def package_support(base_apk, guest, version):
    command=['python3',ROOT/'tools/build_manager_update.py','--base-apk',base_apk.resolve(),
             '--out',ROOT/('out/manager-'+version),'--version-name',version,'--version-code',version.split('.')[1]]
    for path in sorted(guest.rglob('*')):
        if path.is_file(): command+=['--guest-file',path.relative_to(guest).as_posix()+'='+str(path)]
    run(*command)
    run('python3',ROOT/'tools/validate_magisk_support_apk.py',
        ROOT/('out/manager-'+version)/('aosp-pocket-manager-'+version+'.apk'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-apk', type=Path, required=True)
    parser.add_argument('--ndk', type=Path, required=True, help='Project NDK for the guest libc shim')
    parser.add_argument('--package-version', help='Optionally build a complete numbered Manager APK, e.g. 0.23')
    parser.add_argument('--support-only', action='store_true',help='Build compatibility adapters without fetching or building Magisk')
    args = parser.parse_args()
    if args.package_version and not re.fullmatch(r'0\.[1-9][0-9]*', args.package_version):
        parser.error('Expected a numeric Manager version such as 0.23')
    out = ROOT/'out/magisk-pocket'
    out.mkdir(parents=True, exist_ok=True)
    if args.support_only:
        guest=prepare_support(args.base_apk,args.ndk,out)
        if args.package_version: package_support(args.base_apk,guest,args.package_version)
        return
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
    guest=prepare_support(args.base_apk,args.ndk,out)
    addon = out/'optional-runtime'
    if addon.exists(): shutil.rmtree(addon)
    payload = addon/'data/adb/magisk'
    libs = payload/'lib'
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
    # Magisk copies this front-end into its temporary applet directory. Keep
    # dependency lookup stable at both locations through the guest resolver.
    run('patchelf', '--set-rpath', '/data/adb/magisk/lib', payload/'busybox')
    shutil.copyfile(source/'native/out/arm64-v8a/magisk', payload/'magisk')
    shutil.copyfile(source/'LICENSE', notices/'Magisk-LICENSE')
    download('https://www.gnu.org/licenses/old-licenses/gpl-2.0.txt', notices/'BusyBox-GPL-2.0.txt')
    (notices/'libandroid-selinux.txt').write_text('Termux libandroid-selinux 14.0.0.11-1\n'
        'https://github.com/termux/termux-packages/tree/master/packages/libandroid-selinux\n'
        'Package copyright: Public Domain. Upstream: https://selinuxproject.org\n')
    util = (source/'scripts/util_functions.sh').read_text().replace('#MAGISK_VERSION_STUB',
        'MAGISK_VER=30.7\nMAGISK_VER_CODE=30700')
    (payload/'util_functions.sh').write_text(util)
    shutil.copyfile(source/'scripts/module_installer.sh', payload/'module_installer.sh')
    compiler = args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang'
    run(str(compiler)+'++', '-std=c++17', '-fPIC', '-shared', '-O2', '-Wall', '-Wextra', '-Werror',
        '-I'+str(source/'native/src/core/zygisk'), ROOT/'runtime/tests/magisk/zygisk_fixture.cpp',
        '-o', out/'zygisk-fixture.so')
    for src, target in [(ROOT/'runtime/tests/magisk/drop_uid.c', out/'drop-uid'),
                        (ROOT/'runtime/tests/magisk/paths.c', out/'paths')]:
        run(compiler, '-D_GNU_SOURCE', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', src, '-o', target)
    with zipfile.ZipFile(out/'fixture.zip', 'w', zipfile.ZIP_DEFLATED) as fixture:
        for path in (ROOT/'runtime/tests/magisk/module').iterdir():
            fixture.write(path, path.name)
    with zipfile.ZipFile(out/'pocket-magisk-optional-runtime.zip','w',zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(addon.rglob('*')):
            if path.is_file(): archive.write(path,path.relative_to(addon).as_posix())
    # Dedicated device-test input, never included in the distribution APK.
    with tarfile.open(out/'live-payload.tar','w') as archive:
        for tree in (guest,addon):
            for path in sorted(tree.rglob('*')):
                if path.is_file(): archive.add(path,arcname=path.relative_to(tree),recursive=False)
    (out/'provenance.json').write_text(json.dumps({'magiskCommit': COMMIT, 'ondk': ONDK,
        'patch': 'runtime/magisk/magisk-pocket.patch', 'termuxPackages': PACKAGES,
        'hostKernelModified': False, 'hostRootRequired': False, 'magiskBundledInApk': False,
        'optionalRuntime': 'pocket-magisk-optional-runtime.zip'}, indent=2)+'\n')
    print('PASS: support-only APK assets and separate optional Magisk runtime prepared:', guest)
    if args.package_version:
        package_support(args.base_apk,guest,args.package_version)


if __name__ == '__main__':
    main()
