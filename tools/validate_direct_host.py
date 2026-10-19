#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run the real file/thread/fork/exec ABI on Linux with its native libc."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="pocket-direct-") as tmp:
    root = Path(tmp)
    (root / "system/bin").mkdir(parents=True)
    (root / "data").mkdir()
    (root / "data/fixture").write_text("pocket-direct-fixture\n")
    binary = root / "system/bin/pocket-direct-probe"
    shutil.copy2(ROOT / "out/direct/host-probe", binary)
    env = dict(os.environ, POCKET_DIRECT_ROOT=str(root))
    for name in ("POCKET_DIRECT_LINKER", "POCKET_DIRECT_LIBRARIES", "POCKET_DIRECT_HOLD"):
        env.pop(name, None)
    result = subprocess.run([binary, "suite", "direct"], env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
    print(result.stdout, end="")
    assert result.returncode == 0 and "DIRECT_SUITE_PASS" in result.stdout
    assert "DIRECT_FILES_PASS" in result.stdout and "DIRECT_SOCKET_PASS" in result.stdout
    subprocess.run([binary, "bench", "/data/fixture", "100"], env=env, check=True, timeout=10)
    print("PASS: Linux direct paths/stdio/directories/metadata/rename/hardlinks/Unix sockets/credentials/FD transfer, negative cases, threads, fork/exec, zero tracer")
