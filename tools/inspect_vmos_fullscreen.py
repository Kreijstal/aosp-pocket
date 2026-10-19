#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check VMOS full-screen interaction paths in DEX without running the APK."""
import argparse
import json
from pathlib import Path
import re
import zipfile

TARGETS = [
    ('com/vmos/virtual/ui/fragments/VmFragment', 'H', ['VMOSStateInfo;->isRunning', '->S()']),
    ('com/vmos/virtual/ui/fragments/VmFragment', 'S', ['VmRenderActivity;', 'addFlags', 'startActivity']),
    ('com/vmos/virtual/ui/activities/VmRenderActivity', 'G', ['getWidth', 'getHeight', 'setVisibility']),
    ('com/vmos/virtual/ui/activities/VmRenderActivity', 'z', ['setSystemUiVisibility', '5894']),
    ('com/vmos/virtual/ui/activities/VmRenderActivity', 'onBackPressed', ['->i()', '->m(I)', '->F()']),
    ('com/vmos/virtual/ui/activities/VmRenderActivity', 'onUserLeaveHint', ['Landroid/util/Rational;', 'activities/f;->a(', 'activities/h;->a(']),
    ('com/vmos/virtual/ui/activities/f', 'a', ['setAspectRatio']),
    ('com/vmos/virtual/ui/activities/h', 'a', ['enterPictureInPictureMode']),
    ('com/vmos/virtual/ui/activities/VmRenderActivity', 'onWindowFocusChanged', ['->z()']),
    ('com/vmos/virtual/ui/activities/VmRenderActivity$m$a', 'invokeSuspend', ['createCircularReveal', 'setDuration', '300']),
]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('apk',type=Path)
    parser.add_argument('output',type=Path)
    args=parser.parse_args()
    from loguru import logger
    logger.remove()
    from androguard.core.dex import DEX
    wanted={('L'+cls+';',method):needles for cls,method,needles in TARGETS}
    evidence={};listing=[]
    with zipfile.ZipFile(args.apk) as archive:
        for name in archive.namelist():
            if not re.fullmatch(r'classes\d*\.dex',name):continue
            for cls in DEX(archive.read(name)).get_classes():
                for method in cls.get_methods():
                    key=(cls.get_name(),method.get_name())
                    if key not in wanted:continue
                    instructions=[i.get_name()+' '+i.get_output() for i in method.get_instructions()]
                    text='\n'.join(instructions);label='->'.join(key)
                    evidence[label]={'dex':name,'descriptor':method.get_descriptor(),
                                     'checks':{needle:needle in text for needle in wanted[key]}}
                    listing.append(label+' '+method.get_descriptor()+'\n'+text)
    missing=['->'.join(key) for key in wanted if '->'.join(key) not in evidence]
    failed={key:method['checks'] for key,method in evidence.items() if not all(method['checks'].values())}
    args.output.mkdir(parents=True,exist_ok=True)
    (args.output/'fullscreen-dex.txt').write_text('\n\n'.join(listing)+'\n')
    (args.output/'fullscreen-checks.json').write_text(json.dumps({
        'classification':'DEX derived; static only','executed':False,'methods':evidence,
        'missing':missing,'failed':failed},indent=2)+'\n')
    assert not missing and not failed,(missing,failed)
    print(f'PASS: {len(evidence)} full-screen methods verified directly in DEX; target not executed')

if __name__=='__main__':main()
