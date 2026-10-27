#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Narrow host tests for the staged CE backend, including process-kill recovery."""
from pathlib import Path
import os
import select
import subprocess
import tempfile
import time
from target import ROOT


def main():
    out=ROOT/'out/storage-probe/backend-classes'
    out.mkdir(parents=True,exist_ok=True)
    subprocess.run(['javac','--release','8','-Xlint:all','-d',str(out),
        str(ROOT/'runtime/storage/src/dev/kreijstal/aosppocket/storage/EncryptedStore.java'),
        str(ROOT/'runtime/tests/EncryptedStoreTest.java')],check=True)
    command=['java','-cp',str(out),'dev.kreijstal.aosppocket.storage.EncryptedStoreTest']
    subprocess.run(command,check=True,timeout=90)
    with tempfile.TemporaryDirectory(prefix='pocket-ce-crash-') as directory:
        vault=Path(directory)/'vault'
        secret=os.urandom(32)
        subprocess.run(command+['crash-create',str(vault)],input=secret,check=True,timeout=15)
        for delay in (.001,.003,.007,.015,.03,.06,.1,.2):
            child=subprocess.Popen(command+['crash-writer',str(vault)],stdin=subprocess.PIPE,stdout=subprocess.PIPE)
            try:
                child.stdin.write(secret);child.stdin.close()
                if not select.select([child.stdout],[],[],15)[0] or child.stdout.readline()!=b'READY\n':
                    raise RuntimeError('Crash writer did not initialize')
                if delay==.001:
                    subprocess.run(command+['expect-locked',str(vault)],input=secret,check=True,timeout=15)
                time.sleep(delay)
                if child.poll() is not None:raise RuntimeError('Crash writer exited early')
            finally:
                if child.poll() is None:child.kill()
                child.wait(timeout=10);child.stdout.close()
            subprocess.run(command+['crash-verify',str(vault)],input=secret,check=True,timeout=15)
    print('PASS: encrypted backend behavior and 8 SIGKILL recovery cycles')


if __name__=='__main__':main()
