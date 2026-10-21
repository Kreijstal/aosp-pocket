#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate opt-in path translation in two real Pocket app processes on the Pi."""
import argparse
import io
import json
from pathlib import Path
import shlex
import sys
import time


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config',type=Path,default=Path.home()/'.config/pocket-pi-debug.json')
    parser.add_argument('--install-manager',type=Path)
    parser.add_argument('--expect-disabled',action='store_true')
    parser.add_argument('--launch-existing',action='store_true',help='Launch an already installed fixture after restarting the guest')
    parser.add_argument('--apk',type=Path,default=Path.home()/'probe.apk')
    parser.add_argument('--control',type=Path,default=Path.home()/'control')
    parser.add_argument('--output',type=Path,default=Path.home()/'pocket-syscall-app-validation.json')
    args=parser.parse_args()
    sys.path.insert(0,'/home/pi/lineage-cuttlefish/tools')
    import cuttlefish_arm64 as c
    from pocketctl import request
    c.require_virtual()
    config=json.loads(args.config.read_text())
    def call(op,argv=(),source=None):
        out,err=io.BytesIO(),io.BytesIO()
        code=request(config,op,argv,source=source,stdout=out,stderr=err)
        if code:raise RuntimeError(str(code)+': '+out.getvalue().decode(errors='replace')+err.getvalue().decode(errors='replace'))
        return out.getvalue()
    def state():return json.loads(call(1))
    def command(*argv):return call(2,argv)
    def upload(source,path,mode='644'):
        command_line=['/system/bin/sh','-c','/system/bin/toybox cat > "$1.next" && /system/bin/toybox chmod "$2" "$1.next" && /system/bin/toybox mv "$1.next" "$1"','upload',path,mode]
        call(2,command_line,source)
    host='dev.kreijstal.aosppocket.runtime'
    if args.install_manager:
        call(4)
        for _ in range(45):
            s=state()
            if not(s['running'] or s['starting'] or s['stopping']):break
            time.sleep(1)
        else:raise RuntimeError('Guest did not stop cleanly before update')
        result=c.adb('-s',c.serial(),'install','--no-incremental','-r',str(args.install_manager),timeout=360)
        print(result.stdout,flush=True)
        c.shell('am start -n '+host+'/.MainActivity --ei guest_sdk 28 --ez start_guest true')
        # Host diagnostics only: re-enable the existing owner endpoint after
        # updating its APK. App test calls themselves do not use host root.
        c.shell('su -c '+shlex.quote('am start-foreground-service -n '+host+'/.GuestService -a dev.kreijstal.aosppocket.ADMIN'))
    for i in range(150):
        try:s=state()
        except OSError:
            if not args.install_manager:raise
            time.sleep(2);continue
        if s['ready'] and not s['stopping']:break
        if i%10==0:print(s['status'],flush=True)
        time.sleep(2)
    else:raise RuntimeError('Android 9 did not become ready')
    assert s['sdk']==28
    package='dev.kreijstal.pocketsyscalltest'
    profiles='/data/adb/pocket-syscall-profiles'
    fixture='/data/local/tmp/pocket-syscall-fixture'
    command('/system/bin/toybox','mkdir','-p',profiles,fixture,'/data/pocket-import')
    profile=('openat-prefix-v1\n'+fixture+'\n'+fixture+'\n').encode()
    upload(io.BytesIO(profile),profiles+'/'+package+':adapted.profile')
    upload(io.BytesIO(b'pocket-openat\n'),fixture+'/fixture.txt')
    command('/system/bin/toybox','rm','-f',profiles+'/'+package+':baseline.profile')
    with args.control.open('rb') as source:upload(source,'/data/local/tmp/pocket-syscall-control','755')
    # Reinstall this dedicated fixture through the guest's normal installer,
    # replacing old processes and requiring fresh reports on every run.
    command('/system/bin/toybox','rm','-f','/data/data/'+package+'/files/adapted.json',
            '/data/data/'+package+'/files/baseline.json')
    if args.launch_existing:
        print(command('/data/local/tmp/pocket-syscall-control',package).decode(),flush=True)
    else:
        with args.apk.open('rb') as source:upload(source,'/data/pocket-import/base.apk')
        print(command('/system/bin/toybox','timeout','120','/data/local/tmp/pocket-syscall-control','--install').decode(),flush=True)
        command('/system/bin/toybox','rm','-f','/data/pocket-import/base.apk')
    reports={}
    for i in range(120):
        for profile_name in ('adapted','baseline'):
            path='/data/data/'+package+'/files/'+profile_name+'.json'
            try:reports[profile_name]=json.loads(command('/system/bin/toybox','cat',path))
            except (RuntimeError,json.JSONDecodeError):pass
        if len(reports)==2:break
        time.sleep(1)
    else:
        print(command('/system/bin/toybox','tail','-c','16000','/logs/manager-init.log').decode(errors='replace'))
        raise RuntimeError('Guest app did not produce both reports')
    print(json.dumps(reports,indent=2),flush=True)
    adapted,baseline=reports['adapted'],reports['baseline']
    passed=(adapted['libcOk'] and (adapted['directOk'] != args.expect_disabled) and baseline['libcOk'] and
            not baseline['directOk'] and baseline['directErrno']!=0 and
            adapted['guestUid']==baseline['guestUid']>=10000 and
            adapted['kernelUid']==baseline['kernelUid']!=0)
    result={'environment':'Pi LineageOS VM -> Pocket Android 9 -> real APK with JNI',
        'command':'python3 /home/pi/test_syscall_app_on_pi.py',
        'managerApk':args.install_manager.name if args.install_manager else None,
        'exitCode':0 if passed else 1,
        'moduleExpectedEnabled':not args.expect_disabled,'launchExisting':args.launch_existing,
        'profiles':reports,'scope':'Opt-in openat prefix translation; existing wrappers retained'}
    args.output.write_text(json.dumps(result,indent=2)+'\n')
    assert reports['adapted']['libcOk'] and (reports['adapted']['directOk'] != args.expect_disabled),reports
    assert reports['baseline']['libcOk'] and not reports['baseline']['directOk'],reports
    assert reports['baseline']['directErrno']!=0,reports
    assert reports['adapted']['guestUid']==reports['baseline']['guestUid']>=10000,reports
    assert reports['adapted']['kernelUid']==reports['baseline']['kernelUid']!=0,reports
    print('PASS: module '+('disabled' if args.expect_disabled else 'enabled')+'; both real app processes match expected behavior')


if __name__=='__main__':main()
