#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run on the existing Pi test host. Install only AOSP Pocket; keep ADB alive.

    scp out/aosp-pocket.apk pi@juanraspberry:/home/pi/aosp-pocket.apk
    ssh pi@juanraspberry python3 - < tools/validate_on_pi.py

Root is used only by the external harness to retrieve the non-debuggable app's
own report. All native execution and probes run under the app's ordinary UID.
"""
import json
from pathlib import Path
import shlex
import sys
import time

BASE = Path("/home/pi/lineage-cuttlefish")
PACKAGE = "dev.kreijstal.aosppocket"
REPORT = f"/data/user/0/{PACKAGE}/files/bootstrap-result.json"
APK = Path("/home/pi/aosp-pocket.apk")


def main():
    sys.path.insert(0, str(BASE / "tools"))
    import cuttlefish_arm64 as cvd
    cvd.require_virtual()

    def adb(*args, timeout=20):
        result = cvd.adb("-s", cvd.serial(), *args, timeout=timeout, check=False)
        if result.returncode:
            raise RuntimeError(f"ADB failed: {result.stdout}\n{result.stderr}")
        return result.stdout

    def shell(cmd, root=False):
        return adb("shell", "-T", "su -c " + shlex.quote(cmd) if root else cmd)

    print(adb("install", "-r", str(APK), timeout=60).strip(), flush=True)
    shell(f"am force-stop {PACKAGE}")
    shell("rm -f " + shlex.quote(REPORT), root=True)
    print(shell(f"am start -n {PACKAGE}/.MainActivity --ez run_bootstrap true").strip(), flush=True)
    report = None
    deadline = time.monotonic() + 35
    while time.monotonic() < deadline:
        path = shlex.quote(REPORT)
        raw = shell(f"if [ -f {path} ]; then cat {path}; else echo null; fi", root=True)
        try:
            report = json.loads(raw)
            if report is not None:
                break
        except json.JSONDecodeError:
            pass
        time.sleep(0.5)
    if report is None:
        raise RuntimeError("App did not produce a report within 35 seconds")
    Path("/home/pi/aosp-pocket-bootstrap.json").write_text(json.dumps(report, indent=2) + "\n")
    assert report["stage"] == "native_bootstrap", report
    assert report["bootstrap_ok"], report
    assert report["native_uid_matches_app"] and report["app_uid"] >= 10000, report
    assert report["android_framework_booted"] is False, report
    assert report["guest_isolation_verified"] is False, report
    assert report["android_shared_memory"] is True, report
    assert len(report["steps"]) == 6 and all(s["ok"] for s in report["steps"]), report
    assert all(s["output_matches"] for s in report["steps"][1:]), report
    print(json.dumps({k: report[k] for k in ("bootstrap_ok", "app_uid", "elapsed_ms", "capabilities")}, indent=2))
    print("PASS: native AOSP launch, five command outputs, same UID, shared memory, honest stage flags")


if __name__ == "__main__":
    main()
