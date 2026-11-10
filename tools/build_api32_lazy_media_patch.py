#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Stage a reversible lazy-media trial against an explicitly pinned image."""
import argparse,hashlib,shutil,subprocess,zipfile
from pathlib import Path
from target import ROOT
from build_boot import reserve_interpreter
from api32_runtime_boot import supervised_bootstrap,supervised_boot_script,embedded_system_server_bridge
from build_api32_memory_overlay import build_memory_overlay
from build_api32_media import build as build_media
from build_api32_bootstrap import build as build_bootstrap
from build_api32_runtime_tools import build as build_runtime_tools

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image',type=Path,required=True);p.add_argument('--sha256',required=True)
    p.add_argument('--core',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--ndk',type=Path,required=True)
    p.add_argument('--defer-extractor',action='store_true')
    p.add_argument('--supervised-bootstrap',action='store_true')
    p.add_argument('--memory-profile',action='store_true',help='Also trial the matching immutable cache resource/property profile')
    p.add_argument('--system-server-bridge',action='store_true')
    p.add_argument('--warm-extractor',action='store_true',help='Publish the extractor before framework boot; plugin loading stays on demand')
    a=p.parse_args();assert hashlib.file_digest(a.image.open('rb'),'sha256').hexdigest()==a.sha256
    assert not a.out.exists();a.out.mkdir(parents=True)
    payload={'system/bin/pocket-core-api32':a.core,
        'system/lib64/libpocket_binder_api32.so':ROOT/'out/api32/native/libpocket_binder_api32.so',
        'system/lib64/libpocket_hidl_api32.so':ROOT/'out/api32/framework/libpocket_hidl_api32.so'}
    if a.supervised_bootstrap:payload['system/bin/pocket-init-api32']=ROOT/'out/api32/native/pocket-init-api32'
    if a.memory_profile:
        payload['product/overlay/PocketMemory/PocketMemory.apk']=build_memory_overlay(ROOT/'out/api32/reference')
    if a.system_server_bridge:
        payload['system/framework/pocket-zygote.jar']=build_bootstrap(a.out/'bootstrap-build')
        payload['system/framework/pocket-tools.jar']=build_runtime_tools(a.out/'tools-build')
    for name,source in payload.items():
        dest=a.out/name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,dest)
        if name.endswith(('pocket-core-api32','pocket-init-api32')):reserve_interpreter(dest)
    with zipfile.ZipFile(a.image) as z:
        if a.memory_profile:
            props=dict(line.split('=',1) for line in (ROOT/'runtime/api32/memory.prop').read_text().splitlines()
                if line and not line.startswith('#'))
            name='system/etc/pocket-profile.prop';original=z.read(name).decode();key='ro.pocket.max_cached_processes='
            lines=original.splitlines();assert sum(line.startswith(key) for line in lines)==1
            target=a.out/name;target.parent.mkdir(parents=True,exist_ok=True)
            target.write_text('\n'.join(key+props[key[:-1]] if line.startswith(key) else line for line in lines)+'\n')
        name='system/etc/pocket-runtime-boot.sh';boot=z.read(name).decode()
        for service in ['pocket-mediametrics','pocket-mediaswcodec','pocket-mediaextractor','pocket-mediaserver','pocket-cameraserver']:
            line='start_service '+service+'\n';assert boot.count(line)==1;boot=boot.replace(line,'')
        marker='await_marker API32_INIT_READY\n';assert boot.count(marker)==1
        boot=boot.replace(marker,marker+'/system/bin/pocket-setprop-api32 ro.pocket.lazy_media 1\n')
        if a.warm_extractor:
            line='start_service pocket-surfaceflinger\n';assert boot.count(line)==1
            boot=boot.replace(line,line+'start_service pocket-mediaextractor\nwait_service media.extractor\n')
        if a.system_server_bridge:
            line='start_service pocket-input-control\n';assert boot.count(line)==1;boot=boot.replace(line,'')
        if a.supervised_bootstrap:boot=supervised_boot_script(boot)
        dest=a.out/name;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_text(boot)
        name='system/etc/init/pocket-runtime.rc';dest=a.out/name;dest.parent.mkdir(parents=True,exist_ok=True)
        config=z.read(name).decode()
        if a.system_server_bridge:config=embedded_system_server_bridge(config)
        dest.write_text(supervised_bootstrap(config) if a.supervised_bootstrap else config)
    cache=ROOT/'out/api32';reference=cache/'reference';libs=reference/'system/lib64';bionic=reference/'apex/com.android.runtime/lib64/bionic'
    compiler=a.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android32-clang++'
    includes=[cache/'native/generated',cache/'framework/generated',cache/'frameworks-native/libs/binder/include',cache/'libhidl/base/include',cache/'libhidl/transport/include',cache/'libhwbinder/include',cache/'libbase/include',cache/'system-logging/liblog/include']
    includes += [cache/'system-core'/n/'include' for n in ['libutils','libcutils','libsystem']]
    includes += [cache/'libfmq/include',cache/'libfmq/base']
    probe=a.out/'pocket-lazy-media-probe'
    # Pinned generated BnHwBase predates Clang's missing-override diagnostic.
    subprocess.run([str(compiler),'-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H','-nostdinc++','-isystem',str(cache/'libcxx/include'),'-nostdlib++','-std=c++17','-O2','-fno-rtti','-fno-exceptions','-Wall','-Wextra','-Werror','-Wno-inconsistent-missing-override','-fPIE','-pie',*['-I'+str(n) for n in includes],str(ROOT/'runtime/tests/api32/lazy_service_probe.cpp'),str(bionic/'libc.so'),'-L'+str(libs),'-L'+str(bionic),'-Wl,-rpath-link,'+str(libs),'-Wl,-rpath-link,'+str(bionic),'-lbinder','-lutils','-lc++','-llog','-lhidlbase','-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',str(probe)],check=True)
    reserve_interpreter(probe)
    if a.defer_extractor:
        target=a.out/'system/lib64/libpocket_media_api32.so';target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(build_media(a.ndk,a.out/'media-build'),target)
    for directory in ['bootstrap-build','tools-build','media-build']:
        if (a.out/directory).exists():shutil.rmtree(a.out/directory)
    print('API32_LAZY_MEDIA_PATCH_BUILD_PASS',a.out)
if __name__=='__main__':main()
