#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the complete Manager with the native Android 9 syscall backend."""
import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import zipfile
from target import ROOT
from build_boot import reserve_interpreter
from build_init_loader import build
from validate_native_loader import validate as validate_loader
from build_guest_media import build as build_media
from build_guest_storage import build_parts as build_storage

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base-apk',type=Path,required=True)
    p.add_argument('--ndk',type=Path,required=True)
    p.add_argument('--version',default='0.35')
    args=p.parse_args();out=ROOT/('out/manager-'+args.version);out.mkdir(parents=True,exist_ok=True)
    def run(*argv):subprocess.run(list(map(str,argv)),cwd=ROOT,check=True)
    cc=args.ndk.resolve()/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang'
    loader=out/'pocket-loader'
    run(cc,'-O2','-g','-Wall','-Wextra','-Werror','-mno-outline-atomics','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pic',
        '-nostdlib','-static','-Wl,-T,runtime/loader/loader.ld,-z,max-page-size=16384',
        ROOT/'runtime/loader/loader.c',ROOT/'runtime/loader/entry.S','-o',loader)
    validate_loader(loader)
    assert loader.read_bytes().count(b'PCKELF01')==1
    bins,libs,data=build(args.ndk.resolve(),out,run,framework=True)
    probe=out/'pocket-boot-probe'
    run(cc,'-O2','-Wall','-Wextra','-Werror',ROOT/'runtime/direct/boot_probe.c','-ldl','-o',probe)
    payload={'system/bin/pocket-loader':loader,'system/bin/pocket-boot-probe':probe,
        'system/lib64/libpocket_zygote_api28.so':libs['libpocket_zygote_api28.so']}
    firewall=out/'iptables'
    run(cc,'-O2','-Wall','-Wextra','-Werror',ROOT/'runtime/loader/iptables.c',
        '-Wl,-z,max-page-size=16384','-o',firewall)
    payload['system/bin/iptables']=firewall
    payload['system/bin/ip6tables']=firewall
    app_process=out/'app_process'
    run(cc,'-O2','-Wall','-Wextra','-Werror',ROOT/'runtime/api28/app_process.c','-Wl,-z,max-page-size=16384','-o',app_process)
    payload['system/bin/app_process64']=app_process
    payload['system/bin/app_process']=app_process
    run('python3',ROOT/'tools/build_guest_netd.py','--base-apk',args.base_apk.resolve(),
        '--out-dir',out/'firewall-netd','--ndk',args.ndk.resolve())
    payload['system/bin/pocket-netd-offline']=out/'firewall-netd/pocket-netd-offline'
    media_payload=build_media(args.base_apk.resolve(),args.ndk.resolve(),out/'media-codecs')
    payload.update({name:Path(path) for name,path in media_payload.items()})
    storage_payload,storage_library=build_storage(args.base_apk.resolve(),args.ndk.resolve(),out/'storage')
    payload.update(storage_payload)
    for name in ('pocket-init-api28','pocket-app-process-api28','pocket-log-writer-api28'):payload['system/bin/'+name]=bins[name]
    with zipfile.ZipFile(args.base_apk) as apk:
        rc=Path(media_payload['system/etc/init/init.syscall.rc']).read_text()
        # Preserve the full Manager framework/control configuration.
        rc=rc.replace(' -Xplugin:libopenjdkjvmti.so','').replace(' -agentpath:/system/lib64/libpocket_zygote_api28.so','')
        # The native loader resolves Magisk's module view. Restore the optional
        # runtime's data/zygote ordering; without a core, setup.sh skips it.
        gate='on property:pocket.data.ready=1\n    setprop pocket.magisk.status adapter-required\n    class_start pocket-zygote\n'
        assert rc.count(gate)==1,'Unexpected syscall init data/zygote ordering'
        (out/'init.native.rc').write_text('import /system/etc/init/init.magisk.rc\n'+rc.replace(gate,'',1))
        payload['system/etc/init/init.syscall.rc']=out/'init.native.rc'
        for entry in apk.infolist():
            name=entry.filename
            if not(name.startswith('assets/api28-rootfs/system/bin/') or name=='assets/api28-rootfs/system/etc/pocket-magisk/getprop.payload'):continue
            if not name.endswith('.payload'):continue
            guest=name[len('assets/api28-rootfs/'):-8]
            if guest in ('system/bin/pocket-syscall-tracer','system/lib64/libpocket_direct_boot.so') or guest in payload:continue
            contents=apk.read(name)
            if contents[:4]!=b'\x7fELF':continue
            phoff=struct.unpack_from('<Q',contents,32)[0];stride,count=struct.unpack_from('<HH',contents,54)
            small=any(struct.unpack_from('<I',contents,phoff+i*stride)[0]==3 and
                struct.unpack_from('<Q',contents,phoff+i*stride+32)[0]<128 for i in range(count))
            if small:
                target=out/'reserved'/guest;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(contents)
                assert reserve_interpreter(target);payload[guest]=target
    for name,path in payload.items():
        if name.startswith('system/bin/') and name!='system/bin/pocket-loader':reserve_interpreter(path)
    toolchain=ROOT.parent/'helloworldandroid';android=toolchain/'vendor/android-platforms/android-36/android.jar'
    r8=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar'
    for name in ('host-classes','host-dex'):(out/name).mkdir(exist_ok=True)
    java=sorted((ROOT/'runtime/app/src').rglob('*.java'))+sorted((ROOT/'runtime/storage/src').rglob('*.java'))+[ROOT/'runtime/direct/BootBattery.java']
    run('javac','--release','8','-classpath',android,'-d',out/'host-classes',*java)
    run('jar','cf',out/'host-classes.jar','-C',out/'host-classes','.')
    run('java','-cp',r8,'com.android.tools.r8.D8','--lib',android,'--min-api','28','--output',out/'host-dex',out/'host-classes.jar')
    command=['python3',ROOT/'tools/build_manager_update.py','--base-apk',args.base_apk.resolve(),'--out',out,
        '--host-dex',out/'host-dex/classes.dex','--version-code',args.version.split('.')[-1],'--version-name',args.version,
        '--host-library','libpocket_ce.so='+str(storage_library),
        '--guest-control-jar',data['system/framework/pocket-zygote.jar'],
        '--remove-guest-file','system/bin/pocket-syscall-tracer','--remove-guest-file','system/lib64/libpocket_direct_boot.so']
    for name,path in payload.items():command+=['--guest-file',name+'='+str(path)]
    run(*command)
    target=out/('aosp-pocket-manager-'+args.version+'.apk')
    with zipfile.ZipFile(target) as apk,zipfile.ZipFile(args.base_apk) as base:
        for name in ('system/lib64/libc.so','system/bin/linker64'):
            asset='assets/api28-rootfs/'+name+'.payload';assert apk.read(asset)==base.read(asset),'Stock runtime changed: '+name
        assert not any('api28-rootfs/' in name and ('pocket-syscall-tracer' in name or 'libpocket_direct_boot.so' in name) for name in apk.namelist())
    print('PASS: complete native Manager; stock Bionic/linker unchanged; no Android 9 tracer/wrapper payload')
if __name__=='__main__':main()
