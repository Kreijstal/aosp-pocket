#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Contain the Android 12L charging ringtone failure during volume recovery."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from zipfile import ZipFile

from target import ROOT

SMALI = ROOT.parent / 's20fe-hardware-lab/artifacts/lineage/source/prebuilts/extract-tools/common/smali'
NOTIFIER = 'com/android/server/power/Notifier.smali'
METHOD = re.compile(r'^\.method private playChargingStartedFeedback\(IZ\)V\n.*?^\.end method$', re.M | re.S)
PLAY = '    invoke-virtual {v4}, Landroid/media/Ringtone;->play()V'
HANDLER = '''    :pocket_charging_unavailable
    move-exception v0

    const-string v1, "PowerManagerNotifier"
    const-string v2, "Charging sound unavailable during storage recovery"
    invoke-static {v1, v2, v0}, Landroid/util/Slog;->w(Ljava/lang/String;Ljava/lang/String;Ljava/lang/Throwable;)I
    return-void
'''


def patch(source):
    methods = list(METHOD.finditer(source))
    if len(methods) != 1 or methods[0][0].count(PLAY) != 1 or '.catch' in methods[0][0]:
        raise ValueError('Unexpected stock charging feedback implementation')
    method = methods[0][0].replace(PLAY, '\n'.join((
        '    :pocket_charging_play_start', PLAY, '    :pocket_charging_play_end',
        '    .catch Ljava/lang/IllegalArgumentException; {:pocket_charging_play_start .. :pocket_charging_play_end} :pocket_charging_unavailable',
    )))
    method = method.replace('.end method', '\n' + HANDLER + '.end method')
    return source[:methods[0].start()] + method + source[methods[0].end():]


def build(source, output):
    output.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(source) as original, tempfile.TemporaryDirectory() as temp:
        work = Path(temp)
        names = [name for name in original.namelist() if re.fullmatch(r'classes\d*\.dex', name)
                 and b'playChargingStartedFeedback' in original.read(name)]
        if len(names) != 1:
            raise ValueError('Expected one DEX containing charging feedback')
        name, = names
        dex = work / name
        dex.write_bytes(original.read(name))
        smali = work / 'smali'
        subprocess.run(['java', '-jar', SMALI / 'baksmali.jar', 'disassemble', dex, '-o', smali], check=True)
        notifier = smali / NOTIFIER
        notifier.write_text(patch(notifier.read_text()))
        subprocess.run(['java', '-jar', SMALI / 'smali.jar', 'assemble', '--api', '32', smali, '-o', dex], check=True)
        with ZipFile(output, 'w') as result:
            for entry in original.infolist():
                result.writestr(entry, dex.read_bytes() if entry.filename == name else original.read(entry))
    return output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    print(build(args.source, args.output))
