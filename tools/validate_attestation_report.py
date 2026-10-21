#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate a probe's cryptography and reported claims against an explicit root.

Matching claims does not establish hardware enforcement or Play Integrity.
"""
import argparse
import base64
import json
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec, padding, rsa


def der_item(data, offset=0):
    """Read one bounded, definite-length DER item (including high-number tags)."""
    start = offset
    first = data[offset]
    offset += 1
    if first & 31 == 31:
        while data[offset] & 128:
            offset += 1
        offset += 1
    tag = data[start:offset]
    size = data[offset]
    offset += 1
    if size & 128:
        count = size & 127
        assert 0 < count <= 4, 'Invalid DER length'
        size = int.from_bytes(data[offset:offset+count], 'big')
        offset += count
    end = offset + size
    assert start < end <= len(data), 'Truncated DER'
    return tag, data[offset:end], end


def children(data):
    result, offset = [], 0
    while offset < len(data):
        tag, value, offset = der_item(data, offset)
        result.append((tag, value))
    return result


def verify_certificate(cert, issuer):
    key = issuer.public_key()
    if isinstance(key, rsa.RSAPublicKey):
        key.verify(cert.signature, cert.tbs_certificate_bytes,
                   cert.signature_algorithm_parameters or padding.PKCS1v15(), cert.signature_hash_algorithm)
    else:
        key.verify(cert.signature, cert.tbs_certificate_bytes, ec.ECDSA(cert.signature_hash_algorithm))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--root', type=Path, required=True, help='Public PEM certificate; no private key')
    parser.add_argument('--expected-security-level', type=int, choices=(0, 1, 2), default=0,
                        help='Expected reported claim: software=0, TEE=1, StrongBox=2; not proof of enforcement')
    parser.add_argument('--chain-length', type=int, default=3)
    parser.add_argument('--expected-device-locked', choices=('true', 'false'))
    parser.add_argument('--expected-boot-state', type=int, choices=(0, 1, 2, 3))
    args = parser.parse_args()
    report = json.loads(args.report.read_text())
    assert report['uid'] >= 10000 and report['sdk'] == 28
    for name in ('plain', 'attested'):
        test = report[name]
        assert test['status'] == 'PASS' and test['signatureVerified'], name
        assert test['insideSecureHardware'] is (args.expected_security_level != 0), name
        assert test['temporaryKeyDeleted'] is True, name
    chain = [x509.load_der_x509_certificate(base64.b64decode(c['der']))
             for c in report['attested']['certificates']]
    assert len(chain) == args.chain_length
    expected_root = x509.load_pem_x509_certificate(args.root.read_bytes())
    assert chain[-1].fingerprint(hashes.SHA256()) == expected_root.fingerprint(hashes.SHA256())
    for index, cert in enumerate(chain):
        issuer = chain[min(index+1, len(chain)-1)]
        assert cert.issuer == issuer.subject
        verify_certificate(cert, issuer)
    extension = chain[0].extensions.get_extension_for_oid(
        x509.ObjectIdentifier('1.3.6.1.4.1.11129.2.1.17')).value.value
    tag, sequence, end = der_item(extension)
    assert tag == b'\x30' and end == len(extension)
    fields = children(sequence)
    assert len(fields) == 8
    level = bytes([args.expected_security_level])
    assert fields[1] == (b'\x0a', level) and fields[3] == (b'\x0a', level), 'Reported security level mismatch'
    assert fields[4] == (b'\x04', base64.b64decode(report['challenge'])), 'Caller challenge mismatch'
    if args.expected_device_locked is not None or args.expected_boot_state is not None:
        # RootOfTrust is authorization tag [704], encoded BF 85 40.
        roots = [value for auth in fields[6:8] for tag, value in children(auth[1]) if tag == b'\xbf\x85\x40']
        assert len(roots) == 1, 'Expected exactly one RootOfTrust'
        tag, value, end = der_item(roots[0])
        assert tag == b'\x30' and end == len(roots[0])
        boot = children(value)
        if args.expected_device_locked is not None:
            assert boot[1] == (b'\x01', b'\xff' if args.expected_device_locked == 'true' else b'\x00')
        if args.expected_boot_state is not None:
            assert boot[2] == (b'\x0a', bytes([args.expected_boot_state]))
    strongbox = report['strongBox']
    if args.expected_security_level != 2:
        assert strongbox['status'] == 'FAIL'
        assert any(c.get('errorCode') == -68 for c in strongbox['error']['causes'])
    else:
        assert strongbox['status'] == 'PASS' and strongbox['signatureVerified']
        assert strongbox['insideSecureHardware'] is True
    print('PASS: ordinary Android 9 app; generated keys and sign/verify;', len(chain), 'signed certificates')
    print('PASS: explicit root, exact caller challenge, reported security level', args.expected_security_level)
    print('Claim/crypto test only: revocation, hardware enforcement and Play Integrity are separate checks')


if __name__ == '__main__':
    main()
