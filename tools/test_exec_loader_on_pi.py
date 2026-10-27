#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run the separate loader fixture APK in the Pi's virtual Android host."""
import argparse
import json
import re
from pathlib import Path
import shlex
import sys
import time

sys.path.insert(0,'/home/pi/lineage-cuttlefish/tools')
import cuttlefish_arm64 as c

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apk',type=Path,default=Path('/home/pi/loader-test.apk'))
    parser.add_argument('--boot',action='store_true')
    parser.add_argument('--firewall',action='store_true')
    parser.add_argument('--denylist',action='store_true')
    parser.add_argument('--proc-view',action='store_true',help='Require the native proc path/descriptor regression')
    args=parser.parse_args();c.require_virtual()
    if args.proc_view and (args.boot or args.firewall):parser.error('--proc-view uses the standalone native probe')
    package='dev.kreijstal.aosppocket.loadertest'
    def adb(*argv,timeout=30):
        result=c.adb('-s',c.serial(),*argv,timeout=timeout,check=False)
        if result.returncode:raise RuntimeError((result.stdout+result.stderr)[-6000:])
        return result.stdout
    print(adb('install','--no-incremental','-r',str(args.apk),timeout=120),flush=True)
    adb('shell','am force-stop '+package)
    report_path='/data/user/0/'+package+'/files/report.json'
    adb('shell','su -c '+shlex.quote('rm -f '+report_path))
    print(adb('shell','am start -n '+package+'/.MainActivity'+(' --ez boot true' if args.boot else '')+(' --ez firewall true' if args.firewall else '')+(' --ez denylist true' if args.denylist else '')),flush=True)
    for _ in range(190):
        raw=adb('shell','su -c '+shlex.quote('if [ -f '+report_path+' ]; then cat '+report_path+'; fi'))
        if raw.strip():break
        time.sleep(1)
    else:raise RuntimeError('Fixture did not write its report')
    report=json.loads(raw)
    if args.proc_view:
        marker=re.search(r'^PROC_VIEW_PASS (.+)$',report.get('log',''),re.MULTILINE)
        assert marker,'The proc-view regression did not run'
        report['procView']={key:int(value) for key,value in (field.split('=') for field in marker[1].split())}
        assert all(report['procView'].values()),'Incomplete proc-view regression'
    chain=re.search(r'LOADER_CHAIN_PASS execs=(\d+) filters=(\d+) libcBytes=(\d+) differentBytes=(\d+) tracerPid=(\d+) kernelUid=(\d+)',report.get('log',''))
    if chain:
        report['native']={name:int(value) for name,value in zip(
            ('execs','filtersPerProcess','bionicExecutableBytesCompared','differentBytes','tracerPid','kernelUid'),chain.groups())}
        assert report['native']['kernelUid']==report['hostUid'],'Fixture changed kernel UID'
    report['command']='python3 /home/pi/test_exec_loader_on_pi.py --apk '+shlex.quote(str(args.apk))+(' --boot' if args.boot else '')+(' --firewall' if args.firewall else '')+(' --proc-view' if args.proc_view else '')+(' --denylist' if args.denylist else '')
    report['scope']='Separate non-debuggable Android APK; native init/zygote bring-up, no system_server' if args.boot else 'Separate non-debuggable Android APK under its normal untrusted_app domain; not a full Android guest boot'
    if args.firewall:report['scope']='Real TCP/UDP syscalls under the native loader in an untrusted_app fixture; host traffic remains allowed'
    (Path.home()/('denylist-view-validation.json' if args.denylist else 'proc-view-validation.json' if args.proc_view else 'guest-firewall-validation.json' if args.firewall else 'init-zygote-validation.json' if args.boot else 'exec-loader-validation.json')).write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2),flush=True)
    assert report.get('passed'),'Native fixture failed; see the saved validation JSON'

if __name__=='__main__':main()
