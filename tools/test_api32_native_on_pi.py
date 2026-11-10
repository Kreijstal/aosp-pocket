#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run the API 32 native fixture as an ordinary ARM64 Linux user.

This foundation gate tests the actual Bionic/ART payloads through Pocket's
loader. It does not claim Android app-sandbox, zygote, framework or Manager boot.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import shutil
import socket
import subprocess
import threading


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture', type=Path, default=Path.home() / 'pocket-api32-test')
    parser.add_argument('--timeout', type=int, default=120)
    args = parser.parse_args()
    if platform.machine() != 'aarch64' or os.getuid() == 0:
        parser.error('Run as an ordinary user on the ARM64 Pi')
    if not 10 <= args.timeout <= 600:
        parser.error('Timeout must be 10..600 seconds')
    root = args.fixture.resolve() / 'root'
    manifest = json.loads((args.fixture / 'build.json').read_text())
    assert manifest['sdk'] == 32 and manifest['runtimeRoot'] == str(root)
    assert not manifest.get('binderRequested'), 'Host Binder transport requires the separate Android sandbox fixture'
    assert (root / '.pocket-api32-fixture').is_file()
    for name, expected in manifest['files'].items():
        assert not name.startswith('/') and '..' not in name.split('/')
        path = root / name
        assert not path.is_symlink() and hashlib.file_digest(path.open('rb'), 'sha256').hexdigest() == expected, name
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if entry.stat().st_uid == os.getuid() and str((entry / 'exe').readlink()).startswith(str(root) + '/'):
                raise RuntimeError('An existing fixture process is running: ' + entry.name)
        except (PermissionError, FileNotFoundError, ProcessLookupError):
            pass
    for path in (root / 'linkerconfig').iterdir():
        if path.is_dir() and not path.is_symlink():
            shutil.rmtree(path)
        else:
            path.unlink()
    shutil.copy2(root / 'bootstrap-linkerconfig', root / 'linkerconfig/ld.config.txt')
    for path in (root / 'dev/__properties__').iterdir():
        assert path.is_file() and not path.is_symlink()
        path.unlink()
    env = {'POCKET_LOADER_BOOT': '1', 'PATH': '/system/bin', 'ANDROID_ROOT': '/system', 'ANDROID_DATA': '/data',
           'LD_LIBRARY_PATH': '/apex/com.android.runtime/lib64/bionic:/system/lib64', 'HOME': '/data/local/tmp', 'TMPDIR': '/tmp'}
    # Collect the real liblog logdw datagrams. JNI loading failures otherwise
    # disappear into an absent Android log daemon on this Linux-only fixture.
    address = root / 'dev/socket/logdw'
    if address.exists():
        assert address.is_socket()
        address.unlink()
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    listener.bind(str(address))
    listener.settimeout(0.2)
    finished, log = threading.Event(), []
    def collect():
        while not finished.is_set():
            try:
                data = listener.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                return
            # AOSP's packed android_log_header_t has an id, tid and log_time.
            if len(data) >= 14 and data[0] not in (2, 5, 6):
                message = data[12:].rstrip(b'\0').replace(b'\0', b': ').decode('utf-8', errors='replace')
                if sum(map(len, log)) < 4 * 1024 * 1024:
                    log.append(message)
    collector = threading.Thread(target=collect, daemon=True)
    collector.start()
    process = subprocess.Popen([str(root / 'system/bin/sh'), '/probe.sh'], cwd=root, env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
    error = None
    try:
        stdout, stderr = process.communicate(timeout=args.timeout)
    except subprocess.TimeoutExpired:
        error = 'Native foundation fixture timed out'
        os.killpg(process.pid, signal.SIGTERM)
        try:
            stdout, stderr = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            stdout, stderr = process.communicate(timeout=5)
    finally:
        finished.set()
        collector.join(1)
        listener.close()
        address.unlink(missing_ok=True)
    art = manifest.get('artRequested', False)
    passed = process.returncode == 0 and 'API32_NATIVE_RUNTIME_PASS' in stdout
    if art:
        passed = passed and 'REAL_ANDROID12L_ART_PASS' in stdout and 'API32_LINKER_CONFIG_GENERATED' in stdout
    report = {'hostUid': os.getuid(), 'hostPlatform': platform.system(), 'guestSdk': 32, 'exitCode': process.returncode,
              'stdout': stdout, 'stderr': stderr, 'passed': passed, 'artRequested': art, 'fullGuestBootVerified': False,
              'androidAppSandboxVerified': False, 'scope': 'ARM64 Linux-user native foundation only', 'nativeLog': '\n'.join(log)}
    if error:
        report['error'] = error
    destination = args.fixture / ('art-validation.json' if art else 'native-validation.json')
    destination.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2), flush=True)
    raise SystemExit(0 if passed else 1)


if __name__ == '__main__':
    main()
