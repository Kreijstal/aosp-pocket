#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build typed 12L volume and storage services for the app-private profile."""
import argparse
import json
from pathlib import Path
import re
import subprocess
from prepare_api32 import ROOT,CACHE,prepare,prepare_sources,prepare_aidl


def build_storage(ndk):
    prepare_sources(['libcxx','system-core','libbase','system-logging','frameworks-native','frameworks-base','system-vold'])
    reference=prepare();out=CACHE/'storage';generated=out/'generated';generated.mkdir(parents=True,exist_ok=True)
    vold=CACHE/'system-vold/binder';core=CACHE/'system-core/storaged/binder'
    framework=CACHE/'frameworks-base/core/java'
    binder=CACHE/'frameworks-native/aidl/binder'
    files=sorted(vold.rglob('*.aidl'))+[core/'android/os/IStoraged.aidl',
        framework/'android/os/incremental/IncrementalFileSystemControlParcel.aidl']
    subprocess.run([str(prepare_aidl()),'--lang=cpp','--out='+str(generated/'src'),
        '--header_out='+str(generated),*['-I'+str(p) for p in [vold,core,framework,binder]],*map(str,files)],check=True)
    methods=[]
    for line in (generated/'android/os/IVold.h').read_text().splitlines():
        if not line.strip().startswith('virtual ::android::binder::Status ') or not line.endswith(' = 0;'):continue
        declaration=line.strip().removeprefix('virtual ').removesuffix(' = 0;')
        name=re.search(r'Status (\w+)\(',declaration).group(1)
        methods.append(declaration+' override { return storageUnsupported32("'+name+'"); }')
    assert len(methods)==86 and any('supportsCheckpoint(' in m for m in methods)
    (generated/'VoldUnsupported32.h').write_text('// Generated from the pinned 12L platform IVold.\n#pragma once\n'
        '#include <android/os/BnVold.h>\n#include <stdio.h>\n'
        'inline android::binder::Status storageUnsupported32(const char* operation) {\n'
        ' fprintf(stderr,"API32_VOLD_UNSUPPORTED operation=%s host_kernel=unchanged\\n",operation);\n'
        ' return android::binder::Status::fromExceptionCode(android::binder::Status::EX_UNSUPPORTED_OPERATION,\n'
        ' "Operation unavailable in the app-private volume profile");\n}\n'
        'class VoldUnsupported32 : public android::os::BnVold { public:\n'+'\n'.join(methods)+'\n};\n')
    modules=json.loads((reference/'.pocket-api32-reference.json').read_text())['apexModules']
    names=[m['name'] for m in modules];assert len(set(names))==len(names)
    assert all(re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.-]*',n) for n in names)
    (generated/'storage_modules32.h').write_text('// From the verified SDK32 APEX manifests.\n'
        'static const char* const modules32[]={'+','.join(map(json.dumps,names))+'};\n')
    llvm=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    includes=[generated,ROOT/'runtime/api32',CACHE/'frameworks-native/libs/binder/include',
        CACHE/'frameworks-native/libs/binder/ndk/include_cpp',
        CACHE/'libbase/include',CACHE/'system-logging/liblog/include']
    includes += [CACHE/'system-core'/name/'include' for name in ['libutils','libcutils','libsystem']]
    common=[str(llvm/'aarch64-linux-android32-clang++'),'-DPOCKET_GUEST_API=32',
        '-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H','-nostdinc++','-isystem',str(CACHE/'libcxx/include'),
        '-std=c++17','-O2','-fno-rtti','-fno-exceptions','-nostdlib++','-Wall','-Wextra','-Werror',
        '-Wno-unused-parameter','-Wno-inconsistent-missing-override','-fPIE','-pie',*['-I'+str(p) for p in includes]]
    libs=reference/'system/lib64';bionic=reference/'apex/com.android.runtime/lib64/bionic'
    link=[str(bionic/'libc.so'),'-L'+str(libs),'-L'+str(bionic),'-Wl,-rpath-link,'+str(libs),
        '-Wl,-rpath-link,'+str(bionic),'-lbinder','-lutils','-lbase','-lc++','-llog',
        '-Wl,--no-undefined','-Wl,-z,max-page-size=16384']
    # These platform AIDL implementations are statically linked in stock vold;
    # compile the matching generated dispatch and parcelables into each client.
    sources=list(map(str,sorted((generated/'src').rglob('*.cpp'))))
    for name in ['storage_service.cpp','storage_probe.cpp']:
        subprocess.run(common+[str(ROOT/'runtime/api32'/name),*sources]+link+
            ['-o',str(out/('pocket-'+('storage-api32' if name=='storage_service.cpp' else 'storage-probe-api32')))],check=True)
    print('API32_STORAGE_BUILD_PASS',out,'methods='+str(len(methods)))
    return out


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--ndk',type=Path,required=True)
    build_storage(parser.parse_args().ndk.resolve())
