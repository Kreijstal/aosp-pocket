#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run isolated rootless storage tests in the dedicated Pi Cuttlefish VM."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import sys
import time


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apk',type=Path,required=True)
    parser.add_argument('--output',type=Path,default=Path.home()/'storage-probe-validation.json')
    args=parser.parse_args()
    sys.path[:0]=[str(Path.home()/'lineage-cuttlefish/tools'),str(Path.home())]
    import cuttlefish_arm64 as c
    c.require_virtual()
    package='dev.kreijstal.aosppocket.storageprobe'
    manager='dev.kreijstal.aosppocket.runtime'
    version_command='dumpsys package '+manager+' | sed -n "/versionCode=/p; /versionName=/p"'
    original=c.shell(version_command)
    c.adb('-s',c.serial(),'install','-r',str(args.apk),check=True,timeout=90)
    report_path='/data/user/0/'+package+'/files/report.json'
    try:
        c.shell('am force-stop '+package)
        c.shell('su -c '+shlex.quote('rm -f '+report_path))
        c.shell('am start -n '+package+'/.MainActivity')
        deadline=time.monotonic()+90
        while time.monotonic()<deadline:
            try:
                report=json.loads(c.shell('su -c '+shlex.quote('cat '+report_path)))
                break
            except Exception as error:
                last_error=error
                time.sleep(1)
        else:raise RuntimeError('Storage probe report missing') from last_error
        report['apkSha256']=hashlib.sha256(args.apk.read_bytes()).hexdigest()
        report['managerUnchanged']=c.shell(version_command)==original
        report['selinuxEvidence']=c.shell('su -c '+shlex.quote('dmesg | grep -F '+package+' | tail -6')).strip().splitlines()
        args.output.write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps(report,indent=2))
        if report.get('validation')!='PASS':raise RuntimeError('Storage probe failed')
        assert report['uid']>=10000 and report['capEff']=='0000000000000000'
        assert report['selinux'].startswith('u:r:untrusted_app')
        assert report['encryptedBackend']['validation']=='PASS'
        assert report['encryptedProxy']['mappedWriteSurvivedBackendReopen']
        assert report['managerUnchanged'] and report['filesystemReady'] is False
    finally:
        c.shell('am force-stop '+package)
        c.shell('am start -n '+manager+'/.MainActivity --ei guest_sdk 28')


if __name__=='__main__':main()
