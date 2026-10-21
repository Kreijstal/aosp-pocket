#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Create an opt-in Android 9 SOFTWARE profile from an owner-supplied keybox."""
import argparse
import base64
import json
import os
from pathlib import Path
import xml.etree.ElementTree as ET


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--keybox', type=Path, required=True)
    parser.add_argument('--package', required=True)
    parser.add_argument('--uid', type=int, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if args.uid < 10000:
        parser.error('Select an ordinary guest app UID from /data/system/packages.list')
    with args.keybox.open('rb') as stream:
        keybox = stream.read(2*1024*1024+1)
    if len(keybox) > 2*1024*1024:
        parser.error('Keybox exceeds the 2 MiB input limit')
    root = ET.fromstring(keybox)
    keys = root.findall('.//Key')
    if not any(k.get('algorithm') in ('rsa', 'ecdsa') and
               (k.findtext('PrivateKey') or '').strip() and
               len(k.findall('.//Certificate')) >= 2 for k in keys):
        parser.error('Expected an RSA or EC signing key with at least two certificates')
    config = {'type': 'config', 'epoch': 1,
              'bootInfo': {'deviceLocked': False, 'verifiedBootState': 2,
                           'strongBoxAvailable': False, 'attestVersionTee': 3,
                           'attestVersionStrongBox': 3},
              'profiles': [{'id': 'pocket-owner', 'mode': 'generation',
                            'securityLevel': 0, 'osVersion': 90000,
                            'osPatchLevel': 0, 'vendorPatchLevel': 0, 'bootPatchLevel': 0,
                            'packages': [args.package], 'uids': [args.uid],
                            'uidPackages': [args.package],
                            'keyboxB64': base64.b64encode(keybox).decode()}]}
    # Refuse to replace an existing private identity by accident.
    fd = os.open(args.out, os.O_WRONLY|os.O_CREAT|os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w') as stream:
        json.dump(config, stream)
        stream.write('\n')
    print('Created private SOFTWARE configuration:', args.out)


if __name__ == '__main__':
    main()
