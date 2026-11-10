#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the immutable framework memory configuration for the API 32 guest."""
import hashlib
import json
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET
import zipfile
from prepare_api32 import ROOT, CACHE, prepare, specification, verified_download, verify_file

def overlay_tool():
    spec=specification()['overlay_aapt2_tool']
    tools=CACHE/'tools';tools.mkdir(parents=True,exist_ok=True)
    archive=tools/('aapt2-'+spec['version']+'-linux.jar')
    verified_download(spec['url'],archive,spec['sha256'],spec['bytes'])
    binary=tools/('aapt2-'+spec['version'])
    if not binary.exists():
        with zipfile.ZipFile(archive) as source:payload=source.read('aapt2')
        if hashlib.sha256(payload).hexdigest()!=spec['binarySha256']:
            raise ValueError('AAPT2 binary differs from the source lock')
        binary.write_bytes(payload);binary.chmod(0o755)
    verify_file(binary,spec['binarySha256'])
    return binary

def build_memory_overlay(reference):
    source=ROOT/'runtime/api32/overlays/PocketMemory';out=CACHE/'memory-overlay'
    out.mkdir(parents=True,exist_ok=True)
    values=ET.parse(source/'res/values/config.xml').getroot()
    value=values.find("integer[@name='config_customizedMaxCachedProcesses']")
    limit=int(value.text) if value is not None else 0
    properties=dict(line.split('=',1) for line in (ROOT/'runtime/api32/memory.prop').read_text().splitlines()
                    if line and not line.startswith('#'))
    if not 2<=limit<=32 or int(properties['ro.pocket.max_cached_processes'])!=limit:
        raise ValueError('Framework overlay and memory profile cache limits must agree')
    aapt=overlay_tool()
    def run(*args):subprocess.run(list(map(str,args)),check=True)
    run(aapt,'compile','--dir',source/'res','-o',out/'compiled.zip')
    run(aapt,'link','-I',reference/'system/framework/framework-res.apk',
        '--manifest',source/'AndroidManifest.xml','--version-code','1','--version-name','1',
        '--min-sdk-version','32','--target-sdk-version','32','-o',out/'unsigned.apk',out/'compiled.zip')
    signer=['java','-cp',ROOT.parent/'helloworldandroid/build/apksigner.jar','com.android.apksigner.ApkSignerTool']
    target=out/'PocketMemory.apk'
    run(*signer,'sign','--ks',ROOT/'out/debug.keystore','--ks-pass','pass:android',
        '--key-pass','pass:android','--out',target,out/'unsigned.apk')
    run(*signer,'verify','--min-sdk-version','32','--max-sdk-version','32',target)
    (out/'build.json').write_text(json.dumps({'package':'dev.kreijstal.aosppocket.overlay.memory',
        'sdk':32,'cachedProcessLimit':limit,'frameworkApkModified':False,
        'apkSha256':hashlib.sha256(target.read_bytes()).hexdigest(),'apkBytes':target.stat().st_size,
        'runtimeVerified':False},indent=2)+'\n')
    print('API32_MEMORY_OVERLAY_BUILD_PASS',target)
    return target

if __name__=='__main__':build_memory_overlay(prepare())
