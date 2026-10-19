# SPDX-License-Identifier: GPL-3.0-only
# Included by runtime/Makefile; all outputs separate from the working guest.
API28_OUT := out/api28
API28_JAVA := runtime/api28/Api28ProbeActivity.java runtime/app/src/dev/kreijstal/aosppocket/runtime/GuestBinder.java runtime/app/src/dev/kreijstal/aosppocket/runtime/Native.java
API28_NATIVE := $(API28_OUT)/build.json $(API28_OUT)/native/libpocket_binder_api28.so $(API28_OUT)/native/pocket-api28-probe $(API28_OUT)/native/pocket-property-service-api28 $(API28_OUT)/native/pocket-property-probe-api28 $(API28_OUT)/native/pocket-init-api28 $(API28_OUT)/native/pocket-init-test-api28 $(API28_OUT)/native/libpocket_zygote_api28.so $(API28_OUT)/native/pocket-app-process-api28 $(API28_OUT)/native/pocket-zygote-client-api28 $(API28_OUT)/native/pocket-log-writer-api28
.PHONY: api28-native api28-verify

api28-native: $(API28_NATIVE)
$(API28_NATIVE) &: tools/prepare_api28.py tools/build_api28.py sources.lock.json runtime/api28/init-rootless.patch runtime/api28/zygote-rootless.patch $(wildcard runtime/api28/*.cpp runtime/api28/*.h runtime/api28/*.rc)
	python3 tools/build_api28.py --ndk $(NDK)

$(API28_OUT)/classes.dex: $(API28_JAVA) $(ANDROID_JAR) $(R8)
	mkdir -p $(API28_OUT)/classes $(API28_OUT)/dex
	javac --release 8 -classpath $(ANDROID_JAR) -d $(API28_OUT)/classes $(API28_JAVA)
	jar cf $(API28_OUT)/classes.jar -C $(API28_OUT)/classes .
	java -cp $(R8) com.android.tools.r8.D8 --lib $(ANDROID_JAR) --min-api 28 --output $(API28_OUT)/dex $(API28_OUT)/classes.jar
	cp $(API28_OUT)/dex/classes.dex $@

$(API28_OUT)/AndroidManifest.xml: runtime/api28/AndroidManifest.xml runtime/api28/Makefile $(OUT)/tools/PocketManifest.class
	mkdir -p $(API28_OUT)
	java -cp $(TOOLS):$(OUT)/tools PocketManifest $< $@ 28 28 4 0.4-api28-zygote

$(API28_OUT)/base.apk: $(API28_OUT)/classes.dex $(API28_OUT)/AndroidManifest.xml
	java -cp $(TOOLS) com.example.build.ApkBuilder $(API28_OUT)/AndroidManifest.xml $(API28_OUT)/classes.dex $@

$(API28_OUT)/art-probe.dex: runtime/api28/ArtProbe.java $(ANDROID_JAR) $(R8)
	mkdir -p $(API28_OUT)/art-classes $(API28_OUT)/art-dex
	javac --release 8 -d $(API28_OUT)/art-classes $<
	jar cf $(API28_OUT)/art-classes.jar -C $(API28_OUT)/art-classes .
	java -cp $(R8) com.android.tools.r8.D8 --lib $(ANDROID_JAR) --min-api 28 --output $(API28_OUT)/art-dex $(API28_OUT)/art-classes.jar
	cp $(API28_OUT)/art-dex/classes.dex $@

$(API28_OUT)/pocket-zygote.jar: runtime/api28/ZygoteBootstrap.java runtime/api28/ZygoteChild.java $(ANDROID_JAR) $(R8)
	mkdir -p $(API28_OUT)/zygote-classes $(API28_OUT)/zygote-dex
	javac --release 8 -d $(API28_OUT)/zygote-classes runtime/api28/ZygoteBootstrap.java runtime/api28/ZygoteChild.java
	jar cf $(API28_OUT)/zygote-classes.jar -C $(API28_OUT)/zygote-classes .
	java -cp $(R8) com.android.tools.r8.D8 --lib $(ANDROID_JAR) --min-api 28 --output $(API28_OUT)/zygote-dex $(API28_OUT)/zygote-classes.jar
	jar cf $@ -C $(API28_OUT)/zygote-dex classes.dex

$(API28_OUT)/packed.apk: $(API28_OUT)/base.apk $(API28_OUT)/build.json $(API28_OUT)/art-probe.dex $(API28_OUT)/pocket-zygote.jar out/runtime-native/libpocket_runtime.so out/runtime-native/libproot.so tools/pack_api28_probe.py runtime/NOTICE $(wildcard runtime/licenses/*) LICENSE vendor/proot/COPYING
	python3 tools/pack_api28_probe.py

$(API28_OUT)/aosp-pocket-api28-probe.apk: $(API28_OUT)/packed.apk out/debug.keystore
	java -cp $(SIGNER) com.android.apksigner.ApkSignerTool sign --ks out/debug.keystore --ks-pass pass:android --key-pass pass:android --out $@ $<

api28-verify: $(API28_OUT)/aosp-pocket-api28-probe.apk
	java -cp $(SIGNER) com.android.apksigner.ApkSignerTool verify --verbose $<
	python3 tools/validate_api28_probe.py $< --ndk $(NDK)
