#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate guest screen capture (MediaProjection) on the dedicated Pi Android VM.

A projection grant from the guest's MediaProjectionManagerService, a virtual display mirroring
the guest screen (pixels checked through an ImageReader), and a few seconds recorded with the
guest's H.264 encoder into an MP4. The consent dialog is not exercised: the probe runs as the
system uid, as system screen recorders do.
"""
import argparse
import io
import json
from pathlib import Path
import shlex
import sys
import time

sys.path[:0]=['/home/pi/lineage-cuttlefish/tools','/home/pi']
import cuttlefish_arm64 as c
from pocketctl import request

PACKAGE='dev.kreijstal.aosppocket.runtime'
ROOT='/data/user/0/'+PACKAGE+'/files/api28-native-rootfs'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True,help='runtime/tests/ProjectionProbe.java as a dex jar')
    args=parser.parse_args()
    c.require_virtual()
    config=json.loads((Path.home()/'.config/pocket-pi-debug.json').read_text())
    def root(command):return c.shell('su -c '+shlex.quote(command))
    def rpc(op,argv=()):
        out,err=io.BytesIO(),io.BytesIO()
        code=request(config,op,argv,stdout=out,stderr=err)
        if code:raise RuntimeError((code,out.getvalue().decode(),err.getvalue().decode()))
        return out.getvalue().decode()
    result={'scope':'Pocket Android 9 on dedicated Pi Android VM'}
    def save():(Path.home()/'projection-validation.json').write_text(json.dumps(result,indent=2)+'\n')
    deadline=time.monotonic()+300
    while time.monotonic()<deadline:
        try:
            state=json.loads(rpc(1))
            if state['running'] and state['ready'] and rpc(2,['/system/bin/getprop','sys.boot_completed']).strip()=='1':break
        except (OSError,ValueError,RuntimeError):pass
        time.sleep(3)
    else:raise RuntimeError('Guest did not complete boot')
    result['version']=c.shell('dumpsys package '+PACKAGE+' | grep versionName').strip()
    destination='/data/local/tmp/pocket-projection-probe.jar'
    c.adb('-s',c.serial(),'push',str(args.probe),destination,check=True,timeout=60)
    physical=ROOT+'/system-data'+destination[len('/data'):]
    root('cp '+destination+' '+physical+' && chmod 0644 '+physical)
    command='export CLASSPATH='+destination+'; exec /system/bin/pocket-app-process-api28 /system/bin ProjectionProbe 3'
    out,err=io.BytesIO(),io.BytesIO()
    code=request(config,2,['/system/bin/toybox','timeout','90','/system/bin/sh','-c',command],stdout=out,stderr=err)
    result.update(exitCode=code,stdout=out.getvalue().decode(),stderr=err.getvalue().decode()[-3000:])
    result['guest_log']=rpc(2,['/system/bin/sh','-c','/system/bin/toybox grep -E "MediaProjection|VirtualDisplay|DisplayManagerService|SurfaceFlinger.*[Vv]irtual|OMX.google.h264.encoder|SoftAVCEnc|MPEG4Writer" '
                               '/logs/manager-init.log | /system/bin/toybox tail -n 30'])
    # Keep the recording for inspection next to the result.
    recording=ROOT+'/system-data/local/tmp/pocket-projection.mp4'
    root('cp '+recording+' /data/local/tmp/pocket-projection.mp4 && chmod 0644 /data/local/tmp/pocket-projection.mp4')
    pulled=c.adb('-s',c.serial(),'pull','/data/local/tmp/pocket-projection.mp4',str(Path.home()/'pocket-projection.mp4'),check=False,timeout=60)
    result['recording']=str(Path.home()/'pocket-projection.mp4') if pulled.returncode==0 else None
    result['validation']='PASS' if code==0 and 'PROJECTION_PROBE_PASS' in result['stdout'] else 'FAIL'
    save();print(json.dumps(result,indent=2))
    assert result['validation']=='PASS',result['validation']

if __name__=='__main__':main()
