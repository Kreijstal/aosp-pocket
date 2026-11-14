# SPDX-License-Identifier: GPL-3.0-only
"""Recovery data must survive failures without replacing another run's file."""
from pathlib import Path
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from test_api32_ce_manager_on_pi import persist_test_credential_recovery,persist_stopped_cycle


class CredentialRecoveryTest(unittest.TestCase):
    def payload(self):
        return json.dumps({'runId':'a'*32,'passwords':['P32'+value*14 for value in '123']}).encode()

    def test_durable_owner_only_file_survives_later_driver_failure(self):
        with tempfile.TemporaryDirectory() as folder:
            path=persist_test_credential_recovery(Path(folder),self.payload())
            try:raise RuntimeError('later observation failed')
            except RuntimeError:pass
            self.assertEqual(path.read_bytes(),self.payload())
            self.assertEqual(path.stat().st_mode&0o777,0o600)
            self.assertEqual(path.stat().st_uid,os.getuid())

    def test_existing_recovery_is_never_replaced(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'test-credentials.json';path.write_bytes(b'earlier-owned-run')
            with self.assertRaises(FileExistsError):persist_test_credential_recovery(Path(folder),self.payload())
            self.assertEqual(path.read_bytes(),b'earlier-owned-run')

    def test_dangling_link_is_not_followed(self):
        with tempfile.TemporaryDirectory() as folder:
            target=Path(folder)/'outside';(Path(folder)/'test-credentials.json').symlink_to(target)
            with self.assertRaises(FileExistsError):persist_test_credential_recovery(Path(folder),self.payload())
            self.assertFalse(target.exists())

    def test_invalid_credentials_do_not_create_a_file(self):
        for value in [{'runId':'bad','passwords':['P32'+'1'*14]*3},
                      {'runId':'a'*32,'passwords':['not-a-test-password']*3}]:
            with self.subTest(value='redacted'),tempfile.TemporaryDirectory() as folder:
                with self.assertRaises(AssertionError):persist_test_credential_recovery(Path(folder),json.dumps(value).encode())
                self.assertEqual(list(Path(folder).iterdir()),[])


class StoppedCyclePreservationTest(unittest.TestCase):
    def test_raw_cycle_survives_a_later_validation_failure(self):
        with tempfile.TemporaryDirectory() as folder:
            cycle={'runId':'b'*32,'index':4,'migration':False,'console':'mount failed then retried'}
            saved=persist_stopped_cycle(Path(folder),'b'*32,4,cycle)
            try:raise AssertionError('strict validation rejected the cycle')
            except AssertionError:pass
            self.assertEqual(json.loads(Path(saved['path']).read_text()),cycle)

    def test_existing_or_foreign_cycle_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as folder:
            cycle={'runId':'b'*32,'index':4,'migration':False}
            saved=persist_stopped_cycle(Path(folder),'b'*32,4,cycle)
            original=Path(saved['path']).read_bytes()
            with self.assertRaises(FileExistsError):persist_stopped_cycle(Path(folder),'b'*32,4,cycle)
            with self.assertRaises(AssertionError):persist_stopped_cycle(Path(folder),'c'*32,4,cycle)
            self.assertEqual(Path(saved['path']).read_bytes(),original)


if __name__=='__main__':unittest.main()
