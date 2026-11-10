#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the pinned 12L netd mDNS frontend for the stock image responder."""
import argparse
from pathlib import Path
import subprocess
from prepare_api32 import ROOT,CACHE,prepare,prepare_sources


def build_mdns(ndk):
    prepare_sources(['libcxx','system-core','system-logging','libbase','system-netd','mdnsresponder'])
    reference=prepare();out=CACHE/'mdns';out.mkdir(parents=True,exist_ok=True)
    source=out/'MDnsSdListener.cpp'
    text=(CACHE/'system-netd/server/MDnsSdListener.cpp').read_text()
    anchor='    if (argc < 2) {'
    assert text.count(anchor)==1
    guard='''    uint64_t identity=0;
    const uid_t kernelUid=pocket_kernel_uid_from_proc();
    if(kernelUid==static_cast<uid_t>(-1) || cli->getUid()!=kernelUid ||
       !pocket_binder_identity(cli->getPid(),&identity) ||
       ((identity>>32)!=0 && (identity>>32)!=1000)) {
        cli->sendMsg(ResponseCode::OperationFailed,"Guest mDNS requires a system role",false);
        return -1;
    }
'''
    source.write_text('#include "kernel_identity.h"\n#include "binder_identity.h"\n'+text.replace(anchor,guard+anchor))
    compiler=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android32-clang++'
    includes=[CACHE/'system-netd/server',CACHE/'system-netd/libnetdutils/include',
        CACHE/'mdnsresponder/mDNSShared',CACHE/'system-core/libsysutils/include',
        CACHE/'libbase/include',CACHE/'system-logging/liblog/include',ROOT/'runtime/api28',ROOT/'runtime/direct']
    includes += [CACHE/'system-core'/name/'include' for name in ['libcutils','libutils','libsystem']]
    common=[str(compiler),'-DPOCKET_GUEST_API=32','-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H',
        '-nostdinc++','-isystem',str(CACHE/'libcxx/include'),'-std=c++17','-O2','-fno-rtti',
        '-fno-exceptions','-nostdlib++','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
        '-Wno-inconsistent-missing-override','-fPIE','-pie']
    common += ['-I'+str(path) for path in includes]
    libs=reference/'system/lib64';bionic=reference/'apex/com.android.runtime/lib64/bionic'
    link=[str(bionic/'libc.so'),'-L'+str(libs),'-L'+str(bionic),
        '-Wl,-rpath-link,'+str(libs),'-Wl,-rpath-link,'+str(bionic),
        '-lsysutils','-lmdnssd','-lnetdutils','-lcutils','-lbase','-lutils','-llog','-lc++',
        '-Wl,--no-undefined','-Wl,-z,max-page-size=16384']
    subprocess.run(common+[str(ROOT/'runtime/api32/mdns_main.cpp'),str(source),
        str(CACHE/'system-netd/server/NetdCommand.cpp')]+link+['-o',str(out/'pocket-mdns-control-api32')],check=True)
    subprocess.run(common+[str(ROOT/'runtime/api32/mdns_probe.cpp')]+link+['-o',str(out/'pocket-mdns-probe-api32')],check=True)
    print('API32_MDNS_BUILD_PASS',out)
    return out


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--ndk',type=Path,required=True)
    build_mdns(parser.parse_args().ndk.resolve())
