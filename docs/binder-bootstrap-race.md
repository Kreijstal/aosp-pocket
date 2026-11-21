# Pocket Binder bootstrap can abort native clients with `EAGAIN`

## Environment and reproduction

Observed in the Android 12L Pocket guest managed by 0.78.182, inside the `cvd-73` LineageOS Cuttlefish VM on the Raspberry Pi. The guest was running and credential-encrypted storage was available. The reproducer uses Android's built-in Package Manager and requires only a ready Pocket guest with administration enabled.

```bash
ssh pi@juanraspberry 'pocket exec -- /system/bin/cmd package path android'
```

**Expected.** `cmd` prints the path to `framework-res.apk` and exits `0`.

**Observed.** The command can print the path and then abort with exit status `134`. Timing varies across runs. The log contains `getAndExecuteCommand(fd=3) returned unexpected error -11, aborting`. The native stack ends in `abort`, called from `android::IPCThreadState::joinThreadPool(bool)` through liblog. `dumpsys package android` completed normally in the same session.

## Observed Binder sequence

`cmd` starts a libbinder thread pool before its first service-manager lookup. Pocket translates that lookup in `binder_bootstrap()` and temporarily sets `O_NONBLOCK` on the shared Binder descriptor so it can poll for replies with a timeout. File status flags apply to the open file description, so the thread pool sees the same flag. The syscall trace showed this order on fd `3`.

1. The lookup thread sets `O_NONBLOCK` with `fcntl(F_SETFL)`.
2. The Binder pool thread calls `ioctl(BINDER_WRITE_READ)` while the flag is set and receives `EAGAIN`.
3. The lookup thread restores blocking mode. The Binder pool thread reports the unexpected `-EAGAIN`, and the process aborts.

## Suspected cause

The temporary flag change may let a pool read that expects to block return `EAGAIN`, triggering the abort. The reproducer can print the package path before the process exits, so the CLI exit status alone does not establish whether a service request completed.

## Candidate change

The proposed loader change retries an eligible read after bootstrap restores blocking mode. It preserves the driver's `write_consumed` value and leaves explicit nonblocking reads unchanged.

Implementation and tests are in [binder_runtime.h](../runtime/loader/binder_runtime.h) and [binder_transport.c](../runtime/tests/binder_transport.c).

## Verification

- `make -f runtime/Makefile check-binder-transport` passed all 43 cases with AddressSanitizer and UndefinedBehaviorSanitizer. The added cases cover `EAGAIN` during and just after bootstrap, preservation of write progress, and explicitly nonblocking descriptors.
- The API 32 ARM64 loader rebuilt successfully with the normal image builder's `build_loader()` and passed its ELF layout validation.
- In an isolated live run, the candidate printed the expected package path and remained blocked waiting for the result callback. The command thread was waiting on a futex and the Binder worker was waiting for work.

## Result-callback hang

The cvd kernel uses the Rust Binder driver, so trace `events/rust_binder/*`. The `binder/*` tracepoints never fire on it. The trace showed that guest system_server sends the one-way `IResultReceiver.send` (code 1) to `cmd`, and that `cmd`'s pool thread receives it and frees it. The trace also confirms the retry: the first pool read returned `-11` and was reissued after bootstrap.

The callback is lost in libbinder. Guest services preload `libpocket_binder_api32.so`. Its `writeInterfaceToken` prefixes an 8-byte caller identity (token format 5), and its `enforceInterface` skips that prefix. `pocket exec` started programs without that preload. As a result, `cmd`'s stock `enforceInterface` rejected the callback, the one-way failure went unseen, and `cmd` waited forever.

The fix is `SyscallBackend.adminEnvironment()`, which `DirectImage.admin()` now uses. It adds the preload for 12L owner-console programs only. The boot environment is unchanged. Run `make -f runtime/Makefile check-admin-environment` to test it. With the preload, `getContextObject` is also served by the adapter rather than by the loader's `binder_bootstrap()`. This is why preloaded clients did not hit the `EAGAIN` race.

Validation on VM 73: the 0.78.182 test manager, with only `DirectImage`/`SyscallBackend` replaced in `classes.dex`, was installed in place. The reproducer `pocket exec -- /system/bin/cmd package path android` returned `0` with the path in 6/6 runs. Before the change it exited `134` in 6/6 runs. The candidate loader with the preload also returned `0` in 6/6 runs, and without it, 6/6 runs hung.
