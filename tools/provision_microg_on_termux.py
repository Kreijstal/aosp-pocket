#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Provision official microG and the owner-supplied lock in Samsung's Pocket guest.

Run inside the already authenticated Samsung Termux account. APKs and original
ARM64 libraries are staged privately first. The credential is read from a
mode-0600 owner file and sent only on the guest command's stdin.
"""
import argparse
import fcntl
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import time

ROOT='/data/user/0/dev.kreijstal.aosppocket.runtime/files/api28-native-rootfs'
EXPECTED={
 'com.google.android.gms-252432034.apk':'e5e6cba078bdec812b5450599350e5c1a12c8b7438da5bf77123af170f8c1a35',
 'com.android.vending-84022634.apk':'9b8616011cc8f026bcbbe094df850794abb9d6d9f23214d7a8f8c4d1033ea012',
 'GsfProxy-0.1.0.apk':'86891b174301f06a1c84187b545a0a2a57044c6b768f3e84e865908743349692',
}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--timeout',type=int,default=900,help='Total runtime limit in seconds')
    args=parser.parse_args()
    if args.timeout<30:parser.error('--timeout must be at least 30 seconds')
    assert subprocess.check_output(['getprop','ro.product.model'],text=True).strip()=='SM-G780G'
    private=Path.home()/'.config/pocketctl';bundle=private/'microg-install'
    guard=(bundle/'provision.lock').open('a')
    fcntl.flock(guard,fcntl.LOCK_EX|fcntl.LOCK_NB)
    credential=private/'guest-lock-credential.json'
    assert credential.stat().st_mode&0o077==0
    secret=json.loads(credential.read_text())
    assert secret['scope']=='Pocket Android 9 guest on Samsung SM-G780G'
    password=secret['password']
    client=bundle/'pocketctl.py'
    if not client.is_file():client=Path.home()/'storage/downloads/pocketctl.py'
    spec=importlib.util.spec_from_file_location('pocketctl',client)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    connect=module.connect;read_timeout=30
    def bounded_connect(config):
        peer=connect(config);peer.settimeout(read_timeout);return peer
    module.connect=bounded_connect
    config=json.loads((private/'config.json').read_text())
    report={'scope':secret['scope'],'validation':'IN_PROGRESS','credentialFilePrivate':True}
    output=bundle/'provision-validation.json'
    def save():
        tmp=output.with_suffix('.part')
        with os.fdopen(os.open(tmp,os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600),'w') as f:
            json.dump(report,f,indent=2);f.write('\n');f.flush();os.fsync(f.fileno())
        os.replace(tmp,output)
    def failed(error):
        report['validation']='FAIL';report['error']=str(error).replace(password,'<redacted>')
        save();print('FAIL:',report['error'],flush=True)
    sys.excepthook=lambda kind,error,traceback:failed(error)
    # RuntimeError deliberately bypasses the transport OSError retry below.
    def deadline(signum,frame):raise RuntimeError('Provisioning exceeded its total runtime limit')
    signal.signal(signal.SIGALRM,deadline);signal.alarm(args.timeout)
    import atexit
    atexit.register(lambda: failed('Provisioning stopped before completion') if report['validation']=='IN_PROGRESS' else None)
    def rpc(op,argv=(),source=None,check=True,timeout=30):
        nonlocal read_timeout
        read_timeout=10 if op==1 else timeout
        out,err=io.BytesIO(),io.BytesIO()
        for attempt in range(2):
            try:
                rc=module.request(config,op,argv,source=source,stdout=out,stderr=err);break
            except OSError:
                if op!=1 or attempt==1:raise
                time.sleep(1)
        stdout=out.getvalue();stderr=err.getvalue().decode().replace(password,'<redacted>')
        if check and rc:raise RuntimeError((rc,stderr))
        return rc,stdout,stderr
    def shell(command,**kwargs):return rpc(2,['/system/bin/sh','-c',command],**kwargs)
    def digest(path):return rpc(2,['/system/bin/toybox','sha256sum',path])[1].decode().split()[0]
    def upload(source,target,mode='0644'):
        expected=hashlib.sha256(source.read_bytes()).hexdigest()
        rc,old,_=rpc(2,['/system/bin/toybox','sha256sum',target],check=False)
        if rc==0 and old.decode().split()[0]==expected:return
        temporary=target+'.pocket-upload'
        shell('mkdir -p '+shlex.quote(str(Path(target).parent)))
        if rc==0:
            backup='/data/local/tmp/microg-before-'+str(time.time_ns())+'-'+Path(target).name
            shell('cp '+shlex.quote(target)+' '+shlex.quote(backup))
        with source.open('rb') as stream:
            shell('umask 077; exec /system/bin/toybox cat > '+shlex.quote(temporary),source=stream,timeout=120)
        assert digest(temporary)==expected,'Guest upload digest mismatch: '+source.name
        shell('chmod '+mode+' '+shlex.quote(temporary)+' && mv '+shlex.quote(temporary)+' '+shlex.quote(target))
        assert digest(target)==expected
    def lock(mode):
        command='export CLASSPATH=/data/local/tmp/guest-lock-probe.jar; exec /system/bin/pocket-app-process-api28 /system/bin GuestLockProbe '+mode
        rc,out,err=shell(command,source=None if mode=='status' else io.BytesIO((password+'\n').encode()),timeout=140)
        for line in out.decode().replace(password,'<redacted>').splitlines():
            if line.startswith('GUEST_LOCK_RESULT '):
                result=json.loads(line[len('GUEST_LOCK_RESULT '):]);assert result['validation']=='PASS';return result
        raise RuntimeError('No lock result: '+err)
    def validate_microg():
        def owner(*args):
            command='export CLASSPATH=/system/framework/pocket-zygote.jar:/data/local/tmp/microg-owner.jar; exec /system/bin/pocket-app-process-api28 /system/bin MicrogOwner '
            return shell(command+' '.join(map(shlex.quote,args)),timeout=220 if args[0]=='--selfcheck' else 120)[1].decode()
        assert 'RUNTIME_PROBE_INSTALLED' in owner('--install','/data/local/tmp/microg-api-probe.apk')
        # Allow the normal PackageInstaller/AMS package-added work to settle.
        time.sleep(3)
        path='/data/user/0/dev.kreijstal.pocketmicrogtest/files/report.json'
        shell('rm -f '+path)
        assert 'MICROG_ACTIVITY_STARTED' in owner('--launch','dev.kreijstal.pocketmicrogtest/.MainActivity')
        deadline=time.monotonic()+150
        while time.monotonic()<deadline:
            rc,data,_=rpc(2,['/system/bin/toybox','cat',path],check=False)
            if rc==0:
                try:api=json.loads(data);break
                except ValueError:pass
            time.sleep(2)
        else:raise RuntimeError('Ordinary microG client did not finish')
        report['microgApi']=api;save()
        assert api['validation']=='PASS','Ordinary microG client failed'
        print('MICROG_ORDINARY_CLIENT_PASS',flush=True)
        ui=owner('--selfcheck')
        for line in ui.splitlines():
            if line.startswith('MICROG_SELFCHECK_UI '):
                pages=json.loads(line[len('MICROG_SELFCHECK_UI '):]);break
        else:raise RuntimeError('No microG Self-Check UI hierarchy')
        checks={}
        def texts(node):
            if node.get('text'):yield node['text']
            for child in node.get('children',[]):yield from texts(child)
        def collect(node):
            children=node.get('children',[]);boxes=[x for x in children if x.get('checkable')]
            labels=list(texts(node))
            if boxes and labels:checks[labels[0]]=all(x.get('checked') for x in boxes)
            for child in children:collect(child)
        for page in pages:collect(page)
        required=['System spoofs signature:','microG Services installed:','microG Services has correct signature:',
                  'microG Companion / Play Store installed:','microG Companion / Play Store has correct signature:',
                  'microG Services Framework installed:','Battery optimizations ignored:']
        report['microgSelfCheck']={name:checks.get(name) for name in required}
        report['microgOptionalPermissions']={name:ok for name,ok in checks.items() if name.startswith('Permission to ')}
        save();assert all(report['microgSelfCheck'].values()),'Required microG Self-Check failed'
        print('MICROG_REAL_SELFCHECK_PASS',flush=True)
    save();print('CHECK_TRANSFER',flush=True)
    checksums=bundle/'payload-sha256.json'
    if checksums.exists():
        for name,expected in json.loads(checksums.read_text()).items():
            assert Path(name).name==name,'Unsafe bundle filename'
            with (bundle/name).open('rb') as stream:
                assert hashlib.file_digest(stream,'sha256').hexdigest()==expected,'Transfer checksum mismatch: '+name
    print('CHECK_GUEST',flush=True)
    state=json.loads(rpc(1)[1]);assert state['sdk']==28,'Select the Android 9 guest first'
    if not state['running'] and not state['starting']:
        rpc(3)
    initial_deadline=time.monotonic()+300
    while not (state['running'] and state['ready']):
        if time.monotonic()>=initial_deadline:raise TimeoutError('Initial guest boot timed out')
        time.sleep(2);state=json.loads(rpc(1)[1])
    shell('[ -f '+ROOT+'/system/build.prop ]')
    for name,digest_expected in EXPECTED.items():
        with (bundle/name).open('rb') as f:assert hashlib.file_digest(f,'sha256').hexdigest()==digest_expected
    print('CHECK_LOCK_HELPER',flush=True)
    upload(bundle/'guest-lock-probe.jar','/data/local/tmp/guest-lock-probe.jar')
    print('CHECK_LOCK_STATUS',flush=True)
    report['lockBefore']=lock('status');save()
    # Check an existing password before modifying the system image.
    if report['lockBefore']['deviceSecure']:lock('verify')
    # Write the persistent base image, not an optional generated module view.
    manifest=json.loads((bundle/'install-manifest.json').read_text())
    for row in sorted(manifest,key=lambda r:r['target'].endswith('.apk')):
        assert row['target'].startswith(('system/priv-app/Pocket','system/app/Pocket'))
        assert '..' not in Path(row['target']).parts
        upload(bundle/row['source'],ROOT+'/'+row['target'],'0755' if row['target'].endswith('.so') else '0644')
        print('STAGED',row['target'],flush=True)
    for name in ['microg-owner.jar','microg-api-probe.apk']:
        upload(bundle/name,'/data/local/tmp/'+name)
    report['microgFilesVerified']=True;save()
    report['lockSetup']=lock('verify' if report['lockBefore']['deviceSecure'] else 'setup');save()
    print('PASSWORD_SETUP_PASS',flush=True)
    rpc(4)
    for _ in range(120):
        state=json.loads(rpc(1)[1])
        if not any(state[k] for k in ['running','starting','stopping']):break
        time.sleep(1)
    else:raise RuntimeError('Guest did not stop normally')
    rpc(3)
    for i in range(300):
        state=json.loads(rpc(1)[1])
        if i%15==0:print('BOOT',state['status'],flush=True)
        if state['running'] and state['ready'] and rpc(2,['/system/bin/getprop','sys.boot_completed'])[1].strip()==b'1':break
        time.sleep(2)
    else:raise RuntimeError('Guest did not finish boot')
    report['lockAfterBoot']=lock('status');assert report['lockAfterBoot']['deviceSecure'] and report['lockAfterBoot']['keyguardLocked'];save()
    report['normalPasswordUiUnlock']=lock('unlock');save()
    print('NORMAL_PASSWORD_UI_UNLOCK_PASS',flush=True)
    packages=rpc(2,['/system/bin/toybox','cat','/data/system/packages.list'])[1].decode().splitlines()
    report['installedPackages']={line.split()[0]:int(line.split()[1]) for line in packages if line.split()[0] in ['com.google.android.gms','com.android.vending','com.google.android.gsf','com.revolut.revolut']}
    assert all(n in report['installedPackages'] for n in ['com.google.android.gms','com.android.vending','com.google.android.gsf'])
    revolut_uid=report['installedPackages'].get('com.revolut.revolut')
    rules={}
    for family in ['-4','-6']:
        rows=rpc(2,['/system/bin/iptables',family,'-S'])[1].decode().strip().splitlines()
        if revolut_uid is not None:
            blocked='-A OUTPUT -m owner --uid-owner '+str(revolut_uid)+' -j DROP'
            indexes=[i for i,row in enumerate((r for r in rows if r.startswith('-A ')),1) if row==blocked]
            if indexes:
                shell('cp /data/misc/pocket-firewall.rules /data/local/tmp/firewall-before-microg-'+str(time.time_ns()))
                for index in reversed(indexes):rpc(2,['/system/bin/iptables',family,'-D','OUTPUT',str(index)])
            rows=rpc(2,['/system/bin/iptables',family,'-S'])[1].decode().strip().splitlines()
            assert blocked not in rows,'Revolut DROP rule remains'
        rules[family]=rows
    report['revolutFirewall']=rules;save()
    validate_microg()
    report['validation']='PASS';save()
    print('PASS: official microG installed, normal password UI unlock verified, Revolut firewall remains open',flush=True)

if __name__=='__main__':main()
