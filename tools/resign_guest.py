#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Release-sign the pinned guest's test-signed APKs without rebuilding its runtime.

Private keys live outside the checkout. System-image upgrades use Android's
existing system-package certificate migration, not trust in public old keys.
Third-party, independently signed APKs retain their original certificates.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile
from target import ROOT

PREFIX = 'assets/api28-rootfs/'
ROLES = ('releasekey', 'platform', 'shared', 'media', 'pocket')
POCKET_DEVELOPMENT = 'b04ea6cd081f4b5afb30b25f5492ccd044c821fd1e17d18c72067f1b9fcf6916'
TEST_CERTIFICATES = {
    'releasekey':'a40da80a59d170caa950cf15c18c454d47a39b26989d8b640ecd745ba71bf5dc',
    'platform':'c8a2e9bccf597c2fb6dc66bee293fc13f2fc47ec77bc6b2b0d52c11f51192ab8',
    'shared':'28bbfe4a7b97e74681dc55c2fbb6ccb8d6c74963733f6af6ae74d8c3a6e879fd',
    'media':'465983f7791f2abeb43ea2cbdc7f21a8260b72bc08a55c839fc1a43bc741a81e',
}
SIGNER = ['java', '-cp', str(ROOT.parent/'helloworldandroid/build/apksigner.jar'),
          'com.android.apksigner.ApkSignerTool']


def sha(data):
    return hashlib.sha256(data).hexdigest()


def run(*args):
    return subprocess.run(list(map(str, args)), check=True, capture_output=True,
                          timeout=180, umask=0o077).stdout


def certificate(path):
    return run('openssl', 'x509', '-in', path, '-outform', 'DER')


def apk_certificate(path):
    text = run(*SIGNER, 'verify', '--verbose', '--print-certs',
               '--min-sdk-version', '28', '--max-sdk-version', '28', path).decode()
    certs = re.findall(r'Signer #\d+ certificate SHA-256 digest: ([0-9a-f]{64})', text)
    if len(certs) != 1:
        raise ValueError('Expected exactly one APK signer: ' + str(path))
    return certs[0], text


def keys(directory, create):
    directory = directory.expanduser().resolve()
    if directory.is_relative_to(ROOT):
        raise ValueError('Private signing keys must be outside the checkout')
    if not directory.exists():
        if not create:
            raise ValueError('No private key directory; use --create-keys once')
        directory.mkdir(parents=True, mode=0o700)
    if directory.stat().st_mode & 0o077:
        raise ValueError('Private key directory must have mode 0700')
    result = {}
    for role in ROLES:
        key, cert = directory/(role+'.pk8'), directory/(role+'.x509.pem')
        if key.exists() != cert.exists():
            raise ValueError('Incomplete signing identity: '+role)
        if not key.exists():
            if not create:
                raise ValueError('Missing signing identity: '+role)
            with tempfile.TemporaryDirectory(prefix='generate-', dir=directory) as tmp:
                tmp = Path(tmp)
                run('openssl', 'req', '-new', '-newkey', 'rsa:3072', '-x509',
                    '-sha256', '-days', '10000', '-nodes', '-subj',
                    '/CN=Pocket Guest '+role+'/O=Pocket', '-keyout', tmp/'key.pem',
                    '-out', tmp/'cert.pem')
                run('openssl', 'pkcs8', '-topk8', '-nocrypt', '-in', tmp/'key.pem',
                    '-outform', 'DER', '-out', tmp/'key.pk8')
                # Refuse replacing identities created by another build.
                for src, dest in [(tmp/'key.pk8', key), (tmp/'cert.pem', cert)]:
                    with dest.open('xb') as out:
                        os.fchmod(out.fileno(), 0o600)
                        out.write(src.read_bytes()); out.flush(); os.fsync(out.fileno())
        if key.stat().st_mode & 0o077:
            raise ValueError('Private key must have mode 0600: '+role)
        der = certificate(cert)
        public = run('openssl', 'x509', '-in', cert, '-pubkey', '-noout')
        private_public = run('openssl', 'pkey', '-inform', 'DER', '-in', key, '-pubout')
        if public != private_public:
            raise ValueError('Certificate/private key mismatch: '+role)
        result[role] = {'key':key, 'certificate':cert, 'der':der, 'sha256':sha(der)}
    if len({v['sha256'] for v in result.values()}) != len(ROLES):
        raise ValueError('Signing groups must retain distinct identities')
    return result


def content(apk):
    with zipfile.ZipFile(apk) as z:
        # JAR signing records may change; every actual APK payload must match.
        return {n:sha(z.read(n)) for n in z.namelist() if not
                (n.upper() == 'META-INF/MANIFEST.MF' or re.fullmatch(
                    r'META-INF/[^/]+\.(SF|RSA|DSA|EC)', n, re.IGNORECASE))}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base-apk', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--version', required=True)
    p.add_argument('--key-dir', type=Path, required=True)
    p.add_argument('--create-keys', action='store_true')
    p.add_argument('--test-certificates', type=Path, required=True)
    args = p.parse_args()
    if not re.fullmatch(r'0\.\d+', args.version):
        p.error('Expected a 0.N Manager version')
    out = args.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    identities = keys(args.key_dir, args.create_keys)
    mapping = {POCKET_DEVELOPMENT:'pocket'}
    old_certificates = {}
    for original, role in [('testkey','releasekey'), ('platform','platform'),
                           ('shared','shared'), ('media','media')]:
        der = certificate(args.test_certificates/(original+'.x509.pem'))
        if sha(der) != TEST_CERTIFICATES[role]:
            raise ValueError('Unexpected AOSP test certificate: '+original)
        mapping[sha(der)] = role; old_certificates[role] = der
    if len(mapping) != 5:
        raise ValueError('Unexpected original certificate identities')
    replacements, apks = {}, []
    with zipfile.ZipFile(args.base_apk) as base:
        host_dex = out/'host.dex'; host_dex.write_bytes(base.read('classes.dex'))
        if base.read('assets/pocketctl.py') != (ROOT/'tools/pocketctl.py').read_bytes():
            raise ValueError('Console helper differs; preserve it before building')
        for name in base.namelist():
            if not name.startswith(PREFIX) or not name.endswith('.apk.payload'):
                continue
            relative = name.removeprefix(PREFIX).removesuffix('.payload')
            original = out/'original'/relative; original.parent.mkdir(parents=True, exist_ok=True)
            original.write_bytes(base.read(name))
            signer, _ = apk_certificate(original)
            role = mapping.get(signer) or next((k for k,v in identities.items()
                                                if signer == v['sha256']), None)
            row = {'path':relative, 'originalSignerSha256':signer,
                   'originalApkSha256':sha(original.read_bytes()), 'keyRole':role}
            if role is None:
                row['action'] = 'preserve independently signed APK'
            else:
                target = out/'guest'/relative; target.parent.mkdir(parents=True, exist_ok=True)
                identity = identities[role]
                run(*SIGNER, 'sign', '--key', identity['key'], '--cert', identity['certificate'],
                    '--min-sdk-version', '28', '--v1-signing-enabled', 'true',
                    '--v2-signing-enabled', 'true', '--v3-signing-enabled', 'true',
                    '--v4-signing-enabled', 'false', '--out', target, original)
                new_signer, verification = apk_certificate(target)
                if new_signer != identity['sha256'] or new_signer in mapping:
                    raise ValueError('Release signing verification failed: '+relative)
                if 'Verified using v3 scheme (APK Signature Scheme v3): true' not in verification:
                    raise ValueError('No Android 9 v3 signature: '+relative)
                if content(original) != content(target):
                    raise ValueError('Re-signing changed APK payloads: '+relative)
                replacements[relative] = target
                row.update(action='re-signed', signerSha256=new_signer,
                           apkSha256=sha(target.read_bytes()), payloadUnchanged=True)
            apks.append(row)
        for name in base.namelist():
            if not name.startswith(PREFIX) or not name.endswith('.payload'):
                continue
            relative = name.removeprefix(PREFIX).removesuffix('.payload')
            raw = base.read(name); changed = raw
            if relative.endswith('.prop') or relative.endswith('/prop.default'):
                changed = raw.replace(b'test-keys', b'release-keys')
            elif relative.endswith('mac_permissions.xml'):
                for role, old in old_certificates.items():
                    changed = changed.replace(old.hex().encode(), identities[role]['der'].hex().encode())
            elif relative == 'system/etc/security/otacerts.zip':
                buffer = io.BytesIO()
                with zipfile.ZipFile(buffer, 'w', zipfile.ZIP_DEFLATED) as z:
                    z.writestr('pocket-releasekey.x509.pem', identities['releasekey']['certificate'].read_bytes())
                changed = buffer.getvalue()
            if changed != raw:
                target = out/'guest'/relative; target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(changed); replacements[relative] = target
        props = (replacements['system/build.prop'].read_text() if 'system/build.prop' in replacements
                 else base.read(PREFIX+'system/build.prop.payload').decode())
        if '\nro.build.tags=release-keys\n' not in props:
            raise ValueError('Missing release signing tag')
        # Re-signing does not claim a different framework build configuration.
        old_props = base.read(PREFIX+'system/build.prop.payload').decode()
        for key in ['ro.build.type', 'ro.debuggable', 'ro.build.version.security_patch']:
            pattern = '^'+re.escape(key)+'=(.*)$'
            if re.search(pattern, props, re.M)[1] != re.search(pattern, old_props, re.M)[1]:
                raise ValueError('Unexpected framework configuration change: '+key)
    metadata = {'format':1, 'mode':'private-key guest re-signing',
                'frameworkRecompiled':False, 'historicalTestKeysTrusted':False,
                'certificates':{k:v['sha256'] for k,v in identities.items()},
                'apks':{row['path']:row['keyRole'] for row in apks if row['keyRole']}}
    manifest = out/'guest/system/etc/pocket-release-signing.json'
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text(json.dumps(metadata, indent=2)+'\n')
    replacements['system/etc/pocket-release-signing.json'] = manifest
    command = ['python3', str(ROOT/'tools/build_manager_update.py'),
               '--base-apk', str(args.base_apk.resolve()), '--out', str(out),
               '--version-code', args.version.split('.')[-1], '--version-name', args.version,
               '--host-dex', str(host_dex)]
    for relative, path in replacements.items():
        command += ['--guest-file', relative+'='+str(path)]
    subprocess.run(command, check=True)
    target = out/('aosp-pocket-manager-'+args.version+'.apk')
    with zipfile.ZipFile(args.base_apk) as a, zipfile.ZipFile(target) as b:
        changed = sorted(n for n in set(a.namelist())|set(b.namelist()) if
                         not n.startswith('META-INF/') and
                         (n not in a.namelist() or n not in b.namelist() or a.read(n)!=b.read(n)))
        allowed = {'AndroidManifest.xml'} | {PREFIX+n+'.payload' for n in replacements}
        if not set(changed) <= allowed:
            raise ValueError('Unexpected Manager/payload changes: '+str(set(changed)-allowed))
    report = {'validation':'PASS', 'version':args.version,
              'baseApkSha256':sha(args.base_apk.read_bytes()), 'apkSha256':sha(target.read_bytes()),
              'hostDexSha256':sha(host_dex.read_bytes()), 'changedEntries':changed,
              'guestApks':apks, **metadata}
    (out/'signing-audit.json').write_text(json.dumps(report, indent=2)+'\n')
    print('PASS: guest release-signing and payload audit:', target)
    print('Re-signed APKs:', sum(r['action']=='re-signed' for r in apks))
    print('Private key directory:', args.key_dir.expanduser().resolve())


if __name__ == '__main__':
    main()
