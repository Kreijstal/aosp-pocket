#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Prepare/validate owner-authorized CE migration on the dedicated Pi guest."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import secrets
import sys
import time


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('mode',choices=['prepare','locked','verify']);args=p.parse_args()
    sys.path[:0]=[str(Path.home()/'lineage-cuttlefish/tools'),str(Path.home())]
    import cuttlefish_arm64 as c
    from pocketctl import request
    c.require_virtual()
    config=json.loads((Path.home()/'.config/pocket-pi-debug.json').read_text())
    password_file=Path.home()/'.config/pocket-pi-lock-test.json'
    if password_file.stat().st_mode&0o077:raise RuntimeError('Guest password file must be private')
    password=json.loads(password_file.read_text())['password']
    canary_file=Path.home()/'.config/pocket-pi-encryption-canary.json'
    if not canary_file.exists():
        if args.mode!='prepare':raise RuntimeError('Prepare the encryption data canary first')
        with os.fdopen(os.open(canary_file,os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o600),'w') as out:
            json.dump({'value':'pocket-encryption-'+secrets.token_hex(32)},out)
    value=json.loads(canary_file.read_text())['value']
    path='/data/user/0/dev.kreijstal.attestprobe/files/.pocket-encryption-canary'
    def rpc(argv,source=None):
        out,err=io.BytesIO(),io.BytesIO();rc=request(config,2,argv,source=source,stdout=out,stderr=err)
        return rc,out.getvalue().decode(),err.getvalue().decode()
    out,err=io.BytesIO(),io.BytesIO()
    rc=request(config,1,[],stdout=out,stderr=err)
    state=json.loads(out.getvalue())
    if rc or not state.get('ready') or not state.get('running') or state.get('sdk')!=28:
        raise RuntimeError('Start the dedicated Android 9 guest and wait until it is ready')
    def helper(mode,credential):
        command='export CLASSPATH=/system/framework/pocket-ce-key.jar; exec /system/bin/pocket-app-process-api28 /system/bin GuestCeKey '+mode
        rc,out,err=rpc(['/system/bin/sh','-c',command],io.BytesIO((credential+'\n').encode()))
        return rc,out.replace(credential,'<redacted>'),err.replace(credential,'<redacted>')
    report={'mode':args.mode,'canarySha256':hashlib.sha256(value.encode()).hexdigest()}
    if args.mode=='prepare':
        rc,_,err=rpc(['/system/bin/sh','-c','umask 077; /system/bin/toybox cat > '+path],io.BytesIO(value.encode()))
        if rc:raise RuntimeError('Cannot create guest data canary: '+err)
        rc,out,err=helper('prepare',password);report.update(exitCode=rc,stdout=out,stderr=err)
        if rc or '"prepared":true' not in out:raise RuntimeError('Guest CE preparation failed: '+out+err)
        report['keyPreparedInOwnerMemory']=True
    elif args.mode=='locked':
        rc,out,_=rpc(['/system/bin/toybox','cat',path])
        report.update(beforeUnlockCatExitCode=rc,beforeUnlockStdoutBytes=len(out.encode()),beforeUnlockCanaryReadable=value in out)
        if rc==0 or value in out:
            report['validation']='FAIL'
            (Path.home()/'guest-encryption-locked-validation.json').write_text(json.dumps(report,indent=2)+'\n')
            raise AssertionError('Private file open succeeded before credential unlock; diagnostic metadata saved without file contents')
        rc,out,err=helper('inspect',password+'x')
        assert rc!=0 and 'SecurityException: Guest credential rejected' in out+err,'Wrong-password test did not reach credential rejection: '+out+err
        report.update(wrongPasswordRejected=True,wrongPasswordOutput=out+err)
        rc,out,_=rpc(['/system/bin/toybox','cat',path]);assert rc!=0 and value not in out,'Wrong password unlocked private storage'
        report['privateStorageUnavailableBeforeUnlock']=True
    else:
        rc,out,err=helper('inspect',password)
        if rc:raise RuntimeError('Guest credential verification failed: '+out+err)
        # LockSettings delivers the user-key unlock asynchronously after a
        # successful credential check. Wait for that transition, not a screen
        # unlock, and never accept mismatching readable bytes.
        start=time.monotonic();deadline=start+15
        while True:
            rc,data,err=rpc(['/system/bin/toybox','cat',path])
            if rc==0:
                assert data==value,'Encrypted data canary bytes changed'
                break
            if time.monotonic()>=deadline:raise AssertionError('CE storage did not become readable after credential unlock')
            time.sleep(.25)
        report['unlockWaitMilliseconds']=round((time.monotonic()-start)*1000)
        report.update(correctPasswordVerified=True,encryptedCanaryPreserved=True)
    report['validation']='PASS';output=Path.home()/('guest-encryption-'+args.mode+'-validation.json')
    output.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))


if __name__=='__main__':main()
