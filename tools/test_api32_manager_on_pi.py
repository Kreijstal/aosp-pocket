#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Test actual API32 Manager sessions only on the dedicated VM73, without host overrides."""
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
ACTIVITY=PACKAGE+'/dev.kreijstal.aosppocket.runtime.ManagerSessionProbe32'


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--vm-root',type=Path,default=Path('/home/pi/pocket-api32-vm'))
    p.add_argument('--work-dir',type=Path);args=p.parse_args()
    base=args.vm_root.resolve();work=(args.work_dir or base/'manager-session-validation').resolve()
    assert (base/'.pocket-api32-test-vm').is_file() and work.is_relative_to(base)
    config=base/'runtime/assembly/cuttlefish_config.json'
    assert config.resolve().is_relative_to(base) and set(json.loads(config.read_text())['instances'])=={'73'}
    spec=importlib.util.spec_from_file_location('manager_vm',base/'tools/cuttlefish_arm64.py')
    vm=importlib.util.module_from_spec(spec);spec.loader.exec_module(vm)
    assert vm.BASE.resolve()==base and vm.lock()['instance']==73;vm.require_virtual()
    def adb(*argv,timeout=40):
        result=vm.adb('-s',vm.serial(),*argv,check=False,timeout=timeout)
        if result.returncode:raise RuntimeError((result.stdout+result.stderr)[-2000:])
        return result.stdout.strip()
    metadata=json.loads((work/'build.json').read_text());apk=work/'api32-session-manager-probe.apk'
    assert metadata['probe'] and metadata['guestImagesBundled']==0 and metadata['guestSdks']==[28,29,32]
    assert hashlib.file_digest(apk.open('rb'),'sha256').hexdigest()==metadata['sha256']
    with zipfile.ZipFile(apk) as archive:
        image=[v for v in json.loads(archive.read('assets/pocket-images.json'))['images'] if v['sdk']==32][0]
        assert b'Ldev/kreijstal/aosppocket/runtime/ManagerSessionProbe32;' in archive.read('classes.dex')
    url=urllib.parse.urlsplit(image['url']);filename=url.path.removeprefix('/')
    assert url.scheme=='http' and url.hostname=='127.0.0.1' and url.port==8794 and '/' not in filename
    payload=work/filename
    assert payload.stat().st_size==image['bytes'] and hashlib.file_digest(payload.open('rb'),'sha256').hexdigest()==image['sha256']
    report={'runId':uuid.uuid4().hex,'vmInstance':73,'serial':vm.serial(),'apkSha256':metadata['sha256'],'imageSha256':image['sha256']}
    report['selinuxBefore']=adb('shell','getenforce');assert report['selinuxBefore']=='Enforcing'
    report['phantomLimitBefore']=adb('shell','device_config get activity_manager max_phantom_processes')
    report['phantomMonitoringBefore']=adb('shell','settings get global settings_enable_monitor_phantom_procs')
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
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start();reverse=False;tapped=False
    try:
        adb('reverse','tcp:8794','tcp:8794');reverse=True
        adb('shell','am force-stop '+PACKAGE)
        adb('shell','input keyevent KEYCODE_WAKEUP');adb('shell','wm dismiss-keyguard')
        print(adb('shell','am start -n '+ACTIVITY+' --es probe_run_id '+report['runId']),flush=True)
        last=None;errors=0;deadline=time.monotonic()+1200
        while time.monotonic()<deadline:
            try:output=adb('shell','dumpsys activity '+ACTIVITY+' pocket-session32')
            except Exception:
                errors+=1;time.sleep(2);continue
            rows=[line.strip().split('=',1)[1] for line in output.splitlines() if line.strip().startswith('POCKET_SESSION32=')]
            state=json.loads(base64.b64decode(rows[-1],validate=True)) if rows else {}
            if state.get('runId')!=report['runId']:time.sleep(1);continue
            stage=state.get('stage');status=state.get('managerStatus');current=(stage,status)
            if current!=last:print('SESSION32_STAGE',stage,status,flush=True);last=current
            if stage=='awaiting-host-tap' and not tapped:
                target=state['hostTapTarget'];x=target['x'];y=target['y'];assert isinstance(x,int) and isinstance(y,int) and x>=0 and y>=0
                adb('shell',f'input tap {x} {y}');tapped=True;report['hostTap']={'x':x,'y':y,'runId':report['runId']}
            if state.get('validation'):
                report['app']=state;break
            time.sleep(1)
        else:raise TimeoutError('Actual Manager session did not finish; do not restart a live guest from an observation timeout')
        report['pollErrors']=errors;report['selinuxAfter']=adb('shell','getenforce')
        report['phantomLimitAfter']=adb('shell','device_config get activity_manager max_phantom_processes')
        report['phantomMonitoringAfter']=adb('shell','settings get global settings_enable_monitor_phantom_procs')
        assert report['selinuxBefore']==report['selinuxAfter']=='Enforcing'
        assert report['phantomLimitBefore']==report['phantomLimitAfter'] and report['phantomMonitoringBefore']==report['phantomMonitoringAfter']
        native=report['app'].get('nativeLog','');console=report['app'].get('console','')
        report['anrs']=re.findall(r'ANR in ([^\n]+)',native)
        report['javaFatalHeaders']=re.findall(r'FATAL EXCEPTION: ([^\n]+)',native)
        report['nativeFatals']=re.findall(r'Fatal signal [^\n]+',native)
        report['systemUiStarts']=re.findall(r'Start proc (\d+):com\.android\.systemui/',native)
        report['nativeLogWithinLimit']=len(native.encode('utf-16-le','surrogatepass'))//2<2*1024*1024
        report['initStop']=re.findall(r'API32_INIT_STOPPED reaped=(\d+) remaining=(\d+) failed=(\d+)',console)
        report['functionalPassed']=report['app'].get('functionalPassed') is True
        report['strictPassed']=bool(tapped and report['app'].get('validation')=='PASS' and report['nativeLogWithinLimit']
            and not report['anrs'] and not report['javaFatalHeaders'] and not report['nativeFatals'] and len(report['systemUiStarts'])==1
            and report['initStop'] and all(row[1:]==('0','0') for row in report['initStop']))
        report['validation']='PASS' if report['strictPassed'] else 'FAIL'
    except Exception as error:report['validation']='FAIL';report['error']=str(error);raise
    finally:
        server.shutdown();server.server_close();thread.join(5)
        if reverse:adb('reverse','--remove','tcp:8794')
        report['requests']=requests
        (work/'manager-session-report.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps({k:report.get(k) for k in ('validation','functionalPassed','strictPassed','runId','error','anrs','systemUiStarts')}),flush=True)
    if not report['strictPassed']:raise SystemExit(1)


if __name__=='__main__':main()
