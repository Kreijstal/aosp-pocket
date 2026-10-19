# SPDX-License-Identifier: GPL-3.0-only
"""Read method ordinals from the pinned, unversioned Android 9 AIDL inputs."""
import re
from prepare_api28 import ROOT


def generate(output):
    interfaces={
        'IVold':ROOT/'out/api28/system-vold/binder/android/os/IVold.aidl',
        'IVoldListener':ROOT/'out/api28/system-vold/binder/android/os/IVoldListener.aidl',
        'IVoldTaskListener':ROOT/'out/api28/system-vold/binder/android/os/IVoldTaskListener.aidl',
        'IStoraged':ROOT/'out/api28/system-core/storaged/binder/android/os/IStoraged.aidl',
        'INetd':ROOT/'out/api28/system-netd/server/binder/android/net/INetd.aidl',
    }
    lines=['// Generated from pinned Android 9 AIDL declaration order.','enum StorageTransaction {']
    for interface,path in interfaces.items():
        text=re.sub(r'/\*.*?\*/|//[^\n]*','',path.read_text(),flags=re.S)
        body=text.split('interface '+interface,1)[1].split('{',1)[1].rsplit('}',1)[0]
        body=re.sub(r'\bconst\s+[^;]+;','',body)
        methods=list(re.finditer(r'(?:@[\w.]+\s+)*(?:[\w.]+(?:\[\])?)\s+(\w+)\s*\([^;]*?\)\s*;',body,re.S))
        assert methods and not re.sub(r'(?:@[\w.]+\s+)*(?:[\w.]+(?:\[\])?)\s+(\w+)\s*\([^;]*?\)\s*;','',body,flags=re.S).strip(),path
        for ordinal,method in enumerate(methods,1):
            lines.append(f'    {interface}_{method[1]} = {ordinal},')
    lines.append('};')
    output.write_text('\n'.join(lines)+'\n')
