# rongyaoidea/ananbox-fork review

Reviewed commit `d8436891759232cef8d7d3801a208e6c0605d930` from
[the public fork](https://github.com/rongyaoidea/ananbox-fork). This was a
read-only source review; its app was not installed and no fork code or ROM was
added to AOSP Pocket.

The fork pins Anbox commit `fbe569a8a3a42cfdf7276422ba5da128cfb2e124`
and PRoot commit `8c60d0287ce9ece3f21df1dc60de55e33f6bdba3`, the same upstream
commits in our lockfile before local patches. Its `RomCatalog.kt` points to the
same ARM64 reference archive digest
`31c7976b6b3b6f6df1028b8dde67fffc2d7b3b7a1abb5f948de3d6517860fe4b`.
It does not supply an Android 9 runtime port in the inspected tree.

Its additions include:

- A Kotlin Anna host gateway and Python MCP client for guest console commands,
  app launching, APK installation, file writes and keyboard input.
- In-app ROM download from mirrored GitHub releases, with size/digest checks.
- JSON metadata for runtime-captured Binder Parcel templates.
- A Binder death recipient that clears a stale registry reference and unlinks
  a replaced registry's death recipient.
- CI/cross-compilation fixes and focused tests.

The Binder lifecycle diagnostics and guest management interfaces are useful
references for future improvements. They do not replace the Android 9
framework/ART/native integration work.

`GuestCommand.needsNoSeccomp` selects the PRoot accelerator based on whether
the host kernel predates Linux 4.8. That test does not cover the modern host
Android seccomp-TRAP conflict observed in our VM. The inspected build uses the
same PRoot backend, so the fork provides no demonstrated replacement for our
full-ptrace execution path.

Relevant source files at the reviewed commit:

- [PRoot build](https://github.com/rongyaoidea/ananbox-fork/blob/d8436891759232cef8d7d3801a208e6c0605d930/app/src/main/cpp/cmake/Proot.cmake)
- [ROM catalog](https://github.com/rongyaoidea/ananbox-fork/blob/d8436891759232cef8d7d3801a208e6c0605d930/app/src/main/java/com/github/ananbox/RomCatalog.kt)
- [Guest command construction](https://github.com/rongyaoidea/ananbox-fork/blob/d8436891759232cef8d7d3801a208e6c0605d930/app/src/main/java/com/github/ananbox/anna/GuestCommand.kt)
- [Binder receiver](https://github.com/rongyaoidea/ananbox-fork/blob/d8436891759232cef8d7d3801a208e6c0605d930/app/src/main/java/com/github/ananbox/BinderReceiver.kt)
