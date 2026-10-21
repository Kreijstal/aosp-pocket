#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run private native subprocesses; preserve the running guest and its shim."""
import argparse
import io
import json
from pathlib import Path
import struct
import sys

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifacts',type=Path,default=Path.home()/'bionic-mode')
    parser.add_argument('--config',type=Path,default=Path.home()/'.config/pocket-pi-debug.json')
    parser.add_argument('--output',type=Path,default=Path.home()/'pocket-bionic-mode-validation.json')
    args=parser.parse_args()
    sys.path.insert(0,'/home/pi/lineage-cuttlefish/tools')
    import cuttlefish_arm64 as c
    from pocketctl import request
    c.require_virtual()
    config=json.loads(args.config.read_text())
    def invoke(argv=(),source=None,op=2):
        out,err=io.BytesIO(),io.BytesIO()
        rc=request(config,op,argv,source=source,stdout=out,stderr=err)
        return rc,out.getvalue(),err.getvalue()
    def command(*argv,source=None):
        rc,out,err=invoke(argv,source)
        if rc:raise RuntimeError((rc,out.decode(errors='replace'),err.decode(errors='replace')))
        return out
    before=json.loads(invoke(op=1)[1])
    assert before['ready'] and before['sdk']==28,before
    root='/data/user/0/dev.kreijstal.aosppocket.runtime/files/api28-rootfs'
    stage='/data/local/tmp/pocket-bionic-mode'
    backing=root+'/system-data/local/tmp/pocket-bionic-mode'
    command('/system/bin/toybox','mkdir','-p',stage,'/pocket-bionic-probe')
    def upload(name,data,mode='755'):
        command('/system/bin/sh','-c',
            '/system/bin/toybox cat > "$1.next" && /system/bin/toybox chmod "$2" "$1.next" && /system/bin/toybox mv "$1.next" "$1"',
            'upload',stage+'/'+name,mode,source=io.BytesIO(data))
    for name in ('libpocket_direct_boot.so','bionic-mode-launcher'):
        upload(name,(args.artifacts/name).read_bytes())
    image=bytearray((args.artifacts/'bionic-mode-probe').read_bytes())
    phoff=struct.unpack_from('<Q',image,32)[0]
    phsize,phnum=struct.unpack_from('<HH',image,54)
    for i in range(phnum):
        header=struct.unpack_from('<IIQQQQQQ',image,phoff+i*phsize)
        if header[0]!=3:continue
        interpreter=(root+'/system/bin/linker64').encode()+b'\0'
        assert len(interpreter)<=header[5]
        image[header[2]:header[2]+header[5]]=interpreter.ljust(header[5],b'\0')
        break
    else:raise AssertionError('No reserved guest interpreter')
    upload('bionic-mode-probe',image)
    command('/system/bin/sh','-c','/system/bin/toybox cat > /pocket-bionic-probe/fixture.txt',
        source=io.BytesIO(b'bionic-mode-fixture\n'))
    reports={}
    try:
        for mode in ('wrapped','unmodified'):
            rc,out,err=invoke([stage+'/bionic-mode-launcher',root,backing,mode,backing+'/bionic-mode-probe'])
            assert rc==0,(mode,rc,out.decode(errors='replace'),err.decode(errors='replace'))
            result=json.loads(out)
            assert result['mode']==mode
            assert ('bionic='+mode) in err.decode(),err.decode()
            if mode=='unmodified':
                assert result['differentBytes']==0 and result['libcUid']==result['syscallUid']
                assert 'hooks=0 ' in err.decode() and 'properties_init=-1 ' in err.decode()
            reports[mode]=result
            print(json.dumps(result),flush=True)
        rc,out,err=invoke([stage+'/bionic-mode-launcher',root,backing,'invalid',backing+'/bionic-mode-probe'])
        assert rc==125 and b'DIRECT_BOOT_INVALID_BIONIC_MODE' in err,(rc,out,err)
        after=json.loads(invoke(op=1)[1])
        assert after['ready'] and after['running'],after
        report={'command':'python3 /home/pi/test_bionic_mode_on_pi.py',
            'environment':'Pi outer LineageOS VM -> Pocket owner console -> isolated native subprocesses',
            'modes':reports,'invalidModeExitCode':rc,'guestStillReady':True,'exitCode':0,
            'scope':'Isolated native Bionic modes; full Android boot in unmodified mode is not established'}
        args.output.write_text(json.dumps(report,indent=2)+'\n')
        print('PASS: original native Bionic modes and running guest preserved')
    finally:
        command('/system/bin/toybox','rm','-f','/pocket-bionic-probe/fixture.txt')
        command('/system/bin/toybox','rmdir','/pocket-bionic-probe')

if __name__=='__main__':main()
