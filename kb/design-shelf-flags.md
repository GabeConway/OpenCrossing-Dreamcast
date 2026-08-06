# sh-elf compile hazards — the flag set, GCC probe facts, and `-O2`

⚠️ **Two of this document's instructions violate `CLAUDE.md` §1 and must not be
followed as written.** §8 Step 0 says to add a definition to
`src/static/JSystem/JUtility/JUTFont.cpp`, and §2.4 says to add an `#include`
to `include/JSystem/JUtility/JUTFont.h`. **`src/` is never edited to make
something compile** — every compat fix goes in `dc/include/dc_prelude.h`, which
is force-included, and all 3917 TUs build that way today with zero exclusions.
Read those steps as "here is the collision", not as "here is the fix".

## ⭐ 2026-08-06 — §9 WAS RIGHT, AND IT WAS OVERRULED FOR NOTHING

The sentence that used to sit here read: *"Similarly, §3.4 and §9 recommend
building at `-O2`; codegen flags are banned by user directive. (Noted
2026-08-02 while splitting this document.)"* **That ban is gone.** `src/` now
builds at **`-Os`** with a 14-TU `-O3` hot list (`DC_OPT_PROFILE=perf`, the
default; lists in `dc/opt-lists.mk`), and `dc/src` moved `-O2` → `-O3`.

Measured on this tree, sh-elf GCC 15.2 (`kb/state-log.md`, 2026-08-06 entry):

| | `-O0` | `-Os` + `-O3` hot list |
|---|---:|---:|
| `.text` | 5,506,964 | **2,753,700** |
| town FPS | 11.6 | **20.6** |
| `draw` ms | 79.1 | **45.4** |
| µs/vertex | 4.05 | **3.11** |

**This is the lesson, not a gloat.** §9 below concluded `-O2` was *"achievable,
and probably mandatory"* and gave five arguments, two of which — the confounded
ARM evidence (§1.2) and "SH-4 has no 64-bit load to merge" (§4.1) — are exactly
what the 2026-08-06 audit re-derived from scratch. §3.4's extrapolation
("`.text` ≈ 5.7 MB at `-O0` → ≈ 2.9 MB at `-O2` → ≈ 2.6 MB at `-Os`") was made
from a 648-TU sample on GCC 9.3, and the GCC 15.2 whole-tree measurement landed
squarely inside that bracket. The document was overruled by a single armhf session
(2026-07-13) that was never reproduced on SH-4 and never isolated from a
simultaneous `-mcpu=cortex-a53 -mfpu=neon-vfpv4` change. **When a measured
recommendation is overridden by an anecdote from a different ISA, record the
disagreement instead of deleting it** — that is the only reason this file was
still here to be vindicated.

⚠️ One thing §9 did *not* get right: it recommended plain `-O2` with a per-TU
downgrade list. What shipped is `-Os` + a *hot* list, plus a guard set §9 never
names — `OPT_GUARDS` = `-fno-isolate-erroneous-paths-dereference`,
`-fno-ipa-icf`, `-fno-ipa-sra`, `-fno-store-merging`. The last one exists
because of the store-merging/alignment analysis in §4.1
(`kb/design-shelf-alignment.md`), read together with §7's alignment-trap
semantics below: that analysis was measured on GCC 9.3 and never re-probed on
GCC 15.2, so the flag is carried instead of the bet.

---

The per-flag justified compiler/linker flag set (§3), the consolidated SH-4
GCC findings and probe output (§7), and the risk verdict on `-O2` (§9). Read
before changing any flag in `dc/Makefile`.
Part of `kb/design-shelf-hazards.md`, whose stub maps every § to its file.

Written 2026-08-01. **Everything marked VERIFIED below was measured**, not
reasoned about: `sh-elf-gcc 9.3.0` from the `einsteinx2/dcdev-kos-toolchain`
image (KOS `525cbda`, newlib 3.3.0), run against this repo's actual `src/`
tree. Items marked UNVERIFIED say so explicitly.

> **Toolchain caveat.** All compiler measurements used **GCC 9.3.0**. PLAN §8
> targets dc-chain **GCC 14+** per sm64-dc. ABI facts (`__BIGGEST_ALIGNMENT__`,
> `__SIZEOF_DOUBLE__`, char signedness, absence of 64-bit load/store) are
> stable across GCC versions. Codegen-detail claims (store merging, memcpy
> inlining) must be re-checked once the real M0 container exists.

> ## ⚠️ Superseded in part — read this before trusting anything below
>
> **Updated 2026-08-01.** The real M0 container now exists and is *not* the
> image this document measured: `opencrossing-dc:sdk` is **GCC 15.2.0 /
> newlib 4.6.0.20260123 / KOS 2.3.0 (`1c6398f9`)**, versus GCC 9.3.0 / newlib
> 3.3.0 / KOS `525cbda` here. All 3917 TUs compile and link against it with
> **zero exclusions and no edits to `src/`**, so this document's exclusion
> lists are pessimistic. Known divergences:
>
> - **`-fno-builtin` (§3.1) is falsified** — it was marked "(VERIFIED)" as KOS
>   convention and it breaks the link. See the flag table.
> - **The §2.3 header-collision scan missed the two collisions that actually
>   bit us**, because newer KOS/newlib headers introduce them: KOS
>   `dc/fmath.h:109`'s `static inline float fsqrt(float)` versus the decomp's
>   `math64.h:34` `#define fsqrt(x) sqrtf(x)`, and POSIX `link()` versus the
>   decomp's `typedef struct link_ link` arriving through `<stdio.h>` →
>   `<sys/stdio.h>` → `<unistd.h>`. Both are fixed in
>   `dc/include/dc_prelude.h`; the second cannot be fixed with a blanket
>   `-Dlink=`, which renames both sides.
> - **Char signedness (§2.x):** this image defaults to **SIGNED**, so
>   `-fsigned-char` is belt-and-braces, not load-bearing.
> - ~~**Optimization flags anywhere below are moot** — `src/` builds at `-O0`
>   by user directive (CLAUDE.md, PLAN §3.2).~~ **[STALE 2026-08-06 — the
>   opposite is now true.]** `src/` builds at `-Os` plus a 14-TU `-O3` hot
>   list. Every optimization claim below is back in scope; re-measure it on
>   GCC 15.2 rather than trusting the GCC 9.3 number. `kb/state-log.md`
>   2026-08-06.
>
> Treat the ABI facts as durable and every codegen-detail claim as unverified
> until re-measured on GCC 15.2.

---

## 3. Recommended flag set, justified per flag

All flags below were **individually VERIFIED to be accepted by `sh-elf-gcc
9.3.0`** (compile test, rc=0).

### 3.1 Global, every TU

| Flag | Why |
|---|---|
| `-ml -m4-single-only` | Mandatory. KOS's own `environ_dreamcast.sh` sets exactly this (VERIFIED), and libc/libm/libkallisti are built with it. Deviating breaks ABI compatibility with the prebuilt libraries. |
| `-fno-strict-aliasing` | The decomp type-puns constantly (`*(u64*)&this->gfx`, `*(float*)&int`). Upstream `4f428276` proves this is one of the two flags that made `-O2` viable on x86. Already global in the base repo. |
| `-fwrapv` | Signed-overflow UB. Second half of upstream's `-O2` fix. |
| `-fno-delete-null-pointer-checks` | Base-repo guard: decomp dereferences then null-checks; GCC would delete the check. |
| `-fno-lifetime-dse` | Base-repo guard: dead stores into objects about to die are load-bearing in JSystem ctors/dtors. |
| `-fno-aggressive-loop-optimizations` | Base-repo guard: decomp loops index past declared array bounds. |
| `-fno-strict-overflow` | Base-repo guard: pointer-arithmetic overflow. |
| `-fno-stack-protector` | Base-repo flag; also KOS has no `__stack_chk_fail` guarantee for game TUs. |
| ~~`-fno-builtin`~~ | **FALSIFIED 2026-08-01 — do not use.** This row claimed KOS convention "(VERIFIED)". It is not: `$KOS_CFLAGS` in `opencrossing-dc:sdk` does not contain it, and adding it **breaks the link** — `m_select.c:936,993` then emit calls to a real `alloca` that newlib does not provide, and there is no `-fbuiltin-alloca` to re-enable just that one. The original rationale (stopping GCC from turning hand-rolled `Jac_bcopyfast`-style loops into `memcpy` under stronger alignment assumptions) is real but does not justify an unlinkable binary; handle that class by alignment triage instead. |
| `-ffunction-sections -fdata-sections` (+ `-Wl,--gc-sections`) | KOS default. Directly attacks PLAN §3.1's ≤4 MB binary target. |
| `-fsigned-char` | **Not strictly required.** VERIFIED: `sh-elf-gcc` defaults to **signed** char (`(char)-1 < 0` is true; `__CHAR_UNSIGNED__` is undefined). This **corrects PLAN §8 and kb/base-repo-map.md**, both of which state SH-4 GCC chars are unsigned. Keep the flag anyway — zero cost, documents intent, survives a toolchain swap. |
| `-DTARGET_PC` **and** `-DTARGET_DC` | **Critical.** `#ifdef TARGET_PC` in `src/` guards the base port's little-endian correctness fixes — byte-wise texconv in `emu64.c:840-864`, the swapped `u16` pair ordering in `sys_matrix.c:_MtxF_to_Mtx`/`Matrix_MtxtoMtxF`, the overlap-safe `Jac_bcopy` in `sample.c:33`. `TARGET_PC` here means "not GameCube", not "PC". Dropping it silently reintroduces big-endian assumptions. Add `TARGET_DC` for genuinely DC-specific branches. |

**Added 2026-08-06 — `OPT_GUARDS`, the flags this table could not have known
it needed.** When `src/` stopped being `-O0`, four more went in (`dc/Makefile`):
`-fno-isolate-erroneous-paths-dereference` (stops GCC turning a tolerated NULL
deref into a trap — the "wild pointer" shape the armhf ban was blamed on),
`-fno-ipa-sra` (the decomp calls 968 K&R `()`-declared functions, some *with*
arguments), `-fno-store-merging` (§7: SH-4 traps any misaligned store —
this is §4.1's analysis turned into a flag), `-fno-ipa-icf` (keeps crash
addresses unambiguous during triage). The row above justifying
`-fno-strict-aliasing`/`-fwrapv` by "upstream's `-O2` fix" is no longer a
hypothetical: those two are now doing that job on this target too.

### 3.2 Explicitly NOT used

| Flag | Why not |
|---|---|
| `-mdalign` | Aligns doubles to 64 bits and **changes calling conventions** — GCC's own docs warn the standard library must be rebuilt. Prebuilt newlib/KOS are not. And `double` is 32-bit anyway (§5.2), so it buys nothing. |
| `-mfmovd` | Enables 64-bit `fmov.d` register-pair moves, which **require 8-byte alignment on a 4-byte-max-alignment ABI**. This is precisely the ARM LDRD hazard class, voluntarily re-imported. Off by default (VERIFIED); keep it off. |
| `-ffast-math` | Breaks the exact 32-bit float semantics the GameCube code depends on (same reason the PC build forces `-msse2 -mfpmath=sse` on x86). |
| `-mno-unaligned-access` | **Does not exist on SH.** VERIFIED against the full `--help=target` output. SH is unconditionally `STRICT_ALIGNMENT`. |
| `-mrelax` | Link-time address shortening; interacts badly with `--gc-sections` diagnosis during triage. Revisit at M4 for size. |

### 3.3 Per-TU, mirroring `pc/CMakeLists.txt`

```
decomp C (default) : -w -std=gnu89
jaudio_NES/*, emu64/*  : -w -std=gnu11        (+ -Dnullptr=NULL, C only)
emu64.c, ja_calc.c, jammain_2.c, game64.c : compiled as C++, -w -fpermissive
decomp C++        : -w -fpermissive -fno-exceptions -fno-rtti -fno-operator-names
src/main.c        : -Dmain=ac_entry
src/static/boot.c : -Dmain=boot_main
```

`-fno-exceptions -fno-rtti` (KOS_CPPFLAGS) are safe: VERIFIED that no kept
`.cpp` or C++-compiled `.c` uses `throw`, `try`, `catch`, `typeid`, or
`dynamic_cast`. `JKRHeap.cpp:321-350` overrides global `operator new`/`delete`
onto JKRHeap — with KOS this is fine (no interposition problem, so the base
repo's `version_script.lds` hack is unnecessary on DC).

### 3.4 Optimization level — recommendation

✅ **ADOPTED 2026-08-06, in the `-Os`-leaning form.** What shipped is `-Os`
everywhere plus `-O3` on 14 measured-hot TUs, not `-O2` everywhere. The
extrapolation below was made on GCC 9.3 from a 648-TU sample and proved
accurate on GCC 15.2 over the whole tree: predicted ≈ 2.6 MB at `-Os`,
measured **2,753,700 B** with the `-O3` hot list included (`kb/state-log.md`
2026-08-06). The per-TU control that makes it safe is `dc/opt-lists.mk`.

**Ship `-O2` for game code from day one, `-Os` for `src/data/` and cold TUs.**
Rationale beyond speed — VERIFIED size measurement on a 648-TU representative
sample (1/6 of the tree, includes `src/data`):

| | `.text` | `.data` | `.bss` |
|---|---|---|---|
| no `-O` | 945,214 | 348,436 | 469,750 |
| `-O2`   | **488,474 (−48.3 %)** | 314,671 | 469,586 |
| `-Os`   | **431,076 (−54.4 %)** | 314,671 | 469,586 |

Extrapolated ×6 for the whole tree: `.text` ≈ 5.7 MB at `-O0` → **≈ 2.9 MB at
`-O2` → ≈ 2.6 MB at `-Os`**. On a 16 MB machine, `-O0` costs roughly **3 MB of
RAM** on top of being 2–3× slower. `-O0` is not an option on Dreamcast the way
it was on a 1 GB handheld. This reframes PLAN §3.2: `-O2` is not an
optimization, it is a *budget requirement*.

---

## 7. SH-4 GCC findings (consolidated, all VERIFIED unless noted)

```
sh-elf-gcc 9.3.0, --print-multi-lib = ".;"   (single multilib: m4-single-only)
-m4, -m4-single, -m4-nofpu  → "not supported by this configuration"
__BYTE_ORDER__       = __ORDER_LITTLE_ENDIAN__
__BIGGEST_ALIGNMENT__= 4
__SIZEOF_DOUBLE__    = 4          __SIZEOF_LONG_LONG__ = 8
__SH4_SINGLE_ONLY__, __SH_FPU_ANY__, __sh__
__CHAR_UNSIGNED__    undefined  →  char is SIGNED by default
sizeof(void*)=4, alignof(long long)=4, alignof(double)=4
```

UB-guard flag acceptance (each compiled rc=0):
`-fno-strict-aliasing`, `-fwrapv`, `-fno-delete-null-pointer-checks`,
`-fno-lifetime-dse`, `-fno-aggressive-loop-optimizations`,
`-fno-strict-overflow`, `-fsigned-char`, `-fno-stack-protector`, `-Os/-O2/-O3`.
`-fpermissive` warns "valid for C++/ObjC++ but not for C" but does not error.

Alignment-trap semantics: SH-4 raises a CPU address error on any misaligned
`mov.w`/`mov.l`/`fmov.s`. KOS surfaces this as `EXC_DATA_ADDRESS_READ 0x00e0`
and `EXC_DATA_ADDRESS_WRITE 0x0100`, hookable with `irq_set_handler` /
`irq_set_global_handler`; unhandled → `dbglog(DBG_DEAD, ...)` + `arch_panic`.
Faulting address in TEA (`0xff00000c`).

No `-mno-unaligned-access` equivalent; SH is unconditionally strict.
`movua.l` (SH-4A unaligned load) is not emitted for `-m4-single-only`; the
compiler's only safe-unaligned idiom is `packed` / `aligned(1)` (byte-wise) or
an out-of-line `memcpy`.

UNVERIFIED: GCC 14 codegen may differ (newer store-merging and SLP passes).
Re-run the §4.1 probe set against the M0 container.

---

## 9. Risk verdict on `-O2`

> ✅ **CONFIRMED 2026-08-06.** Every "argument for" below survived contact with
> the real toolchain; the "arguments against" were addressed with tooling
> rather than with a ban. Specifically: (1) the whole tree still compiles, now
> on GCC 15.2; (2) both ARM confounds were re-derived independently and one of
> them — `JUTRomFont::spFontHeader_` — is **still undefined in this tree, on
> purpose**, so an optimized build that starts referencing it fails at link
> instead of dereferencing NULL; (3) argument 3 became a *flag*,
> `-fno-store-merging`, rather than a bet; (5) `.text` fell 2,753,264 B.
> Residual risk 1 (alignment) is covered by `-fno-store-merging` + the
> address-error handler; residual risk 2 (`#pragma dont_inline`) is unaddressed
> and remains the honest open item; residual risk 3 (newer GCC store-merging)
> is what the flag exists for; residual risk 4 (Flycast) is unchanged —
> **nothing in the 2026-08-06 measurement ran on hardware.**
> Detail: `kb/state-log.md` 2026-08-06.

**Achievable, and probably mandatory.** Confidence: moderate-high on
"it compiles and links"; moderate on "it runs correctly without per-TU
fallbacks".

Arguments for:
1. All 3900 TUs compile at `-O2` with the full guard set, zero ICEs (VERIFIED).
2. The **ARM `-O2` evidence is confounded twice** (§1.2) — a missing static
   member definition that upstream had to add for exactly this reason, and a
   simultaneous `-mfpu=neon-vfpv4` switch. Neither confound exists on SH-4.
3. The specific mechanism blamed for the ARM `-O1` SIGBUS — GCC merging loads
   into 64-bit `LDRD`/VFP accesses — **cannot occur on SH-4**: max alignment is
   4, there are no 64-bit integer loads, and GCC was measured *not* merging
   adjacent byte/half stores at `-O2` (VERIFIED).
4. Upstream runs `-O2` in production on x86 with only
   `-fno-strict-aliasing -fwrapv`; we carry four additional guards.
5. `-O2` cuts `.text` by 48 % (VERIFIED). On 16 MB that is ~3 MB of RAM. `-O0`
   is arguably not affordable regardless of speed.

Arguments against / residual risk:
1. **The 16/32-bit alignment class is entirely untested by every prior port.**
   ARM services those in hardware; x86 always has. SH-4 will not. This risk is
   *independent of `-O2`* — it bites at `-O0` too — but `-O2` increases the
   chance of hitting it (more aggressive reassociation of address expressions,
   more inlining exposing cross-function alignment assumptions). Hazards #1–#5
   in §4.3 are in per-frame or per-vertex paths.
2. `#pragma dont_inline` is silently ignored by GCC; any TU relying on it for
   correctness changes behaviour at `-O2`.
3. GCC 9.3.0 was measured; GCC 14's store-merging and SLP passes are more
   aggressive and could reintroduce a merged-access hazard. Re-verify.
4. If Flycast does not trap misalignment, the feedback loop for this bug class
   runs at CD-R burn speed, not emulator speed. **This is the schedule risk,
   not the technical one.**

**Recommendation:** build `-O2` from M1, with the address-error handler in from
M0 and a per-TU downgrade list ready. Do not repeat the base repo's decision to
pin everything at `-O0` — on this hardware that trades a solvable correctness
problem for an unsolvable budget problem.

---
