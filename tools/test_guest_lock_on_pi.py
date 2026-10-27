#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Enroll/verify an owner-authorized test password in the dedicated Pi guest."""
import argparse
import io
import json
import os
from pathlib import Path
import secrets
import shlex
import sys

sys.path[:0]=['/home/pi/lineage-cuttlefish/tools','/home/pi']
import cuttlefish_arm64 as c
from pocketctl import request

ROOT='/data/user/0/dev.kreijstal.aosppocket.runtime/files/api28-native-rootfs'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode',choices=['setup','verify','unlock'])
    parser.add_argument('--probe',type=Path,required=True)
    args=parser.parse_args()
    c.require_virtual()
    config=json.loads((Path.home()/'.config/pocket-pi-debug.json').read_text())
    credential=Path.home()/'.config/pocket-pi-lock-test.json'
    if not credential.exists():
        assert args.mode=='setup','No saved test password for verification'
        with os.fdopen(os.open(credential,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600),'w') as output:
            json.dump({'scope':'Pocket Android 9 guest on dedicated Pi VM','password':'Pocket-'+secrets.token_urlsafe(12)},output)
            output.write('\n')
    assert credential.stat().st_mode&0o077==0,'Test credential file must be private'
    secret=json.loads(credential.read_text())
    assert secret['scope']=='Pocket Android 9 guest on dedicated Pi VM'
    password=secret['password']
    def rpc(op,argv=(),source=None):
        out,err=io.BytesIO(),io.BytesIO()
        code=request(config,op,argv,source=source,stdout=out,stderr=err)
        return code,out.getvalue().decode(),err.getvalue().decode()
    code,status,_=rpc(1);state=json.loads(status)
    assert code==0 and state['ready'] and state['running'] and state['sdk']==28,state
    destination='/data/local/tmp/guest-lock-probe.jar'
    c.adb('-s',c.serial(),'push',str(args.probe),destination,check=True)
    physical=ROOT+'/system-data'+destination[len('/data'):]
    c.shell('su -c '+shlex.quote('cp '+destination+' '+physical+' && chmod 0644 '+physical))
    command='export CLASSPATH='+destination+'; exec /system/bin/pocket-app-process-api28 /system/bin GuestLockProbe '+args.mode
    code,stdout,stderr=rpc(2,['/system/bin/toybox','timeout','150','/system/bin/sh','-c',command],io.BytesIO((password+'\n').encode()))
    stdout=stdout.replace(password,'<redacted>');stderr=stderr.replace(password,'<redacted>')
    print(stdout,flush=True)
    result={'scope':secret['scope'],'mode':args.mode,'exitCode':code,'stdout':stdout,'stderr':stderr}
    for line in stdout.splitlines():
        if line.startswith('GUEST_LOCK_RESULT '):result.update(json.loads(line[len('GUEST_LOCK_RESULT '):]))
    (Path.home()/('guest-lock-'+args.mode+'-validation.json')).write_text(json.dumps(result,indent=2)+'\n')
    if code or result.get('validation')!='PASS':
        print(stderr,flush=True)
        raise RuntimeError('Guest lock validation failed; test credential retained privately for recovery')
    print('PASS: guest password '+args.mode+'; credential stored privately on Pi',flush=True)


if __name__=='__main__':main()
