# SPDX-License-Identifier: GPL-3.0-only
MAGISK_BASE_APK ?= out/manager-0.22/aosp-pocket-manager-0.22.apk
.PHONY: magisk-pocket magisk-pocket-manager
magisk-pocket:
	python3 tools/build_magisk_pocket.py --base-apk $(MAGISK_BASE_APK) --ndk $(NDK)
magisk-pocket-manager:
	python3 tools/build_magisk_pocket.py --base-apk $(MAGISK_BASE_APK) --ndk $(NDK) --support-only --package-version 0.27
