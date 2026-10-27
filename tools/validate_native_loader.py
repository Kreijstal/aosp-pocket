#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check the ELF interpreter layout required by older Android kernels."""
import argparse
from pathlib import Path
import struct


def validate(path):
    data = Path(path).read_bytes()
    assert data[:6] == b'\x7fELF\x02\x01', 'Expected little-endian ELF64'
    assert struct.unpack_from('<HH', data, 16) == (2, 183), 'Expected static AArch64 ET_EXEC interpreter'
    offset, = struct.unpack_from('<Q', data, 32)
    stride, count = struct.unpack_from('<HH', data, 54)
    assert stride == 56 and 0 < count <= 64 and offset + stride*count <= len(data), 'Invalid program headers'
    headers = [struct.unpack_from('<IIQQQQQQ', data, offset+i*stride) for i in range(count)]
    assert not any(p[0] in (2, 3) for p in headers), 'Interpreter must have no dynamic section or PT_INTERP'
    loads = [p for p in headers if p[0] == 1]
    assert loads and any(p[1] & 2 for p in loads), 'Missing loader runtime data'
    for p in loads:
        assert p[5] == p[6], 'Unbacked BSS can fault under older Android interpreter loaders'
        assert p[2] + p[5] <= len(data), 'Segment extends past file'
        assert p[7] >= 16384 and p[2] % p[7] == p[3] % p[7], 'Invalid segment alignment'
    final = max(loads, key=lambda p: p[3])
    assert final[1] == 5 and final[5] and final[5] % 16384 == 0, 'Final probe segment must be page-complete RX'
    return {'segments': len(loads), 'unbackedBssBytes': 0, 'finalSegmentBytes': final[5]}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('loader', type=Path)
    args = parser.parse_args()
    print('PASS:', args.loader, validate(args.loader))
