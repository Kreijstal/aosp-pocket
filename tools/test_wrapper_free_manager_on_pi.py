#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate an already installed/running wrapper-free Manager on the Pi VM."""
import io
import json
import re
from pathlib import Path
import sys

sys.path.insert(0,'/home/pi/lineage-cuttlefish/tools')
import cuttlefish_arm64 as c
from pocketctl import request

def main():
    c.require_virtual()
    config=json.loads((Path.home()/'.config/pocket-pi-debug.json').read_text())
    def call(op,argv=()):
        out,err=io.BytesIO(),io.BytesIO()
        rc=request(config,op,argv,stdout=out,stderr=err)
        assert rc==0,(rc,err.getvalue().decode(errors='replace'))
        return out.getvalue()
    state=json.loads(call(1))
    assert state.get('ready') and state.get('sdk')==28,state
    checks=call(2,['/system/bin/sh','-c',
        'test "$POCKET_SYSCALL_BACKEND" = proot && '
        'test ! -e /system/lib64/libpocket_direct_boot.so && '
        'test "$(/system/bin/getprop ro.build.version.sdk)" = 28 && '
        'test "$(/system/bin/toybox id -u)" = 0 && '
        'test "$(/system/bin/toybox realpath /data)" = /data && '
        'cd /data && test "$(/system/bin/toybox pwd -P)" = /data && '
        'echo WRAPPER_FREE_MANAGER_PASS']).decode().strip()
    assert checks=='WRAPPER_FREE_MANAGER_PASS',checks
    package=c.adb('-s',c.serial(),'shell','dumpsys package dev.kreijstal.aosppocket.runtime').stdout
    version=re.search(r'versionName=([^\s]+)',package)
    assert version,'Installed Manager version missing'
    report={'command':'python3 /home/pi/test_wrapper_free_manager_on_pi.py',
        'exitCode':0,'version':version.group(1),'sdk':28,'ready':True,'backend':'proot','bionicWrapperPresent':False,
        'checks':['real Manager display/control session ready','owner console logical root',
            'Android 9 private properties readable','data realpath and cwd use guest /data'],
        'scope':'Installed Manager on Pi Android VM; physical Samsung and all peripherals not validated'}
    (Path.home()/'pocket-wrapper-free-manager-validation.json').write_text(json.dumps(report,indent=2)+'\n')
    print(checks)

if __name__=='__main__':main()
