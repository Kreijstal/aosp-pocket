# SPDX-License-Identifier: GPL-3.0-only
import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from prepare_api29 import stage_reference


class Android10ReferenceTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self): self.temporary.cleanup()

    def archive(self, *, sdk=29, extra=None, missing_jar=False):
        path = self.root / "reference.tar.gz"
        elf = bytearray(64); elf[:5] = b"\x7fELF\x02"; elf[18:20] = (183).to_bytes(2, "little")
        files = {"system/build.prop": f"ro.build.version.sdk={sdk}\n".encode(),
                 "init.environ.rc": b"on early-init\n    export BOOTCLASSPATH /apex/com.android.runtime/javalib/core-oj.jar\n",
                 "system/apex/com.android.runtime.debug/apex_manifest.json": json.dumps({"name": "com.android.runtime", "version": 1}).encode(),
                 "system/apex/com.android.runtime.debug/bin/linker64": bytes(elf),
                 "system/apex/com.android.runtime.debug/bin/dalvikvm64": bytes(elf),
                 "system/apex/com.android.runtime.debug/lib64/bionic/libc.so": bytes(elf),
                 "system/bin/sh": bytes(elf), "system/bin/app_process64": bytes(elf)}
        if not missing_jar: files["system/apex/com.android.runtime.debug/javalib/core-oj.jar"] = b"matching-runtime-jar"
        with tarfile.open(path, "w:gz") as tf:
            for name, data in files.items():
                entry = tarfile.TarInfo(name); entry.mode = 0o755; entry.size = len(data)
                tf.addfile(entry, io.BytesIO(data))
            for name in ["linker64", "dalvikvm64"]:
                entry = tarfile.TarInfo("system/bin/" + name); entry.type = tarfile.SYMTYPE
                entry.linkname = "/apex/com.android.runtime/bin/" + name; tf.addfile(entry)
            if extra:
                entry = tarfile.TarInfo(extra[0]); entry.type = tarfile.SYMTYPE; entry.linkname = extra[1]; tf.addfile(entry)
        return path, hashlib.sha256(path.read_bytes()).hexdigest()

    def test_apex_boot_paths_are_regular_payloads(self):
        archive, digest = self.archive(); root = stage_reference(archive, self.root / "staged", digest)
        self.assertEqual((root / "apex/com.android.runtime/javalib/core-oj.jar").read_bytes(), b"matching-runtime-jar")
        for path in ["system/bin/linker64", "system/bin/dalvikvm64"]:
            self.assertTrue((root / path).is_file()); self.assertFalse((root / path).is_symlink())
        self.assertFalse(any(p.is_symlink() for p in root.rglob("*")))

    def test_digest_mismatch_does_not_publish(self):
        archive, _ = self.archive()
        with self.assertRaisesRegex(ValueError, "pinned OCI digest"):
            stage_reference(archive, self.root / "staged", "0" * 64)
        self.assertFalse((self.root / "staged").exists())

    def test_escaping_archive_link_rejected(self):
        archive, digest = self.archive(extra=("system/escape", "../../outside"))
        with self.assertRaisesRegex(ValueError, "Unsafe"):
            stage_reference(archive, self.root / "staged", digest)
        self.assertFalse((self.root / "outside").exists())

    def test_wrong_sdk_and_missing_boot_payload_rejected(self):
        for options in [{"sdk": 28}, {"missing_jar": True}]:
            with self.subTest(options=options):
                archive, digest = self.archive(**options)
                with self.assertRaises(ValueError): stage_reference(archive, self.root / "staged", digest)
                self.assertFalse((self.root / "staged").exists())

    def test_unrelated_directory_is_preserved(self):
        archive, digest = self.archive(); destination = self.root / "staged"; destination.mkdir()
        (destination / "keep").write_text("existing guest data")
        with self.assertRaisesRegex(ValueError, "unrelated"):
            stage_reference(archive, destination, digest)
        self.assertEqual((destination / "keep").read_text(), "existing guest data")


if __name__ == "__main__": unittest.main()
