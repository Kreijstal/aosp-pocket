#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Measure real guest LatinIME digit taps through to the app's editable text."""
import argparse
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shlex
import sys
import time

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--label',default='baseline')
    parser.add_argument('--install',action='store_true')
    args=parser.parse_args()
    if not args.label.replace('-','').isalnum():parser.error('Unsafe report label')
    sys.path.insert(0,str(Path.home()/'lineage-cuttlefish/tools'));import cuttlefish_arm64 as c
    c.require_virtual();stage=Path.home()/'.config/pocketctl/input-latency'
    spec=importlib.util.spec_from_file_location('pc',Path.home()/'pocketctl.py')
    pc=importlib.util.module_from_spec(spec);spec.loader.exec_module(pc);connect=pc.connect
    def bounded(cfg):
        peer=connect(cfg);peer.settimeout(200);return peer
    pc.connect=bounded;cfg=json.loads((Path.home()/'.config/pocket-pi-debug.json').read_text())
    def rpc(argv,source=None):
        out,err=io.BytesIO(),io.BytesIO();rc=pc.request(cfg,2,argv,source=source,stdout=out,stderr=err)
        if rc:raise RuntimeError((rc,out.getvalue().decode(),err.getvalue().decode()))
        return out.getvalue().decode()
    def shell(command,**kwargs):return rpc(['/system/bin/sh','-c',command],**kwargs)
    def upload(name):
        target='/data/local/tmp/'+name
        with (stage/name).open('rb') as stream:shell('cat > '+shlex.quote(target+'.part'),source=stream)
        actual=rpc(['/system/bin/toybox','sha256sum',target+'.part']).split()[0]
        with (stage/name).open('rb') as stream:assert actual==hashlib.file_digest(stream,'sha256').hexdigest()
        shell('chmod 0644 '+shlex.quote(target+'.part')+' && mv '+shlex.quote(target+'.part')+' '+shlex.quote(target))
    def owner(*values):
        return shell('export CLASSPATH=/system/framework/pocket-zygote.jar:/data/local/tmp/input-latency-owner.jar; exec /system/bin/pocket-app-process-api28 /system/bin MicrogOwner '+' '.join(map(shlex.quote,values)))
    command='export CLASSPATH=/data/local/tmp/input-latency-lock.jar; exec /system/bin/pocket-app-process-api28 /system/bin GuestLockProbe '
    status=shell(command+'status')
    lock=json.loads(next(line[len('GUEST_LOCK_RESULT '):] for line in status.splitlines() if line.startswith('GUEST_LOCK_RESULT ')))
    if lock['deviceSecure'] and lock['keyguardLocked']:
        credential=Path.home()/'.config/pocket-pi-lock-test.json';assert credential.stat().st_mode&0o077==0
        secret=json.loads(credential.read_text());assert secret['scope']=='Pocket Android 9 guest on dedicated Pi VM'
        assert '"uiPasswordUnlockVerified":true' in shell(command+'unlock',source=io.BytesIO((secret['password']+'\n').encode()))
        print('EXISTING_PASSWORD_UI_UNLOCK_PASS',flush=True)
    upload('typing-probe.jar')
    if args.install:
        upload('typing-test.apk');print(owner('--install','/data/local/tmp/typing-test.apk'),flush=True)
        time.sleep(3)
    path='/data/user/0/dev.kreijstal.pocketinputlatency/files/'
    shell('rm -f '+path+'typing-change.json '+path+'typing-draw.json')
    print(owner('--force-stop','dev.kreijstal.pocketinputlatency'),flush=True)
    print(owner('--launch','dev.kreijstal.pocketinputlatency/.TypingActivity'),flush=True)
    output=shell('export CLASSPATH=/system/framework/pocket-zygote.jar:/data/local/tmp/typing-probe.jar; exec /system/bin/pocket-app-process-api28 /system/bin TypingProbe --direct')
    print(output,flush=True)
    for line in output.splitlines():
        if line.startswith('TYPING_RESULT '):
            report=json.loads(line[len('TYPING_RESULT '):]);break
    else:raise RuntimeError('Typing probe returned no result')
    report['label']=args.label;report['scope']='Pi VM -> Pocket Android 9 -> real LatinIME numeric keyboard'
    (stage/('typing-'+args.label+'.json')).write_text(json.dumps(report,indent=2)+'\n')
    assert report['validation']=='PASS';print('PASS: actual numeric keyboard text-entry timing recorded',flush=True)

if __name__=='__main__':main()
