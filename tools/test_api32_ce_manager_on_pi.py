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
NORMAL_PACKAGE='dev.kreijstal.aosppocket.api32managerimage'


def persist_test_credential_recovery(work,payload):
    value=json.loads(payload)
    assert re.fullmatch('[a-f0-9]{32}',value['runId'])
    assert len(value['passwords'])==3 and len(set(value['passwords']))==3
    assert all(re.fullmatch('P32[a-f0-9]{14}',password) for password in value['passwords'])
    path=work/'test-credentials.json'
    descriptor=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
    with os.fdopen(descriptor,'wb') as output:
        output.write(payload);output.flush();os.fsync(output.fileno())
    info=path.lstat();assert stat.S_ISREG(info.st_mode) and info.st_uid==os.getuid() and info.st_mode&0o777==0o600
    directory=os.open(work,os.O_RDONLY|os.O_DIRECTORY)
    try:os.fsync(directory)
    finally:os.close(directory)
    return path


def persist_stopped_cycle(work,run_id,index,cycle):
    assert re.fullmatch('[a-f0-9]{32}',run_id)
    assert type(index) is int and index in range(6)
    assert cycle['runId']==run_id and type(cycle['index']) is int and cycle['index']==index
    path=work/('ce-manager-cycle-'+run_id+'-'+str(index)+'.json')
    with path.open('x') as output:
        output.write(json.dumps(cycle,indent=2)+'\n');output.flush();os.fsync(output.fileno())
    with path.open('rb') as source:digest=hashlib.file_digest(source,'sha256').hexdigest()
    return {'index':index,'path':str(path),'sha256':digest,'migration':cycle['migration']}


def validate_retained_plaintext_baseline(previous):
    assert previous['validation']=='PASS' and previous['strictPassed']
    assert previous['managerPackage']==NORMAL_PACKAGE and previous['embeddedManagerControlVerified']
    assert previous['app']['stoppedSession']['ownerAlive'] is False and previous['app']['stoppedSession']['ownerExitCode']==0
    assert previous['app']['storageEncryptionEnabledBeforeStart'] is False
    assert previous['app']['fiveRootEncryptionLayoutBeforeStart'] is False
    assert re.fullmatch('[a-f0-9]{64}',previous['apkSha256']) and re.fullmatch('[a-f0-9]{64}',previous['imageSha256'])


def load_recovered_retained_profile(base,work,failure_path,recovery_path,previous):
    """Authenticate public recovery evidence before any APK/password change.

    This deliberately does not turn the failed migration trial into a pass.
    It authorizes a new encrypted lifecycle in that exact restored profile.
    No credential file is opened here.
    """
    pins={}
    def read(path):
        path=Path(path);relative=path.relative_to(base)
        assert '..' not in relative.parts and path.parent!=work
        cursor=base
        for part in relative.parts:
            cursor=cursor/part;assert not cursor.is_symlink(),'Recovery report path redirects'
        with os.fdopen(os.open(path,os.O_RDONLY|os.O_NOFOLLOW),'rb') as stream:
            info=os.fstat(stream.fileno());assert stat.S_ISREG(info.st_mode) and info.st_uid==os.getuid() and info.st_size<8*1024*1024
            raw=stream.read(8*1024*1024);assert len(raw)==info.st_size
        pins[str(path)]=hashlib.sha256(raw).hexdigest();return json.loads(raw)
    assert previous['validation']=='PASS' and previous['strictPassed'] is True and previous['managerPackage']==NORMAL_PACKAGE and previous['vmInstance']==73
    assert previous['embeddedManagerControlVerified'] is True and previous['externalVolumeVerified'] is True
    assert all(re.fullmatch('[a-f0-9]{64}',previous[k]) for k in ['apkSha256','imageSha256'])
    assert previous['app']['storageEncryptionEnabledBeforeStart'] is True and previous['app']['fiveRootEncryptionLayoutBeforeStart'] is True
    assert previous['app']['stoppedSession']['ownerAlive'] is False and previous['app']['stoppedSession']['ownerExitCode']==0
    assert Path(failure_path).name=='ce-manager-report.json' and Path(recovery_path).name=='restore-report.json'
    assert Path(failure_path).parent!=Path(recovery_path).parent
    failed=read(failure_path);recovered=read(recovery_path)
    assert failed['validation']=='FAIL' and failed['managerPackage']==NORMAL_PACKAGE and failed['vmInstance']==73
    assert failed['retainedPlaintextMigration'] is True and failed['guestDataReset'] is False and failed['freshProfile'] is False
    source=failed['runId'];assert re.fullmatch('[a-f0-9]{32}',source)
    assert recovered['status']=='PASS_OWN_TEST_AUTHLESS_RESTORATION_AND_AUTOMATIC_REOPEN' and recovered['vm']==73
    assert recovered['sourceRunId']==source and recovered['sourceCredentialsRemovedAfterVerifiedAuthlessBoot'] is True
    assert recovered['failedFullLifecycleStillFailed'] is True and recovered['guestDataReset'] is False and recovered['historicalCredentialsRead'] is False
    assert not os.path.lexists(Path(failure_path).parent/'test-credentials.json'),'Inspect this run recovery credentials before another trial'
    assert [c['index'] for c in failed['cycles']]==list(range(4))
    for item in failed['cycles']:
        path=Path(item['path']);assert path.parent==Path(failure_path).parent and path.name=='ce-manager-cycle-'+source+'-'+str(item['index'])+'.json'
        raw=read(item['path']);assert pins[item['path']]==item['sha256'] and raw['runId']==source and raw['index']==item['index']
        assert raw['migration']==(item['index']==0) and inspect_cycle(raw,require_kernel_ownership=True,require_external_volume=True)['strictPassed']
    snapshot=read(Path(failure_path).parent/'app-failure-snapshot.json')
    assert snapshot['runId']==source and snapshot['validation']=='FAIL' and snapshot['cycleIndex']==4 and snapshot['retainedPlaintextMigration']
    inventory=snapshot['retainedInventoryBefore'];assert inventory['includesKeptData'] and inventory['packages']
    phases=[]
    assert len(recovered['phases'])==2
    for index,item in enumerate(recovered['phases']):
        path=Path(item['path']);assert path.parent==Path(recovery_path).parent and path.name=='phase-'+str(index)+'-result.json'
        state=read(path);assert pins[str(path)]==item['sha256'] and state['runId']==item['runId'] and state['sourceRunId']==source
        assert state['validation']=='PASS' and state['originalPasswordFreeStateRestored'] and state['originalAppPreserved']
        assert state['verifyAuthlessOnly'] is bool(index) and state['authlessBootVerified'] is bool(index) and state['keyguard']['secure'] is False
        assert state['readableCanariesVerified']==5 and state['publicCanaryVerified'] and state['app']['cryptoValid'] and state['app']['nonExportable']
        assert all(state['app'][k]==snapshot['originalApp'][k] for k in ['token','cipherDigest','uid','canonicalDataPath'])
        assert inspect_cycle(state['cycle'],require_kernel_ownership=True,require_external_volume=True)['strictPassed']
        phases.append(state)
    assert len({s['runId'] for s in phases})==2 and source not in {s['runId'] for s in phases}
    return {'failed':failed,'recovered':recovered,'snapshot':snapshot,'authlessState':phases[-1],'sourcePins':pins}


def validate_precredential_space_failure(failed,preserved,image_sha):
    error='java.io.IOException: Android 12L failed: Not enough storage to download and set up Android 12L'
    assert failed['validation']=='FAIL' and failed['error']==error and failed['cycles']==[]
    assert failed['managerPackage']==PACKAGE+'3' and failed['imageSha256']==image_sha
    assert failed['preservedReportRunId']==preserved['runId'] and failed['preservedRunId']==preserved['preservedRunId']
    assert failed['testCredentialsRetainedForRecovery']
    app=failed['app'];assert app['validation']=='FAIL' and app['error']==error and app['preservedLifecycle']
    assert app['completedCheckpointPredecessor']==preserved['runId']
    assert 'ManagerSessionProbe32.boot(' in app['errorStack']
    assert all(key not in app for key in ['initialCredential','prepared','originalApp','encryptedBoots','cycleIndex','completedCheckpointArchive'])
    session=app['failedSession'];assert session['sdk']==32 and not session['ready'] and not session['running'] and session['closed']
    assert session['nativeLog']=='' and not session.get('ownerAlive',False)
    assert re.fullmatch('[a-f0-9]{64}',failed['managerSha256'])


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--vm-root',type=Path,default=Path('/home/pi/pocket-api32-vm'))
    p.add_argument('--work-dir',type=Path)
    p.add_argument('--package',choices=(PACKAGE,PACKAGE+'2',PACKAGE+'3',NORMAL_PACKAGE),default=PACKAGE)
    p.add_argument('--baseline-report',type=Path,help='Completed strict app report for this exact image')
    p.add_argument('--retained-plaintext-report',type=Path,help='Explicit completed plaintext baseline for the existing normal Manager; requires matching test-only APK asset')
    p.add_argument('--preserved-profile-report',type=Path,help='Continue the credential lifecycle in the verified stopped authless CE3 profile without resetting its data')
    p.add_argument('--recovered-retained-report',type=Path,help='Exact completed two-boot authless recovery report for the failed normal-profile migration trial')
    p.add_argument('--retained-failure-report',type=Path,help='Original failed migration report; required with --recovered-retained-report and preserved unchanged')
    p.add_argument('--prior-checkpoint-rejection-report',type=Path,help='Exact terminal pre-credential checkpoint rejection when continuing a completed preserved-profile test')
    p.add_argument('--precredential-space-failure-report',type=Path,help='Exact stopped pre-boot download-space failure; preserve its unused recovery file and verify authless state before new credentials')
    p.add_argument('--resume-outer-report',type=Path,help='Resume only outer restart after five verified guest cycles and a host-verifier NameError; keep the installed APK and original failed report')
    args=p.parse_args();base=args.vm_root.resolve();work=(args.work_dir or base/'ce-manager-validation').resolve()
    retained=args.retained_plaintext_report is not None;recovered_retained=args.recovered_retained_report is not None
    if bool(args.retained_failure_report)!=recovered_retained:p.error('--recovered-retained-report and --retained-failure-report are required together')
    if (args.package==NORMAL_PACKAGE)!=(retained or recovered_retained):p.error('The normal Manager requires explicit retained plaintext or recovered retained evidence')
    if recovered_retained and (not args.baseline_report or retained or args.preserved_profile_report or args.resume_outer_report
            or args.prior_checkpoint_rejection_report or args.precredential_space_failure_report):
        p.error('--recovered-retained-report requires --baseline-report and no other profile recovery mode')
    if retained and (not args.baseline_report or args.baseline_report.resolve()!=args.retained_plaintext_report.resolve()
            or args.preserved_profile_report or args.resume_outer_report or args.prior_checkpoint_rejection_report or args.precredential_space_failure_report):
        p.error('--retained-plaintext-report requires the same --baseline-report and no encrypted-profile recovery flags')
    if args.prior_checkpoint_rejection_report and not args.preserved_profile_report:p.error('--prior-checkpoint-rejection-report requires --preserved-profile-report')
    if args.resume_outer_report and (not args.preserved_profile_report or args.prior_checkpoint_rejection_report):p.error('--resume-outer-report requires --preserved-profile-report and a separate work directory')
    if args.precredential_space_failure_report and (not args.preserved_profile_report or args.resume_outer_report or args.prior_checkpoint_rejection_report):p.error('--precredential-space-failure-report requires only --preserved-profile-report')
    package=args.package;activity=package+'/dev.kreijstal.aosppocket.runtime.ManagerCeProbe32'
    assert work.is_relative_to(base) and (base/'.pocket-api32-test-vm').is_file()
    assert set(json.loads((base/'runtime/assembly/cuttlefish_config.json').read_text())['instances'])=={'73'}
    s=importlib.util.spec_from_file_location('vm',base/'tools/cuttlefish_arm64.py');vm=importlib.util.module_from_spec(s);s.loader.exec_module(vm)
    assert vm.BASE.resolve()==base and vm.lock()['instance']==73;vm.require_virtual()
    baseline=(args.baseline_report or base/'manager-app-validation/manager-app-report.json').resolve();assert baseline.is_relative_to(base)
    previous=json.loads(baseline.read_text())
    if retained:
        assert baseline.parent!=work
        validate_retained_plaintext_baseline(previous)
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
    preserved=None;preserved_state=None;preserved_nonce=None;checkpoint_rejection=None;space_failure=None;resume=None;resume_path=None;recovery_evidence=None
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
        if args.precredential_space_failure_report:
            path=args.precredential_space_failure_report.resolve();assert path.is_relative_to(base) and path.parent!=work
            space_failure=json.loads(path.read_text());validate_precredential_space_failure(space_failure,preserved,previous['imageSha256'])
            recovery=path.parent/'test-credentials.json';info=recovery.lstat()
            assert stat.S_ISREG(info.st_mode) and info.st_uid==os.getuid() and info.st_mode&0o777==0o600
            expected_installed=space_failure['managerSha256']
        old=adb('shell','pm path '+package).splitlines();assert len(old)==1 and old[0].startswith('package:/data/app/')
        assert adb('shell','sha256sum '+shlex.quote(old[0][8:])).split()[0]==expected_installed,'Inspect a previous preserved-lifecycle attempt before replacing its APK'
    elif recovered_retained:
        recovery_evidence=load_recovered_retained_profile(base,work,args.retained_failure_report,args.recovered_retained_report,previous)
        restored=recovery_evidence['recovered'];failed=recovery_evidence['failed'];preserved_state=recovery_evidence['authlessState'];preserved_nonce=failed['runId']
        preserved=dict(restored,runId=preserved_state['runId'],imageSha256=failed['imageSha256'],managerSha256=previous['apkSha256'])
        assert 'package:'+package in installed_packages
        old=adb('shell','pm path '+package).splitlines();assert len(old)==1 and old[0].startswith('package:/data/app/')
        assert adb('shell','sha256sum '+shlex.quote(old[0][8:])).split()[0]==previous['apkSha256'],'Recovered normal profile differs from its completed encrypted baseline'
        assert re.findall(r'(?m)^\s*max_phantom_processes=(\d+)\s*$',adb('shell','dumpsys activity settings'))==['32']
    elif retained:
        assert 'package:'+package in installed_packages
        installed=adb('shell','pm path '+package).splitlines();assert len(installed)==1 and installed[0].startswith('package:/data/app/')
        assert adb('shell','sha256sum '+shlex.quote(installed[0][8:])).split()[0]==previous['apkSha256'],'Retained Manager differs from completed plaintext baseline'
        policy=adb('shell','dumpsys activity settings')
        assert re.findall(r'(?m)^\s*max_phantom_processes=(\d+)\s*$',policy)==['32']
        assert adb('shell','getprop persist.sys.fflag.override.settings_enable_monitor_phantom_procs')==''
    else:assert 'package:'+package not in installed_packages,'CE test package already exists; inspect its exact prior state before reuse'
    meta=json.loads((work/'build.json').read_text());apk=work/'api32-ce-manager-probe.apk'
    assert meta['package']==package and meta['probe'] and meta['guestImagesBundled']==0 and meta['guestSdks']==[28,29,32]
    assert hashlib.file_digest(apk.open('rb'),'sha256').hexdigest()==meta['sha256']
    with zipfile.ZipFile(apk) as z:
        assert ('assets/api32-ce-retained-plaintext' in z.namelist())==retained
        assert ('assets/api32-ce-recovered-retained' in z.namelist())==recovered_retained
        if retained:assert z.read('assets/api32-ce-retained-plaintext')==b'v1\n'
        if recovered_retained:assert z.read('assets/api32-ce-recovered-retained')==b'v1\n'
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
    report={'runId':resume['runId'] if resume else uuid.uuid4().hex,'vmInstance':73,'managerPackage':package,'managerSha256':meta['sha256'],'imageSha256':image['sha256'],'baselineRunId':previous['runId'],'cycles':[],'freshProfile':preserved is None and not retained}
    if retained:
        report.update(retainedPlaintextMigration=True,guestDataReset=False,baselineReportSha256=hashlib.file_digest(baseline.open('rb'),'sha256').hexdigest(),historicalCredentialsRead=False)
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
    if recovered_retained:
        report.update(recoveredRetainedProfile=True,previousMigrationLifecycleStillFailed=True,historicalCredentialsRead=False,
            recoveredSourcePins=recovery_evidence['sourcePins'],originalRetainedPackageCount=len(recovery_evidence['snapshot']['retainedInventoryBefore']['packages']),
            baselineReportSha256=hashlib.file_digest(baseline.open('rb'),'sha256').hexdigest())
    if checkpoint_rejection:report['priorCheckpointRejectionRunId']=checkpoint_rejection['runId']
    if space_failure:
        report['priorPrecredentialSpaceFailureRunId']=space_failure['runId']
        report['priorUnusedRecoveryFilePreserved']=True
    settings=[('selinux','getenforce'),('phantomLimit','device_config get activity_manager max_phantom_processes'),('phantomMonitoring','settings get global settings_enable_monitor_phantom_procs')]
    for key,command in settings:report[key+'Before']=adb('shell',command)
    assert report['selinuxBefore']=='Enforcing'
    if retained or recovered_retained:assert report['phantomLimitBefore']==report['phantomMonitoringBefore']=='null'
    # Check kept-data/uninstalled packages too, and never replace an earlier
    # encryption trial. Put Home in front so another old probe cannot resume.
    adb('shell','am start -W -a android.intent.action.MAIN -c android.intent.category.HOME')
    if not resume:
        install=['install','--no-incremental'];
        if preserved or retained:install.append('-r')
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
    secret_value={'runId':report['runId'],'passwords':passwords}
    if recovered_retained:
        secret_value['preservedRetained']={'sourceRunId':preserved_nonce,'originalApp':{k:preserved_state['app'][k] for k in ['token','cipherDigest','uid','canonicalDataPath']}}
    secret_payload=json.dumps(secret_value).encode();assert len(secret_payload)<=4096
    if not resume:
        # Every trial, including retained plaintext and fresh sandboxes, must
        # remain recoverable if the host driver exits before restoration.
        # Persist before exposing the credential channel or starting the probe.
        credential_file=persist_test_credential_recovery(work,secret_payload)
        credential_hash=hashlib.sha256(secret_payload).digest()
        report['testCredentialStorage']='Owner-only0600 Pi host file, outside Android; retained on failure for deliberate recovery'
    if resume:report['testCredentialStorage']='Exact prior run owner0600 Pi recovery file; not copied; remove only after verified authless restoration'
    inventory_requests=[];inventory_path=secret_path+'/retained-inventory'
    inventory_payload=json.dumps({'runId':report['runId'],'sourceRunId':preserved_nonce,'inventory':recovery_evidence['snapshot']['retainedInventoryBefore']}).encode() if recovered_retained else None
    assert inventory_payload is None or len(inventory_payload)<128*1024
    class SecretHandler(http.server.BaseHTTPRequestHandler):
        def log_message(self,*args):pass
        def do_GET(self):
            if self.path==secret_path:secret_requests.append(1);payload=secret_payload
            elif inventory_payload is not None and self.path==inventory_path:inventory_requests.append(1);payload=inventory_payload
            else:self.send_error(404);return
            self.send_response(200);self.send_header('Content-Type','application/json')
            self.send_header('Content-Length',str(len(payload)));self.end_headers();self.wfile.write(payload)
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
                saved=persist_stopped_cycle(work,report['runId'],index,cycle)
                report['stoppedCyclePendingValidation']=saved
                report['failedAppProgress']=a
                checks=inspect_cycle(cycle,require_kernel_ownership=image.get('child_supervision')=='kernel-subreaper',require_external_volume=image.get('external_volume')=='rootless-v1')
                children=re.findall(r'^INIT_CHILD service=(\S+) pid=(\d+)$',cycle['console'],re.M)
                reaped=re.findall(r'^INIT_REAP service=(\S+) pid=(\d+) code=\d+ status=\d+$',cycle['console'],re.M)
                checks['supervisedLifetimeCount']=len(children)
                checks['reapAccounting']=bool(children and len(checks['initStop'])==1
                    and int(checks['initStop'][0][0])==len(children)==len(reaped) and sorted(children)==sorted(reaped))
                saved['check']=checks
                assert checks['strictPassed'] and checks['reapAccounting'],checks
                report['cycles'].append(saved)
                report.pop('stoppedCyclePendingValidation');report.pop('failedAppProgress')
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
        if retained:start+=' --ez ce_retained_plaintext true'
        if preserved:start+=' --ez ce_preserved_lifecycle true --es ce_preserved_nonce '+preserved_nonce
        if recovered_retained:start+=' --ez ce_recovered_retained true'
        if preserved and preserved.get('fullPreservedCredentialLifecycleVerified'):
            start+=' --es ce_completed_predecessor '+preserved['runId']
        if resume:report['app']=resume['app']
        else:
            print(adb('shell',start),flush=True)
            report['app']=observe(time.monotonic()+(4800 if retained else 3000))
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
        if retained:
            assert report['app']['retainedPlaintextMigration'] and report['app']['initialAuthlessKeyguard']['secure'] is False
            require_readiness(report['app']['initialAuthlessReadiness'])
            assert report['app']['capacityBeforeWrites']['requiredBytes']>0 and report['app']['capacityBeforeCredentialChange']['requiredBytes']>0
            assert all(boot['retainedAppsVerified'] for boot in report['app']['encryptedBoots'])
            assert report['cycles'][0]['migration'] is True and all(not c['migration'] for c in report['cycles'][1:])
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
        if recovered_retained:
            assert report['app']['recoveredRetainedProfile'] and report['app']['retainedAppsVerified']
            assert all(boot['retainedAppsVerified'] for boot in report['app']['encryptedBoots'])
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
        if preserved or retained:assert resumed.get('originalPasswordFreeStateRestored') is True
        if retained:
            assert resumed['retainedAppsVerified'] and resumed['retainedPlaintextMigration']
            report['fullRetainedPlaintextMigrationVerified']=True
        if recovered_retained:
            assert resumed['retainedAppsVerified'] and resumed['recoveredRetainedProfile'] and len(inventory_requests)==2
            # Keep the only recovery credentials until the final default slot
            # has actually survived a cold, credential-free host/guest boot.
            # This existing read-only probe branches before loadTestPasswords.
            no_owners=adb('shell','ps -A -o PID,NAME,ARGS')
            assert not any(any(tag in row for tag in ['pocket-init-api','pocket-core-api32','fuse-stock','pocket-media-client']) for row in no_owners.splitlines())
            assert adb('shell','pidof '+package)==str(resumed['hostPid'])
            adb('shell','am force-stop '+package)
            authless_nonce=uuid.uuid4().hex
            report['finalAuthlessProof']={'runId':authless_nonce,'credentialChannelProvided':False,'credentialsRequested':None,'rawCycle':None}
            final=report['finalAuthlessProof'];acknowledged=False
            intent='am start -n '+activity+' --es probe_run_id '+authless_nonce+' --ez ce_authless_verify true --ez ce_preserved_lifecycle true --ez ce_recovered_retained true --es ce_preserved_nonce '+preserved_nonce
            adb('shell',intent);deadline=time.monotonic()+1200
            while True:
                current=state('pocket-ce-progress','POCKET_SESSION32')
                if current.get('runId')==authless_nonce:
                    if current.get('stage')=='cycle-stopped' and not acknowledged:
                        raw=state('pocket-ce-cycle','POCKET_CE_CYCLE')
                        saved=persist_stopped_cycle(work,authless_nonce,0,raw);final['rawCycle']=saved
                        checks=inspect_cycle(raw,require_kernel_ownership=True,require_external_volume=True);saved['check']=checks
                        children=re.findall(r'^INIT_CHILD service=(\S+) pid=(\d+)$',raw['console'],re.M)
                        reaped=re.findall(r'^INIT_REAP service=(\S+) pid=(\d+) code=\d+ status=\d+$',raw['console'],re.M)
                        checks['reapAccounting']=bool(children and len(checks['initStop'])==1 and int(checks['initStop'][0][0])==len(children)==len(reaped) and sorted(children)==sorted(reaped))
                        assert checks['strictPassed'] and checks['reapAccounting'],checks
                        adb('shell','am start -n '+activity+' --es probe_run_id '+authless_nonce+' --ei ce_cycle_ack 0');acknowledged=True
                    if current.get('validation'):
                        final_path=work/('ce-manager-final-authless-'+authless_nonce+'.json')
                        with final_path.open('x') as output:output.write(json.dumps(current,indent=2)+'\n');output.flush();os.fsync(output.fileno())
                        final.update(path=str(final_path),sha256=hashlib.file_digest(final_path.open('rb'),'sha256').hexdigest())
                        assert current['validation']=='PASS' and acknowledged,current.get('error','Final authless boot failed')
                        assert current['authlessBootUnlocked'] and current['keyguard']['secure'] is False and current['app']['androidUserUnlocked']
                        assert current['readableCanariesVerified']==5 and current['unlockedExternalCanaryVerified']
                        assert all(current['app'][k]==report['app']['originalApp'][k] for k in ['token','cipherDigest','uid','canonicalDataPath'])
                        assert current['hostPid']!=resumed['hostPid'] and len(secret_requests)==2 and len(inventory_requests)==2
                        final.update(verified=True,hostPid=current['hostPid'],credentialsRequested=False);break
                if time.monotonic()>=deadline:raise TimeoutError('Final authless observation expired; inspect the exact guest before any restart; own credentials retained')
                time.sleep(1)
            report['fullRecoveredRetainedCredentialLifecycleVerified']=True
            report['originalRetainedAppInventoryAcrossSixBootsVerified']=True
            report['finalColdAuthlessReopenVerified']=True
        report['managerProcessRestartVerified']=True;report['pollErrors']=errors
        for key,command in settings:
            report[key+'After']=adb('shell',command);assert report[key+'After']==report[key+'Before']
        report['strictPassed']=True;report['validation']='PASS'
        report['fullPreservedCredentialLifecycleVerified']=preserved is not None
        report['plaintextMigrationRepeated']=preserved is None
        report['mountedStorageAcrossCredentialLifecycleVerified']=image.get('external_volume')=='rootless-v1'
        if credential_file is not None and resumed.get('originalPasswordFreeStateRestored') is True:
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
