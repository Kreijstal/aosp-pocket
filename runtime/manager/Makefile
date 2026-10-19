# SPDX-License-Identifier: GPL-3.0-only
# Production upgrade: make -f runtime/Makefile manager-verify
# Parallel development install: add MANAGER_PACKAGE=dev.kreijstal.aosppocket.managerdev MANAGER_OUT=out/manager-dev
MANAGER_OUT ?= out/manager
MANAGER_PACKAGE ?= dev.kreijstal.aosppocket.runtime
MANAGER_GUEST_JAVA := $(BOOT_GUEST_JAVA) runtime/direct/GuestControl.java
.PHONY: manager-verify
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
	java -cp $(TOOLS):$(OUT)/tools PocketManifest $< $@ 28 28 4 0.4-android9-manager

$(MANAGER_OUT)/base.apk: $(OUT)/classes.dex $(MANAGER_OUT)/AndroidManifest.xml
	java -cp $(TOOLS) com.example.build.ApkBuilder $(MANAGER_OUT)/AndroidManifest.xml $(OUT)/classes.dex $@

$(MANAGER_OUT)/packed.apk: $(MANAGER_OUT)/base.apk $(OUT)/packed.apk $(BOOT_OUT)/build.stamp out/manager-guest/pocket-control.jar tools/pack_manager.py
	python3 tools/pack_manager.py --out $(MANAGER_OUT)

$(MANAGER_OUT)/aosp-pocket-manager.apk: $(MANAGER_OUT)/packed.apk out/debug.keystore
	java -cp $(SIGNER) com.android.apksigner.ApkSignerTool sign --ks out/debug.keystore --ks-pass pass:android --key-pass pass:android --out $@ $<

manager-verify: $(MANAGER_OUT)/aosp-pocket-manager.apk
	java -cp $(SIGNER) com.android.apksigner.ApkSignerTool verify --verbose $<
	python3 tools/validate_manager_apk.py $< --ndk $(NDK)
