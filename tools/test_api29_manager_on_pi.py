#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate image download, Manager display/input and offline Android 10 restart."""
import argparse
import hashlib
import http.server
import json
from pathlib import Path
import shlex
import sys
import threading
import time
sys.path.insert(0,'/home/pi/lineage-cuttlefish/tools')
import cuttlefish_arm64 as c
from test_api29_on_pi import vm_network_mode


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--apk',type=Path,required=True)
    p.add_argument('--image',type=Path,required=True)
    p.add_argument('--timeout',type=int,default=1500)
    args=p.parse_args();c.require_virtual()
    if not 60<=args.timeout<=1800:p.error('Timeout must be 60..1800 seconds')
    package='dev.kreijstal.aosppocket.api29manager';files='/data/user/0/'+package+'/files/'
    def adb(*argv,timeout=30,check=True):
        result=c.adb('-s',c.serial(),*argv,timeout=timeout,check=False)
        if check and result.returncode:raise RuntimeError((result.stdout+result.stderr)[-2500:])
        return result
    def owned(command):return adb('shell','su -c '+shlex.quote(command),check=False).stdout
    expected='/'+args.image.name;requests=[]
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path!=expected:self.send_error(404);return
            requests.append(self.path);self.send_response(200);self.send_header('Content-Length',str(args.image.stat().st_size));self.end_headers()
            with args.image.open('rb') as image:
                for data in iter(lambda:image.read(1024*1024),b''):self.wfile.write(data)
        def log_message(self,*args):pass
    server=http.server.ThreadingHTTPServer(('127.0.0.1',38080),Handler)
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start();online=True
    try:
        with vm_network_mode(True) as environment:
            assert not owned('test -f '+files+'api29-manager-result.json && echo exists').strip(),'Previous Manager result exists; inspect before rerunning'
            adb('reverse','tcp:38080','tcp:38080')
            print(adb('install','--no-incremental','-r',str(args.apk),timeout=120).stdout,flush=True)
            adb('shell','pm grant '+package+' android.permission.POST_NOTIFICATIONS')
            adb('shell','input keyevent KEYCODE_WAKEUP')
            adb('shell','wm dismiss-keyguard')
            adb('shell','am force-stop '+package)
            print(adb('shell','am start -n '+package+'/dev.kreijstal.aosppocket.runtime.ManagerProbe').stdout,flush=True)
            deadline=time.monotonic()+args.timeout;next_status=0
            while time.monotonic()<deadline:
                raw=owned('cat '+files+'api29-manager-result.json')
                if raw.strip():report=json.loads(raw);break
                if online and owned('test -f '+files+'api29-offline-ready && echo ready').strip()=='ready':
                    server.shutdown();server.server_close();thread.join(3);online=False
                    adb('reverse','--remove','tcp:38080')
                    owned('touch '+files+'api29-offline-go')
                    print('API29_MANAGER_OFFLINE_RESTART image_server=stopped',flush=True)
                if time.monotonic()>=next_status:
                    print('API29_MANAGER_STAGE',owned('cat '+files+'api29-manager-stage.txt').strip()[:250],flush=True);next_status=time.monotonic()+30
                time.sleep(2)
            else:raise RuntimeError('API29 Manager validation timed out; private guest data retained for diagnosis')
            report.update(apkSha256=hashlib.file_digest(args.apk.open('rb'),'sha256').hexdigest(),
                imageSha256=hashlib.file_digest(args.image.open('rb'),'sha256').hexdigest(),imageHttpRequests=len(requests),hostNetworkEnvironment=environment)
            if report.get('validation')=='PASS':
                assert not online and len(requests)==1,'Offline boot unexpectedly required another image download'
            destination=Path.home()/'api29-manager-validation.json';destination.write_text(json.dumps(report,indent=2)+'\n')
            display={key:value for key,value in report.items() if key!='log'}
            print(json.dumps(display,indent=2),flush=True)
            assert report.get('validation')=='PASS','Manager test failed; full guest log saved in '+str(destination)
    finally:
        adb('reverse','--remove','tcp:38080',check=False)
        if online:server.shutdown();server.server_close();thread.join(3)


if __name__=='__main__':main()
