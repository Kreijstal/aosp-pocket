#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Read only named, immutable files from the separate API 29 VM fixture."""
import argparse
import json
from pathlib import Path, PurePosixPath
import re
import shlex
import sys
sys.path.insert(0,'/home/pi/lineage-cuttlefish/tools')
import cuttlefish_arm64 as c

PACKAGE='dev.kreijstal.aosppocket.api29test'


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--request',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    args=p.parse_args();request=json.loads(args.request.read_text())
    assert request['schema']==1 and request['package']==PACKAGE
    files=request['files'];assert len(files)==len(set(files)) and len(files)<=20000
    for name in files:
        path=PurePosixPath(name)
        assert not path.is_absolute() and '..' not in path.parts and str(path)==name
        assert name.startswith(('system/','vendor/','apex/')) or name in {
            'probe.sh','probe-environment','probe-binder-enabled','data/local/tmp/hello29.jar'}
    c.require_virtual();root='/data/user/0/'+PACKAGE+'/files/root'
    result={}
    for offset in range(0,len(files),100):
        selected=files[offset:offset+100]
        cmd='cd '+shlex.quote(root)+' && sha256sum '+' '.join(map(shlex.quote,selected))
        r=c.adb('-s',c.serial(),'shell','su -c '+shlex.quote(cmd),timeout=60,check=False)
        for line in r.stdout.splitlines():
            match=re.fullmatch(r'([0-9a-f]{64})\s+(.+)',line)
            if match:
                assert match[2] in selected;result[match[2]]=match[1]
    args.out.write_text(json.dumps({'schema':1,'package':PACKAGE,'guestSdk':29,'files':result},indent=2)+'\n')
    print('API29_INVENTORY_PASS immutable_files='+str(len(result))+' requested='+str(len(files)),flush=True)


if __name__=='__main__':main()
