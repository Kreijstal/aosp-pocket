#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate the real image profile and reject contradictory codec policies."""
import argparse
from pathlib import Path
from api32_codec_policy import PROPERTY, configure_codec_timeout_profile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path, required=True)
    args = parser.parse_args()
    original = args.profile.read_text()
    configured, policy = configure_codec_timeout_profile(original)
    assert configured.startswith(original) and configured.count(PROPERTY + '=') == 1
    assert policy['hardwareTimeoutMultiplier'] == policy['codecTimeoutMultiplier'] == 4
    assert policy['nominalOperationTimeoutMs'] == 6000 and policy['watchdogEnabled']
    assert configure_codec_timeout_profile(configured) == (configured, policy)
    for multiplier in range(1, 5):
        text, result = configure_codec_timeout_profile('ro.hw_timeout_multiplier=' + str(multiplier))
        assert text.endswith(PROPERTY + '=' + str(multiplier) + '\n')
        assert result['codecTimeoutMultiplier'] == multiplier
    invalid = ['', 'ro.hw_timeout_multiplier=0', 'ro.hw_timeout_multiplier=5',
               'ro.hw_timeout_multiplier=-1', 'ro.hw_timeout_multiplier=garbage',
               'ro.hw_timeout_multiplier=4\nro.hw_timeout_multiplier=4',
               'ro.hw_timeout_multiplier=4\n' + PROPERTY + '=1',
               configured + PROPERTY + '=4\n']
    for profile in invalid:
        try:
            configure_codec_timeout_profile(profile)
        except ValueError:
            continue
        raise AssertionError('Invalid profile accepted: ' + profile)
    print('PASS: actual VM profile preserved, matching bounded Codec2 scale, idempotence, '
          'four supported scales and eight invalid/conflicting profiles')


if __name__ == '__main__':
    main()
