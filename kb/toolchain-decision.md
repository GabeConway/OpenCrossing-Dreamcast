# Dreamcast toolchain — the decision, the rejected options, the fallbacks

Why the SDK is built from source with KOS's own `kos-chain` into
`opencrossing-dc:sdk`, what was rejected and the measurement that killed each,
the five ranked fallbacks (F1–F5), and the open items left for the next agent.
Read first when questioning or changing the toolchain choice. Tag discipline (**[VERIFIED]** = ran it on this host and read the output, **[UNVERIFIED]** = inferred or read from docs) is unchanged from `kb/design-toolchain.md`; do not upgrade an UNVERIFIED claim without re-measuring.

## 0. Decision (TL;DR)

**Build the toolchain from source with KOS's own `kos-chain`, natively for
linux/arm64, inside a Debian bookworm container.** Bake toolchain + KallistiOS +
GLdc + mkdcdisc into one image `opencrossing-dc:sdk`; bind-mount the repo at
`/work` and invoke `dc/build-dc-docker.sh`, exactly mirroring the base repo's
`pc/build-armhf-docker.sh` pattern.

> **This is not a proposal — it was executed.** The Dockerfile in §5 was built on
> this host (stage 1: 24 min, stage 2: 2.5 min, both exit 0). The resulting image
> compiles and links C, C++/libstdc++ and GLdc for sh-elf and produces a bootable
> CDI (§5.1). **Both images already exist in the local Docker daemon**, so the
> next agent can start using `opencrossing-dc:sdk` immediately.
>
> Remaining risk is not "does the toolchain work" but "does the *decomp* build
> with GCC 15.2.0" — a separate question, gated at M1.

Rejected alternatives, with the measurement that killed each:

| Option | Verdict | Why |
|---|---|---|
| `einsteinx2/dcdev-kos-toolchain` (arm64) | **fallback only** | Native arm64 and *works today*, but image built **2021-03-15**: GCC 9.3, KOS master-of-2021, `m4-single-only` ABI, no mkdcdisc. [VERIFIED] |
| `nold360/kallistios-sdk` | **rejected** | Publishes **amd64 only** — no arm64 manifest. [VERIFIED] |
| Any amd64 image under qemu | **rejected** | **17–23× slower** *and* throws **nondeterministic ICEs**. [VERIFIED] |
| Prebuilt `drpaneas` linux-aarch64 tarball | **fallback** | Real, GCC 15.1.0 + KOS 2.2.1, 470 MB. Third-party, pinned to a KOS *tag*. [VERIFIED it exists; UNVERIFIED that it runs] |

---

## 8. Fallback plan

Ordered. Each is independently sufficient to unblock M0.

**F1 — `einsteinx2/dcdev-kos-toolchain:latest`, arm64. Works today; verified.**

```bash
docker pull --platform linux/arm64 einsteinx2/dcdev-kos-toolchain:latest
docker run --rm --platform linux/arm64 -v "$PWD":/src \
  einsteinx2/dcdev-kos-toolchain:latest    # entrypoint sets KOS env
```

I compiled and linked a KOS hello-world in it: [VERIFIED]

```
. /opt/toolchains/dc/kos/environ.sh
kos-cc -o hello.elf hello.c
  ->  text 242208  data 12476  bss 185148   (hello.elf, 2.5 MB unstripped)
```

`libkallisti.a` (5.1 MB) and `kos-ports/lib/libGL.a` (GLdc) are both present and
prebuilt. Caveats, all [VERIFIED]: image dated **2021-03-15**; GCC **9.3.0**;
`__SH4_SINGLE_ONLY__` is defined, i.e. **`m4-single-only` — doubles truncate to
float**; `KOS_SH4_PRECISION` is unset (the variable postdates the image); no
mkdcdisc (but `cdi4dc`/`makeip`/`scramble` are there, and mkdcdisc builds
separately in a plain Debian arm64 container in ~1 minute, per §4.4). Good enough
to prove `harness/dc/smoke.sh` end to end; **not** good enough to build the game
on (5-year-old KOS, wrong float ABI).

**F2 — prebuilt `drpaneas/dreamcast-toolchain-builds`, `linux-aarch64`.**
Release `gcc15.1.0-kos2.2.1` (2025-12-29) publishes
`dreamcast-toolchain-gcc15.1.0-kos2.2.1-linux-aarch64.tar.gz` (470 MB, with
`.sha256`). GCC 15.1.0 + KOS **v2.2.1**. [VERIFIED the asset exists via the
GitHub API; **UNVERIFIED** that it unpacks/runs — I did not download it.] Use if
the from-source build proves unreliable: `ADD` the tarball into a Debian arm64
image and skip stage 1 entirely. Trade-off: third-party binaries, KOS pinned to a
2024-era tag rather than master, and no control over profile knobs.

**F3 — swap the toolchain profile.** If GCC 15.2.0 miscompiles the decomp (see
`PLAN.md` §3.2 — this codebase has a documented history of optimizer breakage),
change one ARG: `TOOLCHAIN_PROFILE=legacy` (GCC 13.2.0) or `14.4.0`. Note sm64-dc
shipped on GCC 14.1 per `kb/research-dreamcast.md`, so `14.4.0` is the
best-precedented middle option.

**F4 — Alpine instead of Debian.** Upstream's own
`utils/kos-chain/docker/Dockerfile` is Alpine and is the configuration upstream
CI exercises. If the Debian host toolchain (GCC 12) fails to build GCC 15.2.0,
switch the base image and follow `doc/alpine.md`. mkdcdisc would then need
`libisofs` from Alpine (`apk add libisofs-dev meson`) — [UNVERIFIED that Alpine
packages it].

**F5 — reduce parallelism.** kos-chain's own docs warn that multiple jobs "may
cause issues in certain environments"; KOS master's most recent commit is
literally a fix for a **parallel libgcc build race**. If stage 1 fails
nondeterministically, set `--build-arg JOBS=1` and accept the wall clock.

**Never a fallback: running an amd64 image under qemu.** See §2.

---

## 9. Open items for the next agent

1. Commit the §5 Dockerfile as `dc/Dockerfile` and write
   `dc/build-dc-docker.sh` per §7. The images already exist locally; committing
   the Dockerfile is what makes them reproducible.
2. Add `dc/build/`, `*.cdi`, `*.gdi`, `*.mds`, `*.mdf`, `*.nrg` to `.gitignore`
   before anything writes a disc image into the tree.
3. Fix the wrong sentence in `kb/base-repo-map.md` — SH-4 GCC `char` is
   **signed** by default, confirmed on GCC 15.2.0 (§3.1, §5.1). Not edited here
   because it is not this pass's assigned file.
4. Measure a real full-tree compile (4,824 files) and replace the §2.1
   extrapolation with the true number.
5. Ask the user to re-provision colima at 8 CPUs / 16 GB (§1.2) — every build
   time in this document was measured at 4 cores.
6. Evaluate kos-ports `sh4zam` against `PLAN.md` §3.2's hand-rolled FTRV/FIPR
   plan before writing SH-4 math by hand.
7. Decide the GCC-15.2.0-vs-decomp question at M1. `PLAN.md` §3.2 documents this
   codebase's history of optimizer breakage; fallback F3 (profile `14.4.0`,
   matching sm64-dc's GCC 14) is one ARG change away.

---
