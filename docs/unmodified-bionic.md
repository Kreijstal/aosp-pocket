# Unmodified Bionic native mode

`POCKET_BIONIC_MODE=unmodified` opts a **fresh native process** out of Pocket's
pinned-libc function-entry patches and private property-state reset. An unset
variable or `wrapped` retains the working compatibility backend. Other values
terminate startup with exit 125 and `DIRECT_BOOT_INVALID_BIONIC_MODE`.

This does not undo patches in an already-running process or in a child forked
from its patched zygote. Use a fresh exec with an explicit environment. The
original libc file on disk is unchanged in both modes.

The unmodified mode preserves executable Bionic bytes and exposes ordinary
kernel filesystem and UID semantics. It does not supply a virtual root, virtual
UIDs or private Android properties. The preload shim still exists and exports
its existing linker/compatibility functions; this is not a shim-free runtime.
It does not conceal libraries, paths, or alter app security decisions.

Full Android boot in this mode is **not supported yet**. The syscall backend
must cover the path and identity operations currently supplied by the libc
wrappers before this can replace the default backend. Do not enable it globally
for init or zygote. No automatic app-specific switching is implemented.

## Reproducible native validation

With the pinned Android 9 reference already prepared:

```sh
python tools/build_bionic_mode_probe.py --ndk /home/kreijstal/.local/share/android-neutral-test/sdk/ndk/android-ndk-r27c
rsync -a out/bionic-mode/ pi@juanraspberry:/home/pi/bionic-mode/
rsync -a tools/test_bionic_mode_on_pi.py pi@juanraspberry:/home/pi/
ssh pi@juanraspberry 'python3 -u /home/pi/test_bionic_mode_on_pi.py'
```

The Pi harness requires the existing paired Pocket administration console and
guards against running outside the virtual Android host. It launches a private
rebuilt shim in isolated subprocesses under Pocket's real host application UID.
It does not replace the running guest's library or restart the guest. The static
launcher bypasses the existing dynamic exec wrapper only for these test
subprocesses, so the test's explicit environment/preload is preserved.

The probe compares every file-backed executable byte of the loaded guest libc
with the same library on disk. It also checks real versus virtual UID semantics,
guest versus backing-path access, rejection of an invalid mode, and that the
existing Android guest remains ready.

| Observation | Wrapped | Unmodified |
| --- | ---: | ---: |
| Executable bytes compared | 858,956 | 858,956 |
| Differing bytes | 763 | 0 |
| libc `getuid()` | 12345 (test guest identity) | 10177 (real kernel UID) |
| Guest fixture path | Opens | ENOENT |
| Explicit backing path | Opens | Opens |

These are native results observed inside Pocket on the Pi's LineageOS VM.
They do not establish app compatibility or successful Android boot using
unmodified Bionic. Machine-readable evidence: [bionic-mode-validation.json](bionic-mode-validation.json).

The focused builder regenerates the hook table from the selected pinned ARM64
libc in its own output directory. It does not reuse `out/boot/boot_hooks.h`,
which may belong to a previous x86-64 build.
