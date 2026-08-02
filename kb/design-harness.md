# Emulator test-harness design (macOS host, Flycast) — split index

Recon pass 2026-08-01. Everything below is tagged **[V]** = verified by running
it on this machine today, or **[U]** = unverified / inferred from source only.
Where something did not work, it says so.

**Bottom line: console capture is GO.** A KOS `printf` inside the emulated
Dreamcast lands on the host process's **stdout**, in real time, with no
permissions, no GUI interaction and no extra tooling. Round-trip
"build ELF → boot in Flycast → read program output → exit" measured at
**~1 second wall clock**, bit-for-bit deterministic across runs and across
parallel instances. Agents can self-test.

⚠️ §12/§13 (`kb/design-harness-corrections.md`) were written later the same day
against the real `opencrossing-dc:sdk` image and **correct parts of §3, §5, §8
and §8(a)** — read them alongside whichever part you load.

| part | sections | contents |
|---|---|---|
| `kb/design-harness-flycast-setup.md` | §1, §2, §7, §10 | install + quarantine, the exact invocation and config keys, what is compiled out of the macOS build, host-environment gotchas |
| `kb/design-harness-capture.md` | §3, §4, §6 | console capture over SCIF, serial baud cost, framebuffer hash/thumbnail, crash triage without GDB |
| `kb/design-harness-runner.md` | §5, §8 | the marker-driven Python runner, and the `smoke`/`console`/`screenshot`/`perf`/`crash` harness contracts |
| `kb/design-harness-alternatives.md` | §9 | lxdream-nitro / Nitrocast and RetroArch — evaluated, NO-GO today |
| `kb/design-harness-corrections.md` | §11, §12, §13 | verified-claims index, plus everything the real build changed (`scif_flush()`, fail-marker regex, CDI boot, KOS banner, perf tolerance band) |

The living operational doc is `harness/dc/README.md`; this tree is the design
rationale behind it.
