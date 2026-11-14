#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the actual-ART original/patched Launcher recents fault-injection fixture."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import zipfile
from build_api32_launcher import ORIGINAL_SHA,digest,load_package
from prepare_api32 import ROOT


def build(base,launcher,out):
    assert digest(base)==ORIGINAL_SHA
    package=load_package(launcher)
    assert not out.exists(),'Use a fresh fixture output'
    out.mkdir(parents=True);classes=out/'classes';classes.mkdir();dex=out/'dex';dex.mkdir()
    source=ROOT/'runtime/tests/api32/LauncherRecentsProbe32.java'
    android=ROOT.parent/'helloworldandroid/vendor/android-platforms/android-36/android.jar'
    r8=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar'
    commands=[['javac','--release','8','-cp',android,'-d',classes,source],
        ['jar','cf',out/'classes.jar','-C',classes,'.'],
        ['java','-cp',r8,'com.android.tools.r8.D8','--lib',android,'--min-api','32','--output',dex,out/'classes.jar']]
    for command in commands:subprocess.run(list(map(str,command)),check=True)
    target=out/'launcher-recents-probe.jar'
    with zipfile.ZipFile(target,'w',compression=zipfile.ZIP_STORED) as archive:
        info=zipfile.ZipInfo('classes.dex',(1980,1,1,0,0,0));info.external_attr=0o100644<<16;archive.writestr(info,(dex/'classes.dex').read_bytes())
    original=out/'Launcher3QuickStep-original.apk';shutil.copyfile(base,original)
    spec={'schema':1,'sdk':32,'mainClass':'dev.kreijstal.aosppocket.guest.LauncherRecentsProbe32',
        'installedLauncherPath':'/system_ext/priv-app/Launcher3QuickStep/Launcher3QuickStep.apk',
        'installedLauncherSha256':package['sha256'],'installedLauncherCertificateSha256':package['certificateSha256'],
        'originalApk':{'name':original.name,'bytes':original.stat().st_size,'sha256':digest(original)},
        'probeJar':{'name':target.name,'bytes':target.stat().st_size,'sha256':digest(target)},
        'scope':'Actual Launcher APK DEX on Android32 ART in separate class loaders. Controlled null/empty/disconnected/nonempty/RemoteException/permission/state results; preserve task object identity/order and query arguments. Real task UI and Binder authorization remain separate integration gates.'}
    (out/'probe-spec.json').write_text(json.dumps(spec,indent=2)+'\n')
    metadata={'schema':1,'sdk':32,'spec':spec,'sources':{str(p.relative_to(ROOT)):digest(p) for p in [source,Path(__file__).resolve()]},
        'androidJarSha256':digest(android),'r8Sha256':digest(r8),'commands':[list(map(str,c)) for c in commands],
        'originalApkModified':False,'ordinaryAppInstalled':False,'androidExecuted':False}
    (out/'build.json').write_text(json.dumps(metadata,indent=2)+'\n')
    print('API32_LAUNCHER_ART_PROBE_BUILD_PASS',target,spec['probeJar']['sha256'],flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--base-apk',type=Path,required=True)
    parser.add_argument('--launcher-dir',type=Path,required=True);parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();build(args.base_apk.resolve(),args.launcher_dir.resolve(),args.out.resolve())
