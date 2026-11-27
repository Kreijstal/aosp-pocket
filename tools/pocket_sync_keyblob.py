# SPDX-License-Identifier: GPL-3.0-only
"""Read and re-seal AOSP software-Keymaster key blobs as stored by keystore2.

Both Waydroid (keymaster@4.0-service) and Pocket 12L (pocket-crypto-api32) use
system/keymaster's PureSoftKeymasterContext. Its blobs are *integrity assured*,
not encrypted:

    keystore2 km_compat prefix: b"pKMblob" + isSoftKeyMint(0/1)      (8 bytes)
    version 0 (1 byte)
    key_material  : u32 len + bytes
    hw_enforced   : AuthorizationSet
    sw_enforced   : AuthorizationSet
    hmac          : HMAC-SHA256(b"IntegrityAssuredBlob0\\0", above + hidden)[:8]

hidden = {APPLICATION_ID, APPLICATION_DATA if given, ROOT_OF_TRUST = b"SW"}.
All integers are little endian (x86_64 and arm64 alike). See
system/keymaster key_blob_utils/integrity_assured_key_blob.cpp.
"""
import hashlib
import hmac
import struct

PREFIX_MAGIC = b'pKMblob'
HMAC_KEY = b'IntegrityAssuredBlob0\x00'  # sizeof() of the C string includes NUL
HMAC_SIZE = 8

# Tag types (keymaster_tag_type_t), upper 4 bits of the tag.
ENUM, ENUM_REP, UINT, UINT_REP, ULONG, DATE, BOOL, BIGNUM, BYTES, ULONG_REP = (
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
TAG_APPLICATION_ID = (BYTES << 28) | 601
TAG_APPLICATION_DATA = (BYTES << 28) | 700
TAG_ROOT_OF_TRUST = (BYTES << 28) | 704
TAG_OS_VERSION = (UINT << 28) | 705
TAG_OS_PATCHLEVEL = (UINT << 28) | 706
TAG_VENDOR_PATCHLEVEL = (UINT << 28) | 718
TAG_BOOT_PATCHLEVEL = (UINT << 28) | 719
VERSION_TAGS = (TAG_OS_VERSION, TAG_OS_PATCHLEVEL, TAG_VENDOR_PATCHLEVEL, TAG_BOOT_PATCHLEVEL)


class BlobError(ValueError):
    pass


def _ttype(tag):
    return tag >> 28


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, n):
        if self.pos + n > len(self.data):
            raise BlobError('truncated key blob')
        part = self.data[self.pos:self.pos + n]
        self.pos += n
        return part

    def u32(self):
        return struct.unpack('<I', self.take(4))[0]

    def u64(self):
        return struct.unpack('<Q', self.take(8))[0]


def parse_authset(r):
    """Return a list of (tag, value); bytes values are resolved from indirect data."""
    indirect = r.take(r.u32())
    count, size = r.u32(), r.u32()
    elems = Reader(r.take(size))
    out = []
    for _ in range(count):
        tag = elems.u32()
        t = _ttype(tag)
        if t in (ENUM, ENUM_REP, UINT, UINT_REP):
            value = elems.u32()
        elif t in (ULONG, ULONG_REP, DATE):
            value = elems.u64()
        elif t == BOOL:
            value = bool(elems.take(1)[0])
        elif t in (BIGNUM, BYTES):
            length, offset = elems.u32(), elems.u32()
            if offset + length > len(indirect):
                raise BlobError('authorization blob points outside indirect data')
            value = indirect[offset:offset + length]
        else:
            raise BlobError(f'unknown tag type in {tag:#x}')
        out.append((tag, value))
    if elems.pos != len(elems.data):
        raise BlobError('authorization set size mismatch')
    return out


def serialize_authset(params):
    indirect, elems = bytearray(), bytearray()
    for tag, value in params:
        t = _ttype(tag)
        elems += struct.pack('<I', tag)
        if t in (ENUM, ENUM_REP, UINT, UINT_REP):
            elems += struct.pack('<I', value)
        elif t in (ULONG, ULONG_REP, DATE):
            elems += struct.pack('<Q', value)
        elif t == BOOL:
            elems += bytes([1 if value else 0])
        elif t in (BIGNUM, BYTES):
            elems += struct.pack('<II', len(value), len(indirect))
            indirect += value
        else:
            raise BlobError(f'unknown tag type in {tag:#x}')
    return (struct.pack('<I', len(indirect)) + bytes(indirect)
            + struct.pack('<II', len(params), len(elems)) + bytes(elems))


def hidden_authset(app_id=None, app_data=None):
    hidden = []
    if app_id is not None:
        hidden.append((TAG_APPLICATION_ID, app_id))
    if app_data is not None:
        hidden.append((TAG_APPLICATION_DATA, app_data))
    hidden.append((TAG_ROOT_OF_TRUST, b'SW'))
    return serialize_authset(hidden)


def _mac(body, hidden):
    return hmac.new(HMAC_KEY, body + hidden, hashlib.sha256).digest()[:HMAC_SIZE]


class KeyBlob:
    """A parsed keystore2 blob. Raises BlobError for anything that isn't a
    software integrity-assured blob (hardware-backed blobs can't be moved)."""

    def __init__(self, raw, app_id=None, app_data=None):
        self.prefix = b''
        body = raw
        if raw[:7] == PREFIX_MAGIC and len(raw) >= 8 and raw[7] in (0, 1):
            self.prefix, body = raw[:8], raw[8:]
        if len(body) < 1 + HMAC_SIZE or body[0] != 0:
            raise BlobError('not an integrity-assured software key blob')
        self.hidden = hidden_authset(app_id, app_data)
        if not hmac.compare_digest(_mac(body[:-HMAC_SIZE], self.hidden), body[-HMAC_SIZE:]):
            raise BlobError('key blob HMAC mismatch (hardware-backed or app-id-bound key)')
        r = Reader(body[:-HMAC_SIZE])
        r.take(1)
        self.key_material = r.take(r.u32())
        self.hw = parse_authset(r)
        self.sw = parse_authset(r)
        if r.pos != len(r.data):
            raise BlobError('trailing bytes in key blob')

    def versions(self):
        return {tag: value for tag, value in self.hw + self.sw if tag in VERSION_TAGS}

    def set_versions(self, new):
        """Replace OS version/patch-level tags that the blob already carries."""
        def fix(params):
            return [(t, new[t]) if t in VERSION_TAGS and t in new else (t, v) for t, v in params]
        self.hw, self.sw = fix(self.hw), fix(self.sw)

    def serialize(self):
        body = (b'\x00' + struct.pack('<I', len(self.key_material)) + self.key_material
                + serialize_authset(self.hw) + serialize_authset(self.sw))
        return self.prefix + body + _mac(body, self.hidden)
