#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Download/install the actual image in an isolated Pi-only Android test app."""
import argparse
import functools
import http.server
import json
import os
from pathlib import Path
import shlex
import sys
import threading
import time


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apk',type=Path,default=Path.home()/'pocket-image-install-probe.apk')
    parser.add_argument('--image',type=Path,default=Path.home()/'pocket-api28-arm64-0.75.0.zip')
    parser.add_argument('--timeout',type=int,default=900)
    args=parser.parse_args()
    if not 30<=args.timeout<=3600:parser.error('--timeout must be between 30 and 3600 seconds')
    sys.path.insert(0,str(Path.home()/'lineage-cuttlefish/tools'))
    import cuttlefish_arm64 as c
    c.require_virtual()
    stage=Path.home()/'pocket-image-download-test';stage.mkdir(exist_ok=True);stage.chmod(0o700)
    target=stage/args.image.name
    if target.exists() and not os.path.samefile(target,args.image):target.unlink()
    if not target.exists():os.link(args.image,target)
    assert os.path.samefile(target,args.image),'Unexpected existing test image'
    class Handler(http.server.SimpleHTTPRequestHandler):
        def list_directory(self,path):self.send_error(403);return None
        def log_message(self,*args):pass
    server=http.server.ThreadingHTTPServer(('127.0.0.1',38080),functools.partial(Handler,directory=str(stage)))
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    package='dev.kreijstal.pocketimagetest';result='/data/user/0/'+package+'/files/result.json'
    try:
        c.adb('-s',c.serial(),'reverse','tcp:38080','tcp:38080')
        c.adb('-s',c.serial(),'install','--no-incremental','-r',str(args.apk),timeout=90)
        c.shell('am force-stop '+package)
        assert c.adb('-s',c.serial(),'shell','-T','su -c '+shlex.quote('test -e '+result),check=False).returncode!=0,'Prior probe data exists; inspect before rerunning'
        c.shell('am start -n '+package+'/dev.kreijstal.aosppocket.runtime.ImageInstallProbe')
        deadline=time.monotonic()+args.timeout;attempt=0
        while time.monotonic()<deadline:
            response=c.adb('-s',c.serial(),'shell','-T','su -c '+shlex.quote('cat '+result),check=False)
            if response.returncode==0 and response.stdout.strip():report=json.loads(response.stdout);break
            if attempt%15==0:print('Image download/install running in isolated test app…',flush=True)
            attempt+=1
            time.sleep(2)
        else:raise RuntimeError('Image install probe timed out')
        (Path.home()/'pocket-image-download-validation.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps(report,indent=2),flush=True);assert report['validation']=='PASS',report
        c.adb('-s',c.serial(),'uninstall',package)
        print('PASS: fresh download, file verification, offline boot preparation and legacy migration; probe data removed',flush=True)
    finally:
        c.adb('-s',c.serial(),'reverse','--remove','tcp:38080',check=False)
        server.shutdown();server.server_close();thread.join(3)


if __name__=='__main__':main()
