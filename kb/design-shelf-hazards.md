# sh-elf compile hazards for `src/` (M1 recon) — split index

Written 2026-08-01. **Everything marked VERIFIED below was measured**, not
reasoned about: `sh-elf-gcc 9.3.0` from the `einsteinx2/dcdev-kos-toolchain`
image (KOS `525cbda`, newlib 3.3.0), run against this repo's actual `src/`
tree. Items marked UNVERIFIED say so explicitly.

⚠️ **Read the "Superseded in part" warning that heads every part before
trusting anything here.** This document was measured on GCC 9.3.0 / newlib
3.3.0 / KOS `525cbda`, not the GCC 15.2.0 / newlib 4.6.0.20260123 / KOS 2.3.0
(`1c6398f9`) the project actually builds with, and it missed both header
collisions that really bit us. The warning is reproduced verbatim at the top
of each file below.

⚠️ **Two of this document's instructions violate `CLAUDE.md` §1 and must not be
followed as written.** §8 Step 0 says to add a definition to
`src/static/JSystem/JUtility/JUTFont.cpp`, and §2.4 says to add an `#include`
to `include/JSystem/JUtility/JUTFont.h`. **`src/` is never edited to make
something compile** — every compat fix goes in `dc/include/dc_prelude.h`, which
is force-included, and all 3917 TUs build that way today with zero exclusions.
Read those steps as "here is the collision", not as "here is the fix".

⭐ **2026-08-06 — the `-O2` half of that warning is REVERSED, and this document
was right.** The sentence that used to end the paragraph above read *"Similarly,
§3.4 and §9 recommend building at `-O2`; codegen flags are banned by user
directive."* The ban is gone: `src/` builds at `-Os` with a 14-TU `-O3` hot
list (`dc/opt-lists.mk`), `.text` fell **5,506,964 → 2,753,700** and town FPS
rose **11.6 → 20.6**. §9's verdict ("achievable, and probably mandatory") and
§3.4's size extrapolation were both correct and were overruled by one
unreproduced armhf session. Full note at the top of `kb/design-shelf-flags.md`;
evidence in `kb/state-log.md`, 2026-08-06 entry. The `src/`-editing half of the
warning still stands in full.

| part | sections | contents |
|---|---|---|
| `kb/design-shelf-exclusions.md` | §0, §1, §2 | the 12 TUs that fail, `pc/CMakeLists.txt` read in full, the two `-O2` confounds (**both re-derived and upheld 2026-08-06**), the DC exclusion/fix list |
| `kb/design-shelf-flags.md` | §3, §7, §9 | the flag set justified per flag, SH-4 GCC probe findings, the `-O2` risk verdict — **✅ vindicated 2026-08-06; read its header first** |
| `kb/design-shelf-alignment.md` | §4, §8 | SH-4 alignment exposure, the ranked hazard sites, the `LD32U` fix idiom, miscompile triage procedure |
| `kb/design-shelf-abi-libc.md` | §5, §6, §10 | inline asm, 32-bit `double`, attributes/pragmas/`register`, the newlib gap list, corrections to other docs |
