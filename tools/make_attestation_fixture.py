#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate a private, locally signed test identity. No Google signing material."""
import argparse
import base64
import datetime
import json
import os
from pathlib import Path
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--uid', type=int, default=10018)
    parser.add_argument('--package', default='dev.kreijstal.pocketattestationtest')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True, mode=0o700)
    now = datetime.datetime.now(datetime.timezone.utc)
    root_key = ec.generate_private_key(ec.SECP256R1())
    batch_key = ec.generate_private_key(ec.SECP256R1())
    root_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'Pocket LOCAL TEST Root')])
    batch_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'Pocket LOCAL TEST Attestation')])

    def cert(subject, issuer, public, signing):
        return (x509.CertificateBuilder().subject_name(subject).issuer_name(issuer)
                .public_key(public).serial_number(x509.random_serial_number())
                .not_valid_before(now-datetime.timedelta(minutes=5))
                .not_valid_after(now+datetime.timedelta(days=30))
                .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
                .sign(signing, hashes.SHA256()))

    root = cert(root_name, root_name, root_key.public_key(), root_key)
    batch = cert(batch_name, root_name, batch_key.public_key(), root_key)
    pem = serialization.Encoding.PEM
    # Upstream's signing-key reader expects SEC1 EC material in the keybox.
    key = batch_key.private_bytes(pem, serialization.PrivateFormat.TraditionalOpenSSL,
                                  serialization.NoEncryption()).decode()
    chain = ''.join('<Certificate format="pem">'+c.public_bytes(pem).decode()+'</Certificate>' for c in (batch, root))
    xml = ('<AndroidAttestation><NumberOfKeyboxes>1</NumberOfKeyboxes><Keybox DeviceID="pocket-local-test">'
           '<Key algorithm="ecdsa"><PrivateKey format="pem">'+key+'</PrivateKey><CertificateChain>'
           '<NumberOfCertificates>2</NumberOfCertificates>'+chain+'</CertificateChain></Key></Keybox></AndroidAttestation>')
    config = {'type': 'config', 'epoch': 1, 'bootInfo': {
        'deviceLocked': False, 'verifiedBootState': 2, 'strongBoxAvailable': False,
        'attestVersionTee': 3, 'attestVersionStrongBox': 3}, 'profiles': [{
            'id': 'pocket-local-test', 'mode': 'generation', 'securityLevel': 0,
            'osVersion': 90000, 'osPatchLevel': 0, 'vendorPatchLevel': 0, 'bootPatchLevel': 0,
            'packages': [args.package], 'uids': [args.uid], 'uidPackages': [args.package],
            'keyboxB64': base64.b64encode(xml.encode()).decode()}]}
    for name, content in [('test-keybox.xml', xml), ('pocket-teesim.json', json.dumps(config))]:
        fd = os.open(args.out/name, os.O_WRONLY|os.O_CREAT|os.O_TRUNC, 0o600)
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, 'w') as stream:
            stream.write(content+'\n')
    (args.out/'test-root.pem').write_bytes(root.public_bytes(pem))
    print('Created local SOFTWARE test identity in', args.out)


if __name__ == '__main__':
    main()
