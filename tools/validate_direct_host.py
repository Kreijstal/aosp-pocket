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
    # Real dynamically linked programs whose guest-only RUNPATH cannot be
    # resolved by the host loader. Include $ORIGIN and caller search paths.
    libraries = root / "data/exec-libs"
    libraries.mkdir()
    library_source = root / "library.c"
    library_source.write_text("int pocket_exec_value(void) { return 42; }\n")
    program_source = root / "program.c"
    program_source.write_text("extern int pocket_exec_value(void); int main(void) { return pocket_exec_value() != 42; }\n")
    subprocess.run(["cc", "-fPIC", "-shared", library_source, "-Wl,-soname,libpocket_exec_test.so",
                    "-o", libraries / "libpocket_exec_test.so"], check=True)
    for name, search in (("absolute", "/data/exec-libs"), ("origin", "$ORIGIN/../../data/exec-libs"), ("caller", None)):
        program = root / "system/bin" / name
        command = ["cc", program_source, "-L"+str(libraries), "-lpocket_exec_test", "-o", program]
        if search is not None:
            command.append("-Wl,-rpath,"+search)
        subprocess.run(command, check=True)
        fixture_env = dict(env, POCKET_DIRECT_LIBRARIES=str(root / "system/lib64"))
        if search is None:
            fixture_env["LD_LIBRARY_PATH"] = "/data/exec-libs"
        subprocess.run([binary, "exec-file", "/system/bin/"+name], env=fixture_env, check=True, timeout=10)
    print("PASS: native ELF guest RUNPATH, $ORIGIN and caller LD_LIBRARY_PATH")
    print("PASS: Linux direct paths/stdio/directories/metadata/rename/hardlinks/Unix sockets/credentials/FD transfer, negative cases, threads, fork/exec, zero tracer")
