#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate complete CE metadata replacement and both persisted namespace layouts."""
from pathlib import Path
import subprocess
from target import ROOT


def main():
    out = ROOT / 'out/ce-snapshot-fix/classes'
    out.mkdir(parents=True, exist_ok=True)
    names = ['CeSnapshotNoReadTest', 'CeNamespaceTest', 'CeNamespace32Test']
    source = ROOT / 'runtime/storage/src/dev/kreijstal/aosppocket/storage'
    subprocess.run(['javac', '--release', '8', '-Xlint:all', '-d', str(out),
        str(source / 'EncryptedStore.java'), str(source / 'CeNamespace.java')]
        + [str(ROOT / 'runtime/tests' / (name + '.java')) for name in names], check=True)
    for name in names:
        subprocess.run(['java', '-cp', str(out), 'dev.kreijstal.aosppocket.storage.' + name],
            check=True, timeout=60)


if __name__ == '__main__':
    main()
