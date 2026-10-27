#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate real software codecs in the dedicated Pi Android VM's Pocket guest."""
import argparse
import base64
import io
import json
import os
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
    parser.add_argument('--install',type=Path,help='Update the Manager first; preserves guest data')
    parser.add_argument('--probe',type=Path,required=True)
    args=parser.parse_args()
    c.require_virtual()
    config=json.loads((Path.home()/'.config/pocket-pi-debug.json').read_text())
    def root(command):return c.shell('su -c '+shlex.quote(command))
    def rpc(op,argv=()):
        out,err=io.BytesIO(),io.BytesIO()
        code=request(config,op,argv,stdout=out,stderr=err)
        if code:raise RuntimeError((code,out.getvalue().decode(),err.getvalue().decode()))
        return out.getvalue().decode()
    canary=ROOT+'/system-data/local/tmp/.pocket-media-update-canary'
    value='media-update-'+str(time.time_ns())
    if args.install:
        previous=root('if [ -f '+canary+' ]; then cat '+canary+'; fi').strip()
        if previous:
            assert previous.startswith('media-update-') and previous[13:].isdigit(), 'Unexpected existing canary'
            value=previous
        else:root('printf %s '+shlex.quote(value)+' > '+canary)
        print('STOP',rpc(4),flush=True)
        deadline=time.monotonic()+60
        while time.monotonic()<deadline:
            state=json.loads(rpc(1))
            if not state['running'] and not state.get('stopping'):break
            time.sleep(1)
        else:raise RuntimeError('Guest did not stop; update not attempted')
        backups=Path.home()/'pocket-backups';backups.mkdir(mode=0o700,exist_ok=True)
        archive='/data/local/tmp/pocket-media-hostprefs-'+str(time.time_ns())+'.tar'
        root('umask 077; tar -cf '+archive+' -C /data/user/0/'+PACKAGE+' shared_prefs')
        data=base64.b64decode(root('base64 '+archive))
        backup=backups/Path(archive).name
        with os.fdopen(os.open(backup,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600),'wb') as target:
            target.write(data)
        root('rm '+archive)
        installed=c.adb('-s',c.serial(),'install','--streaming','-r',str(args.install),check=False,timeout=360)
        print(installed.stdout,flush=True)
        if installed.returncode:
            raise RuntimeError('Manager update failed: '+installed.stderr.strip())
        print(root('am start-foreground-service -n '+PACKAGE+'/.GuestService -a dev.kreijstal.aosppocket.ADMIN'),flush=True)
        print(c.shell('am start -n '+PACKAGE+'/.MainActivity --ei guest_sdk 28 --ez start_guest true'),flush=True)
    deadline=time.monotonic()+180
    while time.monotonic()<deadline:
        try:
            state=json.loads(rpc(1))
            if state['running'] and state['ready']:break
        except (OSError,ValueError):pass
        time.sleep(2)
    else:raise RuntimeError('Pocket did not become ready')
    print('READY',state,flush=True)
    if args.install:
        assert root('cat '+canary).strip()==value,'Guest data canary changed'
        root('rm '+canary)
    destination='/data/local/tmp/pocket-media-probe.jar'
    c.adb('-s',c.serial(),'push',str(args.probe),destination,check=True,timeout=60)
    physical=ROOT+'/system-data'+destination[len('/data'):]
    root('cp '+destination+' '+physical+' && chmod 0644 '+physical)
    command='export CLASSPATH='+destination+'; exec /system/bin/pocket-app-process-api28 /system/bin MediaCodecProbe'
    out,err=io.BytesIO(),io.BytesIO()
    code=request(config,2,['/system/bin/toybox','timeout','120','/system/bin/sh','-c',command],stdout=out,stderr=err)
    stdout,stderr=out.getvalue().decode(),err.getvalue().decode()
    print(stdout,flush=True)
    if code:print(stderr,flush=True)
    result={'validation':'PASS' if code==0 and 'MEDIA_CODEC_PROBE_PASS uid=19990' in stdout else 'FAIL',
            'scope':'Pocket Android 9 on dedicated Pi Android VM; logical app UID 19990',
            'status':state,'exitCode':code,'stdout':stdout,'stderr':stderr,
            'guestDataPreserved':True if args.install else 'not updated'}
    (Path.home()/'guest-media-validation.json').write_text(json.dumps(result,indent=2)+'\n')
    assert result['validation']=='PASS',result['validation']


if __name__=='__main__':main()
