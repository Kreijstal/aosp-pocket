# SPDX-License-Identifier: GPL-3.0-only
# Invoke from the project root: make -f runtime/Makefile verify
TOOLCHAIN ?= $(abspath ../helloworldandroid)
ANDROID_JAR ?= $(TOOLCHAIN)/vendor/android-platforms/android-36/android.jar
R8 ?= $(abspath ../s20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar)
NDK ?= $(HOME)/.local/share/android-neutral-test/sdk/ndk/android-ndk-r27c
LLVM := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin
TOOLS := $(TOOLCHAIN)/build/tools
SIGNER := $(TOOLCHAIN)/build/apksigner.jar
JAVA := $(shell find runtime/app/src -name '*.java')
OUT := out/runtime
BUNDLED_GUEST ?= out/aosp-pocket-guest-arm64.zip
.DEFAULT_GOAL := verify
.PHONY: native verify smoke check-host check-profiles
.DELETE_ON_ERROR:

NATIVE_INPUTS := $(shell find runtime/native -type f) runtime/anbox-renderer.patch runtime/anbox-input.patch runtime/proot-android.patch runtime/talloc.cross tools/build_runtime_native.py sources.lock.json vendor/anbox/CMakeLists.txt vendor/proot/src/GNUmakefile
native: out/runtime-native/libpocket_runtime.so out/runtime-native/libproot.so

out/runtime-native/libpocket_runtime.so out/runtime-native/libproot.so &: $(NATIVE_INPUTS)
	python3 tools/build_runtime_native.py --ndk $(NDK)

out/runtime-native/libpocket_binder_bootstrap.so: runtime/guest-binder-bootstrap.cpp tools/build_guest_binder.py sources.lock.json
	python3 tools/build_guest_binder.py --ndk $(NDK)

$(OUT)/classes.dex: $(JAVA) $(ANDROID_JAR) $(R8)
	mkdir -p $(OUT)/classes $(OUT)/dex
	javac --release 8 -classpath $(ANDROID_JAR) -d $(OUT)/classes $(JAVA)
	jar cf $(OUT)/classes.jar -C $(OUT)/classes .
	java -cp $(R8) com.android.tools.r8.D8 --lib $(ANDROID_JAR) --min-api 28 --output $(OUT)/dex $(OUT)/classes.jar
	cp $(OUT)/dex/classes.dex $@

$(OUT)/tools/PocketManifest.class: runtime/tools/PocketManifest.java
	mkdir -p $(OUT)/tools
	javac --release 8 -classpath $(TOOLS) -d $(OUT)/tools $<

$(OUT)/AndroidManifest.xml: runtime/app/AndroidManifest.xml runtime/Makefile $(OUT)/tools/PocketManifest.class
	mkdir -p $(OUT)
	java -cp $(TOOLS):$(OUT)/tools PocketManifest $< $@ 28 28 3 0.3-bundled

$(OUT)/base.apk: $(OUT)/classes.dex $(OUT)/AndroidManifest.xml
	java -cp $(TOOLS) com.example.build.ApkBuilder $(OUT)/AndroidManifest.xml $(OUT)/classes.dex $@

out/aosp-pocket-guest-arm64.zip: tools/prepare_default_guest.py tools/pack_guest_image.py tools/build_guest_binder.py sources.lock.json
	python3 tools/prepare_default_guest.py $@

$(OUT)/packed.apk: $(OUT)/base.apk out/runtime-native/libpocket_runtime.so out/runtime-native/libproot.so out/runtime-native/libpocket_binder_bootstrap.so runtime/guest.sh tools/pack_runtime.py sources.lock.json runtime/NOTICE $(wildcard runtime/licenses/*) $(BUNDLED_GUEST)
	python3 tools/pack_runtime.py --ndk $(NDK) --guest $(BUNDLED_GUEST)

out/debug.keystore:
	mkdir -p out
	keytool -genkeypair -keystore $@ -alias debug -keyalg RSA -keysize 2048 -validity 3650 -storepass android -keypass android -dname 'CN=AOSP Pocket Development'

$(OUT)/aosp-pocket-runtime.apk: $(OUT)/packed.apk out/debug.keystore
	java -cp $(SIGNER) com.android.apksigner.ApkSignerTool sign --ks out/debug.keystore --ks-pass pass:android --key-pass pass:android --out $@ $<

verify: $(OUT)/aosp-pocket-runtime.apk
	java -cp $(SIGNER) com.android.apksigner.ApkSignerTool verify --verbose $<
	python3 tools/validate_runtime_apk.py $< --ndk $(NDK)

out/connections-test: runtime/tests/connections.cpp runtime/native/overrides/anbox/network/connections.h
	mkdir -p out
	$(CXX) -std=c++17 -pthread -Iruntime/native/overrides $< -o $@

check-host: out/connections-test
	timeout 5s $<

smoke: $(OUT)/guest-test.apk $(OUT)/property-probe

$(OUT)/property-probe: runtime/smoke/property_probe.c
	mkdir -p $(OUT)
	$(LLVM)/aarch64-linux-android25-clang -O2 -Wall -Wextra -Werror -fPIE -pie -Wl,-z,max-page-size=16384 $< -o $@

$(OUT)/guest-test.apk: $(shell find runtime/smoke -name '*.java' -o -name '*.xml') out/debug.keystore
	mkdir -p $(OUT)/smoke-classes
	javac --release 8 -classpath $(ANDROID_JAR) -d $(OUT)/smoke-classes $(shell find runtime/smoke/src -name '*.java')
	java -cp $(TOOLCHAIN)/build/dx.jar com.android.dx.command.Main --dex --output=$(OUT)/guest-test.dex $(OUT)/smoke-classes
	java -cp $(TOOLS) com.example.build.BinaryXmlEncoder runtime/smoke/AndroidManifest.xml $(OUT)/smoke-manifest.xml 23 25 1 1.0
	java -cp $(TOOLS) com.example.build.ApkBuilder $(OUT)/smoke-manifest.xml $(OUT)/guest-test.dex $(OUT)/guest-test-unsigned.apk
	java -cp $(SIGNER) com.android.apksigner.ApkSignerTool sign --ks out/debug.keystore --ks-pass pass:android --key-pass pass:android --v1-signing-enabled true --v2-signing-enabled true --v3-signing-enabled false --out $@ $(OUT)/guest-test-unsigned.apk
	java -cp $(SIGNER) com.android.apksigner.ApkSignerTool verify --verbose $@

include runtime/api28/Makefile
include runtime/direct/Makefile
include runtime/direct/boot.mk

check-profiles:
	mkdir -p out/profile-tests
	javac --release 8 -d out/profile-tests runtime/app/src/dev/kreijstal/aosppocket/runtime/GuestProfile.java runtime/tests/GuestProfileTest.java
	java -cp out/profile-tests dev.kreijstal.aosppocket.runtime.GuestProfileTest
