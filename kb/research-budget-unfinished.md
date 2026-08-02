# Budget premises §6 — what is NOT finished, numbered, with next steps

§6 of `kb/research-budget-premises.md`, moved verbatim. This is the list other
docs cite as "§6 lists what is unfinished"; `kb/levers.md` cites **§6.2** (the
`-DTARGET_PC` audit) by name. Read before starting any budget investigation, to
check it is not already a known-open question. **Nothing in the parent document
was executed on hardware.**

---

## 6. Not finished — numbered, with next steps

**Three parallel investigations were still running when the session ended.
Their results are lost; the questions below are open.**

1. **Bucket 6's real high-water mark. THE priority.** §2 characterises the
   arena and finds 1.29 MB of dead weight, but the `__osMalloc` peak is
   unknown. **Next step: §2.4, on the Anbernic build.** Also still unanswered:
   are `gamealloc.c` / `TwoHeadArena.c` / `THA_GA.c` live in this build or
   dead libultra-era code, and do they carve from the same arena? Not checked.
   *Note:* whether an Anbernic host build tree is already configured (and how
   long a build takes) was not established either — check
   `pc/CMakeLists.txt`, `build_make.sh`, `pc/build/`.

2. **The `-DTARGET_PC` audit (question 5).** Only the premise is confirmed
   (§3.9). **The valuable part — a complete list of what the define changes —
   was not produced.** Next steps, in order:
   a. Check whether `src/data/**/assets/*.inc` files **exist in the repo**. If
      they do not, the non-`TARGET_PC` branch does not build and "revert to
      the GameCube path" is not an available option at all. This one `ls`
      decides the whole question.
   b. **Do not expect a saving from turning it off.** Under the
      non-`TARGET_PC` branch those arrays become *initialised* data — the same
      resident bytes, moved from `.bss` to `.data`, plus disc bytes. On a
      no-MMU sbrk machine that is neutral at best. The "8.5 MB is free PC
      scaffolding" reframing is therefore **probably FALSE as a RAM lever**,
      though it may still be true as a description of history. Verify before
      repeating either version.
   c. Bucket the ~2,633 `TARGET_PC` files into the mechanical `src/data`
      placeholder pattern vs the ~60–100 genuine behavioural forks in
      `src/game`, `src/static`, `src/system`, `src/effect`, `src/furniture`,
      `src/actor`, and classify the latter (endianness, pointer size, asset
      loading, file I/O, stubbed GC hardware, debug). Flag any that assume
      SDL, a host filesystem, or 64-bit.

3. **`.data`/`.rodata` const-ness and dedup (question 6).** Per-tree sizes are
   in §3.6; the analysis is not done. Next steps:
   a. Both `.data` and `.rodata` are resident RAM on DC, so adding `const`
      **saves zero by itself** — the deliverable is *bytes that are
      read-only-in-practice AND pointer-free*, i.e. evictable to disc.
   b. The 948,688 B pointer-free figure came from **dynamic relocations on the
      ARM PIE build**. That method does not exist on a static non-PIE SH ELF,
      so it is **unverified for this target**. Re-derive by scanning `.data`
      contents for words in `0x8c010000.._end` and classifying per symbol;
      report false-positive risk honestly.
   c. Exact-duplicate dedup of generated `src/data` tables (hash the
      initialised bytes per symbol, group). `--icf` is unavailable on SH but
      source-level dedup in the generator is allowed. **Completely unmeasured
      — could be zero, could be significant.**

4. **Bucket 8 (ARAM graph window, 512,000).** Still a guess. §3.5 shows the
   archives hold only 86 coarse files, which may make a small window unworkable
   — a single `forest_2nd.arc` member could be large. **Next step: list the
   member sizes from the RARC file-entry table (the reader in §3.5 already
   parses the header) before sizing the window.**

5. **Bucket 5's post-conversion floor.** §3.7 gives 3,896,317 B of
   non-`src/data` `.bss` today against a 1,950,000 B budget. The itemised path
   from one to the other (`kb/mem-budget.md` §4.1) predates the DC link and was
   not re-checked.

6. **Nothing here was executed.** Every runtime figure in §3.1/§3.2 is
   static source reading, and §2's arena consumption is static analysis of a
   binary that **has never booted**. A boot-time `mallinfo()` /
   `arch_get_ram_free()` probe after `pvr_init` would convert most of §3 from
   [S] to [M] cheaply, and should be the first thing M1 does once the image
   fits.
