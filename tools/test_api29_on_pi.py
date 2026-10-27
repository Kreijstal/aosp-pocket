#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate API 29 native/ART bring-up in a separate untrusted Android app."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import sys
import time
from contextlib import contextmanager

sys.path.insert(0, '/home/pi/lineage-cuttlefish/tools')
import cuttlefish_arm64 as c


@contextmanager
def vm_network_mode(temporarily_unrestrict):
    """Restore the dedicated VM's setting even when the guest assertion fails."""
    def shell(command):
        result=c.adb('-s',c.serial(),'shell',command,timeout=20,check=False)
        if result.returncode:raise RuntimeError((result.stdout+result.stderr)[-2000:])
        return result.stdout.strip()
    original=shell('settings get global restricted_networking_mode')
    if original not in ('null','0','1'):raise RuntimeError('Unknown VM restricted networking mode')
    changed=temporarily_unrestrict and original=='1'
    try:
        if changed:
            shell('settings put global restricted_networking_mode 0')
            assert shell('settings get global restricted_networking_mode')=='0'
        yield {'restrictedNetworkingModeBefore':original,'temporarilyDisabled':changed}
    finally:
        if changed:
            shell('settings put global restricted_networking_mode 1')
            assert shell('settings get global restricted_networking_mode')==original
            print('API29_VM_NETWORK_SETTING_RESTORED restricted_networking_mode=1',flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apk',type=Path,required=True)
    parser.add_argument('--art',action='store_true')
    parser.add_argument('--properties',action='store_true')
    parser.add_argument('--binder',action='store_true')
    parser.add_argument('--init',action='store_true')
    parser.add_argument('--zygote',action='store_true')
    parser.add_argument('--hardware',action='store_true')
    parser.add_argument('--system-server',action='store_true')
    parser.add_argument('--ui',action='store_true')
    parser.add_argument('--temporarily-unrestrict-vm-network',action='store_true',
                        help='Temporarily disable Restricted Networking Mode in the guarded test VM and restore it after validation')
    args=parser.parse_args();c.require_virtual()
    with vm_network_mode(args.temporarily_unrestrict_vm_network) as environment:
        run(args,environment)


def run(args,environment):
    if args.ui:args.system_server=True
    digest=hashlib.sha256()
    with args.apk.open('rb') as apk:
        for block in iter(lambda:apk.read(1024*1024),b''):digest.update(block)
    package='dev.kreijstal.aosppocket.api29test'
    def adb(*argv,timeout=30):
        result=c.adb('-s',c.serial(),*argv,timeout=timeout,check=False)
        if result.returncode:raise RuntimeError((result.stdout+result.stderr)[-4000:])
        return result.stdout
    print(adb('install','--no-incremental','-r',str(args.apk),timeout=120),flush=True)
    adb('shell','am force-stop '+package)
    report_path='/data/user/0/'+package+'/files/report.json'
    adb('shell','su -c '+shlex.quote('rm -f '+report_path))
    print(adb('shell','am start -n '+package+'/.MainActivity'),flush=True)
    # Extraction precedes the native 120-second watchdog. A full framework
    # fixture is much larger than the foundation probe on the Pi.
    deadline=time.monotonic()+420
    while time.monotonic()<deadline:
        raw=adb('shell','su -c '+shlex.quote('if [ -f '+report_path+' ]; then cat '+report_path+'; fi'))
        if raw.strip():break
        time.sleep(1)
    else:raise RuntimeError('API 29 fixture did not finish within 420 seconds')
    report=json.loads(raw)
    report['hostNetworkEnvironment']=environment
    report.update(apkSha256=digest.hexdigest(),apkBytes=args.apk.stat().st_size)
    report.update(guestSdk=29,artRequested=args.art,propertiesRequested=args.properties,binderRequested=args.binder,initRequested=args.init,fullGuestBootVerified=False,
                  command='python3 /home/pi/test_api29_on_pi.py --apk '+shlex.quote(str(args.apk))+(' --art' if args.art else '')+(' --properties' if args.properties else '')+(' --binder' if args.binder else '')+(' --init' if args.init else ''))
    if args.art:
        report['passed']=report.get('passed',False) and 'REAL_ANDROID10_ART_PASS' in report.get('log','')
    if args.properties:
        report['passed']=report.get('passed',False) and 'API29_PROPERTY_SERVICE_PASS' in report.get('log','')
    if args.binder:
        report['passed']=report.get('passed',False) and 'API29_BINDER_PASS' in report.get('log','')
    if args.init:
        report['passed']=report.get('passed',False) and 'API29_INIT_PASS' in report.get('log','')
    if args.zygote:
        report['passed']=report.get('passed',False) and 'API29_ZYGOTE_PASS' in report.get('log','')
        report['zygoteRequested']=True
        report['command']+=' --zygote'
    if args.hardware:
        report['passed']=report.get('passed',False) and 'API29_HARDWARE_PASS' in report.get('log','')
        report['hardwareRequested']=True
        report['command']+=' --hardware'
    if args.system_server:
        report['passed']=report.get('passed',False) and 'API29_FRAMEWORK_SERVICES_PASS' in report.get('log','')
        report['systemServerRequested']=True
        report['command']+=' --system-server'
    if args.ui:
        report['uiRequested']=True
        report['passed']=report.get('passed',False) and all(marker in report.get('log','') for marker in
            ['API29_UI_SMOKE_PASS','DIRECT_FRAMEWORK_LAUNCHER_PASS','DIRECT_FRAMEWORK_INSTALL_PASS','DIRECT_FRAMEWORK_INPUT_PASS'])
        report['command']+=' --ui'
        report['fullGuestBootVerified']=bool(report['passed'])
        if report['passed']:report['scope']='Isolated Android 10 framework, launcher, APK install/run and injected touch verified; production Manager integration remains separate'
    if args.temporarily_unrestrict_vm_network:
        report['command']+=' --temporarily-unrestrict-vm-network'
    path=Path.home()/('api29-system-server-validation.json' if args.system_server else 'api29-art-validation.json' if args.art else 'api29-native-validation.json')
    path.write_text(json.dumps(report,indent=2)+'\n')
    display=dict(report)
    if len(display.get('log',''))>7000:
        display['log']=display['log'][:4000]+'\n... full log in '+str(path)+' ...\n'+display['log'][-2500:]
    print(json.dumps(display,indent=2),flush=True)
    assert report.get('passed'),'Android 10 runtime test failed; see saved report'


if __name__=='__main__':main()
