#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build an isolated Android 12L loader/property fixture for the ARM64 Pi.

The native fixture runs as the ordinary Linux user first. Android app-sandbox
and Manager validation are later, separate gates; this builder claims neither.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import xml.etree.ElementTree as ET
import zipfile
from prepare_api32 import ROOT, CACHE, prepare, prepare_system_sdk
from build_api32_native import build_properties, build_binder, build_zygote, build_init
from build_api32_framework import build_hardware, build_apex, build_installd
from build_api32_tombstoned import build_tombstoned
from build_api32_perfetto import build_perfetto
from build_api32_timers import build_timers
from build_api32_runtime_tools import SOURCES as RUNTIME_TOOL_SOURCES
from build_api32_graphics import build_swiftshader, build_hal
from build_api32_input import build_input
from build_api32_network import build_network
from build_api32_mdns import build_mdns
from build_api32_audio import build_audio
from build_api32_uevent import build_uevent
from build_api32_storage import build_storage
from build_api32_vintf import build_vintf
from build_api32_keystore import build_keystore
from build_api32_memory import build_memory
from build_api32_guest_app import build_guest_app, build_guest_bridge
from build_api32_gatekeeper import build_gatekeeper
from api32_hardware_profile import apply_hardware_profile
from build_api32_memory_overlay import build_memory_overlay
from api32_runtime_boot import stage_runtime_boot,consolidate_core,CONSOLIDATED_COMPONENTS,CORE_SERVICES_LAYOUT
from build_api32_core import build_core
from build_api32_crypto import build_crypto
from build_api32_media import build as build_media,binaries as media_binaries
from build_api32_camera import build_camera
from build_api32_adbd import build as build_adbd
from build_api32_microg import build_jar as build_microg_jar
from build_boot import reserve_interpreter
from validate_native_loader import validate as validate_loader


def write_dex_jar(destination, dex):
    # Keep bootclasspath bytes stable when unrelated tools are rebuilt.
    entry=zipfile.ZipInfo('classes.dex',(1980,1,1,0,0,0))
    entry.create_system=3;entry.external_attr=0o100644 << 16
    entry.extra=struct.pack('<HH',0xffff,3)+bytes(3) # 4-byte aligned DEX payload.
    with zipfile.ZipFile(destination,'w') as archive:
        archive.writestr(entry,dex.read_bytes())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk', type=Path, required=True)
    parser.add_argument('--out', type=Path, default=CACHE / 'native-probe')
    parser.add_argument('--runtime-root', default='/home/pi/pocket-api32-test/root')
    parser.add_argument('--reuse-native', action='store_true')
    parser.add_argument('--art', action='store_true', help='Also run the real API 32 ART and verify an API 32 framework class')
    parser.add_argument('--binder', action='store_true', help='Android sandbox fixture only: private host Binder transport')
    parser.add_argument('--zygote', action='store_true', help='Also verify stock API 32 ZygoteConnection and child specialization')
    parser.add_argument('--init', action='store_true', help='Run the version-matched rootless init and managed services')
    parser.add_argument('--hardware', action='store_true', help='Validate the independent 12L HIDL endpoint and host relays')
    parser.add_argument('--apex', action='store_true', help='Validate the typed 12L flattened APEX inventory service')
    parser.add_argument('--installd', action='store_true', help='Validate stock 12L installd through its matching typed AIDL')
    parser.add_argument('--system-server', action='store_true', help='Attempt normal stock framework boot after the component checks')
    parser.add_argument('--graphics', action='store_true', help='Stage and validate the 12L software display profile')
    parser.add_argument('--input', action='store_true', help='Use the matching rootless 12L input reader and validate host wake protection')
    parser.add_argument('--network', action='store_true', help='Validate typed 12L netd and actual guest UID traffic rules')
    parser.add_argument('--dns', action='store_true', help='Validate typed 12L resolver and real host DNS through dnsproxyd')
    parser.add_argument('--mdns', action='store_true', help='Validate the stock 12L mDNS command frontend and responder')
    parser.add_argument('--audio', action='store_true', help='Validate stock 12L audio services and real host PCM output')
    parser.add_argument('--storage', action='store_true', help='Validate typed 12L internal user-storage lifecycle')
    parser.add_argument('--keystore', action='store_true', help='Validate stock 12L keystore2 and software Keymaster')
    parser.add_argument('--apps', action='store_true', help='Install and restart an ordinary SDK 32 app; verify Settings, keystore and HTTPS')
    parser.add_argument('--display', action='store_true', help='Present actual guest frames on a host surface and verify a host tap')
    parser.add_argument('--compile-ui', action='store_true', help='Prepare stock SystemUI speed compilation through the guest PackageManager')
    parser.add_argument('--trace-startup', action='store_true', help='Diagnostic only: request repeated ART SIGQUIT dumps during startup')
    parser.add_argument('--observe-startup-code', action='store_true', help='Diagnostic only: bounded SystemUI executable-code/scheduling snapshots')
    parser.add_argument('--runtime-boot', action='store_true', help='Validate normal service startup without the component-check sequence')
    parser.add_argument('--core-services', action='store_true', help='Share the UID-0 native services in one process')
    parser.add_argument('--renderer-worker-scheduling',action='store_true',help='Test normal priority for software-compositor workers')
    args = parser.parse_args()
    if args.renderer_worker_scheduling and not args.graphics:parser.error('--renderer-worker-scheduling requires --graphics')
    if args.core_services and not all((args.binder,args.init,args.hardware,args.keystore,args.network,args.dns,args.mdns,args.apex,args.storage)):
        parser.error('--core-services requires --binder --init --hardware --keystore --network --dns --mdns --apex --storage')
    if args.runtime_boot and (not all((args.binder,args.zygote,args.init,args.hardware,args.apex,args.installd,
            args.graphics,args.input,args.network,args.dns,args.mdns,args.audio,args.storage,args.keystore,
            args.system_server,args.apps,args.display,args.compile_ui)) or args.trace_startup or args.observe_startup_code):
        parser.error('--runtime-boot requires all runtime/app/display options and startup diagnostics disabled')
    if args.system_server and not all((args.binder,args.zygote,args.init,args.hardware,args.apex,args.installd)):
        parser.error('--system-server requires --binder --zygote --init --hardware --apex --installd')
    if args.apps and not all((args.system_server,args.graphics,args.network,args.dns,args.keystore)):
        parser.error('--apps requires --system-server --graphics --network --dns --keystore')
    if args.display and not args.apps:parser.error('--display requires --apps')
    if args.compile_ui and not args.system_server:parser.error('--compile-ui requires --system-server')
    if args.trace_startup and not args.system_server:parser.error('--trace-startup requires --system-server')
    if args.observe_startup_code and not args.system_server:parser.error('--observe-startup-code requires --system-server')
    if args.graphics and not all((args.binder,args.init,args.hardware)):
        parser.error('--graphics requires --binder --init --hardware')
    if args.input and not all((args.binder,args.init,args.hardware)):
        parser.error('--input requires --binder --init --hardware')
    if args.network and not all((args.binder,args.init)):
        parser.error('--network requires --binder --init')
    if args.dns and not args.network:
        parser.error('--dns requires --network')
    if args.mdns and not (args.init and args.network):
        parser.error('--mdns requires --init --network')
    if args.audio and not all((args.binder,args.init,args.hardware)):
        parser.error('--audio requires --binder --init --hardware')
    if args.audio and not args.system_server:
        parser.error('--audio requires --system-server for AudioPolicyService UID observers')
    if args.storage and not all((args.binder,args.init,args.apex,args.installd)):
        parser.error('--storage requires --binder --init --apex --installd')
    if args.keystore and not all((args.binder,args.init,args.hardware,args.zygote)):
        parser.error('--keystore requires --binder --init --hardware --zygote')
    if args.init:
        args.art = True
    if args.hardware and not (args.init and args.binder):
        parser.error('--hardware requires --init --binder in the Android sandbox')
    if args.apex and not (args.init and args.binder):
        parser.error('--apex requires --init --binder in the Android sandbox')
    if args.installd and not (args.init and args.binder):
        parser.error('--installd requires --init --binder in the Android sandbox')
    if args.zygote:
        args.art = True
        if not args.binder:
            parser.error('--zygote requires the Android sandbox fixture with --binder')
    if not re.fullmatch(r'/[A-Za-z0-9_./-]+', args.runtime_root) or '..' in args.runtime_root.split('/'):
        parser.error('Invalid isolated runtime path')
    reference, out = prepare(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    stage = out / 'root'
    if stage.exists():
        if not (stage / '.pocket-api32-fixture').is_file():
            parser.error('Output contains an unrelated root directory')
        shutil.rmtree(stage)
    stage.mkdir()
    (stage / '.pocket-api32-fixture').write_text('native foundation fixture\n')
    (stage / '.pocket-readonly-system').write_text('immutable Android platform partitions\n')
    native = CACHE / 'native' if args.reuse_native else build_properties(args.ndk.resolve())
    if args.init and not args.reuse_native:
        build_init(args.ndk.resolve())
    compiler = args.ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang'
    loader = out / 'pocket-loader'
    subprocess.run(list(map(str, [compiler, '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-mno-outline-atomics',
        '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-pic', '-nostdlib', '-static',
        '-Wl,-T,runtime/loader/loader.ld,-z,max-page-size=16384', ROOT / 'runtime/loader/loader.c',
        ROOT / 'runtime/loader/entry.S', '-o', loader])), cwd=ROOT, check=True)
    validate_loader(loader)
    data = bytearray(loader.read_bytes())
    marker = b'PCKELF01' + bytes(3072)
    if data.count(marker) != 1:
        raise ValueError('Loader config marker is not unique')
    offset = data.index(marker) + 8
    for name in [args.runtime_root, args.runtime_root + '/system/bin/pocket-loader', args.runtime_root + '/system/bin/linker64']:
        encoded = name.encode() + b'\0'
        if len(encoded) > 1024:
            raise ValueError('Fixture root is too long')
        data[offset:offset + 1024] = encoded.ljust(1024, b'\0')
        offset += 1024
    loader.write_bytes(data)
    generated = {'system/bin/' + name: native / name for name in ['pocket-property-service-api32', 'pocket-property-probe-api32']}
    generated['system/bin/pocket-loader'] = loader
    executables = {'system/bin/sh', 'system/bin/toybox', *generated}
    signal_probe=out/'pocket-signal-trap-api32'
    subprocess.run([str(compiler),'-O2','-Wall','-Wextra','-Werror',
        str(ROOT/'runtime/tests/loader/trap_signal.c'),'-Wl,-z,max-page-size=16384','-o',str(signal_probe)],check=True)
    generated['system/bin/pocket-signal-trap-api32']=signal_probe
    executables.add('system/bin/pocket-signal-trap-api32')
    readonly_probe=out/'pocket-readonly-api32'
    subprocess.run([str(compiler),'-O2','-Wall','-Wextra','-Werror',
        str(ROOT/'runtime/tests/loader/readonly_view.c'),'-Wl,-z,max-page-size=16384','-o',str(readonly_probe)],check=True)
    generated['system/bin/pocket-readonly-api32']=readonly_probe
    executables.add('system/bin/pocket-readonly-api32')
    if args.init:
        for name in ['pocket-init-api32', 'pocket-init-test-api32']:
            generated['system/bin/' + name] = native / name
            executables.add('system/bin/' + name)
    if args.binder:
        if not args.reuse_native:
            build_binder(args.ndk.resolve())
        generated['system/bin/pocket-binder-probe-api32'] = native / 'pocket-binder-probe-api32'
        executables.add('system/bin/pocket-binder-probe-api32')
        if args.runtime_boot or args.core_services:
            generated['system/bin/pocket-servicemanager-api32']=native/'pocket-servicemanager-api32'
            executables.add('system/bin/pocket-servicemanager-api32')
            generated['system/bin/pocket-setprop-api32']=native/'pocket-setprop-api32'
            executables.add('system/bin/pocket-setprop-api32')
    if args.zygote:
        if not args.reuse_native:
            build_zygote(args.ndk.resolve())
        for name in ['pocket-app-process-api32', 'pocket-zygote-client-api32', 'pocket-zygote-launcher-api32']:
            generated['system/bin/' + name] = native / name
            executables.add('system/bin/' + name)
        generated['system/lib64/libpocket_zygote_api32.so'] = native / 'libpocket_zygote_api32.so'
    if args.hardware:
        hardware = CACHE / 'framework' if args.reuse_native else build_hardware(args.ndk.resolve())
        for name in ['pocket-hw-services-api32', 'pocket-hardware-probe-api32', 'pocket-suspend-probe-api32']:
            generated['system/bin/' + name] = hardware / name
            executables.add('system/bin/' + name)
        generated['system/lib64/libpocket_hidl_api32.so'] = hardware / 'libpocket_hidl_api32.so'
    if args.apex:
        apex = CACHE / 'framework' if args.reuse_native else build_apex(args.ndk.resolve())
        generated['system/bin/pocket-apex-service-api32'] = apex / 'pocket-apex-service-api32'
        executables.add('system/bin/pocket-apex-service-api32')
    if args.installd:
        install = CACHE / 'framework' if args.reuse_native else build_installd(args.ndk.resolve())
        for name in ['pocket-installd-probe-api32', 'pocket-boot-probe-api32']:
            generated['system/bin/' + name] = install / name
            executables.add('system/bin/' + name)
        executables.add('system/bin/installd')
        executables.update('apex/com.android.art/bin/' + name for name in ['dex2oat64', 'profman', 'dexoptanalyzer'])
        generated['system/lib64/libpocket_policy_api32.so'] = install / 'libpocket_policy_api32.so'
        if args.system_server:
            generated['system/bin/pocket-framework-probe-api32'] = install / 'pocket-framework-probe-api32'
            executables.add('system/bin/pocket-framework-probe-api32')
    if args.graphics:
        generated['system/lib64/libpocket_renderer_workers_api32.so']=CACHE/'graphics/libpocket_renderer_workers_api32.so'
        if not args.reuse_native:
            build_swiftshader(args.ndk.resolve(),12);build_hal(args.ndk.resolve())
        graphics=CACHE/'graphics'
        generated['system/lib64/libpocket_graphics_api32.so']=graphics/'libpocket_graphics_api32.so'
        for name in ['gralloc.pocket.so','hwcomposer.pocket.so']:
            generated['vendor/lib64/hw/'+name]=graphics/name
        for name in ['libEGL_swiftshader.so','libGLESv1_CM_swiftshader.so','libGLESv2_swiftshader.so']:
            generated['vendor/lib64/egl/'+name]=graphics/name
        for name in ['pocket-graphics-probe-api32','pocket-egl-probe-api32']:
            generated['system/bin/'+name]=graphics/name;executables.add('system/bin/'+name)
        if args.system_server:executables.add('system/bin/surfaceflinger')
    if args.system_server:
        executables.update(['system/bin/idmap2','system/bin/idmap2d','system/bin/tombstoned','system/bin/cameraserver',
                            'system/bin/perfetto','system/bin/traced',
                            'system/bin/abb','system/bin/cmd','system/bin/dumpsys',
                            'apex/com.android.runtime/bin/crash_dump64'])
        perfetto=CACHE/'perfetto' if args.reuse_native else build_perfetto(args.ndk.resolve())
        generated['system/lib64/libpocket_perfetto_api32.so']=perfetto/'libpocket_perfetto_api32.so'
        generated['system/bin/pocket-perfetto-probe-api32']=perfetto/'pocket-perfetto-probe-api32'
        executables.add('system/bin/pocket-perfetto-probe-api32')
        tombstoned=CACHE/'tombstoned' if args.reuse_native else build_tombstoned(args.ndk.resolve())
        generated['system/lib64/libpocket_tombstoned_api32.so']=tombstoned/'libpocket_tombstoned_api32.so'
        generated['system/bin/pocket-tombstoned-probe-api32']=tombstoned/'pocket-tombstoned-probe-api32'
        executables.add('system/bin/pocket-tombstoned-probe-api32')
        timers=CACHE/'timers' if args.reuse_native else build_timers(args.ndk.resolve())
        generated['system/lib64/libpocket_service_timers_api32.so']=timers/'libpocket_service_timers_api32.so'
        generated['system/bin/pocket-service-timer-probe-api32']=timers/'pocket-service-timer-probe-api32'
        executables.add('system/bin/pocket-service-timer-probe-api32')
        generated['system/lib64/libpocket_framework_policy_api32.so']=install/'libpocket_framework_policy_api32.so'
        generated['system/lib64/libpocket_trace_api32.so']=native/'libpocket_trace_api32.so'
        uevent=CACHE/'uevent' if args.reuse_native else build_uevent(args.ndk.resolve())
        generated['system/lib64/libpocket_uevent_api32.so']=uevent/'libpocket_uevent_api32.so'
        generated['system/bin/pocket-uevent-probe-api32']=uevent/'pocket-uevent-probe-api32'
        executables.add('system/bin/pocket-uevent-probe-api32')
        vintf=CACHE/'vintf' if args.reuse_native else build_vintf(args.ndk.resolve())
        generated['system/lib64/libpocket_vintf_api32.so']=vintf/'libpocket_vintf_api32.so'
        generated['system/bin/pocket-vintf-probe-api32']=vintf/'pocket-vintf-probe-api32'
        executables.add('system/bin/pocket-vintf-probe-api32')
    if args.storage:
        storage=CACHE/'storage' if args.reuse_native else build_storage(args.ndk.resolve())
        for name in ['pocket-storage-api32','pocket-storage-probe-api32']:
            generated['system/bin/'+name]=storage/name;executables.add('system/bin/'+name)
    if args.keystore:
        keystore=CACHE/'keystore' if args.reuse_native else build_keystore(args.ndk.resolve())
        generated['system/lib64/libpocket_keystore_api32.so']=keystore/'libpocket_keystore_api32.so'
        generated['system/bin/pocket-keystore-probe-api32']=keystore/'pocket-keystore-probe-api32'
        # Matching service entry uses the immutable SDK software factory in the
        # platform namespace, with startup diagnostics and no crypto replacement.
        generated['system/bin/pocket-keymaster-api32']=keystore/'pocket-keymaster-api32'
        executables.update(['system/bin/keystore2','system/bin/pocket-keymaster-api32','system/bin/pocket-keystore-probe-api32'])
        gatekeeper=CACHE/'gatekeeper' if args.reuse_native else build_gatekeeper(args.ndk.resolve())
        generated['system/lib64/libpocket_gatekeeper_pool_api32.so']=gatekeeper/'libpocket_gatekeeper_pool_api32.so'
        generated['system/bin/pocket-gatekeeper-probe-api32']=gatekeeper/'pocket-gatekeeper-probe-api32'
        generated['system/bin/pocket-gatekeeper-hal-api32']=reference/'vendor/bin/hw/android.hardware.gatekeeper@1.0-service.software'
        executables.update(['system/bin/gatekeeperd','system/bin/pocket-gatekeeper-hal-api32','system/bin/pocket-gatekeeper-probe-api32'])
    if args.input:
        input_runtime=CACHE/'input' if args.reuse_native else build_input(args.ndk.resolve())
        generated['system/lib64/libinputreader.so']=input_runtime/'libinputreader.so'
        generated['system/bin/pocket-input-probe-api32']=input_runtime/'pocket-input-probe-api32'
        executables.add('system/bin/pocket-input-probe-api32')
    if args.network:
        network=CACHE/'network' if args.reuse_native else build_network(args.ndk.resolve(),args.dns)
        for name in ['pocket-netd-api32','pocket-network-probe-api32']:
            generated['system/bin/'+name]=network/name
            executables.add('system/bin/'+name)
        memory=CACHE/'memory' if args.reuse_native else build_memory(args.ndk.resolve())
        for name in ['pocket-memory-api32','pocket-memory-probe-api32']:
            generated['system/bin/'+name]=memory/name
            executables.add('system/bin/'+name)
        if args.dns:
            for name in ['pocket-dns-api32','pocket-dns-probe-api32']:
                generated['system/bin/'+name]=network/name
                executables.add('system/bin/'+name)
    if args.art:
        # The system entry uses the stock system linker namespace, which exports
        # com_android_art to libnativeloader for boot-classpath JNI libraries.
        executables.update(['system/bin/linkerconfig', 'apex/com.android.sdkext/bin/derive_classpath', 'system/bin/dalvikvm'])
    if args.mdns:
        mdns=CACHE/'mdns' if args.reuse_native else build_mdns(args.ndk.resolve())
        for name in ['pocket-mdns-control-api32','pocket-mdns-probe-api32']:
            generated['system/bin/'+name]=mdns/name;executables.add('system/bin/'+name)
        executables.add('system/bin/mdnsd')
    if args.audio:
        audio=CACHE/'audio' if args.reuse_native else build_audio(args.ndk.resolve())
        generated['system/lib64/libpocket_audio_api32.so']=audio/'libpocket_audio_api32.so'
        generated['vendor/lib64/hw/audio.primary.pocket.so']=audio/'audio.primary.pocket.so'
        generated['system/bin/pocket-audio-probe-api32']=audio/'pocket-audio-probe-api32'
        executables.update(['system/bin/audioserver','system/bin/mediametrics','system/bin/pocket-audio-probe-api32'])
        for name in ['android.hardware.audio.legacy@7.0-impl.ranchu.so','android.hardware.audio.effect@7.0-impl.so']:
            generated['system/lib64/'+name]=reference/'vendor/lib64/hw'/name
        for name in ['libaudiohal@7.0.so','libaudiopolicyenginedefault.so']:
            generated['system/lib64/'+name]=reference/'system/lib64'/name
    if args.core_services:
        core=CACHE/'core' if args.reuse_native else build_core(args.ndk.resolve(),reuse_generated=True)
        for name in ['pocket-core-api32','pocket-core-test-api32']:
            generated['system/bin/'+name]=core/name;executables.add('system/bin/'+name)
        crypto=CACHE/'crypto' if args.reuse_native else build_crypto(args.ndk.resolve(),reuse_generated=True)
        for name in ['pocket-crypto-api32','pocket-scrypt-probe-api32']:
            generated['system/bin/'+name]=crypto/name;executables.add('system/bin/'+name)
    runtime_services=args.runtime_boot or args.core_services
    if runtime_services:
        generated['system/lib64/libpocket_media_api32.so']=build_media(args.ndk.resolve())
        generated.update(media_binaries(reference));executables.update(media_binaries(reference))
        generated['system/lib64/libpocket_camera_api32.so']=build_camera(args.ndk.resolve())
        generated['system/lib64/libpocket_adbd_api32.so']=build_adbd(args.ndk.resolve())
        generated['system/bin/adbd']=reference/'apex/com.android.adbd/bin/adbd';executables.add('system/bin/adbd')
    queue = [(name, generated.get(name, reference / name)) for name in executables if name != 'system/bin/pocket-loader']
    queue += [(name, path) for name, path in generated.items() if name not in executables]
    queue.append(('system/bin/linker64', reference / 'apex/com.android.runtime/bin/linker64'))
    queue.append(('apex/com.android.runtime/bin/linker64', reference / 'apex/com.android.runtime/bin/linker64'))
    copied = {}
    def copy(source, name):
        if name in copied:
            return
        target = stage / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        copied[name] = str(source)
    if args.core_services:
        copy(CACHE/'scrypt/NOTICE','system/etc/pocket-notices/scrypt.NOTICE')
    if runtime_services:copy(build_microg_jar(out/'microg'),'system/framework/pocket-microg.jar')
    if args.installd:
        copy(ROOT / 'out/runtime/guest-test.apk', 'system/etc/pocket-api32-smoke.apk')
    if args.graphics:
        copy(CACHE/'swiftshader/LICENSE.txt','system/etc/pocket-licenses/swiftshader.txt')
        copy(ROOT/'runtime/api32/licenses/LLVM-10.txt','system/etc/pocket-licenses/llvm.txt')
        copy(CACHE/'swiftshader/third_party/llvm-10.0/llvm/include/llvm/Support/LICENSE.TXT','system/etc/pocket-licenses/llvm-system.txt')
        copy(CACHE/'swiftshader/third_party/astc-encoder/LICENSE.txt','system/etc/pocket-licenses/astc-encoder.txt')
        copy(args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/NOTICE','system/etc/pocket-licenses/ndk.txt')
    profile_inputs=[]
    if args.system_server:profile_inputs.append(ROOT/'runtime/api32/memory.prop')
    if args.graphics:profile_inputs.append(ROOT/'runtime/api32/software-graphics.prop')
    if args.input:profile_inputs.append(ROOT/'runtime/api32/input.prop')
    if args.audio:profile_inputs.append(ROOT/'runtime/api32/audio.prop')
    if args.keystore:profile_inputs.append(ROOT/'runtime/api32/keystore.prop')
    if runtime_services:profile_inputs += [ROOT/'runtime/api32/media.prop',ROOT/'runtime/api32/adbd.prop']
    if profile_inputs:
        name='system/etc/pocket-profile.prop';target=stage/name;target.parent.mkdir(parents=True,exist_ok=True)
        target.write_text('\n'.join(path.read_text() for path in profile_inputs))
        copied[name]='+'.join(map(str,profile_inputs))
    def library(name, origin):
        if 'system/lib64/'+name in generated:
            return generated['system/lib64/'+name]
        if name == 'libpocket_binder_api32.so':
            return native / name
        if name == 'libpocket_hidl_api32.so':
            return hardware / name
        directories = [reference / 'apex/com.android.runtime/lib64/bionic']
        if origin.is_relative_to(reference / 'apex'):
            directories.append(reference / 'apex' / origin.relative_to(reference / 'apex').parts[0] / 'lib64')
        directories.append(reference / 'system/lib64')
        directories += sorted((reference / 'apex').glob('*/lib64'))
        if args.audio or args.keystore:directories.append(reference/'vendor/lib64')
        for directory in directories:
            if (directory / name).is_file():
                return directory / name
        raise ValueError('Missing Android 12L dependency ' + name + ' from ' + str(origin))
    if args.art:
        # APEX-native libraries retain their module paths and private copies.
        # The stock API 32 linkerconfig below generates the actual namespaces.
        for path in (reference / 'apex').rglob('*'):
            if not path.is_file():
                continue
            name = path.relative_to(reference).as_posix()
            with path.open('rb') as stream:
                magic = stream.read(4)
            if path.suffix == '.so':
                queue.append((name, path))
            elif (magic != b'\x7fELF' or '/javalib/' in name) and path.suffix not in ('.apk', '.apex', '.capex'):
                copy(path, name)
        for path in (reference / 'system/framework').glob('*.jar'):
            copy(path, path.relative_to(reference).as_posix())
        # The platform boot extension and preoptimized framework services must
        # accompany those exact JARs. Shipping only the core ART APEX images
        # leaves framework startup interpreted despite having stock artifacts.
        for path in (reference / 'system/framework/arm64').glob('boot-*'):
            if path.is_file() and path.suffix in ('.art', '.oat', '.vdex'):
                copy(path, path.relative_to(reference).as_posix())
        for path in (reference / 'system/framework/oat/arm64').glob('*'):
            if path.is_file() and path.suffix in ('.art', '.odex', '.vdex') and (reference / 'system/framework' / (path.stem+'.jar')).is_file():
                copy(path, path.relative_to(reference).as_posix())
        for path in (reference / 'system/etc').rglob('*'):
            if path.is_file():
                copy(path, path.relative_to(reference).as_posix())
        for path in (reference / 'vendor/etc').rglob('*'):
            if path.is_file():
                copy(path, path.relative_to(reference).as_posix())
        for line in (reference / 'system/etc/public.libraries.txt').read_text().splitlines():
            parts = line.split('#', 1)[0].split()
            if parts and '32' not in parts[1:] and 'nopreload' not in parts[1:]:
                path = library(parts[0], reference / 'system/lib64/platform')
                queue.append((path.relative_to(reference).as_posix(), path))
        for name in ['libartpalette-system.so', 'libandroid_runtime.so']:
            path = library(name, reference / 'system/lib64/platform')
            queue.append((path.relative_to(reference).as_posix(), path))
        if args.system_server:
            queue.append(('system/lib64/libandroid_servers.so', reference / 'system/lib64/libandroid_servers.so'))
            # Framework/app JNI libraries are loaded by name at runtime, outside
            # the DT_NEEDED closure of app_process and system_server.
            queue += [(path.relative_to(reference).as_posix(),path)
                      for path in (reference/'system/lib64').glob('*.so')]
            # Preserve canonical partition locations, rather than copying the
            # reference extractor's materialized aliases below /system twice.
            for partition in ['system','system_ext','product','vendor']:
                for directory in ['app','priv-app','overlay']:
                    for path in (reference/partition/directory).rglob('*'):
                        if path.is_file():
                            name=path.relative_to(reference).as_posix()
                            if path.suffix=='.so':queue.append((name,path))
                            else:copy(path,name)
            for path in (reference/'apex').rglob('*.apk'):
                copy(path,path.relative_to(reference).as_posix())
            for partition in ['system_ext','product']:
                for directory in ['etc','framework']:
                    for path in (reference / partition / directory).rglob('*'):
                        if path.is_file():
                            copy(path,path.relative_to(reference).as_posix())
    if args.zygote:
        copy(reference / 'system/framework/framework-res.apk', 'system/framework/framework-res.apk')
        for path in (reference / 'system/fonts').rglob('*'):
            if path.is_file():
                copy(path, path.relative_to(reference).as_posix())
    if args.input:
        for path in (reference/'system/usr').rglob('*'):
            if path.is_file():copy(path,path.relative_to(reference).as_posix())
    while queue:
        name, source = queue.pop()
        source=generated.get(name,source)
        if name in copied:
            continue
        copy(source, name)
        text = subprocess.check_output(['readelf', '-dW', str(source)], text=True)
        for dependency in re.findall(r'Shared library: \[([^]]+)\]', text):
            path = library(dependency, source)
            name=path.relative_to(reference).as_posix() if path.is_relative_to(reference) else 'system/lib64/'+dependency
            if (args.audio or args.keystore) and name.startswith('vendor/lib64/'):
                name='system/lib64/'+dependency
            queue.append((name,path))
            if dependency in ('libc.so', 'libm.so', 'libdl.so', 'libdl_android.so'):
                copy(path, 'system/lib64/' + dependency)
    copy(loader, 'system/bin/pocket-loader')
    if args.system_server:
        for name in ['am','input','pm','settings','wm']:copy(reference/('system/bin/'+name),'system/bin/'+name)
        if runtime_services:
            copy(ROOT/'runtime/api32/camera-permissions.xml','system/etc/permissions/pocket-camera.xml')
            copy(ROOT/'runtime/camera/api32/external_camera_config.xml','vendor/etc/external_camera_config.xml')
            copy(ROOT/'runtime/api28/microg-google-cert.hex','system/etc/pocket-microg/google-cert.hex')
            copy(ROOT/'runtime/api32/microg/pocket-microg.xml','system/etc/sysconfig/pocket-microg.xml')
        for name in apply_hardware_profile(stage):
            copied[name] += ' + API32 software hardware profile'
        copied['system/etc/pocket-hardware-capabilities.json']=str(ROOT/'runtime/api32/software-hardware.json')
        copy(build_memory_overlay(reference),'product/overlay/PocketMemory/PocketMemory.apk')
    if args.audio:
        for name in ['audio_policy_configuration.xml','audio_effects.xml']:
            source=ROOT/'runtime/api32'/name
            shutil.copy2(source,stage/'vendor/etc'/name)
            copied['vendor/etc/'+name]=str(source)
    for name in ['system/build.prop', 'system/etc/prop.default', 'vendor/default.prop', 'vendor/build.prop',
                 'system_ext/etc/build.prop', 'system_ext/build.prop', 'product/etc/build.prop', 'product/build.prop']:
        if (reference / name).is_file():
            copy(reference / name, name)
    for directory in ['.pocket-owner', 'dev/__properties__', 'dev/socket', 'tmp', 'data/local/tmp',
                      'data/dalvik-cache/arm64', 'data/resource-cache', 'linkerconfig', 'system/etc']:
        (stage / directory).mkdir(parents=True, exist_ok=True)
    search = '/apex/com.android.runtime/lib64/bionic:/system/lib64'
    bootstrap = 'dir.pocket = /system/bin\ndir.pocket += /apex/com.android.sdkext/bin\ndir.pocket += /apex/com.android.art/bin\n[pocket]\nnamespace.default.isolated = false\nnamespace.default.search.paths = ' + search + '\nnamespace.default.permitted.paths = /\n'
    (stage / 'bootstrap-linkerconfig').write_text(bootstrap)
    (stage / 'linkerconfig/ld.config.txt').write_text(bootstrap)
    (stage / 'loader-signal-fixture.txt').write_text('loader-fixture\n')
    (stage / 'system/etc/pocket-readonly-fixture.txt').write_text('readonly-fixture\n')
    script = '''set -eu
echo API32_SHELL_STARTED
/system/bin/toybox grep -Fx ro.build.version.sdk=32 /system/build.prop
/system/bin/pocket-signal-trap-api32 /loader-signal-fixture.txt
/system/bin/pocket-readonly-api32
: > /tmp/properties.log
: > /tmp/property-wait.log
/system/bin/pocket-property-service-api32 > /tmp/properties.log 2>&1 &
property_pid=$!
manager_pid=''
zygote_pid=''
cleanup() {
    if test -n "$zygote_pid"; then
        kill "$zygote_pid" 2>/dev/null || :
        wait "$zygote_pid" 2>/dev/null || :
        /system/bin/toybox cat /tmp/zygote.log
    fi
    if test -n "$manager_pid"; then
        kill "$manager_pid" 2>/dev/null || :
        wait "$manager_pid" 2>/dev/null || :
        /system/bin/toybox cat /tmp/binder.log
    fi
    if test -n "$property_pid"; then
        kill "$property_pid" 2>/dev/null || :
        wait "$property_pid" 2>/dev/null || :
    fi
    /system/bin/toybox cat /tmp/properties.log /tmp/property-wait.log
}
trap cleanup EXIT INT TERM
await_marker() {
    count=0
    until /system/bin/toybox grep -q "$1" "$2"; do
        kill -0 "$3" || return 1
        count=$((count+1)); test "$count" -lt 200 || return 1
        /system/bin/toybox sleep 0.05
    done
}
await_marker API32_PROPERTIES_READY /tmp/properties.log "$property_pid"
/system/bin/pocket-property-probe-api32 fresh
/system/bin/pocket-property-probe-api32 wait > /tmp/property-wait.log 2>&1 &
wait_pid=$!
await_marker API32_PROPERTY_WAIT_READY /tmp/property-wait.log "$wait_pid"
/system/bin/pocket-property-probe-api32 write
wait "$wait_pid"
'''
    if args.art:
        # This SDK ART manifest exports native libraries but has no jniLibs.
        # Its libnativeloader still resolves boot JNI through com_android_art.
        # The AOSP LinkerConfig protobuf's field 2 explicitly forces visibility;
        # keep all stock search paths/isolation and change only that export.
        art_config = stage / 'apex/com.android.art/etc/linker.config.pb'
        art_config.write_bytes(art_config.read_bytes() + b'\x10\x01')
        info = ET.Element('apex-info-list')
        modules = json.loads((reference / '.pocket-api32-reference.json').read_text())['apexModules']
        for module in modules:
            packages = [path for path in (reference / 'system/apex').iterdir()
                        if path.stem == module['name'] or
                        (path.stem == 'com.android.vndk.current' and module['name'].startswith('com.android.vndk.'))]
            if len(packages) != 1:
                raise ValueError('Missing unique preinstalled APEX package: ' + module['name'])
            ET.SubElement(info, 'apex-info', {'moduleName': module['name'], 'modulePath': '/apex/' + module['name'],
                'preinstalledModulePath': '/' + packages[0].relative_to(reference).as_posix(), 'versionCode': str(module['version']),
                'versionName': module['versionName'], 'isFactory': 'true', 'isActive': 'true',
                'lastUpdateMillis': '0', 'provideSharedApexLibs': 'false'})
        ET.ElementTree(info).write(stage / 'apex/apex-info-list.xml', encoding='utf-8', xml_declaration=True)
        classes, dex = out / 'hello-classes', out / 'hello-dex'
        for directory in [classes, dex]:
            if directory.exists():
                shutil.rmtree(directory)
            directory.mkdir()
        android = ROOT.parent / 'helloworldandroid/vendor/android-platforms/android-36/android.jar'
        r8 = ROOT.parent / 's20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar'
        subprocess.run(['javac', '--release', '8', '-d', str(classes), str(ROOT / 'runtime/tests/api32/HelloAndroid12L.java')], check=True)
        subprocess.run(['jar', 'cf', str(out / 'hello.jar'), '-C', str(classes), '.'], check=True)
        subprocess.run(['java', '-cp', str(r8), 'com.android.tools.r8.D8', '--lib', str(android), '--min-api', '32', '--output', str(dex), str(out / 'hello.jar')], check=True)
        write_dex_jar(stage/'data/local/tmp/hello32.jar',dex/'classes.dex')
        script += '''
/system/bin/linkerconfig --target /linkerconfig
test -s /linkerconfig/ld.config.txt
echo API32_LINKER_CONFIG_GENERATED
# The bootstrap override must not leak across the stock APEX namespaces.
# Android's linkerconfig supplies the correct Bionic/system namespace links.
unset LD_LIBRARY_PATH
/apex/com.android.sdkext/bin/derive_classpath /tmp/classpath
while read -r directive name value; do
    test "$directive" = export
    case "$name" in
        BOOTCLASSPATH|DEX2OATBOOTCLASSPATH|SYSTEMSERVERCLASSPATH) export "$name=$value" ;;
        *) exit 1 ;;
    esac
done < /tmp/classpath
export ANDROID_ART_ROOT=/apex/com.android.art
export ANDROID_I18N_ROOT=/apex/com.android.i18n
export ANDROID_TZDATA_ROOT=/apex/com.android.tzdata
/system/bin/dalvikvm -Xint -Xnorelocate -Xnoimage-dex2oat -Ximage:/apex/com.android.art/javalib/boot.art:/system/framework/boot-framework.art -cp /data/local/tmp/hello32.jar HelloAndroid12L
'''
    if args.init:
        config = (ROOT / 'runtime/api32/init.pocket.rc').read_text()
        init_dir = stage / 'system/etc/init'
        init_dir.mkdir(exist_ok=True)
        (init_dir / 'pocket-import.rc').write_text('on early-init\n    setprop pocket.test.import loaded\n')
        if args.binder:
            config += '''
service pocket-registry /system/bin/pocket-binder-probe-api32 manager
    disabled
    user 0
    group 0
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api32.so
'''
        if args.zygote:
            config += '''
service pocket-zygote /system/bin/pocket-app-process-api32 -Xzygote -Xnorelocate -Xnoimage-dex2oat -Ximage:/apex/com.android.art/javalib/boot.art:/system/framework/boot-framework.art -Xms16m -Xmx192m /system/bin --zygote --socket-name=zygote --enable-lazy-preload
    disabled
    user 0
    group 0
    socket zygote stream 0660 0 0
    socket usap_pool_primary stream 0660 0 0
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api32.so
'''
        if args.hardware:
            config += '''
service pocket-hardware /system/bin/pocket-hw-services-api32
    disabled
    user 0
    group 0
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_hidl_api32.so
'''
        if args.audio:
            config += '''
service pocket-mediametrics /system/bin/mediametrics
    disabled
    user 1013
    group 1013
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api32.so

service pocket-audio /system/bin/audioserver
    disabled
    user 1041
    group 1041
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_hidl_api32.so
'''
        if args.network:
            config += '''
service pocket-network /system/bin/pocket-netd-api32
    disabled
    user 0
    group 0
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api32.so

service pocket-memory /system/bin/pocket-memory-api32
    disabled
    user 0
    group 0
    socket lmkd seqpacket 0660 1000 1000
'''
        if args.mdns:
            config += '''
service mdnsd /system/bin/mdnsd
    disabled
    oneshot
    user 1020
    group 3003
    socket mdnsd stream 0660 1020 3003

service pocket-mdns-control /system/bin/pocket-mdns-control-api32
    disabled
    user 0
    group 1000
    socket mdns stream 0660 0 1000
'''
        if args.dns:
            config += '''
service pocket-dns /system/bin/pocket-dns-api32
    disabled
    user 0
    group 0
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api32.so
'''
        if args.apex:
            config += '''
service pocket-apex /system/bin/pocket-apex-service-api32 serve
    disabled
    user 0
    group 0
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api32.so
'''
        if args.storage:
            config += '''
service pocket-storage /system/bin/pocket-storage-api32
    disabled
    user 0
    group 0
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api32.so
'''
        if args.keystore:
            config += '''
service pocket-keymaster /system/bin/pocket-keymaster-api32
    disabled
    user 1000
    group 1000 1026
    setenv LD_PRELOAD /system/lib64/libpocket_hidl_api32.so

service pocket-keystore /system/bin/keystore2 /data/misc/keystore
    disabled
    user 1017
    group 1017 3009 1007
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_hidl_api32.so:/system/lib64/libpocket_keystore_api32.so

service pocket-gatekeeper-hal /system/bin/pocket-gatekeeper-hal-api32
    disabled
    user 1000
    group 1000
    setenv LD_PRELOAD /system/lib64/libpocket_hidl_api32.so:/system/lib64/libpocket_gatekeeper_pool_api32.so

service gatekeeperd /system/bin/gatekeeperd /data/misc/gatekeeper
    disabled
    user 1000
    group 1000
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_hidl_api32.so
'''
        if args.installd:
            config += '''
on boot
    setprop dalvik.vm.dex2oat64.enabled 1

service pocket-installd /system/bin/installd
    disabled
    user 0
    group 0
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_policy_api32.so
'''
        if args.system_server:
            config += '''
service idmap2d /system/bin/idmap2d
    disabled
    user 1000
    group 1000
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api32.so

service pocket-traced /system/bin/traced
    disabled
    user 9999
    group 9999
    socket traced_consumer stream 0666 0 0
    socket traced_producer stream 0666 0 0
    setenv LD_PRELOAD /system/lib64/libpocket_perfetto_api32.so

service pocket-tombstoned /system/bin/tombstoned
    disabled
    user 1058
    group 1000
    socket tombstoned_crash seqpacket 0666 1000 1000
    socket tombstoned_intercept seqpacket 0666 1000 1000
    socket tombstoned_java_trace seqpacket 0666 1000 1000
    setenv LD_PRELOAD /system/lib64/libpocket_tombstoned_api32.so

service pocket-framework /system/bin/pocket-app-process-api32 -Xzygote -Xnorelocate -Xnoimage-dex2oat -Ximage:/apex/com.android.art/javalib/boot.art:/system/framework/boot-framework.art -Xms32m -Xmx384m /system/bin --zygote --start-system-server --socket-name=zygote
    disabled
    user 0
    group 0
    socket zygote stream 0660 0 0
    socket usap_pool_primary stream 0660 0 0
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_hidl_api32.so:/system/lib64/libpocket_uevent_api32.so:/system/lib64/libpocket_vintf_api32.so:/system/lib64/libpocket_trace_api32.so:/system/lib64/libpocket_framework_policy_api32.so:/system/lib64/libpocket_service_timers_api32.so
    setenv POCKET_API32_TRACE_STARTUP @STARTUP_TRACE@
    setenv POCKET_API32_OBSERVE_STARTUP_CODE @STARTUP_CODE@
    setenv POCKET_API32_USE_TOMBSTONED 1
    setenv POCKET_API32_ROOTLESS_BLUETOOTH_SCHEDULING 1

service pocket-cameraserver /system/bin/cameraserver
    disabled
    user 1047
    group 1047
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_hidl_api32.so:/system/lib64/libpocket_framework_policy_api32.so
'''
            config=config.replace('@STARTUP_TRACE@','1' if args.trace_startup else '0')
            config=config.replace('@STARTUP_CODE@','1' if args.observe_startup_code else '0')
            if args.display:
                config += '''
service pocket-input-control /system/bin/pocket-app-process-api32 -Xnorelocate -Xnoimage-dex2oat -Ximage:/apex/com.android.art/javalib/boot.art:/system/framework/boot-framework.art -cp /system/framework/pocket-zygote.jar /system/bin dev.kreijstal.aosppocket.guest.InputControl32
    disabled
    user 0
    group 0
    socket pocket_control32 stream 0600 1000 1000
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api32.so
'''
            if args.graphics:
                config += '''
service pocket-surfaceflinger /system/bin/surfaceflinger
    disabled
    user 1000
    group 1000 1003 1004 3003
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_hidl_api32.so@RENDERER_WORKERS@
'''
        config=config.replace('@RENDERER_WORKERS@',':/system/lib64/libpocket_renderer_workers_api32.so' if args.renderer_worker_scheduling else '')
        config=config.replace('-cp /system/framework/pocket-zygote.jar ','-cp /system/framework/pocket-tools.jar ')
        (init_dir / 'pocket-api32.rc').write_text(config)
        # Each rejected configuration contains an otherwise valid service and
        # trigger, so the test observes whether parsing really prevents launch.
        prefix = 'on boot\n    start must-not-run\nservice must-not-run /system/bin/pocket-init-test-api32 hold\n    user 39999\n'
        for name, invalid in [('namespace', '    namespace mnt\n'),
                              ('builtin', '\non boot\n    mount tmpfs tmpfs /data\n'),
                              ('import', '\nimport /system/etc/init/does-not-exist.rc\n')]:
            (init_dir / ('reject-' + name + '.rc')).write_text(prefix + invalid)
        script += '''
# Retire the standalone property server before handing its role to init.
kill "$property_pid"
wait "$property_pid"
property_pid=''
for config in namespace builtin import; do
    /system/bin/toybox rm -f /dev/__properties__/*
    result=0
    /system/bin/pocket-init-api32 /system/etc/init/reject-$config.rc > /tmp/reject-$config.log 2>&1 || result=$?
    test "$result" -eq 66
    /system/bin/toybox grep INIT_CONFIG_REJECTED /tmp/reject-$config.log
    /system/bin/pocket-init-test-api32 rejected
done
/system/bin/toybox rm -f /dev/__properties__/*
/system/bin/pocket-init-api32 /system/etc/init/pocket-api32.rc > /tmp/properties.log 2>&1 &
property_pid=$!
await_marker API32_INIT_READY /tmp/properties.log "$property_pid"
/system/bin/pocket-init-test-api32 client-native
'''
        if args.zygote:
            script = script.replace('/system/bin/pocket-init-api32 /system/etc/init/pocket-api32.rc >',
                'export BOOTCLASSPATH="$BOOTCLASSPATH:/system/framework/pocket-zygote.jar"\n/system/bin/pocket-init-api32 /system/etc/init/pocket-api32.rc >')
    if args.binder:
        script += '''
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-binder-probe-api32 manager > /tmp/binder.log 2>&1 &
manager_pid=$!
await_marker API32_READY /tmp/binder.log "$manager_pid"
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-binder-probe-api32 client
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-binder-probe-api32 client-system
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-binder-probe-api32 client-virtual
echo API32_BINDER_PASS
'''
        if args.init:
            script = script.replace('LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-binder-probe-api32 manager > /tmp/binder.log 2>&1 &\nmanager_pid=$!\nawait_marker API32_READY /tmp/binder.log "$manager_pid"',
                '/system/bin/pocket-init-test-api32 start pocket-registry\nawait_marker API32_READY /tmp/properties.log "$property_pid"')
    if args.installd:
        script += '''
POCKET_SYSCALL_BACKEND=native /system/bin/pocket-boot-probe-api32 prepare-data
/system/bin/toybox mkdir -p /data/misc/installd
/system/bin/pocket-init-test-api32 start pocket-installd
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-installd-probe-api32 check
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-installd-probe-api32 denied
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-installd-probe-api32 dexopt
'''
    if args.network:
        script += '''
/system/bin/toybox mkdir -p /data/misc
/system/bin/pocket-init-test-api32 start pocket-network
await_marker API32_NETD_READY /tmp/properties.log "$property_pid"
/system/bin/pocket-init-test-api32 start pocket-memory
/system/bin/pocket-memory-probe-api32
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-network-probe-api32 check
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-network-probe-api32 denied
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-network-probe-api32 stack
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-network-probe-api32 policy
'''
    if args.mdns:
        script += '''
/system/bin/pocket-init-test-api32 start pocket-mdns-control
await_marker API32_MDNS_CONTROL_READY /tmp/properties.log "$property_pid"
/system/bin/pocket-mdns-probe-api32 denied
/system/bin/pocket-mdns-probe-api32 check
'''
    if args.dns:
        script += '''
/system/bin/pocket-init-test-api32 start pocket-dns
await_marker API32_DNS_READY /tmp/properties.log "$property_pid"
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-dns-probe-api32 check
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-dns-probe-api32 denied
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-dns-probe-api32 resolve
'''
    if args.apex:
        script += '''
/system/bin/pocket-init-test-api32 start pocket-apex
await_marker API32_APEX_READY /tmp/properties.log "$property_pid"
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-apex-service-api32 check
/system/bin/pocket-apex-service-api32 check-data
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-apex-service-api32 denied
'''
    if args.storage:
        script += '''
/system/bin/pocket-init-test-api32 start pocket-storage
await_marker API32_STORAGE_READY /tmp/properties.log "$property_pid"
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-storage-probe-api32 check
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-storage-probe-api32 denied
'''
    if args.hardware:
        script += '''
/system/bin/pocket-init-test-api32 start pocket-hardware
await_marker 'DIRECT_HWB_REGISTRY_READY status=0' /tmp/properties.log "$property_pid"
/system/bin/pocket-hardware-probe-api32
/system/bin/pocket-hardware-probe-api32 denied
await_marker API32_SUSPEND_CONTROL_READY /tmp/properties.log "$property_pid"
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_hidl_api32.so /system/bin/pocket-suspend-probe-api32 check
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_hidl_api32.so /system/bin/pocket-suspend-probe-api32 denied
# Publishing the HIDL endpoint must leave the typed native registry intact.
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-binder-probe-api32 client-virtual
echo API32_BINDER_ENDPOINTS_COEXIST_PASS
'''
    if args.input:
        script += '''
/system/bin/toybox mkdir -p /dev/input
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_hidl_api32.so /system/bin/pocket-input-probe-api32
'''
    if args.audio:
        script += '''
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-audio-probe-api32 denied
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_hidl_api32.so /system/bin/pocket-audio-probe-api32 hal
/system/bin/pocket-init-test-api32 start pocket-mediametrics
/system/bin/pocket-init-test-api32 start pocket-audio
'''
    if args.graphics:
        script += '''
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_hidl_api32.so /system/bin/pocket-graphics-probe-api32
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_hidl_api32.so /system/bin/pocket-egl-probe-api32
echo API32_GRAPHICS_PASS
'''
    if args.zygote:
        groups={
            'zygote':['ZygoteBootstrap32.java','Api32Child.java'],
            'tools':list(RUNTIME_TOOL_SOURCES),
        }
        for name,sources in groups.items():
            group_android=prepare_system_sdk() if name=='tools' else android
            classes,dex=out/(name+'-classes'),out/(name+'-dex')
            for directory in [classes,dex]:
                if directory.exists():shutil.rmtree(directory)
                directory.mkdir()
            java=[ROOT/'runtime/api32'/source for source in sources]
            if name=='tools':java.append(ROOT/'runtime/app/src/dev/kreijstal/aosppocket/bridge/HostWire.java')
            subprocess.run(['javac','--release','8','-cp',str(group_android),'-d',str(classes)]+list(map(str,java)),check=True)
            jar=out/(name+'.jar')
            subprocess.run(['jar','cf',str(jar),'-C',str(classes),'.'],check=True)
            subprocess.run(['java','-cp',str(r8),'com.android.tools.r8.D8','--lib',str(group_android),'--min-api','32',
                '--output',str(dex),str(jar)],check=True)
            write_dex_jar(stage/('system/framework/pocket-'+name+'.jar'),dex/'classes.dex')
        script += '''
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-app-process-api32 -Xnorelocate -Xnoimage-dex2oat -Ximage:/apex/com.android.art/javalib/boot.art:/system/framework/boot-framework.art -cp /system/framework/pocket-tools.jar /system/bin dev.kreijstal.aosppocket.guest.BootArtifactsProbe32
export BOOTCLASSPATH="$BOOTCLASSPATH:/system/framework/pocket-zygote.jar"
POCKET_ZYGOTE_FD_TRACE=1 LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-zygote-launcher-api32 > /tmp/zygote.log 2>&1 &
zygote_pid=$!
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-zygote-client-api32
'''
        if args.init:
            script = script.replace('export BOOTCLASSPATH="$BOOTCLASSPATH:/system/framework/pocket-zygote.jar"\nPOCKET_ZYGOTE_FD_TRACE=1 LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-zygote-launcher-api32 > /tmp/zygote.log 2>&1 &\nzygote_pid=$!',
                '/system/bin/pocket-init-test-api32 start pocket-zygote')
    if args.keystore:
        script += '''
/system/bin/toybox mkdir -p /data/misc/keystore
/system/bin/toybox chmod 0700 /data/misc/keystore
/system/bin/toybox chown 1017:1017 /data/misc/keystore
/system/bin/pocket-init-test-api32 start pocket-keymaster
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_hidl_api32.so /system/bin/pocket-keystore-probe-api32 hal
/system/bin/pocket-init-test-api32 start pocket-keystore
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-keystore-probe-api32 ready
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_hidl_api32.so /system/bin/pocket-keystore-probe-api32 denied
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-app-process-api32 -Xnorelocate -Xnoimage-dex2oat -Ximage:/apex/com.android.art/javalib/boot.art:/system/framework/boot-framework.art -cp /system/framework/pocket-zygote.jar /system/bin dev.kreijstal.aosppocket.guest.KeystoreProbe32
/system/bin/toybox mkdir -p /data/misc/gatekeeper
/system/bin/toybox chmod 0700 /data/misc/gatekeeper
/system/bin/toybox chown 1000:1000 /data/misc/gatekeeper
/system/bin/pocket-init-test-api32 start pocket-gatekeeper-hal
await_marker 'descriptor=android.hardware.gatekeeper@1.0::IGatekeeper instance=default' /tmp/properties.log "$property_pid"
/system/bin/pocket-init-test-api32 start gatekeeperd
'''
    if args.system_server:
        script += '''
/system/bin/toybox rm -rf /tmp/art-traces
/system/bin/toybox mkdir /tmp/art-traces
/system/bin/toybox chmod 1777 /tmp/art-traces
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_trace_api32.so /system/bin/pocket-app-process-api32 -Xnorelocate -Xnoimage-dex2oat -Ximage:/apex/com.android.art/javalib/boot.art:/system/framework/boot-framework.art -cp /system/framework/pocket-zygote.jar /system/bin dev.kreijstal.aosppocket.guest.TraceProbe32
/system/bin/toybox mkdir -p /data/tombstones /data/anr
/system/bin/toybox chown 1058:1000 /data/tombstones /data/anr
/system/bin/toybox chmod 0770 /data/tombstones /data/anr
/system/bin/pocket-init-test-api32 start pocket-tombstoned
await_marker API32_TOMBSTONED_SCOPE_READY /tmp/properties.log "$property_pid"
/system/bin/pocket-init-test-api32 start pocket-traced
/system/bin/pocket-perfetto-probe-api32 "$(/system/bin/toybox pidof traced)"
tombstoned_pid=$(/system/bin/toybox pidof tombstoned)
/system/bin/pocket-tombstoned-probe-api32 denied "$tombstoned_pid"
/system/bin/pocket-tombstoned-probe-api32 outside "$tombstoned_pid"
POCKET_API32_ROOTLESS_BLUETOOTH_SCHEDULING=1 LD_PRELOAD=/system/lib64/libpocket_service_timers_api32.so /system/bin/pocket-service-timer-probe-api32
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_trace_api32.so /system/bin/pocket-app-process-api32 -Xnorelocate -Xnoimage-dex2oat -Ximage:/apex/com.android.art/javalib/boot.art:/system/framework/boot-framework.art -cp /system/framework/pocket-tools.jar /system/bin dev.kreijstal.aosppocket.guest.TombstonedProbe32 check
/system/bin/pocket-uevent-probe-api32
/system/bin/pocket-vintf-probe-api32
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_uevent_api32.so /system/bin/pocket-app-process-api32 -Xnorelocate -Xnoimage-dex2oat -Ximage:/apex/com.android.art/javalib/boot.art:/system/framework/boot-framework.art -cp /system/framework/pocket-zygote.jar /system/bin dev.kreijstal.aosppocket.guest.UEventProbe
'''
        script += '/system/bin/pocket-init-test-api32 start idmap2d\n'
        if args.graphics:
            script += '/system/bin/pocket-init-test-api32 start pocket-surfaceflinger\nLD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-framework-probe-api32 surface\n'
        script += '''
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-framework-probe-api32 prepare
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-app-process-api32 -Xnorelocate -Xnoimage-dex2oat -Ximage:/apex/com.android.art/javalib/boot.art:/system/framework/boot-framework.art -cp /system/framework/pocket-tools.jar /system/bin dev.kreijstal.aosppocket.guest.SystemServerDexopt32
/system/bin/pocket-init-test-api32 start pocket-cameraserver
/system/bin/pocket-init-test-api32 start pocket-framework
/system/bin/pocket-zygote-client-api32 preloaded
LD_PRELOAD=/system/lib64/libpocket_hidl_api32.so /system/bin/pocket-hardware-probe-api32 framework
'''
        if args.audio:
            script += 'LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-audio-probe-api32 services\n'
        script += '''
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-framework-probe-api32 wait
LD_PRELOAD=/system/lib64/libpocket_hidl_api32.so /system/bin/pocket-hardware-probe-api32 camera
LD_PRELOAD=/system/lib64/libpocket_hidl_api32.so /system/bin/pocket-hardware-probe-api32 camera-role-denied
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-app-process-api32 -Xnorelocate -Xnoimage-dex2oat -Ximage:/apex/com.android.art/javalib/boot.art:/system/framework/boot-framework.art -cp /system/framework/pocket-zygote.jar /system/bin dev.kreijstal.aosppocket.guest.FrameworkReadyProbe
'''
        if args.compile_ui:
            script += 'LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-app-process-api32 -Xnorelocate -Xnoimage-dex2oat -Ximage:/apex/com.android.art/javalib/boot.art:/system/framework/boot-framework.art -cp /system/framework/pocket-tools.jar /system/bin dev.kreijstal.aosppocket.guest.UiRuntimeProbe32 compile\n'
        if args.keystore:
            # Stock gatekeeperd asks the framework permission controller about
            # non-root callers, so role/credential probes belong after readiness.
            script += 'LD_PRELOAD=/system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_hidl_api32.so /system/bin/pocket-gatekeeper-probe-api32\n'
    if args.apps:
        copy(build_guest_bridge(),'system/app/PocketServices/PocketServices.apk')
        copy(build_guest_app(),'data/local/tmp/api32-guest-test.apk')
        if args.display:
            script += '/system/bin/pocket-init-test-api32 start pocket-input-control\n'
            script += 'await_marker API32_INPUT_CONTROL_READY /tmp/properties.log "$property_pid"\n'
        script += '''
LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-app-process-api32 -Xnorelocate -Xnoimage-dex2oat -Ximage:/apex/com.android.art/javalib/boot.art:/system/framework/boot-framework.art -cp /system/framework/pocket-zygote.jar /system/bin dev.kreijstal.aosppocket.guest.AppLifecycleProbe32 /data/local/tmp/api32-guest-test.apk
'''
    if args.compile_ui:
        script += '''
if test "${POCKET_API32_REQUIRE_UI:-0}" = 1; then
    LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-app-process-api32 -Xnorelocate -Xnoimage-dex2oat -Ximage:/apex/com.android.art/javalib/boot.art:/system/framework/boot-framework.art -cp /system/framework/pocket-tools.jar /system/bin dev.kreijstal.aosppocket.guest.UiRuntimeProbe32 verify
fi
'''
    if args.init:
        script += '''
kill "$property_pid"
wait "$property_pid"
property_pid=''
/system/bin/pocket-init-test-api32 stopped
echo API32_INIT_PASS
'''
    script += 'echo API32_NATIVE_RUNTIME_PASS\n'
    script=script.replace('-cp /system/framework/pocket-zygote.jar ','-cp /system/framework/pocket-tools.jar ')
    (stage / 'probe.sh').write_text(script)
    if args.runtime_boot:stage_runtime_boot(stage,args.core_services)
    elif args.core_services:
        # Keep the normal entrypoints as inactive inputs so switching validation
        # modes can use a hash-verified delta instead of repacking the full image.
        stage_runtime_boot(stage,True)
        config_path=stage/'system/etc/init/pocket-api32.rc'
        config_path.write_text(consolidate_core(config_path.read_text(),test_echo=True))
        prepare_data='POCKET_SYSCALL_BACKEND=native /system/bin/pocket-boot-probe-api32 prepare-data\n'
        registry_start='/system/bin/pocket-init-test-api32 start pocket-registry\n'
        if script.count(prepare_data)!=1 or script.count(registry_start)!=1:
            raise ValueError('Core startup requires one data preparation and registry start')
        script=script.replace(prepare_data,'').replace(registry_start,prepare_data+registry_start)
        crypto_start='/system/bin/pocket-init-test-api32 start pocket-keymaster\n'
        if script.count(crypto_start)!=1:raise ValueError('Missing shared crypto startup')
        script=script.replace(crypto_start,crypto_start+'/system/bin/pocket-scrypt-probe-api32\n')
        for name in CONSOLIDATED_COMPONENTS:
            script=script.replace('/system/bin/pocket-init-test-api32 start '+name+'\n','')
        (stage/'probe.sh').write_text(script)
    # Reserve PT_INTERP space only in executables. Bionic and ART stay unchanged.
    for name in executables - {'system/bin/pocket-loader'}:
        path = stage / name
        if not reserve_interpreter(path):
            # Android 12L's linkerconfig is static. The existing loader exec
            # path maps static programs itself and preserves the syscall view.
            header = path.read_bytes()[:64]
            if header[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', header, 18)[0] != 183:
                raise ValueError('Invalid static ARM64 executable: ' + name)
            continue
        elf = bytearray(path.read_bytes())
        table = struct.unpack_from('<Q', elf, 32)[0]
        stride, count = struct.unpack_from('<HH', elf, 54)
        for index in range(count):
            entry = struct.unpack_from('<IIQQQQQQ', elf, table + index * stride)
            if entry[0] == 3:
                value = (args.runtime_root + '/system/bin/pocket-loader').encode() + b'\0'
                if len(value) > entry[5]:
                    raise ValueError('Interpreter reservation too short')
                elf[entry[2]:entry[2] + entry[5]] = value.ljust(entry[5], b'\0')
                break
        path.write_bytes(elf)
    inventory = {p.relative_to(stage).as_posix(): hashlib.file_digest(p.open('rb'), 'sha256').hexdigest()
                 for p in stage.rglob('*') if p.is_file() and p.relative_to(stage).as_posix() != 'linkerconfig/ld.config.txt'}
    (out / 'build.json').write_text(json.dumps({'sdk': 32, 'runtimeRoot': args.runtime_root, 'files': inventory,
        'artRequested': args.art, 'binderRequested': args.binder, 'zygoteRequested': args.zygote,
        'initRequested': args.init,
        'hardwareRequested': args.hardware,
        'apexRequested': args.apex,
        'installdRequested': args.installd,
        'systemServerRequested': args.system_server,
        'graphicsRequested': args.graphics,
        'rendererWorkerScheduling': 1 if args.renderer_worker_scheduling else 0,
        'inputRequested': args.input,
        'compileUiRequested': args.compile_ui,
        'startupTraceRequested': args.trace_startup,
        'startupCodeObserveRequested': args.observe_startup_code,
        'runtimeBootRequested': args.runtime_boot,
        'runtimeKernelChildOwnership': bool(args.runtime_boot),
        'runtimeSystemServerBridge': bool(args.runtime_boot or args.core_services),
        'runtimeMediaServices': bool(runtime_services),
        'runtimeMediaMetricsInCore': bool(args.runtime_boot and args.core_services),
        'runtimeMicrogSupport': bool(runtime_services),
        'coreServicesRequested': args.core_services,
        'coreServicesLayout': CORE_SERVICES_LAYOUT if args.core_services else 0,
        'networkRequested': args.network,
        'dnsRequested': args.dns,
        'mdnsRequested': args.mdns,
        'audioRequested': args.audio,
        'storageRequested': args.storage,
        'keystoreRequested': args.keystore,
        'appsRequested': args.apps,
        'displayRequested': args.display,
        'scope': 'Native Linux-user foundation fixture; Android sandbox and framework not yet verified'}, indent=2) + '\n')
    print('API32_NATIVE_PROBE_BUILD_PASS', stage, 'files=' + str(len(inventory)))


if __name__ == '__main__':
    main()
