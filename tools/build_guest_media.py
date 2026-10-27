#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build Android 9 ARM64 software OMX against a complete Manager's guest ABI."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET
import zipfile
from target import ROOT
from prepare_api28 import prepare
from prepare_boot_resources import prepare_resources
from build_boot import reserve_interpreter


def build(base_apk, ndk, output, audio=False):
    prepare(art=True, arch='arm64')
    output.mkdir(parents=True, exist_ok=True)
    api = ROOT/'out/api28'
    av = api/'frameworks-av'
    libs = output/'lib64'
    libs.mkdir(exist_ok=True)
    with zipfile.ZipFile(base_apk) as apk:
        prefix = 'assets/api28-rootfs/system/lib64/'
        for entry in apk.infolist():
            name = entry.filename
            if name.startswith(prefix) and name.endswith('.so.payload') and '/' not in name[len(prefix):-8]:
                (libs/name[len(prefix):-8]).write_bytes(apk.read(entry))
    generated = output/'generated'
    generated.mkdir(exist_ok=True)
    gen = ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/out/host/linux-x86/bin/hidl-gen'
    for package in ('android.hidl.base@1.0', 'android.hidl.manager@1.0', 'android.hidl.manager@1.1',
                    'android.hardware.configstore@1.0', 'android.hardware.configstore@1.1',
                    'android.hardware.health@1.0', 'android.hardware.health@2.0',
                    'android.hardware.graphics.common@1.0', 'android.hardware.graphics.bufferqueue@1.0',
                    'android.hardware.media@1.0', 'android.hardware.media.omx@1.0',
                    'android.hidl.memory@1.0', 'android.hidl.allocator@1.0',
                    'android.hardware.vibrator@1.0', 'android.hardware.vibrator@1.1', 'android.hardware.vibrator@1.2'):
        subprocess.run([gen,'-o',generated,'-Lc++-headers',
                        '-randroid.hidl:'+str(api/'libhidl/transport'),
                        '-randroid.hardware:'+str(api/'hardware-interfaces'),package],check=True)
    record=generated/'android/media/IAudioRecord.h'
    record.parent.mkdir(parents=True,exist_ok=True)
    record.write_text('namespace android { namespace media { class IAudioRecord; }}\n')
    includes = [api/'headers', generated, api/'frameworks-native/libs/binder/include',
                api/'frameworks-native/headers/media_plugin',
                api/'frameworks-native/headers/media_plugin/media/openmax',
                api/'frameworks-native/headers/media_plugin/media/hardware',
                api/'frameworks-native/libs/gui/include',api/'frameworks-native/libs/ui/include',
                api/'libhidl/base/include',api/'libhidl/transport/include',api/'libhwbinder/include',
                api/'libhidl/transport/token/1.0/utils/include',
                api/'libhidl/libhidlmemory/include',
                api/'hardware-interfaces/configstore/1.1/default',
                av/'include',av/'media/libmedia/include',av/'media/libaudioclient/include',
                av/'media/libstagefright/include',av/'media/libstagefright/omx/include',
                av/'media/libstagefright/foundation/include',av/'media/libstagefright/xmlparser/include',
                av/'media/utils/include',av/'services/mediaresourcemanager',
                api/'system-media/audio/include',api/'libhidl/transport/memory/1.0/default',
                api/'libhidl/transport/allocator/1.0/default']
    includes += [api/'system-core'/p/'include' for p in ('libutils','libcutils','liblog','libsystem','base')]
    clang = ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang'
    cxx = Path(str(clang)+'++')
    common = ['-O2','-fPIC','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
              '-Wno-unused-function','-Wno-writable-strings','-Wno-missing-field-initializers',
              '-Wno-inconsistent-missing-override']
    cpp = [cxx,'-std=gnu++17','-nostdinc++','-isystem',api/'libcxx/include','-nostdlib++',
           '-fno-rtti','-fno-exceptions','-include',ROOT/'runtime/direct/hidl_codegen_compat.h',
           *common,*['-I'+str(p) for p in includes]]
    link = ['-L'+str(libs),'-Wl,-rpath-link,'+str(libs),'-lc++','-lutils','-llog',
            '-Wl,--no-undefined','-Wl,-z,max-page-size=16384']
    def run(argv): subprocess.run(list(map(str,argv)),check=True)
    run([cxx,'-std=c++17','-Wall','-Wextra','-Werror',
         '-I'+str(api/'frameworks-native/headers/media_plugin/media/openmax'),'-c',
         ROOT/'runtime/tests/omx_port_layout.cpp','-o',output/'omx-port-layout-check.o'])
    payload = {}
    def shared(name,sources,extra=(),dependencies=()):
        target=output/name
        run([*cpp,'-shared',*sources,*extra,*link,*dependencies,'-Wl,-soname,'+name,'-o',target])
        payload['system/lib64/'+name]=str(target.resolve())
        print('BUILT',name,flush=True)
    codecs=av/'media/libstagefright/codecs'
    for suffix,src in [('rawdec','raw/SoftRaw.cpp'),('g711dec','g711/dec/SoftG711.cpp')]:
        shared('libstagefright_soft_'+suffix+'.so',[codecs/src],
               extra=['-Wl,--version-script='+str((codecs/src).parent/'exports.lds')],
               dependencies=['-lstagefright_omx','-lstagefright_foundation'])
    mp3=codecs/'mp3dec'
    shared('libstagefright_soft_mp3dec.so',[mp3/'SoftMP3.cpp',*sorted((mp3/'src').glob('*.cpp'))],
           extra=['-I'+str(mp3/'include'),'-I'+str(mp3/'src'),'-DOSCL_UNUSED_ARG(x)=(void)(x)',
                  '-Wno-implicit-const-int-float-conversion','-Wl,--version-script='+str(mp3/'exports.lds')],
           dependencies=['-lstagefright_omx','-lstagefright_foundation'])
    # Use the pinned module's exact common + ARM64 source lists and flags.
    avc=api/'libavc'
    bp=(avc/'Android.bp').read_text()
    for module,wrapper,suffix in [('libavcdec','avcdec/SoftAVCDec.cpp','avcdec'),
                                  ('libavcenc','avcenc/SoftAVCEnc.cpp','avcenc')]:
        block=next(b for b in bp.split('cc_library_static {') if 'name: "'+module+'"' in b)
        main=block.split('arch: {')[0]
        arm64=block.split('arm64: {')[1].split('mips: {')[0]
        sources=[]
        for part in (main,arm64):
            sources += re.findall(r'"([^"]+\.(?:c|s))"',re.search(r'srcs:\s*\[(.*?)\]',part,re.S)[1])
        # Both the module's defines (N_MB_ENABLE changes encoder reconstruction) and the ARM64 ones;
        # its warning flags are left out, as this compiler is newer than the pinned tree's.
        common=re.search(r'cflags:\s*\[(.*?)\]',main,re.S)
        flags=[f for f in re.findall(r'"([^"\n]+)"',common[1] if common else '') if f.startswith(('-D','-U'))]
        flags+=re.findall(r'"([^"\n]+)"',re.search(r'cflags:\s*\[(.*?)\]',arm64,re.S)[1])
        inc=['-I'+str(avc/p) for p in ('common','decoder','decoder/arm','encoder','encoder/arm','encoder/armv8','common/armv8')]
        objects=output/(module+'-objects');objects.mkdir(exist_ok=True)
        def compile_one(name):
            target=objects/(name.replace('/','_')+'.o')
            run([clang,'-O3','-fPIC','-Wno-incompatible-function-pointer-types','-Wno-unused-command-line-argument',*flags,*inc,
                 '-I'+str(api/'system-core/liblog/include'),'-I'+str(api/'system-core/libcutils/include'),
                 '-c',avc/name,'-o',target])
            return target
        with ThreadPoolExecutor(max_workers=8) as pool: built=list(pool.map(compile_one,sources))
        shared('libstagefright_soft_'+suffix+'.so',[codecs/wrapper,*built],
               extra=[*inc,'-Wno-unused-variable','-Wno-unused-but-set-variable','-Wl,-Bsymbolic',
                      '-Wl,--version-script='+str((codecs/wrapper).parent/'exports.lds')],
               dependencies=['-lstagefright_omx','-lstagefright_foundation','-lcutils'])
    config=api/'hardware-interfaces/configstore/1.1/default/SurfaceFlingerConfigs.cpp'
    memory=api/'libhidl/transport/memory/1.0/default'
    allocator=api/'libhidl/transport/allocator/1.0/default'
    uevent=output/'boot_uevent.o'
    run([clang,'-O2','-fPIC','-Wall','-Wextra','-Werror','-c',
         ROOT/'runtime/direct/boot_uevent.c','-o',uevent])
    shared('libpocket_hidl_api28.so',
           [*[ROOT/'runtime/direct'/p for p in ('boot_hidl.cpp','boot_binder.cpp','boot_health.cpp',
              'boot_hardware.cpp','boot_audio.cpp','boot_media.cpp','boot_media_port.cpp',
              'boot_media_fence.cpp','boot_vibrator.cpp')],uevent,config,
            memory/'AshmemMapper.cpp',memory/'AshmemMemory.cpp',allocator/'AshmemAllocator.cpp'],
           extra=['-DPOCKET_MEDIA_CODECS',*(['-DPOCKET_GUEST_AUDIO'] if audio else []),
                  '-include',ROOT/'runtime/direct/hidl_codegen_compat.h'],
           dependencies=['-l:android.hardware.health@1.0.so','-l:android.hardware.health@2.0.so',
                         '-l:android.hardware.configstore@1.0.so','-l:android.hardware.configstore@1.1.so',
                         '-l:android.hardware.media.omx@1.0.so','-l:android.hidl.memory@1.0.so',
                         '-l:android.hidl.allocator@1.0.so','-l:android.hardware.vibrator@1.0.so',
                         '-l:android.hardware.vibrator@1.1.so','-l:android.hardware.vibrator@1.2.so',
                         '-lstagefright','-lstagefright_omx',
                         '-lhwbinder','-lhidlbase','-lhidltransport','-lbinder','-lcutils','-lbase','-ldl'])
    wanted={'OMX.google.raw.decoder','OMX.google.g711.alaw.decoder','OMX.google.g711.mlaw.decoder',
            'OMX.google.mp3.decoder','OMX.google.h264.decoder','OMX.google.h264.encoder'}
    xml=ET.Element('MediaCodecs')
    dec=ET.SubElement(xml,'Decoders');enc=ET.SubElement(xml,'Encoders')
    found=set()
    for kind in ('audio','telephony','video'):
        source=av/('media/libstagefright/data/media_codecs_google_'+kind+'.xml')
        for group in ET.parse(source).getroot():
            for node in group:
                if node.tag=='MediaCodec' and node.get('name') in wanted:
                    (enc if group.tag=='Encoders' else dec).append(node);found.add(node.get('name'))
    assert found==wanted,(found,wanted)
    path=output/'media_codecs.xml';ET.indent(xml);ET.ElementTree(xml).write(path,encoding='utf-8',xml_declaration=True)
    payload['vendor/etc/media_codecs.xml']=str(path.resolve())
    resources=output/'pocket-media-resources'
    run([*cpp,'-fPIE','-pie',ROOT/'runtime/direct/media_resources.cpp',
         av/'services/mediaresourcemanager/ResourceManagerService.cpp',
         av/'services/mediaresourcemanager/ServiceLog.cpp',
         *link,'-lmedia','-lmediautils','-lbinder','-lstagefright','-lcutils',
         '-o',resources])
    assert reserve_interpreter(resources)
    payload['system/bin/pocket-media-resources']=str(resources.resolve())
    metrics=output/'mediametrics'
    metrics.write_bytes((prepare_resources()/'system/bin/mediametrics').read_bytes())
    assert reserve_interpreter(metrics)
    payload['system/bin/mediametrics']=str(metrics.resolve())
    with zipfile.ZipFile(base_apk) as apk:
        rc=apk.read('assets/api28-rootfs/system/etc/init/init.syscall.rc.payload').decode()
    if '\nservice pocket-media-resources ' not in rc:
        rc+='''
service pocket-media-resources /system/bin/pocket-media-resources
    class pocket-zygote
    user 1013
    group 1013

service pocket-media-metrics /system/bin/mediametrics
    class pocket-zygote
    user 1013
    group 1013
'''
    path=output/'init.media.rc';path.write_text(rc)
    payload['system/etc/init/init.syscall.rc']=str(path.resolve())
    notices=[av/'media/libstagefright/codecs'/p/'NOTICE' for p in ('raw','g711/dec','mp3dec')]
    notices.append(avc/'NOTICE')
    notice=output/'pocket-media-NOTICE.txt'
    notice.write_text('Pocket Android 9 software codecs; AOSP android-9.0.0_r61\n'
                      'Source revisions: sources.lock.json, guest_api28.\n\n'+
                      '\n\n'.join(str(p.relative_to(api))+'\n'+p.read_text() for p in notices))
    payload['system/etc/pocket-media-NOTICE.txt']=str(notice.resolve())
    (output/'payload.json').write_text(json.dumps(payload,indent=2)+'\n')
    print('PASS: six real ARM64 software codecs and private OMX backend built',flush=True)
    return payload


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base-apk',type=Path,required=True)
    p.add_argument('--ndk',type=Path,required=True)
    p.add_argument('--out-dir',type=Path,required=True)
    a=p.parse_args();build(a.base_apk.resolve(),a.ndk.resolve(),a.out_dir.resolve())
