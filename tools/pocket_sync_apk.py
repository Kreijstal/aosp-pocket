# SPDX-License-Identifier: GPL-3.0-only
"""APK inspection for pocket_sync: versionName/versionCode from the binary
AndroidManifest.xml and the native ABIs an APK set needs."""
import re
import struct
import zipfile

RES_STRING_POOL, RES_XML, RES_XML_START_ELEMENT = 0x0001, 0x0003, 0x0102
TYPE_STRING, TYPE_INT_DEC, TYPE_INT_HEX = 0x03, 0x10, 0x11
ATTR_VERSION_CODE, ATTR_VERSION_NAME = 0x0101021b, 0x0101021c
ABIS = ('arm64-v8a', 'armeabi-v7a', 'armeabi', 'x86_64', 'x86', 'riscv64', 'mips', 'mips64')


def _string_pool(data, off):
    _, hsize, _ = struct.unpack_from('<HHI', data, off)
    count, _, flags, strings_start, _ = struct.unpack_from('<IIIII', data, off + 8)
    offsets = struct.unpack_from(f'<{count}I', data, off + hsize)
    utf8 = bool(flags & 0x100)
    out = []
    for o in offsets:
        p = off + strings_start + o
        if utf8:
            n = data[p]; p += 2 if n & 0x80 else 1          # utf16 length (skipped)
            n = data[p]
            if n & 0x80:
                n = ((n & 0x7f) << 8) | data[p + 1]; p += 2
            else:
                p += 1
            out.append(data[p:p + n].decode('utf-8', 'replace'))
        else:
            n = struct.unpack_from('<H', data, p)[0]; p += 2
            if n & 0x8000:
                n = ((n & 0x7fff) << 16) | struct.unpack_from('<H', data, p)[0]; p += 2
            out.append(data[p:p + 2 * n].decode('utf-16-le', 'replace'))
    return out


def manifest_info(data):
    """Return (package, versionCode, versionName) from binary manifest bytes."""
    if struct.unpack_from('<H', data, 0)[0] != RES_XML:
        raise ValueError('not a binary XML manifest')
    off = struct.unpack_from('<H', data, 2)[0]
    strings, resmap = [], []
    while off + 8 <= len(data):
        ctype, hsize, csize = struct.unpack_from('<HHI', data, off)
        if csize < 8:
            break
        if ctype == RES_STRING_POOL:
            strings = _string_pool(data, off)
        elif ctype == 0x0180:
            resmap = list(struct.unpack_from(f'<{(csize - hsize) // 4}I', data, off + hsize))
        elif ctype == RES_XML_START_ELEMENT:
            ns, name, attr_start, attr_size, attr_count = struct.unpack_from('<iiHHH', data, off + 16)
            if strings[name] == 'manifest':
                package = code = vname = None
                base = off + 16 + attr_start
                for i in range(attr_count):
                    a_ns, a_name, a_raw, _, _, a_type, a_data = struct.unpack_from(
                        '<iiiHBBI', data, base + i * attr_size)
                    rid = resmap[a_name] if 0 <= a_name < len(resmap) else None
                    label = strings[a_name] if 0 <= a_name < len(strings) else ''
                    value = strings[a_raw] if a_raw >= 0 else (
                        strings[a_data] if a_type == TYPE_STRING else a_data)
                    if label == 'package':
                        package = value
                    elif rid == ATTR_VERSION_CODE or label == 'versionCode':
                        code = int(value) if not isinstance(value, int) else value
                    elif rid == ATTR_VERSION_NAME or label == 'versionName':
                        vname = value if isinstance(value, str) else None
                return package, code, vname
        off += csize
    raise ValueError('manifest element not found')


def apk_info(path):
    """dict(package, version_code, version_name, split, abis) for one APK file."""
    with zipfile.ZipFile(path) as z:
        package, code, vname = manifest_info(z.read('AndroidManifest.xml'))
        abis = sorted({m.group(1) for n in z.namelist()
                       if (m := re.match(r'lib/([^/]+)/[^/]+\.so$', n))})
    split = None
    name = str(path).rsplit('/', 1)[-1]
    if (m := re.match(r'split_(.+)\.apk$', name)):
        split = m.group(1)
    return {'package': package, 'version_code': code, 'version_name': vname,
            'split': split, 'abis': abis}


def split_abi(split):
    """'config.arm64_v8a' -> 'arm64-v8a'; None when the split is not an ABI split."""
    if not split:
        return None
    tail = split.rsplit('.', 1)[-1].replace('_', '-')
    return tail if tail in ABIS else None


def select_for_abis(apks, target_abis):
    """Choose the APK files to install on a device supporting target_abis.

    apks: {filename: apk_info}. Returns (files, abi or None) or raises
    ValueError explaining why the set can't run there."""
    base = apks.get('base.apk')
    if base is None:
        raise ValueError('no base.apk')
    abi_splits = {split_abi(i['split']): f for f, i in apks.items() if split_abi(i['split'])}
    plain = [f for f, i in apks.items() if f != 'base.apk' and not split_abi(i['split'])]
    native = set(base['abis']) | set(abi_splits)
    for f, i in apks.items():
        if f != 'base.apk' and not split_abi(i['split']):
            native |= set(i['abis'])
    if not native:
        return ['base.apk'] + sorted(plain), None
    for abi in target_abis:
        if abi in abi_splits:
            return ['base.apk'] + sorted(plain) + [abi_splits[abi]], abi
        if abi in base['abis']:
            return ['base.apk'] + sorted(plain), abi
    raise ValueError('native code only for ' + ', '.join(sorted(native))
                     + '; target needs ' + '/'.join(target_abis))


def version_key(name):
    """Sortable key for versionName comparison ('4.2.3 (37)' -> (4,2,3,37))."""
    return tuple(int(x) for x in re.findall(r'\d+', name or ''))
