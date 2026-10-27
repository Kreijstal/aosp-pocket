#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise production FUSE scheduling through JNI and real protocol sockets."""
from pathlib import Path
import shutil
import subprocess
from target import ROOT

def main():
    out=ROOT/'out/ce-fuse-concurrency';out.mkdir(exist_ok=True)
    jdk=Path(shutil.which('javac')).resolve().parents[1]
    def run(*argv):subprocess.run(list(map(str,argv)),cwd=ROOT,check=True,timeout=30)
    run('cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-fPIC','-shared','-pthread',
        '-I'+str(jdk/'include'),'-I'+str(jdk/'include/linux'),
        ROOT/'runtime/storage/native/ce_fuse.c',ROOT/'runtime/tests/storage-integration/fuse_socket_test.c',
        '-o',out/'libpocket_ce_host_test.so')
    run('javac','--release','8','-d',out,ROOT/'runtime/tests/storage-integration/CeFuseConcurrencyTest.java')
    run('java','-cp',out,'dev.kreijstal.aosppocket.runtime.CeFuseConcurrencyTest',out/'libpocket_ce_host_test.so')

if __name__=='__main__':main()
