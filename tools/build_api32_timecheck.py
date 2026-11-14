#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build libpocket_timecheck_api32.so, the 12L audioserver TimeCheck floor preload."""
import argparse,hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def configure_audio_service(config):
    """Add the watchdog adapter to audioserver alone, preserving other services."""
    library='/system/lib64/libpocket_timecheck_api32.so'
    base='/system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_hidl_api32.so'
    lines=config.splitlines(keepends=True)
    service=None;audio=[];preloads=[]
    for index,line in enumerate(lines):
        text=line.strip()
        if not text or text.startswith('#'):continue
        if not line[0].isspace():
            words=text.split();service=words[1] if words[:1]==['service'] and len(words)>1 else None
            if service=='pocket-audio':
                if text!='service pocket-audio /system/bin/audioserver':
                    raise ValueError('Unexpected audioserver command')
                audio.append(index)
        if library in text and service!='pocket-audio':
            raise ValueError('TimeCheck adapter must be scoped to audioserver')
        if service=='pocket-audio' and text.startswith('setenv LD_PRELOAD '):
            preloads.append(index)
    if len(audio)!=1 or len(preloads)!=1:
        raise ValueError('Expected exactly one audioserver and preload directive')
    index=preloads[0];line=lines[index];value=line.strip()
    if value not in ['setenv LD_PRELOAD '+base,'setenv LD_PRELOAD '+base+':'+library]:
        raise ValueError('Unexpected audioserver preload libraries')
    if value.endswith(base):
        ending='\r\n' if line.endswith('\r\n') else '\n' if line.endswith('\n') else ''
        lines[index]=(line[:-len(ending)] if ending else line)+':'+library+ending
    return ''.join(lines)

def build(ndk,out):
    out.mkdir(parents=True,exist_ok=True);lib=out/'libpocket_timecheck_api32.so'
    cc=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang'
    subprocess.run([str(cc),'-O2','-Wall','-Wextra','-Werror','-fPIC','-shared','-Wl,-z,max-page-size=16384','-Wl,--build-id=none',
        str(ROOT/'runtime/api32/timecheck_preload.c'),'-llog','-ldl','-o',str(lib)],check=True)
    syms=subprocess.run(['readelf','-W','--dyn-syms',str(lib)],capture_output=True,text=True,check=True).stdout
    assert any(l.split()[-1]=='_ZN7android9TimeCheckC1EPKcj' and ' UND ' not in l for l in syms.splitlines() if l.strip()),'TimeCheck constructor not exported'
    receipt={'schema':1,'library':lib.name,'sha256':hashlib.sha256(lib.read_bytes()).hexdigest(),
        'bytes':lib.stat().st_size,'scope':'pocket-audio only','timeoutFloorMillis':20000,
        'watchdogDisabled':False,'runtimeVerified':False,
        'sources':{name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in
            ['runtime/api32/timecheck_preload.c','tools/build_api32_timecheck.py']}}
    (out/'build.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print('API32_TIMECHECK_BUILD_PASS',lib,hashlib.sha256(lib.read_bytes()).hexdigest(),flush=True)
    return lib

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ndk',type=Path,required=True)
    p.add_argument('--out',type=Path,default=ROOT/'out/api32/timecheck')
    a=p.parse_args();build(a.ndk,a.out)
