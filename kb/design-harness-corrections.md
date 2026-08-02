# Flycast harness — verified-claims index and later corrections

The index of what was and was not verified by execution (§11), then the
corrections found while actually building the harness against the real
`opencrossing-dc:sdk` image — KOS 2.3.0 / GCC 15.2.0, not the 2021 image the
rest of the design was measured on (§12, §13). **Read this before trusting any
other part of `kb/design-harness.md`**, whose stub maps every § to its file.

Recon pass 2026-08-01. Everything below is tagged **[V]** = verified by running
it on this machine today, or **[U]** = unverified / inferred from source only.
Where something did not work, it says so.

## 11. Verified-claims index (so nothing here is taken on trust)

Verified by execution today: cask install + quarantine strip; v2.6 == upstream
tag `392a429e8`; `-config section:key=value` grammar; direct `.elf` boot via
reios; hermetic `HOME` via pre-created `$HOME/.flycast/`; 3-way parallel runs;
`Debug.SerialConsoleEnabled` → stdout with KOS banner + `printf` +
`dbgio_write_str`; Flycast logs on stderr and `log:LogToConsole=no`;
`Debug.SerialPTY` → `/dev/ttys000` named on stderr; SCIF baud cost 57 600 vs
1 562 500 (1 246 075 µs → 47 900 µs for 7200 B); determinism of guest timer,
`FBHASH`, and thumbnail across runs and instances; illegal-instruction →
KOS register dump on stdout → `sh-elf-addr2line` symbolisation; unaligned
access **not** trapped (dynarec *and* interpreter); write to `0x0` not trapped;
GDB port 3263 not listening + no RSP strings in binary; Lua not present and
`flycast.lua` never executed; `SDL_VIDEODRIVER=dummy/offscreen` fail (exit 6);
`screencapture` and `osascript`/System Events both blocked by TCC; marker
runner ok/fail/timeout paths; Nitrocast builds on macOS arm64 with 3 patches
and then SIGBUSes; stale Command Line Tools (clang 12.0.5 / SDK 11.3) on
macOS 26.5.2; colima `/private/tmp` mount failure; `einsteinx2` image contents.

Not verified: CDI boot (no CDI exists yet); PVR-rendered framebuffer readback;
guest→host bytes over the PTY; KOS `gdb_init()` stub over SCIF; any
from-source Flycast build; RetroArch/libretro headless; deecy; the absolute
accuracy of any timing number relative to real SH-4 silicon.

---

## 12. Corrections from building the harness (2026-08-01, later the same day)

The scripts in §8 were written and run end to end against real CDIs built from
`harness/dc/selftest/` with the real `opencrossing-dc:sdk` image
(KOS **2.3.0**, git `1c6398f`, GCC **15.2.0**) — not the 2021 `einsteinx2`
image everything above was measured on. Four things changed.

### 12.1 CDI boot works — §8(a)'s **[U]** is now **[V]**

`mkdcdisc -q -N -e prog.elf -o prog.cdi` → Flycast boots it through `IP.BIN`
exactly as predicted. Guest serial, maple enumeration, `FBHASH` and the crash
dump all behave identically to the ELF path. Cost: **~3.1 s** per run with
`config:FastGDRomLoad=yes`, versus **~1.0 s** for the same program booted as a
raw `.elf`. Keep using ELF boot for iteration; use CDI for the gate.

### 12.2 **`scif_flush()` from guest code silently kills the console** [V]

This is the big one, and it is a trap the §3 design walks straight into.
KOS's `scif_flush()` clears TEND (`SCFSR2 &= 0xbf`) and then spins up to
800 000 iterations waiting for TEND to come back. **Flycast never re-raises
TEND on an already-idle TX FIFO**, so the spin always times out, and on timeout
KOS latches `serial_enabled = 0` (`hardware/scif.c`) — permanently disabling
*all* further serial output, including the panic path.

Consequence: a guest that calls `scif_flush()` and then crashes produces **no
`Unhandled exception` block at all**. The run can only end as a wall-clock
timeout, with no triage information. Isolated by bisection over seven guest
variants:

| guest does | crash dump reaches host? | run ends as |
|---|---|---|
| nothing special | yes | `fail_marker`, 1.6 s |
| `scif_set_parameters(1562500,1); scif_init()` | **yes** | `fail_marker`, 1.6 s |
| `scif_flush()` (any baud, with or without `scif_init`) | **no** | `timeout` |

So raising the baud (§3) is safe and stays; the explicit flush must go.
`printf` → `scif_write_buffer` already flushes internally on a FIFO that has
just been written, and that path succeeds. Both `selftest.c` and `crashtest.c`
were fixed. **Rule: guest code never calls `scif_flush()`.**

### 12.3 The §5 fail-marker regex misses KOS assertion failures [V]

KOS 2.3 prints `*** ASSERTION FAILURE ***` / `Assertion "x" failed at f.c:9`
(capital A) and then `arch: aborting the system`. The regex in §5 tests for
lowercase `assertion "`, so a failed guest `assert()` was caught only by the
timeout. `_runner.py`'s `DEFAULT_FAIL_MARKER` now also matches
`ASSERTION FAILURE` and `arch: aborting the system`.

### 12.4 The KOS banner changed shape [V]

2021 image: `KallistiOS Git revision 525cbda:` on one line.
KOS 2.3.0: `KallistiOS v2.3.0 [dreamcast/pristine]` then `  Git revision: 1c6398f`
on the next. Any assertion that greps for the old single-line form silently
fails. `_runner.py` now parses both and reports `kos_version` separately.

### 12.5 Minor

- KOS 2.3 also enumerates two VMUs (`A1`, `A2`) alongside the controller; the
  §8(a) assertion should key on `A0: Dreamcast Controller` only.
- Guest `PERF` timings are reproducible but **not bit-identical**: the 1 M
  iteration loop measured 35085 / 35090 µs across runs, not one fixed value.
  A regression gate needs a tolerance band, not equality.
- Crash triage confirmed on the new toolchain: PC `8c010112` →
  `sh-elf-addr2line -f -C -i -e crashtest.elf` → `main` / `crashtest.c:39`,
  the illegal instruction. KOS 2.3 additionally prints a ready-made
  `$KOS_ADDR2LINE …` command line with the PC, PR and unwound stack.

---

## 13. Building `crash.sh` and `perf.sh` (§8d/§8e), same day

### 13.1 §8e's crash path is fully automated and needs no Flycast rebuild [V]

`harness/dc/crash.sh` parses the KOS dump out of a console log and symbolises it
with `sh-elf-addr2line` + `sh-elf-objdump` inside `opencrossing-dc:sdk`.
Verified: `crashtest.cdi` → `Illegal instruction`, PC `8c010112` →
`main at /src/crashtest.c:39`, stack `arch_main at …/init.c:319`, plus a
disassembly window with `>> 8c010112: fd ff .word 0xfffd` marked. `smoke.sh`
runs it automatically on any failure whose console holds a dump. **The GDB path
in §8e is not needed for this class of bug at all** — deprioritise the
from-source Flycast build accordingly (it is still the only route to frame-exact
scripted control and raw video dump).

### 13.2 ELF provenance is a hard requirement, not a nicety [V]

A CDI holds a scrambled, stripped `1ST_READ.BIN`, so nothing in the image says
which ELF produced it. Every CDI producer now writes `<image>.src.json` beside
the image recording the ELF's absolute path, sha256 and size;
`selftest/build.sh` does it, and **`dc/build-dc-docker.sh` must too**.
`crash.sh` refuses to symbolise when the sidecar is missing, or when the ELF on
disk no longer matches the recorded hash — verified both refusals, plus the
happy path after restoring the original ELF. A confidently wrong line number is
worse than no answer.

### 13.3 Two container gotchas that cost real time [V]

- **`bash -lc` breaks the toolchain PATH.** `opencrossing-dc:sdk` bakes
  `/opt/toolchains/dc/sh-elf/bin` into the image `ENV`, but a *login* shell
  re-runs `/etc/profile`, which rebuilds `PATH` and drops it. `sh-elf-addr2line`
  then comes back "command not found" and every address symbolises to `??`.
  Use `bash -c`. (`selftest/build.sh` gets away with `-lc` only because it
  explicitly sources `environ.sh`.)
- **Sourcing `environ.sh` under `set -u` kills the shell** — unbound variable,
  exit 127, nothing on stderr. Don't add it "for safety".
- Bash parses backticks inside `$( … )` **even in a quoted heredoc**, so a
  Python regex containing a literal backtick (KOS writes ``failed at f.c:9 in
  `main'``) makes the whole script an unterminated-quote syntax error. Build it
  with `chr(96)`.

### 13.4 §8d's regression gate: use a band, never equality [V]

The §8d claim that guest timing is "exactly reproducible" is very nearly true
but not literally so: the 1 M-iteration loop measured **35085 µs** on most runs
and **35090 µs** on one — 0.014% spread. `perf.sh` therefore gates on
**max(2% of baseline, 100 µs)**, ~140× the observed jitter and still far tighter
than anything that matters against a 33 333 µs frame budget. The absolute floor
is there because 2% of a 40 µs metric is noise. Verified: passes unchanged
(±0.000%), fails a baseline shifted to 30000 (+16.95%), passes one shifted to
34600 (+1.4%, inside the band), fails one at 34000 (+3.2%, outside it), and
treats a faster-than-baseline result as an `improvement` (non-fatal without
`--strict`, a `regression` under `--lower-is-worse`).

Also: `PERF` lines carrying checksums rather than measurements (selftest's
`acc=`) need `--exact`, so prefer emitting checksums as `MEM` or `ASSERT`
records instead.
