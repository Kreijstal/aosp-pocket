#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Verify Attest Probe's real app-process runtime checks in the running guest.

Requires the separately built Attest Probe 1.6 and its RuntimeProbeRunner test
JAR already installed/staged. Does not attach a debugger or change networking.
"""
import io
import json
from pathlib import Path
import sys
import time
sys.path.insert(0,'/home/pi/lineage-cuttlefish/tools')
import cuttlefish_arm64 as c
from pocketctl import request

def main():
    c.require_virtual()
    config=json.loads((Path.home()/'.config/pocket-pi-debug.json').read_text())
    def rpc(op,argv=()):
        out,err=io.BytesIO(),io.BytesIO()
        code=request(config,op,argv,stdout=out,stderr=err)
        if code:raise RuntimeError((code,out.getvalue().decode(errors='replace'),err.getvalue().decode(errors='replace')))
        return out.getvalue()
    assert json.loads(rpc(1))['ready'],'Guest is not ready'
    path='/data/user/0/dev.kreijstal.attestprobe/files/runtime-checks.json'
    rpc(2,['/system/bin/toybox','rm','-f',path])
    cmd='export CLASSPATH=/system/framework/pocket-zygote.jar:/data/local/tmp/runtime-attest-runner.jar; exec /system/bin/pocket-app-process-api28 /system/bin RuntimeProbeRunner'
    print(rpc(2,['/system/bin/sh','-c',cmd]).decode(),flush=True)
    for _ in range(60):
        try:report=json.loads(rpc(2,['/system/bin/toybox','cat',path]));break
        except (RuntimeError,json.JSONDecodeError):time.sleep(1)
    else:raise RuntimeError('The app did not publish its runtime report')
    for name in ('runtime_jvmti','runtime_jdwp','native_tracer'):
        assert report['checks'][name]['status']=='PASS',(name,report['checks'][name])
    assert report['checks']['runtime_agent_arguments']['status']=='INFO'
    report['guest_status']=json.loads(rpc(1));assert report['guest_status']['ready']
    report['command']='python3 /home/pi/test_jvmti_free_on_pi.py'
    report['result']='PASS'
    (Path.home()/'jvmti-free-manager-validation.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2),flush=True)

if __name__=='__main__':main()
