# SPDX-License-Identifier: GPL-3.0-only
"""Keep software-profile hardware declarations consistent with its backends."""
import json
from pathlib import Path
from xml.dom import minidom
from prepare_api32 import ROOT

PROFILE = ROOT / 'runtime/api32/software-hardware.json'
# Declared by system/etc/permissions/pocket-camera.xml, backed by the host camera provider.
CAMERA_DECLARED = {'android.hardware.camera', 'android.hardware.camera.any'}


def apply_hardware_profile(stage):
    stage = Path(stage)
    if not (stage / '.pocket-api32-fixture').is_file():
        raise ValueError('Expected the isolated SDK32 staging tree')
    payload = PROFILE.read_bytes()
    profile = json.loads(payload)
    if (profile['schema'] != 1 or profile['sdk'] != 32 or
            profile['backends'] != {'bluetoothHci': False, 'cameraProvider': True} or
            not profile['features'] or
            {name for name, value in profile['features'].items() if value is not False} != CAMERA_DECLARED or
            any(not name.startswith(('android.hardware.bluetooth', 'android.hardware.camera')) for name in profile['features'])):
        raise ValueError('Review hardware declarations when adding an actual backend')
    disabled = {name for name, value in profile['features'].items() if value is False}
    changed = {}
    for partition in ['system', 'system_ext', 'product', 'vendor']:
        for directory in ['permissions', 'sysconfig']:
            for path in sorted((stage / partition / 'etc' / directory).glob('*.xml')):
                original = path.read_bytes()
                if not any(name.encode() in original for name in disabled):
                    continue
                document = minidom.parseString(original)
                removed = []
                for element in list(document.getElementsByTagName('feature')):
                    name = element.getAttribute('name')
                    if name.startswith(('android.hardware.bluetooth', 'android.hardware.camera')) and name not in disabled \
                            and name not in CAMERA_DECLARED:
                        raise ValueError('Review newly declared hardware feature: ' + name)
                    if name in disabled:
                        element.parentNode.removeChild(element)
                        element.unlink()
                        removed.append(name)
                if removed:
                    # Preserve copyright comments, processing instructions and
                    # other permissions/features; only feature nodes change.
                    path.write_bytes(document.toxml(encoding='utf-8') + b'\n')
                    changed[path.relative_to(stage).as_posix()] = removed
                document.unlink()
    destination = stage / 'system/etc/pocket-hardware-capabilities.json'
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(payload)
    return changed
