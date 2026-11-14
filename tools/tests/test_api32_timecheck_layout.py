# SPDX-License-Identifier: GPL-3.0-only
"""The audio watchdog adapter must not affect another guest service."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from build_api32_timecheck import configure_audio_service


BASE='/system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_hidl_api32.so'
LIB='/system/lib64/libpocket_timecheck_api32.so'
AUDIO=f'''service pocket-audio /system/bin/audioserver
    user 1041
    group 1041
    setenv LD_PRELOAD {BASE}
'''
OTHER=f'''service unrelated /system/bin/other
    setenv LD_PRELOAD {BASE}
'''


class AudioTimecheckLayoutTest(unittest.TestCase):
    def test_only_audio_changes_and_reapplication_is_identical(self):
        config=OTHER+'\n'+AUDIO+'\non boot\n    start unrelated\n'
        selected=configure_audio_service(config)
        self.assertEqual(selected,OTHER+'\n'+AUDIO.replace(BASE,BASE+':'+LIB)+'\non boot\n    start unrelated\n')
        self.assertEqual(configure_audio_service(selected),selected)

    def test_adapter_in_another_service_is_rejected(self):
        with self.assertRaises(ValueError):
            configure_audio_service(AUDIO+'\n'+OTHER.replace(BASE,BASE+':'+LIB))

    def test_missing_duplicate_or_wrong_audio_command_is_rejected(self):
        for config in [OTHER,AUDIO+AUDIO,AUDIO.replace('/system/bin/audioserver','/system/bin/other')]:
            with self.subTest(config=config),self.assertRaises(ValueError):
                configure_audio_service(config)

    def test_missing_duplicate_or_unknown_preload_is_rejected(self):
        for config in [AUDIO.replace('    setenv LD_PRELOAD '+BASE+'\n',''),
                AUDIO+'    setenv LD_PRELOAD '+BASE+'\n',AUDIO.replace(BASE,BASE+':/unknown.so')]:
            with self.subTest(config=config),self.assertRaises(ValueError):
                configure_audio_service(config)

    def test_line_endings_and_final_line_are_preserved(self):
        for config in [AUDIO.rstrip('\n'),AUDIO.replace('\n','\r\n')]:
            with self.subTest(config=config):
                self.assertEqual(configure_audio_service(config),config.replace(BASE,BASE+':'+LIB))


if __name__=='__main__':unittest.main()
