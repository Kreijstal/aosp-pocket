#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
from build_api32_guest_app import build_java_app
from prepare_api32 import ROOT,CACHE
if __name__=='__main__':
    build_java_app(ROOT/'runtime/tests/api32/ceapp',CACHE/'ce-app','dev.kreijstal.pocketce32test','ce-test.apk')
