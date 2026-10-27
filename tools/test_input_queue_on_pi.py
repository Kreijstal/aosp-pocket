#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Pi only: exercise the host touch queue without stopping Pocket's guest."""
import argparse
import json
from pathlib import Path
import shlex
import sys
import time


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apk',required=True)
    parser.add_argument('--baseline',action='store_true')
    args=parser.parse_args()
    sys.path.insert(0,str(Path.home()/'lineage-cuttlefish/tools'))
    import cuttlefish_arm64 as c
    c.require_virtual()
    package='dev.kreijstal.pocketinputqueuetest'
    result='/data/user/0/'+package+'/files/result.json'
    c.adb('-s',c.serial(),'install','--no-incremental','-r',args.apk,timeout=90)
    c.shell('am force-stop '+package)
    c.shell('su -c '+shlex.quote('rm -f '+result))
    c.shell('am start -n '+package+'/dev.kreijstal.aosppocket.runtime.InputQueueTest')
    for _ in range(30):
        output=c.adb('-s',c.serial(),'shell','-T','su -c '+shlex.quote('cat '+result),check=False)
        if output.returncode==0 and output.stdout.strip():
            report=json.loads(output.stdout);break
        time.sleep(1)
    else:raise RuntimeError('No fresh input queue report')
    label='baseline' if args.baseline else 'optimized'
    (Path.home()/('pocket-input-queue-'+label+'.json')).write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2),flush=True)
    assert report['validation']=='PASS',report
    assert report['latestFirstGestureX']==80,report
    assert report['deliveredFirstGestureMoves']==(80 if args.baseline else 1),report
    c.shell('am force-stop '+package)
    print('PASS: '+label+' queue; ordered boundaries and latest motion preserved',flush=True)


if __name__=='__main__':main()
