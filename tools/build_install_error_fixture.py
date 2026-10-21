#!/usr/bin/env python3
"""Build a real APK requiring Android 10 to exercise Android 9 rejection."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'out/install-errors'
TOOLS=ROOT.parent/'helloworldandroid'
ANDROID=TOOLS/'vendor/android-platforms/android-36/android.jar'
R8=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar'
def run(*argv):subprocess.run(list(map(str,argv)),cwd=ROOT,check=True)
for p in ('classes','dex'):(OUT/p).mkdir(parents=True,exist_ok=True)
run('make','-f','runtime/Makefile','out/runtime/tools/PocketManifest.class')
run('javac','--release','8','-cp',ANDROID,'-d',OUT/'classes',ROOT/'runtime/tests/install-errors/FutureActivity.java')
run('jar','cf',OUT/'classes.jar','-C',OUT/'classes','.')
run('java','-cp',R8,'com.android.tools.r8.D8','--lib',ANDROID,'--min-api','29','--output',OUT/'dex',OUT/'classes.jar')
run('java','-cp',str(TOOLS/'build/tools')+':'+str(ROOT/'out/runtime/tools'),'PocketManifest',ROOT/'runtime/tests/install-errors/AndroidManifest.xml',OUT/'AndroidManifest.xml','29','29','1','0.1')
run('java','-cp',TOOLS/'build/tools','com.example.build.ApkBuilder',OUT/'AndroidManifest.xml',OUT/'dex/classes.dex',OUT/'unsigned.apk')
run('java','-cp',TOOLS/'build/apksigner.jar','com.android.apksigner.ApkSignerTool','sign','--ks',ROOT/'out/debug.keystore','--ks-pass','pass:android','--key-pass','pass:android','--out',OUT/'future.apk',OUT/'unsigned.apk')
run('java','-cp',TOOLS/'build/apksigner.jar','com.android.apksigner.ApkSignerTool','verify',OUT/'future.apk')
print(OUT/'future.apk')
