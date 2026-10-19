# SPDX-License-Identifier: GPL-3.0-only
BOOT_OUT := out/boot
HIDL_GEN ?= $(or $(shell command -v hidl-gen 2>/dev/null),$(abspath ../s20fe-hardware-lab/artifacts/lineage/source/out/host/linux-x86/bin/hidl-gen))
.PHONY: boot-verify boot-overlay
BASE_APK ?= out/boot/baseline.apk
boot-overlay: $(BOOT_OUT)/build.stamp $(BOOT_OUT)/classes.dex $(BOOT_OUT)/AndroidManifest.xml $(BOOT_OUT)/pocket-zygote.jar out/runtime-native/libpocket_runtime.so
	python3 tools/pack_boot_overlay.py --base-apk $(BASE_APK)
BOOT_GUEST_JAVA := runtime/api28/ZygoteBootstrap.java runtime/api28/ZygoteChild.java runtime/direct/FrameworkProbe.java
$(BOOT_OUT)/pocket-zygote.jar: $(BOOT_GUEST_JAVA) $(ANDROID_JAR) $(R8)
	rm -rf $(BOOT_OUT)/guest-classes
	mkdir -p $(BOOT_OUT)/guest-classes $(BOOT_OUT)/guest-dex
	javac --release 8 -classpath $(ANDROID_JAR) -d $(BOOT_OUT)/guest-classes $(BOOT_GUEST_JAVA)
	jar cf $(BOOT_OUT)/guest-classes.jar -C $(BOOT_OUT)/guest-classes .
	java -cp $(R8) com.android.tools.r8.D8 --lib $(ANDROID_JAR) --min-api 28 --output $(BOOT_OUT)/guest-dex $(BOOT_OUT)/guest-classes.jar
	jar cf $@ -C $(BOOT_OUT)/guest-dex classes.dex
$(BOOT_OUT)/build.stamp: $(API28_NATIVE) out/runtime/guest-test.apk tools/build_boot.py tools/build_boot_zygote.py tools/boot_storage_protocol.py tools/prepare_boot_resources.py runtime/direct/zygote-system-server.patch runtime/direct/boot_runtime.c runtime/direct/boot_probe.c runtime/direct/boot_services.cpp runtime/direct/boot_storage.cpp runtime/direct/boot_network.cpp runtime/direct/boot_audio.cpp runtime/direct/boot_audio_probe.cpp runtime/direct/boot_hidl.cpp runtime/direct/boot_binder.cpp runtime/direct/hidl_codegen_compat.h runtime/direct/boot_egl.c runtime/direct/boot_health.cpp runtime/direct/boot_health.h runtime/direct/boot_hardware.cpp runtime/direct/boot_hardware.h runtime/direct/boot_hw_main.c runtime/direct/guest_paths.c runtime/direct/guest_paths.h
	python3 tools/build_boot.py --ndk $(NDK) --hidl-gen $(HIDL_GEN)

$(BOOT_OUT)/classes.dex: $(API28_JAVA) runtime/direct/BootProbeActivity.java runtime/direct/BootBattery.java runtime/direct/BootOverlay.java $(ANDROID_JAR) $(R8)
	mkdir -p $(BOOT_OUT)/classes $(BOOT_OUT)/dex
	javac --release 8 -classpath $(ANDROID_JAR) -d $(BOOT_OUT)/classes $(API28_JAVA) runtime/direct/BootProbeActivity.java runtime/direct/BootBattery.java runtime/direct/BootOverlay.java
	jar cf $(BOOT_OUT)/classes.jar -C $(BOOT_OUT)/classes .
	java -cp $(R8) com.android.tools.r8.D8 --lib $(ANDROID_JAR) --min-api 28 --output $(BOOT_OUT)/dex $(BOOT_OUT)/classes.jar
	cp $(BOOT_OUT)/dex/classes.dex $@

$(BOOT_OUT)/AndroidManifest.xml: runtime/direct/BootManifest.xml $(OUT)/tools/PocketManifest.class
	mkdir -p $(BOOT_OUT)
	java -cp $(TOOLS):$(OUT)/tools PocketManifest $< $@ 28 28 1 0.1-direct-platform

$(BOOT_OUT)/base.apk: $(BOOT_OUT)/classes.dex $(BOOT_OUT)/AndroidManifest.xml
	java -cp $(TOOLS) com.example.build.ApkBuilder $(BOOT_OUT)/AndroidManifest.xml $(BOOT_OUT)/classes.dex $@

$(BOOT_OUT)/packed.apk: $(BOOT_OUT)/base.apk $(BOOT_OUT)/build.stamp $(API28_OUT)/art-probe.dex $(BOOT_OUT)/pocket-zygote.jar out/runtime-native/libpocket_runtime.so tools/pack_boot_probe.py runtime/NOTICE
	python3 tools/pack_boot_probe.py

$(BOOT_OUT)/aosp-pocket-boot-probe.apk: $(BOOT_OUT)/packed.apk out/debug.keystore
	java -cp $(SIGNER) com.android.apksigner.ApkSignerTool sign --ks out/debug.keystore --ks-pass pass:android --key-pass pass:android --out $@ $<

boot-verify: $(BOOT_OUT)/aosp-pocket-boot-probe.apk
	java -cp $(SIGNER) com.android.apksigner.ApkSignerTool verify --verbose $<
	python3 tools/validate_boot_probe.py $< --ndk $(NDK)
