#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run in Termux after pairing pocketctl; install an offline guest-only API probe."""
import importlib.util
import argparse
import io
import json
from pathlib import Path
import time
import uuid

DOWNLOADS = Path('/storage/emulated/0/Download')
spec = importlib.util.spec_from_file_location('pocketctl', DOWNLOADS/'pocketctl.py')
client = importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)
config = json.loads((Path.home()/'.config/pocketctl/config.json').read_text())


def request(op, argv=(), source=None, check=True):
    out, err = io.BytesIO(), io.BytesIO()
    code = client.request(config, op, argv, source=source, stdout=out, stderr=err)
    if check and code:
        raise RuntimeError('Guest request failed: '+err.getvalue().decode(errors='replace')+out.getvalue().decode(errors='replace'))
    return code, out.getvalue(), err.getvalue()


def command(*argv, **kwargs):
    return request(2, argv, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--launch-only', action='store_true', help='Use the installed probe without a package update')
    args = parser.parse_args()
    state = json.loads(request(1)[1])
    if not state.get('ready'):
        request(3)
        for _ in range(180):
            state = json.loads(request(1)[1])
            if state.get('ready'):
                break
            time.sleep(.5)
        else:
            raise RuntimeError('Guest did not reach ready state: '+str(state))
    assert state['sdk'] == 28, state
    print('Guest ready. Uploading offline test artifacts.', flush=True)
    files = [('pocket-attestation-launch', '/data/local/tmp/pocket-attestation-launch')]
    if not args.launch_only:
        files.append(('pocket-key-attestation-0.2.apk', '/data/local/tmp/pocket-key-attestation-test.apk'))
    for source, destination in files:
        with (DOWNLOADS/source).open('rb') as stream:
            command('/system/bin/sh', '-c', 'umask 077; /system/bin/toybox cat > "$1"', 'pocket-test', destination, source=stream)
    command('/system/bin/toybox', 'chmod', '700', files[0][1])
    command('/system/bin/toybox', 'rm', '-f', '/data/data/dev.kreijstal.pocketattestationtest/files/report.json')
    # Refuse an already active APK import; never delete somebody else's staging.
    if args.launch_only:
        command(files[0][1], 'dev.kreijstal.pocketattestationtest')
    else:
        command('/system/bin/toybox', 'mkdir', '/data/pocket-import')
        marker = '/data/pocket-import/.attestation-'+uuid.uuid4().hex
        try:
            command('/system/bin/toybox', 'touch', marker)
            command('/system/bin/toybox', 'cp', files[1][1], '/data/pocket-import/base.apk')
            command('/system/bin/toybox', 'chmod', '644', '/data/pocket-import/base.apk')
            code, out, err = command(files[0][1], '--install')
            print(out.decode(errors='replace'), err.decode(errors='replace'), flush=True)
            assert b'Installed dev.kreijstal.pocketattestationtest' in out
        finally:
            command('/system/bin/sh', '-c',
                    'if [[ -f "$1" ]]; then /system/bin/toybox rm -rf /data/pocket-import; fi',
                    'pocket-cleanup', marker, check=False)
    path = '/data/data/dev.kreijstal.pocketattestationtest/files/report.json'
    for _ in range(90):
        code, out, err = command('/system/bin/toybox', 'cat', path, check=False)
        if code == 0:
            report = json.loads(out)
            break
        time.sleep(1)
    else:
        raise RuntimeError('Probe did not write a report: '+err.decode(errors='replace'))
    destination = Path.home()/'.config/pocketctl/key-attestation-report.json'
    destination.write_text(json.dumps(report, indent=2)+'\n')
    assert report['uid'] >= 10000, 'Probe did not execute as an ordinary app'
    print('Ordinary guest app UID:', report['uid'], 'SDK:', report['sdk'], flush=True)
    for name in ['plain', 'attested', 'strongBox']:
        result = report.get(name, {})
        summary = {key: value for key, value in result.items() if key != 'certificates'}
        summary['certificateCount'] = len(result.get('certificates', []))
        print(name, json.dumps(summary), flush=True)
    diagnostic = report.get('explicitStockProviderDiagnostic')
    if diagnostic:
        print('Explicit stock provider initialization:', diagnostic.get('providerInstalled'), diagnostic.get('initializationError'), flush=True)
        for name in ['plain', 'attested', 'strongBox']:
            result = diagnostic.get(name, {})
            summary = {key: value for key, value in result.items() if key != 'certificates'}
            summary['certificateCount'] = len(result.get('certificates', []))
            print('stock-provider-'+name, json.dumps(summary), flush=True)
    print('Full report saved:', destination, flush=True)


if __name__ == '__main__':
    main()
