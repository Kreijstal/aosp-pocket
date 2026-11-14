#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check the actual new/old native probes against the Manager v2 contract."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
from build_api29_manager import validate_postboot_installd_client


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client',type=Path,required=True)
    parser.add_argument('--old-client',type=Path,required=True)
    args=parser.parse_args()
    receipt=validate_postboot_installd_client(args.client)
    assert receipt['protocol']==2 and len(receipt['operations'])==5

    def reject(path,expected):
        try:validate_postboot_installd_client(path)
        except ValueError as error:
            assert expected in str(error),(path,str(error),expected)
        else:raise AssertionError('Incompatible native probe accepted: '+str(path))

    reject(args.old_client,'dexopt-postboot')
    original=args.client.read_bytes()
    with tempfile.TemporaryDirectory() as directory:
        path=Path(directory)/'probe'
        for operation in receipt['operations']:
            token=operation.encode()+b'\0'
            assert original.count(token)>=1
            path.write_bytes(original.replace(token,b'?'*len(token)))
            reject(path,operation)
        for offset,value in [(0,b'NOPE'),(4,b'\x01'),(18,b'\x3e\x00')]:
            damaged=bytearray(original);damaged[offset:offset+len(value)]=value
            path.write_bytes(damaged);reject(path,'ARM64 ELF')
        destination=Path(directory)/'never-built'
        result=subprocess.run([sys.executable,str(Path(__file__).with_name('build_api29_manager.py')),
            '--catalog',str(Path(directory)/'unused-catalog.json'),'--ce-probe32',
            '--api32-catalog',str(Path(directory)/'unused-api32-catalog.json'),
            '--installd-postboot32','--installd-client32',str(args.old_client),
            '--out',str(destination)],capture_output=True,text=True)
        assert result.returncode==2 and 'lacks v2 operations' in result.stderr
        assert not destination.exists(),'Rejected probe created a build output directory'
    print('PASS_ACTUAL_NEW_PROBE_FIVE_MODES_OLD_CLIENT_AND_EIGHT_CORRUPTIONS_REJECTED_CLI_FAILS_BEFORE_BUILD')


if __name__=='__main__':main()
