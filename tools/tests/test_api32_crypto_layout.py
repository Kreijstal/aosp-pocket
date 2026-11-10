# SPDX-License-Identifier: GPL-3.0-only
"""The shared credential host must never start alongside standalone gatekeeperd."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from api32_runtime_boot import shared_gatekeeper_frontend


HIDL='/system/lib64/libpocket_hidl_api32.so'
DUAL='/system/lib64/libpocket_binder_api32.so:'+HIDL
BINARY=b'ELF-placeholder\0POCKET_GATEKEEPER_FRONTEND_READY pid=%d'
CONFIG=f'''on boot
    start something-else

service pocket-keymaster /system/bin/pocket-crypto-api32
    disabled
    user 1000
    group 1000
    setenv LD_PRELOAD {HIDL}

service gatekeeperd /system/bin/gatekeeperd /data/misc/gatekeeper
    disabled
    user 1000
    group 1000
    setenv LD_PRELOAD {DUAL}

service unrelated /system/bin/other
    user 1017
'''
BOOT='''    start_service pocket-keymaster
    start_service pocket-keystore
    wait_service android.system.keystore2.IKeystoreService/default
    start_service gatekeeperd
    start_service unrelated
'''


class SharedCredentialStartup(unittest.TestCase):
    def test_only_frontend_service_and_readiness_change(self):
        config,boot=shared_gatekeeper_frontend(CONFIG,BOOT,BINARY)
        self.assertNotIn('service gatekeeperd ',config)
        self.assertIn('    setenv LD_PRELOAD '+DUAL+'\n',config)
        self.assertIn('service unrelated /system/bin/other\n    user 1017\n',config)
        self.assertEqual(boot,BOOT.replace('    start_service gatekeeperd\n',
                                         '    await_marker POCKET_GATEKEEPER_FRONTEND_READY\n'))
        self.assertEqual(shared_gatekeeper_frontend(config,boot,BINARY),(config,boot))

    def test_unpaired_binary_or_boot_is_rejected(self):
        for binary in [b'',b'POCKET_CRYPTO_READY components=2']:
            with self.subTest(binary=binary),self.assertRaises(ValueError):
                shared_gatekeeper_frontend(CONFIG,BOOT,binary)
        for boot in [BOOT.replace('    start_service gatekeeperd\n',''),
                     BOOT+'    start_service gatekeeperd\n',
                     BOOT+'    await_marker POCKET_GATEKEEPER_FRONTEND_READY\n',
                     BOOT.replace('    start_service pocket-keymaster\n','')]:
            with self.subTest(boot=boot),self.assertRaises(ValueError):
                shared_gatekeeper_frontend(CONFIG,boot,BINARY)

    def test_identity_preload_and_duplicate_services_are_rejected(self):
        for config in [CONFIG.replace('user 1000','user 0',1),
                       CONFIG.replace('group 1000','group 1000 1026',1),
                       CONFIG.replace('setenv LD_PRELOAD '+HIDL+'\n','setenv LD_PRELOAD '+DUAL+'\n',1),
                       CONFIG.replace('setenv LD_PRELOAD '+DUAL,'setenv LD_PRELOAD '+HIDL),
                       CONFIG.replace('/data/misc/gatekeeper','/tmp/other'),
                       CONFIG+'\n'+CONFIG,
                       CONFIG.replace('pocket-crypto-api32','pocket-keymaster-api32')]:
            with self.subTest(config=config),self.assertRaises(ValueError):
                shared_gatekeeper_frontend(config,BOOT,BINARY)


if __name__=='__main__':
    unittest.main()
