#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Measure Contacts provider acquisition/query/count separately in a normal app."""
import argparse
import io
import json
from pathlib import Path
import shlex
import sys
import time
import threading
import re

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--probe',type=Path,required=True);p.add_argument('--owner',type=Path,required=True)
    p.add_argument('--report',type=Path,required=True)
    p.add_argument('--max-ms',type=int)
    p.add_argument('--expect-version',help='Require this installed Pocket version before and after the query')
    p.add_argument('--expect-dex-sha256',help='Reject concurrent APK replacements even when version names match')
    p.add_argument('--expect-ce-sha256',help='Verify the native encrypted-filesystem library being tested')
    p.add_argument('--restart-provider',action='store_true')
    p.add_argument('--trace-after-seconds',type=float,help='Diagnostic SIGQUIT sample; not a latency acceptance run')
    args=p.parse_args()
    if args.max_ms is not None and args.trace_after_seconds is not None:p.error('Run latency acceptance separately from stack sampling')
    sys.path[:0]=[str(Path.home()/'lineage-cuttlefish/tools'),str(Path.home())]
    import cuttlefish_arm64 as c
    from pocketctl import request
    c.require_virtual();cfg=json.loads((Path.home()/'.config/pocket-pi-debug.json').read_text())
    def rpc(op,argv=(),check=True):
        out,err=io.BytesIO(),io.BytesIO();rc=request(cfg,op,argv,stdout=out,stderr=err)
        if check and rc:raise RuntimeError((rc,out.getvalue().decode(),err.getvalue().decode()))
        return rc,out.getvalue().decode()
    def shell(command,check=True):return rpc(2,['/system/bin/sh','-c',command],check)[1]
    state=json.loads(rpc(1)[1]);assert state['running'] and state['ready'] and state['sdk']==28,state
    def manager_version():
        match=re.search(r'versionName=(\S+)',c.shell('dumpsys package dev.kreijstal.aosppocket.runtime'))
        if not match:raise RuntimeError('Cannot determine installed Pocket version')
        return match[1]
    version=manager_version()
    if args.expect_version is not None:assert version==args.expect_version,version
    def manager_asset(entry):
        apk=c.shell('pm path dev.kreijstal.aosppocket.runtime').strip().removeprefix('package:')
        command='unzip -p '+shlex.quote(apk)+' '+shlex.quote(entry)+' | sha256sum'
        digest=c.shell('su -c '+shlex.quote(command)).split()[0]
        if not re.fullmatch('[0-9a-f]{64}',digest):raise RuntimeError('Cannot read installed Manager DEX digest')
        return digest
    dex_before=manager_asset('classes.dex') if args.expect_dex_sha256 else None
    if args.expect_dex_sha256:assert dex_before==args.expect_dex_sha256,dex_before
    ce_before=manager_asset('lib/arm64-v8a/libpocket_ce.so') if args.expect_ce_sha256 else None
    if args.expect_ce_sha256:assert ce_before==args.expect_ce_sha256,ce_before
    root='/data/user/0/dev.kreijstal.aosppocket.runtime/files/api28-native-rootfs'
    for source,name in [(args.probe,'microg-api-probe.apk'),(args.owner,'microg-owner.jar')]:
        stage='/data/local/tmp/'+name;c.adb('-s',c.serial(),'push',str(source),stage,check=True,timeout=60)
        dest=root+'/system-data/local/tmp/'+name
        c.shell('su -c '+shlex.quote('cp '+stage+' '+dest+' && chmod 0644 '+dest+' && chown 10177:10177 '+dest))
    def owner(*argv):return shell('export CLASSPATH=/system/framework/pocket-zygote.jar:/data/local/tmp/microg-owner.jar; exec /system/bin/pocket-app-process-api28 /system/bin MicrogOwner '+' '.join(shlex.quote(a) for a in argv))
    print(owner('--install','/data/local/tmp/microg-api-probe.apk'),flush=True)
    # The integration test normally has no READ_CONTACTS. Restore that state
    # even if acquisition or query fails; microG's own permissions are untouched.
    granted=False
    try:
        initial=owner('--contacts-permission')
        if 'CONTACTS_TEST_PERMISSION=0' not in initial:
            granted=True;print(owner('--grant-contacts'),flush=True)
        host_uid=re.search(r'uid:(\d+)',c.shell('cmd package list packages -U dev.kreijstal.aosppocket.runtime'))[1]
        def guest_pids():
            return {f[2]:f[1] for f in (line.split() for line in c.shell('ps -A -o UID,PID,NAME').splitlines()) if len(f)==3 and f[0]==host_uid}
        if args.restart_provider:
            pid=guest_pids().get('android.process.acore')
            if pid:rpc(2,['/system/bin/toybox','kill','-TERM',pid]);time.sleep(1)
        else:pid=None
        path='/data/user/0/dev.kreijstal.pocketmicrogtest/files/contacts-timing.json'
        shell('/system/bin/toybox rm -f '+path)
        print(owner('--launch','dev.kreijstal.pocketmicrogtest/.ContactsTiming'),flush=True)
        sampler=None
        if args.trace_after_seconds is not None:
            def sample():
                # Sample the slow query itself, after provider acquisition and
                # app/provider startup have completed.
                started=False
                for _ in range(60):
                    pid=guest_pids().get('dev.kreijstal.pocketmicrogtest')
                    log=c.shell('su -c '+shlex.quote('tail -n 2000 '+root+'/logs/manager-init.log'))
                    if pid and any('pid='+pid+' ' in line and 'PocketContactsTiming run=0 acquireMs=' in line and 'query start' in line for line in log.splitlines()):started=True;break
                    time.sleep(.25)
                if not started:print('Query sample skipped: no fresh query-start marker',flush=True);return
                time.sleep(args.trace_after_seconds)
                pids=guest_pids()
                for name in ['android.process.acore','dev.kreijstal.aosppocket.runtime']:
                    if name in pids:
                        c.shell('su -c '+shlex.quote('kill -QUIT '+pids[name]));print('Sampled stack:',name,pids[name],flush=True)
            sampler=threading.Thread(target=sample);sampler.start()
        deadline=time.monotonic()+120
        while time.monotonic()<deadline:
            try:report=json.loads(shell('/system/bin/toybox timeout 3 /system/bin/toybox cat '+path));break
            except (RuntimeError,ValueError):time.sleep(1)
        else:raise RuntimeError('Contacts query still blocked after 120 seconds; inspect PocketContactsTiming log')
        report['scope']='Pi VM -> Pocket API 28 -> ordinary client; no contact values logged'
        report['stackSampling']=args.trace_after_seconds is not None
        report['managerVersion']=version
        report['managerVersionAfter']=manager_version()
        report['providerRestartRequested']=args.restart_provider
        report['providerPidBeforeRestart']=pid
        report['providerPidAfter']=guest_pids().get('android.process.acore')
        if args.restart_provider and pid and report['providerPidAfter']==pid:report['validation']='FAIL'
        if report['managerVersionAfter']!=version:report['validation']='FAIL'
        if dex_before:
            report['managerDexSha256']=dex_before;report['managerDexSha256After']=manager_asset('classes.dex')
            if report['managerDexSha256After']!=dex_before:report['validation']='FAIL'
        if ce_before:
            report['ceLibrarySha256']=ce_before;report['ceLibrarySha256After']=manager_asset('lib/arm64-v8a/libpocket_ce.so')
            if report['ceLibrarySha256After']!=ce_before:report['validation']='FAIL'
        if args.max_ms is not None:
            report['latencyBudgetMs']=args.max_ms
            report['latencyBudgetMet']=all(r['totalMs']<=args.max_ms for r in report['runs'])
            if not report['latencyBudgetMet']:report['validation']='FAIL'
        if sampler:sampler.join()
        args.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2),flush=True)
        assert report['validation']=='PASS',report
    finally:
        if granted:print(owner('--revoke-contacts'),flush=True)

if __name__=='__main__':main()
