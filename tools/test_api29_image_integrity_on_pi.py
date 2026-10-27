#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Prove a thin fixture rejects a changed, omitted image file before boot."""
import argparse
import json
from pathlib import Path
import shlex
import sys
import time
import zipfile
sys.path.insert(0,'/home/pi/lineage-cuttlefish/tools')
import cuttlefish_arm64 as c


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--apk',type=Path,required=True);args=p.parse_args()
    package='dev.kreijstal.aosppocket.api29test';base='/data/user/0/'+package+'/files/'
    source=base+'root/probe-environment';backup=base+'probe-environment.integrity-backup'
    with zipfile.ZipFile(args.apk) as apk:
        assert 'assets/root/probe-environment' not in apk.namelist(),'Test needs a reused image file'
        spec=json.loads(apk.read('assets/fixture-image.json'))['files']['probe-environment']
    c.require_virtual()
    def adb(*argv):
        r=c.adb('-s',c.serial(),*argv,timeout=30,check=False)
        if r.returncode:raise RuntimeError((r.stdout+r.stderr)[-1500:])
        return r.stdout
    def root(cmd):return adb('shell','su -c '+shlex.quote(cmd))
    # Keep the same length, so only the cryptographic digest catches this.
    adb('shell','am force-stop '+package)
    assert root('sha256sum '+shlex.quote(source)).split()[0]==spec['sha256']
    root('test ! -e '+shlex.quote(backup)+' && cp '+shlex.quote(source)+' '+shlex.quote(backup))
    try:
        root("printf X | dd of="+shlex.quote(source)+" bs=1 count=1 conv=notrunc 2>/dev/null")
        before=root('stat -c %Y '+shlex.quote(base+'native.log')).strip()
        root('rm -f '+shlex.quote(base+'report.json'))
        adb('shell','am start -n '+package+'/.MainActivity')
        for _ in range(90):
            raw=root('if [ -f '+shlex.quote(base+'report.json')+' ]; then cat '+shlex.quote(base+'report.json')+'; fi')
            if raw.strip():break
            time.sleep(1)
        else:raise RuntimeError('Integrity fixture did not finish')
        result=json.loads(raw)
        assert not result.get('passed') and 'Fixture image checksum differs: probe-environment' in result.get('error',''),result
        assert root('stat -c %Y '+shlex.quote(base+'native.log')).strip()==before,'Guest boot started before verification'
        report={'validation':'PASS','guestSdk':29,'package':package,'test':'same-size corruption of an omitted image file',
                'corruptionRejected':True,'nativeBootStarted':False,'fixtureResult':result}
        Path('/home/pi/api29-image-integrity-validation.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps(report,indent=2),flush=True)
    finally:
        adb('shell','am force-stop '+package)
        root('cat '+shlex.quote(backup)+' > '+shlex.quote(source))
        assert root('sha256sum '+shlex.quote(source)).split()[0]==spec['sha256']
        root('rm '+shlex.quote(backup))


if __name__=='__main__':main()
