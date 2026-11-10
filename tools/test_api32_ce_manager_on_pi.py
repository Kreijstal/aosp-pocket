#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Test real12L credential encryption in a fresh isolated Manager profile on VM73."""
import argparse
import base64
import functools
import hashlib
import http.server
import importlib.util
import json
import re
import secrets
import os
import stat
from pathlib import Path
import shlex
import threading
import time
import urllib.parse
import uuid
import zipfile
from test_api32_persistence_on_pi import inspect_cycle

PACKAGE='dev.kreijstal.aosppocket.api32ceimage'
ACTIVITY=PACKAGE+'/dev.kreijstal.aosppocket.runtime.ManagerCeProbe32'


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--vm-root',type=Path,default=Path('/home/pi/pocket-api32-vm'))
    p.add_argument('--work-dir',type=Path)
    p.add_argument('--package',choices=(PACKAGE,PACKAGE+'2',PACKAGE+'3'),default=PACKAGE)
    p.add_argument('--baseline-report',type=Path,help='Completed strict app report for this exact image')
    p.add_argument('--preserved-profile-report',type=Path,help='Continue the credential lifecycle in the verified stopped authless CE3 profile without resetting its data')
    p.add_argument('--prior-checkpoint-rejection-report',type=Path,help='Exact terminal pre-credential checkpoint rejection when continuing a completed preserved-profile test')
    p.add_argument('--resume-outer-report',type=Path,help='Resume only outer restart after five verified guest cycles and a host-verifier NameError; keep the installed APK and original failed report')
    args=p.parse_args();base=args.vm_root.resolve();work=(args.work_dir or base/'ce-manager-validation').resolve()
    if args.prior_checkpoint_rejection_report and not args.preserved_profile_report:p.error('--prior-checkpoint-rejection-report requires --preserved-profile-report')
    if args.resume_outer_report and (not args.preserved_profile_report or args.prior_checkpoint_rejection_report):p.error('--resume-outer-report requires --preserved-profile-report and a separate work directory')
    package=args.package;activity=package+'/dev.kreijstal.aosppocket.runtime.ManagerCeProbe32'
    assert work.is_relative_to(base) and (base/'.pocket-api32-test-vm').is_file()
    assert set(json.loads((base/'runtime/assembly/cuttlefish_config.json').read_text())['instances'])=={'73'}
    s=importlib.util.spec_from_file_location('vm',base/'tools/cuttlefish_arm64.py');vm=importlib.util.module_from_spec(s);s.loader.exec_module(vm)
    assert vm.BASE.resolve()==base and vm.lock()['instance']==73;vm.require_virtual()
    baseline=(args.baseline_report or base/'manager-app-validation/manager-app-report.json').resolve();assert baseline.is_relative_to(base)
    previous=json.loads(baseline.read_text())
    assert previous['validation']=='PASS' and previous['strictPassed']
    assert previous['app']['stoppedSession']['ownerAlive'] is False and previous['app']['stoppedSession']['ownerExitCode']==0
    def adb(*argv,timeout=40):
        r=vm.adb('-s',vm.serial(),*argv,check=False,timeout=timeout)
        if r.returncode:raise RuntimeError((r.stdout+r.stderr)[-2000:])
        return r.stdout.strip()
    # This first full-framework encryption trial gets a new package sandbox.
    # Never clear an existing profile merely because an observation timed out.
    owners=adb('shell','ps -A -o PID,NAME,ARGS')
    assert not any(any(n in row for n in ['pocket-init-api','pocket-core-api32','fuse-stock','pocket-media-client']) for row in owners.splitlines()),'An existing guest owns the VM'
    installed_packages=adb('shell','pm list packages -u '+package).splitlines()
    preserved=None;preserved_state=None;preserved_nonce=None;checkpoint_rejection=None;resume=None;resume_path=None
    if args.resume_outer_report:
        resume_path=args.resume_outer_report.resolve()
        assert resume_path.is_relative_to(base) and resume_path.parent!=work and package==PACKAGE+'3'
        resume=json.loads(resume_path.read_text())
        assert resume['validation']=='FAIL' and resume['error']=="name 'storage' is not defined"
        assert resume['app']['validation']=='PASS' and 'outerRestart' not in resume
        assert resume['app']['cycleIndex']==4 and resume['testCredentialsRetainedForRecovery']
        assert [c['index'] for c in resume['cycles']]==list(range(5))
        for c in resume['cycles']:
            path=Path(c['path']).resolve();assert path.is_relative_to(resume_path.parent)
            assert hashlib.file_digest(path.open('rb'),'sha256').hexdigest()==c['sha256']
            raw=json.loads(path.read_text())
            assert raw['runId']==resume['runId'] and raw['index']==c['index'] and raw['migration'] is False
            assert inspect_cycle(raw,require_kernel_ownership=True,require_external_volume=True)['strictPassed']
    if args.preserved_profile_report:
        path=args.preserved_profile_report.resolve();assert path.is_relative_to(base) and package==PACKAGE+'3'
        preserved=json.loads(path.read_text())
        assert preserved['validation']=='PASS' and preserved['strictPassed']
        if preserved['runId']=='fde5db0b580b49fc9719bdecdea0710f':
            assert preserved['ordinaryAppPersisted'] and preserved['kernelChildOwnershipVerified'] and preserved['initTrial']['originalCodeRestored']
            preserved_state=preserved['app'];preserved_nonce=preserved['preservedRunId']
        elif preserved.get('fullPreservedCredentialLifecycleVerified') is True:
            assert preserved['managerProcessRestartVerified'] and preserved['testCredentialsRemovedAfterAuthlessRestoration']
            assert preserved['plaintextMigrationRepeated'] is False and preserved['guestDataReset'] is False
            assert preserved['ordinaryGuestAppReinstalled'] is False and preserved['runtimeCodePatchOutsideImage'] is False
            assert [cycle['index'] for cycle in preserved['cycles']]==list(range(6))
            for cycle in preserved['cycles']:
                cycle_path=Path(cycle['path']).resolve();assert cycle_path.is_relative_to(base)
                assert hashlib.file_digest(cycle_path.open('rb'),'sha256').hexdigest()==cycle['sha256']
                raw=json.loads(cycle_path.read_text())
                assert raw['runId']==preserved['runId'] and raw['index']==cycle['index']
                assert cycle['check']['strictPassed'] and inspect_cycle(raw,require_kernel_ownership=True,require_external_volume=True)['strictPassed']
            preserved_state=preserved['outerRestart'];preserved_nonce=preserved['preservedRunId']
            assert preserved_state['validation']=='PASS' and preserved_state['originalPasswordFreeStateRestored']
            assert preserved_state['ordinaryAppPersisted'] and preserved_state['readableCanariesVerified']==5 and preserved_state['unlockedExternalCanaryVerified']
            assert all(preserved_state['app'][key]==preserved['app']['originalApp'][key] for key in ['token','cipherDigest','uid','canonicalDataPath'])
            # Restoration authorizes a deliberate continuation. The new probe
            # must observe actual authless user unlock before changing a password.
        else:
            assert preserved['runId']=='04fe2097ab9747deb4a61426b34a7446'
            assert preserved['originalPasswordFreeStateRestored'] and preserved['authlessOuterRestartVerified'] and preserved['ordinaryAppPreserved']
            assert preserved['imageSha256']==previous['imageSha256'] and preserved['imageSourceOfflineForBothBoots']
            assert [c['phase'] for c in preserved['cycles'] if c['check']['strictPassed']]==['restoration','authless']
            preserved_state=preserved['authlessRestart'];preserved_nonce=preserved['preservedCanaryRunId']
            assert preserved_state['readableCanariesVerified']==5 and preserved_state['unlockedExternalCanaryVerified']
        if preserved.get('fullPreservedCredentialLifecycleVerified') is not True:
            assert preserved_state['keyguard']['secure'] is False and preserved_state['authlessBootUnlocked']
        assert 'package:'+package in installed_packages
        expected_installed=preserved['managerSha256']
        if resume:
            assert resume['preservedReportRunId']==preserved['runId'] and resume['preservedRunId']==preserved_nonce
            assert resume['managerPackage']==package
            expected_installed=resume['managerSha256']
        if args.prior_checkpoint_rejection_report:
            path=args.prior_checkpoint_rejection_report.resolve();assert path.is_relative_to(base)
            checkpoint_rejection=json.loads(path.read_text());failed=checkpoint_rejection['app']
            assert checkpoint_rejection['validation']=='FAIL' and checkpoint_rejection['cycles']==[]
            assert checkpoint_rejection['preservedReportRunId']==preserved['runId'] and checkpoint_rejection['preservedRunId']==preserved_nonce
            assert failed['error']=='java.io.IOException: Existing encryption trial requires deliberate resume'
            assert all(key not in failed for key in ['initialCredential','originalApp','encryptedPhase'])
            assert not failed.get('cleanupError')
            expected_installed=checkpoint_rejection['managerSha256']
        old=adb('shell','pm path '+package).splitlines();assert len(old)==1 and old[0].startswith('package:/data/app/')
        assert adb('shell','sha256sum '+shlex.quote(old[0][8:])).split()[0]==expected_installed,'Inspect a previous preserved-lifecycle attempt before replacing its APK'
    else:assert 'package:'+package not in installed_packages,'CE test package already exists; inspect its exact prior state before reuse'
    meta=json.loads((work/'build.json').read_text());apk=work/'api32-ce-manager-probe.apk'
    assert meta['package']==package and meta['probe'] and meta['guestImagesBundled']==0 and meta['guestSdks']==[28,29,32]
    assert hashlib.file_digest(apk.open('rb'),'sha256').hexdigest()==meta['sha256']
    with zipfile.ZipFile(apk) as z:
        if meta.get('hostNativeSourceBuild'):
            libraries=meta['hostNativeSourceBuild']['libraries']
            assert set(libraries)=={'libpocket_runtime.so','libproot.so','libpocket_frames.so','libpocket_ce.so'}
            for name,record in libraries.items():assert hashlib.sha256(z.read('lib/arm64-v8a/'+name)).hexdigest()==record['sha256']
        if meta.get('externalHostSourceBuild'):
            assert hashlib.sha256(z.read('lib/arm64-v8a/libpocket_external32.so')).hexdigest()==meta['externalHostSourceBuild']['sha256']
        image=next(i for i in json.loads(z.read('assets/pocket-images.json'))['images'] if i['sdk']==32)
        assert image['sha256']==previous['imageSha256'],'Baseline and CE image differ'
        if checkpoint_rejection:assert image['sha256']==checkpoint_rejection['imageSha256']
        if resume:assert image['sha256']==resume['imageSha256'] and meta['sha256']==resume['managerSha256']
        assert b'Ldev/kreijstal/aosppocket/runtime/ManagerCeProbe32;' in z.read('classes.dex')
    url=urllib.parse.urlsplit(image['url']);name=url.path.removeprefix('/')
    assert url.scheme=='http' and url.hostname=='127.0.0.1' and url.port==8794 and '/' not in name
    payload=work/name;assert payload.stat().st_size==image['bytes'] and hashlib.file_digest(payload.open('rb'),'sha256').hexdigest()==image['sha256']
    report={'runId':resume['runId'] if resume else uuid.uuid4().hex,'vmInstance':73,'managerPackage':package,'managerSha256':meta['sha256'],'imageSha256':image['sha256'],'baselineRunId':previous['runId'],'cycles':[],'freshProfile':preserved is None}
    if resume:
        report['resumedFiveBootReportSha256']=hashlib.file_digest(resume_path.open('rb'),'sha256').hexdigest()
        report['resumedFiveBootReport']=str(resume_path)
        report['previousHostDriverError']=resume['error']
        report['previousHostDriverFailurePreserved']=True
        report['apkReinstalledForResume']=False
        report['priorImageRequests']=resume['requests']
        for c in resume['cycles']:
            target=work/Path(c['path']).name
            with target.open('xb') as dest:dest.write(Path(c['path']).read_bytes())
            assert hashlib.file_digest(target.open('rb'),'sha256').hexdigest()==c['sha256']
            report['cycles'].append(dict(c,path=str(target)))
    if preserved:
        report['preservedRunId']=preserved_nonce;report['preservedReportRunId']=preserved['runId']
        report['preservedImageSha256']=preserved['imageSha256']
        report['guestDataReset']=False;report['ordinaryGuestAppReinstalled']=False;report['runtimeCodePatchOutsideImage']=False
    if checkpoint_rejection:report['priorCheckpointRejectionRunId']=checkpoint_rejection['runId']
    settings=[('selinux','getenforce'),('phantomLimit','device_config get activity_manager max_phantom_processes'),('phantomMonitoring','settings get global settings_enable_monitor_phantom_procs')]
    for key,command in settings:report[key+'Before']=adb('shell',command)
    assert report['selinuxBefore']=='Enforcing'
    # Check kept-data/uninstalled packages too, and never replace an earlier
    # encryption trial. Put Home in front so another old probe cannot resume.
    adb('shell','am start -W -a android.intent.action.MAIN -c android.intent.category.HOME')
    if not resume:
        install=['install','--no-incremental'];
        if preserved:install.append('-r')
        print(adb(*install,str(apk),timeout=180),flush=True)
    assert 'DEBUGGABLE' not in adb('shell','dumpsys package '+package)
    installed=adb('shell','pm path '+package).splitlines();assert len(installed)==1 and installed[0].startswith('package:/data/app/')
    assert adb('shell','sha256sum '+shlex.quote(installed[0][8:])).split()[0]==meta['sha256']
    requests=[]
    class Handler(http.server.SimpleHTTPRequestHandler):
        def log_message(self,*args):pass
        def do_GET(self):
            if self.path!=url.path:self.send_error(404);return
            requests.append(self.path);super().do_GET()
    server=http.server.ThreadingHTTPServer(('127.0.0.1',8794),functools.partial(Handler,directory=str(work)))
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start();reverse=False;image_server_active=True
    # Test credentials live in this driver, outside the Android app being
    # restarted. They are never persisted in the guest or command-line extras.
    passwords=['P32'+secrets.token_hex(7) for _ in range(3)];secret_requests=[]
    credential_file=None;credential_hash=None
    if resume:
        credential_file=resume_path.parent/'test-credentials.json'
        with os.fdopen(os.open(credential_file,os.O_RDONLY|os.O_NOFOLLOW),'rb') as src:
            info=os.fstat(src.fileno())
            assert stat.S_ISREG(info.st_mode) and info.st_uid==os.getuid() and info.st_mode&0o777==0o600 and info.st_size<4096
            raw=src.read();credential_hash=hashlib.sha256(raw).digest();value=json.loads(raw)
        assert value['runId']==report['runId'] and len(value['passwords'])==3
        passwords=value['passwords'];assert all(re.fullmatch('P32[a-f0-9]{14}',p) for p in passwords)
    secret_path='/'+report['runId'];secret_url='http://127.0.0.1:8795'+secret_path
    secret_payload=json.dumps({'runId':report['runId'],'passwords':passwords}).encode()
    if preserved and not resume:
        # Keep this long, failure-sensitive trial recoverable after a driver
        # crash. Test-only credentials stay outside Android in an owner-only
        # Pi file; they are removed after the original authless state returns.
        credential_file=work/'test-credentials.json'
        descriptor=os.open(credential_file,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
        with os.fdopen(descriptor,'wb') as output:
            output.write(secret_payload);output.flush();os.fsync(output.fileno())
        assert credential_file.stat().st_uid==os.getuid() and credential_file.stat().st_mode&0o777==0o600
        report['testCredentialStorage']='Owner-only0600 Pi host file, outside Android; retained on failure for deliberate recovery'
    if resume:report['testCredentialStorage']='Exact prior run owner0600 Pi recovery file; not copied; remove only after verified authless restoration'
    class SecretHandler(http.server.BaseHTTPRequestHandler):
        def log_message(self,*args):pass
        def do_GET(self):
            if self.path!=secret_path:self.send_error(404);return
            secret_requests.append(1);self.send_response(200);self.send_header('Content-Type','application/json')
            self.send_header('Content-Length',str(len(secret_payload)));self.end_headers();self.wfile.write(secret_payload)
    secret_server=http.server.ThreadingHTTPServer(('127.0.0.1',8795),SecretHandler)
    secret_thread=threading.Thread(target=secret_server.serve_forever,daemon=True);secret_thread.start();secret_reverse=False
    def state(argument,prefix):
        text=adb('shell','dumpsys activity '+activity+' '+argument)
        rows=[line.strip().split('=',1)[1] for line in text.splitlines() if line.strip().startswith(prefix+'=')]
        return json.loads(base64.b64decode(rows[-1],validate=True)) if rows else {}
    seen={c['index'] for c in report['cycles']};errors=0
    def observe(deadline,outer=False):
        nonlocal errors
        last=None
        while time.monotonic()<deadline:
            try:a=state('pocket-ce-progress','POCKET_SESSION32')
            except Exception:errors+=1;time.sleep(2);continue
            if a.get('runId')!=report['runId'] or outer and a.get('outerRestart') is not True:time.sleep(1);continue
            stage=(a.get('stage'),a.get('encryptedPhase'),a.get('managerStatus'))
            if stage!=last:print('CE_RESTART_STAGE' if outer else 'CE_MANAGER_STAGE',*stage,flush=True);last=stage
            if a.get('stage')=='cycle-stopped' and a['cycleIndex'] not in seen:
                index=a['cycleIndex'];assert index==5 if outer else index in range(5)
                cycle=state('pocket-ce-cycle','POCKET_CE_CYCLE');assert cycle['runId']==report['runId'] and cycle['index']==index
                checks=inspect_cycle(cycle,require_kernel_ownership=image.get('child_supervision')=='kernel-subreaper',require_external_volume=image.get('external_volume')=='rootless-v1')
                children=re.findall(r'^INIT_CHILD service=(\S+) pid=(\d+)$',cycle['console'],re.M)
                reaped=re.findall(r'^INIT_REAP service=(\S+) pid=(\d+) code=\d+ status=\d+$',cycle['console'],re.M)
                checks['supervisedLifetimeCount']=len(children)
                checks['reapAccounting']=bool(children and len(checks['initStop'])==1
                    and int(checks['initStop'][0][0])==len(children)==len(reaped) and sorted(children)==sorted(reaped))
                assert checks['strictPassed'] and checks['reapAccounting'],checks
                path=work/('ce-manager-cycle-'+report['runId']+'-'+str(index)+'.json');path.write_text(json.dumps(cycle,indent=2)+'\n')
                report['cycles'].append({'index':index,'path':str(path),'sha256':hashlib.file_digest(path.open('rb'),'sha256').hexdigest(),'check':checks,'migration':cycle['migration']})
                seen.add(index);print('CE_MANAGER_CYCLE_PASS',index,flush=True)
                adb('shell','am start -n '+activity+' --es probe_run_id '+report['runId']+' --ei ce_cycle_ack '+str(index))
            if a.get('validation'):return a
            time.sleep(1)
        raise TimeoutError('CE observation timed out; inspect the exact live guest before any restart')
    def require_ui(keyguard):
        assert keyguard.get('physicalInput') is True and keyguard.get('cliCorrectPasswordUsed') is False
        assert all(keyguard['before'].get(k) is True for k in ['secure','deviceLocked','keyguardLocked'])
        assert keyguard['after'].get('secure') is True and keyguard['after'].get('deviceLocked') is False and keyguard['after'].get('keyguardLocked') is False
        assert keyguard.get('passwordEntryVerified') and keyguard['after'].get('unlockObserved') and keyguard['after'].get('interactive')
        entry=keyguard['uiBeforeEntry']['ui']['passwordEntry']
        assert entry['resourceId']=='com.android.systemui:id/passwordEntry'
        assert all(entry[k] for k in ['visible','enabled','focused','password','editable'])
        assert keyguard['input']['inputEventsRejected']==0
    def require_storage(state):
        if image.get('external_volume')!='rootless-v1':return
        assert state['readableCanariesVerified']==5 and state['unlockedExternalCanaryVerified']
        app=state['app'];assert app['androidUserUnlocked'] and app['cryptoValid'] and app['nonExportable']
        storage=app['externalStorage'];assert storage['primaryEmulatedMounted'] and storage['externalPrimaryQuery']
        assert storage['externalPrimaryQueryRows']==0 and storage['queryOwner']=='system-external-token'
    def require_readiness(state,locked=False):
        assert state['sdk']==32 and state['uid']==1000 and state['pid']>0
        assert state['source']=='LocalServices.UserController.mBootCompleted'
        assert state['classLoaderSource']=='ZygoteInit.sCachedSystemServerClassLoader'
        if locked:
            assert not state['userUnlocked'] and not state['readyForApps'] and state['credentialRequired']
            assert state['lockedAppOperationRejected'] is True
        else:
            assert state['userUnlocked'] and state['readyForApps'] and state['bootBroadcastComplete'] and state['primaryVolumeMounted']
            assert not state['credentialRequired']
    try:
        if not resume:adb('reverse','tcp:8794','tcp:8794');reverse=True
        adb('reverse','tcp:8795','tcp:8795');secret_reverse=True
        start='am start -n '+activity+' --es probe_run_id '+report['runId']+' --es ce_secret_url '+secret_url
        if preserved:start+=' --ez ce_preserved_lifecycle true --es ce_preserved_nonce '+preserved_nonce
        if preserved and preserved.get('fullPreservedCredentialLifecycleVerified'):
            start+=' --es ce_completed_predecessor '+preserved['runId']
        if resume:report['app']=resume['app']
        else:
            print(adb('shell',start),flush=True)
            report['app']=observe(time.monotonic()+3000)
        assert report['app'].get('validation')=='PASS',report['app'].get('error','CE Manager check failed')
        assert seen==set(range(5)) and len(report['app']['encryptedBoots'])==4
        for phase,boot in enumerate(report['app']['encryptedBoots']):
            assert boot['phase']==phase
            if phase!=2:assert boot['lockedCanariesDenied']==5 and boot['wrongPasswordRejected'] and boot['credentialUnlock']
            else:assert boot['authlessBootUnlocked'] and boot['passwordRestored']
            assert all(boot['app'][k]==report['app']['originalApp'][k] for k in ['token','cipherDigest','uid','canonicalDataPath'])
            if 'system/etc/pocket-app-readiness32' in image['manifest']['entries']:
                if phase!=2:require_readiness(boot['lockedAppReadiness'],locked=True)
                require_readiness(boot['unlockedAppReadiness'])
        assert report['app']['encryptedBoots'][0]['passwordChanged'] and report['app']['encryptedBoots'][1]['passwordRemoved']
        if preserved:
            observed=report['app']['originalApp'];expected=preserved_state['app']
            assert all(observed[k]==expected[k] for k in ['token','cipherDigest','uid','canonicalDataPath'])
            assert report['app'].get('preservedLifecycle') and report['app']['preservedRunId']==preserved_nonce
            stats=report['app']['imageInstallStats'];assert stats['imageSha256']==image['sha256']
            assert stats['reusedFiles']+stats['replacedFiles']==len(image['manifest']['entries'])
            assert stats['reusedBytes']+stats['writtenBytes']==image['unpacked_bytes']
            report['ordinaryAppPreservedAcrossImageUpgrade']=True
            if 'system/etc/pocket-app-readiness32' in image['manifest']['entries']:
                assert report['app']['initialAuthlessKeyguard']['secure'] is False
                require_readiness(report['app']['initialAuthlessReadiness'])
                assert observed['androidUserUnlocked']
                report['actualPreservedAuthlessBootVerified']=True
                if preserved.get('fullPreservedCredentialLifecycleVerified'):
                    assert report['app']['completedCheckpointPredecessor']==preserved['runId']
                    assert report['app']['completedCheckpointArchive']=='ce32-restart-state-'+preserved['runId']+'.json'
                    assert re.fullmatch('[a-f0-9]{64}',report['app']['completedCheckpointSha256'])
                    report['completedPredecessorCheckpointArchived']=True
        require_ui(report['app']['encryptedBoots'][3].get('keyguardUi',{}))
        if image.get('external_volume')=='rootless-v1':
            assert report['app']['originalReadableCanariesVerified']==5 and report['app']['originalPublicMediaCanaryVerified']
            for boot in report['app']['encryptedBoots']:require_storage(boot)
        report['keyguardUiVerified']=True
        # Restart only the exact completed fresh test app, with no live guest.
        owners=adb('shell','ps -A -o PID,NAME,ARGS')
        assert not any('pocket-init-api' in row or 'pocket-core-api32' in row for row in owners.splitlines())
        assert adb('shell','pidof '+package)==str(report['app']['hostPid'])
        installed=adb('shell','pm path '+package).removeprefix('package:')
        assert adb('shell','sha256sum '+shlex.quote(installed)).split()[0]==meta['sha256']
        server.shutdown();server.server_close();thread.join(5);image_server_active=False
        if reverse:adb('reverse','--remove','tcp:8794');reverse=False
        report['imageSourceOffline']=True
        adb('shell','am force-stop '+package)
        assert not any(package in row for row in adb('shell','ps -A -o PID,NAME').splitlines())
        print(adb('shell',start+' --ez ce_outer_resume true'),flush=True)
        report['outerRestart']=observe(time.monotonic()+1200,outer=True)
        resumed=report['outerRestart'];assert resumed.get('validation')=='PASS',resumed.get('error','Outer restart failed')
        assert seen==set(range(6)) and resumed.get('ordinaryAppPersisted') is True
        assert resumed['hostPid']!=report['app']['hostPid'] and resumed['previousHostPid']==report['app']['hostPid']
        require_ui(resumed.get('keyguardUi',{}));assert len(secret_requests)==(1 if resume else 2)
        require_storage(resumed)
        if 'system/etc/pocket-app-readiness32' in image['manifest']['entries']:
            require_readiness(resumed['lockedAppReadiness'],locked=True)
            require_readiness(resumed['unlockedAppReadiness'])
            report['secureLockedAppReadinessVerified']=True
        assert all(resumed['app'][k]==report['app']['originalApp'][k] for k in ['token','cipherDigest','uid','canonicalDataPath'])
        if preserved:assert resumed.get('originalPasswordFreeStateRestored') is True
        report['managerProcessRestartVerified']=True;report['pollErrors']=errors
        for key,command in settings:
            report[key+'After']=adb('shell',command);assert report[key+'After']==report[key+'Before']
        report['strictPassed']=True;report['validation']='PASS'
        report['fullPreservedCredentialLifecycleVerified']=preserved is not None
        report['plaintextMigrationRepeated']=preserved is None
        report['mountedStorageAcrossCredentialLifecycleVerified']=image.get('external_volume')=='rootless-v1'
        if credential_file is not None:
            if credential_hash is not None:
                assert not credential_file.is_symlink() and hashlib.file_digest(credential_file.open('rb'),'sha256').digest()==credential_hash
            credential_file.unlink();report['testCredentialsRemovedAfterAuthlessRestoration']=True
    except Exception as error:report['validation']='FAIL';report['error']=str(error);raise
    finally:
        if image_server_active:server.shutdown();server.server_close();thread.join(5)
        secret_server.shutdown();secret_server.server_close();secret_thread.join(5)
        if reverse:adb('reverse','--remove','tcp:8794')
        if secret_reverse:adb('reverse','--remove','tcp:8795')
        report['requests']=requests;encoded=json.dumps(report,indent=2)+'\n'
        if credential_file is not None:report['testCredentialsRetainedForRecovery']=credential_file.exists();encoded=json.dumps(report,indent=2)+'\n'
        for password in passwords:encoded=encoded.replace(password,'<redacted>')
        (work/'ce-manager-report.json').write_text(encoded)
        print(json.dumps({key:report.get(key) for key in ('runId','validation','strictPassed','error')}),flush=True)


if __name__=='__main__':main()
