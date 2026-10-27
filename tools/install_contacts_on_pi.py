#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Install a verified Contacts update on the dedicated Pi VM, preserving guest data."""
import argparse
import base64
import hashlib
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tarfile
import time

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--apk',type=Path,required=True);p.add_argument('--sha256',required=True)
    p.add_argument('--version',required=True);p.add_argument('--base-version',required=True)
    p.add_argument('--reuse-backup',type=Path,help='Reuse this private pre-change rollback snapshot after a concurrent-update retry')
    args=p.parse_args()
    sys.path[:0]=[str(Path.home()),str(Path.home()/'lineage-cuttlefish/tools')]
    import cuttlefish_arm64 as c
    from pocketctl import request
    c.require_virtual();cfg=json.loads((Path.home()/'.config/pocket-pi-debug.json').read_text())
    package='dev.kreijstal.aosppocket.runtime'
    root='/data/user/0/'+package+'/files/api28-native-rootfs'
    def rpc(op,argv=()):
        out,err=io.BytesIO(),io.BytesIO();rc=request(cfg,op,argv,stdout=out,stderr=err)
        if rc:raise RuntimeError('Pocket RPC failed: '+str(rc))
        return out.getvalue().decode()
    def version():
        import re
        return re.search(r'versionName=(\S+)',c.shell('dumpsys package '+package))[1]
    assert version()==args.base_version,'Shared guest APK changed; rebase before installing'
    state=json.loads(rpc(1));assert not state['running'] and not state['starting'] and not state['stopping'],'Stop the guest normally first'
    with args.apk.open('rb') as f:assert hashlib.file_digest(f,'sha256').hexdigest()==args.sha256,'APK checksum mismatch'
    backup_dir=Path.home()/'pocket-backups';backup_dir.mkdir(exist_ok=True)
    backup=args.reuse_backup or backup_dir/('ce-vault-before-contacts-'+args.version+'-'+str(time.time_ns())+'.tar')
    if args.reuse_backup:
        assert backup.stat().st_mode&0o077==0,'Vault backup must remain private'
        current_key=base64.b64decode(c.shell('su -c '+shlex.quote('base64 '+root+'/.pocket-ce-v1/key')))
        with tarfile.open(backup) as archive:assert archive.extractfile('.pocket-ce-v1/key').read()==current_key,'Backup belongs to a different vault key'
    else:
        adb=[str(c.BASE/'host/bin/adb'),'-L','tcp:'+str(c.lock()['adb_server_port']),'-s',c.serial()]
        command='tar -C '+shlex.quote(root)+' -cf - .pocket-ce-v1 .pocket-ce-enabled .pocket-ce-root-xattrs'
        with os.fdopen(os.open(backup,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600),'wb') as f:
            result=subprocess.run(adb+['exec-out','su -c '+shlex.quote(command)],stdout=f,stderr=subprocess.PIPE,timeout=240)
            f.flush();os.fsync(f.fileno())
        if result.returncode:
            backup.rename(str(backup)+'.incomplete');raise RuntimeError('Encrypted vault backup failed')
    with tarfile.open(backup) as archive:
        members={n.name for n in archive};assert '.pocket-ce-v1/state' in members and '.pocket-ce-v1/key' in members
    print('BACKUP_OK',backup.name,backup.stat().st_size,flush=True)
    free=int(c.shell('df -k /data').splitlines()[-1].split()[3])
    if free<1250000:
        c.shell('su -c '+shlex.quote('rm -rf '+root+'/.pocket-module-view'),timeout=90)
        print('Cleared generated module view for install space',flush=True)
    assert version()==args.base_version,'Another session updated the guest; rebase before installing'
    previous=c.shell('settings get global sys_storage_threshold_percentage')
    try:
        c.shell('settings put global sys_storage_threshold_percentage 0')
        result=c.adb('-s',c.serial(),'install','--streaming','-r',str(args.apk),check=False,timeout=360)
        print('INSTALL',result.returncode,result.stdout,result.stderr,flush=True);assert result.returncode==0
    finally:
        c.shell('settings delete global sys_storage_threshold_percentage' if previous=='null' else 'settings put global sys_storage_threshold_percentage '+previous)
    assert version()==args.version,'Installed version changed during update'
    c.shell('su -c '+shlex.quote('am start-foreground-service -n '+package+'/.GuestService -a dev.kreijstal.aosppocket.ADMIN'))
    c.shell('am start -n '+package+'/.MainActivity --ei guest_sdk 28 --ez start_guest true')
    attempts=1
    for i in range(300):
        try:
            state=json.loads(rpc(1))
            if i%15==0:print('BOOT',state['status'],flush=True)
            if state['running'] and state['ready'] and rpc(2,['/system/bin/getprop','sys.boot_completed']).strip()=='1':
                print('BOOT_COMPLETE attempts='+str(attempts),flush=True);return
            if 'failed:' in state['status'] and not state['running'] and not state['starting'] and not state['stopping']:
                if attempts>=2:raise RuntimeError(state['status'])
                attempts+=1;print('Retrying cold boot after startup timeout',flush=True);rpc(3)
        except (OSError,ValueError):pass
        time.sleep(2)
    raise RuntimeError('Guest did not complete boot')

if __name__=='__main__':main()
