# SPDX-License-Identifier: GPL-3.0-only
"""Synthetic gate fixtures test the checker; none are Pi qualification evidence."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from check_android12l_lifecycle import CE162_REPORT, CE162_RUN, GateError, check, replay_historical_crash

REPO = Path(__file__).resolve().parents[2]


class LifecycleGateTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.config = json.loads((REPO / 'ci/android12l-lifecycle.json').read_text())
        self.write(self.config['historicalEvidence'], json.loads(
            (REPO / 'docs/android12l-deployment-evidence.json').read_text()))
        crash = self.root / self.config['historicalCrash']['path']
        crash.parent.mkdir(parents=True)
        shutil.copyfile(REPO / self.config['historicalCrash']['path'], crash)

    def write(self, name, value):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value) + '\n')
        return {'path': name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}

    def rejected(self, expected):
        with self.assertRaisesRegex(GateError, expected):
            check(self.root, self.config)

    def test_unresolved_binder_cleanup_blocks_without_any_out_directory(self):
        self.assertFalse((self.root / 'out').exists())
        self.config['binderCleanup']['resolved'] = False
        self.rejected('Binder cleanup remains unresolved.*recorded crash replay')

    def test_committed_workaround_passes_in_this_checkout(self):
        config = json.loads((REPO / 'ci/android12l-lifecycle.json').read_text())
        self.assertIsNone(config['fullLifecycle'])
        self.assertEqual(check(REPO, config), 'binder workaround ' + config['binderCleanup']['fixCommit'][:12])

    def test_clean_checkout_command_exits_one_even_under_python_optimization(self):
        self.config['binderCleanup']['resolved'] = False
        self.write('ci/android12l-lifecycle.json', self.config)
        (self.root / 'tools').mkdir()
        for name in ['check_android12l_lifecycle.py', 'test_api32_persistence_on_pi.py']:
            shutil.copyfile(REPO / 'tools' / name, self.root / 'tools' / name)
        for flags in [[], ['-O']]:
            result = subprocess.run([sys.executable, *flags, str(self.root / 'tools/check_android12l_lifecycle.py')],
                                    cwd=self.root, capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn('BinderProxy_destroy', result.stdout)
            self.assertIn('recorded crash replay: DEBUG:signal 0 (SIGSEGV)', result.stdout)

    def test_real_ce162_debug_excerpt_replays_native_fatal_detection(self):
        history = json.loads((self.root / self.config['historicalEvidence']).read_text())
        fatal = replay_historical_crash(self.root, self.config, history)
        self.assertEqual(fatal, 'DEBUG:signal 0 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr --------')

    def test_removing_real_crash_line_cannot_clear_the_replay(self):
        name = self.config['historicalCrash']['path']
        capture = json.loads((self.root / name).read_text())
        capture['nativeLog'] = '\n'.join(line for line in capture['nativeLog'].splitlines()
                                        if not line.startswith('DEBUG:signal ')) + '\n'
        capture['excerptSha256'] = hashlib.sha256(capture['nativeLog'].encode()).hexdigest()
        self.config['historicalCrash'] = self.write(name, capture)
        self.rejected('Recorded CE162 fatal must be detected')

    def test_resolved_checkbox_does_not_replace_fix_evidence(self):
        self.config['binderCleanup'].update(resolved=True, fixCommit=None, regression=None)
        self.rejected('fix commit reference')

    def test_historical_failure_cannot_be_relabelled(self):
        path = self.root / self.config['historicalEvidence']
        history = json.loads(path.read_text())
        history['failedFullLifecycle']['validation'] = 'PASS'
        self.write(self.config['historicalEvidence'], history)
        self.rejected('Historical CE162 failure')

    @staticmethod
    def ui():
        return {
            'physicalInput': True, 'cliCorrectPasswordUsed': False, 'passwordEntryVerified': True,
            'before': {'secure': True, 'deviceLocked': True, 'keyguardLocked': True},
            'after': {'secure': True, 'deviceLocked': False, 'keyguardLocked': False, 'unlockObserved': True},
            'uiBeforeEntry': {'interactive': True, 'ui': {'readOnly': True, 'passwordTextRead': False,
                'interactiveWindowsRequested': True, 'passwordEntry': {
                    'resourceId': 'com.android.systemui:id/passwordEntry', 'visible': True,
                    'enabled': True, 'focused': True, 'password': True, 'editable': True}}},
            'input': {'inputEventsRejected': 0, 'inputEventsAccepted': 4},
            'wakeAttempts': [{'state': {'interactive': True}}]}

    def cycle(self, run, index, native_extra=''):
        raw = {'runId': run, 'index': index, 'nativeLog':
               'Start proc 100:com.android.systemui/u0a1\nStarting session for id: emulated;0\n' + native_extra,
               'console': 'API32_INIT_SUBREAPER_READY kernel_child_ownership=1\n'
                          'INIT_CHILD service=pocket-framework pid=100\n'
                          'INIT_REAP service=pocket-framework pid=100 code=1 status=0\n'
                          'API32_INIT_STOPPED reaped=1 remaining=0 failed=0\n'
                          'API32_INIT_DESCENDANTS_STOPPED reaped=0 signalled=0 remaining=0 query_error=0\n'
                          'API32_EXTERNAL_VOLUME_MOUNT result=0 accepted=1\n',
               'keyguardObserverAliveAfterStop': False, 'display': {'frames_presented': 1},
               'session': {'ownerExitCode': 0, 'ownerAlive': False, 'input': {'inputEventsRejected': 0},
                   'externalStorage': {'attached': False, 'attaches': 1, 'revocations': 0, 'workerStopped': True}}}
        ref = self.write(f'evidence/ce-manager-cycle-{run}-{index}.json', raw)
        ref.update(index=index, check={'strictPassed': True})
        return ref

    def passing_fixture(self):
        """A minimal synthetic positive control, never a claim about a real guest."""
        run = 'a' * 32
        source = self.root / 'runtime/api32/binder_fix.cpp'
        source.parent.mkdir(parents=True)
        source.write_text('// Synthetic source identity for gate tests only.\n')
        self.report = {'runId': run, 'vmInstance': 73, 'validation': 'PASS', 'strictPassed': True,
                       'managerPackage': 'dev.kreijstal.aosppocket.api32managerimage',
                       'managerSha256': '1' * 64, 'imageSha256': '2' * 64,
                       'recoveredSourcePins': {'runtime/api32/binder_fix.cpp': hashlib.sha256(source.read_bytes()).hexdigest()},
                       'originalRetainedPackageCount': 143, 'requests': [],
                       'cycles': [self.cycle(run, i) for i in range(6)],
                       'finalAuthlessProof': {'verified': True, 'credentialChannelProvided': False,
                           'credentialsRequested': False, 'runId': 'b' * 32, 'rawCycle': self.cycle('b' * 32, 0)}}
        for name in ['fullPreservedCredentialLifecycleVerified', 'fullRecoveredRetainedCredentialLifecycleVerified',
                     'originalRetainedAppInventoryAcrossSixBootsVerified', 'mountedStorageAcrossCredentialLifecycleVerified',
                     'finalColdAuthlessReopenVerified', 'managerProcessRestartVerified', 'imageSourceOffline',
                     'testCredentialsRemovedAfterAuthlessRestoration']:
            self.report[name] = True
        for name in ['guestDataReset', 'ordinaryGuestAppReinstalled', 'historicalCredentialsRead',
                     'testCredentialsRetainedForRecovery']:
            self.report[name] = False
        for side in ['Before', 'After']:
            self.report['selinux' + side] = 'Enforcing'
            self.report['phantomLimit' + side] = self.report['phantomMonitoring' + side] = 'null'
        boots = [{'phase': i, 'readableCanariesVerified': 5, 'unlockedExternalCanaryVerified': True,
                  'lockedCanariesDenied': 5, 'wrongPasswordRejected': True, 'credentialUnlock': True}
                 for i in range(4)]
        boots[0]['passwordChanged'] = boots[1]['passwordRemoved'] = True
        boots[2].update(authlessBootUnlocked=True, passwordRestored=True)
        boots[3]['keyguardUi'] = self.ui()
        self.report['app'] = {'encryptedBoots': boots}
        self.report['outerRestart'] = {'keyguardUi': self.ui()}
        self.receipt = {'status': 'PASS_RECOVERED170_RETAINED143_SIX_CREDENTIAL_CYCLES_AND_SEVENTH_COLD_AUTHLESS',
                        'runId': run, 'managerSha256': self.report['managerSha256'],
                        'imageSha256': self.report['imageSha256'],
                        'cycles': [{'index': c['index'], 'sha256': c['sha256'], 'strictPassed': True}
                                   for c in self.report['cycles']],
                        'observerSamples': 2, 'sampledPeakChildren': 1}
        for name in ['twoPhysicalKeyguardUnlocksVerified', 'originalPasswordFreeStateRestored',
                     'newTestCredentialFileRemoved', 'sameOriginalAppKeyUidPath',
                     'fivePositiveRootCanariesAndPublicMediaEveryBoot', 'hostSettingsUnchanged']:
            self.receipt[name] = True
        budget = {'errors': [], 'peakChildren': 1, 'samples': [
            {'children': 1, 'rows': [['100', '1', '10157', self.report['managerPackage']],
                                    ['101', '100', '10157', 'pocket-app-process-api32']]},
            {'children': 0, 'rows': []}]}
        self.regression = {'validation': 'PASS', 'affectedProcess': 'android.process.acore',
                           'binderProxyCleanupObserved': True, 'nativeCrashes': [], 'nativeLog': '',
                           'managerSha256': self.report['managerSha256'], 'imageSha256': self.report['imageSha256'],
                           'withGuard': {'deadOwnerSequences': [{'events': [
                               '1.000 10 W BC_REQUEST_DEATH h=30 cookie=c1',
                               '1.001 11 R BR_DEAD_BINDER cookie=c1 h=30',
                               '1.002 11 W BC_CLEAR_DEATH h=30 cookie=c1',
                               '1.003 11 W BC_DEAD_BINDER_DONE cookie=c1',
                               '1.004 11 R BR_CLEAR_DEATH_DONE cookie=c1 h=30']}]}}
        self.config['binderCleanup']['resolved'] = True
        self.config['fullLifecycle'] = {'budget': self.write('evidence/process-budget.json', budget)}
        self.save_report()
        subprocess.run(['git', 'init', '--quiet', str(self.root)], check=True)
        subprocess.run(['git', '-C', str(self.root), 'add', 'runtime'], check=True)
        subprocess.run(['git', '-C', str(self.root), '-c', 'user.name=Gate test', '-c', 'user.email=gate@example.invalid',
                        '-c', 'commit.gpgsign=false', '-c', 'core.hooksPath=/dev/null',
                        'commit', '--quiet', '-m', 'Synthetic runtime fix reference'], check=True)
        self.config['binderCleanup']['fixCommit'] = subprocess.check_output(
            ['git', '-C', str(self.root), 'rev-parse', 'HEAD'], text=True).strip()
        self.save_regression()

    def save_regression(self):
        self.regression['fixCommit'] = self.config['binderCleanup']['fixCommit']
        self.config['binderCleanup']['regression'] = self.write('evidence/regression.json', self.regression)

    def save_report(self):
        full = self.config['fullLifecycle']
        full['report'] = self.write('evidence/ce-manager-report.json', self.report)
        self.receipt['reportSha256'] = full['report']['sha256']
        full['verification'] = self.write('evidence/independent-check.json', self.receipt)

    def test_workaround_passes_without_full_lifecycle(self):
        self.passing_fixture()
        self.config['fullLifecycle'] = None
        self.assertEqual(check(self.root, self.config), 'binder workaround ' + self.config['binderCleanup']['fixCommit'][:12])

    def test_duplicate_clear_done_is_rejected(self):
        self.passing_fixture()
        self.config['fullLifecycle'] = None
        events = self.regression['withGuard']['deadOwnerSequences'][0]['events']
        events.insert(3, '1.0025 11 R BR_CLEAR_DEATH_DONE cookie=c1 h=30')
        self.save_regression()
        self.rejected('exactly once')

    def test_regression_without_dead_owner_sequence_is_rejected(self):
        self.passing_fixture()
        self.config['fullLifecycle'] = None
        self.regression['withGuard']['deadOwnerSequences'] = []
        self.save_regression()
        self.rejected('No dead-owner')

    def test_regression_for_another_fix_commit_is_rejected(self):
        self.passing_fixture()
        self.regression['fixCommit'] = 'f' * 40
        self.config['binderCleanup']['regression'] = self.write('evidence/regression.json', self.regression)
        self.rejected('different fix commit')

    def test_complete_synthetic_evidence_can_pass_checker_only(self):
        self.passing_fixture()
        self.assertEqual(check(self.root, self.config), 'a' * 32)

    def test_relabelled_native_crash_still_fails_raw_inspection(self):
        self.passing_fixture()
        # Use the actual 37 DEBUG lines, not an invented crash string. The
        # surrounding positive-control lifecycle remains explicitly synthetic.
        capture = json.loads((self.root / self.config['historicalCrash']['path']).read_text())
        self.report['cycles'][2] = self.cycle('a' * 32, 2, capture['nativeLog'])
        self.receipt['cycles'][2]['sha256'] = self.report['cycles'][2]['sha256']
        self.save_report()
        self.rejected('native fatal')

    def test_partial_cycles_cannot_be_relabelled_pass(self):
        self.passing_fixture()
        self.report['cycles'] = self.report['cycles'][:2]
        self.save_report()
        self.rejected('Exactly six')

    def test_raw_capture_tampering_is_rejected(self):
        self.passing_fixture()
        (self.root / self.report['cycles'][0]['path']).write_text('{}')
        self.rejected('Raw cycle hash mismatch')

    def test_seventh_boot_cannot_request_a_password(self):
        self.passing_fixture()
        self.report['finalAuthlessProof']['credentialsRequested'] = True
        self.save_report()
        self.rejected('Seventh cold authless')

    def test_cli_unlock_cannot_replace_physical_ui(self):
        self.passing_fixture()
        self.report['outerRestart']['keyguardUi']['cliCorrectPasswordUsed'] = True
        self.save_report()
        self.rejected('Physical keyguard')

    def test_incomplete_credential_phases_are_rejected(self):
        self.passing_fixture()
        self.report['app']['encryptedBoots'][1]['passwordRemoved'] = False
        self.save_report()
        self.rejected('Password change/remove')

    def test_stale_runtime_sources_are_rejected(self):
        self.passing_fixture()
        (self.root / 'runtime/api32/binder_fix.cpp').write_text('// Changed after the full run.\n')
        self.rejected('stale/different sources')

    def test_unobserved_acore_cleanup_is_rejected(self):
        self.passing_fixture()
        self.regression['binderProxyCleanupObserved'] = False
        self.save_regression()
        self.rejected('Affected-process Binder cleanup')

    def test_nonexistent_fix_commit_is_rejected(self):
        self.passing_fixture()
        self.config['binderCleanup']['fixCommit'] = 'f' * 40
        self.rejected('checkout history')

    def test_process_budget_cannot_exceed_default32(self):
        self.passing_fixture()
        rows = [[str(i), '1', '10157', 'helper'] for i in range(33)]
        ref = self.write('evidence/process-budget.json', {'errors': [], 'peakChildren': 33,
            'samples': [{'children': 33, 'rows': rows}, {'children': 0, 'rows': []}]})
        self.config['fullLifecycle']['budget'] = ref
        self.rejected('Default32')

    def test_seventh_raw_capture_is_required(self):
        self.passing_fixture()
        (self.root / self.report['finalAuthlessProof']['rawCycle']['path']).unlink()
        self.rejected('Missing raw cycle')

    def test_independent_receipt_must_match_the_report(self):
        self.passing_fixture()
        self.receipt['reportSha256'] = 'f' * 64
        self.config['fullLifecycle']['verification'] = self.write('evidence/independent-check.json', self.receipt)
        self.rejected('does not match the raw report')

    def test_evidence_path_cannot_escape_checkout(self):
        self.passing_fixture()
        self.config['fullLifecycle']['report']['path'] = '../outside.json'
        self.rejected('escapes repository')

    def test_binder_regression_must_match_candidate(self):
        self.passing_fixture()
        self.regression['imageSha256'] = 'f' * 64
        self.save_regression()
        self.rejected('different candidate')


if __name__ == '__main__':
    unittest.main()
