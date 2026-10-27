#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run the installed, unchanged Attest Probe without a package-replacement race."""
import argparse
import io
import json
from pathlib import Path
import re
import shlex
import sys
import time

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--expect-version',required=True)
    p.add_argument('--report',type=Path,required=True)
    p.add_argument('--require-no-failures',action='store_true')
    args=p.parse_args()
    sys.path[:0]=[str(Path.home()),str(Path.home()/'lineage-cuttlefish/tools')]
    import cuttlefish_arm64 as c
    from pocketctl import request
    c.require_virtual();cfg=json.loads((Path.home()/'.config/pocket-pi-debug.json').read_text())
    manager='dev.kreijstal.aosppocket.runtime';package='dev.kreijstal.attestprobe'
    def version():return re.search(r'versionName=(\S+)',c.shell('dumpsys package '+manager))[1]
    def rpc(op,argv=(),check=True):
        out,err=io.BytesIO(),io.BytesIO();rc=request(cfg,op,argv,stdout=out,stderr=err)
        if check and rc:raise RuntimeError((rc,out.getvalue().decode(),err.getvalue().decode()))
        return rc,out.getvalue().decode()
    assert version()==args.expect_version
    state=json.loads(rpc(1)[1]);assert state['running'] and state['ready'] and state['sdk']==28
    uid=re.search(r'uid:(\d+)',c.shell('cmd package list packages -U '+manager))[1]
    for line in c.shell('ps -A -o UID,PID,NAME').splitlines():
        fields=line.split()
        if len(fields)==3 and fields[0]==uid and fields[2]==package:
            c.shell('su -c '+shlex.quote('kill -TERM '+fields[1]))
    report='/data/user/0/'+package+'/files/report.json'
    rpc(2,['/system/bin/toybox','rm','-f',report])
    command='export CLASSPATH=/system/framework/pocket-zygote.jar:/data/local/tmp/device-attest-runner.jar; exec /system/bin/pocket-app-process-api28 /system/bin RuntimeProbeRunner --device'
    print(rpc(2,['/system/bin/sh','-c',command])[1],flush=True)
    for _ in range(75):
        rc,data=rpc(2,['/system/bin/toybox','cat',report],check=False)
        try:
            result=json.loads(data)
            if rc==0 and result.get('summary',{}).get('checks',0)>=59:break
        except ValueError:pass
        time.sleep(2)
    else:raise RuntimeError('No complete fresh Attest Probe report')
    args.report.write_text(json.dumps(result,indent=2)+'\n')
    assert version()==args.expect_version,'Manager changed during validation'
    print('SUMMARY',json.dumps(result['summary']),flush=True)
    for row in result['results']:
        if row['status']=='FAIL' or row['id'] in ['build_tags_test_keys','contacts_readable','root_su_paths','magisk_paths']:
            print(json.dumps(row),flush=True)
    assert next(r for r in result['results'] if r['id']=='build_tags_test_keys')['status']=='PASS'
    if args.require_no_failures:assert result['summary']['fail']==0
    print('PASS: fresh installed Attest Probe; no reinstallation or changed deadlines',flush=True)

if __name__=='__main__':main()
