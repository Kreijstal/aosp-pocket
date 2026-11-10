#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Resume the exact stopped, locked CE3 profile; verify host-input unlock and an offline outer restart."""
import argparse
import base64
import functools
import hashlib
import http.server
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import stat
import threading
import time
import urllib.parse
import uuid
import zipfile
from test_api32_persistence_on_pi import inspect_cycle

PACKAGE='dev.kreijstal.aosppocket.api32ceimage3'
ACTIVITY=PACKAGE+'/dev.kreijstal.aosppocket.runtime.ManagerCeProbe32'


def require_ui(value):
    assert value.get('physicalInput') is True and value.get('cliCorrectPasswordUsed') is False
    assert value.get('passwordEntryVerified') is True
    before=value['before'];after=value['after'];ready=value['uiBeforeEntry'];observer=ready['ui'];entry=observer['passwordEntry']
    assert all(before.get(k) is True for k in ['secure','deviceLocked','keyguardLocked'])
    assert after.get('secure') is True and after.get('deviceLocked') is False and after.get('keyguardLocked') is False
    assert ready.get('interactive') is True and observer.get('readOnly') is True and observer.get('passwordTextRead') is False
    assert observer.get('interactiveWindowsRequested') is True
    assert entry['resourceId']=='com.android.systemui:id/passwordEntry'
    assert all(entry.get(k) is True for k in ['visible','enabled','focused','password','editable'])
    assert value['input']['inputEventsRejected']==0
    assert value.get('wakeAttempts') and value['wakeAttempts'][-1]['state']['interactive'] is True


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vm-root',type=Path,default=Path('/home/pi/pocket-api32-vm'))
    parser.add_argument('--work-dir',type=Path,required=True)
    parser.add_argument('--prior-report',type=Path,required=True)
    parser.add_argument('--prior-attempt-report',type=Path,help='Exact terminal pre-input frame failure to inspect before replacing its CE3 APK')
    parser.add_argument('--credential-file',type=Path,required=True)
    parser.add_argument('--baseline-report',type=Path,required=True)
    args=parser.parse_args();base=args.vm_root.resolve();work=args.work_dir.resolve()
    assert (base/'.pocket-api32-test-vm').is_file() and work.is_relative_to(base)
    assert set(json.loads((base/'runtime/assembly/cuttlefish_config.json').read_text())['instances'])=={'73'}
    spec=importlib.util.spec_from_file_location('keyguard_vm',base/'tools/cuttlefish_arm64.py')
    vm=importlib.util.module_from_spec(spec);spec.loader.exec_module(vm)
    assert vm.BASE.resolve()==base and vm.lock()['instance']==73;vm.require_virtual()
    for path in [args.prior_report,args.credential_file,args.baseline_report]:assert path.resolve().is_relative_to(base)
    prior=json.loads(args.prior_report.read_text());baseline=json.loads(args.baseline_report.read_text())
    assert prior['runId']=='ed1b907536e84efdacfc5465eaadab78' and prior['validation']=='FAIL'
    assert prior['managerPackage']==PACKAGE and prior['app']['encryptedPhase']==3
    assert prior['app']['encryptedBoots'][2]['passwordRestored'] is True
    assert 'CE did not unlock after credential verification' in prior['app']['error']
    assert prior['app']['failedSession']['ownerAlive'] is False and prior['app']['failedSession']['ownerExitCode']==0
    assert [c['index'] for c in prior['cycles'] if c['check']['strictPassed']]==[0,1,2,3]
    assert baseline['validation']=='PASS' and baseline['strictPassed'] and baseline['externalVolumeVerified']
    assert baseline['sourceBuiltPrivateProviderVerified'] and baseline['kernelChildOwnershipVerified']
    assert baseline['app']['stoppedSession']['ownerAlive'] is False and baseline['app']['stoppedSession']['ownerExitCode']==0
    expected_installed=prior['managerSha256'];attempt=None
    if args.prior_attempt_report:
        path=args.prior_attempt_report.resolve();assert path.is_relative_to(base)
        attempt=json.loads(path.read_text())
        assert attempt['runId'] in ['22dcd4d87a3d4218b8e493c13b2c0f19','02b729cff9f04ff58380a75f29f9571e','3de54a64a1d64344a2c049bb86b73e8d','3b906fb0cb764ecf9d687388f34a8399','b27b6bbde8334be0976bdbb5d45fa0cb','968ed68158354944b3c77acdb609e735','b0bb2e37101e4fdaa58848c503190753','e3d9857762c84804b610d36e6b2cda2d','5af4c207e0804b59b2e24c23499af66a'] and attempt['validation']=='FAIL'
        assert attempt['priorRunId']==prior['runId'] and attempt['imageSha256']==baseline['imageSha256']
        attempted=attempt['outerRestart'] if attempt['runId']=='968ed68158354944b3c77acdb609e735' else attempt['app']
        failed=attempted['failedSession'];assert failed['ownerAlive'] is False and failed['ownerExitCode']==0
        if attempt['runId']=='5af4c207e0804b59b2e24c23499af66a':
            require_ui(attempted['keyguardUi'])
            assert 'Volume external_primary not found' in attempted['error']
            assert 'POCKET_CE32_USER_UNLOCKED wait_ms=3934' in attempted['error']
            assert attempted['hostPasswordEncodedUptimeMillis']-attempted['hostPasswordInputUptimeMillis']==33
            assert failed['input']['touchEventsAccepted']==16 and failed['input']['keyEventsAccepted']==88
            assert 'lastUserActivityTime=567735349' in attempted['keyguardAfterInput']['powerDump']
        elif attempt['runId']=='e3d9857762c84804b610d36e6b2cda2d':
            assert attempted['error'].startswith('java.io.IOException: Keyguard did not unlock after physical input:')
            ready=attempted['keyguardUiBeforeEntry'];entry=ready['ui']['passwordEntry']
            assert ready['interactive'] and all(entry[k] for k in ['visible','enabled','focused','password','editable'])
            assert failed['input']['touchEventsAccepted']==12 and failed['input']['keyEventsAccepted']==84
            after=attempted['keyguardAfterInput'];assert after['deviceLocked'] and after['keyguardLocked'] and not after['interactive']
            assert attempted['hostPasswordInputUptimeMillis']<after['uptimeMillis']
            assert 'isFileEncryptedNativeOrEmulated: true hasToken: false hasSecret: true' in failed['nativeLog']
        elif attempt['runId']=='968ed68158354944b3c77acdb609e735':
            assert attempt['app']['validation']=='PASS';require_ui(attempt['app']['keyguardUi'])
            assert attempt['app']['unlockedExternalCanaryVerified'] is True
            assert len(attempt['cycles'])==1 and attempt['cycles'][0]['index']==0 and attempt['cycles'][0]['check']['strictPassed']
            assert attempted['outerRestart'] is True and attempted['hostPid']!=attempt['app']['hostPid']
            assert attempted['error']=='java.io.IOException: CE did not unlock after credential verification'
            ready=attempted['keyguardUiBeforeEntry'];entry=ready['ui']['passwordEntry']
            assert ready['interactive'] and all(entry[k] for k in ['visible','enabled','focused','password','editable'])
            assert failed['input']['touchEventsAccepted']==13 and failed['input']['keyEventsAccepted']==46
        elif attempt['runId']=='b0bb2e37101e4fdaa58848c503190753':
            assert attempted['error']=='java.io.IOException: Manager did not present a nontrivial compositor frame'
            assert failed['input']['touchEventsAccepted']==39 and failed['input']['keyEventsAccepted']==18
            windows=attempted['keyguardUiAttempts'];assert len(windows)==3
            assert all(s['ui']['interactiveWindowsRequested'] and s['ui']['passwordEntry'] is None for s in windows)
            assert all(set(s['ui']['windowPackages'])<=set(['com.android.inputmethod.latin']) for s in windows)
            assert 'hostPasswordInputUptimeMillis' not in attempted
        elif attempt['runId']=='b27b6bbde8334be0976bdbb5d45fa0cb':
            assert attempt['app']['error']=='java.io.IOException: Manager did not present a nontrivial compositor frame'
            assert failed['input']['touchEventsAccepted']==39 and failed['input']['keyEventsAccepted']==18
            windows=attempt['app']['keyguardUiAttempts'];assert len(windows)==3
            assert all(s['ui']['windowPackages']==[] and s['ui']['matchingNodes']==0 and s['ui']['passwordEntry'] is None for s in windows)
            wakes=attempt['app']['keyguardWakeAttempts']
            assert any(s['state']['interactive'] for s in wakes) and any(not s['state']['interactive'] for s in wakes)
        elif attempt['runId']=='22dcd4d87a3d4218b8e493c13b2c0f19':
            assert attempt['app']['error']=='java.io.IOException: Manager did not present a nontrivial compositor frame'
            assert failed['input']['inputEventsAccepted']==0
        elif attempt['runId'] in ['02b729cff9f04ff58380a75f29f9571e','3b906fb0cb764ecf9d687388f34a8399']:
            assert attempt['app']['error']=='java.io.IOException: Guest display did not wake before the first keyguard frame'
            assert failed['input']['inputEventsAccepted']==failed['input']['keyEventsAccepted']==2
            assert attempt['app']['keyguardBeforeInput']['interactive'] is False
            if attempt['runId']=='3b906fb0cb764ecf9d687388f34a8399':
                diagnostic=attempt['app']['keyguardWakeDiagnostic'];assert diagnostic['readOnly'] and diagnostic['theaterMode']==0
                assert 'keyCode=224' in diagnostic['inputDump'] and 'policyFlags=0x2b000000' in diagnostic['inputDump']
        else:
            assert 'Unable to find app for caller' in attempt['app']['error'] and 'content provider settings' in attempt['app']['error']
            assert failed['input']['inputEventsAccepted']==failed['input']['keyEventsAccepted']==2
            assert attempt['app']['keyguardBeforeInput']['interactive'] is False
        assert failed['input']['inputEventsRejected']==0
        assert attempted['lockedCanariesDenied']==5 and attempted['lockedExternalCanaryDenied'] is True
        if attempt['runId'] not in ['968ed68158354944b3c77acdb609e735','e3d9857762c84804b610d36e6b2cda2d','5af4c207e0804b59b2e24c23499af66a']:
            assert 'keyguardUiBeforeEntry' not in attempted and 'keyguardUi' not in attempted
        expected_installed=attempt['managerSha256']
    assert not args.credential_file.is_symlink()
    with os.fdopen(os.open(args.credential_file,os.O_RDONLY|os.O_NOFOLLOW),'rb') as source:
        info=os.fstat(source.fileno());assert stat.S_ISREG(info.st_mode) and info.st_uid==os.getuid() and info.st_mode&0o777==0o600 and info.st_size<4096
        raw=source.read();secret_original_hash=hashlib.sha256(raw).digest();secrets=json.loads(raw)
    assert secrets['runId']==prior['runId'] and len(secrets['passwords'])==3
    passwords=secrets['passwords'];assert all(re.fullmatch('P32[a-f0-9]{14}',p) for p in passwords)
    def adb(*argv,timeout=40):
        result=vm.adb('-s',vm.serial(),*argv,check=False,timeout=timeout)
        if result.returncode:raise RuntimeError((result.stdout+result.stderr)[-2000:])
        return result.stdout.strip()
    def no_owners():
        ps=adb('shell','ps -A -o PID,NAME,ARGS')
        assert not any(any(n in line for n in ['pocket-init-api','pocket-core-api32','fuse-stock','pocket-media-client']) for line in ps.splitlines()),'An existing guest owns VM73'
    no_owners()
    installed=adb('shell','pm path '+PACKAGE).splitlines();assert len(installed)==1 and installed[0].startswith('package:/data/app/')
    assert adb('shell','sha256sum '+shlex.quote(installed[0][8:])).split()[0]==expected_installed,'Inspect a newer CE3 attempt before replacing its APK'
    meta=json.loads((work/'build.json').read_text());apk=work/'api32-ce-manager-probe.apk'
    assert meta['package']==PACKAGE and meta['probe'] and meta['guestImagesBundled']==0 and meta['guestSdks']==[28,29,32]
    assert hashlib.file_digest(apk.open('rb'),'sha256').hexdigest()==meta['sha256']
    with zipfile.ZipFile(apk) as archive:
        image=next(i for i in json.loads(archive.read('assets/pocket-images.json'))['images'] if i['sdk']==32)
        assert image['sha256']==baseline['imageSha256'] and image['external_volume']=='rootless-v1' and image['child_supervision']=='kernel-subreaper'
        assert b'ce_keyguard_resume' in archive.read('classes.dex') and b'passwordEntryVerified' in archive.read('classes.dex')
        for name,record in meta['hostNativeSourceBuild']['libraries'].items():assert hashlib.sha256(archive.read('lib/arm64-v8a/'+name)).hexdigest()==record['sha256']
        assert hashlib.sha256(archive.read('lib/arm64-v8a/libpocket_external32.so')).hexdigest()==meta['externalHostSourceBuild']['sha256']
    url=urllib.parse.urlsplit(image['url']);filename=url.path.removeprefix('/')
    assert url.scheme=='http' and url.hostname=='127.0.0.1' and url.port==8794 and '/' not in filename
    payload=work/filename;assert payload.stat().st_size==image['bytes'] and hashlib.file_digest(payload.open('rb'),'sha256').hexdigest()==image['sha256']
    report={'runId':uuid.uuid4().hex,'vmInstance':73,'managerPackage':PACKAGE,'managerSha256':meta['sha256'],'imageSha256':image['sha256'],
        'priorRunId':prior['runId'],'preservedCanaryRunId':prior['preservedRunId'],'freshProfile':False,'guestDataReset':False,
        'ordinaryGuestAppReinstalled':False,'lockCredentialChangesRequested':False,'fullCredentialLifecycleVerified':False,'cycles':[]}
    if attempt:report['priorAttemptRunId']=attempt['runId']
    settings=[('selinux','getenforce'),('phantomLimit','device_config get activity_manager max_phantom_processes'),('phantomMonitoring','settings get global settings_enable_monitor_phantom_procs')]
    for key,command in settings:report[key+'Before']=adb('shell',command)
    assert report['selinuxBefore']=='Enforcing'
    adb('shell','am start -W -a android.intent.action.MAIN -c android.intent.category.HOME')
    print(adb('install','--no-incremental','-r',str(apk),timeout=180),flush=True)
    assert 'DEBUGGABLE' not in adb('shell','dumpsys package '+PACKAGE)
    installed=adb('shell','pm path '+PACKAGE).splitlines();assert len(installed)==1
    assert adb('shell','sha256sum '+shlex.quote(installed[0][8:])).split()[0]==meta['sha256']
    requests=[];secret_requests=[];secret_payload=json.dumps({'runId':report['runId'],'passwords':passwords}).encode();secret_path='/'+report['runId']
    class ImageHandler(http.server.SimpleHTTPRequestHandler):
        def log_message(self,*args):pass
        def do_GET(self):
            if self.path!=url.path:self.send_error(404);return
            requests.append(self.path);super().do_GET()
    class SecretHandler(http.server.BaseHTTPRequestHandler):
        def log_message(self,*args):pass
        def do_GET(self):
            if self.path!=secret_path:self.send_error(404);return
            secret_requests.append(1);self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(secret_payload)))
            self.end_headers();self.wfile.write(secret_payload)
    server=http.server.ThreadingHTTPServer(('127.0.0.1',8794),functools.partial(ImageHandler,directory=str(work)))
    secret_server=http.server.ThreadingHTTPServer(('127.0.0.1',8795),SecretHandler)
    thread=threading.Thread(target=server.serve_forever,daemon=True);secret_thread=threading.Thread(target=secret_server.serve_forever,daemon=True)
    thread.start();secret_thread.start();image_active=True;reverse=False;secret_reverse=False;seen=set();poll_errors=0
    def observe(outer,timeout):
        nonlocal poll_errors
        deadline=time.monotonic()+timeout;last=None
        while time.monotonic()<deadline:
            try:
                text=adb('shell','dumpsys activity '+ACTIVITY+' pocket-ce-progress')
                rows=[l.strip().split('=',1)[1] for l in text.splitlines() if l.strip().startswith('POCKET_SESSION32=')]
                state=json.loads(base64.b64decode(rows[-1],validate=True)) if rows else {}
            except Exception:poll_errors+=1;time.sleep(2);continue
            if state.get('runId')!=report['runId'] or bool(state.get('outerRestart'))!=outer:time.sleep(1);continue
            stage=state.get('stage'),state.get('managerStatus')
            if stage!=last:print('CE_KEYGUARD_STAGE',outer,*stage,flush=True);last=stage
            if state.get('stage')=='cycle-stopped':
                index=state['cycleIndex'];assert index==(1 if outer else 0)
                if index not in seen:
                    text=adb('shell','dumpsys activity '+ACTIVITY+' pocket-ce-cycle')
                    rows=[l.strip().split('=',1)[1] for l in text.splitlines() if l.strip().startswith('POCKET_CE_CYCLE=')]
                    cycle=json.loads(base64.b64decode(rows[-1],validate=True));assert cycle['runId']==report['runId'] and cycle['index']==index and cycle['migration'] is False
                    checks=inspect_cycle(cycle,require_kernel_ownership=True,require_external_volume=True)
                    started=re.findall(r'(?m)^INIT_CHILD service=(\S+) pid=(\d+)$',cycle['console']);reaped=re.findall(r'(?m)^INIT_REAP service=(\S+) pid=(\d+) code=\d+ status=\d+$',cycle['console'])
                    checks['reapAccounting']=bool(started and sorted(started)==sorted(reaped) and len(checks['initStop'])==1 and int(checks['initStop'][0][0])==len(started))
                    path=work/f'ce-keyguard-cycle-{index}.json';path.write_text(json.dumps(cycle,indent=2)+'\n')
                    report['cycles'].append({'index':index,'path':str(path),'sha256':hashlib.file_digest(path.open('rb'),'sha256').hexdigest(),'check':checks})
                    assert checks['strictPassed'] and checks['reapAccounting'],checks
                    seen.add(index);adb('shell','am start -n '+ACTIVITY+' --es probe_run_id '+report['runId']+' --ei ce_cycle_ack '+str(index))
            if state.get('validation'):return state
            time.sleep(1)
        raise TimeoutError('Observation expired; inspect this exact keyguard guest before any restart')
    def verify(state):
        assert state.get('validation')=='PASS',state.get('error','No passing keyguard result')
        assert state['preservedRunId']==prior['preservedRunId'] and state['lockedCanariesDenied']==5
        assert state['lockedExternalCanaryDenied'] is True and state['unlockedExternalCanaryVerified'] is True
        assert state['lockCredentialChangesRequested'] is False and state['fullCredentialLifecycleVerified'] is False
        require_ui(state['keyguardUi']);current=state['app'];original=prior['app']['originalApp']
        assert state['keyguardUi']['after']['unlockObserved'] is True and state['keyguardUi']['after']['interactive'] is True
        assert state['keyguardUi']['after']['keyguardObservationPolls']>=1
        assert all(current[k]==original[k] for k in ['token','cipherDigest','uid','canonicalDataPath'])
        assert current['sdk']==32 and current['nonExportable'] and current['cryptoValid'] and current['androidUserUnlocked']
        assert current['externalStorage']['primaryEmulatedMounted'] and current['externalStorage']['externalPrimaryQuery']
        assert current['externalStorage']['externalPrimaryQueryRows']==0 and current['externalStorage']['queryOwner']=='system-external-token'
        assert current['externalStorage']['externalPrimaryQueryWaitMillis']>=0 and current['externalStorage']['volumeNotReadyRetries']>=0
        installed=state['imageInstallStats']
        assert installed['sdk']==32 and installed['imageSha256']==image['sha256']
        assert installed['reusedFiles']>0 and installed['replacedFiles']>0
        assert installed['reusedFiles']+installed['replacedFiles']==len(image['manifest']['entries'])
        assert installed['reusedBytes']+installed['writtenBytes']==image['unpacked_bytes']
    try:
        adb('reverse','tcp:8794','tcp:8794');reverse=True
        adb('reverse','tcp:8795','tcp:8795');secret_reverse=True
        no_owners();adb('shell','am force-stop '+PACKAGE)
        start='am start -n '+ACTIVITY+' --es probe_run_id '+report['runId']+' --es ce_secret_url http://127.0.0.1:8795'+secret_path+' --ez ce_keyguard_resume true --es ce_preserved_nonce '+prior['preservedRunId']
        print(adb('shell',start),flush=True);report['app']=observe(False,1800);verify(report['app']);assert seen=={0}
        no_owners();assert adb('shell','pidof '+PACKAGE)==str(report['app']['hostPid'])
        server.shutdown();server.server_close();thread.join(5);image_active=False
        adb('reverse','--remove','tcp:8794');reverse=False;report['imageSourceOffline']=True
        adb('shell','am force-stop '+PACKAGE)
        print(adb('shell',start+' --ez ce_keyguard_outer_resume true'),flush=True)
        report['outerRestart']=observe(True,1200);verify(report['outerRestart']);assert seen=={0,1}
        assert report['outerRestart']['ordinaryAppPersisted'] and report['outerRestart']['hostPid']!=report['app']['hostPid']
        assert report['outerRestart']['previousHostPid']==report['app']['hostPid'] and len(secret_requests)==2
        for key,command in settings:report[key+'After']=adb('shell',command);assert report[key+'After']==report[key+'Before']
        assert hashlib.sha256(args.credential_file.read_bytes()).digest()==secret_original_hash
        report.update({'credentialFileUnchanged':True,'keyguardUiVerified':True,'managerProcessRestartVerified':True,'ordinaryAppPreserved':True,'strictPassed':True,'validation':'PASS'})
    except Exception as error:report['validation']='FAIL';report['error']=str(error);raise
    finally:
        if image_active:server.shutdown();server.server_close();thread.join(5)
        secret_server.shutdown();secret_server.server_close();secret_thread.join(5)
        if reverse:adb('reverse','--remove','tcp:8794')
        if secret_reverse:adb('reverse','--remove','tcp:8795')
        report['requests']=requests;report['pollErrors']=poll_errors;report['credentialFileRetainedForRecovery']=args.credential_file.is_file()
        encoded=json.dumps(report,indent=2)+'\n'
        for password in passwords:encoded=encoded.replace(password,'<redacted>')
        (work/'ce-keyguard-report.json').write_text(encoded)
        print(json.dumps({k:report.get(k) for k in ['runId','validation','strictPassed','error']}),flush=True)


if __name__=='__main__':main()
