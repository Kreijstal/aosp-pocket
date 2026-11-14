# SPDX-License-Identifier: GPL-3.0-only
"""Recovery evidence must fail closed before installing or setting credentials."""
from pathlib import Path
import copy
import hashlib
import json
import sys
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from test_api32_ce_manager_on_pi import NORMAL_PACKAGE,load_recovered_retained_profile


def write(path,value):
    path.write_text(json.dumps(value))
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cycle(migration=False):
    # Synthetic wire/log fixture for the guard; this is not a runtime proof.
    return {'migration':migration,'session':{'ownerAlive':False,'ownerExitCode':0,
        'input':{'inputEventsRejected':0},'externalStorage':{'attached':False,'attaches':1,'revocations':0,'workerStopped':True}},
        'nativeLog':'Start proc 123:com.android.systemui/u0a1\nStarting session for id: emulated;0\n',
        'console':'API32_INIT_SUBREAPER_READY kernel_child_ownership=1\nINIT_DESCENDANT_REAP pid=124 code=2 status=15\nAPI32_INIT_DESCENDANTS_STOPPED reaped=1 signalled=0 remaining=0 query_error=0\nAPI32_INIT_STOPPED reaped=15 remaining=0 failed=0\nAPI32_EXTERNAL_VOLUME_MOUNT result=0 user=0\n',
        'display':{'frames_presented':1}}


class RecoveredRetainedGuardTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.base=Path(self.temp.name);self.work=self.base/'new';self.work.mkdir()
        self.failed_dir=self.base/'failed';self.failed_dir.mkdir();self.restored_dir=self.base/'restored';self.restored_dir.mkdir()
        self.failure_path=self.failed_dir/'ce-manager-report.json';self.recovery_path=self.restored_dir/'restore-report.json'
        self.original={'uid':10042,'token':'original-token','cipherDigest':'d'*64,'canonicalDataPath':'/data/user/0/example',
            'nonExportable':True,'cryptoValid':True}
        self.previous={'validation':'PASS','strictPassed':True,'managerPackage':NORMAL_PACKAGE,'vmInstance':73,
            'apkSha256':'e'*64,'imageSha256':'f'*64,'embeddedManagerControlVerified':True,'externalVolumeVerified':True,
            'app':{'storageEncryptionEnabledBeforeStart':True,'fiveRootEncryptionLayoutBeforeStart':True,
                'stoppedSession':{'ownerAlive':False,'ownerExitCode':0}}}
        self.failed={'validation':'FAIL','managerPackage':NORMAL_PACKAGE,'vmInstance':73,'retainedPlaintextMigration':True,
            'guestDataReset':False,'freshProfile':False,'runId':'a'*32,'cycles':[]}
        for i in range(4):
            path=self.failed_dir/('ce-manager-cycle-'+'a'*32+'-'+str(i)+'.json');value=dict(cycle(i==0),runId='a'*32,index=i)
            self.failed['cycles'].append({'path':str(path),'sha256':write(path,value),'index':i})
        write(self.failure_path,self.failed)
        self.snapshot_path=self.failed_dir/'app-failure-snapshot.json'
        write(self.snapshot_path,{'runId':'a'*32,'validation':'FAIL','cycleIndex':4,'retainedPlaintextMigration':True,
            'originalApp':self.original,'retainedInventoryBefore':{'includesKeptData':True,'packages':[{'package':'example'}]}})
        self.recovered={'status':'PASS_OWN_TEST_AUTHLESS_RESTORATION_AND_AUTOMATIC_REOPEN','vm':73,'sourceRunId':'a'*32,
            'sourceCredentialsRemovedAfterVerifiedAuthlessBoot':True,'failedFullLifecycleStillFailed':True,
            'guestDataReset':False,'historicalCredentialsRead':False,'phases':[]}
        for i,nonce in enumerate(['b'*32,'c'*32]):
            path=self.restored_dir/f'phase-{i}-result.json'
            value={'runId':nonce,'sourceRunId':'a'*32,'validation':'PASS','originalPasswordFreeStateRestored':True,
                'originalAppPreserved':True,'verifyAuthlessOnly':bool(i),'authlessBootVerified':bool(i),
                'keyguard':{'secure':False},'readableCanariesVerified':5,'publicCanaryVerified':True,
                'app':copy.deepcopy(self.original),'cycle':cycle()}
            self.recovered['phases'].append({'path':str(path),'sha256':write(path,value),'runId':nonce})
        write(self.recovery_path,self.recovered)

    def load(self):
        return load_recovered_retained_profile(self.base,self.work,self.failure_path,self.recovery_path,self.previous)

    def change_phase(self,field,value):
        item=self.recovered['phases'][1];path=Path(item['path']);state=json.loads(path.read_text())
        state[field]=value;item['sha256']=write(path,state);write(self.recovery_path,self.recovered)

    def test_complete_evidence_is_read_only_and_keeps_failure_failed(self):
        before={p:p.read_bytes() for p in self.base.rglob('*.json')}
        evidence=self.load()
        self.assertEqual(evidence['failed']['validation'],'FAIL')
        self.assertEqual(evidence['authlessState']['runId'],'c'*32)
        self.assertEqual(len(evidence['sourcePins']),9)
        self.assertEqual(before,{p:p.read_bytes() for p in self.base.rglob('*.json')})
        self.assertEqual(list(self.work.iterdir()),[])

    def test_plaintext_active_or_other_current_baseline_is_rejected(self):
        for kind in ['plaintext','active','other_package','other_vm','nonboolean']:
            original=copy.deepcopy(self.previous)
            if kind=='plaintext':self.previous['app']['storageEncryptionEnabledBeforeStart']=False
            if kind=='active':self.previous['app']['stoppedSession']['ownerAlive']=True
            if kind=='other_package':self.previous['managerPackage']=NORMAL_PACKAGE+'3'
            if kind=='other_vm':self.previous['vmInstance']=72
            if kind=='nonboolean':self.previous['strictPassed']='false'
            with self.subTest(kind=kind),self.assertRaises(AssertionError):self.load()
            self.previous=original

    def test_wrong_source_nonce_and_stale_phase_pin_are_rejected(self):
        self.recovered['sourceRunId']='d'*32;write(self.recovery_path,self.recovered)
        with self.assertRaises(AssertionError):self.load()
        self.recovered['sourceRunId']='a'*32;self.recovered['phases'][1]['sha256']='0'*64;write(self.recovery_path,self.recovered)
        with self.assertRaises(AssertionError):self.load()

    def test_key_loss_secure_state_and_failed_native_stop_are_rejected(self):
        path=Path(self.recovered['phases'][1]['path']);original=json.loads(path.read_text())
        for kind in ['key','secure','owner','second_mount']:
            state=copy.deepcopy(original)
            if kind=='key':state['app']['token']='replacement'
            if kind=='secure':state['keyguard']['secure']=True
            if kind=='owner':state['cycle']['session']['ownerAlive']=True
            if kind=='second_mount':state['cycle']['console']+='API32_EXTERNAL_VOLUME_MOUNT result=-5 user=0\n'
            self.recovered['phases'][1]['sha256']=write(path,state);write(self.recovery_path,self.recovered)
            with self.subTest(kind=kind),self.assertRaises(AssertionError):self.load()

    def test_recovery_credentials_and_dangling_credential_link_block_new_trial(self):
        path=self.failed_dir/'test-credentials.json';path.write_text('must not be read')
        with self.assertRaises(AssertionError):self.load()
        self.assertEqual(path.read_text(),'must not be read');path.unlink();path.symlink_to('absent-own-file')
        with self.assertRaises(AssertionError):self.load()

    def test_symlinked_or_external_report_is_rejected(self):
        alias=self.base/'alias';alias.symlink_to(self.restored_dir,target_is_directory=True)
        with self.assertRaises(AssertionError):load_recovered_retained_profile(self.base,self.work,self.failure_path,alias/'restore-report.json',self.previous)
        self.recovered['phases'][1]['path']=str(self.base.parent/'unrelated.json');write(self.recovery_path,self.recovered)
        with self.assertRaises(AssertionError):self.load()


if __name__=='__main__':unittest.main()
