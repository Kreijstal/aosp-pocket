# SPDX-License-Identifier: GPL-3.0-only
# Production upgrade: make -f runtime/Makefile manager-verify
# Parallel development install: add MANAGER_PACKAGE=dev.kreijstal.aosppocket.managerdev MANAGER_OUT=out/manager-dev
MANAGER_OUT ?= out/manager
MANAGER_PACKAGE ?= dev.kreijstal.aosppocket.runtime
MANAGER_GUEST_JAVA := $(BOOT_GUEST_JAVA) runtime/direct/GuestControl.java runtime/direct/HostDevices.java runtime/direct/HostNetwork.java runtime/app/src/dev/kreijstal/aosppocket/bridge/HostWire.java runtime/app/src/dev/kreijstal/aosppocket/bridge/DisplayGeometry.java runtime/app/src/dev/kreijstal/aosppocket/bridge/SharedFrames.java runtime/app/src/dev/kreijstal/aosppocket/bridge/AnonymousFrames.java
.PHONY: manager-verify
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
	java -cp $(TOOLS):$(OUT)/tools PocketManifest $< $@ 28 28 9 0.9-shared-display

$(MANAGER_OUT)/base.apk: $(OUT)/classes.dex $(MANAGER_OUT)/AndroidManifest.xml
	java -cp $(TOOLS) com.example.build.ApkBuilder $(MANAGER_OUT)/AndroidManifest.xml $(OUT)/classes.dex $@

$(MANAGER_OUT)/packed.apk: $(MANAGER_OUT)/base.apk $(OUT)/packed.apk $(BOOT_OUT)/build.stamp out/manager-guest/pocket-control.jar out/manager-guest/host-phone.apk out/runtime-native/libpocket_frames.so tools/pack_manager.py
	python3 tools/pack_manager.py --out $(MANAGER_OUT)

$(MANAGER_OUT)/aosp-pocket-manager.apk: $(MANAGER_OUT)/packed.apk out/debug.keystore
	java -cp $(SIGNER) com.android.apksigner.ApkSignerTool sign --ks out/debug.keystore --ks-pass pass:android --key-pass pass:android --out $@ $<

manager-verify: $(MANAGER_OUT)/aosp-pocket-manager.apk
	java -cp $(SIGNER) com.android.apksigner.ApkSignerTool verify --verbose $<
	python3 tools/validate_manager_apk.py $< --ndk $(NDK)
