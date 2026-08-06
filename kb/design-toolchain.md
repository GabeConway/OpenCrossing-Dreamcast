# Dreamcast toolchain — design & measurements (M0)

Written 2026-08-01 by the toolchain recon pass. Everything below is tagged
**[VERIFIED]** (I ran it on this host and read the output) or **[UNVERIFIED]**
(inferred, extrapolated, or read from docs but not executed). Do not upgrade an
UNVERIFIED claim without re-measuring.

---

**This file is now an index.** Content was moved verbatim, tags intact; old
section numbers are kept so existing citations (e.g. `BUILDING-DC.md` §2, §5.2)
still resolve. For how to actually run a build, read `BUILDING-DC.md`.

⚠️ **2026-08-06 — the optimization level these docs assume is gone.** Every
file below was written while `src/` was pinned at `-O0`. It now builds at
**`-Os` with a 14-TU `-O3` hot list** (`DC_OPT_PROFILE=perf`, the default;
`size` = `-Os` everywhere; `o0` = byte-identical revert), and `dc/src` moved
`-O2` → `-O3`. The per-TU lists live in `dc/opt-lists.mk` and the guard set
`OPT_GUARDS` lives in `dc/Makefile`. Measured: `.text` **5,506,964 →
2,753,700**, town FPS **11.6 → 20.6**, full rebuild **96 s**. Flag assembly is
documented in `BUILDING-DC.md`, not here; the toolchain *facts* below (GCC
15.2.0, newlib 4.6.0, KOS 2.3.0, `-m4-single-only`, float ABI, signed `char`)
are unaffected. Reasoning: `kb/state-log.md`, 2026-08-06 entry.

| file | old §§ | what it answers |
|---|---|---|
| `kb/toolchain-decision.md` | §0, §8, §9 | why build from source with `kos-chain`, what was rejected and why, fallbacks F1–F5, open items |
| `kb/toolchain-host-env.md` | §1, §2, §10 | colima/Docker facts, the `$HOME`-only bind-mount trap, the qemu 17–23×/ICE measurement, how to reproduce it |
| `kb/toolchain-components.md` | §3, §4 | upstream state, float ABI, signed `char`, and the recipe for kos-chain / KOS / GLdc / mkdcdisc |
| `kb/toolchain-dockerfile.md` | §5 | the two-stage Dockerfile with pinned SHAs, what the built image was proven to do, mkdcdisc `-N` padding |
| `kb/toolchain-build-invocation.md` | §6, §7 | measured image build times, and the bind-mount / container invocation design |
