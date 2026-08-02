# Flycast harness — getting data out of the guest

The console channel (SCIF → host stdout) and its baud cost (§3), screenshots
and the guest-side framebuffer hash/thumbnail (§4), and crash triage without
GDB (§6). Read when deciding how a guest test should report a result.
Part of `kb/design-harness.md`, whose stub maps every § to its file.

Recon pass 2026-08-01. Everything below is tagged **[V]** = verified by running
it on this machine today, or **[U]** = unverified / inferred from source only.
Where something did not work, it says so.

⚠️ §12 and §13 (`kb/design-harness-corrections.md`) were written later
the same day against the real `opencrossing-dc:sdk` image and correct
parts of this file — read them too.
Specifically: **guest code must never call `scif_flush()`** (§12.2) — it
silently kills the console, including the panic path.

## 3. Console capture — the mechanism (VERIFIED, this is the GO)

### How it works

1. KOS registers dbgio handlers in `arch/dreamcast/kernel/init.c` (dcload,
   dcls, **scif**, null, fb) and calls `dbgio_dev_select_auto()`.
   `scif_detected()` unconditionally returns 1, and dcload/dcls are absent
   under Flycast, so **`scif` is auto-selected**. `printf` → newlib
   `_write_r` → `fs_write(1,…)` → KOS console pty (unattached) →
   `dbgio_write_buffer_xlat()` → SCIF.
2. Flycast v2.6 `core/hw/sh4/modules/serial.cpp` installs a `PTYPipe` whose
   `write()` does `::write(tty, &data, 1)` with `tty == 1` (**stdout**) when
   `Debug.SerialConsoleEnabled` is on and `Debug.SerialPTY` is off.

### Verified output

Building a stock KOS hello-world and running the invocation in §2 produced on
**stdout**:

```
--
KallistiOS Git revision 525cbda:
…
maple: attached devices:
  A0: Dreamcast Controller          (01000000: Controller)
OC-DC-HARNESS-BEGIN
dbgio_dev=scif
RAW-DBGIO-LINE
TICK 0 … TICK 4
MARK:BOOT_OK
OC-DC-HARNESS-END rc=0
arch: shutting down kernel
```

**[V]** Both `printf()` and `dbgio_write_str()` arrive. **[V]** The KOS banner
and maple enumeration arrive too — free boot diagnostics.
**[V]** Guest programs should still call `dbgio_dev_select("scif")` explicitly
at startup so the channel can't be stolen by a future `fb`/dcload selection.

### Serial throughput is *emulated-baud limited* — and this matters [V]

Measured inside the guest with `timer_us_gettime64()`:

| SCIF baud | 7200 bytes of `printf` cost, in **emulated** µs |
|---|---|
| KOS default (57 600) | **1 246 075 µs** (1.25 s — ≈ 5.8 KB/s) |
| `scif_set_parameters(1562500, 1); scif_init();` | **47 900 µs** (≈ 150 KB/s) |

1.25 s matches 7200 bytes × 10 bits ÷ 57600 exactly, i.e. Flycast models SCIF
baud faithfully and KOS busy-waits on the TX FIFO. At the default baud, ~190
bytes of logging consumes an entire 30 fps frame budget.

**Design rule:** the DC build must raise SCIF to 1 562 500 baud
(`SCBRR2 = 0`, the SH-4 maximum: 50 MHz / 32) behind a harness/debug flag
before any logging, and must never emit high-volume serial inside a timed
measurement window. **[U]** 1.5 Mbps is emulator-only; a real coder's cable
will not sync there, so keep the baud a build/settings switch, not a constant.

### PTY variant [V]

`-config config:Debug.SerialConsoleEnabled=yes -config config:Debug.SerialPTY=yes`
→ stdout stays empty and Flycast logs to stderr:
`hw/sh4/modules/serial.cpp:491 N[BOOT]: Pseudoterminal is at /dev/ttys000`.
The harness can parse that line and open the pty for **bidirectional** serial
— i.e. host → guest bytes. That is the transport for a future in-guest command
REPL (drive a test scenario without touching the GUI). Each instance gets its
own `/dev/ptmx` slave, so it is parallel-safe. Only the "a pty appears and is
named on stderr" part is verified; no round-trip byte was pushed guest-ward.

---

## 4. Screenshots

### Host-side capture is BLOCKED on this machine [V]

- `screencapture -x -t png out.png` → `could not create image from display`
  (Screen Recording TCC permission not granted to the agent's terminal).
- `osascript -e 'tell application "System Events" …'` → **hangs** (blocked on
  the Accessibility TCC prompt), killed at 10 s.

So the Flycast built-in screenshot (default hotkey **F12**, `EMU_BTN_SCREENSHOT`,
saved as `~/Pictures/Flycast-<ISO8601>.png` via `hostfs::saveScreenshot`) is
**not reachable unattended** — there is no way to synthesise the keypress.
Unlocking it requires a human to grant Screen Recording + Accessibility to the
terminal app in System Settings. Treat host-side capture as a manual-only path.

### Guest-side framebuffer dump — WORKS, use this [V]

The guest owns the framebuffer, so it can hash and describe it and ship the
result over the console channel. Verified end-to-end:

```
FBHASH 5ea3f8c5
FBTHUMB16x12 AChQeKDI8BhAaJC44AgwWAEpUXmhyfEZQWmRueEJMVkCKlJ6osryGkJq…
```

- **[V]** `vid_set_mode(DM_640x480, PM_RGB565)`, write pattern to `vram_s`,
  read it back, FNV-1a over 640×480×2 → stable hash.
- **[V]** A 16×12 sample of the framebuffer, base64'd, is ~256 chars ≈ 45 ms
  emulated at high baud — cheap enough to emit per checkpoint.
- **[V] Both the hash and the thumbnail were byte-identical across separate
  runs and across 3 parallel instances.** Golden-image regression testing on a
  hash is therefore viable, no image files needed.
- **[U]** Not yet done with real PVR output (TA-rendered scene rather than a
  CPU-written framebuffer). PVR renders into VRAM and the front buffer is
  readable the same way, so this should carry over — verify at M2.
- If a real PNG is ever needed, emit the full framebuffer as base64 over the
  high-baud serial link (640×480×2 = 614 KB ≈ 4 s emulated) and have the host
  decode it. Slow but permission-free.

---

## 6. Crash triage — works without GDB [V]

An illegal instruction (`.word 0xfffd`) in the guest produced, on stdout:

```
Unhandled exception: PC 8c010226, code 1, evt 0180
 R0-R7:  00000000 00000000 ffffffff 00002889 8c07be48 8c07bdf0 00000000 00002889
 R8-R15: 8c038910 8c037410 8c04de00 00000000 00000000 00000000 ffffffff 8cffffec
 SR 40000101 PR 8c010226
Stack Trace: frame pointers not enabled!
kernel panic: unhandled IRQ/Exception
arch: aborting the system
```

and `sh-elf-addr2line -f -C -e prog.elf 0x8c010226` resolved it to
`main` / `prog.c:8`. **[V]** That is a complete crash-triage loop with no
debugger at all: fault PC + all registers + `evt` code, symbolised on the host.

Two notes:

- **[V]** Build guest code with `-g` and, for usable stack traces, with frame
  pointers (`-fno-omit-frame-pointer`) — KOS printed "frame pointers not
  enabled!".
- **[V] CAUTION — Flycast does NOT trap unaligned accesses.** A deliberately
  misaligned 32-bit read (`*(volatile u32*)(buf+1)`) executed silently and
  returned data, under **both** the dynarec and the interpreter
  (`Dynarec.Enabled=no`). Real SH-4 raises an address error here. This is
  exactly the failure class `PLAN.md` §3.2 predicts from `-O2` on the decomp
  (the base port's ARM SIGBUS). **Flycast cannot find it.** Alignment triage
  needs real hardware, or a more accurate emulator, or host-side detection
  (build the same TUs for the PC port under `-fsanitize=alignment` / UBSan and
  triage there). Record this against the M3 gate.
- **[V]** A write to address `0x00000000` did *not* fault either (it lands in
  the boot ROM area with the MMU off). Don't use null-deref as a crash canary.

---
