#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Restore only the dedicated CE3 test profile's original password-free state."""
import argparse
import base64
import hashlib
import http.server
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import socket
import stat
import threading
import time
import uuid
import zipfile
from test_api32_ce_keyguard_on_pi import require_ui
from test_api32_persistence_on_pi import inspect_cycle

PACKAGE='dev.kreijstal.aosppocket.api32ceimage3'
ACTIVITY=PACKAGE+'/dev.kreijstal.aosppocket.runtime.ManagerCeProbe32'


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work-dir',type=Path,required=True)
    p.add_argument('--baseline-report',type=Path,required=True)
    p.add_argument('--original-report',type=Path,required=True)
    p.add_argument('--credential-file',type=Path,required=True)
    args=p.parse_args();base=Path('/home/pi/pocket-api32-vm');work=args.work_dir.resolve()
    assert work.is_relative_to(base) and (base/'.pocket-api32-test-vm').is_file()
    assert set(json.loads((base/'runtime/assembly/cuttlefish_config.json').read_text())['instances'])=={'73'}
    spec=importlib.util.spec_from_file_location('restore_vm',base/'tools/cuttlefish_arm64.py')
    vm=importlib.util.module_from_spec(spec);spec.loader.exec_module(vm)
    assert vm.BASE.resolve()==base and vm.lock()['instance']==73;vm.require_virtual()
    for path in [args.baseline_report,args.original_report,args.credential_file]:assert path.resolve().is_relative_to(base)
    prior=json.loads(args.baseline_report.read_text());original=json.loads(args.original_report.read_text())
    assert prior['runId']=='7e7a5d35b53d40178669e05a80425a91' and prior['validation']=='PASS' and prior['strictPassed']
    assert prior['managerPackage']==PACKAGE and prior['credentialFileUnchanged'] and prior['managerProcessRestartVerified']
    assert [c['index'] for c in prior['cycles'] if c['check']['strictPassed']]==[0,1]
    for phase in ['app','outerRestart']:
        require_ui(prior[phase]['keyguardUi']);assert prior[phase]['unlockedExternalCanaryVerified']
    assert original['runId']=='ed1b907536e84efdacfc5465eaadab78' and original['validation']=='FAIL'
    assert original['app']['encryptedBoots'][2]['passwordRestored'] is True
    assert prior['preservedCanaryRunId']==original['preservedRunId']
    assert not args.credential_file.is_symlink()
    with os.fdopen(os.open(args.credential_file,os.O_RDONLY|os.O_NOFOLLOW),'rb') as source:
        info=os.fstat(source.fileno());assert stat.S_ISREG(info.st_mode) and info.st_uid==os.getuid() and info.st_mode&0o777==0o600 and info.st_size<4096
        raw=source.read();secret_hash=hashlib.sha256(raw).digest();secrets=json.loads(raw)
    assert secrets['runId']==original['runId'] and len(secrets['passwords'])==3
    passwords=secrets['passwords'];assert all(re.fullmatch('P32[a-f0-9]{14}',s) for s in passwords)
    def adb(*argv,timeout=40):
        r=vm.adb('-s',vm.serial(),*argv,check=False,timeout=timeout)
        if r.returncode:raise RuntimeError((r.stdout+r.stderr)[-2000:])
        return r.stdout.strip()
    def no_owners():
        rows=adb('shell','ps -A -o PID,NAME,ARGS').splitlines()
        assert not any(any(s in row for s in ['pocket-init-api','pocket-core-api32','fuse-stock','pocket-media-client','system/bin/pocket-loader']) for row in rows)
    no_owners();old=adb('shell','pm path '+PACKAGE).splitlines();assert len(old)==1 and old[0].startswith('package:/data/app/')
    assert adb('shell','sha256sum '+shlex.quote(old[0][8:])).split()[0]==prior['managerSha256'],'Inspect a newer CE3 attempt before replacement'
    apk=work/'api32-ce-manager-probe.apk';meta=json.loads((work/'build.json').read_text())
    assert meta['package']==PACKAGE and meta['probe'] and meta['guestImagesBundled']==0
    assert hashlib.file_digest(apk.open('rb'),'sha256').hexdigest()==meta['sha256']
    with zipfile.ZipFile(apk) as z:
        image=next(i for i in json.loads(z.read('assets/pocket-images.json'))['images'] if i['sdk']==32)
        assert image['sha256']==prior['imageSha256'] and image['external_volume']=='rootless-v1' and image['child_supervision']=='kernel-subreaper'
        for name,pin in meta['hostNativeSourceBuild']['libraries'].items():assert hashlib.sha256(z.read('lib/arm64-v8a/'+name)).hexdigest()==pin['sha256']
        assert hashlib.sha256(z.read('lib/arm64-v8a/libpocket_external32.so')).hexdigest()==meta['externalHostSourceBuild']['sha256']
        assert b'ce_restore_authless' in z.read('classes.dex')
    report={'runId':uuid.uuid4().hex,'authlessRunId':uuid.uuid4().hex,'vmInstance':73,'managerPackage':PACKAGE,
        'managerSha256':meta['sha256'],'imageSha256':image['sha256'],'baselineRunId':prior['runId'],'originalRunId':original['runId'],
        'preservedCanaryRunId':prior['preservedCanaryRunId'],'guestDataReset':False,'ordinaryGuestAppReinstalled':False,
        'fullCredentialLifecycleVerified':False,'lockCredentialChangesRequested':True,'purpose':'Restore original dedicated CE3 password-free test state after verified two-boot physical unlock','cycles':[]}
    settings=[('selinux','getenforce'),('phantomLimit','device_config get activity_manager max_phantom_processes'),('phantomMonitoring','settings get global settings_enable_monitor_phantom_procs')]
    for key,command in settings:report[key+'Before']=adb('shell',command)
    assert report['selinuxBefore']=='Enforcing'
    assert 'tcp:8794' not in adb('reverse','--list') and 'tcp:8795' not in adb('reverse','--list')
    offline=socket.socket();offline.bind(('127.0.0.1',8794))
    secret_requests=[];payload=json.dumps({'runId':report['runId'],'passwords':passwords}).encode();secret_path='/'+report['runId']
    class Secrets(http.server.BaseHTTPRequestHandler):
        def log_message(self,*args):pass
        def do_GET(self):
            if self.path!=secret_path:self.send_error(404);return
            secret_requests.append(1);self.send_response(200);self.send_header('Content-Length',str(len(payload)));self.end_headers();self.wfile.write(payload)
    server=http.server.ThreadingHTTPServer(('127.0.0.1',8795),Secrets);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start();reverse=False;poll_errors=0
    def observe(phase,run_id):
        nonlocal poll_errors
        deadline=time.monotonic()+1500;last=None;seen=False
        while time.monotonic()<deadline:
            try:
                text=adb('shell','dumpsys activity '+ACTIVITY+' pocket-ce-progress')
                rows=[s.strip().split('=',1)[1] for s in text.splitlines() if s.strip().startswith('POCKET_SESSION32=')]
                state=json.loads(base64.b64decode(rows[-1],validate=True)) if rows else {}
            except Exception:poll_errors+=1;time.sleep(2);continue
            if state.get('runId')!=run_id:time.sleep(1);continue
            stage=state.get('stage'),state.get('managerStatus')
            if stage!=last:print('CE_RESTORE_STAGE',phase,*stage,flush=True);last=stage
            if state.get('stage')=='cycle-stopped' and not seen:
                text=adb('shell','dumpsys activity '+ACTIVITY+' pocket-ce-cycle')
                rows=[s.strip().split('=',1)[1] for s in text.splitlines() if s.strip().startswith('POCKET_CE_CYCLE=')]
                c=json.loads(base64.b64decode(rows[-1],validate=True));assert c['runId']==run_id and c['index']==0 and not c['migration']
                checks=inspect_cycle(c,require_kernel_ownership=True,require_external_volume=True)
                started=re.findall(r'(?m)^INIT_CHILD service=(\S+) pid=(\d+)$',c['console']);reaped=re.findall(r'(?m)^INIT_REAP service=(\S+) pid=(\d+) code=\d+ status=\d+$',c['console'])
                checks['reapAccounting']=bool(started and sorted(started)==sorted(reaped) and len(checks['initStop'])==1 and int(checks['initStop'][0][0])==len(started))
                cp=work/('ce-restore-'+phase+'-cycle.json');cp.write_text(json.dumps(c,indent=2)+'\n')
                report['cycles'].append({'phase':phase,'runId':run_id,'path':str(cp),'sha256':hashlib.file_digest(cp.open('rb'),'sha256').hexdigest(),'check':checks})
                assert checks['strictPassed'] and checks['reapAccounting'];seen=True
                adb('shell','am start -n '+ACTIVITY+' --es probe_run_id '+run_id+' --ei ce_cycle_ack 0')
            if state.get('validation'):assert seen or state['validation']=='FAIL';return state
            time.sleep(1)
        raise TimeoutError('Observation expired; inspect the exact restoration guest before any restart')
    def require_app(state):
        assert state['validation']=='PASS',state.get('error')
        assert state['preservedRunId']==report['preservedCanaryRunId'] and state['unlockedExternalCanaryVerified']
        current=state['app'];baseline=original['app']['originalApp']
        assert all(current[k]==baseline[k] for k in ['token','cipherDigest','uid','canonicalDataPath'])
        assert current['sdk']==32 and current['cryptoValid'] and current['nonExportable'] and current['androidUserUnlocked']
        assert current['externalStorage']['primaryEmulatedMounted'] and current['externalStorage']['externalPrimaryQuery']
        assert current['externalStorage']['externalPrimaryQueryRows']==0
        assert state['fullCredentialLifecycleVerified'] is False and state['readableCanariesVerified']==5
    try:
        adb('shell','am start -W -a android.intent.action.MAIN -c android.intent.category.HOME')
        print(adb('install','--no-incremental','-r',str(apk),timeout=180),flush=True)
        assert 'DEBUGGABLE' not in adb('shell','dumpsys package '+PACKAGE)
        installed=adb('shell','pm path '+PACKAGE).splitlines();assert len(installed)==1
        assert adb('shell','sha256sum '+shlex.quote(installed[0][8:])).split()[0]==meta['sha256']
        adb('reverse','tcp:8795','tcp:8795');reverse=True
        no_owners();adb('shell','am force-stop '+PACKAGE)
        command='am start -n '+ACTIVITY+' --es probe_run_id '+report['runId']+' --es ce_secret_url http://127.0.0.1:8795'+secret_path+' --ez ce_keyguard_resume true --ez ce_restore_authless true --es ce_preserved_nonce '+report['preservedCanaryRunId']
        print(adb('shell',command),flush=True);report['restoration']=observe('restoration',report['runId']);state=report['restoration'];require_app(state);require_ui(state['keyguardUi'])
        assert state['originalPasswordFreeStateRestored'] and state['restoreOriginalPasswordFreeStateRequested'] and state['lockCredentialChangesRequested']
        no_owners();assert adb('shell','pidof '+PACKAGE)==str(state['hostPid']);adb('shell','am force-stop '+PACKAGE)
        command='am start -n '+ACTIVITY+' --es probe_run_id '+report['authlessRunId']+' --ez ce_authless_verify true --es ce_preserved_nonce '+report['preservedCanaryRunId']
        print(adb('shell',command),flush=True);report['authlessRestart']=observe('authless',report['authlessRunId']);state=report['authlessRestart'];require_app(state)
        assert state['authlessBootUnlocked'] and state['keyguard']['secure'] is False
        assert state['hostPid']!=report['restoration']['hostPid'] and state['app']['pid']!=report['restoration']['app']['pid']
        assert len(secret_requests)==1 and len(report['cycles'])==2;no_owners()
        for key,command in settings:report[key+'After']=adb('shell',command);assert report[key+'After']==report[key+'Before']
        assert hashlib.sha256(args.credential_file.read_bytes()).digest()==secret_hash
        report.update(validation='PASS',strictPassed=True,originalPasswordFreeStateRestored=True,authlessOuterRestartVerified=True,ordinaryAppPreserved=True,credentialFileUnchanged=True,imageSourceOfflineForBothBoots=True)
    except Exception as error:report.update(validation='FAIL',error=str(error));raise
    finally:
        server.shutdown();server.server_close();thread.join(5);offline.close()
        if reverse:adb('reverse','--remove','tcp:8795')
        report['secretRequests']=len(secret_requests);report['pollErrors']=poll_errors;report['credentialFileRetainedForHistory']=args.credential_file.is_file()
        encoded=json.dumps(report,indent=2)+'\n'
        for password in passwords:encoded=encoded.replace(password,'<redacted>')
        (work/'ce-restore-report.json').write_text(encoded)
        print(json.dumps({k:report.get(k) for k in ['runId','validation','strictPassed','error']}),flush=True)


if __name__=='__main__':main()
