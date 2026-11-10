#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate the five-root CE transport in an isolated app on dedicated VM73."""
import argparse
import base64
import hashlib
import importlib.util
import json
from pathlib import Path
import shlex
import time
import uuid

PACKAGE='dev.kreijstal.aosppocket.cefixture'
ACTIVITY=PACKAGE+'/dev.kreijstal.aosppocket.runtime.CeFixtureActivity'


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--apk',type=Path,required=True)
    p.add_argument('--vm-root',type=Path,default=Path('/home/pi/pocket-api32-vm'))
    p.add_argument('--legacy-layout',action='store_true')
    args=p.parse_args();base=args.vm_root.resolve();work=args.apk.resolve().parent
    assert work.is_relative_to(base) and (base/'.pocket-api32-test-vm').is_file()
    assert set(json.loads((base/'runtime/assembly/cuttlefish_config.json').read_text())['instances'])=={'73'}
    s=importlib.util.spec_from_file_location('vm',base/'tools/cuttlefish_arm64.py')
    vm=importlib.util.module_from_spec(s);s.loader.exec_module(vm)
    assert vm.BASE.resolve()==base and vm.lock()['instance']==73;vm.require_virtual()
    prior=json.loads((base/'manager-app-validation/manager-app-report.json').read_text())
    assert prior['runId']=='5cef4bf755304c02a3ff914e66f731d3' and prior['strictPassed']
    stopped=prior['app']['stoppedSession'];assert stopped['ownerAlive'] is False and stopped['ownerExitCode']==0
    def adb(*argv,timeout=40):
        r=vm.adb('-s',vm.serial(),*argv,check=False,timeout=timeout)
        if r.returncode:raise RuntimeError((r.stdout+r.stderr)[-2000:])
        return r.stdout.strip()
    run=uuid.uuid4().hex;digest=hashlib.file_digest(args.apk.open('rb'),'sha256').hexdigest()
    result={'runId':run,'vmInstance':73,'apkSha256':digest,'legacyLayout':args.legacy_layout}
    commands=[('selinux','getenforce'),('phantomLimit','device_config get activity_manager max_phantom_processes'),('phantomMonitoring','settings get global settings_enable_monitor_phantom_procs')]
    for name,command in commands:result[name+'Before']=adb('shell',command)
    assert result['selinuxBefore']=='Enforcing'
    print(adb('install','--no-incremental','-r',str(args.apk),timeout=120),flush=True)
    assert 'DEBUGGABLE' not in adb('shell','dumpsys package '+PACKAGE)
    installed=adb('shell','pm path '+PACKAGE).splitlines();assert len(installed)==1 and installed[0].startswith('package:/data/app/')
    assert adb('shell','sha256sum '+shlex.quote(installed[0][8:])).split()[0]==digest
    adb('shell','am force-stop '+PACKAGE)
    extra='false' if args.legacy_layout else 'true'
    print(adb('shell','am start -n '+ACTIVITY+' --es probe_run_id '+run+' --ez api32_layout '+extra),flush=True)
    deadline=time.monotonic()+300
    try:
        while time.monotonic()<deadline:
            text=adb('shell','dumpsys activity '+ACTIVITY+' pocket-ce')
            rows=[line.strip().split('=',1)[1] for line in text.splitlines() if line.strip().startswith('POCKET_CE_FIXTURE=')]
            report=json.loads(base64.b64decode(rows[-1],validate=True)) if rows else {}
            if report.get('runId')==run and report.get('validation'):result['fixture']=report;break
            time.sleep(1)
        else:raise TimeoutError('CE fixture timed out; inspect the exact live app before any restart')
        for name,command in commands:
            result[name+'After']=adb('shell',command);assert result[name+'Before']==result[name+'After']
        assert report['validation']=='PASS',report.get('error','CE fixture failed')
        assert report['uid']>=10000
        assert 'CE_METADATA_PASS' in report['metadata'] and 'CE_METADATA_PASS' in report['metadataReopened']
        assert 'CE_LOCKED_PASS' in report['locked'] and 'CE_SQLITE_PASS' in report['native'] and 'CE_REOPEN_PASS' in report['reopened']
        assert report['credentialRemovalAndRestoration'].startswith('PASS:')
        if not args.legacy_layout:
            assert report['api32Layout'] is True and report['ceRoots']==5 and report['layoutMarker'] and report['vendorPlaintextRemoved']
            assert 'CE_VENDOR_LOCKED_PASS' in report['vendorLocked'] and 'CE_VENDOR_FILESYSTEM_PASS' in report['vendorNative'] and 'CE_VENDOR_REOPEN_PASS' in report['vendorReopened']
        result['validation']='PASS'
    except Exception as error:result['validation']='FAIL';result['error']=str(error);raise
    finally:
        output=work/('ce-transport-legacy-report.json' if args.legacy_layout else 'ce-transport32-report.json')
        output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2),flush=True)


if __name__=='__main__':main()
