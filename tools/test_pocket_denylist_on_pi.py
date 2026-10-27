#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare the real app with/without Magisk DenyList; restore its settings."""
import argparse
import io
import json
from pathlib import Path
import re
import shlex
import sys
import time

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--fixture',type=Path,required=True);args=p.parse_args()
    sys.path[:0]=[str(Path.home()/'lineage-cuttlefish/tools'),str(Path.home())]
    import cuttlefish_arm64 as c
    from pocketctl import request
    c.require_virtual();cfg=json.loads((Path.home()/'.config/pocket-pi-debug.json').read_text())
    def rpc(argv,source=None):
        out,err=io.BytesIO(),io.BytesIO();rc=request(cfg,2,argv,source=source,stdout=out,stderr=err)
        if rc:raise RuntimeError((argv,rc,err.getvalue().decode(errors='replace')))
        return out.getvalue()
    def text(*argv):return rpc(list(argv)).decode()
    def shell(command):return text('/system/bin/sh','-c',command)
    def magisk(*argv):return text('/sbin/magisk',*argv)
    assert text('/system/bin/getprop','pocket.zygisk.enabled').strip()=='1','Enable guest Zygisk and restart Pocket before testing its DenyList'
    def upload(path,data):
        rpc(['/system/bin/sh','-c','umask 077; /system/bin/toybox cat > "$1"','pocket-upload',path+'.new'],io.BytesIO(data))
        text('/system/bin/toybox','mv',path+'.new',path)
    package='dev.kreijstal.attestprobe';module='/data/adb/modules/pocket_denylist_fixture'
    assert shell('if [ -e '+module+' ]; then printf exists; fi')=='','Scratch module already exists'
    old_setting=magisk('--sqlite',"SELECT value FROM settings WHERE key='denylist';")
    enabled=bool(re.search(r'value=[1-9]',old_setting))
    old_row=bool(magisk('--sqlite',"SELECT process FROM denylist WHERE package_name='"+package+"' AND process='"+package+"';").strip())
    uid=int(re.search(r'uid:(\d+)',c.shell('cmd package list packages -U dev.kreijstal.aosppocket.runtime'))[1])
    report={'scope':'Pi dedicated VM -> Pocket -> real ordinary Attest Probe process','phases':{}}
    def pids():
        found=[]
        for line in c.shell('ps -A -o UID,PID,NAME').splitlines():
            f=line.split()
            if len(f)==3 and f[0]==str(uid) and f[2]==package:found.append(int(f[1]))
        return found
    def stop_probe():
        for pid in pids():text('/system/bin/toybox','kill','-TERM',str(pid))
    def run_phase(name,listed):
        stop_probe();text('/system/bin/toybox','rm','-f','/data/local/tmp/pocket-denylist-events')
        path='/data/user/0/'+package+'/files/report.json';text('/system/bin/toybox','rm','-f',path)
        cmd='export CLASSPATH=/system/framework/pocket-zygote.jar:/data/local/tmp/device-attest-runner.jar; exec /system/bin/pocket-app-process-api28 /system/bin RuntimeProbeRunner --device /data/local/tmp/pocket-media-attest.apk'
        print(shell(cmd),flush=True)
        deadline=time.monotonic()+130
        while time.monotonic()<deadline:
            try:
                data=json.loads(text('/system/bin/toybox','cat',path))
                if data.get('summary',{}).get('checks',0)>=59:break
            except (RuntimeError,ValueError):pass
            time.sleep(2)
        else:raise RuntimeError('No complete fresh report')
        app=pids();assert len(app)==1,app
        events=text('/system/bin/toybox','cat','/data/local/tmp/pocket-denylist-events')
        flags=re.search(r'uid=\d+ flags=(\d+) pid='+str(app[0])+r' process='+re.escape(package)+r'\n',events)
        assert flags,events
        actual=int(flags[1]);assert actual==(2 if listed else 0),(name,actual,events)
        checks={x['id']:x for x in data['results']}
        for key in ['root_su_paths','magisk_paths']:
            assert checks[key]['status']==('PASS' if listed else 'FAIL'),(name,checks[key])
        maps=text('/system/bin/toybox','cat','/proc/'+str(app[0])+'/maps')
        assert '/.pocket-module-view/' not in maps,'Module-view backing prefix leaked through proc maps'
        report['phases'][name]={'pid':app[0],'zygiskFlags':actual,'report':data,'moduleViewPrefixInMaps':False}
        print(name,data['summary'],'flags='+str(actual),flush=True)
    try:
        text('/system/bin/toybox','mkdir','-p',module+'/zygisk')
        upload(module+'/module.prop',b'id=pocket_denylist_fixture\nname=Pocket DenyList test\nversion=1\nversionCode=1\nauthor=Pocket\ndescription=Temporary test\n')
        upload(module+'/zygisk/arm64-v8a.so',args.fixture.read_bytes())
        if old_row:magisk('--denylist','rm',package,package)
        # Neo's own DenyList must work without Magisk namespace enforcement.
        magisk('--denylist','disable')
        report['magiskEnforcementDuringTest']=False
        run_phase('unlisted',False)
        magisk('--denylist','add',package,package)
        run_phase('listed',True)
        assert 'MAGISK' in magisk('-v').upper(),'Unlisted owner lost Magisk access'
        report['ownerStillHasMagisk']=True
    finally:
        stop_probe()
        current=bool(magisk('--sqlite',"SELECT process FROM denylist WHERE package_name='"+package+"' AND process='"+package+"';").strip())
        if current and not old_row:magisk('--denylist','rm',package,package)
        if old_row and not current:magisk('--denylist','add',package,package)
        magisk('--denylist','enable' if enabled else 'disable')
        if not old_setting.strip():magisk('--sqlite',"DELETE FROM settings WHERE key='denylist';")
        text('/system/bin/toybox','rm','-rf',module)
        report['originalDenyListRestored']=True
        (Path.home()/'pocket-denylist-validation.json').write_text(json.dumps(report,indent=2)+'\n')
    report['validation']='PASS';(Path.home()/'pocket-denylist-validation.json').write_text(json.dumps(report,indent=2)+'\n')
    print('PASS: real app root/Magisk checks, per-process membership flags, maps, unlisted owner, original settings restored')

if __name__=='__main__':main()
