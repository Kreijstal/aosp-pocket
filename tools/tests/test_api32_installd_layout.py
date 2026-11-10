# SPDX-License-Identifier: GPL-3.0-only
"""Reject mixed binaries, startup sequences and service identities."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from api32_runtime_boot import shared_installd_frontend

BINDER='/system/lib64/libpocket_binder_api32.so'
HIDL='/system/lib64/libpocket_hidl_api32.so'
POLICY='/system/lib64/libpocket_policy_api32.so'
BINARY=b'ELF-placeholder\0POCKET_INSTALLD_FRONTEND_READY pid=%d'
FLAG='    setenv POCKET_API32_INSTALLD_IN_CORE 1\n'
CONFIG=f'''on boot
    start something-else

service pocket-registry /system/bin/pocket-core-api32
    disabled
    user 0
    group 0
    setenv LD_PRELOAD {BINDER}:{HIDL}
    socket lmkd seqpacket 0660 1000 1000
    setenv POCKET_API32_METRICS_IN_CORE 1

service pocket-installd /system/bin/installd
    disabled
    user 0
    group 0
    setenv LD_PRELOAD {BINDER}:{POLICY}

service pocket-keymaster /system/bin/pocket-crypto-api32
    user 1000
    group 1000
'''
BOOT='''    start_service pocket-registry
    await_marker POCKET_SERVICEMANAGER_READY
    await_marker POCKET_CORE_METRICS_READY
    start_service pocket-installd
    wait_service installd
    start_service pocket-keymaster
'''


class SharedInstalldStartup(unittest.TestCase):
    def test_complete_selection_is_idempotent_and_retains_typed_wait(self):
        config,boot=shared_installd_frontend(CONFIG,BOOT,BINARY)
        self.assertNotIn('service pocket-installd ',config)
        self.assertEqual(config.count(FLAG),1)
        self.assertIn('    socket lmkd seqpacket 0660 1000 1000\n',config)
        self.assertIn('    setenv POCKET_API32_METRICS_IN_CORE 1\n',config)
        self.assertTrue(config.endswith(
            'service pocket-keymaster /system/bin/pocket-crypto-api32\n    user 1000\n    group 1000\n'))
        self.assertEqual(boot,BOOT.replace('    start_service pocket-installd\n',
            '    await_marker POCKET_INSTALLD_FRONTEND_READY\n'))
        self.assertEqual(shared_installd_frontend(config,boot,BINARY),(config,boot))

    def test_unpaired_startup_and_binary_are_rejected(self):
        config,boot=shared_installd_frontend(CONFIG,BOOT,BINARY)
        for c,b,binary in [(CONFIG,BOOT,b''),(config,BOOT,BINARY),(CONFIG,boot,BINARY),
                           (config,boot,b'POCKET_CORE_START components=9'),
                           (config.replace(FLAG,''),boot,BINARY),
                           (config+FLAG,boot,BINARY),
                           (config.replace(FLAG,FLAG+FLAG),boot,BINARY),
                           (config,boot.replace('    wait_service installd\n',''),BINARY),
                           (CONFIG,BOOT.replace('    wait_service installd\n','')+
                            '    wait_service installd\n    start_service pocket-installd\n',BINARY)]:
            with self.subTest(config=c,boot=b),self.assertRaises(ValueError):
                shared_installd_frontend(c,b,binary)

    def test_identity_preload_command_and_duplicate_guards(self):
        for changed in [
                CONFIG.replace('user 0','user 1000',1),
                CONFIG.replace('group 0','group 0 1000'),
                CONFIG.replace(BINDER+':'+HIDL,HIDL),
                CONFIG.replace(BINDER+':'+POLICY,BINDER),
                CONFIG.replace('/system/bin/installd','/system/bin/other'),
                CONFIG.replace('/system/bin/pocket-core-api32','/system/bin/pocket-core-test-api32'),
                CONFIG+CONFIG,
                CONFIG.replace('    disabled\n',FLAG+'    disabled\n',1),
                CONFIG.replace('service pocket-keymaster',FLAG+'service pocket-keymaster'),
        ]:
            with self.subTest(config=changed),self.assertRaises(ValueError):
                shared_installd_frontend(changed,BOOT,BINARY)


if __name__=='__main__':unittest.main()
