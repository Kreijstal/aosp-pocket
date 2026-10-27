# SPDX-License-Identifier: GPL-3.0-only
"""Read method ordinals from matching pinned, unversioned platform AIDL inputs."""
import re
from prepare_api28 import ROOT


def generate(output,sdk=28):
    if sdk not in (28,29):raise ValueError('Review AIDL transaction generation for this guest API')
    source=ROOT/('out/api'+str(sdk))
    interfaces={
        'IVold':source/'system-vold/binder/android/os/IVold.aidl',
        'IVoldListener':source/'system-vold/binder/android/os/IVoldListener.aidl',
        'IVoldTaskListener':source/'system-vold/binder/android/os/IVoldTaskListener.aidl',
        'IStoraged':source/'system-core/storaged/binder/android/os/IStoraged.aidl',
        'INetd':source/'system-netd/server/binder/android/net/INetd.aidl',
    }
    if sdk==29:
        interfaces['IApexService']=source/'system-apex/apexd/aidl/android/apex/IApexService.aidl'
        interfaces['IDnsResolver']=source/'system-netd/resolv/binder/android/net/IDnsResolver.aidl'
    lines=['// Generated from pinned Android '+str(sdk-19)+' AIDL declaration order.','enum StorageTransaction {']
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
