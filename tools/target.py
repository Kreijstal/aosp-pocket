# SPDX-License-Identifier: GPL-3.0-only
"""Per-architecture build facts shared by the native builders.

Select with POCKET_ARCH=arm64|x86_64 (default arm64). The arm64 values are the
ones every existing artifact was built with; x86_64 mirrors them for the x86_64
direct backend.
"""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARCHITECTURES = {
    "arm64": {
        "triple": "aarch64-linux-android",
        "abi": "arm64-v8a",
        "framework": "arm64",
        "elf_machine": "AArch64",
        "reference": "reference",
    },
    "x86_64": {
        "triple": "x86_64-linux-android",
        "abi": "x86_64",
        "framework": "x86_64",
        "elf_machine": "Advanced Micro Devices X86-64",
        "reference": "reference-x86_64",
    },
}


def selected():
    name = os.environ.get("POCKET_ARCH", "arm64")
    if name not in ARCHITECTURES:
        raise SystemExit(f"Unknown POCKET_ARCH={name!r}; expected one of {sorted(ARCHITECTURES)}")
    return name


def info(name=None):
    return ARCHITECTURES[name or selected()]


def clang(llvm, api, cxx=False, name=None):
    """NDK wrapper for the selected architecture, e.g. aarch64-linux-android28-clang++."""
    return Path(llvm) / f"{info(name)['triple']}{api}-clang{'++' if cxx else ''}"


def reference_dir(name=None):
    return ROOT / "out/api28" / info(name)["reference"]
