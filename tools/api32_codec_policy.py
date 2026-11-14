# SPDX-License-Identifier: GPL-3.0-only
"""Keep Codec2's stock watchdog consistent with the normal VM timeout profile."""

PROPERTY = 'debug.stagefright.ccodec_timeout_mult'


def configure_codec_timeout_profile(profile):
    def values(key):
        found = []
        for line in profile.splitlines():
            line = line.strip()
            if line and not line.startswith('#') and '=' in line:
                name, value = line.split('=', 1)
                if name.strip() == key:
                    found.append(value.strip())
        return found

    hardware = values('ro.hw_timeout_multiplier')
    if len(hardware) != 1 or hardware[0] not in ('1', '2', '3', '4'):
        raise ValueError('Expected one bounded hardware timeout multiplier (1..4)')
    codec = values(PROPERTY)
    if codec and codec != hardware:
        raise ValueError('Conflicting or duplicate Codec2 timeout multiplier')
    if not codec:
        profile += ('' if profile.endswith('\n') else '\n')
        profile += ('\n# Codec2 has a separate watchdog multiplier; retain the existing VM scale.\n'
                    + PROPERTY + '=' + hardware[0] + '\n')
    multiplier = int(hardware[0])
    return profile, {
        'property': PROPERTY,
        'hardwareTimeoutMultiplier': multiplier,
        'codecTimeoutMultiplier': multiplier,
        'nominalOperationTimeoutMs': 1500 * multiplier,
        'stockWatchdogIntervalUs': 3300000,
        'watchdogEnabled': True,
        'runtimeVerified': False,
    }
