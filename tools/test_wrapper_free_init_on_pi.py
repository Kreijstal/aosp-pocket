#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise Android 9 init with PRoot and no Bionic preload, in a private root."""
import argparse
import io
import json
from pathlib import Path
import shlex
import sys
import struct
import time
import zipfile

sys.path.insert(0,'/home/pi/lineage-cuttlefish/tools')
import cuttlefish_arm64 as c
from pocketctl import request

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--framework',action='store_true')
    parser.add_argument('--timeout',type=int)
    args=parser.parse_args()
    seconds=args.timeout or (90 if args.framework else 25)
    c.require_virtual()
    config=json.loads((Path.home()/'.config/pocket-pi-debug.json').read_text())
    def invoke(argv,source=None,stdout=None,stderr=None):
        out=stdout if stdout is not None else io.BytesIO()
        err=stderr if stderr is not None else io.BytesIO()
        rc=request(config,2,argv,source=source,stdout=out,stderr=err)
        return rc,out,err
    def command(*argv,source=None):
        rc,out,err=invoke(list(argv),source)
        if rc:raise RuntimeError((rc,out.getvalue().decode(errors='replace'),err.getvalue().decode(errors='replace')))
        return out.getvalue()
    def host(command):
        result=c.adb('-s',c.serial(),'shell','-T','su -c '+shlex.quote(command),timeout=90,check=False)
        if result.returncode:raise RuntimeError(result.stdout+result.stderr)
        return result.stdout
    if command('/system/bin/sh','-c','echo "$POCKET_SYSCALL_BACKEND"').strip()==b'proot':
        raise RuntimeError('Private-launcher test requires the legacy untraced 0.32 owner console; on 0.33 use test_wrapper_free_manager_on_pi.py and test_proot_owner_on_pi.py to test the installed backend without nesting tracers')
    base='/data/user/0/dev.kreijstal.aosppocket.runtime/files'
    original=base+'/api28-rootfs'
    root=base+'/syscall-rootfs'
    if args.framework:
        out,err=io.BytesIO(),io.BytesIO()
        request(config,4,[],stdout=out,stderr=err)
        for _ in range(40):
            status_out,status_err=io.BytesIO(),io.BytesIO()
            request(config,1,[],stdout=status_out,stderr=status_err)
            state=json.loads(status_out.getvalue())
            if not(state['running'] or state['starting'] or state['stopping']):break
            time.sleep(1)
        else:raise RuntimeError('Existing guest did not stop before private Binder test')
    # Root is used only to prepare a sibling fixture. The execution itself is
    # through the app's paired console under its ordinary kernel UID.
    setup=f'''set -eu
test ! -L {root}
mkdir -p {root}
if [ ! -d {root}/system ]; then cp -a {original}/system {root}/system; fi
if [ ! -d {root}/vendor ]; then cp -a {original}/vendor {root}/vendor; fi
if [ ! -d {root}/system-data ]; then
    if [ -d {root}/data ]; then mv {root}/data {root}/system-data; else mkdir {root}/system-data; fi
fi
mkdir -p {root}/dev/socket {root}/dev/__properties__ {root}/dev/input {root}/data {root}/tmp {root}/logs
chown 10177:10177 {root} {root}/dev {root}/dev/socket {root}/dev/__properties__ {root}/dev/input {root}/data {root}/system-data {root}/tmp {root}/logs
rm -f {root}/dev/__properties__/* {root}/dev/socket/*
'''
    host(setup)
    rc_text='''on boot
    setprop pocket.wrapperfree.init ready
    start pocket-wrapperfree-id

service pocket-wrapperfree-id /system/bin/toybox id
    user 39999
    group 39999
    oneshot
'''
    # Only private copies and new standalone files are written.
    host('printf %s '+shlex.quote(rc_text)+' > '+root+'/init.wrapperfree.rc\nchown 10177:10177 '+root+'/init.wrapperfree.rc')
    stage='/data/local/tmp/pocket-bionic-mode'
    launcher=stage+'/syscall-guest-launcher'
    command('/system/bin/toybox','mkdir','-p',stage)
    for native_name in ('syscall-guest-launcher','libproot.so'):
      with (Path.home()/'bionic-mode'/native_name).open('rb') as source:
        command('/system/bin/sh','-c','/system/bin/toybox cat > "$1.next" && /system/bin/toybox chmod 755 "$1.next" && /system/bin/toybox mv "$1.next" "$1"',
            'upload',stage+'/'+native_name,source=source)
    backing=original+'/system-data/local/tmp/pocket-bionic-mode'
    # Every private executable uses a guest-relative interpreter. Never bind
    # the original root into the test guest's app-data namespace.
    with zipfile.ZipFile(Path.home()/'aosp-pocket-manager-0.32.apk') as apk:
        for name in apk.namelist():
            prefix='assets/api28-rootfs/system/bin/'
            if not(name.startswith(prefix) and name.endswith('.payload')):continue
            data=bytearray(apk.read(name))
            if data[:4]!=b'\x7fELF':continue
            phoff=struct.unpack_from('<Q',data,32)[0]
            stride,count=struct.unpack_from('<HH',data,54)
            for i in range(count):
                ph=struct.unpack_from('<IIQQQQQQ',data,phoff+i*stride)
                if ph[0]==3:
                    interpreter=b'/system/bin/linker64\0'
                    assert len(interpreter)<=ph[5]
                    data[ph[2]:ph[2]+ph[5]]=interpreter.ljust(ph[5],b'\0');break
            else:continue
            filename=name[len(prefix):-len('.payload')]
            command('/system/bin/sh','-c','/system/bin/toybox cat > \"$1.next\" && /system/bin/toybox mv \"$1.next\" \"$1\"',
                'upload',stage+'/private-bin',source=io.BytesIO(data))
            target=root+'/system/bin/'+filename
            host('cp '+backing+'/private-bin '+target+'.next\nchmod 755 '+target+'.next\nchown 10177:10177 '+target+'.next\nmv -f '+target+'.next '+target)
    linker_config='dir.pocket = /\n[pocket]\nnamespace.default.isolated = false\nnamespace.default.search.paths = /system/lib64:/vendor/lib64\nnamespace.default.permitted.paths = /\n'
    host('printf %s '+shlex.quote(linker_config)+' > '+root+'/system/etc/ld.config.txt.next\nchown 10177:10177 '+root+'/system/etc/ld.config.txt.next\nmv -f '+root+'/system/etc/ld.config.txt.next '+root+'/system/etc/ld.config.txt')
    canary=original+'/pocket-isolation-canary'
    host('printf %s isolated-host-canary > '+canary+'\nchown 10177:10177 '+canary)
    if args.framework:
        jar=Path.home()/'bionic-mode/pocket-control.jar'
        with jar.open('rb') as source:
            command('/system/bin/sh','-c','/system/bin/toybox cat > "$1.next" && /system/bin/toybox mv "$1.next" "$1"',
                'upload',stage+'/pocket-control.jar',source=source)
        target=root+'/system/framework/pocket-zygote.jar'
        host('cp '+backing+'/pocket-control.jar '+target+'.next\nchown 10177:10177 '+target+'.next\nmv -f '+target+'.next '+target)
        for name in ('pocket-boot-probe','libpocket_hidl_api28.so','libpocket_zygote_api28.so'):
            data=bytearray((Path.home()/'bionic-mode'/name).read_bytes())
            if name=='pocket-boot-probe':
                phoff=struct.unpack_from('<Q',data,32)[0]
                phsize,phnum=struct.unpack_from('<HH',data,54)
                for i in range(phnum):
                    ph=struct.unpack_from('<IIQQQQQQ',data,phoff+i*phsize)
                    if ph[0]==3:
                        interpreter=b'/system/bin/linker64\0'
                        assert len(interpreter)<=ph[5]
                        data[ph[2]:ph[2]+ph[5]]=interpreter.ljust(ph[5],b'\0');break
            command('/system/bin/sh','-c','/system/bin/toybox cat > "$1.next" && /system/bin/toybox chmod 755 "$1.next" && /system/bin/toybox mv "$1.next" "$1"',
                'upload',stage+'/'+name,source=io.BytesIO(data))
            target=root+('/system/bin/' if name=='pocket-boot-probe' else '/system/lib64/')+name
            host('cp '+backing+'/'+name+' '+target+'.next\nchown 10177:10177 '+target+'.next\nmv -f '+target+'.next '+target)
        for name in ('host-api','host-token-position','activityServiceRequest','localBroadcastIntent','binderBroadcastIntent','trans_code'):
            host('cp '+original+'/'+name+' '+root+'/'+name+'\nchown 10177:10177 '+root+'/'+name)
        full=host('cat '+original+'/system/etc/init/init.manager.rc')
        full=full.replace('import /system/etc/init/init.magisk.rc','')
        full=full.replace('# Optional runtime controls data/zygote ordering',
            'on property:pocket.data.ready=1\n    class_start pocket-zygote')
        full=full.replace('setenv LD_PRELOAD /system/lib64/libpocket_binder_api28.so',
            'setenv LD_PRELOAD /system/lib64/libpocket_binder_api28.so:/system/lib64/libpocket_hidl_api28.so')
        host('printf %s '+shlex.quote(full)+' > '+root+'/init.wrapperfree.rc')
    log_path=Path.home()/('pocket-wrapper-free-framework.log' if args.framework else 'pocket-wrapper-free-init.log')
    argv=[launcher,root,backing]
    if args.framework:argv+=['--log',root+'/logs/framework-test.log']
    argv+=['/system/bin/toybox','timeout',str(seconds)]
    if args.framework:argv+=['/system/bin/toybox','env','LD_PRELOAD=/system/lib64/libpocket_binder_api28.so:/system/lib64/libpocket_hidl_api28.so']
    argv+=['/system/bin/pocket-init-api28','/init.wrapperfree.rc']
    with log_path.open('wb') as log:rc,_,_=invoke(argv,stdout=log,stderr=log)
    if args.framework:log_path.write_text(host('tail -c 1048576 '+root+'/logs/framework-test.log'))
    assert host('cat '+canary).strip()=='isolated-host-canary', 'Original guest canary changed'
    host('test -f '+original+'/system/bin/sh')
    text=log_path.read_text(errors='replace')
    useful=[line for line in text.splitlines() if any(key in line for key in ('READY','FAIL','Fatal','fatal','Error','error','PREPARED','INIT_REAP','INIT_STOPPED','Starting service')) and not line.startswith('proot error:')]
    print('\n'.join(useful[-60:]),flush=True)
    passed=('API28_INIT_READY' in text and 'uid=39999' in text and 'API28_PROPERTIES_READY' in text
        and 'DIRECT_BOOT_READY' not in text and rc in (0,124)
        and 'remaining=0 failed=0' in text)
    if args.framework:passed='POCKET_CONTROL_READY' in text and 'DIRECT_BOOT_READY' not in text
    report={'command':'python3 /home/pi/test_wrapper_free_init_on_pi.py'+(' --framework' if args.framework else '')+' --timeout '+str(seconds),'exitCode':0 if passed else 1,
        'processExitCode':rc,'separateRoot':root,'bionicPreload':False,
        'initReady':'API28_INIT_READY' in text,'virtualServiceUid39999':'uid=39999' in text,
        'scope':'Real Android 9 framework reaches Pocket control service; does not establish full peripheral compatibility' if args.framework else 'Real init/property/service fork+exec; does not establish full Android framework boot'}
    (Path.home()/('pocket-wrapper-free-framework-validation.json' if args.framework else 'pocket-wrapper-free-init-validation.json')).write_text(json.dumps(report,indent=2)+'\n')
    assert passed,report
    print('PASS: real Android 9 init, private properties and UID-changing child exec without Bionic wrappers')

if __name__=='__main__':main()
