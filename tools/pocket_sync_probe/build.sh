#!/usr/bin/env bash
# Build the pocket_sync keystore probe APK (universal: no native code).
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
tc=${TOOLCHAIN:-$HOME/git/helloworldandroid}
android_jar=$tc/vendor/android-platforms/android-33/android.jar
out=${1:-$HOME/.cache/pocket-sync/probe}
rm -rf "$out/classes" && mkdir -p "$out/classes" "$out/tools"
javac -source 8 -target 8 -bootclasspath "$android_jar" -classpath "$android_jar" -d "$out/classes" \
    $(find "$here/src" -name '*.java') 2>&1 | grep -v -E 'bootstrap classpath|source value 8|target value 8|To suppress|^warning|^[0-9]+ warnings?$' || true
java -cp "$tc/build/dx.jar" com.android.dx.command.Main --dex --output="$out/classes.dex" "$out/classes"
javac -source 8 -target 8 -d "$out/tools" $(find "$tc/tools/src/com" -name '*.java') 2>/dev/null
java -cp "$out/tools" com.example.build.BinaryXmlEncoder "$here/AndroidManifest.xml" "$out/AndroidManifest.xml" 28 30 1 1.0
java -cp "$out/tools" com.example.build.ApkBuilder "$out/AndroidManifest.xml" "$out/classes.dex" "$out/unsigned.apk"
java -cp "$tc/build/apksigner.jar" com.android.apksigner.ApkSignerTool sign --ks "$tc/build/debug.keystore" \
    --ks-pass pass:android --key-pass pass:android --out "$out/psprobe.apk" "$out/unsigned.apk"
echo "$out/psprobe.apk"
