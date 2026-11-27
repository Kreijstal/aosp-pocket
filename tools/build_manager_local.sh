#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Build and verify the Manager's Java APK using local tools only.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
toolchain="$HOME/git/helloworldandroid"
android_jar="$toolchain/vendor/android-platforms/android-36/android.jar"
signer_jar="$toolchain/build/apksigner.jar"
build_tools="$toolchain/tools/src/com/example/build"
default_r8="$root/../s20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar"
r8_jar=${R8_JAR:-$toolchain/build/r8.jar}
if [[ -z ${R8_JAR:-} && ! -f "$r8_jar" ]]; then
    r8_jar=$default_r8
fi

javac_version=$(javac -version 2>&1)
if [[ $javac_version != 'javac 17' && $javac_version != 'javac 17.'* ]]; then
    printf 'JDK 17 is required, found %s\n' "$javac_version" >&2
    exit 1
fi
for input in "$android_jar" "$r8_jar" "$signer_jar" \
    "$build_tools/BinaryXmlEncoder.java" "$build_tools/ApkBuilder.java"; do
    if [[ ! -r "$input" ]]; then
        printf 'Missing local build input: %s\n' "$input" >&2
        exit 1
    fi
done

out="$root/out-local"
mkdir -p "$out"
work=$(mktemp -d "$out/.manager-build.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
mkdir -p "$work/classes" "$work/dex" "$work/tools"

cd "$root"
find runtime/app/src runtime/storage/src -name '*.java' -print > "$work/sources"
printf '%s\n' runtime/direct/BootBattery.java >> "$work/sources"
javac --release 8 -classpath "$android_jar" -d "$work/classes" "@$work/sources"
jar cf "$work/classes.jar" -C "$work/classes" .
java -cp "$r8_jar" com.android.tools.r8.D8 --lib "$android_jar" \
    --min-api 28 --output "$work/dex" "$work/classes.jar"

javac --release 8 -d "$work/tools" "$build_tools/BinaryXmlEncoder.java" \
    "$build_tools/ApkBuilder.java" runtime/tools/PocketManifest.java
java -cp "$work/tools" PocketManifest runtime/app/AndroidManifest.xml \
    "$work/AndroidManifest.xml" 28 28 78 0.78-local
java -cp "$work/tools" com.example.build.ApkBuilder "$work/AndroidManifest.xml" \
    "$work/dex/classes.dex" "$work/unsigned.apk"

keystore="$toolchain/build/debug.keystore"
if [[ ! -f "$keystore" ]]; then
    keystore="$out/debug.keystore"
    if [[ ! -f "$keystore" ]]; then
        keytool -genkeypair -keystore "$work/debug.keystore" -alias debug -keyalg RSA \
            -keysize 2048 -validity 3650 -storepass android -keypass android \
            -dname 'CN=AOSP Pocket Development'
        mv -n -- "$work/debug.keystore" "$keystore"
    fi
fi
signer=(java -cp "$signer_jar" com.android.apksigner.ApkSignerTool)
target="$out/aosp-pocket-manager-local.apk"
"${signer[@]}" sign --ks "$keystore" --ks-pass pass:android --key-pass pass:android \
    --v4-signing-enabled false --out "$work/manager.apk" "$work/unsigned.apk"
"${signer[@]}" verify "$work/manager.apk"
mv -f -- "$work/manager.apk" "$target"
printf 'Verified APK: %s\n' "$target"
