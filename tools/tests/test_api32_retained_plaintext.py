# SPDX-License-Identifier: GPL-3.0-only
"""Reject encrypted, active or unrelated baselines before replacing any APK."""
from pathlib import Path
import copy
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from test_api32_ce_manager_on_pi import NORMAL_PACKAGE,validate_retained_plaintext_baseline

BASELINE={'validation':'PASS','strictPassed':True,'managerPackage':NORMAL_PACKAGE,
    'embeddedManagerControlVerified':True,'apkSha256':'a'*64,'imageSha256':'b'*64,
    'app':{'stoppedSession':{'ownerAlive':False,'ownerExitCode':0},
           'storageEncryptionEnabledBeforeStart':False,'fiveRootEncryptionLayoutBeforeStart':False}}


class RetainedPlaintextBaselineTest(unittest.TestCase):
    def test_completed_plaintext_normal_profile_is_accepted(self):
        validate_retained_plaintext_baseline(copy.deepcopy(BASELINE))

    def test_encryption_and_unknown_state_fail_closed(self):
        for field in ['storageEncryptionEnabledBeforeStart','fiveRootEncryptionLayoutBeforeStart']:
            for value in [True,None,0,'false']:
                changed=copy.deepcopy(BASELINE);changed['app'][field]=value
                with self.subTest(field=field,value=value),self.assertRaises(AssertionError):validate_retained_plaintext_baseline(changed)

    def test_active_or_unclean_owner_is_rejected(self):
        for field,value in [('ownerAlive',True),('ownerAlive',None),('ownerExitCode',1),('ownerExitCode',None)]:
            changed=copy.deepcopy(BASELINE);changed['app']['stoppedSession'][field]=value
            with self.subTest(field=field,value=value),self.assertRaises(AssertionError):validate_retained_plaintext_baseline(changed)

    def test_other_package_missing_control_and_invalid_pins_are_rejected(self):
        for field,value in [('managerPackage',NORMAL_PACKAGE+'3'),('validation','FAIL'),('strictPassed',False),
                ('embeddedManagerControlVerified',False),('apkSha256',''),('imageSha256','X'*64)]:
            changed=copy.deepcopy(BASELINE);changed[field]=value
            with self.subTest(field=field),self.assertRaises(AssertionError):validate_retained_plaintext_baseline(changed)


if __name__=='__main__':unittest.main()
