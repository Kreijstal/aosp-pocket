# SPDX-License-Identifier: Apache-2.0
# Reuse the existing source-built Java APK tools without changing their workspace.
TOOLCHAIN ?= $(abspath ../helloworldandroid)
ANDROID_JAR ?= $(TOOLCHAIN)/vendor/android-platforms/android-36/android.jar
NDK ?= $(HOME)/.local/share/android-neutral-test/sdk/ndk/android-ndk-r27c
LLVM := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin
CC_ANDROID := $(LLVM)/aarch64-linux-android28-clang
TOOLS := $(TOOLCHAIN)/build/tools
DX := $(TOOLCHAIN)/build/dx.jar
SIGNER := $(TOOLCHAIN)/build/apksigner.jar
APK := out/aosp-pocket.apk
JAVA := $(shell find app/src -name '*.java')
ELF_FLAGS := -O2 -Wall -Wextra -Werror -fPIE -pie -Wl,-z,max-page-size=16384
.DELETE_ON_ERROR:

.PHONY: all verify clean
all: $(APK)

out/classes.dex: $(JAVA) $(ANDROID_JAR) $(DX)
	mkdir -p out/classes
	javac --release 8 -classpath $(ANDROID_JAR) -d out/classes $(JAVA)
	java -cp $(DX) com.android.dx.command.Main --dex --output=$@ out/classes

out/AndroidManifest.xml: app/AndroidManifest.xml Makefile
	mkdir -p out
	java -cp $(TOOLS) com.example.build.BinaryXmlEncoder $< $@ 28 36 1 0.1-bootstrap

out/lib/arm64-v8a/libpocket_probe.so: native/probe.c Makefile
	mkdir -p $(@D)
	$(CC_ANDROID) $(ELF_FLAGS) $< -o $@
	$(LLVM)/llvm-strip $@

out/lib/arm64-v8a/libtoybox_exec.so: tools/build_toybox.py guest/toybox.config sources.lock.json vendor/toybox/Makefile Makefile
	python3 tools/build_toybox.py --ndk $(NDK)

out/base.apk: out/classes.dex out/AndroidManifest.xml
	java -cp $(TOOLS) com.example.build.ApkBuilder out/AndroidManifest.xml out/classes.dex $@

out/packed.apk: out/base.apk out/lib/arm64-v8a/libtoybox_exec.so out/lib/arm64-v8a/libpocket_probe.so tools/pack.py sources.lock.json vendor/toybox/LICENSE LICENSE
	python3 tools/pack.py

out/debug.keystore:
	mkdir -p out
	keytool -genkeypair -keystore $@ -alias debug -keyalg RSA -keysize 2048 -validity 3650 -storepass android -keypass android -dname 'CN=AOSP Pocket Development'

$(APK): out/packed.apk out/debug.keystore
	java -cp $(SIGNER) com.android.apksigner.ApkSignerTool sign --ks out/debug.keystore --ks-pass pass:android --key-pass pass:android --out $@ $<

verify: $(APK)
	java -cp $(SIGNER) com.android.apksigner.ApkSignerTool verify --verbose $(APK)
	python3 tools/validate_apk.py $(APK)

# Keep the development signing identity so an ordinary rebuild can update the app.
clean:
	python3 -c 'import pathlib,shutil; [(shutil.rmtree(p) if p.is_dir() else p.unlink()) for p in pathlib.Path("out").iterdir() if p.name != "debug.keystore"]'
