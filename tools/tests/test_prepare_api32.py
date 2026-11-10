# SPDX-License-Identifier: GPL-3.0-only
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from prepare_api32 import apex_manifest, gpt_partition, materialize_links, virtual_path


class Android12LPreparationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_apex_library_exports_and_unknown_metadata(self):
        manifest = apex_manifest(b'\x0a\x0fcom.android.art\x10\x01\x3a\x07libc.so\x3a\x07libm.so\x62\x02\x08\x01')
        self.assertEqual(manifest['name'], 'com.android.art')
        self.assertEqual(manifest['provideNativeLibs'], ['libc.so', 'libm.so'])

    def test_malformed_apex_is_rejected(self):
        for raw in [b'', b'\x0a\xff', b'\x0a\x01a\x10\x01', b'\x08\x01', b'\x0a\x0fcom.android.art\x10' + b'\xff' * 10]:
            with self.subTest(raw=raw), self.assertRaises((ValueError, UnicodeDecodeError)):
                apex_manifest(raw)

    def test_links_resolve_inside_guest_and_are_materialized(self):
        (self.root / 'system/lib64').mkdir(parents=True)
        (self.root / 'apex/com.android.runtime/lib64').mkdir(parents=True)
        actual = self.root / 'apex/com.android.runtime/lib64/libc.so'
        actual.write_bytes(b'guest libc')
        (self.root / 'system/lib64/libc.so').symlink_to('/apex/com.android.runtime/lib64/libc.so')
        self.assertEqual(virtual_path(self.root, 'system/lib64/libc.so'), actual)
        materialize_links(self.root)
        self.assertEqual((self.root / 'system/lib64/libc.so').read_bytes(), b'guest libc')
        self.assertFalse(any(p.is_symlink() for p in self.root.rglob('*')))

    def test_escape_cycle_and_host_fallback_are_rejected(self):
        (self.root / 'escape').symlink_to('../outside')
        (self.root / 'cycle').symlink_to('cycle')
        (self.root / 'missing').symlink_to('/etc/passwd')
        for name in ['escape', 'cycle']:
            with self.subTest(name=name), self.assertRaises(ValueError):
                virtual_path(self.root, name)
        self.assertEqual(virtual_path(self.root, 'missing'), self.root / 'etc/passwd')
        self.assertFalse(virtual_path(self.root, 'missing').exists())

    def test_checked_gpt_partition_range_and_corruption(self):
        image = self.root / 'disk.img'
        data = bytearray(80 * 512)
        entry = bytearray(128)
        entry[:16] = b'1' * 16
        struct.pack_into('<QQ', entry, 32, 40, 60)
        entry[56:66] = 'super'.encode('utf-16le')
        data[1024:1152] = entry
        header = bytearray(92)
        header[:8] = b'EFI PART'
        struct.pack_into('<II', header, 8, 0x10000, 92)
        struct.pack_into('<QIII', header, 72, 2, 1, 128, zlib.crc32(entry))
        struct.pack_into('<I', header, 16, zlib.crc32(header))
        data[512:604] = header
        image.write_bytes(data)
        self.assertEqual(gpt_partition(image, 'super'), (40 * 512, 21 * 512))
        data[1100] ^= 1
        image.write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'partition table checksum'):
            gpt_partition(image, 'super')
        data[512] ^= 1
        image.write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'GPT header'):
            gpt_partition(image, 'super')


if __name__ == '__main__':
    unittest.main()
