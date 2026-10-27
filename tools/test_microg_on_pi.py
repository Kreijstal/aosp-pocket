#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise original microG and package-query isolation in the dedicated Pi VM."""
import argparse
import io
import json
from pathlib import Path
import shlex
import sys
import time

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--probe',type=Path,required=True)
    p.add_argument('--owner',type=Path,required=True)
    p.add_argument('--require-zygisk-disabled',action='store_true')
    p.add_argument('--selfcheck-only',action='store_true',help='Inspect the UI without reinstalling/retesting the client')
    args=p.parse_args()
    sys.path[:0]=[str(Path.home()/'lineage-cuttlefish/tools'),str(Path.home())]
    import cuttlefish_arm64 as c
    from pocketctl import request
    c.require_virtual();cfg=json.loads((Path.home()/'.config/pocket-pi-debug.json').read_text())
    def rpc(op,argv=()):
        out,err=io.BytesIO(),io.BytesIO();rc=request(cfg,op,argv,stdout=out,stderr=err)
        if rc:raise RuntimeError((rc,out.getvalue().decode(),err.getvalue().decode()))
        return out.getvalue().decode()
    def shell(command):return rpc(2,['/system/bin/sh','-c',command])
    state=json.loads(rpc(1));assert state['ready'] and state['running'] and state['sdk']==28,state
    enabled=shell('/system/bin/getprop pocket.zygisk.enabled').strip()
    if args.require_zygisk_disabled:assert enabled=='0','Restart guest with Zygisk disabled first'
    root='/data/user/0/dev.kreijstal.aosppocket.runtime/files/api28-native-rootfs'
    for source,name in [(args.probe,'microg-api-probe.apk'),(args.owner,'microg-owner.jar')]:
        if args.selfcheck_only and name.endswith('.apk'):continue
        stage='/data/local/tmp/'+name;c.adb('-s',c.serial(),'push',str(source),stage,check=True,timeout=60)
        dest=root+'/system-data/local/tmp/'+name
        c.shell('su -c '+shlex.quote('cp '+stage+' '+dest+' && chmod 0644 '+dest+' && chown 10177:10177 '+dest))
    def owner(*args):
        command='export CLASSPATH=/system/framework/pocket-zygote.jar:/data/local/tmp/microg-owner.jar; exec /system/bin/pocket-app-process-api28 /system/bin MicrogOwner '
        return shell(command+' '.join(shlex.quote(a) for a in args))
    if not args.selfcheck_only:
        print(owner('--install','/data/local/tmp/microg-api-probe.apk'),flush=True)
        path='/data/user/0/dev.kreijstal.pocketmicrogtest/files/report.json'
        shell('/system/bin/toybox rm -f '+path)
        print(owner('--launch','dev.kreijstal.pocketmicrogtest/.MainActivity'),flush=True)
        deadline=time.monotonic()+150
        while time.monotonic()<deadline:
            try:report=json.loads(shell('/system/bin/toybox cat '+path));break
            except (RuntimeError,ValueError):time.sleep(2)
        else:raise RuntimeError('Ordinary app did not complete its microG API tests')
        report['zygiskEnabled']=enabled;report['scope']='Pi dedicated VM -> Pocket Android 9 -> ordinary client app'
        dest=Path.home()/'pocket-microg-validation.json';dest.write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps(report,indent=2),flush=True)
        assert report['validation']=='PASS',report.get('error')
    ui=owner('--selfcheck')
    for line in ui.splitlines():
        if line.startswith('MICROG_SELFCHECK_UI '):
            tree=json.loads(line[len('MICROG_SELFCHECK_UI '):]);break
    else:raise RuntimeError('Real microG Self-Check UI did not return a hierarchy')
    (Path.home()/'pocket-microg-selfcheck-ui.json').write_text(json.dumps(tree,indent=2)+'\n')
    def texts(node):
        if node.get('text'):yield node['text']
        for child in node.get('children',[]):yield from texts(child)
    labels=list(dict.fromkeys(label for page in tree for label in texts(page)))
    print('microG Self-Check UI:',json.dumps(labels),flush=True)
    assert any('microg' in str(page).lower() for page in tree),'Self-Check activity was not visible'
    checks={}
    def collect(node):
        children=node.get('children',[]);boxes=[x for x in children if x.get('checkable')]
        names=list(texts(node))
        if boxes and names:checks[names[0]]=all(x.get('checked') for x in boxes)
        for child in children:collect(child)
    for page in tree:collect(page)
    required=['System spoofs signature:','microG Services installed:','microG Services has correct signature:',
              'microG Companion / Play Store installed:','microG Companion / Play Store has correct signature:',
              'microG Services Framework installed:','Battery optimizations ignored:']
    result={'validation':'PASS' if all(checks.get(name) is True for name in required) else 'FAIL',
            'source':'original microG Self-Check UI','required':{name:checks.get(name) for name in required},
            'optionalPermissionChecks':{name:ok for name,ok in checks.items() if name.startswith('Permission to ')}}
    (Path.home()/'pocket-microg-selfcheck-validation.json').write_text(json.dumps(result,indent=2)+'\n')
    assert result['validation']=='PASS',result['required']
    print('Required microG framework/package checks: PASS; optional runtime permissions retain Android controls',flush=True)
    print('PASS: actual microG Self-Check hierarchy saved' if args.selfcheck_only else 'PASS: package APIs, caller isolation, Google account authenticator, real GMS broker/common RPC; actual microG Self-Check hierarchy saved')

if __name__=='__main__':main()
