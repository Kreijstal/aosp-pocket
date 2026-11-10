#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate on-demand 12L image preparation only in the dedicated VM73."""
import argparse
import base64
import functools
import hashlib
import http.server
import importlib.util
import json
from pathlib import Path
import shlex
import threading
import time
import uuid

PACKAGE='dev.kreijstal.aosppocket.api32managerimage'
ACTIVITY=PACKAGE+'/dev.kreijstal.aosppocket.runtime.ImageProbe32'


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--vm-root',type=Path,default=Path('/home/pi/pocket-api32-vm'))
    p.add_argument('--work-dir',type=Path)
    args=p.parse_args();base=args.vm_root.resolve();work=(args.work_dir or base/'manager-image-validation').resolve()
    assert (base/'.pocket-api32-test-vm').is_file() and work.is_relative_to(base)
    config=base/'runtime/assembly/cuttlefish_config.json'
    assert config.resolve().is_relative_to(base) and set(json.loads(config.read_text())['instances'])=={'73'}
    spec=importlib.util.spec_from_file_location('api32_image_vm',base/'tools/cuttlefish_arm64.py')
    vm=importlib.util.module_from_spec(spec);spec.loader.exec_module(vm)
    assert vm.BASE.resolve()==base and vm.lock()['instance']==73
    vm.require_virtual()
    def adb(*arguments,timeout=30):
        result=vm.adb('-s',vm.serial(),*arguments,check=False,timeout=timeout)
        if result.returncode:raise RuntimeError((result.stdout+result.stderr)[-2000:])
        return result.stdout.strip()
    metadata=json.loads((work/'build.json').read_text());apk=work/'api32-image-manager-probe.apk'
    assert metadata['probe'] and metadata['guestImagesBundled']==0 and metadata['guestSdks']==[28,29,32]
    assert hashlib.file_digest(apk.open('rb'),'sha256').hexdigest()==metadata['sha256']
    import zipfile
    with zipfile.ZipFile(apk) as source:
        image=[v for v in json.loads(source.read('assets/pocket-images.json'))['images'] if v['sdk']==32][0]
    assert image['url']=='http://127.0.0.1:8794/pocket-api32-arm64-0.78.0.zip'
    payload=work/'pocket-api32-arm64-0.78.0.zip'
    assert payload.stat().st_size==image['bytes'] and hashlib.file_digest(payload.open('rb'),'sha256').hexdigest()==image['sha256']
    report={'runId':uuid.uuid4().hex,'vmInstance':73,'serial':vm.serial(),'managerSha256':metadata['sha256'],
            'imageSha256':image['sha256'],'guestBootVerified':False,'managerSessionVerified':False}
    report['selinuxBefore']=adb('shell','getenforce');assert report['selinuxBefore']=='Enforcing'
    installed=vm.adb('-s',vm.serial(),'shell','pm path '+PACKAGE,check=False,timeout=30)
    assert installed.returncode==0 or installed.returncode==1 and not (installed.stdout+installed.stderr).strip()
    report['freshAppInstall']=not installed.stdout.strip()
    print(adb('install','--no-incremental','-r',str(apk),timeout=180),flush=True)
    assert 'DEBUGGABLE' not in adb('shell','dumpsys package '+PACKAGE)
    paths=adb('shell','pm path '+PACKAGE).splitlines();assert len(paths)==1 and paths[0].startswith('package:/data/app/')
    assert adb('shell','sha256sum '+shlex.quote(paths[0][8:]),timeout=90).split()[0]==metadata['sha256']
    requests=[]
    class Handler(http.server.SimpleHTTPRequestHandler):
        def log_message(self,format,*values):pass
        def do_GET(self):
            assert self.path=='/pocket-api32-arm64-0.78.0.zip'
            requests.append({'path':self.path,'range':self.headers.get('Range')});super().do_GET()
    server=http.server.ThreadingHTTPServer(('127.0.0.1',8794),functools.partial(Handler,directory=str(work)))
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start();stopped=False;reverse=False
    try:
        adb('reverse','tcp:8794','tcp:8794');reverse=True
        adb('shell','am force-stop '+PACKAGE)
        print(adb('shell','am start -n '+ACTIVITY+' --es probe_run_id '+report['runId']),flush=True)
        deadline=time.monotonic()+600;last=None
        while time.monotonic()<deadline:
            output=adb('shell','dumpsys activity '+ACTIVITY+' pocket-image32',timeout=40)
            rows=[line.strip().split('=',1)[1] for line in output.splitlines() if line.strip().startswith('POCKET_IMAGE32=')]
            state=json.loads(base64.b64decode(rows[-1],validate=True)) if rows else {}
            if state.get('runId')!=report['runId']:time.sleep(1);continue
            stage=state.get('stage')
            if stage!=last:print('IMAGE32_STAGE',stage,flush=True);last=stage
            if stage=='awaiting-offline' and not stopped:
                if report['freshAppInstall']:assert requests,'Fresh on-demand installation did not request the image'
                server.shutdown();server.server_close();thread.join(5);stopped=True
                report['imageServerStoppedBeforeSecondPreparation']=True
                print(adb('shell','am start --activity-single-top -n '+ACTIVITY+' --es probe_run_id '+report['runId']+' --ez continue_offline true'),flush=True)
            if state.get('validation'):
                # The app publishes the final state again after supervised cleanup.
                if stage in ('finished','failed'):
                    time.sleep(1)
                    again=adb('shell','dumpsys activity '+ACTIVITY+' pocket-image32')
                    rows=[line.strip().split('=',1)[1] for line in again.splitlines() if line.strip().startswith('POCKET_IMAGE32=')]
                    state=json.loads(base64.b64decode(rows[-1],validate=True));report['app']=state;break
            time.sleep(1)
        else:raise TimeoutError('Image download/preparation did not finish')
        report['selinuxAfter']=adb('shell','getenforce');assert report['selinuxAfter']==report['selinuxBefore']
        state=report['app'];assert state['runId']==report['runId'] and state['validation']=='PASS',state
        assert stopped and state['imageId']==image['sha256'] and state['hostUid']>=10000
        assert all(state[k] is True for k in ('offlinePreparation','dataPreserved','relocatedElfExecuted','nativeGetprop'))
        report['validation']='PASS'
    except Exception as error:
        report['validation']='FAIL';report['error']=str(error);raise
    finally:
        if not stopped:server.shutdown();server.server_close();thread.join(5)
        if reverse:adb('reverse','--remove','tcp:8794')
        report['requests']=requests
        report['downloadRequestedThisRun']=bool(requests)
        (work/'image-preparation-report.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps({key:report.get(key) for key in ('validation','runId','error','imageSha256')}),flush=True)


if __name__=='__main__':main()
