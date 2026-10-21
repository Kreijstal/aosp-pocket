# SPDX-License-Identifier: GPL-3.0-only
TEESIM_BASE_APK ?= out/manager-0.19/aosp-pocket-manager-0.19.apk
TEESIM_OUT ?= out/teesim-api28
.PHONY: teesim-api28 check-teesim-api28
teesim-api28:
	python3 tools/build_teesim_api28.py --base-apk $(TEESIM_BASE_APK) --out-dir $(TEESIM_OUT) --ndk $(NDK)

# Native wire/crypto validation under qemu-user; no Binder RPC claim.
check-teesim-api28: teesim-api28
	python3 tools/make_attestation_fixture.py --out $(TEESIM_OUT)/test-identity
	qemu-aarch64-static -E LD_LIBRARY_PATH=$(abspath $(TEESIM_OUT)/lib64) $(TEESIM_OUT)/build/pocket_teesim_wire_test $(TEESIM_OUT)/test-identity/test-keybox.xml
