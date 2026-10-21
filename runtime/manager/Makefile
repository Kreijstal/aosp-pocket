# SPDX-License-Identifier: GPL-3.0-only
# Production upgrade: make -f runtime/Makefile manager-verify
# Parallel development install: add MANAGER_PACKAGE=dev.kreijstal.aosppocket.managerdev MANAGER_OUT=out/manager-dev
MANAGER_OUT ?= out/manager
MANAGER_PACKAGE ?= dev.kreijstal.aosppocket.runtime
MANAGER_GUEST_JAVA := $(BOOT_GUEST_JAVA) runtime/direct/GuestControl.java runtime/direct/HostDevices.java runtime/direct/HostNetwork.java runtime/app/src/dev/kreijstal/aosppocket/bridge/HostWire.java runtime/app/src/dev/kreijstal/aosppocket/bridge/DisplayGeometry.java
.PHONY: manager-verify
.PHONY: check-admin
.PHONY: check-direct-image-links
check-direct-image-links: out/runtime/classes.dex
	mkdir -p out/direct-image-tests
	javac --release 8 -cp $(ANDROID_JAR):out/runtime/classes -d out/direct-image-tests runtime/tests/DirectImageLinksFixture.java
	java -cp $(ANDROID_JAR):out/runtime/classes:out/direct-image-tests dev.kreijstal.aosppocket.runtime.DirectImageLinksFixture

check-admin:
	mkdir -p out/admin-tests
	javac --release 8 -d out/admin-tests runtime/app/src/dev/kreijstal/aosppocket/runtime/AdminServer.java runtime/tests/AdminServerFixture.java
	python3 runtime/tests/test_admin.py

.PHONY: key-attestation-probe
key-attestation-probe: out/key-attestation/probe.apk out/key-attestation/control.jar out/key-attestation/launch
out/key-attestation/launch: runtime/tests/key-attestation/launch.c
	mkdir -p out/key-attestation
	$(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang -std=c11 -O2 -Wall -Wextra -Werror $< -Wl,-z,max-page-size=16384 -o $@
out/key-attestation/probe.apk: runtime/tests/key-attestation/MainActivity.java runtime/tests/key-attestation/AndroidManifest.xml $(ANDROID_JAR) $(R8) out/debug.keystore $(OUT)/tools/PocketManifest.class
	mkdir -p out/key-attestation/classes out/key-attestation/dex
	javac --release 8 -cp $(ANDROID_JAR) -d out/key-attestation/classes runtime/tests/key-attestation/MainActivity.java
	jar cf out/key-attestation/classes.jar -C out/key-attestation/classes .
	java -cp $(R8) com.android.tools.r8.D8 --lib $(ANDROID_JAR) --min-api 28 --output out/key-attestation/dex out/key-attestation/classes.jar
	java -cp $(TOOLS):$(OUT)/tools PocketManifest runtime/tests/key-attestation/AndroidManifest.xml out/key-attestation/AndroidManifest.xml 28 28 2 0.2
	java -cp $(TOOLS) com.example.build.ApkBuilder out/key-attestation/AndroidManifest.xml out/key-attestation/dex/classes.dex out/key-attestation/unsigned.apk
	java -cp $(SIGNER) com.android.apksigner.ApkSignerTool sign --ks out/debug.keystore --ks-pass pass:android --key-pass pass:android --out $@ out/key-attestation/unsigned.apk
out/key-attestation/control.jar: runtime/tests/key-attestation/Control.java runtime/app/src/dev/kreijstal/aosppocket/bridge/DisplayGeometry.java $(ANDROID_JAR) $(R8)
	mkdir -p out/key-attestation/control-classes out/key-attestation/control-dex
	javac --release 8 -cp $(ANDROID_JAR) -d out/key-attestation/control-classes runtime/tests/key-attestation/Control.java runtime/app/src/dev/kreijstal/aosppocket/bridge/DisplayGeometry.java
	jar cf out/key-attestation/control-classes.jar -C out/key-attestation/control-classes .
	java -cp $(R8) com.android.tools.r8.D8 --lib $(ANDROID_JAR) --min-api 28 --output out/key-attestation/control-dex out/key-attestation/control-classes.jar
	jar cf $@ -C out/key-attestation/control-dex classes.dex
.PHONY: network-permission-probe
network-permission-probe: out/network-permissions-test/probe.apk
out/network-permissions-test/probe.apk: runtime/tests/network-permission/MainActivity.java runtime/tests/network-permission/AndroidManifest.xml $(ANDROID_JAR) $(R8) $(OUT)/tools/PocketManifest.class out/debug.keystore
	mkdir -p out/network-permissions-test/classes out/network-permissions-test/dex
	javac --release 8 -cp $(ANDROID_JAR) -d out/network-permissions-test/classes runtime/tests/network-permission/MainActivity.java
	jar cf out/network-permissions-test/classes.jar -C out/network-permissions-test/classes .
	java -cp $(R8) com.android.tools.r8.D8 --lib $(ANDROID_JAR) --min-api 28 --output out/network-permissions-test/dex out/network-permissions-test/classes.jar
	java -cp $(TOOLS):$(OUT)/tools PocketManifest runtime/tests/network-permission/AndroidManifest.xml out/network-permissions-test/AndroidManifest.xml 28 28 1 0.1
	java -cp $(TOOLS) com.example.build.ApkBuilder out/network-permissions-test/AndroidManifest.xml out/network-permissions-test/dex/classes.dex out/network-permissions-test/unsigned.apk
	java -cp $(SIGNER) com.android.apksigner.ApkSignerTool sign --ks out/debug.keystore --ks-pass pass:android --key-pass pass:android --out $@ out/network-permissions-test/unsigned.apk
.PHONY: install-input-probe
install-input-probe: out/install-input/probe.apk
out/install-input/probe.apk: runtime/tests/install-input/InstallInputTest.java runtime/tests/install-input/AndroidManifest.xml $(OUT)/classes.dex $(OUT)/tools/PocketManifest.class out/debug.keystore
	mkdir -p out/install-input/test-classes out/install-input/dex
	javac --release 8 -cp $(ANDROID_JAR):$(OUT)/classes.jar -d out/install-input/test-classes runtime/tests/install-input/InstallInputTest.java
	jar cf out/install-input/test.jar -C out/install-input/test-classes .
	java -cp $(R8) com.android.tools.r8.D8 --lib $(ANDROID_JAR) --min-api 28 --output out/install-input/dex $(OUT)/classes.jar out/install-input/test.jar
	java -cp $(TOOLS):$(OUT)/tools PocketManifest runtime/tests/install-input/AndroidManifest.xml out/install-input/AndroidManifest.xml 28 28 1 0.1
	java -cp $(TOOLS) com.example.build.ApkBuilder out/install-input/AndroidManifest.xml out/install-input/dex/classes.dex out/install-input/unsigned.apk
	java -cp $(SIGNER) com.android.apksigner.ApkSignerTool sign --ks out/debug.keystore --ks-pass pass:android --key-pass pass:android --out $@ out/install-input/unsigned.apk
out/composer/hwcomposer.pocket.so: runtime/direct/pocket_composer.c runtime/direct/composer_slots.h tools/build_composer.py sources.lock.json
	python3 tools/build_composer.py --ndk $(NDK)
.PHONY: display-probe
display-probe: out/manager-guest/display-test.apk
out/manager-guest/display-test.apk: runtime/tests/display/MainActivity.java runtime/tests/display/AndroidManifest.xml $(ANDROID_JAR) $(R8) out/debug.keystore $(OUT)/tools/PocketManifest.class
	mkdir -p out/manager-guest/display-classes out/manager-guest/display-dex
	javac --release 8 -classpath $(ANDROID_JAR) -d out/manager-guest/display-classes runtime/tests/display/MainActivity.java
	jar cf out/manager-guest/display-classes.jar -C out/manager-guest/display-classes .
	java -cp $(R8) com.android.tools.r8.D8 --lib $(ANDROID_JAR) --min-api 28 --output out/manager-guest/display-dex out/manager-guest/display-classes.jar
	java -cp $(TOOLS):$(OUT)/tools PocketManifest runtime/tests/display/AndroidManifest.xml out/manager-guest/display-manifest.xml 28 28 1 0.1-display-test
	java -cp $(TOOLS) com.example.build.ApkBuilder out/manager-guest/display-manifest.xml out/manager-guest/display-dex/classes.dex out/manager-guest/display-unsigned.apk
	java -cp $(SIGNER) com.android.apksigner.ApkSignerTool sign --ks out/debug.keystore --ks-pass pass:android --key-pass pass:android --out $@ out/manager-guest/display-unsigned.apk
.PHONY: host-network-probe
host-network-probe: out/manager-guest/host-network-test.apk
out/manager-guest/host-network-test.apk: runtime/tests/host-network/MainActivity.java runtime/tests/host-network/AndroidManifest.xml $(ANDROID_JAR) $(R8) out/debug.keystore $(OUT)/tools/PocketManifest.class
	mkdir -p out/manager-guest/network-classes out/manager-guest/network-dex
	javac --release 8 -classpath $(ANDROID_JAR) -d out/manager-guest/network-classes runtime/tests/host-network/MainActivity.java
	jar cf out/manager-guest/network-classes.jar -C out/manager-guest/network-classes .
	java -cp $(R8) com.android.tools.r8.D8 --lib $(ANDROID_JAR) --min-api 28 --output out/manager-guest/network-dex out/manager-guest/network-classes.jar
	java -cp $(TOOLS):$(OUT)/tools PocketManifest runtime/tests/host-network/AndroidManifest.xml out/manager-guest/network-manifest.xml 28 28 1 0.1-network-test
	java -cp $(TOOLS) com.example.build.ApkBuilder out/manager-guest/network-manifest.xml out/manager-guest/network-dex/classes.dex out/manager-guest/network-unsigned.apk
	java -cp $(SIGNER) com.android.apksigner.ApkSignerTool sign --ks out/debug.keystore --ks-pass pass:android --key-pass pass:android --out $@ out/manager-guest/network-unsigned.apk
out/manager-guest/host-phone.apk: runtime/host-phone/MainActivity.java runtime/host-phone/AndroidManifest.xml runtime/app/src/dev/kreijstal/aosppocket/bridge/HostWire.java $(ANDROID_JAR) $(R8) out/debug.keystore $(OUT)/tools/PocketManifest.class
	mkdir -p out/manager-guest/phone-classes out/manager-guest/phone-dex
	javac --release 8 -classpath $(ANDROID_JAR) -d out/manager-guest/phone-classes runtime/host-phone/MainActivity.java runtime/app/src/dev/kreijstal/aosppocket/bridge/HostWire.java
	jar cf out/manager-guest/phone-classes.jar -C out/manager-guest/phone-classes .
	java -cp $(R8) com.android.tools.r8.D8 --lib $(ANDROID_JAR) --min-api 28 --output out/manager-guest/phone-dex out/manager-guest/phone-classes.jar
	java -cp $(TOOLS):$(OUT)/tools PocketManifest runtime/host-phone/AndroidManifest.xml out/manager-guest/phone-manifest.xml 28 28 1 0.1-host-phone
	java -cp $(TOOLS) com.example.build.ApkBuilder out/manager-guest/phone-manifest.xml out/manager-guest/phone-dex/classes.dex out/manager-guest/phone-unsigned.apk
	java -cp $(SIGNER) com.android.apksigner.ApkSignerTool sign --ks out/debug.keystore --ks-pass pass:android --key-pass pass:android --out $@ out/manager-guest/phone-unsigned.apk
out/manager-guest/pocket-control.jar: $(MANAGER_GUEST_JAVA) $(ANDROID_JAR) $(R8)
	rm -rf out/manager-guest/classes
	mkdir -p out/manager-guest/classes out/manager-guest/dex
	javac --release 8 -classpath $(ANDROID_JAR) -d out/manager-guest/classes $(MANAGER_GUEST_JAVA)
	jar cf out/manager-guest/classes.jar -C out/manager-guest/classes .
	java -cp $(R8) com.android.tools.r8.D8 --lib $(ANDROID_JAR) --min-api 28 --output out/manager-guest/dex out/manager-guest/classes.jar
	jar cf $@ -C out/manager-guest/dex classes.dex

$(MANAGER_OUT)/manifest-source.xml: runtime/app/AndroidManifest.xml runtime/manager/Makefile
	mkdir -p $(MANAGER_OUT)
	python3 -c 'from pathlib import Path; s=Path("runtime/app/AndroidManifest.xml").read_text(); s=s.replace("android:name=\".","android:name=\"dev.kreijstal.aosppocket.runtime."); s=s.replace("dev.kreijstal.aosppocket.runtime.manager\"","$(MANAGER_PACKAGE).manager\"").replace("dev.kreijstal.aosppocket.runtime.android\"","$(MANAGER_PACKAGE).android\""); s=s.replace("package=\"dev.kreijstal.aosppocket.runtime\"","package=\"$(MANAGER_PACKAGE)\""); Path("$@").write_text(s)'

$(MANAGER_OUT)/AndroidManifest.xml: $(MANAGER_OUT)/manifest-source.xml $(OUT)/tools/PocketManifest.class
	java -cp $(TOOLS):$(OUT)/tools PocketManifest $< $@ 28 28 25 0.25

$(MANAGER_OUT)/base.apk: $(OUT)/classes.dex $(MANAGER_OUT)/AndroidManifest.xml
	java -cp $(TOOLS) com.example.build.ApkBuilder $(MANAGER_OUT)/AndroidManifest.xml $(OUT)/classes.dex $@

$(MANAGER_OUT)/packed.apk: $(MANAGER_OUT)/base.apk $(OUT)/packed.apk $(BOOT_OUT)/build.stamp out/manager-guest/pocket-control.jar out/manager-guest/host-phone.apk out/runtime-native/libpocket_frames.so out/composer/hwcomposer.pocket.so tools/pack_manager.py tools/pocketctl.py
	python3 tools/pack_manager.py --out $(MANAGER_OUT)

$(MANAGER_OUT)/aosp-pocket-manager.apk: $(MANAGER_OUT)/packed.apk out/debug.keystore
	java -cp $(SIGNER) com.android.apksigner.ApkSignerTool sign --ks out/debug.keystore --ks-pass pass:android --key-pass pass:android --out $@ $<

manager-verify: $(MANAGER_OUT)/aosp-pocket-manager.apk
	java -cp $(SIGNER) com.android.apksigner.ApkSignerTool verify --verbose $<
	python3 tools/validate_manager_apk.py $< --ndk $(NDK)
