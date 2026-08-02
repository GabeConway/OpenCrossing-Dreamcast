# Save budget: 289 KB of Animal Crossing into a 100 KB VMU

Measured 2026-08-01 (M1, PLAN.md §6 / risk "Save won't fit VMU").
Harness: `tools/savebench/` — re-run it whenever the save layout changes.

**Verdict up front:** the uncompressed unique payload is **295,910 bytes**
(289 KiB) — not the 456 KB the GC card file suggests, because a third of that
file is a backup copy and another chunk is GC banner/icon/padding. With the
proposed shipping encoder (chunked deflate-6) a *typical* save fits one VMU
comfortably (**111 of 200 blocks**); a *maxed* save does not fit with any
safety margin (**203 blocks, 101%**). The fix is not a better compressor — it
is trimming the three storage-locker blocks, which drops the maxed case to
**150 blocks (75%)**. The adversarial ceiling still overflows one VMU, so the
save path must also degrade gracefully rather than fail.

> **All compression numbers on this page are SYNTHETIC.** No real `.gci` exists
> on this machine (searched `OpenCrossing-Anbernic/**` including
> `pc/build-*/bin/save/card_a|card_b`, `harness/out/`, and git history — the
> card dirs exist but are empty; the only artefact in history is
> `harness/inspect-gci.py`). The *layout* numbers are exact and verified. The
> *content* model is an educated construction. Re-run with `--gci` against a
> real late-game save before treating any ratio as settled.

---

## This document was split (2026-08-02)

It was 375 lines. The verdict and the synthetic-data warning above stay here
because they govern all three parts. Section numbers inside the parts are the
original ones — use this table to resolve a `§n` reference.

| part | original sections | contents |
|---|---|---|
| [`kb/save-layout.md`](save-layout.md) | §1, §1.1 | verified struct sizes, `.gci` layout, the 295,910 B unique payload, byte breakdown by field group |
| [`kb/save-compression.md`](save-compression.md) | §2, §3 | codec comparison (synthetic), where the compressed bytes go, the fit verdict against 200 VMU blocks, PLAN §6 options measured |
| [`kb/save-plan.md`](save-plan.md) | §4, §5, §6, Sources | the shipping recommendation, compression and VMU-flash write cost, open items, sources |
