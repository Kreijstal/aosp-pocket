#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the ordinary SDK32 private-data/keystore persistence fixture."""
from build_api32_guest_app import build_java_app
from prepare_api32 import ROOT,CACHE

if __name__=='__main__':
    build_java_app(ROOT/'runtime/tests/api32/persistenceapp',CACHE/'persistence-app',
        'dev.kreijstal.pocketpersist32','persistence-test.apk')
