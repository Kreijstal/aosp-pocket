#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify the installed-app import path in VMOS DEX without executing it.

Requires androguard in the existing isolated research environment. Writes
instruction evidence under ignored out/, never into the runtime APK.
"""
import argparse
import json
from pathlib import Path
import re
import zipfile

TARGETS = [
    ('com/vmos/virtual/ui/fragments/ImportDeviceAppFragment', 'p', ['getInstalledPackages', 'sourceDir', 'splitNames', 'getParent']),
    ('com/vmos/virtual/ui/fragments/InstallAppFragment', 'v', ['getSourcePath', 'InstallQueueInfo']),
    ('com/vmos/virtual/manager/f', 'i', ['->X(']),
    ('com/vmos/sdk/p', 'Y', ['->Z(']),
    ('com/vmos/core/t', 'I', ['install_app_', '->E(']),
    ('com/vmos/exsocket/engine/a', 'E', ['->t(']),
    ('com/vmos/exsocket/engine/EngineSession$d', 'run', ['listFiles', 'getFD', 'setFileDescriptorsForSend']),
    ('com/vmos/exsocket/rom/c', 'm', ['getAncillaryFileDescriptors']),
    ('com/vmos/exsocket/sample/rom/apks/a', 'f', ['install patch Apks', 'install a single Apk']),
    ('com/vmos/exsocket/sample/rom/apks/a', 'k', ['install-write', 'install-commit']),
    ('com/vmos/exsocket/sample/rom/apks/a', 'm', ['install-create']),
    # JADX renames the actual Unicode package to p000 in its Java output.
    ('com/vmos/exsocket/sample/rom/apks/\u02cb/b', 'i', ['FileInputStream']),
]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('apk', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    from loguru import logger
    logger.remove()
    from androguard.core.dex import DEX
    args.output.mkdir(parents=True, exist_ok=True)
    wanted = {('L'+cls+';', method): needles for cls, method, needles in TARGETS}
    evidence, listing = {}, []
    with zipfile.ZipFile(args.apk) as archive:
        for name in archive.namelist():
            if not re.fullmatch(r'classes\d*\.dex', name): continue
            dex = DEX(archive.read(name))
            for cls in dex.get_classes():
                for method in cls.get_methods():
                    key = (cls.get_name(), method.get_name())
                    if key not in wanted: continue
                    instructions = [i.get_name()+' '+i.get_output() for i in method.get_instructions()]
                    text = '\n'.join(instructions)
                    checks = {needle: needle in text for needle in wanted[key]}
                    label = key[0]+'->'+key[1]
                    evidence[label] = dict(dex=name, descriptor=method.get_descriptor(),
                        instruction_count=len(instructions), checks=checks)
                    listing.append(label+' '+method.get_descriptor()+' ['+name+']\n'+text)
    missing = ['->'.join(key) for key in wanted if '->'.join(key) not in evidence]
    failed = {key: value['checks'] for key, value in evidence.items() if not all(value['checks'].values())}
    report = dict(classification='DEX derived; static only', executed=False,
        methods=evidence, missing=missing, failed=failed)
    (args.output/'app-import-dex.txt').write_text('\n\n'.join(listing)+'\n')
    (args.output/'app-import-checks.json').write_text(json.dumps(report, indent=2)+'\n')
    assert not missing and not failed, (missing, failed)
    print(f'PASS: {len(evidence)} import-path methods verified directly in DEX; target not executed')

if __name__ == '__main__': main()
