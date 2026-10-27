#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Merge current namespace code into a verified APK without replacing other features."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile
from target import ROOT

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base-apk',type=Path,required=True)
    p.add_argument('--version',required=True)
    p.add_argument('--host-library',action='append',default=[])
    args=p.parse_args();base=args.base_apk.resolve()
    out=ROOT/('out/contacts-'+args.version);out.mkdir(parents=True,exist_ok=True)
    smali=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts/extract-tools/common/smali'
    android=ROOT.parent/'helloworldandroid/vendor/android-platforms/android-36/android.jar'
    r8=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar'
    def run(*argv):subprocess.run(list(map(str,argv)),cwd=ROOT,check=True)
    for name in ['classes','source-dex','baseline-smali','source-smali','patched-smali','verified-smali']:
        shutil.rmtree(out/name,ignore_errors=True)
    (out/'classes').mkdir();(out/'source-dex').mkdir()
    sources=[ROOT/'runtime/storage/src/dev/kreijstal/aosppocket/storage'/name for name in ['EncryptedStore.java','CeNamespace.java']]
    run('javac','--release','8','-d',out/'classes',*sources)
    run('jar','cf',out/'source.jar','-C',out/'classes','.')
    run('java','-cp',r8,'com.android.tools.r8.D8','--lib',android,'--min-api','28','--output',out/'source-dex',out/'source.jar')
    with zipfile.ZipFile(base) as z:(out/'base.dex').write_bytes(z.read('classes.dex'))
    for folder,dex in [('baseline-smali',out/'base.dex'),('source-smali',out/'source-dex/classes.dex')]:
        run('java','-jar',smali/'baksmali.jar','disassemble',dex,'-o',out/folder)
    baseline=out/'baseline-smali';source=out/'source-smali'
    namespace=Path('dev/kreijstal/aosppocket/storage/CeNamespace.smali')
    # The replacement calls the same backend and uses the same nested node types.
    # Refuse incompatible bases rather than silently dropping their changes.
    for file in source.rglob('*.smali'):
        if file.name.startswith(('EncryptedStore','CeNamespace$')):
            relative=file.relative_to(source)
            if not (baseline/relative).exists() or file.read_bytes()!=(baseline/relative).read_bytes():
                p.error('Base storage ABI/implementation differs: '+str(relative))
    patched=out/'patched-smali';shutil.copytree(baseline,patched)
    shutil.copyfile(source/namespace,patched/namespace)
    dex=out/'classes.dex'
    run('java','-jar',smali/'smali.jar','assemble','--api','28',patched,'-o',dex)
    run('java','-jar',smali/'baksmali.jar','disassemble',dex,'-o',out/'verified-smali')
    verified=out/'verified-smali'
    names={f.relative_to(baseline) for f in baseline.rglob('*.smali')}
    assert names=={f.relative_to(verified) for f in verified.rglob('*.smali')}
    changed=sorted(str(n) for n in names if (baseline/n).read_bytes()!=(verified/n).read_bytes())
    assert set(changed)<={str(namespace)},changed
    libraries=[item for spec in args.host_library for item in ['--host-library',spec]]
    stage=ROOT/'runtime/magisk/stage.sh'
    run('sh','-n',stage)
    run('python3',ROOT/'tools/build_manager_update.py','--base-apk',base,'--out',out,
        '--host-dex',dex,'--guest-file','system/etc/pocket-magisk/stage.sh='+str(stage),
        '--version-code',args.version.split('.')[-1],'--version-name',args.version,*libraries)
    apk=out/('aosp-pocket-manager-'+args.version+'.apk')
    with zipfile.ZipFile(base) as a,zipfile.ZipFile(apk) as b:
        changed_entries=sorted(n for n in set(a.namelist())|set(b.namelist())
            if not n.startswith('META-INF/') and (n not in a.namelist() or n not in b.namelist() or a.read(n)!=b.read(n)))
        allowed={'AndroidManifest.xml','classes.dex','assets/api28-rootfs/system/etc/pocket-magisk/stage.sh.payload'}
        allowed.update('lib/arm64-v8a/'+s.partition('=')[0] for s in args.host_library)
        assert set(changed_entries)<=allowed,changed_entries
    report={'validation':'PASS','baseApk':str(base),'version':args.version,'changedClasses':changed,
        'changedApkEntries':changed_entries,'sha256':hashlib.sha256(apk.read_bytes()).hexdigest(),
        'dexSha256':hashlib.sha256(dex.read_bytes()).hexdigest(),'bytes':apk.stat().st_size,
        'namespaceSourceSha256':hashlib.sha256(sources[1].read_bytes()).hexdigest()}
    (out/'audit.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))

if __name__=='__main__':main()
