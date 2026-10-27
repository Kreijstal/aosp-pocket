#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Measure and apply the narrow async-input update in Termux or the Pi VM."""
import argparse
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shlex
import sys
import time

ROOT='/data/user/0/dev.kreijstal.aosppocket.runtime/files/api28-native-rootfs'
BASE='50e27e92b3213a596587318d7540f962ed67cbdc00f3ae026af6593801449127'
PATCHED='427570550f0f9cd80d4bc7bcf5fbb29625bec356d4aa909b5ace875a806b698a'

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pi',action='store_true',help='Use the dedicated Pi Android VM and its existing owner pairing')
    args=parser.parse_args()
    private=Path.home()/'.config/pocketctl';stage=private/'input-latency'
    if args.pi:
        sys.path.insert(0,str(Path.home()/'lineage-cuttlefish/tools'))
        import cuttlefish_arm64 as c
        c.require_virtual()
    spec=importlib.util.spec_from_file_location('pc',Path.home()/'pocketctl.py' if args.pi else private/'microg-install/pocketctl.py')
    pc=importlib.util.module_from_spec(spec);spec.loader.exec_module(pc);connect=pc.connect
    def bounded(config):
        peer=connect(config);peer.settimeout(110);return peer
    pc.connect=bounded;config=json.loads((Path.home()/'.config/pocket-pi-debug.json' if args.pi else private/'config.json').read_text())
    report={'scope':('Pi VM' if args.pi else 'Samsung')+' Pocket Android 9 real input socket','validation':'IN_PROGRESS'}
    def save():
        (stage/'validation.json').write_text(json.dumps(report,indent=2)+'\n')
    def rpc(argv,source=None,check=True):
        out,err=io.BytesIO(),io.BytesIO();rc=pc.request(config,2,argv,source=source,stdout=out,stderr=err)
        if check and rc:raise RuntimeError((rc,out.getvalue().decode(),err.getvalue().decode()))
        return rc,out.getvalue().decode()
    def shell(command,**kwargs):return rpc(['/system/bin/sh','-c',command],**kwargs)[1]
    def digest(path):return rpc(['/system/bin/toybox','sha256sum',path])[1].split()[0]
    def upload(source,target):
        temporary=target+'.input-update'
        with source.open('rb') as stream:shell('exec /system/bin/toybox cat > '+shlex.quote(temporary),source=stream)
        assert digest(temporary)==hashlib.sha256(source.read_bytes()).hexdigest()
        shell('chmod 0644 '+shlex.quote(temporary)+' && mv '+shlex.quote(temporary)+' '+shlex.quote(target))
    def owner(*args):
        return shell('export CLASSPATH=/system/framework/pocket-zygote.jar:/data/local/tmp/input-latency-owner.jar; exec /system/bin/pocket-app-process-api28 /system/bin MicrogOwner '+' '.join(map(shlex.quote,args)))
    def control_pid():
        rows=rpc(['/system/bin/toybox','ps','-A','-o','PID,ARGS'])[1].splitlines()
        ids=[int(row.split()[0]) for row in rows if 'dev.kreijstal.aosppocket.guest.GuestControl' in row]
        return ids[0] if len(ids)==1 else None
    def measure(asynchronous):
        command='export CLASSPATH=/system/framework/pocket-zygote.jar:/data/local/tmp/input-latency-probe.jar; exec /system/bin/pocket-app-process-api28 /system/bin InputLatencyProbe'
        if asynchronous:command+=' --expect-async'
        output=shell(command)
        for line in output.splitlines():
            if line.startswith('INPUT_LATENCY_RESULT '):
                result=json.loads(line[len('INPUT_LATENCY_RESULT '):]);assert result['validation']=='PASS';return result
        raise RuntimeError('No input latency result: '+output)
    try:
        current=digest('/system/framework/pocket-zygote.jar');assert current in (BASE,PATCHED),'Guest JAR differs; refusing to overwrite another build'
        assert hashlib.sha256((stage/'pocket-zygote.jar').read_bytes()).hexdigest()==PATCHED
        for name in ['input-latency-test.apk','input-latency-probe.jar','input-latency-owner.jar','input-latency-lock.jar']:
            upload(stage/name,'/data/local/tmp/'+name)
        lock_command='export CLASSPATH=/data/local/tmp/input-latency-lock.jar; exec /system/bin/pocket-app-process-api28 /system/bin GuestLockProbe '
        status=shell(lock_command+'status')
        lock_state=json.loads(next(line[len('GUEST_LOCK_RESULT '):] for line in status.splitlines() if line.startswith('GUEST_LOCK_RESULT ')))
        if lock_state['deviceSecure'] and lock_state['keyguardLocked']:
            credential=Path.home()/'.config/pocket-pi-lock-test.json' if args.pi else private/'guest-lock-credential.json'
            assert credential.stat().st_mode&0o077==0
            secret=json.loads(credential.read_text())
            expected='Pocket Android 9 guest on dedicated Pi VM' if args.pi else 'Pocket Android 9 guest on Samsung SM-G780G'
            assert secret['scope']==expected
            unlock=shell(lock_command+'unlock',source=io.BytesIO((secret['password']+'\n').encode()))
            assert '"uiPasswordUnlockVerified":true' in unlock
            report['existingPasswordUiUnlock']='PASS';print('EXISTING_PASSWORD_UI_UNLOCK_PASS',flush=True)
        print(owner('--install','/data/local/tmp/input-latency-test.apk'),flush=True)
        rpc(['/system/bin/toybox','rm','-f','/data/user/0/dev.kreijstal.pocketinputlatency/files/ready.json','/data/user/0/dev.kreijstal.pocketinputlatency/files/result.json'])
        print(owner('--launch','dev.kreijstal.pocketinputlatency/.MainActivity'),flush=True)
        for _ in range(60):
            if rpc(['/system/bin/toybox','cat','/data/user/0/dev.kreijstal.pocketinputlatency/files/ready.json'],check=False)[0]==0:break
            time.sleep(1)
        else:raise RuntimeError('Real latency test activity did not gain focus')
        time.sleep(2)
        if current==BASE:
            report['baseline']=measure(False);save();print('BASELINE',json.dumps(report['baseline']),flush=True)
            before=control_pid();assert before,'Cannot locate running input bridge'
            backup='/data/local/tmp/pocket-zygote-before-async-input.jar'
            shell('cp /system/framework/pocket-zygote.jar '+backup)
            target=ROOT+'/system/framework/pocket-zygote.jar'
            upload(stage/'pocket-zygote.jar',target)
            # A generated module view can retain a hard link to the old inode.
            # Replace that known bootstrap copy too; the rest of the live view
            # and all loaded mappings stay intact until their processes exit.
            view=ROOT+'/.pocket-module-view/system/framework/pocket-zygote.jar'
            exists,data=rpc(['/system/bin/toybox','sha256sum',view],check=False)
            if exists==0:
                assert data.split()[0] in (BASE,PATCHED),'Module overrides bootstrap JAR; refusing to overwrite it'
                upload(stage/'pocket-zygote.jar',view)
                report['activeModuleViewUpdated']=True
            assert digest('/system/framework/pocket-zygote.jar')==PATCHED
            rpc(['/system/bin/setprop','ctl.restart','pocket-control'])
            for _ in range(40):
                after=control_pid()
                if after and after!=before:break
                time.sleep(.5)
            else:raise RuntimeError('Input bridge did not restart')
            report['bridgePids']={'before':before,'after':after};report['backup']=backup;save()
            time.sleep(1)
        report['patched']=measure(True);report['guestJarSha256']=digest('/system/framework/pocket-zygote.jar')
        report['validation']='PASS';save();print('PATCHED',json.dumps(report['patched']),flush=True)
        print('PASS: slow app receives ordered events; input bridge does not wait for its callback',flush=True)
    except Exception as error:
        report['validation']='FAIL';report['error']=str(error);save();raise

if __name__=='__main__':main()
