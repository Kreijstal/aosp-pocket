#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate real syscall-backend ownership, credentials, timers and failed exec."""
import io
import json
from pathlib import Path
import struct
import shlex
import sys
import threading
import time
import re

def main():
    sys.path.insert(0,'/home/pi/lineage-cuttlefish/tools')
    import cuttlefish_arm64 as c
    from pocketctl import request
    c.require_virtual()
    config=json.loads((Path.home()/'.config/pocket-pi-debug.json').read_text())
    def call(argv,source=None):
        out,err=io.BytesIO(),io.BytesIO()
        rc=request(config,2,argv,source=source,stdout=out,stderr=err)
        if rc:raise RuntimeError((rc,out.getvalue().decode(errors='replace'),err.getvalue().decode(errors='replace')))
        return out.getvalue().decode(errors='replace')
    original='/data/user/0/dev.kreijstal.aosppocket.runtime/files/api28-rootfs'
    root='/data/user/0/dev.kreijstal.aosppocket.runtime/files/syscall-rootfs'
    installed=call(['/system/bin/sh','-c','echo "$POCKET_SYSCALL_BACKEND"']).strip()=='proot'
    if installed:root=original
    stage='/data/local/tmp/pocket-bionic-mode'
    backing=original+'/system-data/local/tmp/pocket-bionic-mode'
    for name in ('libproot.so','syscall-guest-launcher','proot-owner-probe','bionic-mode-probe'):
        data=bytearray((Path.home()/'bionic-mode'/name).read_bytes())
        if name in ('proot-owner-probe','bionic-mode-probe'):
            phoff=struct.unpack_from('<Q',data,32)[0]
            stride,count=struct.unpack_from('<HH',data,54)
            for i in range(count):
                ph=struct.unpack_from('<IIQQQQQQ',data,phoff+i*stride)
                if ph[0]!=3:continue
                interpreter=b'/system/bin/linker64\0'
                assert len(interpreter)<=ph[5]
                data[ph[2]:ph[2]+ph[5]]=interpreter.ljust(ph[5],b'\0');break
            else:raise AssertionError('No reserved interpreter')
        call(['/system/bin/sh','-c','/system/bin/toybox cat > "$1.next" && /system/bin/toybox chmod 755 "$1.next" && /system/bin/toybox mv "$1.next" "$1"',
            'upload',stage+'/'+name],source=io.BytesIO(data))
    def host(command):
        result=c.adb('-s',c.serial(),'shell','-T','su -c '+shlex.quote(command),check=False)
        assert result.returncode==0,result.stderr
        return result.stdout
    for name in ('proot-owner-probe','bionic-mode-probe'):
        target=root+'/system/bin/'+name
        host('cp '+backing+'/'+name+' '+target+'.next\nchown 10177:10177 '+target+'.next\nmv -f '+target+'.next '+target)
    host('mkdir -p '+root+'/pocket-bionic-probe\nchown 10177:10177 '+root+'/pocket-bionic-probe')
    host('printf "bionic-mode-fixture\\n" > '+root+'/pocket-bionic-probe/fixture.txt\nchown 10177:10177 '+root+'/pocket-bionic-probe/fixture.txt')
    def guest(argv):return argv if installed else [stage+'/syscall-guest-launcher',root,backing]+argv
    bionic=json.loads(call(guest(['/system/bin/bionic-mode-probe','syscall',
        '/pocket-bionic-probe/fixture.txt',original+'/pocket-isolation-canary'])))
    assert bionic['differentBytes']==0 and not bionic['bootShimLoaded'] and not bionic['backingPathOpened'],bionic
    output=call(guest(['/system/bin/proot-owner-probe']))
    result=c.adb('-s',c.serial(),'shell','-T','su -c '+shlex.quote('cat '+original+'/pocket-isolation-canary'),check=False)
    assert result.returncode==0 and result.stdout.strip()=='isolated-host-canary',result.stdout
    assert 'OWNER_CHILD_EXEC_PASS' in output and 'OWNER_METADATA_PASS' in output,output
    # A killed tracer must never release an Android process without translation.
    lifecycle=io.BytesIO();life_errors=io.BytesIO();life_result=[]
    thread=threading.Thread(target=lambda:life_result.append(request(config,2,
        guest(['/system/bin/proot-owner-probe','lifecycle']),
        stdout=lifecycle,stderr=life_errors)),daemon=True)
    thread.start()
    for _ in range(100):
        match=re.search(rb'TRACE_LIFECYCLE_READY pid=(\d+)',lifecycle.getvalue())
        if match:break
        if not thread.is_alive():raise AssertionError(life_errors.getvalue())
        time.sleep(.1)
    else:raise AssertionError('Lifecycle fixture did not start')
    child=int(match.group(1))
    status=host('cat /proc/'+str(child)+'/status')
    tracer=int(re.search(r'^TracerPid:\s*(\d+)',status,re.M).group(1))
    assert tracer>0
    tracer_name=(original+'/system/bin/pocket-syscall-tracer') if installed else (backing+'/libproot.so')
    assert tracer_name in host('tr "\\000" " " < /proc/'+str(tracer)+'/cmdline')
    host('kill -KILL '+str(tracer))
    thread.join(10)
    assert not thread.is_alive(),'Killed tracer RPC did not finish'
    status=host('if [ -e /proc/'+str(child)+'/status ]; then cat /proc/'+str(child)+'/status; fi')
    assert not status or re.search(r'^State:\s+Z',status,re.M),'Tracee survived killed tracer'
    report={'command':'python3 /home/pi/test_proot_owner_on_pi.py','exitCode':0,'nativeOutput':output.strip(),'bionic':bionic,
        'checks':['data realpath, fchdir, cwd and directory FD reverse mapping',
            'file and directory ownership','relative fchownat','AT_EMPTY_PATH','symlink ownership',
            'existing O_CREAT retains ownership','fork and exec retain metadata and groups',
            'unprivileged ownership changes denied','failed exec preserves errno and credentials',
            'actual kernel capability mask','timerfd alarm clock adaptation','host time changes denied','host guest-root canary inaccessible and preserved',
            'killed tracer kills its tracees (PTRACE_O_EXITKILL)'],
        'scope':'Installed wrapper-free Manager owner-console subprocesses; framework validation is reported separately' if installed else 'Isolated native subprocesses; no Bionic preload; not a full Android boot verdict'}
    (Path.home()/'pocket-proot-owner-validation.json').write_text(json.dumps(report,indent=2)+'\n')
    print(output.strip())
    print('PASS: native ownership/identity/timer/failed-exec regressions')

if __name__=='__main__':main()
