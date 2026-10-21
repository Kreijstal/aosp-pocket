#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run on the Pi: production DirectSession, stalled copy/commit, real LocalSockets.

This installs only the small dedicated probe; it never stops an existing guest.
The peer deliberately simulates installation delays rather than installing an app.
"""
import argparse
import json
from pathlib import Path
import shlex
import sys
import time

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--apk',default='/home/pi/pocket-install-input-test.apk')
parser.add_argument('--expect-blocked',action='store_true')
args=parser.parse_args()
sys.path.insert(0,'/home/pi/lineage-cuttlefish/tools')
import cuttlefish_arm64 as c
assert c.shell('getprop ro.product.device')=='vsoc_arm64','This test targets only the Pi VM'
package='dev.kreijstal.pocketinstalltest'
report='/data/user/0/'+package+'/files/result.json'
c.adb('-s',c.serial(),'install','--no-incremental','-r',args.apk,timeout=90)
c.shell('su -c '+shlex.quote('rm -f '+report))
c.shell('am start -n '+package+'/dev.kreijstal.aosppocket.runtime.InstallInputTest')
for _ in range(30):
    result=c.adb('-s',c.serial(),'shell','-T','su -c '+shlex.quote('cat '+report),check=False)
    if result.returncode==0 and result.stdout.strip():
        data=json.loads(result.stdout)
        break
    time.sleep(1)
else:
    raise RuntimeError('Probe did not return a fresh result')
Path('/home/pi/pocket-install-input-validation.json').write_text(json.dumps(data,indent=2)+'\n')
print(json.dumps(data,indent=2),flush=True)
assert 'error' not in data,data
for phase in ('copy','commit'):
    assert data[phase]['input_while_blocked'] is (not args.expect_blocked),data
    assert data[phase]['install_completed_after_release'],data
print('PASS: '+('baseline blocks input as expected' if args.expect_blocked else 'input remains responsive during copy and commit'))
