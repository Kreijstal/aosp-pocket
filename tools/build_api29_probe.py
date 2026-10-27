#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build a separate non-debuggable fixture for real Android 10 loader/ART tests."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import zipfile
import xml.etree.ElementTree as ET
from prepare_api29 import ROOT, prepare
from build_boot import reserve_interpreter
from validate_native_loader import validate as validate_loader
from build_api29_native import build_properties, build_binder, build_init, build_zygote
from boot_class_manifest import write_manifest
from build_api29_framework import build_hardware, build_storage, build_graphics, build_network

PACKAGE = "dev.kreijstal.aosppocket.api29test"


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk',type=Path,required=True)
    parser.add_argument('--out',type=Path,default=ROOT/'out/api29/probe')
    parser.add_argument('--art',action='store_true')
    parser.add_argument('--properties',action='store_true')
    parser.add_argument('--binder',action='store_true')
    parser.add_argument('--init',action='store_true')
    parser.add_argument('--zygote',action='store_true')
    parser.add_argument('--hardware',action='store_true')
    parser.add_argument('--system-server',action='store_true',help='Experimental full framework startup; still requires launcher/app validation')
    parser.add_argument('--ui',action='store_true',help='Also verify the real launcher, APK installation, drawing and injected touch input')
    parser.add_argument('--reuse-native',action='store_true',help='Repackage existing API 29 native outputs; rebuild native sources separately before using this after a native change')
    parser.add_argument('--installed-hashes',type=Path,help='Build a small fixture update; unchanged private-root files must pass the signed full-image SHA-256 manifest')
    args=parser.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    if args.ui:args.system_server=True
    if args.system_server:args.hardware=True;args.zygote=True
    if args.hardware:args.binder=True
    if args.binder:args.properties=True
    if args.zygote:args.init=True;args.art=True
    if args.init:args.properties=True
    reference=prepare();stage=out/'root'
    if stage.exists():shutil.rmtree(stage)
    stage.mkdir();runtime_root='/data/user/0/'+PACKAGE+'/files/root'
    android=ROOT.parent/'helloworldandroid/vendor/android-platforms/android-36/android.jar'
    r8=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar'
    tool=ROOT.parent/'helloworldandroid';cc=args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang'
    def run(*cmd):subprocess.run(list(map(str,cmd)),cwd=ROOT,check=True)
    loader=out/'pocket-loader'
    run(cc,'-O2','-g','-Wall','-Wextra','-Werror','-mno-outline-atomics','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pic',
        '-nostdlib','-static','-Wl,-T,runtime/loader/loader.ld,-z,max-page-size=16384',ROOT/'runtime/loader/loader.c',ROOT/'runtime/loader/entry.S','-o',loader)
    validate_loader(loader)
    data=bytearray(loader.read_bytes());marker=b'PCKELF01'+bytes(3072);assert data.count(marker)==1;offset=data.index(marker)+8
    for name in [runtime_root,runtime_root+'/system/bin/pocket-loader',runtime_root+'/system/bin/linker64']:
        value=name.encode()+b'\0';assert len(value)<=1024;data[offset:offset+1024]=value.ljust(1024,b'\0');offset+=1024
    loader.write_bytes(data)
    payloads={};generated={};executables={'system/bin/sh','system/bin/toybox'}
    if args.properties:
        native=ROOT/'out/api29/native' if args.reuse_native else build_properties(args.ndk.resolve())
        for name in ['pocket-property-service-api29','pocket-property-probe-api29']:
            generated['system/bin/'+name]=native/name
            executables.add('system/bin/'+name)
    if args.binder:
        native=ROOT/'out/api29/native' if args.reuse_native else build_binder(args.ndk.resolve())
        generated['system/bin/pocket-binder-probe-api29']=native/'pocket-binder-probe-api29'
        generated['system/lib64/libpocket_binder_api29.so']=native/'libpocket_binder_api29.so'
        executables.add('system/bin/pocket-binder-probe-api29')
    if args.init:
        native=ROOT/'out/api29/native' if args.reuse_native else build_init(args.ndk.resolve())
        for name in ['pocket-init-api29','pocket-init-test-api29']:
            generated['system/bin/'+name]=native/name
            executables.add('system/bin/'+name)
    if args.zygote:
        native=ROOT/'out/api29/native' if args.reuse_native else build_zygote(args.ndk.resolve())
        for name in ['pocket-app-process-api29','pocket-zygote-client-api29','pocket-log-writer-api29']:
            generated['system/bin/'+name]=native/name
            executables.add('system/bin/'+name)
        generated['system/lib64/libpocket_zygote_api29.so']=native/'libpocket_zygote_api29.so'
    if args.hardware:
        hidl_gen=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts/build-tools/linux-x86/bin/hidl-gen'
        native=ROOT/'out/api29/framework' if args.reuse_native else build_hardware(args.ndk.resolve(),hidl_gen)
        for name in ['pocket-hw-services-api29','pocket-hardware-probe-api29']:
            generated['system/bin/'+name]=native/name
            executables.add('system/bin/'+name)
        if args.system_server:
            if not args.reuse_native:build_storage(args.ndk.resolve())
            if not args.reuse_native:build_graphics(args.ndk.resolve())
            if not args.reuse_native:build_network(args.ndk.resolve())
            for name in ['pocket-framework-probe-api29','pocket-boot-probe-api29','pocket-suspend-control-api29','pocket-apex-service-api29','pocket-network-services-api29']:
                generated['system/bin/'+name]=native/name
                executables.add('system/bin/'+name)
            generated['system/bin/pocket-storage-services-api29']=native/'pocket-storage-services-api29'
            for name in ['pocket-gralloc-probe-api29','pocket-egl-probe-api29']:
                generated['system/bin/'+name]=native/name;executables.add('system/bin/'+name)
            generated['vendor/lib64/hw/hwcomposer.pocket.so']=native/'hwcomposer.pocket.so'
            executables|={'system/bin/pocket-storage-services-api29','system/bin/installd','system/bin/surfaceflinger','system/bin/screencap'}
            executables|={'system/bin/idmap2','system/bin/idmap2d'}
            executables|={'system/bin/keystore','system/bin/gatekeeperd'}
            executables.add('system/bin/audioserver')
        generated['system/lib64/libpocket_hidl_api29.so']=native/'libpocket_hidl_api29.so'
    if args.art:executables|={'system/bin/dalvikvm64','apex/com.android.runtime/bin/dalvikvm64'}
    bionic=reference/'apex/com.android.runtime/lib64/bionic'
    runtime=reference/'apex/com.android.runtime/lib64';platform=reference/'system/lib64'
    def locate(name,origin=None):
        # APEX keeps a private libc++ with the same SONAME as the platform
        # copy. Preserve both dependency closures instead of mixing their
        # namespaces through a filename-only search order.
        if 'system/lib64/'+name in generated:return generated['system/lib64/'+name]
        apex_origin=origin is not None and origin.is_relative_to(reference/'apex')
        module=reference/'apex'/origin.relative_to(reference/'apex').parts[0]/'lib64' if apex_origin else runtime
        vendor_origin=origin is not None and any(origin.is_relative_to(path) for path in
            [reference/'vendor',platform/'vndk-sp-29',platform/'vndk-29'])
        vendor=[reference/'vendor/lib64',platform/'vndk-sp-29',platform/'vndk-29']
        search=[bionic,module,runtime,platform] if apex_origin else [bionic,*vendor,platform,runtime] if vendor_origin else [bionic,platform,runtime,*vendor]
        for directory in search:
            if (directory/name).is_file():return directory/name
        raise RuntimeError('Missing Android 10 library: '+name)
    queue=[generated.get(name,reference/name) for name in executables]
    queue += [path for name,path in generated.items() if name.endswith('.so')]
    generated_names={path:name for name,path in generated.items()}
    app_roots=['system/app','system/priv-app','system/product/priv-app',
               'system/product/app/LatinIME','system/product/app/ModuleMetadata'] if args.system_server else []
    if args.system_server:
        for prefix in ['apex',*app_roots]:
            for path in (reference/prefix).rglob('*'):
                if not path.is_file():continue
                with path.open('rb') as source:magic=source.read(4)
                if magic!=b'\x7fELF':continue
                if path.suffix=='.so':queue.append(path)
                elif re.search(r'\bINTERP\b',subprocess.check_output(['readelf','-lW',str(path)],text=True)):
                    name=path.relative_to(reference).as_posix();executables.add(name);queue.append(path)
    if args.art:
        queue += [locate(n,runtime/'runtime') for n in ['libart.so','libjavacore.so','libopenjdk.so','libopenjdkjvm.so']]
        # ART's APEX palette shim opens its platform implementation with
        # dlopen; it therefore does not appear in the DT_NEEDED closure.
        queue.append(locate('libartpalette-system.so',platform/'platform'))
        # NativeLoader eagerly opens the real platform's public libraries
        # before the Java entry point, even when that entry point uses no JNI.
        for line in (reference/'system/etc/public.libraries.txt').read_text().splitlines():
            parts=line.split('#',1)[0].split()
            if parts and '32' not in parts[1:] and 'nopreload' not in parts[1:]:
                queue.append(locate(parts[0],platform/'platform'))
    if args.system_server:
        # Framework JNI entry points (for example wifi-service) are loaded by
        # name at runtime and are absent from the executable DT_NEEDED graph.
        # A usable guest needs the complete matching platform library set.
        queue.extend(platform.glob('*.so'))
        queue.append(locate('libandroid_servers.so',platform/'platform'))
        # JIT loads the compiler on demand rather than through DT_NEEDED.
        queue.append(locate('libart-compiler.so',runtime/'runtime'))
        for relative in ['hw/android.hardware.graphics.composer@2.1-impl.so','hw/android.hardware.graphics.allocator@2.0-impl.so',
            'hw/android.hardware.graphics.mapper@2.0-impl-2.1.so','hw/gralloc.redroid.so',
            'hw/android.hardware.keymaster@3.0-impl.so','hw/gatekeeper.redroid.so','hw/android.hardware.gatekeeper@1.0-impl.so',
            'hw/android.hardware.audio@4.0-impl.so','hw/android.hardware.audio.effect@4.0-impl.so',
            'hw/audio.primary.default.so','hw/audio.r_submix.default.so',
            'egl/libEGL_swiftshader.so','egl/libGLESv1_CM_swiftshader.so','egl/libGLESv2_swiftshader.so']:
            queue.append(reference/'vendor/lib64'/relative)
    def copy(source,name=None):
        name=name or source.relative_to(reference).as_posix()
        if name in payloads:return
        target=stage/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target);payloads[name]=str(source)
    while queue:
        source=queue.pop();name=generated_names[source] if source in generated_names else source.relative_to(reference).as_posix()
        if name in payloads:continue
        copy(source,name)
        output=subprocess.check_output(['readelf','-dW',str(source)],text=True)
        for dependency in re.findall(r'Shared library: \[([^]]+)\]',output):
            library=locate(dependency,source);queue.append(library)
            # The platform provides these Bionic aliases before and after APEX activation.
            if dependency in ['libc.so','libm.so','libdl.so']:copy(library,'system/lib64/'+dependency)
            if args.system_server and any(source.is_relative_to(path) for path in
                [reference/'vendor',platform/'vndk-29',platform/'vndk-sp-29']) and library.is_relative_to(platform/'vndk-29') and not dependency.startswith('android.'):
                # Non-SP VNDK helper libraries normally live in a vendor
                # daemon's namespace. Our software passthrough HALs retain
                # their matching helper ABI as private vendor copies.
                copy(library,'vendor/lib64/'+dependency)
    copy(reference/'system/bin/linker64')
    copy(reference/'apex/com.android.runtime/bin/linker64')
    copy(reference/'system/build.prop')
    copy(reference/'system/etc/ld.config.29.txt','system/etc/ld.config.txt')
    if args.system_server:
        config=stage/'system/etc/ld.config.txt'
        text=config.read_text()
        # The stock image runs composer/allocator out of process. Pocket's
        # rootless software profile uses passthrough objects, so expose only
        # their matching framework interfaces and FMQ support to SP-HAL.
        marker='namespace.sphal.link.vndk.shared_libs = '
        assert text.count(marker)==1
        text=text.replace(marker,'namespace.sphal.link.default.shared_libs += android.hardware.graphics.composer@2.1.so:android.hardware.graphics.allocator@2.0.so:android.hardware.keymaster@3.0.so:android.hardware.gatekeeper@1.0.so:android.hardware.audio@4.0.so:android.hardware.audio.effect@4.0.so:android.hardware.audio.common@4.0.so:libfmq.so\n'+marker)
        config.write_text(text)
        # With ro.vndk.version=29 the stock linker selects this versioned
        # configuration instead of the unversioned fallback.
        copy(reference/'system/etc/ld.config.29.txt','system/etc/ld.config.29.txt')
        (stage/'system/etc/ld.config.29.txt').write_text(text)
    copy(reference/'apex/com.android.runtime/etc/ld.config.txt')
    environment=json.loads((reference/'.pocket-api29-reference.json').read_text())['bootclasspath']
    if args.art:
        for name in environment.split(':'):copy(reference/name.lstrip('/'))
        for path in (reference/'system/framework').rglob('boot*'):
            if path.is_file() and '/arm/' not in str(path):copy(path)
        for prefix in ['apex/com.android.runtime/etc','apex/com.android.tzdata/etc','system/usr','system/etc/permissions']:
            for path in (reference/prefix).rglob('*'):
                if path.is_file():copy(path)
        for name in ['system/etc/public.libraries.txt','system/etc/preloaded-classes','system/etc/event-log-tags']:copy(reference/name)
    if args.zygote:
        copy(reference/'system/framework/framework-res.apk')
    if args.system_server:
        for prefix in ['system/framework','system/fonts','system/etc','vendor/etc','system/product/etc','apex',*app_roots]:
            for path in (reference/prefix).rglob('*'):
                if path.is_file() and '/arm/' not in str(path):copy(path)
        # This profile has no Bluetooth controller or HAL bridge. Advertising
        # it starts the stock Bluetooth process, whose kernel wake-alarm path
        # is unavailable to an app UID. Keep the permission XML itself while
        # declaring only the hardware actually supplied by Pocket.
        for prefix in ['system/etc/permissions','vendor/etc/permissions','system/product/etc/permissions']:
            for path in (stage/prefix).glob('*.xml'):
                tree=ET.parse(path);changed=False
                for feature in list(tree.getroot().findall('feature')):
                    if feature.get('name','').startswith('android.hardware.bluetooth'):
                        tree.getroot().remove(feature);changed=True
                if changed:tree.write(path,encoding='utf-8',xml_declaration=True)
        # This guest profile contains only a 64-bit runtime.
        merged={}
        for path in [reference/'system/etc/prop.default',reference/'system/build.prop',
                     reference/'vendor/build.prop',reference/'system/product/build.prop']:
            if not path.is_file():continue
            for line in path.read_text().splitlines():
                if '=' in line and not line.lstrip().startswith('#'):
                    key,value=line.split('=',1);merged[key.strip()]=value.strip()
        merged.update({'ro.product.cpu.abilist':'arm64-v8a','ro.product.cpu.abilist64':'arm64-v8a',
                       'ro.product.cpu.abilist32':'','ro.zygote':'zygote64','dalvik.vm.usejitprofiles':'false',
                       'ro.vendor.product.cpu.abilist':'arm64-v8a','ro.vendor.product.cpu.abilist64':'arm64-v8a',
                       'ro.vendor.product.cpu.abilist32':'','ro.vndk.version':'29','ro.input.video_enabled':'false','sys.use_memfd':'true',
                       'dalvik.vm.dexopt.secondary':'false','ro.zygote.disable_gl_preload':'true',
                       'ro.hardware.gralloc':'redroid','ro.hardware.hwcomposer':'pocket','ro.hardware.egl':'swiftshader',
                       'ro.sf.lcd_density':'160','ro.boot.redroid_width':'480','ro.boot.redroid_height':'800','ro.boot.redroid_fps':'30'})
        (stage/'system/build.prop').write_text(''.join(key+'='+value+'\n' for key,value in merged.items()))
    for name in executables:
        path=stage/name;assert reserve_interpreter(path);data=bytearray(path.read_bytes())
        phoff=struct.unpack_from('<Q',data,32)[0];stride,count=struct.unpack_from('<HH',data,54)
        for i in range(count):
            entry=struct.unpack_from('<IIQQQQQQ',data,phoff+i*stride)
            if entry[0]==3:
                value=(runtime_root+'/system/bin/pocket-loader').encode()+b'\0';assert len(value)<=entry[5]
                data[entry[2]:entry[2]+entry[5]]=value.ljust(entry[5],b'\0');break
        else:raise RuntimeError('Executable has no interpreter')
        path.write_bytes(data)
    copy(loader,'system/bin/pocket-loader')
    for name in ['classes','dex','hello-classes','hello-dex','tools','zygote-classes','zygote-dex']:
        directory=out/name
        if directory.exists():shutil.rmtree(directory)
        directory.mkdir()
    java=[ROOT/'runtime/tests/api29/MainActivity.java',ROOT/'runtime/tests/api29/HostBinder.java']
    java += [ROOT/'runtime/app/src/dev/kreijstal/aosppocket/runtime'/name for name in ['GuestBinder.java','Native.java','GuestPower.java']]
    run('javac','--release','8','-cp',android,'-d',out/'classes',*java)
    run('jar','cf',out/'classes.jar','-C',out/'classes','.')
    run('java','-cp',r8,'com.android.tools.r8.D8','--lib',android,'--min-api','28','--output',out/'dex',out/'classes.jar')
    if args.art:
        run('javac','--release','8','-d',out/'hello-classes',ROOT/'runtime/tests/api29/HelloAndroid10.java')
        run('jar','cf',out/'hello-classes.jar','-C',out/'hello-classes','.')
        run('java','-cp',r8,'com.android.tools.r8.D8','--lib',android,'--min-api','29','--output',out/'hello-dex',out/'hello-classes.jar')
        (stage/'data/local/tmp').mkdir(parents=True,exist_ok=True)
        with zipfile.ZipFile(stage/'data/local/tmp/hello29.jar','w') as jar:jar.write(out/'hello-dex/classes.dex','classes.dex')
    if args.zygote:
        for name in ['zygote-classes','zygote-dex']:(out/name).mkdir(exist_ok=True)
        java=[ROOT/'runtime/api29/ZygoteBootstrap29.java',ROOT/'runtime/api29/Api29Child.java']
        if args.ui:
            java += [ROOT/name for name in ['runtime/direct/FrameworkProbe.java','runtime/direct/GuestControl.java',
                'runtime/direct/HostDevices.java','runtime/direct/HostNetwork.java',
                'runtime/app/src/dev/kreijstal/aosppocket/bridge/HostWire.java',
                'runtime/app/src/dev/kreijstal/aosppocket/bridge/DisplayGeometry.java']]
        run('javac','--release','8','-cp',android,'-d',out/'zygote-classes',*java)
        run('jar','cf',out/'zygote-classes.jar','-C',out/'zygote-classes','.')
        run('java','-cp',r8,'com.android.tools.r8.D8','--lib',android,'--min-api','29','--output',out/'zygote-dex',out/'zygote-classes.jar')
        jar=stage/'system/framework/pocket-zygote.jar'
        with zipfile.ZipFile(jar,'w') as archive:archive.write(out/'zygote-dex/classes.dex','classes.dex')
        if args.ui:copy(ROOT/'out/runtime/guest-test.apk','system/etc/pocket-smoke.apk')
        write_manifest(jar,stage/'system/framework/pocket-zygote.classes')
    exports=re.findall(r'^\s*export\s+([A-Z0-9_]+)\s+(\S+)\s*$',(reference/'init.environ.rc').read_text(),re.MULTILINE)
    (stage/'probe-environment').write_text(''.join(key+'='+value+'\n' for key,value in exports))
    if args.system_server:
        with (stage/'probe-environment').open('a') as env:env.write('POCKET_SYSCALL_BACKEND=native\n')
    if args.ui:
        with (stage/'probe-environment').open('a') as env:env.write('POCKET_API29_UI_PROBE=1\n')
    (stage/'probe-binder-enabled').write_text('1' if args.binder else '0')
    if args.init:
        init_dir=stage/'system/etc/init';init_dir.mkdir(parents=True,exist_ok=True)
        (init_dir/'init.pocket.rc').write_text((ROOT/'runtime/api28/init.pocket.rc').read_text()
            .replace('Android 9','Android 10').replace('api28','api29'))
        shutil.copy2(ROOT/'runtime/api28/pocket-import.rc',init_dir/'pocket-import.rc')
        if args.zygote:
            with (init_dir/'init.pocket.rc').open('a') as rc:
                rc.write('''
on boot
    setprop dalvik.vm.heapstartsize 16m
    setprop dalvik.vm.heapsize 192m
    setprop dalvik.vm.jdwp-provider none
    setprop ro.dalvik.vm.native.bridge 0
    start pocket-log
    start pocket-zygote

service pocket-log /system/bin/pocket-log-writer-api29
    class zygote
    user 0
    group 0
    socket logdw dgram 0600 0 0

service pocket-zygote /system/bin/pocket-app-process-api29 -Xzygote -Xint -Xnorelocate -Xnoimage-dex2oat -Ximage:/system/framework/boot.art -Xms16m -Xmx192m /system/bin --zygote --abi-list=arm64-v8a --socket-name=zygote --enable-lazy-preload
    class zygote
    user 0
    group 0
    socket zygote stream 0600 0 0
    socket usap_pool_primary seqpacket 0600 0 0
''')
                rc.write('    setenv BOOTCLASSPATH '+environment+':/system/framework/pocket-zygote.jar\n')
        if args.system_server:
            rc=init_dir/'init.pocket.rc'
            text=rc.read_text().replace('    setprop ro.dalvik.vm.native.bridge 0\n','')
            # The platform defaults enable JIT; ART rejects that together with
            # the foundation fixture's interpreter-only command-line option.
            text=text.replace(' -Xint ',' ')
            text=text.replace('    start pocket-zygote\n','    start pocket-hardware\n    start pocket-data\n')
            text=text.replace('--enable-lazy-preload\n','--enable-lazy-preload start-system-server\n')
            text+='    setenv SYSTEMSERVERCLASSPATH /system/framework/services.jar:/system/framework/ethernet-service.jar:/system/framework/wifi-service.jar:/system/framework/com.android.location.provider.jar\n'
            text+='    setenv LD_PRELOAD /system/lib64/libpocket_binder_api29.so:/system/lib64/libpocket_hidl_api29.so\n'
            text+='''
on property:pocket.data.ready=1
    start pocket-storage
    start pocket-installd
    start pocket-suspend-control
    start pocket-apex
    start idmap2d
    start pocket-network
    start pocket-keystore
    start pocket-gatekeeper
    start pocket-audio
    start pocket-surfaceflinger

on property:pocket.api29.start=1
    start pocket-zygote

service pocket-data /system/bin/pocket-boot-probe-api29 prepare-data
    class hardware
    user 0
    group 0
    oneshot

service pocket-storage /system/bin/pocket-storage-services-api29
    class hardware
    user 0
    group 0
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api29.so

service pocket-suspend-control /system/bin/pocket-suspend-control-api29
    class hardware
    user 0
    group 0
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api29.so

service pocket-apex /system/bin/pocket-apex-service-api29
    class hardware
    user 0
    group 0
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api29.so

service idmap2d /system/bin/idmap2d
    class hardware
    user 1000
    group 1000
    oneshot
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api29.so

service pocket-network /system/bin/pocket-network-services-api29
    class hardware
    user 0
    group 0
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api29.so

service pocket-keystore /system/bin/keystore /data/misc/keystore
    class hardware
    user 1017
    group 1017
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api29.so:/system/lib64/libpocket_hidl_api29.so

service pocket-gatekeeper /system/bin/gatekeeperd /data/misc/gatekeeper
    class hardware
    user 1000
    group 1000
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api29.so:/system/lib64/libpocket_hidl_api29.so

service pocket-audio /system/bin/audioserver
    class hardware
    user 1013
    group 1013
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api29.so:/system/lib64/libpocket_hidl_api29.so

service pocket-surfaceflinger /system/bin/surfaceflinger
    class hardware
    user 1000
    group 1000
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api29.so:/system/lib64/libpocket_hidl_api29.so

service pocket-installd /system/bin/installd
    class hardware
    user 0
    group 0
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api29.so

service pocket-hardware /system/bin/pocket-hw-services-api29
    class hardware
    user 0
    group 0
    setenv LD_PRELOAD /system/lib64/libpocket_hidl_api29.so
'''
            if args.ui:
                text+='''
on property:sys.boot_completed=1
    start pocket-ui-check

service pocket-ui-check /system/bin/pocket-app-process-api29 -Xnorelocate -Xnoimage-dex2oat -Ximage:/system/framework/boot.art -Xms16m -Xmx96m /system/bin dev.kreijstal.aosppocket.guest.FrameworkProbe /system/etc/pocket-smoke.apk
    class hardware
    user 1000
    group 1000
    oneshot
'''
                text+='    setenv BOOTCLASSPATH '+environment+':/system/framework/pocket-zygote.jar\n'
                text+='    setenv LD_PRELOAD /system/lib64/libpocket_binder_api29.so:/system/lib64/libpocket_hidl_api29.so\n'
                text+='    setenv POCKET_GUEST_LOG /tmp/properties.log\n'
            rc.write_text(text)
    script='set -e\necho API29_SHELL_STARTED\n/system/bin/toybox grep -Fx ro.build.version.sdk=29 /system/build.prop\ntest -r /apex/com.android.runtime/lib64/bionic/libc.so\necho API29_APEX_LAYOUT_PASS\n'
    if args.properties:
        script+='''
: > /tmp/properties.log
: > /tmp/property-wait.log
/system/bin/pocket-property-service-api29 > /tmp/properties.log 2>&1 &
property_pid=$!
cleanup() {
    kill "$property_pid" 2>/dev/null || :
    wait "$property_pid" 2>/dev/null || :
    /system/bin/toybox cat /tmp/properties.log /tmp/property-wait.log
}
trap cleanup EXIT INT TERM
await_marker() {
    count=0
    until /system/bin/toybox grep -q "$1" "$2"; do
        kill -0 "$3" || return 1
        count=$((count+1))
        test "$count" -lt 200 || return 1
        /system/bin/toybox sleep 0.05
    done
}
await_marker API29_PROPERTIES_READY /tmp/properties.log "$property_pid"
/system/bin/pocket-property-probe-api29 fresh
/system/bin/pocket-property-probe-api29 wait > /tmp/property-wait.log 2>&1 &
wait_pid=$!
await_marker API29_PROPERTY_WAIT_READY /tmp/property-wait.log "$wait_pid"
/system/bin/pocket-property-probe-api29 write
wait "$wait_pid"
echo API29_PROPERTY_SERVICE_PASS
'''
        if args.init:
            script=script.replace('/system/bin/pocket-property-service-api29 >',
                '/system/bin/pocket-init-api29 /system/etc/init/init.pocket.rc >')
            script=script.replace('await_marker API29_PROPERTIES_READY','await_marker API29_INIT_READY')
    if args.art:script+='/apex/com.android.runtime/bin/dalvikvm64 -Xint -Xnorelocate -Xnoimage-dex2oat -Ximage:/system/framework/boot.art -cp /data/local/tmp/hello29.jar HelloAndroid10\n'
    if args.binder:
        script+='''
: > /tmp/binder-manager.log
/system/bin/pocket-binder-probe-api29 manager > /tmp/binder-manager.log 2>&1 &
manager_pid=$!
cleanup_binder() {
    kill "$manager_pid" 2>/dev/null || :
    wait "$manager_pid" 2>/dev/null || :
    /system/bin/toybox cat /tmp/binder-manager.log
    cleanup
}
trap cleanup_binder EXIT
await_marker API29_READY /tmp/binder-manager.log "$manager_pid"
/system/bin/pocket-binder-probe-api29 client
/system/bin/pocket-binder-probe-api29 client-virtual
echo API29_BINDER_PASS
'''
    if args.hardware:
        script+='''
kill "$manager_pid"
wait "$manager_pid" || :
: > /tmp/binder-manager.log
LD_PRELOAD=/system/lib64/libpocket_hidl_api29.so /system/bin/pocket-hw-services-api29 > /tmp/binder-manager.log 2>&1 &
manager_pid=$!
await_marker 'DIRECT_HWB_REGISTRY_READY status=0' /tmp/binder-manager.log "$manager_pid"
/system/bin/pocket-hardware-probe-api29
'''
    if args.init:script+='/system/bin/pocket-init-test-api29 client-native\n'
    if args.zygote:script+='/system/bin/pocket-zygote-client-api29\n'
    if args.properties:
        script+='/system/bin/pocket-property-probe-api29 stop\nwait "$property_pid"\n'
    if args.init:script+='/system/bin/pocket-init-test-api29 stopped\necho API29_INIT_PASS\n'
    if args.system_server:
        script='''set -e
: > /tmp/properties.log
/system/bin/pocket-init-api29 /system/etc/init/init.pocket.rc > /tmp/properties.log 2>&1 &
init_pid=$!
cleanup() {
    kill "$init_pid" 2>/dev/null || :
    wait "$init_pid" 2>/dev/null || :
    /system/bin/toybox cat /tmp/properties.log
}
trap cleanup EXIT INT TERM
count=0
until /system/bin/toybox grep -q API29_INIT_READY /tmp/properties.log; do
    kill -0 "$init_pid" || exit 1
    count=$((count+1));test "$count" -lt 200 || exit 1
    /system/bin/toybox sleep 0.05
done
/system/bin/pocket-gralloc-probe-api29
LD_PRELOAD=/system/lib64/libpocket_hidl_api29.so /system/bin/pocket-egl-probe-api29
LD_PRELOAD=/system/lib64/libpocket_binder_api29.so /system/bin/pocket-apex-service-api29 --check
LD_PRELOAD=/system/lib64/libpocket_binder_api29.so /system/bin/pocket-storage-services-api29 verify
LD_PRELOAD=/system/lib64/libpocket_binder_api29.so /system/bin/pocket-network-services-api29 verify
/system/bin/toybox setprop pocket.api29.start 1
LD_PRELOAD=/system/lib64/libpocket_binder_api29.so /system/bin/pocket-framework-probe-api29
echo API29_NATIVE_RUNTIME_PASS
'''
        if args.ui:
            script+='''
count=0
until /system/bin/toybox grep -q DIRECT_FRAMEWORK_SMOKE_PASS /tmp/properties.log; do
    if /system/bin/toybox grep -q DIRECT_FRAMEWORK_SMOKE_FAIL /tmp/properties.log; then exit 5; fi
    kill -0 "$init_pid" || exit 1
    count=$((count+1));test "$count" -lt 1000 || exit 6
    /system/bin/toybox sleep 0.2
done
echo API29_UI_SMOKE_PASS
'''
    script+='echo API29_NATIVE_RUNTIME_PASS\n';(stage/'probe.sh').write_text(script)
    image_files={path.relative_to(stage).as_posix():{'bytes':path.stat().st_size,'sha256':hashlib.file_digest(path.open('rb'),'sha256').hexdigest()}
                 for path in sorted(stage.rglob('*')) if path.is_file()}
    image_manifest={'schema':1,'sdk':29,'files':image_files}
    (out/'image-manifest.json').write_text(json.dumps(image_manifest,sort_keys=True,separators=(',',':'))+'\n')
    installed={}
    if args.installed_hashes:
        inventory=json.loads(args.installed_hashes.read_text())
        assert inventory['schema']==1 and inventory['package']==PACKAGE and inventory['guestSdk']==29
        installed=inventory['files']
        assert all(re.fullmatch('[0-9a-f]{64}',value) for value in installed.values())
    run('javac','--release','8','-cp',tool/'build/tools','-d',out/'tools',ROOT/'runtime/tools/PocketManifest.java')
    run('java','-cp',str(tool/'build/tools')+':'+str(out/'tools'),'PocketManifest',ROOT/'runtime/tests/api29/AndroidManifest.xml',out/'AndroidManifest.xml','28','28','1','0.1')
    run('java','-cp',tool/'build/tools','com.example.build.ApkBuilder',out/'AndroidManifest.xml',out/'dex/classes.dex',out/'unsigned.apk')
    with zipfile.ZipFile(out/'unsigned.apk','a',zipfile.ZIP_DEFLATED,compresslevel=1) as apk:
        bundled=0
        for path in sorted(stage.rglob('*')):
            if path.is_file():
                name=path.relative_to(stage).as_posix()
                if installed.get(name)==image_files[name]['sha256']:continue
                apk.write(path,'assets/root/'+name);bundled+=1
        apk.write(out/'image-manifest.json','assets/fixture-image.json')
        apk.write(ROOT/'out/runtime-native/libpocket_runtime.so','lib/arm64-v8a/libpocket_runtime.so')
        for path,name in [(ROOT/'LICENSE','LICENSE'),(ROOT/'runtime/NOTICE','NOTICE'),
                          (reference/'system/etc/NOTICE.xml.gz','android10-NOTICE.xml.gz')]:
            apk.write(path,'assets/licenses/'+name)
    signer=['java','-cp',tool/'build/apksigner.jar','com.android.apksigner.ApkSignerTool']
    target=out/'api29-probe.apk';run(*signer,'sign','--ks',ROOT/'out/debug.keystore','--ks-pass','pass:android','--key-pass','pass:android','--out',target,out/'unsigned.apk')
    run(*signer,'verify',target)
    (out/'build.json').write_text(json.dumps({'apk':str(target),'package':PACKAGE,'sdk':29,
        'stage':'system_server' if args.system_server else 'art' if args.art else 'shell',
        'properties':args.properties,'binder':args.binder,'init':args.init,'zygote':args.zygote,'hardware':args.hardware,
        'system_server':args.system_server,'ui':args.ui,'native_reused':args.reuse_native,'thin_update':bool(args.installed_hashes),
        'image_files':len(image_files),'bundled_image_files':bundled,
        'sha256':hashlib.sha256(target.read_bytes()).hexdigest(),'bytes':target.stat().st_size,
        'full_guest_boot_verified':False,'payloads':payloads},indent=2)+'\n')
    print('API29_PROBE_BUILD_PASS',target,target.stat().st_size)


if __name__=='__main__':main()
