#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Package the API 32 foundation inside a separate, non-debuggable Android app.

This is validation infrastructure, not a production Manager or a guest image
release. The production Manager continues to offer only its verified profiles.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile
from prepare_api32 import ROOT, CACHE
from api32_runtime_boot import CORE_SERVICES_LAYOUT

PACKAGE = 'dev.kreijstal.aosppocket.api32test'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk', type=Path, required=True)
    parser.add_argument('--binder', action='store_true')
    parser.add_argument('--zygote', action='store_true')
    parser.add_argument('--init', action='store_true')
    parser.add_argument('--hardware', action='store_true')
    parser.add_argument('--apex', action='store_true')
    parser.add_argument('--installd', action='store_true')
    parser.add_argument('--system-server', action='store_true')
    parser.add_argument('--graphics', action='store_true')
    parser.add_argument('--input', action='store_true')
    parser.add_argument('--network', action='store_true')
    parser.add_argument('--dns', action='store_true')
    parser.add_argument('--mdns', action='store_true')
    parser.add_argument('--audio', action='store_true')
    parser.add_argument('--storage', action='store_true')
    parser.add_argument('--keystore', action='store_true')
    parser.add_argument('--apps', action='store_true')
    parser.add_argument('--display', action='store_true')
    parser.add_argument('--compile-ui', action='store_true')
    parser.add_argument('--trace-startup', action='store_true', help='Diagnostic only: enable repeated ART SIGQUIT startup sampling')
    parser.add_argument('--observe-startup-code', action='store_true', help='Diagnostic only: bounded SystemUI executable-code/scheduling snapshots')
    parser.add_argument('--runtime-boot', action='store_true', help='Validate the separate normal runtime startup path')
    parser.add_argument('--core-services', action='store_true', help='Validate the shared UID-0 native core')
    parser.add_argument('--renderer-worker-scheduling',action='store_true',help='Test normal priority for software-compositor workers')
    parser.add_argument('--reuse-native', action='store_true', help='Use the already-built native adapters')
    parser.add_argument('--reuse-fixture', action='store_true', help='Rebuild only the host APK after verifying all staged guest inputs')
    parser.add_argument('--thin-update', action='store_true', help='Test update only: require the existing guest files and verify every input hash before execution')
    parser.add_argument('--delta-from', type=Path, help='Test update only: embed changed inputs relative to this prior fixture image manifest; every input is still verified on device')
    args = parser.parse_args()
    if args.renderer_worker_scheduling and not args.graphics:parser.error('--renderer-worker-scheduling requires --graphics')
    if args.trace_startup and not args.system_server:parser.error('--trace-startup requires --system-server')
    if args.observe_startup_code and not args.system_server:parser.error('--observe-startup-code requires --system-server')
    if args.thin_update and not args.reuse_fixture:
        parser.error('--thin-update requires --reuse-fixture; use a full APK for initial installation')
    if args.delta_from and args.thin_update:
        parser.error('--delta-from and --thin-update are mutually exclusive')
    baseline = None
    baseline_digest = None
    if args.delta_from:
        baseline_bytes = args.delta_from.read_bytes()
        baseline = json.loads(baseline_bytes)
        if baseline.get('schema') != 1 or baseline.get('sdk') != 32 or not baseline.get('files'):
            parser.error('--delta-from requires an API 32 fixture image manifest')
        baseline_digest = hashlib.sha256(baseline_bytes).hexdigest()
    out = CACHE / 'android-probe'
    runtime = '/data/user/0/' + PACKAGE + '/files/root'
    command = [sys.executable, str(ROOT / 'tools/build_api32_probe.py'), '--ndk', str(args.ndk),
               '--out', str(out), '--runtime-root', runtime, '--art']
    if args.reuse_native:
        command.append('--reuse-native')
    if args.binder:
        command.append('--binder')
    if args.init:
        command.append('--init')
    if args.hardware:
        if not args.binder or not args.init:
            parser.error('--hardware requires --binder --init')
        command.append('--hardware')
    if args.apex:
        if not args.binder or not args.init:
            parser.error('--apex requires --binder --init')
        command.append('--apex')
    if args.installd:
        if not args.binder or not args.init:
            parser.error('--installd requires --binder --init')
        command.append('--installd')
    if args.system_server:
        command.append('--system-server')
    if args.graphics:
        command.append('--graphics')
    if args.input:
        command.append('--input')
    if args.network:
        command.append('--network')
    if args.dns:
        command.append('--dns')
    if args.mdns:
        command.append('--mdns')
    if args.audio:
        command.append('--audio')
    if args.storage:
        command.append('--storage')
    if args.keystore:
        command.append('--keystore')
    if args.apps:
        command.append('--apps')
    if args.display:
        command.append('--display')
    if args.compile_ui:command.append('--compile-ui')
    if args.trace_startup:command.append('--trace-startup')
    if args.observe_startup_code:command.append('--observe-startup-code')
    if args.runtime_boot:command.append('--runtime-boot')
    if args.core_services:command.append('--core-services')
    if args.renderer_worker_scheduling:command.append('--renderer-worker-scheduling')
    if args.zygote:
        if not args.binder:
            parser.error('--zygote requires --binder')
        command.append('--zygote')
    stage = out / 'root'
    if args.reuse_fixture:
        inputs = json.loads((out / 'build.json').read_text())
        assert inputs['sdk'] == 32 and inputs['runtimeRoot'] == runtime and inputs['artRequested']
        assert inputs.get('binderRequested', False) == args.binder
        assert inputs.get('zygoteRequested', False) == args.zygote
        assert inputs.get('initRequested', False) == args.init
        assert inputs.get('hardwareRequested', False) == args.hardware
        assert inputs.get('apexRequested', False) == args.apex
        assert inputs.get('installdRequested', False) == args.installd
        assert inputs.get('systemServerRequested', False) == args.system_server
        assert inputs.get('graphicsRequested', False) == args.graphics
        assert inputs.get('rendererWorkerScheduling',0) == (1 if args.renderer_worker_scheduling else 0)
        assert inputs.get('inputRequested', False) == args.input
        assert inputs.get('networkRequested', False) == args.network
        assert inputs.get('dnsRequested', False) == args.dns
        assert inputs.get('mdnsRequested', False) == args.mdns
        assert inputs.get('audioRequested', False) == args.audio
        assert inputs.get('storageRequested', False) == args.storage
        assert inputs.get('keystoreRequested', False) == args.keystore
        assert inputs.get('appsRequested', False) == args.apps
        assert inputs.get('displayRequested', False) == args.display
        assert inputs.get('compileUiRequested', False) == args.compile_ui
        assert inputs.get('startupTraceRequested') == args.trace_startup
        assert inputs.get('startupCodeObserveRequested') == args.observe_startup_code
        assert inputs.get('runtimeBootRequested',False) == args.runtime_boot
        assert inputs.get('coreServicesRequested',False) == args.core_services
        assert inputs.get('coreServicesLayout',0) == (CORE_SERVICES_LAYOUT if args.core_services else 0)
        for name, digest in inputs['files'].items():
            assert hashlib.file_digest((stage / name).open('rb'), 'sha256').hexdigest() == digest, name
    else:
        subprocess.run(command, check=True)
    (stage / 'probe-binder-enabled').write_text('1' if args.binder else '0')
    (stage / 'probe-hardware-enabled').write_text('1' if args.hardware else '0')
    (stage / 'probe-dns-enabled').write_text('1' if args.dns else '0')
    (stage / 'probe-audio-enabled').write_text('1' if args.audio else '0')
    (stage / 'probe-runtime-boot-enabled').write_text('1' if args.runtime_boot else '0')
    (stage / 'probe-environment').write_text('POCKET_API32_UI_PROBE=1\n' if args.system_server else '')
    if args.apps:
        with (stage / 'probe-environment').open('a') as environment:environment.write('POCKET_API32_APP_PROBE=1\n')
    if args.display:
        with (stage / 'probe-environment').open('a') as environment:environment.write('POCKET_API32_HOST_DISPLAY=1\n')
    # Generated namespace files are reset before the test and excluded from the
    # immutable input inventory; their verified bootstrap is included instead.
    files = {p.relative_to(stage).as_posix(): {'bytes': p.stat().st_size,
             'sha256': hashlib.file_digest(p.open('rb'), 'sha256').hexdigest()}
             for p in sorted(stage.rglob('*')) if p.is_file() and
             p.relative_to(stage).as_posix() != 'linkerconfig/ld.config.txt'}
    embedded = [] if args.thin_update else list(files)
    if baseline is not None:
        removed = set(baseline['files']) - files.keys()
        if removed:
            raise ValueError('Delta updates cannot remove fixture inputs: '+', '.join(sorted(removed)))
        embedded = [name for name, spec in files.items() if baseline['files'].get(name) != spec]
    manifest = out / 'image-manifest.json'
    manifest.write_text(json.dumps({'schema': 1, 'sdk': 32, 'files': files}, separators=(',', ':')) + '\n')
    tool = ROOT.parent / 'helloworldandroid'
    android = tool / 'vendor/android-platforms/android-36/android.jar'
    r8 = ROOT.parent / 's20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar'
    def run(*arguments):
        subprocess.run(list(map(str, arguments)), check=True)
    for name in ['classes', 'dex', 'manifest-tools']:
        directory = out / name
        if directory.exists():
            shutil.rmtree(directory)
        directory.mkdir()
    java = [ROOT / 'runtime/tests/api32/MainActivity.java', ROOT / 'runtime/tests/api32/NativeLog.java', ROOT / 'runtime/tests/api29/HostBinder.java']
    java += [ROOT / 'runtime/tests/api32/HostHardware.java', ROOT / 'runtime/direct/BootBattery.java']
    java += [ROOT/'runtime/app/src/dev/kreijstal/aosppocket/runtime/HostAudio.java',ROOT/'runtime/app/src/dev/kreijstal/aosppocket/runtime/HostMedia.java',ROOT/'runtime/app/src/dev/kreijstal/aosppocket/runtime/GuestLog32.java']
    java += [ROOT / 'runtime/app/src/dev/kreijstal/aosppocket/runtime' / name
             for name in ['GuestBinder.java', 'GuestPower.java', 'Native.java','HostDns.java','GuestInput32.java','InputCommandQueue.java']]
    java += [ROOT/'runtime/app/src/dev/kreijstal/aosppocket/bridge/HostWire.java']
    java += [ROOT/'runtime/tests/api32/HostDisplay32.java']
    java += [ROOT/'runtime/app/src/dev/kreijstal/aosppocket/bridge'/name for name in ['DisplayGeometry.java','SharedFrames.java','AnonymousFrames.java']]
    run('javac', '--release', '8', '-cp', android, '-d', out / 'classes', *java)
    run('jar', 'cf', out / 'classes.jar', '-C', out / 'classes', '.')
    run('java', '-cp', r8, 'com.android.tools.r8.D8', '--lib', android, '--min-api', '28',
        '--output', out / 'dex', out / 'classes.jar')
    run('javac', '--release', '8', '-cp', tool / 'build/tools', '-d', out / 'manifest-tools',
        ROOT / 'runtime/tools/PocketManifest.java')
    run('java', '-cp', str(tool / 'build/tools') + ':' + str(out / 'manifest-tools'), 'PocketManifest',
        ROOT / 'runtime/tests/api32/AndroidManifest.xml', out / 'AndroidManifest.xml', '28', '28', '1', '0.1')
    run('java', '-cp', tool / 'build/tools', 'com.example.build.ApkBuilder',
        out / 'AndroidManifest.xml', out / 'dex/classes.dex', out / 'unsigned.apk')
    with zipfile.ZipFile(out / 'unsigned.apk', 'a', zipfile.ZIP_DEFLATED, compresslevel=1, strict_timestamps=False) as apk:
        for name in embedded:
            apk.write(stage / name, 'assets/root/' + name)
        apk.write(manifest, 'assets/fixture-image.json')
        apk.write(ROOT / 'out/runtime-native/libpocket_runtime.so', 'lib/arm64-v8a/libpocket_runtime.so')
        apk.write(ROOT / 'out/runtime-native/libpocket_frames.so', 'lib/arm64-v8a/libpocket_frames.so')
        for path, name in [(ROOT / 'LICENSE', 'LICENSE'), (ROOT / 'runtime/NOTICE', 'NOTICE'),
                           (CACHE / 'reference/NOTICE.txt', 'android12L-NOTICE.txt')]:
            apk.write(path, 'assets/licenses/' + name)
    signer = ['java', '-cp', str(tool / 'build/apksigner.jar'), 'com.android.apksigner.ApkSignerTool']
    target = out / 'api32-probe.apk'
    run(*signer, 'sign', '--ks', ROOT / 'out/debug.keystore', '--ks-pass', 'pass:android',
        '--key-pass', 'pass:android', '--out', target, out / 'unsigned.apk')
    run(*signer, 'verify', target)
    (out / 'apk-build.json').write_text(json.dumps({'package': PACKAGE, 'sdk': 32, 'artRequested': True,
        'binderRequested': args.binder, 'zygoteRequested': args.zygote, 'initRequested': args.init, 'thinUpdate': args.thin_update,
        'deltaUpdate': baseline is not None, 'deltaBaselineManifestSha256': baseline_digest,
        'embeddedGuestFiles': embedded,
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
        'imageManifestSha256': hashlib.file_digest(manifest.open('rb'), 'sha256').hexdigest(), 'apkBytes': target.stat().st_size,
        'apkSha256': hashlib.file_digest(target.open('rb'), 'sha256').hexdigest(),
        'fullGuestBootVerified': False, 'androidAppSandboxVerified': False}, indent=2) + '\n')
    print('API32_ANDROID_PROBE_BUILD_PASS', target, target.stat().st_size)


if __name__ == '__main__':
    main()
