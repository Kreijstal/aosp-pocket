#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Recover one acquired loader's filter using bounded, offline ARM64 emulation.

This is a version-specific analysis recipe, not runtime code. No target syscall
is executed: the two prctl calls are intercepted inside Unicorn. Address ranges
are synthetic fixtures; the result is not a physical-device observation.
"""
import argparse
import json
from pathlib import Path
import struct


def evaluate(program, number, ip, arch=0xC00000B7, arguments=None):
    data = struct.pack("<iIQ6Q", number, arch, ip, *(arguments or [0] * 6))
    accumulator, pc = 0, 0
    for _ in range(256):
        code, jt, jf, value = program[pc]
        if code == 0x20:  # BPF_LD | BPF_W | BPF_ABS
            accumulator = struct.unpack_from("<I", data, value)[0]
        elif code in (0x15, 0x25, 0x35):
            condition = (accumulator == value if code == 0x15 else
                         accumulator > value if code == 0x25 else accumulator >= value)
            pc += jt if condition else jf
        elif code == 6:  # BPF_RET | BPF_K
            return value
        else:
            raise ValueError(f"Unsupported cBPF instruction: {code:#x}")
        pc += 1
    raise ValueError("Filter did not terminate")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
    from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2
    from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC
    image = args.elf.read_bytes()
    if image[:6] != b"\x7fELF\x02\x01":
        raise ValueError("Expected ELF64 little-endian")
    phoff = struct.unpack_from("<Q", image, 32)[0]
    phsize, phnum = struct.unpack_from("<HH", image, 54)
    segments = [struct.unpack_from("<IIQQQQQQ", image, phoff + i * phsize)
                for i in range(phnum)]
    segments = [p for p in segments if p[0] == 1]
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    low = min(p[3] & ~4095 for p in segments)
    high = max((p[3] + p[6] + 4095) & ~4095 for p in segments)
    uc.mem_map(low, high-low)
    for p in segments:
        uc.mem_write(p[3], image[p[2]:p[2]+p[5]])
    stack = 0x70000000
    uc.mem_map(stack, 0x100000)
    uc.reg_write(UC_ARM64_REG_SP, stack + 0xFF000)
    # These locations are specific to the prelinker in APK 2.0.0-alpha09.
    fixture = 0x782EEEC000
    uc.mem_map(fixture, 4096)
    # Two synthetic [base, size] pairs, not live VMOS addresses.
    ranges = [0x10000000, 0x10000, 0x20000000, 0x10000]
    uc.mem_write(fixture, struct.pack("<4Q", *ranges))
    sentinel = 0x71000000
    uc.reg_write(UC_ARM64_REG_LR, sentinel)
    calls, program = [], []

    def code_hook(engine, address, size, user):
        if address != 0x8050:  # local prctl wrapper, before its svc instruction
            return
        option = engine.reg_read(UC_ARM64_REG_X0)
        argument = engine.reg_read(UC_ARM64_REG_X1)
        pointer = engine.reg_read(UC_ARM64_REG_X2)
        calls.append({"option": option, "argument": argument})
        if option == 22:  # PR_SET_SECCOMP
            length = struct.unpack("<H", engine.mem_read(pointer, 2))[0]
            filters = struct.unpack("<Q", engine.mem_read(pointer + 8, 8))[0]
            if not 1 <= length <= 128:
                raise ValueError(f"Unexpected filter length: {length}")
            raw = engine.mem_read(filters, length * 8)
            program.extend(struct.unpack_from("<HBBI", raw, i * 8) for i in range(length))
        engine.reg_write(UC_ARM64_REG_X0, 0)
        engine.reg_write(UC_ARM64_REG_PC, engine.reg_read(UC_ARM64_REG_LR))

    uc.hook_add(UC_HOOK_CODE, code_hook)
    uc.emu_start(0x35D0, sentinel, timeout=2_000_000, count=5000)
    assert uc.reg_read(UC_ARM64_REG_PC) == sentinel, "Emulation did not return"
    assert calls == [{"option": 38, "argument": 1}, {"option": 22, "argument": 2}], calls
    assert len(program) == 39, len(program)
    cases = []
    for ip in (0x10000010, 0x20000010, 0x30000010):
        for nr in (29, 56, 134, 167, 222):  # ioctl, openat, rt_sigaction, prctl, mmap
            verdict = evaluate(program, nr, ip)
            cases.append({"ip": hex(ip), "syscall": nr, "result": hex(verdict),
                          "action": {0x7FFF0000: "ALLOW", 0x30000: "TRAP",
                                     0x50000: "ERRNO"}.get(verdict & 0xFFFF0000, "OTHER")})
    checks = [
        ("mmap from second range traps", 222, 0x20000010, [0] * 6, 0x30000),
        ("4 KiB mmap exception", 222, 0x20000010, [0, 4096, 0, 0, 0, 0], 0x7FFF0000),
        ("SIGSYS handler registration traps", 134, 0x30000010, [31, 0, 0, 0, 0, 0], 0x30000),
        ("marked SIGSYS registration allowed", 134, 0x30000010,
         [31, 0, 0, 0, 0, 0x564D4F53], 0x7FFF0000),
        ("first range is allowed", 134, 0x10000010, [31, 0, 0, 0, 0, 0], 0x7FFF0000),
    ]
    targeted = []
    for name, nr, ip, registers, expected in checks:
        actual = evaluate(program, nr, ip, arguments=registers)
        assert actual == expected, (name, hex(actual), hex(expected))
        targeted.append({"name": name, "syscall": nr, "ip": hex(ip),
                         "arguments": registers, "result": hex(actual)})
    result = {"classification": "synthetic_offline_emulation", "entry": "0x35d0",
              "prctl_calls": calls, "filter_length": len(program),
              "synthetic_ranges": [hex(x) for x in ranges],
              "program": [{"code": hex(c), "jt": t, "jf": f, "k": hex(k)}
                          for c, t, f, k in program], "cases": cases, "targeted_checks": targeted}
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(f"PASS: bounded emulation captured {len(program)} cBPF instructions; no real syscalls executed")
    print(json.dumps(targeted, indent=2))


if __name__ == "__main__":
    main()
