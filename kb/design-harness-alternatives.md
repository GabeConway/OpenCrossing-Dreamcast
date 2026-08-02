# Flycast harness — other emulators evaluated

lxdream-nitro / "Nitrocast" (builds on this Mac, does not run — two real
Apple-Silicon bugs found in its `mem.c`), and RetroArch + the flycast libretro
core (untested). Read only when considering a second emulator — Flycast is the
harness. Part of `kb/design-harness.md`, whose stub maps every § to its file.

Recon pass 2026-08-01. Everything below is tagged **[V]** = verified by running
it on this machine today, or **[U]** = unverified / inferred from source only.
Where something did not work, it says so.

## 9. Secondary emulator: lxdream-nitro / "Nitrocast" — NOT usable today

`https://gitlab.com/simulant/community/lxdream-nitro` (last activity
2026-07-29; the `simulant/lxdream-nitro` URL redirects to it). Positions
itself as "a Dreamcast emulator designed primarily to aid homebrew
development": very accurate SH-4, weak PVR. Its CLI is *exactly* what an
AI harness wants — **[V]** from `--help` of a binary built here:

```
-H, --headless          Run in headless (no video) mode
-t, --run-time=SECONDS  Run for the specified number of seconds   (auto-quits)
-e, --execute=PROGRAM   Load and execute the given SH4 program
-g, --gdb-sh4=PORT      Start GDB remote server on PORT for SH4
-G, --gdb-arm=PORT      Start GDB remote server on PORT for ARM
-b, --biosless          Run without the BIOS boot rom
-r, --ram-size=SIZE     [16, 32]
-x / -X                 interpreter only / interpreter+translator cross-check
-T, --trace=REGIONS     trace output
-m, --multiplier=SCALE  SH4 speed multiplier
```

plus (per its README) mounting a host folder at `/pc` and **exiting with the
guest ELF's return code**.

**[V] Build on this Mac: it compiles, it does not run.**

- `brew install meson ninja gtk+3` (glib/libpng already present);
  `meson setup builddir && meson compile -C builddir`.
- Needed a 3-line patch: `src/drivers/osx_iokit.m` uses `kIOMainPortDefault` /
  `IOMainPort`, which the stale 11.3 SDK doesn't have → replaced with
  `kIOMasterPortDefault` / `IOMasterPort`. (Goes away once the CLT are updated.)
- **Two genuine Apple-Silicon bugs found in `src/mem.c`:**
  1. `map_perms()` tests `perms & PERMS_EXEC`, but the enum is
     `PERMS_READ=0x81, PERMS_WRITE=0x82, PERMS_EXEC=0x84` — they share bit
     `0x80`, so **every** allocation requested `PROT_EXEC`. On arm64 macOS
     W^X is enforced, so a plain RW page map `mmap` returned `EPERM`
     (`FATAL Unable to allocate page map!`). Fixed locally by testing
     `0x01/0x02/0x04`.
  2. Genuinely executable allocations then still failed: on Apple Silicon
     `PROT_EXEC` anon mappings need `MAP_JIT` plus the
     `com.apple.security.cs.allow-jit` entitlement. Added both
     (ad-hoc `codesign --entitlements`).
- After both fixes it gets past memory init and then **SIGBUS**es with an
  all-zero SH-4 register dump before executing any guest code. lxdream's core
  is x86-oriented (SSE2 paths, x86-only translator, RWX assumptions); porting
  it to arm64 macOS is a real project, not a patch.

**Verdict: NO-GO as a harness right now.** Keep it on the list because its
GDB-for-SH4-*and*-ARM7, MMIO tracing and interpreter/translator cross-check
are the right tools for the alignment class that Flycast cannot see (§6).
Two **[U]** ways back in, in order of promise: (1) build and run it inside an
`--platform linux/amd64` container under colima (qemu-emulated, slow, but the
x86 assumptions hold); (2) upstream the two `mem.c` fixes and finish the arm64
port. Also **[U]**: `deecy` (Zig, referenced by Nitrocast's own README as the
better modern emulator) was not evaluated.

**[U] RetroArch + flycast libretro core** as a "headless" option was not
tested. RetroArch has no true headless mode for running content on macOS, and
the libretro core is a different build with different config plumbing; it also
loses the `-config Debug.SerialConsoleEnabled` path (libretro logging goes
through the frontend). Lower priority than a from-source Flycast.

---
