#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Verify SDK32 app data and keystore across an outer Manager restart with image source offline."""
import argparse
import base64
import functools
import hashlib
import http.server
import importlib.util
import json
from pathlib import Path
import re
import shlex
import threading
import time
import urllib.parse
import uuid
import zipfile

PACKAGE='dev.kreijstal.aosppocket.api32managerimage'
ACTIVITY=PACKAGE+'/dev.kreijstal.aosppocket.runtime.ManagerPersistenceProbe32'

def native_fatals(log):
    # ART can abort before libc emits its usual Fatal signal line. Tombstones
    # also remain authoritative when the compatibility signal number is zero.
    return [line for line in log.splitlines() if 'Fatal signal ' in line
        or 'JNI DETECTED ERROR IN APPLICATION:' in line or 'DEBUG:Abort message:' in line
        or re.search(r'^DEBUG:signal \d+ \(SIG(?:ABRT|SEGV|BUS|ILL|FPE|SYS|TRAP)\)',line)]


def inspect_cycle(cycle,require_kernel_ownership=False,require_external_volume=False):
    native=cycle['nativeLog'];console=cycle['console'];session=cycle['session']
    result={'anrs':re.findall(r'ANR in ([^\n]+)',native),
        'javaFatalHeaders':re.findall(r'FATAL EXCEPTION(?: IN SYSTEM PROCESS)?: ([^\n]+)',native),
        'nativeFatals':native_fatals(native),
        'systemUiStarts':re.findall(r'Start proc (\d+):com\.android\.systemui/',native),
        'nativeLogWithinLimit':len(native.encode('utf-16-le','surrogatepass'))//2<2*1024*1024,
        'initStop':re.findall(r'API32_INIT_STOPPED reaped=(\d+) remaining=(\d+) failed=(\d+)',console)}
    result['strictPassed']=bool(not result['anrs'] and not result['javaFatalHeaders'] and not result['nativeFatals']
        and len(result['systemUiStarts'])==1 and result['nativeLogWithinLimit'] and len(console.encode())<2*1024*1024
        and result['initStop'] and all(row[1:]==('0','0') for row in result['initStop'])
        and session.get('ownerExitCode')==0 and session.get('ownerAlive') is False and not session.get('cleanupFailure')
        and session['input']['inputEventsRejected']==0 and cycle['display']['frames_presented']>0)
    if require_kernel_ownership or 'API32_INIT_SUBREAPER_READY' in console:
        descendants=re.findall(r'API32_INIT_DESCENDANTS_STOPPED reaped=(\d+) signalled=(\d+) remaining=(\d+) query_error=(-?\d+)',console)
        result['descendantStop']=descendants
        result['kernelChildOwnershipVerified']=bool(console.count('API32_INIT_SUBREAPER_READY kernel_child_ownership=1')==1
            and len(descendants)==1 and descendants[0][2:]==('0','0')
            and int(descendants[0][0])==len(re.findall(r'(?m)^INIT_DESCENDANT_REAP pid=\d+ code=\d+ status=\d+$',console))
            and 'API32_INIT_DESCENDANT_QUERY_FAILED' not in console)
        result['strictPassed']=result['strictPassed'] and result['kernelChildOwnershipVerified']
    if require_external_volume:
        adapter=session.get('externalStorage',{})
        result['externalVolumeVerified']=bool(console.count('API32_EXTERNAL_VOLUME_MOUNT result=0 ')==1
            and 'API32_EXTERNAL_VOLUME_MOUNT result=-' not in console+native
            and not re.search(r'API32_EXTERNAL_VOLUME_(?:STOP|ADAPTER)\b[^\n]*\bresult=-\d+',console+native)
            and native.count('Starting session for id: emulated;0')==1
            and adapter.get('attached') is False and adapter.get('attaches')==1 and adapter.get('revocations')==0
            and adapter.get('workerStopped') is True and 'lastError' not in adapter)
        result['externalVolumeVerified']=result['externalVolumeVerified'] and adapter.get('replyFailures',0)==0
        result['strictPassed']=result['strictPassed'] and result['externalVolumeVerified']
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vm-root',type=Path,default=Path('/home/pi/pocket-api32-vm'))
    parser.add_argument('--work-dir',type=Path);args=parser.parse_args()
    base=args.vm_root.resolve();work=(args.work_dir or base/'manager-persistence-validation').resolve()
    assert (base/'.pocket-api32-test-vm').is_file() and work.is_relative_to(base)
    config=base/'runtime/assembly/cuttlefish_config.json';assert config.resolve().is_relative_to(base)
    assert set(json.loads(config.read_text())['instances'])=={'73'}
    spec=importlib.util.spec_from_file_location('persistence_vm',base/'tools/cuttlefish_arm64.py')
    vm=importlib.util.module_from_spec(spec);spec.loader.exec_module(vm)
    assert vm.BASE.resolve()==base and vm.lock()['instance']==73;vm.require_virtual()
    def adb(*arguments,timeout=40):
        result=vm.adb('-s',vm.serial(),*arguments,check=False,timeout=timeout)
        if result.returncode:raise RuntimeError((result.stdout+result.stderr)[-2000:])
        return result.stdout.strip()
    metadata=json.loads((work/'build.json').read_text());apk=work/'api32-persistence-manager-probe.apk'
    assert metadata['probe'] and metadata['guestImagesBundled']==0 and metadata['guestSdks']==[28,29,32]
    assert hashlib.file_digest(apk.open('rb'),'sha256').hexdigest()==metadata['sha256']
    with zipfile.ZipFile(apk) as archive:
        image=[v for v in json.loads(archive.read('assets/pocket-images.json'))['images'] if v['sdk']==32][0]
        assert b'Ldev/kreijstal/aosppocket/runtime/ManagerPersistenceProbe32;' in archive.read('classes.dex')
    url=urllib.parse.urlsplit(image['url']);filename=url.path.removeprefix('/')
    assert url.scheme=='http' and url.hostname=='127.0.0.1' and url.port==8794 and '/' not in filename
    payload=work/filename
    assert payload.stat().st_size==image['bytes'] and hashlib.file_digest(payload.open('rb'),'sha256').hexdigest()==image['sha256']
    report={'runId':uuid.uuid4().hex,'vmInstance':73,'serial':vm.serial(),'managerSha256':metadata['sha256'],'imageSha256':image['sha256'],
        'imageSourceOffline':False,'hostNetworkDisabled':False,'inputs':[]}
    for name,command in [('selinux','getenforce'),('phantomLimit','device_config get activity_manager max_phantom_processes'),
            ('phantomMonitoring','settings get global settings_enable_monitor_phantom_procs')]:report[name+'Before']=adb('shell',command)
    assert report['selinuxBefore']=='Enforcing'
    owners=adb('shell','ps -A -o PID,NAME,ARGS')
    assert not any(any(name in row for name in ['pocket-init-api32','pocket-core-api32','fuse-stock','pocket-media-client']) for row in owners.splitlines()),'An existing guest owns the VM'
    print(adb('install','--no-incremental','-r',str(apk),timeout=180),flush=True)
    assert 'DEBUGGABLE' not in adb('shell','dumpsys package '+PACKAGE)
    installed=adb('shell','pm path '+PACKAGE).splitlines();assert len(installed)==1 and installed[0].startswith('package:/data/app/')
    assert adb('shell','sha256sum '+shlex.quote(installed[0][8:]),timeout=90).split()[0]==metadata['sha256']
    requests=[]
    class Handler(http.server.SimpleHTTPRequestHandler):
        def log_message(self,*args):pass
        def do_GET(self):
            if self.path!=url.path:self.send_error(404);return
            requests.append(self.path);super().do_GET()
    server=http.server.ThreadingHTTPServer(('127.0.0.1',8794),functools.partial(Handler,directory=str(work)))
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start();stopped=False;reverse=False;restarted=False;sent=set()
    try:
        adb('reverse','tcp:8794','tcp:8794');reverse=True
        adb('shell','am force-stop '+PACKAGE);adb('shell','input keyevent KEYCODE_WAKEUP');adb('shell','wm dismiss-keyguard')
        print(adb('shell','am start -n '+ACTIVITY+' --es probe_run_id '+report['runId']),flush=True)
        deadline=time.monotonic()+1500;last=None;poll_errors=0
        while time.monotonic()<deadline:
            try:output=adb('shell','dumpsys activity '+ACTIVITY+' pocket-persistence32')
            except Exception:poll_errors+=1;time.sleep(2);continue
            rows=[line.strip().split('=',1)[1] for line in output.splitlines() if line.strip().startswith('POCKET_SESSION32=')]
            state=json.loads(base64.b64decode(rows[-1],validate=True)) if rows else {}
            if state.get('runId')!=report['runId']:time.sleep(1);continue
            current=(state.get('phase'),state.get('stage'),state.get('managerStatus'))
            if current!=last:print('PERSISTENCE32_STAGE',*current,flush=True);last=current
            stage=state.get('stage')
            if stage in ('awaiting-host-tap','awaiting-host-text'):
                target=state['hostInputTarget'];identity=target['id']
                if identity not in sent:
                    x=target['x'];y=target['y'];assert isinstance(x,int) and isinstance(y,int) and x>=0 and y>=0
                    adb('shell',f'input tap {x} {y}')
                    action={'phase':state['phase'],'stage':stage,'id':identity,'x':x,'y':y}
                    if stage=='awaiting-host-text':
                        value=state['hostText'];assert re.fullmatch(r'p[0-9a-f]{7}',value)
                        time.sleep(0.5);adb('shell','input text '+value);action['text']=value
                    report['inputs'].append(action);sent.add(identity)
            if stage=='awaiting-host-restart' and not restarted:
                checkpoint=state['checkpoint'];assert checkpoint['runId']==report['runId']
                first=inspect_cycle(checkpoint['cycle'],require_kernel_ownership=image.get('child_supervision')=='kernel-subreaper',require_external_volume=image.get('external_volume')=='rootless-v1');report['firstCycleCheck']=first
                assert first['strictPassed'],'First cycle failed strict startup or shutdown checks'
                report['checkpoint']=checkpoint;report['firstHostPid']=checkpoint['hostPid']
                server.shutdown();server.server_close();thread.join(5);stopped=True;report['imageSourceOffline']=True
                report['requestsBeforeRestart']=len(requests)
                adb('shell','am force-stop '+PACKAGE)
                print(adb('shell','am start -n '+ACTIVITY+' --es probe_run_id '+report['runId']+' --ez offline_restart true'),flush=True)
                restarted=True
            if state.get('validation'):
                report['app']=state;break
            time.sleep(1)
        else:raise TimeoutError('Persistence validation timed out; inspect the live guest before any restart')
        report['pollErrors']=poll_errors
        for name,command in [('selinux','getenforce'),('phantomLimit','device_config get activity_manager max_phantom_processes'),
                ('phantomMonitoring','settings get global settings_enable_monitor_phantom_procs')]:
            report[name+'After']=adb('shell',command);assert report[name+'After']==report[name+'Before']
        assert report['app'].get('validation')=='PASS',report['app'].get('error','Persistence app validation failed')
        assert stopped and restarted and report['app']['hostProcessRestarted'] and report['app']['persistenceVerified']
        assert report['firstHostPid']!=report['app']['hostPid']
        assert len(requests)==report['requestsBeforeRestart'],'Image was requested after its server stopped'
        cycles=report['app']['cycles'];assert len(cycles)==2
        report['cycleChecks']=[inspect_cycle(cycle,require_kernel_ownership=image.get('child_supervision')=='kernel-subreaper',require_external_volume=image.get('external_volume')=='rootless-v1') for cycle in cycles]
        assert all(check['strictPassed'] for check in report['cycleChecks']),'Strict per-boot checks failed'
        assert len(report['inputs'])==3,'Expected a button tap and text entry before restart, and a button tap afterward'
        report['strictPassed']=True;report['validation']='PASS'
    except Exception as error:report['validation']='FAIL';report['error']=str(error);raise
    finally:
        if not stopped:server.shutdown();server.server_close();thread.join(5)
        if reverse:adb('reverse','--remove','tcp:8794')
        report['requests']=requests
        (work/'manager-persistence-report.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps({key:report.get(key) for key in ('validation','strictPassed','runId','error','imageSourceOffline','firstHostPid')}),flush=True)


if __name__=='__main__':main()
