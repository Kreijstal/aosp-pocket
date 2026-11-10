#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Resume only the failed authless-provider check in the preserved VM73 CE3 profile."""
import argparse,base64,hashlib,importlib.util,json,re,shlex,socket,time,uuid,zipfile
from collections import Counter
from pathlib import Path
from test_api32_persistence_on_pi import inspect_cycle

PACKAGE='dev.kreijstal.aosppocket.api32ceimage3'
ACTIVITY=PACKAGE+'/dev.kreijstal.aosppocket.runtime.ManagerCeProbe32'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work-dir',type=Path,required=True)
    parser.add_argument('--prior-attempt-report',type=Path,help='Deliberate changed-code follow-up after a terminal authless failure')
    args=parser.parse_args();base=Path('/home/pi/pocket-api32-vm');work=args.work_dir.resolve()
    assert work.is_relative_to(base) and (base/'.pocket-api32-test-vm').is_file()
    assert set(json.loads((base/'runtime/assembly/cuttlefish_config.json').read_text())['instances'])=={'73'}
    spec=importlib.util.spec_from_file_location('vm',base/'tools/cuttlefish_arm64.py')
    vm=importlib.util.module_from_spec(spec);spec.loader.exec_module(vm)
    assert vm.BASE.resolve()==base and vm.lock()['instance']==73;vm.require_virtual()
    prior=json.loads((base/'source-host-ce-validation/ce-manager-report.json').read_text())
    assert prior['runId']=='dc632c57ab9e46fc8b62e2be9bd10786' and prior['validation']=='FAIL'
    previous=prior['app'];assert previous['encryptedPhase']==2 and previous['encryptedBoots'][-1]['authlessBootUnlocked']
    assert previous['failedSession']['ownerExitCode']==0 and previous['failedSession']['ownerAlive'] is False
    assert 'Ordinary CE app provider unavailable' in previous['error']
    assert [c['index'] for c in prior['cycles'] if c['check']['strictPassed']]==[0,1,2]
    expected_installed=prior['managerSha256'];attempt=None
    if args.prior_attempt_report:
        path=args.prior_attempt_report.resolve();assert path.is_relative_to(base)
        attempt=json.loads(path.read_text())
        assert attempt['runId']=='7dad2e51a44d4f6ab8b8b10505145028' and attempt['validation']=='FAIL'
        assert attempt['preservedRunId']==prior['runId'] and attempt['imageSha256']==prior['imageSha256']
        assert attempt['app']['authlessBootUnlocked'] and attempt['app']['app']['androidUserUnlocked']
        assert attempt['app']['failedSession']['ownerAlive'] is False and attempt['app']['failedSession']['ownerExitCode']==0
        assert attempt['app']['failedSession']['cleanupFailure']=='Guest supervisor left processes requiring cleanup'
        expected_installed=attempt['managerSha256']
    apk=work/'api32-ce-manager-probe.apk';meta=json.loads((work/'build.json').read_text())
    assert hashlib.file_digest(apk.open('rb'),'sha256').hexdigest()==meta['sha256']
    with zipfile.ZipFile(apk) as archive:
        image=next(i for i in json.loads(archive.read('assets/pocket-images.json'))['images'] if i['sdk']==32)
        assert image['sha256']==prior['imageSha256']
        libraries=meta['hostNativeSourceBuild']['libraries']
        assert set(libraries)=={'libpocket_runtime.so','libproot.so','libpocket_frames.so','libpocket_ce.so'}
        for name,record in libraries.items():assert hashlib.sha256(archive.read('lib/arm64-v8a/'+name)).hexdigest()==record['sha256']
        assert b'ce_authless_verify' in archive.read('classes.dex') and b'POCKET_CE32_USER_UNLOCKED wait_ms=' in archive.read('classes.dex')
        if meta.get('ceInitTrial'):
            assert attempt is not None,'A native-init trial must identify the terminal prior attempt'
            assert hashlib.sha256(archive.read('assets/api32-ce-init')).hexdigest()==meta['ceInitTrial']['assetSha256']
            assert b'API32_INIT_SUBREAPER_READY' in archive.read('assets/api32-ce-init')
    def adb(*argv,timeout=40):
        result=vm.adb('-s',vm.serial(),*argv,check=False,timeout=timeout)
        if result.returncode:raise RuntimeError((result.stdout+result.stderr)[-2000:])
        return result.stdout.strip()
    def no_owners():
        text=adb('shell','ps -A -o PID,NAME,ARGS')
        assert not any(any(n in row for n in ['pocket-init-api','pocket-core-api32','fuse-stock','pocket-media-client']) for row in text.splitlines())
    no_owners()
    installed=adb('shell','pm path '+PACKAGE).splitlines();assert len(installed)==1 and installed[0].startswith('package:/data/app/')
    assert adb('shell','sha256sum '+shlex.quote(installed[0][8:])).split()[0]==expected_installed,'Inspect an earlier resume attempt before replacing its APK'
    with socket.socket() as connection:
        connection.settimeout(2);assert connection.connect_ex(('127.0.0.1',8794))!=0,'Image source must remain offline'
    report={'runId':uuid.uuid4().hex,'vmInstance':73,'managerPackage':PACKAGE,'managerSha256':meta['sha256'],
        'imageSha256':image['sha256'],'preservedRunId':prior['runId'],'freshProfile':False,'guestDataReset':False,
        'fullCredentialLifecycleVerified':False,'imageSourceOffline':True,'passwordOperationsRequested':False}
    report['runtimeCodeReplaced']=bool(meta.get('ceInitTrial'))
    if attempt:report['priorAttemptRunId']=attempt['runId']
    settings=[('selinux','getenforce'),('phantomLimit','device_config get activity_manager max_phantom_processes'),
        ('phantomMonitoring','settings get global settings_enable_monitor_phantom_procs')]
    for key,command in settings:report[key+'Before']=adb('shell',command)
    assert report['selinuxBefore']=='Enforcing'
    def state(argument,prefix):
        text=adb('shell','dumpsys activity '+ACTIVITY+' '+argument)
        rows=[line.strip().split('=',1)[1] for line in text.splitlines() if line.strip().startswith(prefix+'=')]
        return json.loads(base64.b64decode(rows[-1],validate=True)) if rows else {}
    try:
        adb('shell','am start -W -a android.intent.action.MAIN -c android.intent.category.HOME')
        print(adb('install','--no-incremental','-r',str(apk),timeout=180),flush=True)
        assert 'DEBUGGABLE' not in adb('shell','dumpsys package '+PACKAGE)
        installed=adb('shell','pm path '+PACKAGE).splitlines();assert len(installed)==1
        assert adb('shell','sha256sum '+shlex.quote(installed[0][8:])).split()[0]==meta['sha256']
        no_owners();adb('shell','am force-stop '+PACKAGE)
        print(adb('shell','am start -n '+ACTIVITY+' --es probe_run_id '+report['runId']+
            ' --ez ce_authless_verify true --es ce_preserved_nonce '+prior['runId']),flush=True)
        deadline=time.monotonic()+1200;last=None;errors=0;cycle=None
        while time.monotonic()<deadline:
            try:current=state('pocket-ce-progress','POCKET_SESSION32')
            except Exception:errors+=1;time.sleep(2);continue
            if current.get('runId')!=report['runId']:time.sleep(1);continue
            stage=current.get('stage'),current.get('managerStatus')
            if stage!=last:print('CE_AUTHLESS_STAGE',*stage,flush=True);last=stage
            if current.get('stage')=='cycle-stopped' and cycle is None:
                cycle=state('pocket-ce-cycle','POCKET_CE_CYCLE');assert cycle['runId']==report['runId'] and cycle['index']==0 and cycle['migration'] is False
                checks=inspect_cycle(cycle)
                spawned=re.findall(r'(?m)^INIT_CHILD service=(\S+) pid=(\d+)$',cycle['console'])
                reaped=re.findall(r'(?m)^INIT_REAP service=(\S+) pid=(\d+) ',cycle['console'])
                checks['reapAccounting']=bool(spawned and Counter(spawned)==Counter(reaped) and len(checks['initStop'])==1
                    and int(checks['initStop'][0][0])==len(spawned))
                checks['supervisedLifetimeCount']=len(spawned)
                if meta.get('ceInitTrial'):
                    descendants=re.findall(r'API32_INIT_DESCENDANTS_STOPPED reaped=(\d+) signalled=(\d+) remaining=(\d+) query_error=(-?\d+)',cycle['console'])
                    checks['kernelChildOwnership']=bool(cycle['console'].count('API32_INIT_SUBREAPER_READY kernel_child_ownership=1')==1
                        and len(descendants)==1 and descendants[0][2:]==('0','0')
                        and int(descendants[0][0])==len(re.findall(r'(?m)^INIT_DESCENDANT_REAP pid=\d+ code=\d+ status=\d+$',cycle['console']))
                        and 'API32_INIT_DESCENDANT_QUERY_FAILED' not in cycle['console'])
                    checks['descendantStop']=descendants;assert checks['kernelChildOwnership'],checks
                path=work/'ce-authless-cycle.json';path.write_text(json.dumps(cycle,indent=2)+'\n')
                report['cycle']={'path':str(path),'sha256':hashlib.file_digest(path.open('rb'),'sha256').hexdigest(),'check':checks}
                assert checks['strictPassed'] and checks['reapAccounting'],checks
                adb('shell','am start -n '+ACTIVITY+' --es probe_run_id '+report['runId']+' --ei ce_cycle_ack 0')
            if current.get('validation'):report['app']=current;break
            time.sleep(1)
        else:raise TimeoutError('Observation expired; inspect this exact authless resume before any retry')
        assert current.get('validation')=='PASS',current.get('error','No passing authless provider check')
        assert cycle is not None and current.get('preservedRunId')==prior['runId'] and current.get('authlessBootUnlocked') is True
        assert current.get('fullCredentialLifecycleVerified') is False and current['keyguard']['secure'] is False
        if meta.get('ceInitTrial'):
            patch=current['initTrial'];assert patch['assetSha256']==meta['ceInitTrial']['assetSha256']
            assert patch['sourceSha256']==meta['ceInitTrial']['sourceSha256'] and patch['originalCodeRestored'] is True
            assert patch['originalSha256']==patch['restoredSha256'] and patch['guestDataReset'] is False
            report['initTrial']=patch;report['kernelChildOwnershipVerified']=True
        observed=current['app'];expected=previous['originalApp']
        assert all(observed[k]==expected[k] for k in ['token','cipherDigest','uid','canonicalDataPath'])
        assert observed['pid']!=expected['pid'] and observed['sdk']==32 and observed['androidUserUnlocked']
        assert observed['nonExportable'] and observed['cryptoValid'] and 0<=observed['androidUnlockWaitMs']<=61000
        report['ordinaryAppPersisted']=True;report['pollErrors']=errors
        for key,command in settings:report[key+'After']=adb('shell',command);assert report[key+'After']==report[key+'Before']
        no_owners();report['strictPassed']=True;report['validation']='PASS'
    except Exception as error:report['validation']='FAIL';report['error']=str(error);raise
    finally:
        (work/'ce-authless-report.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps({k:report.get(k) for k in ['runId','validation','strictPassed','error']}),flush=True)


if __name__=='__main__':main()
