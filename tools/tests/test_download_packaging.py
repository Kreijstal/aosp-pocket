import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

TOOLS=Path(__file__).resolve().parents[1]


def load(name):
    sys.path.insert(0,str(TOOLS))
    spec=importlib.util.spec_from_file_location(name,TOOLS/(name+'.py'))
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module


class DownloadPackagingTest(unittest.TestCase):
    def test_manager_default_keeps_runtime_and_removes_every_image(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);manager=root/'manager';manager.mkdir();(root/'out/runtime').mkdir(parents=True)
            with zipfile.ZipFile(root/'out/runtime/packed.apk','w') as z:
                for name,data in {'classes.dex':b'old','AndroidManifest.xml':b'old',
                                  'assets/default-guest/a.payload':b'old image',
                                  'assets/api28-rootfs/system/build.prop.payload':b'old image',
                                  'lib/arm64-v8a/runtime.so':b'keep native','assets/licenses/NOTICE':b'keep notice'}.items():z.writestr(name,data)
            with zipfile.ZipFile(manager/'base.apk','w') as z:
                z.writestr('classes.dex',b'new host');z.writestr('AndroidManifest.xml',b'new manifest')
            for path,data in {'out/api28/reference/system/etc/NOTICE.xml.gz':b'android notice',
                              'out/boot/hooks.json':b'{}','tools/pocketctl.py':b'console'}.items():
                target=root/path;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
            catalog=root/'catalog.json';catalog.write_text('{"schema":1,"images":[]}')
            module=load('pack_manager');module.ROOT=root
            with patch.object(sys,'argv',['pack_manager','--out',str(manager),'--image-catalog',str(catalog)]):module.main()
            with zipfile.ZipFile(manager/'packed.apk') as z:
                self.assertFalse(any(n.startswith(('assets/default-guest/','assets/api28-rootfs/')) for n in z.namelist()))
                self.assertEqual(z.read('lib/arm64-v8a/runtime.so'),b'keep native')
                self.assertEqual(z.read('classes.dex'),b'new host')
                self.assertEqual(z.read('assets/pocket-images.json'),catalog.read_bytes())

    def test_runtime_default_does_not_require_or_embed_guest_zip(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);(root/'out/runtime').mkdir(parents=True)
            with zipfile.ZipFile(root/'out/runtime/base.apk','w') as z:z.writestr('classes.dex',b'host')
            native=root/'out/runtime-native';native.mkdir()
            for name in ('libpocket_runtime.so','libproot.so','libpocket_frames.so','libpocket_ce.so'):(native/name).write_bytes(b'native')
            for name in ('runtime/guest.sh','out/runtime-native/libpocket_binder_bootstrap.so','sources.lock.json',
                         'runtime/licenses/GPL-3.0-only.txt','LICENSE','runtime/NOTICE','vendor/proot/COPYING',
                         'runtime/licenses/talloc.txt','out/ananbox-native/boost_1_83_0/LICENSE_1_0.txt'):
                path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(b'metadata')
            module=load('pack_runtime');module.ROOT=root
            with patch.object(sys,'argv',['pack_runtime','--ndk',str(root/'ndk')]),patch.object(module.subprocess,'run'):
                module.main()
            with zipfile.ZipFile(root/'out/runtime/packed.apk') as z:
                self.assertFalse(any(n.startswith('assets/default-guest/') for n in z.namelist()))
                self.assertEqual(z.read('assets/guest-binder-bootstrap.so'),b'metadata')
                self.assertEqual(z.read('lib/arm64-v8a/libpocket_ce.so'),b'native')


if __name__=='__main__':unittest.main()
