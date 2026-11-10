#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build an ordinary, debug-signed API32 public audio/camera application."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import zipfile
from build_api32_guest_app import build_java_app
from prepare_api32 import ROOT,CACHE


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,default=CACHE/'av-app')
    a=p.parse_args();out=a.out.resolve()
    apk=build_java_app(ROOT/'runtime/tests/api32/avapp',out,'dev.kreijstal.pocketav32test','av-test.apk')
    meta=json.loads((out/'build.json').read_text())
    signer=['java','-cp',str(ROOT.parent/'helloworldandroid/build/apksigner.jar'),'com.android.apksigner.ApkSignerTool']
    verification=subprocess.check_output(signer+['verify','--print-certs','--min-sdk-version','32','--max-sdk-version','32',str(apk)],text=True)
    cert=re.findall(r'Signer #\d+ certificate SHA-256 digest: ([a-f0-9]{64})',verification);assert len(cert)==1
    assert cert[0]!='c8a2e9bccf597c2fb6dc66bee293fc13f2fc47ec77bc6b2b0d52c11f51192ab8','Ordinary AV test must not use the platform certificate'
    archive=out/'PocketAV32-api32.apks';raw=apk.read_bytes()
    with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_STORED) as z:
        entry=zipfile.ZipInfo('base.apk',(1980,1,1,0,0,0));entry.external_attr=0o100644<<16;z.writestr(entry,raw)
    spec={'package':meta['package'],'version':'0.1','versionCode':1,'minSdk':32,'targetSdk':32,'certificateSha256':cert[0],
        'apkModified':False,'url':'http://127.0.0.1:8794/'+archive.name,'archiveBytes':archive.stat().st_size,
        'archiveSha256':hashlib.file_digest(archive.open('rb'),'sha256').hexdigest(),'files':[{'name':'base.apk','bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest()}],
        'proof':'media-av-v1','requireCleanAudioClose':True,'requireAllCameras':True,'requireCameraRecorder':True,'requireCameraRecorderAudio':True}
    (out/'app-spec.json').write_text(json.dumps(spec,indent=2)+'\n')
    meta.update(scope='Ordinary application UID with normal CAMERA/RECORD_AUDIO runtime permissions; public AudioTrack, AudioRecord, all-camera Camera2 YUV/JPEG and MediaRecorder Surface AVC plus microphone AAC in MP4, decoded pixels/PCM and overlapping timestamps. No runtime pass claimed.',runtimeValidated=False,
        sources={str(path.relative_to(ROOT)):hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted((ROOT/'runtime/tests/api32/avapp').iterdir()) if path.is_file()})
    for name in ['tools/build_api32_av_app.py','tools/build_api32_guest_app.py','runtime/tools/PocketManifest.java']:
        meta['sources'][name]=hashlib.sha256((ROOT/name).read_bytes()).hexdigest()
    meta['certificateSha256']=cert[0];meta['appSpec']=spec
    (out/'build.json').write_text(json.dumps(meta,indent=2)+'\n')
    print('API32_AV_APP_BUILD_PASS',apk)
