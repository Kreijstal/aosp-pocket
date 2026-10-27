#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise the production encrypted VFS only in an isolated Pi VM app."""
import argparse
import json
from pathlib import Path
import shlex
import sys
import time


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--apk',type=Path,required=True);args=p.parse_args()
    sys.path[:0]=[str(Path.home()/'lineage-cuttlefish/tools'),str(Path.home())]
    import cuttlefish_arm64 as c
    c.require_virtual()
    result=c.adb('-s',c.serial(),'install','-r',str(args.apk),check=False,timeout=120)
    if result.returncode:raise RuntimeError(result.stdout+result.stderr)
    package='dev.kreijstal.aosppocket.cefixture';report_path='/data/user/0/'+package+'/files/report.json'
    c.shell('am force-stop '+package);c.shell('su -c '+shlex.quote('rm -f '+report_path))
    c.shell('am start -n '+package+'/dev.kreijstal.aosppocket.runtime.CeFixtureActivity')
    deadline=time.monotonic()+120
    while time.monotonic()<deadline:
        try:
            report=json.loads(c.shell('su -c '+shlex.quote('cat '+report_path)));break
        except Exception:time.sleep(1)
    else:raise RuntimeError('Encrypted VFS fixture timed out')
    (Path.home()/'ce-fixture-validation.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2),flush=True)
    if report.get('validation')!='PASS':raise RuntimeError('Encrypted VFS fixture failed')


if __name__=='__main__':main()
