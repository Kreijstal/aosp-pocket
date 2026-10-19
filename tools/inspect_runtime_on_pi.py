#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Read-only status of this test app on the existing guarded Cuttlefish VM.

Run with ssh pi@juanraspberry python3 - < tools/inspect_runtime_on_pi.py
Root belongs only to this external diagnostics harness, not the APK runtime.
Never restart ADB, change VM security or inspect other apps' private files.
"""
import json
from pathlib import Path
import re
import shlex
import sys

PACKAGE = "dev.kreijstal.aosppocket.runtime"

def main():
    sys.path.insert(0, "/home/pi/lineage-cuttlefish/tools")
    import cuttlefish_arm64 as c
    c.require_virtual()

    def shell(command, root=False):
        result = c.adb("-s", c.serial(), "shell", "-T",
            "su -c " + shlex.quote(command) if root else command,
            timeout=25, check=False)
        return result.stdout.strip()

    listing = shell("cmd package list packages -U " + PACKAGE)
    match = re.search(r"package:" + re.escape(PACKAGE) + r" uid:(\d+)", listing)
    if not match:
        raise SystemExit("Runtime app is not installed: " + listing)
    uid = int(match[1])
    processes = []
    for line in shell("ps -A -o UID,PID,PPID,NAME").splitlines()[1:]:
        fields = line.split(None, 3)
        if len(fields) == 4 and fields[0] == str(uid):
            processes.append(dict(uid=uid, pid=int(fields[1]), ppid=int(fields[2]), name=fields[3]))
    base = "/data/user/0/" + PACKAGE + "/files"
    report = {"package": PACKAGE, "host_app_uid": uid, "processes": processes,
        "files": shell("ls -lh " + base, root=True),
        "proot_tail": shell("tail -c 6000 " + base + "/proot.log", root=True),
        "guest_init_tail": shell("tail -c 6000 " + base + "/rootfs/dev/kmsg", root=True),
        "guest_system_tail": shell("tail -c 6000 " + base + "/rootfs/data/system.log", root=True),
        "framework_boot_verified": False}
    for process in processes:
        if process["name"] in ("init", "system_server", "zygote", "zygote64"):
            pid = process["pid"]
            status = shell(f"cat /proc/{pid}/status; cat /proc/{pid}/attr/current", root=True)
            process["host_security"] = "\n".join(line for line in status.splitlines()
                if line.startswith(("Uid:", "Gid:", "Seccomp:", "NoNewPrivs:", "u:r:")))
    # A live system_server is progress, not proof of a completed guest boot.
    report["guest_system_server_present"] = any(p["name"] == "system_server" for p in processes)
    destination = Path("/home/pi/aosp-pocket-runtime-status.json")
    destination.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))

if __name__ == "__main__":
    main()
