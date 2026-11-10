#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run the API 32 foundation APK only in its dedicated ARM64 Android host VM."""
import argparse
import base64
import hashlib
import importlib.util
import json
import re
import shlex
from pathlib import Path
import time
import uuid

PACKAGE = 'dev.kreijstal.aosppocket.api32test'


def validate_runtime_report(report,app_pattern_proof,tapped,editor_tapped,keyboard_sent,ui_screenshots):
    """Acceptance checks for normal startup; component-suite results are separate."""
    log=report.get('log','');native=report.get('nativeLog','')
    markers=['POCKET_RUNTIME_BOOT_READY sdk=32 component_checks=0',
        'POCKET_RUNTIME_VALIDATION_PASS sdk=32 component_checks=0',
        'POCKET_SERVICEMANAGER_READY sdk=32 private_registry=1',
        'API32_FRAMEWORK_BOOT_PASS','API32_SYSTEMSERVER_DEXOPT_PASS stock_zygote_dexopt=1 system_prebuilt_speed=3 apex_verify=3',
        'API32_FRAMEWORK_USER_UNLOCKED','API32_SETTINGS_PROVIDER_PASS',
        'API32_GUEST_MEMORY_EARLY_DEFAULT_PASS constructor_limit=4 current_limit=4 override=-1',
        'API32_BLUETOOTH_ABSENT_PASS','API32_HARDWARE_FEATURES_PASS','API32_CAMERA_SERVICE_PASS',
        'API32_SOFTWARE_DISPLAY_PASS','API32_INPUT_CONTROL_READY',
        'API32_APP_INSTALL_PASS','API32_APP_INSTALL_READY_PASS','API32_APP_ACTIVITY_PASS',
        'API32_APP_SETTINGS_PASS','API32_APP_KEYSTORE_PASS','API32_APP_HTTPS_PASS','API32_APP_RESTART_PASS',
        'API32_HOST_INPUT_PASS ordinary_app_clicks=3','API32_HOST_KEYBOARD_PASS text=a129',
        'API32_UI_DEXOPT_PASS package=com.android.systemui source_parent_writable=false compiler=stock_dex2oat filter=speed',
        'API32_UI_RUNTIME_PASS home=focused_drawn settings=focused_drawn',
        'API32_SYSTEMSERVER_CODE_LOADED_PASS','API32_SYSTEMUI_CODE_LOADED_PASS stage=before',
        'API32_SYSTEMUI_CODE_LOADED_PASS stage=after']
    report['runtimeMissingMarkers']=[m for m in markers if m not in log]
    report['nativeLogWithinCaptureLimit']=len(native.encode('utf-16-le','surrogatepass'))//2<2*1024*1024
    report['runtimeJavaCrashes']=[{'thread':thread,'process':process,'pid':int(pid)}
        for thread,process,pid in re.findall(r'AndroidRuntime:FATAL EXCEPTION: ([^\n]+)\n(?:AndroidRuntime:)?Process: ([^,\n]+), PID: (\d+)',native)]
    report['runtimeJavaFatalHeaders']=re.findall(r'AndroidRuntime:FATAL EXCEPTION: ([^\n]+)',native)
    report['uiStartupCrashes']=[row for row in report['runtimeJavaCrashes'] if row['process'] in ['com.android.systemui','com.android.launcher3']]
    report['runtimeNativeFatals']=re.findall(r'[^\n]*Fatal signal \d+[^\n]*',native)
    report['runtimeAnrs']=re.findall(r'ActivityManager:ANR in ([^\n]+)',native)
    report['runtimeUiAnrs']=re.findall(r'ActivityManager:ANR in (com\.android\.(?:systemui|launcher3))\b[^\n]*',native)
    report['runtimeUiProcesses']={name:sorted({int(pid) for pid in re.findall(
        r'ActivityManager:Start proc (\d+):'+re.escape(name)+r'/',native)})
        for name in ['com.android.systemui','com.android.launcher3']}
    classes=re.findall(r'Zygote:\.\.\.preloaded (\d+) classes in (\d+)ms\.',native)
    resources=re.findall(r'Zygote:\.\.\.preloaded (\d+) resources in (\d+)ms\.',native)
    report['primaryZygotePreload']={'classes':[{'count':int(n),'millis':int(ms)} for n,ms in classes],
        'resources':[{'count':int(n),'millis':int(ms)} for n,ms in resources],
        'resourceFailureObserved':'Failure preloading resources' in native}
    shown=report.get('hostDisplay',{})
    refresh=re.search(r'API32_SOFTWARE_DISPLAY_PASS expected_hz=(\d+) actual_hz=([\d.]+) source=DisplayManager',log)
    display_ok=bool(refresh and shown.get('refreshHz')==int(refresh.group(1)) and abs(float(refresh.group(2))-int(refresh.group(1)))<=0.1)
    input_ok=(shown.get('framesPresented',0)>0 and app_pattern_proof is not None and
        shown.get('touchEventsAccepted')==8 and shown.get('keyEventsAccepted',0)>=12 and shown.get('inputEventsRejected')==0 and
        list(tapped.values())==[3] and len(editor_tapped)==len(keyboard_sent)==1)
    stopped=re.search(r'API32_INIT_STOPPED reaped=(\d+) remaining=0 failed=0',log)
    unexpected=[m for m in ['API32_INIT_CLIENT_PASS','API32_BINDER_PASS','API32_ZYGOTE_PASS','API32_PERFETTO_SESSION_PASS'] if m in log]
    report['componentSuiteExecuted']=bool(unexpected)
    report['runtimeUnexpectedComponentMarkers']=unexpected
    report['passed']=bool(report.get('passed') and report.get('exitCode')==0 and report.get('hostUid',0)>=10000 and
        not report['runtimeMissingMarkers'] and report['nativeLogWithinCaptureLimit'] and
        not report['runtimeJavaFatalHeaders'] and not report['runtimeNativeFatals'] and not report['runtimeAnrs'] and
        all(len(pids)==1 for pids in report['runtimeUiProcesses'].values()) and 'LOG_COLLECTOR_ERROR' not in native and
        any(int(n)>0 for n,_ in classes) and any(int(n)>0 for n,_ in resources) and
        not report['primaryZygotePreload']['resourceFailureObserved'] and display_ok and input_ok and
        set(ui_screenshots)=={'home','settings'} and stopped and int(stopped.group(1))>0 and not unexpected)
    report['runtimeBootVerified']=report['passed']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apk', type=Path, required=True)
    parser.add_argument('--vm-root', type=Path, default=Path.home() / 'pocket-api32-vm')
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
    parser.add_argument('--trace-startup', action='store_true', help='Match an explicitly instrumented startup fixture')
    parser.add_argument('--observe-startup-code', action='store_true', help='Diagnostic only: bounded SystemUI executable-code/scheduling snapshots')
    parser.add_argument('--runtime-boot', action='store_true', help='Validate normal startup; do not claim the component suite ran')
    parser.add_argument('--core-services', action='store_true', help='Require the consolidated UID-0 service core')
    parser.add_argument('--renderer-worker-scheduling',action='store_true',help='Verify normal priority for software-compositor workers')
    parser.add_argument('--require-ui', action='store_true', help='Verify actual Home/Settings windows and stable SystemUI on a fresh boot')
    parser.add_argument('--reuse-installed', action='store_true', help='Skip installation only after hashing the actual installed base APK')
    args = parser.parse_args()
    if args.renderer_worker_scheduling and not args.graphics:parser.error('--renderer-worker-scheduling requires --graphics')
    if args.display and not args.apps:parser.error('--display requires --apps')
    if args.require_ui and not (args.display and args.compile_ui):parser.error('--require-ui requires --display --compile-ui')
    if args.trace_startup and not args.system_server:parser.error('--trace-startup requires --system-server')
    if args.observe_startup_code and not args.system_server:parser.error('--observe-startup-code requires --system-server')
    if args.runtime_boot and (not args.require_ui or args.trace_startup or args.observe_startup_code):
        parser.error('--runtime-boot requires --require-ui and startup diagnostics disabled')
    base = args.vm_root.resolve()
    assert (base / '.pocket-api32-test-vm').is_file(), 'Use the isolated API 32 validation VM'
    config = base / 'runtime/assembly/cuttlefish_config.json'
    assert config.resolve().is_relative_to(base)
    document = json.loads(config.read_text())
    assert set(document['instances']) == {'73'}, 'Refusing any shared or production VM'
    spec = importlib.util.spec_from_file_location('api32_host_vm', base / 'tools/cuttlefish_arm64.py')
    cvd = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(cvd)
    assert cvd.BASE.resolve() == base and cvd.lock()['instance'] == 73
    # All Android mutations follow this independent device/hardware guard.
    cvd.require_virtual()
    def adb(*arguments, timeout=30):
        result = cvd.adb('-s', cvd.serial(), *arguments, check=False, timeout=timeout)
        if result.returncode:
            raise RuntimeError((result.stdout + result.stderr)[-4000:])
        return result.stdout.strip()
    apk = args.apk.resolve()
    metadata = json.loads(apk.with_name('apk-build.json').read_text())
    digest = hashlib.file_digest(apk.open('rb'), 'sha256').hexdigest()
    assert metadata['package'] == PACKAGE and metadata['sdk'] == 32 and metadata['apkSha256'] == digest
    assert metadata['binderRequested'] == args.binder and metadata['apkBytes'] == apk.stat().st_size
    assert metadata.get('zygoteRequested', False) == args.zygote
    assert metadata.get('initRequested', False) == args.init
    assert metadata.get('hardwareRequested', False) == args.hardware
    assert metadata.get('apexRequested', False) == args.apex
    assert metadata.get('installdRequested', False) == args.installd
    assert metadata.get('systemServerRequested', False) == args.system_server
    assert metadata.get('graphicsRequested', False) == args.graphics
    assert metadata.get('rendererWorkerScheduling',0) == (1 if args.renderer_worker_scheduling else 0), 'Software-renderer policy differs; rebuild the fixture'
    assert metadata.get('inputRequested', False) == args.input
    assert metadata.get('networkRequested', False) == args.network
    assert metadata.get('dnsRequested', False) == args.dns
    assert metadata.get('mdnsRequested', False) == args.mdns
    assert metadata.get('audioRequested', False) == args.audio
    assert metadata.get('storageRequested', False) == args.storage
    assert metadata.get('keystoreRequested', False) == args.keystore
    assert metadata.get('appsRequested', False) == args.apps
    assert metadata.get('displayRequested', False) == args.display
    assert metadata.get('compileUiRequested', False) == args.compile_ui
    assert metadata.get('startupTraceRequested') == args.trace_startup, 'Rebuild the fixture to explicitly declare startup tracing'
    assert metadata.get('startupCodeObserveRequested') == args.observe_startup_code, 'Rebuild the fixture to explicitly declare startup code observation'
    assert metadata.get('runtimeBootRequested',False) == args.runtime_boot, 'Runtime boot mode differs from the installed fixture metadata'
    assert metadata.get('coreServicesRequested',False) == args.core_services, 'Native core mode differs from the installed fixture metadata'
    assert metadata.get('coreServicesLayout',0) == (2 if args.core_services else 0), 'Native service layout differs; rebuild the fixture'
    host_selinux_before=adb('shell','getenforce')
    if args.network:
        connectivity=adb('shell','dumpsys connectivity')
        active=re.search(r'^Active default network: (\d+)\s*$',connectivity,re.M)
        assert active, 'The isolated Android host needs an active uplink (connect its VirtWifi test network)'
        print('Host default network verified: '+active.group(1),flush=True)
    if args.reuse_installed:
        paths=adb('shell','pm path '+PACKAGE).splitlines()
        assert len(paths)==1 and paths[0].startswith('package:/data/app/') and paths[0].endswith('/base.apk')
        installed=paths[0].removeprefix('package:')
        installed_digest=adb('shell','sha256sum '+shlex.quote(installed),timeout=90).split()[0]
        assert installed_digest==digest, 'Installed APK differs; omit --reuse-installed to install the candidate'
        print('Reusing installed APK with matching SHA-256',flush=True)
    else:
        print(adb('install', '--no-incremental', '-r', str(apk), timeout=180), flush=True)
    flags = adb('shell', 'dumpsys package ' + PACKAGE)
    assert 'DEBUGGABLE' not in flags, 'The foundation must run in a non-debuggable app sandbox'
    adb('shell', 'am force-stop ' + PACKAGE)
    run_id = uuid.uuid4().hex
    if args.display:
        adb('shell','input keyevent KEYCODE_WAKEUP')
        adb('shell','wm dismiss-keyguard')
    print(adb('shell', 'am start -n ' + PACKAGE + '/.MainActivity --es probe_run_id ' + run_id +
              (' --ez probe_display true' if args.display else '')+(' --ez probe_ui true' if args.require_ui else '')), flush=True)
    deadline = time.monotonic() + (780 if args.apps else 420)
    tapped={};editor_tapped=set();keyboard_sent=set();input_actions=[];stable_target=None;stable_since=0
    display_state={};screenshot=None;read_retries=0;host_process_pid=None
    ui_screenshots={};ui_phase=None;ui_seen=0;app_pattern_proof=None
    def payload(dump,key):
        matches=[line.strip().split('=',1)[1] for line in dump.splitlines() if line.strip().startswith(key+'=')]
        if not matches:return {}
        try:return json.loads(base64.b64decode(matches[-1],validate=True))
        except (ValueError,UnicodeDecodeError):return {}
    while time.monotonic() < deadline:
        if args.display:
            observed=payload(adb('shell','dumpsys activity '+PACKAGE+'/.MainActivity pocket-api32-display'),'POCKET_API32_DISPLAY')
            if observed.get('runId')==run_id:
                display_state=observed.get('display',{});ready=display_state.get('inputReady',{});nonce=ready.get('nonce')
                if ready.get('runId')==run_id and nonce and ready.get('targetToken') and display_state.get('matchedNonce')==nonce and display_state.get('matchedTargetToken')==ready['targetToken'] and display_state.get('hostPatternMatches')==16:
                    if display_state.get('guestPatternMatches')==16:
                        app_pattern_proof={k:display_state.get(k) for k in ['matchedNonce','matchedTargetToken','guestPatternMatches','hostPatternMatches','pixelCopyStatus','framesPresented']}
                    target=(ready['targetToken'],display_state.get('viewX'),display_state.get('viewY'),display_state.get('viewWidth'),display_state.get('viewHeight'))
                    if target!=stable_target:
                        stable_target=target;stable_since=time.monotonic()
                    elif time.monotonic()-stable_since>=2:
                        sent=tapped.get(nonce,0);clicks=ready.get('clicks',0)
                        assert 0<=clicks<=sent<=3, 'Guest click feedback is duplicated or from another input source'
                        kind='button' if clicks==sent and sent<3 else 'editor' if clicks==3 and nonce not in editor_tapped else None
                        if kind:
                            w,h=display_state.get('width',0),display_state.get('height',0)
                            vw,vh=display_state.get('viewWidth',0),display_state.get('viewHeight',0)
                            gx,gy=ready[kind+'X'],ready[kind+'Y']
                            assert w>0 and h>0 and vw>0 and vh>0 and 0<=gx<w and 0<=gy<h
                            x=int(display_state['viewX']+gx*vw/w);y=int(display_state['viewY']+gy*vh/h)
                            if screenshot is None:
                                screenshot=base/('api32-host-display-'+run_id+'.png')
                                adb('shell','screencap -p /data/local/tmp/pocket-api32-host.png')
                                adb('pull','/data/local/tmp/pocket-api32-host.png',str(screenshot))
                                # Capturing a screenshot takes time; require the same
                                # current layout and surface bounds immediately after it.
                                again=payload(adb('shell','dumpsys activity '+PACKAGE+'/.MainActivity pocket-api32-display'),'POCKET_API32_DISPLAY')
                                shown=again.get('display',{});current=shown.get('inputReady',{})
                                after=(current.get('targetToken'),shown.get('viewX'),shown.get('viewY'),shown.get('viewWidth'),shown.get('viewHeight'))
                                if again.get('runId')!=run_id or after!=target or shown.get('matchedTargetToken')!=current.get('targetToken'):
                                    stable_target=None;continue
                            adb('shell','input tap '+str(x)+' '+str(y))
                            input_actions.append({'kind':kind,'nonce':nonce,'targetToken':ready['targetToken'],'guestX':gx,'guestY':gy,'hostX':x,'hostY':y,'clicksBefore':clicks})
                            if kind=='button':tapped[nonce]=sent+1
                            else:editor_tapped.add(nonce)
                            print('Host surface pattern verified; '+kind+' tap '+str(x)+','+str(y)+'; acknowledged clicks='+str(clicks),flush=True)
                        elif clicks==3 and nonce in editor_tapped and ready.get('textFocused') and nonce not in keyboard_sent:
                            adb('shell','input text a12b')
                            adb('shell','input keyevent KEYCODE_DEL')
                            adb('shell','input text 9');keyboard_sent.add(nonce)
                            input_actions.append({'kind':'keyboard','nonce':nonce,'typed':'a12b','deleted':1,'appended':'9','expectedText':'a129'})
                            print('Host keyboard sequence sent; awaiting actual guest EditText feedback',flush=True)
                else:stable_target=None
            else:stable_target=None
            if args.require_ui:
                phase=display_state.get('uiReady',{});name=phase.get('phase')
                if observed.get('runId')==run_id and phase.get('runId')==run_id and phase.get('windowDrawn') and name in ['home','settings']:
                    if name!=ui_phase:ui_phase=name;ui_seen=time.monotonic()
                    elif name not in ui_screenshots and time.monotonic()-ui_seen>=1:
                        target=base/('api32-host-ui-'+name+'-'+run_id+'.png')
                        adb('shell','screencap -p /data/local/tmp/pocket-api32-ui.png')
                        adb('pull','/data/local/tmp/pocket-api32-ui.png',str(target))
                        after=payload(adb('shell','dumpsys activity '+PACKAGE+'/.MainActivity pocket-api32-display'),'POCKET_API32_DISPLAY')
                        current=after.get('display',{}).get('uiReady',{})
                        if after.get('runId')==run_id and current==phase:
                            ui_screenshots[name]={'path':str(target),'sha256':hashlib.file_digest(target.open('rb'),'sha256').hexdigest(),'phase':phase,'framesPresented':display_state.get('framesPresented',0)}
                            print('Captured actual host '+name+' surface; guest window reports drawn',flush=True)
                else:ui_phase=None
        dump = adb('shell', 'dumpsys activity ' + PACKAGE + '/.MainActivity pocket-api32-report')
        report = payload(dump,'POCKET_API32_REPORT')
        if not report:read_retries+=1
        if report.get('runId') == run_id:
            break
        process=cvd.adb('-s',cvd.serial(),'shell','pidof '+PACKAGE,check=False,timeout=15)
        current_pid=process.stdout.strip()
        if process.returncode==0 and current_pid:
            if host_process_pid is None:host_process_pid=current_pid
            process_exited=current_pid!=host_process_pid
        else:
            # A transport error or observation timeout is not an exit. pidof's
            # clean status 1 means the previously observed main process is gone.
            process_exited=bool(host_process_pid and process.returncode==1 and not current_pid and not process.stderr.strip())
        if process_exited:
            exit_info=adb('shell','dumpsys activity exit-info '+PACKAGE)
            uid=re.search(r'\buserId=(\d+)',flags)
            report={'runId':run_id,'passed':False,'exitCode':None,'hostUid':int(uid.group(1)) if uid else 0,
                    'error':'Observed host test process exited or was replaced before its terminal report',
                    'hostProcessPid':host_process_pid,'replacementPid':current_pid or None,
                    'hostApplicationExitInfo':exit_info,'hostDisplay':display_state}
            print('Observed host process exit; preserving Android ApplicationExitInfo without restarting',flush=True)
            break
        time.sleep(2)
    else:
        raise RuntimeError('The isolated API 32 fixture did not finish within its validation deadline')
    if getattr(args,'runtime_boot',False):
        validate_runtime_report(report,app_pattern_proof,tapped,editor_tapped,keyboard_sent,ui_screenshots)
    else:
        markers = ['API32_NATIVE_RUNTIME_PASS', 'REAL_ANDROID12L_ART_PASS', 'API32_LINKER_CONFIG_GENERATED',
                   'LOADER_SIGNAL_DURING_TRAP_PASS','LOADER_READONLY_VIEW_PASS']
        if args.binder:
            markers += ['API32_BINDER_PASS', 'API32_REGISTRY_TYPED_RESULTS_PASS', 'API32_WORKSOURCE', 'API32_ONEWAY_CALLER_PASS', 'API32_DUMP_IDENTITY_PASS uid=0', 'API32_DUMP_IDENTITY_PASS uid=1000', 'API32_DUMP_IDENTITY_PASS uid=39999']
        if args.compile_ui:markers += ['API32_UI_DEXOPT_PASS package=com.android.systemui source_parent_writable=false compiler=stock_dex2oat filter=speed']
        native_markers = []
        if args.zygote:
            markers += ['API32_ZYGOTE_PASS', 'API32_JAVA_CHILD_PASS', 'API32_ZYGOTE_NATIVE_REGISTERED', 'API32_CPU_ABI_PROPERTIES_PASS',
                        'API32_JCA_FONT_PRELOAD_PASS', 'crypto=aes_gcm fonts=1','API32_FRAMEWORK_BOOT_IMAGES_PASS']
            # Stock ZygoteInit redirects child System.out through liblog. This
            # marker comes from the newly specialized Java child, not init stdout.
            native_markers += ['API32_PROCESS_IDENTITY_PASS']
        if args.init:
            markers += ['API32_INIT_PASS', 'API32_INIT_CLIENT_PASS', 'API32_INIT_SHUTDOWN_PASS', 'API32_INIT_CONFIG_REJECT_PASS']
        if args.hardware:
            markers += ['API32_HARDWARE_PASS', 'API32_HOST_BATTERY_PASS', 'API32_HOST_WAKE_LOCK_PASS',
                        'API32_BINDER_ENDPOINTS_COEXIST_PASS', 'API32_FRAMEWORK_HIDL_POLICY_PASS',
                        'API32_FRAMEWORK_HIDL_ROLE_DENIED_PASS', 'API32_SUSPEND_CONTROL_READY',
                        'API32_SUSPEND_CONTROL_PASS', 'API32_SUSPEND_ROLE_DENIED_PASS']
            report['suspendControlServiceWaits']=len(re.findall(
                r'Waited one second for suspend_control(?:_internal)?',report.get('nativeLog','')))
            if report['suspendControlServiceWaits']:report['passed']=False
        if args.apex:
            markers += ['API32_APEX_PASS', 'API32_APEX_ROLE_DENIED_PASS','API32_APEX_DATA_PASS']
        if args.storage:
            markers += ['API32_STORAGE_PASS','API32_STORAGE_MAINTENANCE_PASS','API32_STORAGE_ROLE_DENIED_PASS']
        if args.keystore:
            markers += ['API32_SOFTWARE_KEYMASTER_PASS','API32_KEYSTORE_CONTROL_PASS',
                        'API32_KEYSTORE_ROLE_DENIED_PASS','API32_KEYSTORE_CRYPTO_PASS']
            if args.system_server:
                markers += ['API32_GATEKEEPER_PASS','API32_GATEKEEPER_ROLE_DENIED_PASS']
        if args.installd:
            markers += ['API32_INSTALLD_PASS', 'API32_INSTALLD_CREATE_PASS', 'API32_INSTALLD_CLEAR_PASS',
                        'API32_INSTALLD_ROLE_DENIED_PASS', 'API32_INSTALLD_DEXOPT_PASS', 'DIRECT_DATA_PREPARED']
        if args.system_server:
            markers += ['API32_FRAMEWORK_PREPARED','API32_FRAMEWORK_BOOT_PASS','API32_FRAMEWORK_HIDL_CLIENT_PASS',
                        'API32_PRIMARY_ZYGOTE_PRELOAD_PASS protocol=stock preload_was_complete=1',
                        'API32_SYSTEMSERVER_DEXOPT_PASS stock_zygote_dexopt=1 system_prebuilt_speed=3 apex_verify=3',
                        'API32_GUEST_MEMORY_PROFILE_PASS cached_process_limit=4',
                        'API32_GUEST_MEMORY_EARLY_DEFAULT_PASS constructor_limit=4 current_limit=4 override=-1',
                        'API32_UEVENT_CHANNEL_PASS','API32_UEVENT_JNI_PASS','API32_VINTF_KERNEL_PASS',
                        'API32_FRAMEWORK_USER_UNLOCKED','API32_SETTINGS_PROVIDER_PASS','API32_ART_TRACE_PASS',
                        'API32_TOMBSTONED_JAVA_PASS','API32_TOMBSTONED_PERSIST_PASS','API32_PERFETTO_SESSION_PASS',
                        'API32_PERFETTO_UNREGISTERED_DENIED_PASS',
                        'API32_SERVICE_TIMER_PASS','API32_SERVICE_TIMER_UID_SCOPE_PASS','API32_SERVICE_TIMER_NAME_SCOPE_PASS','API32_SERVICE_SCHEDULER_PASS',
                        'API32_BLUETOOTH_ABSENT_PASS',
                        'API32_CAMERA_HIDL_PASS','API32_CAMERA_SERVICE_PASS','API32_CAMERA_HIDL_ROLE_DENIED_PASS','API32_HARDWARE_FEATURES_PASS',
                        'API32_TOMBSTONED_ROLE_DENIED_PASS','API32_TOMBSTONED_OUTSIDE_PROFILE_DENIED_PASS']
            if args.graphics:markers += ['API32_SOFTWARE_DISPLAY_PASS']
            if args.observe_startup_code:
                markers += ['API32_STARTUP_CODE_SNAPSHOT']
                snapshots=[]
                for line in report.get('log','').splitlines():
                    if not line.startswith('API32_STARTUP_CODE_SNAPSHOT '):continue
                    try:snapshots.append({k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line)})
                    except ValueError:continue
                report['startupCodeSnapshots']=snapshots
                if not snapshots or any(row.get('maps_readable')!=1 or row.get('main_schedstat')!=1 for row in snapshots):
                    report['passed']=False
            class_preloads=re.findall(r'Zygote:\.\.\.preloaded (\d+) classes in (\d+)ms\.',report.get('nativeLog',''))
            resource_preloads=re.findall(r'Zygote:\.\.\.preloaded (\d+) resources in (\d+)ms\.',report.get('nativeLog',''))
            report['primaryZygotePreload']={'classes':[{'count':int(n),'millis':int(ms)} for n,ms in class_preloads],
                'resources':[{'count':int(n),'millis':int(ms)} for n,ms in resource_preloads],
                'beginDebugObserved':'Zygote:begin preload' in report.get('nativeLog',''),
                'endDebugObserved':'Zygote:end preload' in report.get('nativeLog',''),
                'resourceFailureObserved':'Failure preloading resources' in report.get('nativeLog','')}
            if (not any(int(n)>0 for n,_ in class_preloads) or not any(int(n)>0 for n,_ in resource_preloads) or
                    report['primaryZygotePreload']['resourceFailureObserved']):report['passed']=False
        if args.apps:
            markers += ['API32_APP_INSTALL_PASS','API32_APP_INSTALL_READY_PASS','API32_APP_ACTIVITY_PASS','API32_APP_SETTINGS_PASS',
                        'API32_APP_KEYSTORE_PASS','API32_APP_HTTPS_PASS','API32_APP_RESTART_PASS']
        if args.display:
            markers += ['API32_INPUT_CONTROL_READY','API32_INPUT_GUEST_PEER_DENIED_PASS','API32_HOST_INPUT_PASS ordinary_app_clicks=3','API32_HOST_KEYBOARD_PASS text=a129']
            shown=report.get('hostDisplay',{})
            if args.system_server:
                refresh=re.search(r'API32_SOFTWARE_DISPLAY_PASS expected_hz=(\d+) actual_hz=([\d.]+) source=DisplayManager',report.get('log',''))
                if (not refresh or shown.get('refreshHz')!=int(refresh.group(1)) or
                        abs(float(refresh.group(2))-int(refresh.group(1)))>0.1):report['passed']=False
            if not (shown.get('framesPresented',0)>0 and (app_pattern_proof is not None if args.require_ui else shown.get('guestPatternMatches')==16 and shown.get('hostPatternMatches')==16) and
                    shown.get('touchEventsAccepted')==8 and shown.get('keyEventsAccepted',0)>=12 and shown.get('inputEventsRejected')==0 and list(tapped.values())==[3] and len(editor_tapped)==len(keyboard_sent)==1):report['passed']=False
        if args.require_ui:
            markers += ['API32_UI_RUNTIME_PASS home=focused_drawn settings=focused_drawn','API32_SYSTEMSERVER_CODE_LOADED_PASS',
                        'API32_SYSTEMUI_CODE_LOADED_PASS stage=before', 'API32_SYSTEMUI_CODE_LOADED_PASS stage=after']
            report['nativeLogWithinCaptureLimit']=len(report.get('nativeLog','').encode('utf-16-le','surrogatepass'))//2<2*1024*1024
            if not report['nativeLogWithinCaptureLimit']:report['passed']=False
            report['uiStartupCrashes']=[{'thread':thread,'process':process,'pid':int(pid)}
                for thread,process,pid in re.findall(
                    r'AndroidRuntime:FATAL EXCEPTION: ([^\n]+)\n(?:AndroidRuntime:)?Process: (com\.android\.(?:systemui|launcher3)), PID: (\d+)',
                    report.get('nativeLog',''))]
            if report['uiStartupCrashes']:report['passed']=False
            if set(ui_screenshots)!={'home','settings'} or any('ANR in '+name in report.get('nativeLog','') for name in ['com.android.systemui','com.android.launcher3']):report['passed']=False
        if args.graphics:
            markers += ['API32_GRALLOC_PASS','API32_COMPOSER_PASS','DIRECT_EGL_PASS','API32_GRAPHICS_PASS']
        if args.input:
            markers += ['API32_INPUT_PASS','API32_INPUT_WAKE_DEATH_PASS']
        if args.network:
            markers += ['API32_NETD_PASS','API32_NETD_POLICY_PASS','API32_NETD_ROLE_DENIED_PASS','API32_NETD_STACK_ROLE_PASS','API32_FWMARK_PASS',
                        'API32_NETD_PACKET_BLOCK stage=socket','API32_MEMORY_POLICY_PASS','API32_MEMORY_ROLE_DENIED_PASS']
        if args.dns:
            markers += ['API32_DNS_CONTROL_PASS','API32_DNS_ROLE_DENIED_PASS','API32_DNS_RESOLVE_PASS','API32_DNS_PERMISSION_DENIED_PASS',
                        'API32_DNS_HOST_DIRECT_DENIED_PASS','API32_DNS_LISTENER_DEATH_PASS']
        if args.mdns:
            markers += ['API32_MDNS_PASS','API32_MDNS_ROLE_DENIED_PASS']
        if args.audio:
            markers += ['API32_AUDIO_HAL_PASS','API32_AUDIO_SERVICES_PASS','API32_AUDIO_ROLE_DENIED_PASS']
            host=report.get('audioHost',{})
            if host.get('hostPcmBytesWritten',0)<96000 or host.get('completedStreams',0)<1 or host.get('streamErrors')!=0:
                report['passed']=False
        report['passed'] = bool(report.get('passed') and report.get('hostUid', 0) >= 10000 and
                                all(marker in report.get('log', '') for marker in markers) and
                                all(marker in report.get('nativeLog', '') for marker in native_markers))
    if args.core_services:
        if not args.runtime_boot and 'API32_SCRYPT_KAT_PASS vectors=3 backend=pinned_aosp_arm64' not in report.get('log',''):
            report['passed']=False
        core_rows=[tuple(map(int,row)) for row in re.findall(r'POCKET_CORE_START pid=(\d+) uid=(\d+) components=(\d+) test_echo=(\d+)',report.get('log',''))]
        report['nativeCoreStarts']=[dict(zip(['pid','uid','components','testEcho'],row)) for row in core_rows]
        if (len(core_rows)!=1 or core_rows[0][0]<=1 or core_rows[0][1:]!=(0,8,int(not args.runtime_boot)) or
                'POCKET_CORE_COMPONENT_EXIT' in report.get('log','')):report['passed']=False
        hardware_pids=[int(pid) for pid in re.findall(r'DIRECT_HWB_REGISTRY_READY status=0 pid=(\d+) kernel_uid=',report.get('log',''))]
        report['nativeHardwareRegistryPids']=hardware_pids
        if not core_rows or hardware_pids!=[core_rows[0][0]]:report['passed']=False
        crypto_rows=[tuple(map(int,row)) for row in re.findall(r'POCKET_CRYPTO_START pid=(\d+) uid=(\d+) gid=(\d+) components=(\d+)',report.get('log',''))]
        report['nativeCryptoStarts']=[dict(zip(['pid','uid','gid','components'],row)) for row in crypto_rows]
        if (len(crypto_rows)!=1 or crypto_rows[0][0]<=1 or crypto_rows[0][1:]!=(1000,1000,2) or
                (core_rows and crypto_rows[0][0]==core_rows[0][0]) or
                'POCKET_CRYPTO_READY ' not in report.get('log','')):report['passed']=False
        registrations={name:[int(pid) for pid in re.findall(
            r'API32_FRAMEWORK_HIDL_REGISTERED descriptor='+re.escape(name)+r' instance=default caller_pid=(\d+)',report.get('log',''))]
            for name in ['android.hardware.keymaster@4.1::IKeymasterDevice','android.hardware.gatekeeper@1.0::IGatekeeper']}
        report['nativeCryptoRegistrationPids']=registrations
        if not crypto_rows or any(pids!=[crypto_rows[0][0]] for pids in registrations.values()):report['passed']=False
    if args.renderer_worker_scheduling:
        workers=[{'pid':int(pid),'tid':int(tid),'before':int(before),'after':int(after),'error':int(error),'library':library}
            for pid,tid,before,after,error,library in re.findall(
                r'POCKET_RENDER_WORKER_NICE pid=(\d+) tid=(\d+) before=(-?\d+) after=(-?\d+) error=(\d+) library=(GLESv2|EGL)',report.get('log',''))]
        report['rendererWorkerPriorities']=workers
        sf_pids=[int(pid) for pid in re.findall(r'INIT_CHILD service=pocket-surfaceflinger pid=(\d+)',report.get('log',''))]
        report['rendererWorkersLowered']=sum(w['before']<0 and w['after']==0 and not w['error'] for w in workers)
        if (len(sf_pids)!=1 or not workers or not report['rendererWorkersLowered'] or
                any(w['pid']!=sf_pids[0] or w['error'] or w['after']!=(0 if w['before']<0 else w['before']) for w in workers)):
            report['passed']=False
    host_selinux_after=adb('shell','getenforce')
    report['passed'] = bool(report['passed'] and host_selinux_before==host_selinux_after)
    report.update(androidAppSandboxVerified=report['passed'], fullGuestBootVerified=False,
                  guestSdk=32, binderRequested=args.binder, zygoteRequested=args.zygote, debuggable=False, apkSha256=digest,
                  initRequested=args.init,
                  hardwareRequested=args.hardware,
                  apexRequested=args.apex,
                  installdRequested=args.installd,
                  systemServerRequested=args.system_server,
                  graphicsRequested=args.graphics,
                  rendererWorkerScheduling=1 if args.renderer_worker_scheduling else 0,
                  inputRequested=args.input,
                  networkRequested=args.network,
                  dnsRequested=args.dns,
                  mdnsRequested=args.mdns,
                  audioRequested=args.audio,
                  storageRequested=args.storage,
                  keystoreRequested=args.keystore,
                  appsRequested=args.apps,
                  displayRequested=args.display,hostScreenshotPath=str(screenshot) if screenshot else None,
                  hostScreenshotSha256=hashlib.file_digest(screenshot.open('rb'),'sha256').hexdigest() if screenshot else None,
                  compileUiRequested=args.compile_ui,uiRequested=args.require_ui,uiScreenshots=ui_screenshots,hostAppPatternProof=app_pattern_proof,
                  startupTraceRequested=args.trace_startup,
                  startupCodeObserveRequested=args.observe_startup_code,
                  runtimeBootRequested=args.runtime_boot,
                  coreServicesRequested=args.core_services,
                  coreServicesLayout=2 if args.core_services else 0,
                  hostInputActions=input_actions,
                  reportReadRetries=read_retries,
                  hostSelinuxBefore=host_selinux_before,hostSelinuxAfter=host_selinux_after,
                  reusedInstalledApk=args.reuse_installed,
                  hostVmInstance=73, hostSdk=int(adb('shell', 'getprop ro.build.version.sdk')),
                  imageManifestSha256=metadata.get('imageManifestSha256'), thinUpdate=metadata.get('thinUpdate',False),
                  deltaUpdate=metadata.get('deltaUpdate',False),
                  deltaBaselineManifestSha256=metadata.get('deltaBaselineManifestSha256'),
                  scope='API 32 native/ART foundation in a non-debuggable Android app; no framework or Manager boot claim')
    if args.runtime_boot:
        report['scope']='Normal API 32 runtime startup in the isolated non-debuggable host; separate post-boot acceptance checks, no component-suite or production Manager claim'
    destination = base / 'api32-android-validation.json'
    destination.write_text(json.dumps(report, indent=2) + '\n')
    # Keep stdout bounded. SSH stdout/stderr are separate channels; a fatal
    # assertion can otherwise interleave a traceback into a large JSON line.
    # The complete authoritative report is already on disk above.
    summary={key:report.get(key) for key in ['runId','passed','exitCode','error','hostUid',
        'guestSdk','hostSdk','apkSha256','fullGuestBootVerified','graphicsRequested','systemServerRequested']}
    summary['reportPath']=str(destination)
    print(json.dumps(summary, indent=2), flush=True)
    assert report['passed'], 'API 32 Android sandbox foundation failed; see ' + str(destination)


if __name__ == '__main__':
    main()
