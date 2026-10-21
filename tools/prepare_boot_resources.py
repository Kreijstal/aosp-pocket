#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Stage matching platform resources and preoptimized system-server code."""
import json
from pathlib import Path, PurePosixPath
import posixpath
import shutil
import tarfile
from prepare_api28 import ROOT, prepare
from target import info as arch_info, selected as arch_selected


def prepare_resources(arch=None):
    arch=arch or arch_selected()
    reference=prepare(art=True,arch=arch)
    key='abi_reference' if arch=='arm64' else 'abi_reference_x86_64'
    digest=json.loads((ROOT/'sources.lock.json').read_text())['guest_api28'][key]['layer_digest'].split(':')[1]
    root=ROOT/('out/boot/platform' if arch=='arm64' else 'out/boot/platform-'+arch)
    framework=arch_info(arch)["framework"]
    marker=root/'.source'
    if marker.exists() and marker.read_text().strip()==digest+'-graphics-v1':
        for path in root.rglob("*"):
            if path.is_file(): path.chmod(path.stat().st_mode | 0o400)
        return root
    archive=ROOT/'out/api28/downloads'/(digest+'.tar.gz')
    if root.exists(): shutil.rmtree(root)
    root.mkdir(parents=True,exist_ok=True)
    links=[]
    with tarfile.open(archive) as layer:
        for entry in layer:
            path=PurePosixPath(entry.name)
            if path.is_absolute() or '..' in path.parts: raise SystemExit('Unsafe platform path')
            name=path.as_posix()
            selected=name.startswith(('system/etc/','system/fonts/',f'system/framework/oat/{framework}/','system/app/','system/priv-app/','system/bin/','vendor/lib64/','vendor/bin/','vendor/etc/')) or name in ('system/framework/framework-res.apk','vendor/build.prop')
            if not selected: continue
            if entry.issym():
                target=posixpath.normpath(entry.linkname.lstrip('/') if entry.linkname.startswith('/') else posixpath.join(posixpath.dirname(name),entry.linkname))
                if target.startswith('../') or target.startswith('/'): raise SystemExit('Unsafe platform link')
                links.append((name,target));continue
            if not entry.isfile(): continue
            with layer.extractfile(entry) as stream:
                first=stream.read(64)
                if first.startswith(b'\x7fELF') and first[4]!=2: continue
                destination=root/name;destination.parent.mkdir(parents=True,exist_ok=True)
                with destination.open('wb') as output:
                    output.write(first);shutil.copyfileobj(stream,output)
                destination.chmod((entry.mode&0o777)|0o400)
    # Flatten only selected executable aliases; no archive link is installed.
    for name,target in links:
        if not name.startswith('system/bin/'): continue
        source=root/target
        if source.is_file(): shutil.copy2(source,root/name)
    for name in ('services.jar','ethernet-service.jar','wifi-service.jar','com.android.location.provider.jar'):
        shutil.copy2(reference/'system/framework'/name,root/'system/framework'/name)
    marker.write_text(digest+'-graphics-v1\n')
    return root


if __name__=='__main__': print(prepare_resources())
