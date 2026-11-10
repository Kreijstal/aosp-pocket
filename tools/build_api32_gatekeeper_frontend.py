#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compile the pinned classic Gatekeeper frontend for the UID-1000 crypto host."""
import hashlib
import json
from pathlib import Path
import subprocess
from prepare_api32 import ROOT, CACHE, prepare_aidl, prepare_sources


def build_object(common, out):
    prepare_sources(['system-core', 'system-security', 'hardware-interfaces'])
    out = Path(out) / 'frontend'
    generated = out / 'generated'
    generated.mkdir(parents=True, exist_ok=True)
    keymint = CACHE / 'hardware-interfaces/security/keymint/aidl/aidl_api/android.hardware.security.keymint/1'
    clock = CACHE / 'hardware-interfaces/security/secureclock/aidl/aidl_api/android.hardware.security.secureclock/1'
    authorization = CACHE / 'system-security/keystore2/aidl'
    aidls = [keymint / ('android/hardware/security/keymint/' + name + '.aidl')
             for name in ['HardwareAuthToken', 'HardwareAuthenticatorType']]
    aidls += [clock / ('android/hardware/security/secureclock/' + name + '.aidl')
              for name in ['Timestamp', 'TimeStampToken']]
    aidls += [authorization / ('android/security/authorization/' + name + '.aidl')
              for name in ['LockScreenEvent', 'AuthorizationTokens', 'IKeystoreAuthorization']]
    aidl = prepare_aidl()
    commands = []

    def run(command, **kwargs):
        command = list(map(str, command))
        commands.append(command)
        subprocess.run(command, check=True, **kwargs)

    for source in aidls:
        command = [aidl, '--lang=ndk', '--structured', '--out=' + str(generated / 'src'),
                   '--header_out=' + str(generated),
                   *['-I' + str(root) for root in [keymint, clock, authorization]]]
        if source.is_relative_to(keymint) or source.is_relative_to(clock):
            command.append('--stability=vintf')
        run([*command, source])

    upstream = CACHE / 'system-core/gatekeeperd/gatekeeperd.cpp'
    original = upstream.read_bytes()
    # Review the narrow patch again if the pinned platform source ever changes.
    if hashlib.sha256(original).hexdigest() != 'c7abe390f0616a26252b8d5a609db14ec75f71e64fda7d95c493623551208770':
        raise ValueError('Gatekeeper frontend source differs from the reviewed 12L revision')
    source = out / 'gatekeeperd.cpp'
    source.write_bytes(original)
    patch = ROOT / 'runtime/api32/gatekeeper-frontend.patch'
    run(['patch', '--batch', '--fuzz=0', '-p1', '-i', patch], cwd=out)
    includes = [generated, CACHE / 'system-core/gatekeeperd/include']
    includes += [CACHE / 'frameworks-native/libs/binder' / path
                 for path in ['include', 'ndk/include_ndk', 'ndk/include_cpp', 'ndk/include_platform']]
    target = out / 'gatekeeper-frontend.o'
    # LOG() embeds the patched source path. Keep bytes reproducible when the
    # same pinned source is built in a different image/output directory.
    path_map = '-ffile-prefix-map=' + str(out) + '=pocket-gatekeeper-frontend'
    run([*common, *['-I' + str(path) for path in includes], path_map, '-fPIE', '-c', source, '-o', target])
    receipt = {'upstreamSha256': hashlib.sha256(original).hexdigest(),
               'patchSha256': hashlib.sha256(patch.read_bytes()).hexdigest(),
               'patchedSourceSha256': hashlib.sha256(source.read_bytes()).hexdigest(),
               'aidlSources': {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                               for path in aidls},
               'objectSha256': hashlib.sha256(target.read_bytes()).hexdigest(),
               'stateDirectory': '/data/misc/gatekeeper', 'processCwdChanged': False,
               'sourcePathMapping': 'pocket-gatekeeper-frontend',
               'permissionChecksChanged': False, 'credentialAlgorithmsChanged': False,
               'runtimeVerified': False, 'commands': commands}
    (out / 'build.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return target, receipt
