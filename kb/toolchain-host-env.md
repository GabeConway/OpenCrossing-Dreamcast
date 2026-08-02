# Dreamcast toolchain — host environment and the qemu measurement

The measured colima/Docker environment (4 CPUs, 8 GiB, no BuildKit), the
`$HOME`-only bind-mount constraint that silently mounts empty dirs, and the
17–23× + nondeterministic-ICE measurement that disqualifies amd64-under-qemu,
with the commands to reproduce it. Read before debugging a build that is slow,
flaky, or sees an empty mount. Tag discipline (**[VERIFIED]** = ran it on this host and read the output, **[UNVERIFIED]** = inferred or read from docs) is unchanged from `kb/design-toolchain.md`; do not upgrade an UNVERIFIED claim without re-measuring.

## 1. Host environment — measured, and it is not what the brief said

| Fact | Value | Status |
|---|---|---|
| Host | macOS, Apple M4 arm64, 10 cores, 24 GB | given |
| Docker | client 29.6.1 (darwin/arm64), server 29.5.2 (**linux/arm64**), context `colima` | [VERIFIED] |
| **colima VM CPUs** | **4** (not 10) | [VERIFIED] `docker info` + `colima.yaml` |
| **colima VM RAM** | **8 GiB** (not 24) | [VERIFIED] |
| colima disk free | 81 GB in VM, 87 GB on host | [VERIFIED] |
| mountType | virtiofs | [VERIFIED] |
| BuildKit | **absent** — `docker build --progress` fails with `unknown flag`; legacy builder only | [VERIFIED] |
| binfmt/qemu for amd64 | present and working (`tonistiigi/binfmt` image installed) | [VERIFIED] |

### 1.1 Bind-mount constraint (this bites, plan around it)

**Only paths under `/Users/gabe` are shared into the colima VM.** [VERIFIED]

`/private/tmp/...` (the agent scratchpad) mounts as an **empty directory** — Docker
silently creates the mountpoint inside the VM and you get nothing. This is a
silent-wrong-answer failure, not an error. Confirmed by mounting the scratchpad
(empty) vs mounting the repo at `/Users/gabe/Documents/GitHub/OpenCrossing-Dreamcast`
(full contents visible). [VERIFIED]

Consequence: **every bind mount in every DC build script must be under `$HOME`.**
The repo itself qualifies, so the base-repo pattern works unchanged.

### 1.2 Recommended host change (not applied — needs the user)

colima is provisioned with 4 of 10 cores and 8 of 24 GB. The toolchain build and
the ~4,800-file game build are both `make -j` bound. Recommend:

```bash
colima stop && colima start --cpu 8 --memory 16 --disk 100
```

**Not done by this pass** — it would kill the in-flight build and any other
agent's containers. All build times below are measured at **4 cores** and are
therefore an upper bound. [VERIFIED that colima is at 4 CPUs; UNVERIFIED what
8 cores would give — expect roughly 0.55–0.65× wall clock, not 0.5×.]

---

## 2. Why not qemu: the measurement

Same image (`einsteinx2/dcdev-kos-toolchain`, which ships **both** arm64 and
amd64 manifests — a clean A/B), same corpus, same host. Corpus = 100 synthetic
self-contained C TUs, mean 13.9 KB, compiled `-O2 -fsigned-char`.

| Workload | arm64 native | amd64 under qemu | ratio |
|---|---|---|---|
| 20 × trivial `hello.c`, serial | **0.184 s** (9.2 ms/file) | **4.253 s** (213 ms/file) | **23.1×** |
| 100 × 13.9 KB TU, `-O2`, serial | **2.783 s** (27.8 ms/file) | *did not complete* — ICE | — |
| 10 × 13.9 KB TU, `-O2`, serial | — (see row above: 27.8 ms/file) | **4.66 s** mean of 3 runs (466 ms/file) | **16.8×** |
| 100 × 13.9 KB TU, `-O2`, `-j4` | **0.741 s** | not run | — |

[VERIFIED — every bolded number is a real timing from this host. The qemu
100-TU serial run is absent because it *crashed*, which is the point; the 10-TU
qemu row exists so there is a like-for-like per-file rate to compare against.]

**qemu is not merely slow, it is unsound.** The 100-TU serial run under qemu died
with:

```
sh-elf-gcc: internal compiler error: Segmentation fault signal terminated program cc1
```

Re-running 10 TUs three times: **1 failure in 30 compiles (3.3%), nondeterministic**
— the same file compiled fine on the next attempt. [VERIFIED] The identical corpus
compiled 100/100 natively on arm64. A ~3% random per-TU failure rate across 4,824
files makes a build statistically impossible to complete, and worse, would produce
*intermittent* failures that look like source bugs. This alone disqualifies qemu
regardless of speed.

### 2.1 Extrapolation to the real tree

Repo has **4,824** `.c/.cpp/.s` files under `src/` (4,765 `.c`, 809,797 LOC),
mean 6.95 KB, median 3.1 KB, p90 11.7 KB, max 3.69 MB
(`src/data/field/bg/acre/bg_data.c`). [VERIFIED — counted]

| Path | Serial | `-j4` |
|---|---|---|
| arm64 native | 4824 × 27.8 ms ≈ **134 s** | ≈ **36 s** |
| amd64 qemu | 4824 × 466 ms ≈ **37.5 min** | ≈ 10 min (plus ~160 random ICEs) |

[UNVERIFIED extrapolation.] Caveats, both directions: my corpus averages 13.9 KB
(2× the real mean, so *pessimistic* per file) but is fully self-contained, whereas
real decomp TUs pull the whole `include/` tree (so *optimistic* on preprocessing),
and `src/data/` contains multi-MB generated tables that will dominate. **Realistic
expectation for a full cold game build on arm64 at `-j4`: 5–15 minutes.**
[UNVERIFIED — measure at M1 and replace this line with the real number.]

The load-bearing, verified number is the **17–23× ratio plus the ICEs**. That is
what makes the decision, and it is not close.

---

## 10. Reproducing the measurements

So a later pass can audit or re-run any number in §2. Scratch dir must be under
`$HOME` (§1.1).

```bash
# corpus: 100 self-contained C TUs, mean 13.9 KB (generator was throwaway;
# any similar corpus reproduces the ratio — it is not sensitive to the details)
mkdir -p ~/.cache/dcbench/src

# native arm64
docker run --rm --platform linux/arm64 -v ~/.cache/dcbench:/bench \
  --entrypoint /bin/sh einsteinx2/dcdev-kos-toolchain:latest -c '
    G=/opt/toolchains/dc/sh-elf/bin/sh-elf-gcc; cd /bench
    time (for f in src/tu_*.c; do $G -O2 -fsigned-char -c $f -o /tmp/x.o; done)'

# same image, amd64 manifest, under qemu — expect ~17x and sporadic ICEs
docker run --rm --platform linux/amd64 -v ~/.cache/dcbench:/bench \
  --entrypoint /bin/sh einsteinx2/dcdev-kos-toolchain:latest -c '...same...'
```

Manifest checks that decided §0:

```bash
docker manifest inspect einsteinx2/dcdev-kos-toolchain:latest  # -> amd64 AND arm64
docker manifest inspect nold360/kallistios-sdk:latest          # -> amd64 only
```

Note `docker manifest inspect nold360/kallistios-sdk:latest` returns a bare
single-arch manifest with no `platform` block; use `-v` to see
`"architecture": "amd64"`. `ghcr.io/nold360/*` and `ghcr.io/kallistios/*` both
return `denied` — those namespaces do not publish public images. [VERIFIED]
