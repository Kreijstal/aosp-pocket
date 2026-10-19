#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""End-to-end regression of a running Pocket guest on the guarded outer VM.

Copy out/runtime/guest-test.apk to /home/pi/pocket-guest-test.apk first.
The runtime app must be open and its guest booted. Uses the existing dedicated
ADB server, a temporary forward to this app's loopback guest-adbd bridge, and
only the included test APK. Does not restart ADB or change host settings.
"""
import json
from pathlib import Path
import re
import shlex
import sys
import time
import xml.etree.ElementTree as ET

PACKAGE = "dev.kreijstal.aosppocket.runtime"
TEST = "dev.kreijstal.pocketguesttest"

def main():
    sys.path.insert(0, "/home/pi/lineage-cuttlefish/tools")
    import cuttlefish_arm64 as c
    c.require_virtual()

    def adb(*args, timeout=40):
        result = c.adb(*args, check=False, timeout=timeout)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        return result.stdout.strip()

    def outer(command, root=False):
        return adb("-s", c.serial(), "shell", "-T",
            "su -c " + shlex.quote(command) if root else command)

    # Wait through the outer harness first: exercise normal app startup before
    # opening the legacy guest-adbd transport during framework initialization.
    marker_path = "/data/user/0/" + PACKAGE + "/files/rootfs/data/pocket-boot-complete"
    def process_list():
        return [line.split(None, 3) for line in outer("ps -A -o UID,PID,PPID,NAME").splitlines()[1:]]

    def wait_boot():
        deadline = time.monotonic() + 240
        while True:
            processes = process_list()
            host = next((p for p in processes if len(p) == 4 and p[3] == PACKAGE), None)
            tracers = {p[1] for p in processes if host and len(p) == 4 and p[0] == host[0] and
                       p[2] == host[1] and p[3] == "libproot.so"}
            live_init = any(len(p) == 4 and p[2] in tracers and p[3] == "init" for p in processes)
            # Only accept a marker while this host process has a live guest.
            if live_init and outer("if [ -f " + marker_path + " ]; then cat " + marker_path + "; else echo 0; fi", root=True) == "1":
                print("Guest boot marker received", flush=True)
                return host
            if time.monotonic() >= deadline:
                raise RuntimeError("Guest boot marker did not arrive within 240 seconds")
            time.sleep(2)

    host = wait_boot()
    warm_restart = "--restart" in sys.argv[1:]
    if warm_restart:
        for action in ("Stop", "Start"):
            outer("uiautomator dump /sdcard/pocket-runtime-controls.xml")
            controls = ET.fromstring(outer("cat /sdcard/pocket-runtime-controls.xml"))
            button = next(n for n in controls.iter("node") if n.get("package") == PACKAGE and
                          n.get("text") == action and n.get("enabled") == "true")
            left, top, right, bottom = map(int, re.findall(r"\d+", button.get("bounds")))
            outer(f"input touchscreen -d 0 tap {(left+right)//2} {(top+bottom)//2}")
            if action == "Stop":
                deadline = time.monotonic() + 20
                while True:
                    processes = process_list()
                    children = [p for p in processes if len(p) == 4 and p[0] == host[0] and p[3] != PACKAGE]
                    if not children: break
                    if time.monotonic() >= deadline:
                        raise RuntimeError("Stop left guest/tracer processes: " + repr(children))
                    time.sleep(1)
                assert any(p == host for p in processes), "Stop killed the host app"
                print("PASS: Stop removed every guest/tracer process, keeping the host alive", flush=True)
        assert wait_boot() == host, "Restart replaced the host app process"
        print("PASS: guest rebooted inside the same host app process", flush=True)
    port = adb("-s", c.serial(), "forward", "tcp:0", "tcp:5559")
    assert port.isdecimal(), port
    serial = "127.0.0.1:" + port
    try:
        print(adb("connect", serial), flush=True)
        def guest(command):
            return adb("-s", serial, "shell", command)
        release = guest("getprop ro.build.version.release")
        sdk = guest("getprop ro.build.version.sdk")
        assert release == "7.1.1" and sdk == "25", (release, sdk, "Wrong guest; aborting")
        adb("-s", serial, "push", "/home/pi/pocket-property-probe", "/data/local/tmp/pocket-property-probe")
        property_probe = json.loads(guest("chmod 755 /data/local/tmp/pocket-property-probe; /data/local/tmp/pocket-property-probe"))
        assert property_probe["guest_uid"] == 39999 and property_probe["property_uid"] == 0 and property_probe["property_gid"] == 0, property_probe
        assert property_probe["host_api"] == outer("getprop ro.build.version.sdk"), property_probe
        print("PASS: fresh guest UID 39999 reads root-owned property area", flush=True)
        deadline = time.monotonic() + 180
        while guest("getprop sys.boot_completed") != "1":
            if time.monotonic() >= deadline:
                raise RuntimeError("Guest has not completed boot within 180 seconds")
            time.sleep(2)
        installed = adb("-s", serial, "install", "--no-streaming", "-r", "/home/pi/pocket-guest-test.apk", timeout=55)
        assert "Success" in installed, installed
        print(installed, flush=True)
        print(guest("am start -n " + TEST + "/.MainActivity"), flush=True)

        def hierarchy(inside):
            if inside:
                guest("uiautomator dump /data/local/tmp/pocket-test.xml")
                return ET.fromstring(guest("cat /data/local/tmp/pocket-test.xml"))
            outer("uiautomator dump /sdcard/pocket-runtime-outer.xml")
            return ET.fromstring(outer("cat /sdcard/pocket-runtime-outer.xml"))

        button = None
        for _ in range(5):
            tree = hierarchy(True)
            button = next((n for n in tree.iter("node") if n.get("package") == TEST and
                           n.get("text", "").casefold() == "guest taps: 0"), None)
            if button is not None: break
            time.sleep(1)
        assert button is not None, "Guest test app did not render"
        host = hierarchy(False)
        surface = next((n for n in host.iter("node") if n.get("package") == PACKAGE and
                        n.get("class") == "android.view.SurfaceView"), None)
        assert surface is not None, "Outer runtime app is not foreground; refusing input"
        def bounds(node):
            return list(map(int, re.findall(r"\d+", node.get("bounds"))))
        left, top, right, bottom = bounds(surface)
        x1, y1, x2, y2 = bounds(button)
        size = re.findall(r"(\d+)x(\d+)", guest("wm size"))[-1]
        width, height = map(int, size)
        x = left + (x1 + x2) * (right - left) // (2 * width)
        y = top + (y1 + y2) * (bottom - top) // (2 * height)
        # This is input to the OUTER Surface; Native.touch must carry it into
        # the guest. Sending adb input to the guest would not test that bridge.
        outer(f"input touchscreen -d 0 tap {x} {y}")
        tapped = False
        for _ in range(4):
            tree = hierarchy(True)
            tapped = any(n.get("package") == TEST and n.get("text", "").casefold() == "guest taps: 1"
                         for n in tree.iter("node"))
            if tapped: break
            time.sleep(1)
        assert tapped, "Touch sent to the outer Surface did not reach the guest app"
        marker = outer("cat /data/user/0/" + PACKAGE + "/files/rootfs/data/pocket-boot-complete", root=True)
        assert marker == "1", "App did not observe the fresh guest boot-complete property"
        uid = int(re.search(r"uid:(\d+)", outer("cmd package list packages -U " + PACKAGE))[1])
        processes = [line.split(None, 3) for line in outer("ps -A -o UID,PID,PPID,NAME").splitlines()[1:]]
        init = next(p for p in processes if len(p) == 4 and p[0] == str(uid) and p[3] == "init")
        security = outer(f"cat /proc/{init[1]}/status; cat /proc/{init[1]}/attr/current", root=True)
        assert re.search(r"Uid:\s+" + str(uid) + r"\s", security)
        assert re.search(r"Seccomp:\s+2", security) and "untrusted_app" in security
        outer("screencap -p /sdcard/pocket-guest-test.png")
        adb("-s", c.serial(), "pull", "/sdcard/pocket-guest-test.png", "/home/pi/pocket-guest-test.png")
        report = dict(guest_release=release, guest_sdk=int(sdk), boot_completed=True,
            boot_marker=True, guest_apk_installed=True, guest_app_rendered=True,
            forwarded_touch=True, host_app_uid=uid, host_seccomp=2,
            fresh_nonroot_property_probe=property_probe,
            host_untrusted_app=True, nested_kvm_required=False,
            warm_restart=warm_restart,
            screenshot="/home/pi/pocket-guest-test.png")
        Path("/home/pi/pocket-runtime-validation.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))
        print("PASS: rootless guest boot, separate APK installation/render, forwarded Surface touch, boot marker")
    finally:
        adb("disconnect", serial)
        adb("-s", c.serial(), "forward", "--remove", "tcp:" + port)

if __name__ == "__main__":
    main()
