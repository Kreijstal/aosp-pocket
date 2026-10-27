#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Test guest certificate replacement with disposable data; preserve original inodes.

Requires exclusive installs/restarts on the dedicated Pi VM. Private keys are
never sent to the Pi. A private full data archive and recovery journal are kept
before any data movement. finally restores the original directories and boots
the accepted image, or the supplied higher-version rollback image on failure.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tarfile
import time
import xml.etree.ElementTree as ET

PACKAGE='dev.kreijstal.aosppocket.runtime'
ROOT='/data/user/0/'+PACKAGE+'/files/api28-native-rootfs'


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ['base','candidate','rollback']:
        p.add_argument('--'+name+'-apk',type=Path,required=True)
        p.add_argument('--'+name+'-sha256',required=True)
        p.add_argument('--'+name+'-version',required=True)
    p.add_argument('--probe',type=Path,required=True)
    p.add_argument('--owner',type=Path,required=True)
    p.add_argument('--old-key-apk',type=Path,required=True)
    p.add_argument('--new-key-apk',type=Path,required=True)
    p.add_argument('--reuse-backup',type=Path,help='Private snapshot from this same test attempt; vault bytes and package identities must still match')
    p.add_argument('--resume',action='store_true',help='Resume an installed candidate while all original data remains preserved')
    args=p.parse_args()
    sys.path[:0]=[str(Path.home()),str(Path.home()/'lineage-cuttlefish/tools')]
    import cuttlefish_arm64 as c
    from pocketctl import request
    c.require_virtual()
    cfg=json.loads((Path.home()/'.config/pocket-pi-debug.json').read_text())
    report={'scope':'dedicated Pi VM; disposable guest data; original directories preserved',
            'validation':'IN_PROGRESS'}
    journal=Path.home()/'.config/pocket-resigning-transaction.json'
    if journal.exists():
        old=json.loads(journal.read_text())
        if args.resume:
            assert old.get('originalDataRestored') is False
            report=old
        else:
            assert old.get('originalDataRestored') is True,'Resolve the existing recovery journal before a new transaction'
    elif args.resume:raise RuntimeError('No recovery journal to resume')
    def save(path,data):
        tmp=path.with_suffix('.part')
        with os.fdopen(os.open(tmp,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600),'w') as f:
            json.dump(data,f,indent=2);f.write('\n');f.flush();os.fsync(f.fileno())
        os.replace(tmp,path)
    def progress(message):print(message,flush=True)
    def host(command,timeout=60):return c.shell('su -c '+shlex.quote(command),timeout=timeout)
    def rpc(op,argv=(),check=True):
        for attempt in range(30):
            out,err=io.BytesIO(),io.BytesIO()
            try:
                rc=request(cfg,op,argv,stdout=out,stderr=err);break
            except OSError:
                # Package replacement restarts the foreground admin service.
                # Never replay an exec operation: it may already have mutated data.
                if op not in (1,3,4) or attempt==29:raise
                time.sleep(1)
        if check and rc:raise RuntimeError((rc,out.getvalue().decode(),err.getvalue().decode()))
        return rc,out.getvalue().decode(),err.getvalue().decode()
    def version():
        return re.search(r'versionName=(\S+)',c.shell('dumpsys package '+PACKAGE))[1]
    def installed_hash():
        apk=c.shell('pm path '+PACKAGE).strip().removeprefix('package:')
        return host('sha256sum '+shlex.quote(apk),timeout=180).split()[0]
    def stopped():
        rpc(4)
        for i in range(120):
            state=json.loads(rpc(1)[1])
            if not any(state[k] for k in ['running','starting','stopping']):return
            time.sleep(1)
        raise RuntimeError('Guest did not stop normally; original data must stay preserved')
    def boot(expected):
        assert version()==expected,'Unexpected shared Manager update'
        c.shell('su -c '+shlex.quote('am start-foreground-service -n '+PACKAGE+'/.GuestService -a dev.kreijstal.aosppocket.ADMIN'))
        c.shell('am start -n '+PACKAGE+'/.MainActivity --ei guest_sdk 28 --ez start_guest true')
        attempts=1
        for i in range(300):
            state=json.loads(rpc(1)[1])
            if i%15==0:progress('BOOT '+expected+' '+state['status'])
            if state['running'] and state['ready'] and rpc(2,['/system/bin/getprop','sys.boot_completed'])[1].strip()=='1':
                assert version()==expected;return
            if 'failed:' in state['status'] and not any(state[k] for k in ['running','starting','stopping']):
                if attempts>=2:raise RuntimeError(state['status'])
                attempts+=1;rpc(3)
            time.sleep(2)
        raise RuntimeError('Guest boot timed out')
    def install(kind,expected):
        assert version()==expected,'Another session changed the shared guest'
        assert not any(json.loads(rpc(1)[1])[k] for k in ['running','starting','stopping'])
        # The module view is entirely generated, never the persistent modules.
        host('rm -rf '+ROOT+'/.pocket-module-view',timeout=180)
        previous=c.shell('settings get global sys_storage_threshold_percentage')
        try:
            c.shell('settings put global sys_storage_threshold_percentage 0')
            r=c.adb('-s',c.serial(),'install','--streaming','-r',str(getattr(args,kind+'_apk')),check=False,timeout=360)
            progress('INSTALL '+kind+' '+str(r.returncode)+' '+r.stdout.strip());assert r.returncode==0,r.stderr
        finally:
            c.shell('settings delete global sys_storage_threshold_percentage' if previous=='null' else
                    'settings put global sys_storage_threshold_percentage '+previous)
        assert version()==getattr(args,kind+'_version')
        assert installed_hash()==getattr(args,kind+'_sha256')
    def names():
        all_names=host('ls -A -1 '+ROOT).splitlines()
        return sorted(n for n in all_names if n in ['system-data','.pocket-owner'] or n.startswith('.pocket-ce'))
    def hashes(directory):
        paths=['system-data/system/packages.xml','.pocket-ce-v1/key','.pocket-ce-v1/state']
        return {n:host('sha256sum '+shlex.quote(directory+'/'+n)).split()[0] for n in paths}
    def stage():
        for source,name in [(args.probe,'guest-signing-probe.jar'),(args.owner,'microg-owner.jar'),
                            (args.old_key_apk,'old-platform-key.apk'),(args.new_key_apk,'new-platform-key.apk')]:
            dest='/data/local/tmp/resign-'+name
            c.adb('-s',c.serial(),'push',str(source),dest,check=True,timeout=90)
            physical=ROOT+'/system-data/local/tmp/'+name
            host('mkdir -p '+ROOT+'/system-data/local/tmp && cp '+dest+' '+physical+' && chmod 0644 '+physical+' && chown '+str(uid)+':'+str(uid)+' '+physical)
    def signing(*argv):
        command='export CLASSPATH=/system/framework/pocket-zygote.jar:/data/local/tmp/guest-signing-probe.jar; exec /system/bin/pocket-app-process-api28 /system/bin GuestSigningProbe '
        result=rpc(2,['/system/bin/toybox','timeout','120','/system/bin/sh','-c',command+' '.join(map(shlex.quote,argv))])
        progress(result[1]);return result
    def install_fixture(path):
        command='export CLASSPATH=/system/framework/pocket-zygote.jar:/data/local/tmp/microg-owner.jar; exec /system/bin/pocket-app-process-api28 /system/bin MicrogOwner --install '+shlex.quote(path)
        return rpc(2,['/system/bin/sh','-c',command],check=False)

    for kind in ['base','candidate','rollback']:
        with getattr(args,kind+'_apk').open('rb') as f:
            assert hashlib.file_digest(f,'sha256').hexdigest()==getattr(args,kind+'_sha256'),kind+' APK differs'
    assert int(args.rollback_version.split('.')[-1])>int(args.candidate_version.split('.')[-1])>int(args.base_version.split('.')[-1])
    if args.resume:
        assert version()==args.candidate_version and installed_hash()==args.candidate_sha256
        assert report['baseVersion']==args.base_version and report['candidateVersion']==args.candidate_version
        uid=int(re.search(r'uid:(\d+)',c.shell('cmd package list packages -U '+PACKAGE))[1])
        preserved=report['preservedDirectory'];fixture_archive=report['fixtureArchive'];original_names=report['originalNames']
        parent='/data/user/0/'+PACKAGE+'/files'
        assert re.fullmatch(re.escape(parent)+r'/resign-preserved-\d+',preserved)
        assert re.fullmatch(re.escape(parent)+r'/resign-fixture-\d+',fixture_archive)
        for name in original_names:
            assert name in ['system-data','.pocket-owner'] or (name.startswith('.pocket-ce') and '/' not in name)
            assert host('test -e '+shlex.quote(preserved+'/'+name)+'; echo $?').strip()=='0','Original directory already moved; finish recovery first'
        original_hashes=hashes(preserved)
        assert host('test -f '+ROOT+'/system-data/local/tmp/signing-before.json; echo $?').strip()=='0','No seeded disposable data to resume'
        progress('RESUMING installed candidate with original data preserved')
    else:
        assert version()==args.base_version and installed_hash()==args.base_sha256
        uid=int(re.search(r'uid:(\d+)',c.shell('cmd package list packages -U '+PACKAGE))[1])
        stopped();original_names=names();original_hashes=hashes(ROOT)
        stamp=str(time.time_ns());parent='/data/user/0/'+PACKAGE+'/files'
        preserved=parent+'/resign-preserved-'+stamp;fixture_archive=parent+'/resign-fixture-'+stamp
        backup=args.reuse_backup or Path.home()/'pocket-backups'/('guest-data-before-resigning-'+stamp+'.tar.gz')
        backup.parent.mkdir(exist_ok=True)
        if args.reuse_backup:
            assert backup.resolve().is_relative_to((Path.home()/'pocket-backups').resolve())
            assert backup.stat().st_mode&0o077==0 and not backup.name.endswith('.incomplete')
            progress('Reusing the private snapshot from this test attempt')
        else:
            progress('Creating private guest-data backup; original data remains in place')
            adb=[str(c.BASE/'host/bin/adb'),'-L','tcp:'+str(c.lock()['adb_server_port']),'-s',c.serial()]
            # Unix sockets are transient endpoints, not persistent data. Toybox tar
            # reports them as errors; retain them in the preserved directory tree but
            # exclude them from the rollback archive.
            sockets=host('find '+ROOT+'/system-data -type s',timeout=180).splitlines()
            exclusions=[]
            for path in sockets:
                assert path.startswith(ROOT+'/')
                exclusions.append('--exclude='+path[len(ROOT)+1:])
            # Compression also coalesces tar's small writes before the ADB stream.
            # pipefail prevents a failed tar from being mistaken for a valid backup.
            # exec-out merges remote stderr into stdout: redirect tar diagnostics
            # inside the VM, or a harmless socket warning corrupts the binary archive.
            diagnostics='/data/local/tmp/pocket-resign-backup-'+stamp+'.err'
            exit_file=diagnostics+'.exit'
            command='umask 077; set -o pipefail; tar -C '+shlex.quote(ROOT)+' -cf - '+' '.join(map(shlex.quote,exclusions+original_names))+' 2>'+diagnostics+' | gzip -1; status=$?; echo "$status" >'+exit_file+'; exit "$status"'
            try:
                with os.fdopen(os.open(backup,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600),'wb') as output:
                    result=subprocess.run(adb+['exec-out','su -c '+shlex.quote(command)],stdout=output,stderr=subprocess.PIPE,timeout=3600)
                    output.flush();os.fsync(output.fileno())
            except BaseException:
                backup.rename(str(backup)+'.incomplete');boot(args.base_version);raise
            # exec-out does not propagate the remote exit status (unlike shell-v2).
            remote_exit=host('cat '+exit_file).strip()
            if result.returncode or remote_exit!='0':
                backup.rename(str(backup)+'.incomplete');boot(args.base_version)
                raise RuntimeError('Full guest-data backup failed; original data unchanged')
        subprocess.run(['gzip','-t',str(backup)],check=True,timeout=600)
        with tarfile.open(backup) as archive:
            for name,digest in original_hashes.items():
                data=archive.extractfile(name).read()
                if args.reuse_backup and name=='system-data/system/packages.xml':
                    def identities(xml):
                        return {p.get('name'):(p.get('userId'),p.get('sharedUserId'),p.get('version'),p.get('codePath')) for p in ET.fromstring(xml).findall('package')}
                    assert identities(data)==identities(host('cat '+ROOT+'/'+name)),'Installed package identities changed since the snapshot'
                else:
                    assert hashlib.sha256(data).hexdigest()==digest,'Backup verification failed: '+name
        progress('BACKUP_VERIFIED '+str(backup.stat().st_size)+' bytes')
        assert version()==args.base_version and installed_hash()==args.base_sha256
        assert hashes(ROOT)==original_hashes,'Data changed while stopped'
        report.update(baseVersion=args.base_version,candidateVersion=args.candidate_version,
                      rollbackVersion=args.rollback_version,backup=str(backup),preservedDirectory=preserved,
                      fixtureArchive=fixture_archive,originalNames=original_names,originalDataRestored=False)
        save(journal,report)
    passed=False;failure=None
    try:
        if not args.resume:
            host('mkdir -m 0700 '+preserved+' && chown '+str(uid)+':'+str(uid)+' '+preserved)
            for name in original_names:
                host('mv '+shlex.quote(ROOT+'/'+name)+' '+shlex.quote(preserved+'/'+name))
            save(journal,{**report,'phase':'disposable baseline'})
            boot(args.base_version);stage()
            signing('seed','/data/local/tmp/signing-before.json')
            stopped();install('candidate',args.base_version)
        boot(args.candidate_version)
        signing('verify','/data/local/tmp/signing-before.json','/data/local/tmp/signing-after.json')
        report['upgrade']=json.loads(rpc(2,['/system/bin/toybox','cat','/data/local/tmp/signing-after.json'])[1])
        bad=install_fixture('/data/local/tmp/old-platform-key.apk')
        assert bad[0]!=0 and any(s in (bad[1]+bad[2]).lower() for s in ['shared user','shared_user','signatures','certificates']),bad
        report['publicTestPlatformKeyRejected']=True
        good=install_fixture('/data/local/tmp/new-platform-key.apk');assert good[0]==0,good
        signing('inspect','/data/local/tmp/signing-inspect.json')
        inspected=json.loads(rpc(2,['/system/bin/toybox','cat','/data/local/tmp/signing-inspect.json'])[1])
        assert inspected['packages']['dev.kreijstal.pocketresign.newkey']['uid']==1000
        report['privatePlatformKeyAcceptedForSystemUid']=True
        passed=True;progress('DISPOSABLE_UPGRADE_PASS')
    except BaseException as error:
        failure=error;report['fixtureError']=str(error);progress('Fixture failed; restoring original guest')
    finally:
        # A fresh-data guest may be force-stopped only while original data is
        # quarantined. Normal shutdown remains the first choice.
        try:stopped()
        except Exception:
            c.shell('am force-stop '+PACKAGE);time.sleep(3)
            assert not any(line.split()[0]==str(uid) for line in
                           c.shell('ps -A -o UID,PID,NAME').splitlines()[1:] if line.split()),'Disposable guest processes survived force-stop'
            c.shell('su -c '+shlex.quote('am start-foreground-service -n '+PACKAGE+'/.GuestService -a dev.kreijstal.aosppocket.ADMIN'))
        if not passed and version()!=args.base_version:
            install('rollback',version())
            stopped()
        host('mkdir -m 0700 '+fixture_archive+' && chown '+str(uid)+':'+str(uid)+' '+fixture_archive)
        for name in names():
            # If preservation failed halfway, leave untouched originals alone.
            if name in original_names and host('test -e '+shlex.quote(preserved+'/'+name)+'; echo $?').strip()!='0':continue
            host('mv '+shlex.quote(ROOT+'/'+name)+' '+shlex.quote(fixture_archive+'/'+name))
        for name in original_names:
            if host('test -e '+shlex.quote(preserved+'/'+name)+'; echo $?').strip()=='0':
                host('mv '+shlex.quote(preserved+'/'+name)+' '+shlex.quote(ROOT+'/'+name))
        assert hashes(ROOT)==original_hashes,'Restored data differs; retain recovery journal and backup'
        report['originalDataRestored']=True;save(journal,report)
        expected=version()
        try:boot(expected)
        except Exception:
            if expected==args.candidate_version:
                stopped();install('rollback',expected);boot(args.rollback_version)
                passed=False;failure=RuntimeError('Original guest failed signed-image boot; rollback restored')
            else:raise
    report['installedVersion']=version();report['validation']='PASS' if passed else 'FAIL'
    save(journal,report);save(Path.home()/'guest-resigning-validation.json',report)
    progress('FINAL '+report['validation']+' originalDataRestored='+str(report['originalDataRestored'])+' version='+version())
    if failure is not None:raise failure


if __name__=='__main__':main()
