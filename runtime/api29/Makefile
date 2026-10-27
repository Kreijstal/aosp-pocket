# SPDX-License-Identifier: GPL-3.0-only
# Isolated bring-up only; this does not enable API 29 in Pocket Manager.
.PHONY: api29-probe api29-check

api29-probe:
	@test "$(ARCH)" = arm64 || (echo "API 29 bring-up currently requires ARCH=arm64"; exit 1)
	python3 tools/build_api29_probe.py --ndk $(NDK) --out out/api29/art-probe --art --properties --binder --init --zygote --hardware

api29-check:
	python3 -m unittest discover -s tools/tests -p test_prepare_api29.py -v
	mkdir -p out/api29
	clang++ -Wall -Wextra -Werror -fsanitize=address,undefined runtime/tests/binder_protocol.cpp -o out/api29/binder-protocol-test
	out/api29/binder-protocol-test
