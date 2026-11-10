#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run the immutable-platform regression as an ordinary ARM64 Linux user.

The old/fixed loaders and static Bionic probe must be configured/staged in this
dedicated fixture first. This does not use Android, mounts, root or VM state.
"""
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess


def main():
    assert platform.machine() == 'aarch64' and os.getuid() != 0
    base = Path.home() / 'pocket-api32-readonly-test'
    assert base.is_dir() and not base.is_symlink()
    assert (base / '.pocket-readonly-test-fixture').is_file()
    root = base / 'root'
    for name in ['system/bin', 'system/etc', 'tmp', '.pocket-owner']:
        (root / name).mkdir(parents=True, exist_ok=True)
    marker = root / '.pocket-readonly-system'
    marker.write_text('immutable Android platform partitions\n')
    fixture = root / 'system/etc/pocket-readonly-fixture.txt'
    fixture.write_text('readonly-fixture\n')
    fixture.chmod(0o644)
    before = hashlib.sha256(fixture.read_bytes()).hexdigest()
    results = {}
    for name, arguments in [('old-loader', []), ('fixed-loader', []), ('fixed-loader-disabled', ['--expect-writable'])]:
        if name.endswith('-disabled'):
            marker.unlink()
        loader = root / 'system/bin' / ('fixed-loader' if name.endswith('-disabled') else name)
        probe = root / 'system/bin/probe'
        result = subprocess.run([str(loader), *arguments], cwd=root,
                                env={'POCKET_LOADER_BOOT': '1', 'POCKET_LOADER_EXEC_V1': str(probe)},
                                capture_output=True, text=True, timeout=30)
        results[name] = {'returncode': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr}
    results['backingFileUnchanged'] = hashlib.sha256(fixture.read_bytes()).hexdigest() == before
    results['backingFileWritableOutsideLoader'] = os.access(fixture, os.W_OK) and fixture.stat().st_mode & 0o777 == 0o644
    passed = (results['old-loader']['returncode'] == 1 and
              results['fixed-loader']['returncode'] == 0 and 'LOADER_READONLY_VIEW_PASS' in results['fixed-loader']['stdout'] and
              results['fixed-loader-disabled']['returncode'] == 0 and
              results['backingFileUnchanged'] and results['backingFileWritableOutsideLoader'])
    results['passed'] = passed
    (base / 'native-validation.json').write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps(results, indent=2), flush=True)
    raise SystemExit(0 if passed else 1)


if __name__ == '__main__':
    main()
