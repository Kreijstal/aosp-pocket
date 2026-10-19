# SPDX-License-Identifier: GPL-3.0-only
DIRECT_OUT := out/direct
.PHONY: direct-native direct-verify direct-check-host

direct-native: $(DIRECT_OUT)/build.stamp $(DIRECT_OUT)/rootfs/system/bin/pocket-direct-probe
$(DIRECT_OUT)/build.stamp $(DIRECT_OUT)/rootfs/system/bin/pocket-direct-probe &: runtime/direct/guest_paths.c runtime/direct/guest_paths.h runtime/direct/probe.c tools/build_direct.py tools/prepare_api28.py sources.lock.json
	python3 tools/build_direct.py --ndk $(NDK)

$(DIRECT_OUT)/host-probe: runtime/direct/guest_paths.c runtime/direct/guest_paths.h runtime/direct/probe.c tools/build_direct.py
	python3 tools/build_direct.py --host

direct-check-host: $(DIRECT_OUT)/host-probe
	python3 tools/validate_direct_host.py

$(DIRECT_OUT)/classes.dex: runtime/direct/DirectProbeActivity.java $(ANDROID_JAR) $(R8)
	mkdir -p $(DIRECT_OUT)/classes $(DIRECT_OUT)/dex
	javac --release 8 -classpath $(ANDROID_JAR) -d $(DIRECT_OUT)/classes $<
	jar cf $(DIRECT_OUT)/classes.jar -C $(DIRECT_OUT)/classes .
	java -cp $(R8) com.android.tools.r8.D8 --lib $(ANDROID_JAR) --min-api 28 --output $(DIRECT_OUT)/dex $(DIRECT_OUT)/classes.jar
	cp $(DIRECT_OUT)/dex/classes.dex $@

$(DIRECT_OUT)/AndroidManifest.xml: runtime/direct/AndroidManifest.xml runtime/direct/Makefile $(OUT)/tools/PocketManifest.class
	mkdir -p $(DIRECT_OUT)
	java -cp $(TOOLS):$(OUT)/tools PocketManifest $< $@ 28 28 1 0.1-direct

$(DIRECT_OUT)/base.apk: $(DIRECT_OUT)/classes.dex $(DIRECT_OUT)/AndroidManifest.xml
	java -cp $(TOOLS) com.example.build.ApkBuilder $(DIRECT_OUT)/AndroidManifest.xml $(DIRECT_OUT)/classes.dex $@

$(DIRECT_OUT)/packed.apk: $(DIRECT_OUT)/base.apk $(DIRECT_OUT)/build.stamp out/runtime-native/libproot.so tools/pack_direct_probe.py runtime/NOTICE sources.lock.json runtime/licenses/talloc.txt runtime/licenses/GPL-3.0-only.txt vendor/proot/COPYING
	python3 tools/pack_direct_probe.py

$(DIRECT_OUT)/aosp-pocket-direct-probe.apk: $(DIRECT_OUT)/packed.apk out/debug.keystore
	java -cp $(SIGNER) com.android.apksigner.ApkSignerTool sign --ks out/debug.keystore --ks-pass pass:android --key-pass pass:android --out $@ $<

direct-verify: $(DIRECT_OUT)/aosp-pocket-direct-probe.apk
	java -cp $(SIGNER) com.android.apksigner.ApkSignerTool verify --verbose $<
	python3 tools/validate_direct_probe.py $< --ndk $(NDK)
