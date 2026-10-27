#!/usr/bin/env python3
"""Focused offline checks for delivery, credentials, reconnect and failure handling."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

REPO=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('setup_samsung',REPO/'tools/setup_samsung_pocket.py')
setup=importlib.util.module_from_spec(spec);spec.loader.exec_module(setup)
BUNDLE=REPO/'out/samsung-microg-lock/transfer'


class DeliveryTests(unittest.TestCase):
    def run_setup(self,preflight,final=None,password=False,offline=False):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);(root/'tools').symlink_to(REPO/'tools',target_is_directory=True)
            calls=[];transferred=[];credentials=[]
            def run(argv,**kwargs):
                calls.append(argv)
                if offline:raise subprocess.TimeoutExpired(argv,5)
                if argv[0]=='rsync':
                    stage=Path(argv[-2]);hashes=json.loads((stage/'payload-sha256.json').read_text())
                    for name,digest in hashes.items():self.assertEqual(setup.sha256(stage/name),digest)
                    self.assertIn('provision.py',hashes);self.assertIn('pocketctl.py',hashes)
                    transferred.append(hashes)
                    return subprocess.CompletedProcess(argv,0)
                code=shlex.split(argv[-1])[2];compile(code,'remote-snippet','exec')
                if 'getprop' in code:result=preflight
                elif 'sys.stdin.buffer.read()' in code:
                    credentials.append(json.loads(kwargs['input']));result=None
                elif 'subprocess.Popen' in code:
                    self.assertIn('start_new_session=True',code);result={'pid':123}
                else:result={'report':final or {'validation':'PASS'},'log':'DONE\n','alive':True}
                return subprocess.CompletedProcess(argv,0,b'' if result is None else json.dumps(result).encode(),b'')
            argv=['setup','--bundle',str(BUNDLE)]
            if password:
                private=root/'password';private.write_text('fixture-password');private.chmod(0o600)
                argv+=['--password-file',str(private)]
            with patch.object(setup,'REPO',root),patch.object(sys,'argv',argv),patch.object(setup.subprocess,'run',run),patch.object(setup.shutil,'which',return_value='/usr/bin/mock'),contextlib.redirect_stdout(io.StringIO()):
                result=setup.main()
            return result,calls,transferred,credentials

    def test_complete_delivery_and_report(self):
        result,calls,transferred,_=self.run_setup({'active':False,'credential':True})
        self.assertEqual(result,0);self.assertEqual(len(transferred),1);self.assertEqual(len(calls),4)

    def test_existing_job_is_followed_without_reinstall(self):
        result,calls,transferred,_=self.run_setup({'active':True,'credential':True})
        self.assertEqual(result,0);self.assertEqual(len(calls),2);self.assertFalse(transferred)

    def test_completed_job_is_retrieved_without_restart(self):
        result,calls,transferred,_=self.run_setup({'active':False,'credential':True,'report':{'validation':'PASS'}})
        self.assertEqual(result,0);self.assertEqual(len(calls),1);self.assertFalse(transferred)

    def test_password_only_travels_on_stdin(self):
        result,calls,_,credentials=self.run_setup({'active':False,'credential':False},password=True)
        self.assertEqual(result,0);self.assertEqual(credentials,[{'scope':setup.SCOPE,'password':'fixture-password'}])
        self.assertNotIn('fixture-password',str(calls))

    def test_failed_phone_report_returns_failure(self):
        result,_,_,_=self.run_setup({'active':True,'credential':True},final={'validation':'FAIL','error':'fixture failure'})
        self.assertEqual(result,1)

    def test_offline_phone_fails_before_transfer(self):
        with self.assertRaises(subprocess.TimeoutExpired):self.run_setup({},offline=True)

    def test_modified_apk_is_rejected_before_connection(self):
        actual=setup.sha256
        with patch.object(setup,'sha256',side_effect=lambda path:'0'*64 if path.name=='GsfProxy-0.1.0.apk' else actual(path)):
            with self.assertRaisesRegex(ValueError,'Official APK checksum mismatch'):setup.validate_bundle(BUNDLE)


if __name__=='__main__':unittest.main()
