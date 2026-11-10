#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Real guest MediaProvider session trial in the retained VM73 plaintext profile."""
import argparse,base64,hashlib,importlib.util,json,re,shlex,time,uuid,zipfile
from pathlib import Path
from collections import Counter
PACKAGE='dev.kreijstal.aosppocket.api32managerimage'
ACTIVITY=PACKAGE+'/dev.kreijstal.aosppocket.runtime.ManagerMediaSessionProbe32'

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--work-dir',type=Path,required=True);p.add_argument('--require-external-files',action='store_true');p.add_argument('--require-external-paths',action='store_true');p.add_argument('--require-external-namespace',action='store_true');p.add_argument('--baseline-report',type=Path);a=p.parse_args();a.require_external_files=a.require_external_files or a.require_external_paths or a.require_external_namespace;work=a.work_dir.resolve();base=Path('/home/pi/pocket-api32-vm')
    assert work.is_relative_to(base) and (base/'.pocket-api32-test-vm').is_file()
    assert set(json.loads((base/'runtime/assembly/cuttlefish_config.json').read_text())['instances'])=={'73'}
    s=importlib.util.spec_from_file_location('vm',base/'tools/cuttlefish_arm64.py');vm=importlib.util.module_from_spec(s);s.loader.exec_module(vm)
    assert vm.BASE.resolve()==base and vm.lock()['instance']==73;vm.require_virtual()
    baseline=(a.baseline_report or base/'manager-app-validation/manager-app-report.json').resolve();assert baseline.is_relative_to(base)
    prior=json.loads(baseline.read_text());assert prior['validation']=='PASS' and prior['strictPassed']
    def adb(*args,timeout=40):
        r=vm.adb('-s',vm.serial(),*args,check=False,timeout=timeout)
        if r.returncode:raise RuntimeError((r.stdout+r.stderr)[-2000:])
        return r.stdout.strip()
    ps=adb('shell','ps -A -o PID,NAME,ARGS');assert not any(any(n in row for n in ['pocket-init-api32','pocket-core-api32','fuse-stock']) for row in ps.splitlines())
    assert 'package:'+PACKAGE in adb('shell','pm list packages --user 0 '+PACKAGE).splitlines(),'Preserved Manager profile must already exist'
    apk=work/'api32-media-manager-probe.apk';build=json.loads((work/'build.json').read_text());digest=hashlib.file_digest(apk.open('rb'),'sha256').hexdigest();assert digest==build['sha256'] and (build['hostNativeLibrariesUnchanged'] or build.get('hostNativeSourceBuild'))
    with zipfile.ZipFile(apk) as z:
        if build.get('hostNativeSourceBuild'):
            libraries=build['hostNativeSourceBuild']['libraries']
            assert set(libraries)=={'libpocket_runtime.so','libproot.so','libpocket_frames.so','libpocket_ce.so'}
            for name,record in libraries.items():assert hashlib.sha256(z.read('lib/arm64-v8a/'+name)).hexdigest()==record['sha256']
        catalog=json.loads(z.read('assets/pocket-images.json'));image=next(i for i in catalog['images'] if i['sdk']==32)
        assert image['sha256']==prior['imageSha256'] and re.fullmatch('[0-9a-f]{64}',image['sha256'])
        if a.require_external_files:
            assert hashlib.sha256(z.read('lib/arm64-v8a/libpocket_external32.so')).hexdigest()==build['externalFileJniAdditionSha256']
            assert json.loads(z.read('assets/api32-media-proxy.json'))['externalProxy'] is True
        if a.require_external_namespace:
            assert build.get('externalNamespaceTrial') is True
            assert json.loads(z.read('assets/api32-media-proxy.json'))['externalNamespace'] is True
            assert b'MEDIA_NAMESPACE32_PASS' in z.read('assets/api32-media-client')
        if build.get('lazyMediaProbeSha256'):
            assert hashlib.sha256(z.read('assets/api32-lazy-media-probe')).hexdigest()==build['lazyMediaProbeSha256']
        if build.get('metricsProbeSha256'):
            assert hashlib.sha256(z.read('assets/api32-metrics-probe')).hexdigest()==build['metricsProbeSha256']
    report={'runId':uuid.uuid4().hex,'vmInstance':73,'apkSha256':digest,'imageSha256':image['sha256'],'baselineReport':str(baseline),'baselineRunId':prior['runId']}
    settings=[('selinux','getenforce'),('phantomLimit','device_config get activity_manager max_phantom_processes'),('phantomMonitoring','settings get global settings_enable_monitor_phantom_procs')]
    for key,cmd in settings:report[key+'Before']=adb('shell',cmd)
    assert report['selinuxBefore']=='Enforcing'
    # Updating the foreground fixture removes its activity. Keep the launcher
    # in front so Android cannot resume/recreate an older probe task underneath
    # and accidentally boot another retained profile during this test.
    adb('shell','am start -W -a android.intent.action.MAIN -c android.intent.category.HOME')
    print(adb('install','--no-incremental','-r',str(apk),timeout=180),flush=True)
    assert 'DEBUGGABLE' not in adb('shell','dumpsys package '+PACKAGE)
    installed=adb('shell','pm path '+PACKAGE).splitlines();assert len(installed)==1
    assert adb('shell','sha256sum '+shlex.quote(installed[0][8:])).split()[0]==digest
    # No data clear/uninstall or guest-owner kill: this updates the stopped host app only.
    # The fixture sets show-when-locked and turn-screen-on itself. Launch it
    # directly so waking the VM cannot first resume a different test profile.
    adb('shell','am force-stop '+PACKAGE)
    try:
        print(adb('shell','am start -n '+ACTIVITY+' --es probe_run_id '+report['runId']),flush=True)
        deadline=time.monotonic()+1400;last=None;errors=0
        while time.monotonic()<deadline:
            try:text=adb('shell','dumpsys activity '+ACTIVITY+' pocket-media32')
            except Exception:errors+=1;time.sleep(2);continue
            rows=[l.strip().split('=',1)[1] for l in text.splitlines() if l.strip().startswith('POCKET_SESSION32=')]
            state=json.loads(base64.b64decode(rows[-1],validate=True)) if rows else {}
            if state.get('runId')!=report['runId']:time.sleep(1);continue
            current=state.get('stage'),state.get('managerStatus')
            if current!=last:print('MEDIA32_STAGE',*current,flush=True);last=current
            if state.get('validation'):report['app']=state;break
            time.sleep(1)
        else:raise TimeoutError('Observation expired; inspect exact host/guest processes before any retry')
        report['pollErrors']=errors
        for key,cmd in settings:report[key+'After']=adb('shell',cmd);assert report[key+'After']==report[key+'Before']
        native=state.get('nativeLog','');console=state.get('console','');media=state.get('mediaSession',{})
        report['systemUiStarts']=re.findall(r'Start proc (\d+):com\.android\.systemui/',native)
        report['anrs']=re.findall(r'ANR in ([^\n]+)',native);report['javaFatals']=re.findall(r'FATAL EXCEPTION(?: IN SYSTEM PROCESS)?: ([^\n]+)',native);report['nativeFatals']=re.findall(r'Fatal signal [^\n]+',native)
        report['initStop']=re.findall(r'API32_INIT_STOPPED reaped=(\d+) remaining=(\d+) failed=(\d+)',console)
        spawned=re.findall(r'(?m)^INIT_CHILD service=(\S+) pid=(\d+)$',console)
        reaped=re.findall(r'(?m)^INIT_REAP service=(\S+) pid=(\d+) ',console)
        report['supervisedLifetimeCount']=len(spawned)
        report['reapAccounting']=bool(spawned and Counter(spawned)==Counter(reaped)
            and len(report['initStop'])==1 and int(report['initStop'][0][0])==len(spawned))
        if image.get('child_supervision')=='kernel-subreaper':
            descendants=re.findall(r'API32_INIT_DESCENDANTS_STOPPED reaped=(\d+) signalled=(\d+) remaining=(\d+) query_error=(-?\d+)',console)
            report['descendantStop']=descendants
            report['kernelChildOwnershipVerified']=bool(console.count('API32_INIT_SUBREAPER_READY kernel_child_ownership=1')==1
                and len(descendants)==1 and descendants[0][2:]==('0','0')
                and int(descendants[0][0])==len(re.findall(r'(?m)^INIT_DESCENDANT_REAP pid=\d+ code=\d+ status=\d+$',console))
                and 'API32_INIT_DESCENDANT_QUERY_FAILED' not in console)
        report['functionalPassed']=bool(media.get('validation')=='PASS' and media.get('uid',0)>=10000 and media.get('startCallback') and media.get('endCallback') and media.get('nativeExit')==0 and state.get('originalCodeRestored'))
        if a.require_external_namespace:
            report['externalNamespaceVerified']=bool(media.get('namespaceProof') and 'MEDIA_NAMESPACE32_PASS provider=real' in media.get('nativeLog',''))
            report['functionalPassed']=report['functionalPassed'] and report['externalNamespaceVerified']
        if build.get('packagedImageTrial'):
            before=state.get('packagedCodeBefore',{});after=state.get('packagedCodeAfter',{})
            report['packagedImageVerified']=bool(state.get('packagedImageTrial') and state.get('originalCodeUnchanged')
                and state.get('preservedImageId')==image['sha256'] and len(before)==13 and before==after
                and state.get('guestDataReset') is False and state.get('codePatchOnly') is False
                and state.get('guestPatchPayloadSha256')=={} and build.get('guestCodePatchSha256')=={}
                and all(marker in native for marker in ['POCKET_MICROG_PUBLICATION_READY','POCKET_MICROG_PACKAGE_READY','POCKET_MICROG_APP_CACHE_READY']))
            report['functionalPassed']=report['functionalPassed'] and report['packagedImageVerified']
            player=console.find('API32_LAZY_SERVICE_PUBLISHED name=media.player ')
            framework=console.find('INIT_CHILD service=pocket-framework ')
            report['playerBootStartVerified']=bool(player>=0 and player<framework
                and 'POCKET_RUNTIME_SERVICE_READY name=media.player' in console
                and 'API32_LAZY_BINDER_READY name=media.player cold=0 ' in state.get('lazyMediaActivation',{}).get('log',''))
            report['functionalPassed']=report['functionalPassed'] and report['playerBootStartVerified']
        if a.require_external_files:
            data=media.get('dataProof',{});proxy=state.get('externalFiles',{});nativeData=media.get('nativeLog','')
            report['externalFilesVerified']=bool(state.get('backingDataVerified') and data.get('readWrite') and data.get('sharedMmap') and data.get('readOnlyDenied') and data.get('hostRpcImpersonationDenied') and data.get('uid')==media.get('uid')
                and set(proxy.get('callerUids',[]))=={media.get('uid')} and data.get('mainTid') in proxy.get('readTids',[]) and data.get('workerTid') in proxy.get('readTids',[]) and data.get('mainTid')!=data.get('workerTid')
                and proxy.get('releasedFiles')==(3 if a.require_external_paths else 2) and proxy.get('remainingSlots')==0 and 'MEDIA_PROXY32_PASS' in nativeData and 'remaining_files=0' in nativeData)
            report['functionalPassed']=report['functionalPassed'] and report['externalFilesVerified']
            if a.require_external_paths:
                report['ordinaryPathsVerified']=bool(report['externalFilesVerified'] and all(data.get(k) for k in ['ordinaryPathOpen','openFlagsPreserved','aliasRead','crossPackageDenied','backingAliasDenied','attachPeerImpersonationDenied','statfs']) and proxy.get('openRequests')==3 and proxy.get('openPeerDenials',0)>=1 and proxy.get('acceptThreadStopped') and proxy.get('attachWorkersStopped') and 'MEDIA_PATH32_PASS opens=3' in nativeData)
                report['functionalPassed']=report['functionalPassed'] and report['ordinaryPathsVerified']
        if build.get('lazyMediaProbeSha256'):
            activation=state.get('lazyMediaActivation',{});data=media.get('dataProof',{})
            report['lazyMediaVerified']=bool(activation.get('exit')==0 and 'API32_LAZY_MEDIA_PASS' in activation.get('log','') and data.get('lazyExtractorRead') and data.get('extractorSampleBytes')==320)
            report['functionalPassed']=report['functionalPassed'] and report['lazyMediaVerified']
        if build.get('metricsProbeSha256'):
            checks=state.get('coreMediaMetrics',[])
            core=re.findall(r'INIT_CHILD service=pocket-registry pid=(\d+)(?:\s|$)',console)
            metrics=re.findall(r'POCKET_CORE_METRICS_READY pid=(\d+) logical_uid=0 caller_checks=stock',console)
            report['coreMediaMetricsVerified']=bool(len(checks)==2 and [c.get('mode') for c in checks]==['submit','verify']
                and all(c.get('exit')==0 for c in checks)
                and 'API32_CORE_METRICS_SUBMITTED uid=10021 claimed_uid=1000 oneway=1' in checks[0].get('log','')
                and 'API32_CORE_METRICS_VERIFIED stock_record=1 caller_uid=10021 forged_identity_replaced=1 forbidden_key_absent=1' in checks[1].get('log','')
                and len(core)==1 and metrics==core and 'INIT_CHILD service=pocket-mediametrics ' not in console
                and console.find('POCKET_CORE_METRICS_READY ')<console.find('INIT_CHILD service=pocket-framework '))
            report['functionalPassed']=report['functionalPassed'] and report['coreMediaMetricsVerified']
        if build.get('extractorFirstDemand32') or build.get('packagedImageTrial'):
            markers=['API32_EXTRACTOR_PLUGINS_DEFERRED until=data_or_type_request','API32_LAZY_SERVICE_PUBLISHED name=media.extractor ',
                'API32_EXTRACTOR_PLUGINS_READY first_demand=1']
            positions=[console.find(marker) for marker in markers]
            report['extractorFirstDemandVerified']=bool(all(p>=0 for p in positions) and positions[0]<positions[1]<positions[2])
            report['functionalPassed']=report['functionalPassed'] and report['extractorFirstDemandVerified']
        if build.get('extractorBootStart32') or build.get('packagedImageTrial'):
            published=console.find('API32_LAZY_SERVICE_PUBLISHED name=media.extractor ')
            framework=console.find('INIT_CHILD service=pocket-framework ')
            report['extractorBootStartVerified']=bool(published>=0 and published<framework
                and 'API32_LAZY_BINDER_READY name=media.extractor cold=0 ' in state.get('lazyMediaActivation',{}).get('log',''))
            report['functionalPassed']=report['functionalPassed'] and report['extractorBootStartVerified']
        if build.get('packagedImageTrial') or 'system/bin/pocket-init-api32' in build.get('guestCodePatchSha256',{}):
            checks=state.get('bootstrapFailureChecks',[])
            report['supervisedBootstrapVerified']=bool(len(checks)==2 and {c.get('childExit') for c in checks}=={0,7} and all(c.get('ownerExit')==67 for c in checks) and 'API32_RUNTIME_BOOTSTRAP_REAP' in console and 'POCKET_RUNTIME_BOOT_READY sdk=32 component_checks=0 owner=init' in console)
            report['functionalPassed']=report['functionalPassed'] and report['supervisedBootstrapVerified']
        if build.get('packagedImageTrial') or 'product/overlay/PocketMemory/PocketMemory.apk' in build.get('guestCodePatchSha256',{}):
            report['memoryProfileVerified']=bool('API32_GUEST_MEMORY_EARLY_DEFAULT_PASS constructor_limit=2 current_limit=2 override=-1 source=stock_ActivityManager_settings'
                in state.get('lazyMediaActivation',{}).get('log',''))
            report['functionalPassed']=report['functionalPassed'] and report['memoryProfileVerified']
        if build.get('packagedImageTrial') or 'system/framework/pocket-zygote.jar' in build.get('guestCodePatchSha256',{}):
            bridge=state.get('systemServerBridge',{});network=media.get('bridgeNetwork',{})
            report['systemServerBridgeVerified']=bool(bridge.get('keyDelivered') and bridge.get('touchDelivered')
                and network.get('validated') and network.get('networkBound') and network.get('httpStatus')==200 and network.get('inputPeerDenied')
                and re.search(r'API32_INPUT_CONTROL_READY[^\n]*owner=system_server pid=\d+',console)
                and 'API32_SYSTEM_SERVER_BRIDGE_FAILED' not in native)
            report['functionalPassed']=report['functionalPassed'] and report['systemServerBridgeVerified']
        report['strictPassed']=bool(report['functionalPassed'] and state.get('validation')=='PASS' and len(report['systemUiStarts'])==1 and not report['anrs'] and not report['javaFatals'] and not report['nativeFatals'] and report['reapAccounting'] and report['initStop'] and all(r[1:]==('0','0') for r in report['initStop']))
        if image.get('child_supervision')=='kernel-subreaper':report['strictPassed']=report['strictPassed'] and report['kernelChildOwnershipVerified']
        assert not any('pocket-init-api32' in row or 'pocket-core-api32' in row for row in adb('shell','ps -A -o PID,NAME,ARGS').splitlines())
        report['validation']='PASS' if report['strictPassed'] else 'FAIL'
    except Exception as error:report['validation']='FAIL';report['error']=str(error);raise
    finally:
        (work/'media-session-report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:report.get(k) for k in ['runId','validation','functionalPassed','strictPassed','error','anrs','javaFatals','nativeFatals','systemUiStarts']}),flush=True)
    if not report['strictPassed']:raise SystemExit(1)
if __name__=='__main__':main()
