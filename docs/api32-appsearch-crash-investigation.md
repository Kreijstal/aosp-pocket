# Phone AppSearch crash — saved evidence and offline analysis

Status: **unresolved**. This is a guest crash investigation, not a diagnosis of
the user's reported host One UI restart or a full-lifecycle qualification.
No crash-handling checks, storage data or third-party APK were changed.

The phone's saved tombstones record:

- 2026-11-16 16:08:41 UTC: guest `system_server` crashes in `__memcpy`, called
  from `IcingDynamicTrieStorage::MakeSuffix` in AppSearch's `libicing.so` while
  regenerating the schema's derived files.
- Four seconds later: `pocket-core-api32` aborts on an unchecked HIDL return
  status, with `FAILED_TRANSACTION: Host wake-lock release failed`.

The exact library was verified against the tombstone's Build ID
`58582e14cce8e9aa58280e4cba9cea75`. Its SHA-256 is
`de01dd4c79fff6d55b4ffe8e8ece6ca19fc77e42dd24d83d784ad3c543484c9c`;
the bytes are identical in images 0.78.20 and 0.78.169.

## Established failure path

Disassembly at `0x4e364` obtains mutable array storage through the function at
`0x4aa78`; at `0x4e37c` it calls `memcpy` without checking the returned pointer.
The crash has destination `x0=0` and copy length 35. The saved storage header
contains maximum suffix bytes 131072, used suffix bytes 0 and value size 2.
The top-level suffix-capacity test therefore has enough room for this insert.

The array-storage path can return null when growth fails, and its mapping
state also needs inspection. **The saved evidence does not establish the
underlying syscall error, failed write, mapping failure or responsible writer.**
Removing AppSearch, clearing its index or ignoring its crash would not fix
that platform issue. The newer local Icing source uses different status-return
handling, so it must not be treated as the exact source of this Android 12L
binary or substituted as an untested backport.

## Evidence retained locally

Private diagnostics are under
`out/revolut-after-170/02248b7cf656/`; full tombstones stay outside source and
release archives because they include process memory.

| File | SHA-256 |
| --- | --- |
| `tombstone_16` | `04030893f5dbb40b2a562cb928f0489143a492352dc0fdfe1e3c9bb7fd2d527f` |
| `tombstone_17` | `fdf1b0769cf983627fb4d914964d1d96fd57ea4123388a7b08cc74b13739b2c2` |

That directory also holds verified-library disassembly, the decoded storage
header, launch/window records and host-access failures. The last saved
Revolut Java exception predates the new loader. A subsequent launch produced
a resumed/drawn LoginActivity record, which does not establish a usable
screen or completed login.

The host's boot clock reports a kernel boot at 2026-11-02 09:03:53 UTC. That
rules out a recent full kernel reboot, while leaving a framework or launcher
restart possible. Termux was denied `android.permission.DUMP` for host
DropBox and launcher exit history. No authorized ADB device was available.
These guest tombstones do not establish the host One UI restart's cause.

## Next device observations

Use the authenticated screenshot command to capture the actual failing UI.
For an AppSearch reproduction, retain the syscall result and errno of file
growth and mapping operations, along with the relevant descriptor's access
mode and storage state. Match new records to the failing PID and time without
clearing indexes or resetting encrypted data. Host One UI diagnosis separately
requires authorized access to its host crash/exit records.
