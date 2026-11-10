# SPDX-License-Identifier: GPL-3.0-only
"""Declare the normal rootless device's emulator-package and boot-component policy."""
import json
import xml.etree.ElementTree as ET
from prepare_api32 import ROOT


def device_profile_payloads():
    raw = (ROOT / 'runtime/api32/device-profile.json').read_bytes()
    profile = json.loads(raw)
    # Expanding this policy requires an explicit review, not a broad package filter.
    omitted = profile['omittedReferencePackages']
    receivers = profile['disabledBootReceivers']
    if (profile['schema'] != 1 or profile['sdk'] != 32 or profile['profile'] != 'rootless-v1'
            or [(p['package'], p['directory']) for p in omitted] != [
                ('com.android.emulator.multidisplay', 'system_ext/priv-app/MultiDisplayProvider/')]
            or [(p['package'], p['class']) for p in receivers] != [
                ('com.android.dynsystem', 'com.android.dynsystem.BootCompletedReceiver')]
            or any(not p.get('reason') for p in omitted + receivers)):
        raise ValueError('Review changes to the API32 rootless device policy')
    root = ET.Element('config')
    for receiver in receivers:
        package = ET.SubElement(root, 'component-override', {'package': receiver['package']})
        ET.SubElement(package, 'component', {'class': receiver['class'], 'enabled': 'false'})
    xml = ET.tostring(root, encoding='utf-8', xml_declaration=True) + b'\n'
    return profile, {
        'system/etc/pocket-device-profile.json': raw,
        'system/etc/sysconfig/pocket-device-profile.xml': xml,
    }, tuple(p['directory'] for p in omitted)
