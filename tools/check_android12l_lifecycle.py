#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Read-only CI evidence gate; CE162's recorded failure must remain a failure.

This does not run the Pi VM or qualify a release. A replacement full run must
include its independent verification receipt, hash-bound raw captures and budget,
plus an affected-process Binder cleanup regression and reviewed fix reference.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from test_api32_persistence_on_pi import inspect_cycle, native_fatals

ROOT = Path(__file__).resolve().parents[1]
CE162_RUN = '182e3ad9cec0419cb30677049f69a95e'
CE162_REPORT = '4fb9487164bd360e85da9f2f11057dd5e72b6efa1649f2d422cd00da2f9da948'


class GateError(ValueError):
    pass


def require(value, message):
    if not value:
        raise GateError(message)


def object_pairs(pairs):
    result = {}
    for name, value in pairs:
        require(name not in result, 'Duplicate JSON key: ' + name)
        result[name] = value
    return result


def read_json(path):
    require(path.stat().st_size <= 16 * 1024 * 1024, 'Evidence exceeds 16 MiB: ' + path.name)
    value = json.loads(path.read_text(), object_pairs_hook=object_pairs)
    require(isinstance(value, dict), 'Expected an evidence object: ' + path.name)
    return value


def within(root, name):
    require(isinstance(name, str) and name and not Path(name).is_absolute(),
            'Evidence path must be relative to the repository')
    path = root / name
    require('..' not in Path(name).parts and path.resolve().is_relative_to(root.resolve()),
            'Evidence path escapes repository')
    require(path.is_file() and not path.is_symlink(), 'Missing regular evidence file: ' + name)
    return path


def checked(root, reference):
    require(isinstance(reference, dict), 'Missing hash-bound evidence reference')
    path = within(root, reference['path'])
    pin = reference['sha256']
    require(isinstance(pin, str) and re.fullmatch(r'[a-f0-9]{64}', pin), 'Invalid evidence SHA-256')
    require(hashlib.sha256(path.read_bytes()).hexdigest() == pin, 'Evidence hash mismatch: ' + path.name)
    return path, read_json(path)


def strict_cycle(report_path, reference, run_id, index):
    # Pi reports retain their original absolute paths. Transferred captures
    # must be colocated with the report, with the exact nonce/index filename.
    name = f'ce-manager-cycle-{run_id}-{index}.json'
    require(Path(reference['path']).name == name, 'Wrong cycle filename/run/index')
    path = report_path.parent / name
    require(path.is_file() and not path.is_symlink(), 'Missing raw cycle: ' + name)
    require(hashlib.sha256(path.read_bytes()).hexdigest() == reference['sha256'], 'Raw cycle hash mismatch')
    cycle = read_json(path)
    require(cycle['runId'] == run_id and cycle['index'] == index, 'Raw cycle identity mismatch')
    result = inspect_cycle(cycle, require_kernel_ownership=True, require_external_volume=True)
    require(result['strictPassed'], 'Raw cycle failed strict validation: ' + name +
            (' (native fatal)' if result['nativeFatals'] else ''))
    require(cycle.get('keyguardObserverAliveAfterStop') is False, 'Warm observer not reaped')
    started = re.findall(r'^INIT_CHILD service=(\S+) pid=(\d+)$', cycle['console'], re.M)
    reaped = re.findall(r'^INIT_REAP service=(\S+) pid=(\d+) ', cycle['console'], re.M)
    require(bool(started) and sorted(started) == sorted(reaped), 'Service lifetime accounting mismatch')


def physical_unlock(value):
    require(value.get('physicalInput') is True and value.get('cliCorrectPasswordUsed') is False and
            value.get('passwordEntryVerified') is True, 'Physical keyguard input proof missing')
    require(all(value['before'].get(k) is True for k in ['secure', 'deviceLocked', 'keyguardLocked']),
            'Keyguard was not locked before UI input')
    after = value['after']
    require(after.get('secure') is True and after.get('deviceLocked') is False and
            after.get('keyguardLocked') is False and after.get('unlockObserved') is True,
            'UI unlock was not observed')
    ready = value['uiBeforeEntry']
    ui = ready['ui']
    entry = ui['passwordEntry']
    require(ready.get('interactive') is True and ui.get('readOnly') is True and
            ui.get('passwordTextRead') is False and ui.get('interactiveWindowsRequested') is True,
            'Keyguard UI observation missing')
    require(entry['resourceId'] == 'com.android.systemui:id/passwordEntry' and
            all(entry.get(k) is True for k in ['visible', 'enabled', 'focused', 'password', 'editable']),
            'Password field focus proof missing')
    require(value['input']['inputEventsRejected'] == 0 and value['input']['inputEventsAccepted'] > 0 and
            value['wakeAttempts'][-1]['state']['interactive'] is True, 'Host keyguard input/wake failed')


def replay_historical_crash(root, config, history):
    _, capture = checked(root, config['historicalCrash'])
    failure = history['failedFullLifecycle']
    source = Path(failure['reportPath']).parent / f'ce-manager-cycle-{CE162_RUN}-2.json'
    require(capture.get('schema') == 1 and capture.get('kind') == 'recorded-native-log-excerpt',
            'Wrong historical crash fixture format')
    require(capture['runId'] == CE162_RUN and capture['cycleIndex'] == 2 and
            capture['sourceReportSha256'] == CE162_REPORT and
            capture['sourceCycleSha256'] == history['files'][source.as_posix()]['sha256'],
            'Historical crash fixture does not match CE162 provenance')
    log = capture['nativeLog']
    require(hashlib.sha256(log.encode()).hexdigest() == capture['excerptSha256'],
            'Historical crash excerpt hash mismatch')
    require('DEBUG:Cmdline: android.process.acore' in log and 'DEBUG:uid: 10036' in log and
            'BinderProxy_destroy' in log and 'RefBase::decStrong' in log,
            'Historical Binder crash identity is missing')
    fatals = native_fatals(log)
    require(len(fatals) == 1 and '(SIGSEGV)' in fatals[0], 'Recorded CE162 fatal must be detected')
    return fatals[0]


def check_full(root, evidence):
    report_path, report = checked(root, evidence['report'])
    _, receipt = checked(root, evidence['verification'])
    run = report['runId']
    require(re.fullmatch(r'[a-f0-9]{32}', run) and run != CE162_RUN, 'A new full-run ID is required')
    require(report.get('validation') == 'PASS' and report.get('strictPassed') is True,
            'Full lifecycle did not PASS')
    require(report.get('managerPackage') == 'dev.kreijstal.aosppocket.api32managerimage' and
            report.get('vmInstance') == 73, 'Wrong retained-profile package or VM')
    require(receipt['runId'] == run and receipt['reportSha256'] == evidence['report']['sha256'],
            'Independent verification does not match the raw report')
    require(re.fullmatch(r'PASS_RECOVERED\d+_RETAINED143_SIX_CREDENTIAL_CYCLES_AND_SEVENTH_COLD_AUTHLESS',
                         receipt.get('status', '')), 'Not a complete independent lifecycle verification')
    for name in ['managerSha256', 'imageSha256']:
        require(receipt[name] == report[name] and re.fullmatch(r'[a-f0-9]{64}', report[name]),
                'Candidate artifact identity mismatch: ' + name)
    pins = report['recoveredSourcePins']
    require(isinstance(pins, dict) and pins, 'Missing compiled source pins')
    extensions = {'.java', '.c', '.cpp', '.h', '.S', '.patch', '.xml', '.json', '.prop', '.sh'}
    required_sources = {p.relative_to(root).as_posix() for p in (root / 'runtime').rglob('*')
                        if p.is_file() and p.suffix in extensions}
    require(required_sources and required_sources <= pins.keys(), 'Runtime source coverage is incomplete')
    for name, pin in pins.items():
        require(hashlib.sha256(within(root, name).read_bytes()).hexdigest() == pin,
                'Full run is from stale/different sources: ' + name)
    for name in ['fullPreservedCredentialLifecycleVerified', 'fullRecoveredRetainedCredentialLifecycleVerified',
                 'originalRetainedAppInventoryAcrossSixBootsVerified', 'mountedStorageAcrossCredentialLifecycleVerified',
                 'finalColdAuthlessReopenVerified', 'managerProcessRestartVerified', 'imageSourceOffline',
                 'testCredentialsRemovedAfterAuthlessRestoration']:
        require(report.get(name) is True, 'Incomplete full lifecycle: ' + name)
    for name in ['guestDataReset', 'ordinaryGuestAppReinstalled', 'historicalCredentialsRead',
                 'testCredentialsRetainedForRecovery']:
        require(report.get(name) is False, 'Unsafe or missing preservation result: ' + name)
    require(report['originalRetainedPackageCount'] == 143 and report['requests'] == [],
            'Original inventory/offline image proof missing')
    for name in ['twoPhysicalKeyguardUnlocksVerified', 'originalPasswordFreeStateRestored',
                 'newTestCredentialFileRemoved', 'sameOriginalAppKeyUidPath',
                 'fivePositiveRootCanariesAndPublicMediaEveryBoot', 'hostSettingsUnchanged']:
        require(receipt.get(name) is True, 'Independent verification is incomplete: ' + name)
    cycles = report['cycles']
    require([c['index'] for c in cycles] == list(range(6)), 'Exactly six strict cycles are required')
    require([c['index'] for c in receipt['cycles']] == list(range(6)) and
            all(c.get('strictPassed') is True for c in receipt['cycles']), 'Independent cycle checks incomplete')
    require(all(a['sha256'] == b['sha256'] for a, b in zip(cycles, receipt['cycles'])),
            'Independent cycle hashes do not match the raw report')
    for cycle in cycles:
        strict_cycle(report_path, cycle, run, cycle['index'])
    boots = report['app']['encryptedBoots']
    require([b['phase'] for b in boots] == list(range(4)), 'Four encrypted phases are required')
    for boot in boots:
        require(boot.get('readableCanariesVerified') == 5 and boot.get('unlockedExternalCanaryVerified') is True,
                'Encrypted storage canary verification missing')
        if boot['phase'] != 2:
            require(boot.get('lockedCanariesDenied') == 5 and boot.get('wrongPasswordRejected') is True and
                    boot.get('credentialUnlock') is True, 'Wrong-password/locked-storage checks missing')
    require(boots[0].get('passwordChanged') is True and boots[1].get('passwordRemoved') is True and
            boots[2].get('authlessBootUnlocked') is True and boots[2].get('passwordRestored') is True,
            'Password change/remove/authless/restore checks incomplete')
    physical_unlock(boots[3]['keyguardUi'])
    physical_unlock(report['outerRestart']['keyguardUi'])
    final = report['finalAuthlessProof']
    require(final.get('verified') is True and final.get('credentialChannelProvided') is False and
            final.get('credentialsRequested') is False, 'Seventh cold authless proof is incomplete')
    require(re.fullmatch(r'[a-f0-9]{32}', final['runId']) and final['runId'] != run,
            'Seventh boot must have a fresh run ID')
    strict_cycle(report_path, final['rawCycle'], final['runId'], 0)
    require(final['rawCycle']['check']['strictPassed'] is True, 'Seventh cold cycle was not verified')
    _, budget = checked(root, evidence['budget'])
    require(budget['samples'] and not budget['errors'], 'Process accounting missing or failed')
    for sample in budget['samples']:
        require(sample['children'] == sum(row[3] != report['managerPackage'] for row in sample['rows']),
                'Actual helper/child process accounting mismatch')
    require(budget['peakChildren'] == max(s['children'] for s in budget['samples']) <= 32 and
            budget['samples'][-1]['children'] == 0, 'Default32 or stopped-process gate failed')
    require(receipt['observerSamples'] == len(budget['samples']) and
            receipt['sampledPeakChildren'] == budget['peakChildren'], 'Budget receipt mismatch')
    for side in ['Before', 'After']:
        require(report['selinux' + side] == 'Enforcing' and report['phantomLimit' + side] == 'null' and
                report['phantomMonitoring' + side] == 'null', 'Host policy changed')
    return report


def check(root, config):
    require(config.get('schema') == 1, 'Unsupported lifecycle gate schema')
    history = read_json(within(root, config['historicalEvidence']))
    failure = history['failedFullLifecycle']
    require(failure['runId'] == CE162_RUN and failure['reportSha256'] == CE162_REPORT and
            failure['validation'] == 'FAIL', 'Historical CE162 failure was removed or relabelled')
    require(history['files'][failure['reportPath']]['sha256'] == CE162_REPORT,
            'Historical raw-report pin was changed')
    require(failure['strictCycleIndices'] == [0, 1] and failure['failedCapturedCycleIndex'] == 2,
            'Historical CE162 cycle failure was changed')
    recorded_fatal = replay_historical_crash(root, config, history)
    blockers = []
    binder = config['binderCleanup']
    if binder.get('resolved') is not True:
        blockers.append('Binder cleanup remains unresolved: android.process.acore / '
                        'ReferenceQueueD / RefBase::decStrong / BinderProxy_destroy')
    if not config.get('fullLifecycle'):
        blockers.append('CE162 ' + CE162_RUN + ' is FAIL: 2/6 strict cycles; captured cycle 2 '
                        'contains SIGSEGV; offline outer/UI and seventh-cold lifecycle incomplete; '
                        'recorded crash replay: ' + recorded_fatal)
    require(not blockers, '; '.join(blockers))
    require(isinstance(binder.get('fixCommit'), str) and re.fullmatch(r'[a-f0-9]{40}', binder['fixCommit']),
            'Binder fix commit reference is required')
    fix = binder['fixCommit']
    exists = subprocess.run(['git', '-C', str(root), 'cat-file', '-e', fix + '^{commit}'],
                            capture_output=True)
    ancestor = subprocess.run(['git', '-C', str(root), 'merge-base', '--is-ancestor', fix, 'HEAD'],
                              capture_output=True)
    require(exists.returncode == ancestor.returncode == 0, 'Binder fix commit is not in this checkout history')
    changed = subprocess.check_output(['git', '-C', str(root), 'diff-tree', '--root', '--no-commit-id',
                                       '--name-only', '-r', fix], text=True).splitlines()
    require(any(p.startswith('runtime/') and not p.startswith(('runtime/tests/', 'runtime/tools/'))
                for p in changed), 'Binder fix commit does not change runtime code')
    report = check_full(root, config['fullLifecycle'])
    _, regression = checked(root, binder['regression'])
    require(regression.get('validation') == 'PASS' and regression.get('affectedProcess') == 'android.process.acore'
            and regression.get('binderProxyCleanupObserved') is True,
            'Affected-process Binder cleanup regression has not passed')
    require(regression.get('nativeCrashes') == [] and not native_fatals(regression['nativeLog']),
            'Binder cleanup regression contains a native crash')
    for name in ['imageSha256', 'managerSha256']:
        require(regression[name] == report[name], 'Binder regression is from a different candidate')
    return report['runId']


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=Path('ci/android12l-lifecycle.json'))
    args = parser.parse_args(argv)
    try:
        run = check(ROOT, read_json(within(ROOT, args.config.as_posix())))
    except (GateError, OSError, ValueError, KeyError, TypeError, IndexError, subprocess.CalledProcessError) as error:
        print('ANDROID12L_LIFECYCLE_GATE_FAIL: ' + str(error))
        return 1
    print('ANDROID12L_LIFECYCLE_GATE_PASS: ' + run + '; evidence gate only, not release qualification')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
