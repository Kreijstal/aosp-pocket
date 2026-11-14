# SPDX-License-Identifier: GPL-3.0-only
"""A rebuilt catalog can reuse every payload without rewriting guest files."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from test_api32_app_on_pi import image_reuse_verified


IMAGE={'sha256':'image','unpacked_bytes':100,'manifest':{'entries':{'one':{},'two':{}}}}
REUSED={'imageSha256':'image','sdk':32,'reusedFiles':2,'replacedFiles':0,'reusedBytes':100,'writtenBytes':0}


class ImageReuseTest(unittest.TestCase):
    def test_all_payloads_reused(self):
        self.assertTrue(image_reuse_verified(REUSED,IMAGE))

    def test_partial_reuse(self):
        self.assertTrue(image_reuse_verified(dict(REUSED,reusedFiles=1,replacedFiles=1,reusedBytes=70,writtenBytes=30),IMAGE))

    def test_negative_or_noninteger_counters(self):
        for key in ['reusedFiles','replacedFiles','reusedBytes','writtenBytes']:
            for value in [-1,True,1.0,None]:
                with self.subTest(key=key,value=value):
                    self.assertFalse(image_reuse_verified(dict(REUSED,**{key:value}),IMAGE))

    def test_incomplete_inconsistent_or_fresh_copy_is_rejected(self):
        for changes in [{'reusedFiles':1},{'writtenBytes':1},{'reusedBytes':99,'writtenBytes':1},
                {'reusedFiles':0,'replacedFiles':2,'reusedBytes':0,'writtenBytes':100},
                {'imageSha256':'wrong'},{'sdk':29}]:
            with self.subTest(changes=changes):
                self.assertFalse(image_reuse_verified(dict(REUSED,**changes),IMAGE))


if __name__=='__main__':unittest.main()
