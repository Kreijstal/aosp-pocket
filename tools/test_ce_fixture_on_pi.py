#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run a fresh encrypted-VFS fixture on isolated VM73 without root."""
import argparse,base64,hashlib,importlib.util,json,re,shlex,time,uuid
from pathlib import Path


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--apk',type=Path,required=True)
    p.add_argument('--vm-root',type=Path,default=Path('/home/pi/pocket-api32-vm'))
    p.add_argument('--work-dir',type=Path,required=True)
    p.add_argument('--api32-layout',action='store_true')
    p.add_argument('--checkpoint-migration',action='store_true')
    p.add_argument('--logical-hardlinks',action='store_true',help='Exercise encrypted namespace links when host SELinux forbids creating a physical source link')
    args=p.parse_args()
    if args.checkpoint_migration and not args.api32_layout:p.error('--checkpoint-migration requires --api32-layout')
    if args.logical_hardlinks and not args.checkpoint_migration:p.error('--logical-hardlinks requires --checkpoint-migration')
    base=args.vm_root.resolve();work=args.work_dir.resolve();apk=args.apk.resolve()
    assert (base/'.pocket-api32-test-vm').is_file() and work.is_relative_to(base) and apk.is_relative_to(work)
    config=base/'runtime/assembly/cuttlefish_config.json'
    assert config.resolve().is_relative_to(base) and set(json.loads(config.read_text())['instances'])=={'73'}
    spec=importlib.util.spec_from_file_location('vm73',base/'tools/cuttlefish_arm64.py')
    vm=importlib.util.module_from_spec(spec);spec.loader.exec_module(vm)
    assert vm.BASE.resolve()==base and vm.lock()['instance']==73 and vm.lock()['adb_server_port']==5044
    vm.require_virtual();assert vm.serial()=='127.0.0.1:6592'
    def adb(*argv,timeout=30):return vm.adb('-s',vm.serial(),*argv,check=True,timeout=timeout).stdout.strip()
    meta=json.loads((apk.parent/'build.json').read_text());package=meta['package']
    assert re.fullmatch(r'dev\.kreijstal\.aosppocket\.cefixture[0-9]*',package)
    assert hashlib.file_digest(apk.open('rb'),'sha256').hexdigest()==meta['sha256']
    assert 'package:'+package not in adb('shell','pm list packages -u '+package).splitlines(),'Preserve the existing fixture; build a fresh package'
    names=['pocket-init-api','pocket-core-api32','fuse-stock','pocket-media-client','system/bin/pocket-loader']
    def stopped():
        ps=adb('shell','ps -A -o PID,PPID,UID,NAME:128,ARGS')
        assert not any(any(n in row for n in names) for row in ps.splitlines()),'An active guest owns VM73'
        return ps
    stopped()
    assert int(adb('shell','df -k /data').splitlines()[-1].split()[3])*1024>256*1024*1024
    def policy():
        assert adb('shell','getenforce')=='Enforcing'
        text=adb('shell','dumpsys activity settings')
        assert re.findall(r'(?m)^[ \t]*max_phantom_processes=([0-9]+)[ \t]*$',text)==['32']
        for command,value in [('device_config get activity_manager max_phantom_processes','null'),('settings get global settings_enable_monitor_phantom_procs','null'),('getprop persist.sys.fflag.override.settings_enable_monitor_phantom_procs','')]:assert adb('shell',command)==value
        return text
    before=policy();run=uuid.uuid4().hex;activity=package+'/dev.kreijstal.aosppocket.runtime.CeFixtureActivity'
    launch={'runId':run,'package':package,'apkSha256':meta['sha256'],'activity':activity,'api32Layout':args.api32_layout,'checkpointMigration':args.checkpoint_migration,'sourceHardlink':not args.logical_hardlinks}
    def save(name,value):
        with (work/name).open('x') as out:json.dump(value,out,indent=2)
    save('fixture-launch.json',launch)
    with (work/'host-policy-before.txt').open('x') as out:out.write(before)
    assert 'Success' in adb('install','--no-incremental',str(apk),timeout=180)
    installed=adb('shell','pm path '+package).splitlines();assert len(installed)==1 and installed[0].startswith('package:/data/app/')
    assert adb('shell','sha256sum '+shlex.quote(installed[0][8:])).split()[0]==meta['sha256']
    assert 'DEBUGGABLE' not in adb('shell','dumpsys package '+package)
    command='am start -n '+activity+' --es probe_run_id '+run
    if args.api32_layout:command+=' --ez api32_layout true'
    if args.checkpoint_migration:command+=' --ez checkpoint_migration true'
    if args.logical_hardlinks:command+=' --ez source_hardlink false'
    adb('shell',command)
    deadline=time.monotonic()+600;report=None;errors=[]
    while time.monotonic()<deadline:
        try:
            text=adb('shell','dumpsys activity '+activity+' pocket-ce-fixture')
            rows=[line.strip().split('=',1)[1] for line in text.splitlines() if line.strip().startswith('POCKET_CE_FIXTURE=')]
            if rows:
                value=json.loads(base64.b64decode(rows[-1],validate=True))
                if value.get('runId')==run and value.get('validation') in ['PASS','FAIL']:report=value;break
        except Exception as error:errors.append(str(error))
        time.sleep(1)
    if report is None:
        save('fixture-observation-timeout.json',dict(launch,status='OBSERVATION_TIMED_OUT_TEST_MAY_STILL_BE_RUNNING',observationErrors=errors))
        raise RuntimeError('Observation timed out; preserve this fixture and inspect the same run before further action')
    save('fixture-result.json',report)
    with (work/'host-policy-after.txt').open('x') as out:out.write(policy())
    ps=stopped()
    with (work/'post-test-processes.txt').open('x') as out:out.write(ps)
    assert report['validation']=='PASS',report.get('error')
    assert report['uid']>=10000 and report['api32Layout']==args.api32_layout
    rows=[row.split(None,4) for row in ps.splitlines()[1:]]
    own=[row for row in rows if len(row)==5 and row[2] in [str(report['uid']),'u0_a'+str(report['uid']-10000)]]
    assert all(row[3]==package for row in own),'Fixture native children remain'
    for key in ['readDuringBlockedSync','migration','locked','metadata','procFdReopen','removedDirectory','native','metadataReopened','reopened','credentialRemovalAndRestoration']:assert key in report
    assert 'CE_PROC_REOPEN_PASS actual_appfuse=1 removed_inode=1 replacement_not_read=1 independent_offset=1 current_uid_permission=1 parent_search_not_inherited=1' in report['procFdReopen']
    assert 'CE_REMOVED_DIRECTORY_PASS descriptor_search=1 current_uid_permission=1 cwd_enoent=1 new_child_enoent=1 proc_reopen_same_inode=1 empty_readdir=1' in report['removedDirectory']
    if args.api32_layout:
        assert report['ceRoots']==5 and report['layoutMarker'] and report['vendorPlaintextRemoved']
        assert all(key in report for key in ['vendorLocked','vendorNative','vendorReopened'])
    if args.checkpoint_migration:
        proof=report['checkpointMigration'];assert proof['bytesVerified']==17*1024*1024+13 and proof['logicalHardlinkVerified'] and proof['obsoleteCiphertextObjects']==0
        assert proof['sourceHardlinkPresentBeforeMigration']==proof['sourceHardlinkPreserved']==(not args.logical_hardlinks)
    receipt=dict(launch,validation='PASS',hostDefault32EnforcingUnchanged=True,nativeChildrenGone=True,existingProfilesTouched=False,androidFrameworkCredentialsChanged=False,observationErrors=errors)
    save('fixture-check.json',receipt);print(json.dumps(receipt),flush=True)


if __name__=='__main__':main()
