#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Prepare the pinned, upstream reference guest for the self-contained APK."""
import json
from pathlib import Path
import subprocess
import sys
from build_guest_binder import prepare_image

ROOT = Path(__file__).resolve().parents[1]

if __name__ == '__main__':
    image = prepare_image(json.loads((ROOT / 'sources.lock.json').read_text())['guest_framework'])
    subprocess.run([sys.executable, str(ROOT / 'tools/pack_guest_image.py'), str(image), sys.argv[1]], check=True)
