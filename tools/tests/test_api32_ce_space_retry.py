# SPDX-License-Identifier: GPL-3.0-only
"""Only an exact pre-boot failure can authorize a preserved-profile retry."""
from pathlib import Path
import copy
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from test_api32_ce_manager_on_pi import PACKAGE,validate_precredential_space_failure


PRIOR={'runId':'old','preservedRunId':'canary'}
ERROR='java.io.IOException: Android 12L failed: Not enough storage to download and set up Android 12L'
FAILED={'validation':'FAIL','error':ERROR,'cycles':[],'managerPackage':PACKAGE+'3','imageSha256':'image',
    'preservedReportRunId':'old','preservedRunId':'canary','managerSha256':'a'*64,'testCredentialsRetainedForRecovery':True,
    'app':{'validation':'FAIL','error':ERROR,'preservedLifecycle':True,'completedCheckpointPredecessor':'old',
        'errorStack':'at ManagerSessionProbe32.boot(ManagerSessionProbe32.java:170)',
        'failedSession':{'sdk':32,'ready':False,'running':False,'closed':True,'nativeLog':''}}}


class PrecredentialSpaceRetryTest(unittest.TestCase):
    def test_exact_preboot_failure_is_accepted(self):
        validate_precredential_space_failure(copy.deepcopy(FAILED),PRIOR,'image')

    def test_any_credential_or_cycle_progress_is_rejected(self):
        for key in ['initialCredential','prepared','originalApp','encryptedBoots','cycleIndex','completedCheckpointArchive']:
            value=copy.deepcopy(FAILED);value['app'][key]=None
            with self.subTest(key=key),self.assertRaises(AssertionError):validate_precredential_space_failure(value,PRIOR,'image')
        value=copy.deepcopy(FAILED);value['cycles']=[{'index':0}]
        with self.assertRaises(AssertionError):validate_precredential_space_failure(value,PRIOR,'image')

    def test_wrong_report_image_or_failure_is_rejected(self):
        for key,value in [('validation','PASS'),('error','other failure'),('preservedReportRunId','other'),('preservedRunId','other'),('imageSha256','other'),('managerPackage',PACKAGE),('testCredentialsRetainedForRecovery',False)]:
            changed=copy.deepcopy(FAILED);changed[key]=value
            with self.subTest(key=key),self.assertRaises(AssertionError):validate_precredential_space_failure(changed,PRIOR,'image')

    def test_started_or_unclosed_guest_is_rejected(self):
        for key,value in [('running',True),('ready',True),('closed',False),('nativeLog','guest started'),('ownerAlive',True)]:
            changed=copy.deepcopy(FAILED);changed['app']['failedSession'][key]=value
            with self.subTest(key=key),self.assertRaises(AssertionError):validate_precredential_space_failure(changed,PRIOR,'image')


if __name__=='__main__':unittest.main()
