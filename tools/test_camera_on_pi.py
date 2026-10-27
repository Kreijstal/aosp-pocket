#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate guest cameras on the dedicated Pi Android VM.

NDK Camera2 in the guest -> cameraserver -> Pocket camera provider (patched external camera HAL)
-> host broker -> host Camera2 YUV stream. Preview frames and one JPEG still must arrive.
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
    parser.add_argument('--install',type=Path,help='Update the Manager first; the guest is stopped for it')
    parser.add_argument('--probe',type=Path,required=True,help='runtime/tests/camera_probe.c, built for the guest')
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
    def save():(Path.home()/'camera-validation.json').write_text(json.dumps(result,indent=2)+'\n')
    # The provider lists host cameras when cameraserver starts, so the host permission comes first.
    root('pm grant '+PACKAGE+' android.permission.CAMERA')
    if args.install:
        state=json.loads(rpc(1))
        if state['running']:
            rpc(4);deadline=time.monotonic()+90
            while time.monotonic()<deadline:
                state=json.loads(rpc(1))
                if not state['running'] and not state.get('stopping'):break
                time.sleep(2)
        assert not state['running'] and not state.get('stopping'),'Guest must be stopped before the update'
        installed=c.adb('-s',c.serial(),'install','--streaming','-r',str(args.install),check=False,timeout=360)
        result['install']=installed.stdout.strip();save()
        if installed.returncode:raise RuntimeError('Manager update failed: '+installed.stderr.strip())
        root('pm grant '+PACKAGE+' android.permission.CAMERA')
        root('am start-foreground-service -n '+PACKAGE+'/.GuestService -a dev.kreijstal.aosppocket.ADMIN')
        c.shell('am start -n '+PACKAGE+'/.MainActivity --ei guest_sdk 28 --ez start_guest true')
    deadline=time.monotonic()+300
    while time.monotonic()<deadline:
        try:
            state=json.loads(rpc(1))
            if state['running'] and state['ready'] and rpc(2,['/system/bin/getprop','sys.boot_completed']).strip()=='1':break
        except (OSError,ValueError,RuntimeError):pass
        time.sleep(3)
    else:raise RuntimeError('Guest did not complete boot')
    result['version']=c.shell('dumpsys package '+PACKAGE+' | grep versionName').strip()
    destination='/data/local/tmp/pocket-camera-probe'
    c.adb('-s',c.serial(),'push',str(args.probe),destination,check=True,timeout=60)
    physical=ROOT+'/system-data'+destination[len('/data'):]
    root('cp '+destination+' '+physical+' && chmod 0755 '+physical)
    # Host evidence: the host camera service's client log records each open by the Manager.
    def host_opens():return [l.strip() for l in c.shell('dumpsys media.camera').splitlines() if 'CONNECT' in l and PACKAGE in l]
    before=set(host_opens())
    def markers():return rpc(2,['/system/bin/toybox','grep','-E','^POCKET_CAMERA','/logs/manager-init.log']).splitlines()
    earlier=len(markers())
    out,err=io.BytesIO(),io.BytesIO()
    code=request(config,2,['/system/bin/toybox','timeout','60',destination,'3'],stdout=out,stderr=err)
    result.update(exitCode=code,stdout=out.getvalue().decode(),stderr=err.getvalue().decode()[-2000:])
    result['host_camera_opens']=[l for l in host_opens() if l not in before]
    result['guest_markers']='\n'.join(markers()[earlier:])
    result['guest_log']=rpc(2,['/system/bin/sh','-c','/system/bin/toybox grep -E "CameraService|CameraProviderManager|ExtCam|Camera3-|cameraserver" '
                               '/logs/manager-init.log | /system/bin/toybox tail -n 40'])
    # Closing must be clean too: a session reconfigures while the previous host stream is still releasing.
    ok=code==0 and 'CAMERA_PROBE_PASS' in result['stdout'] and 'POCKET_CAMERA_HOST_STREAM' in result['guest_markers'] and \
        'device_error' not in result['stdout'] and 'POCKET_CAMERA_HOST_UNAVAILABLE' not in result['guest_markers']
    result['validation']='PASS' if ok and result['host_camera_opens'] else 'FAIL'
    save();print(json.dumps(result,indent=2))
    assert result['validation']=='PASS',result['validation']

if __name__=='__main__':main()
