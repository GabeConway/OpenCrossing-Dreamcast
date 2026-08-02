# Budget premises §3 — what was established, with evidence

§3 of `kb/research-budget-premises.md`, moved verbatim: KOS's memory model,
bucket 1's double-count, GLdc costing zero today, buckets 9/10/11 being `.bss`,
archive tables, the exact 888,853 B `s_assets[]` figure (§3.6), `.bss`
composition, and the accounting note about "6,318,568". Read for the sourced
numbers behind the corrected budget. Per-item [M]/[S]/[?] tags are original.

---

## 3. What was established, with evidence

### 3.1 KOS's memory model — confirmed exactly [S]

Read from the SDK image (`opencrossing-dc:sdk`), not from secondhand docs:

```
0x8c000000 ─ 0x8c010000   64 KB  below page_phys_base — unusable   arch.h:53,297
0x8c010000 ─ _end                the ELF: game + KOS + newlib, all of it
_end       ─ 0x8cff0000          the heap; sbrk_base = align(end,4)  mm.c:31-34
0x8cff0000 ─ 0x8d000000   64 KB  kernel thread stack, hard-reserved mm.c:52
```

- `_arch_mem_top = 0x8d000000` — `arch.h:42`
- `THD_KERNEL_STACK_SIZE = 64 * 1024` — `stack.h:52`
- `THD_STACK_SIZE = 32768` (per app thread; none created at boot) — `stack.h:47`
- `mm_sbrk` returns `ENOMEM` at `_arch_mem_top − THD_KERNEL_STACK_SIZE` — `mm.c:50-55`

**Usable for image + heap = 16,646,144 B.** The "every `.bss` byte destroys a
heap byte" claim is confirmed at the source: the heap literally begins at
`end`.

### 3.2 Bucket 1 (KOS 1,000,000) is a double-count and ~3.8× too large [M]

Measured from `dc/build/AnimalCrossing.map` by attributing every allocatable
input section to its owning archive:

| archive | allocatable bytes in our image |
|---|---:|
| `libkallisti.a` | 130,563 (`.text` 95,238 · `.bss` 20,197 · `.rodata` 9,460 · `.data` 5,668) |
| `libc.a` | 84,665 |
| `libstdc++.a` | 48,991 |
| `libgcc.a` | 27,374 |
| `libm.a` | 13,236 |
| **total** | **304,829** |

All 304,829 B are **inside** the 21,374,996 B image span. Subtracting
1,000,000 B on top of the image counts them twice.

Independently corroborated by building test programs in the SDK container:
a bare KOS hello-world's image is **230,488 B**; `+pvr_init_defaults()` adds
**764 B** and **zero heap** (the entire main-RAM PVR state is `_pvr_state`,
520 B of `.bss`; TA/OPB buffers are VRAM offsets, `pvr_buffers.c:252-310`).

Genuinely **additive** KOS heap, read from source:

| item | bytes | file:line |
|---|---:|---|
| kernel stack reservation | 65,536 | `mm.c:52` |
| `fs_iso9660` sector cache + headers | 65,792 | `fs_iso9660.c:1277-1278` |
| maple DMA buffer | 16,384 | `maple_init_shutdown.c:129` |
| 3 × `kthread_t` (kern/idle/reaper) | 3,456 | `thread.c:426,1117-1136` |
| *future* `snd_stream` sep buffer | 65,536 | `snd_stream.c:353` |
| **total incl. future** | **216,704** | |

**Recommended bucket 1 = 262,144 B**, relabelled *"KOS runtime heap, NOT image
bytes"*. **Frees ~738 KB.**

No romdisk is linked (no `__kos_romdisk` in the map) — [M], and it must stay
that way (`kb/research-size-reduction.md` §3.6).

### 3.3 GLdc costs zero today [M]

`dc/Makefile:441` passes `-lGL` and the map shows
`LOAD /opt/toolchains/dc/kos-ports/lib/libGL.a` at line 45413 — but **zero
`libGL.a(…)` members are pulled in.** Nothing references a GL symbol:
`dc/src/dc_gx.c` contains **0** `gl*` calls and targets raw PVR (still stubbed
at `dc_gx.c:1913-1940`). The `-lGL` on the link line is dead weight.

The concern that GLdc holds large main-RAM vertex buffers is **not borne out**
for its init path: a linked GLdc test binary's statics total ≈5.4 KB
(`_OP_LIST`/`_PT_LIST`/`_TR_LIST` 96 B each, `_ATTRIB_LIST` 132 B,
`_SHARED_PALETTES` 256 B, `_pool_header` 4,116 B), with submission buffers
being `AlignedVector` headers grown on demand. Linking GLdc adds **+25,388 B**
of image (a floor — LTO dead-strips unreferenced entry points).

**[?] GLdc's vertex-buffer growth policy and peak are UNVERIFIED** — the SDK
image ships only LTO bytecode and headers, no GLdc sources. Re-open this at M2
when stage A lands.

### 3.4 Buckets 9, 10, 11 are `.bss`, not heap [M]

`dc/src/dc_mem_ledger.c:64-75` marks `AUDIO`, `DISC_IO` and `PVR_STAGING` with
`carve = 1` (they would `malloc(budget)` a real extent), but every call site
only calls `dc_mem_note()` on a **static** buffer:

| bucket | budget | what actually exists | where |
|---|---:|---|---|
| 9 AUDIO | 700,000 | `jaudio_NES` `.bss` = **1,265,101** (`audiomemory` 589,824 · `seq` 277,580 · …) + `dc_audio.c` counters | static `.bss`, in image; `dc_audio.c:105` only notes |
| 10 DISC_IO | 384,000 | `dc_dvd.c.o` `.bss` = **13,320** (`dvd_path_off` + `dvd_path_pool`) | static `.bss`; `dc_dvd.c:218` only notes. **No read-ahead ring exists** |
| 11 PVR_STAGING | 384,000 | `dc_gx.c.o` `.bss` = **334,764** (`vertex_buffer[8192]`) | static `.bss`; `dc_gx.c:372` only notes |

So 1,468,000 B is reserved on the heap side of the budget for things that are
already counted in the image. Worse, if any of those buckets ever takes the
`dc_mem_alloc` path it will carve the extent *in addition to* the `.bss`.

**Note the trap this exposes:** `kb/research-size-reduction.md` §4.2 is right
that moving a static array to the heap saves nothing on a no-MMU sbrk machine.
Buckets 9/10/11 are that mistake encoded in the ledger. Either delete them and
fold the bytes into bucket 5, or make them real extents *and* delete the
`.bss` — never both.

Bucket 9 is also **1.81× over budget** on its real content, which no one had
noticed because the bucket was never compared to the `.bss` it describes.

### 3.5 Mounted archives cost ~3 KB of main RAM, not hundreds [M]

Both `forest_1st.arc` and `forest_2nd.arc` mount as **`JKRAramArchive`**
(`jsyswrap.cpp:532`, `:561`), so their payload goes to ARAM, not the arena.
But `JKRAramArchive.cpp:167` allocates `mArcInfoBlock` — the RARC node, file
entry and string tables — from **main RAM**, resident for the whole game.

Measured by reading the RARC headers straight out of the user's ISO:

| archive | disc size | nodes | file entries | **resident main-RAM table** |
|---|---:|---:|---:|---:|
| `forest_1st.arc` | 852,896 | 2 | 29 | **1,088** |
| `forest_2nd.arc` | 4,132,608 | 2 | 57 | **1,888** |
| `famicom.arc` | 1,699,904 | 4 | 51 | 1,952 |

**2,976 B total** for the two mounted archives. This was a plausible hidden
cost and it is negligible — closed. (The archives hold only 86 files between
them, so RARC granularity is very coarse; that matters for bucket 8's window
design, not for RAM.)

### 3.6 `.rodata` — the `s_assets[]` string pool figure is exact [M]

Per-tree attribution from the map (project objects only, current ELF):

| tree | `.data` | `.rodata` | total |
|---|---:|---:|---:|
| `src/data/model` | 1,123,275 | 47,780 | 1,171,055 |
| **`pc/src`** | 48 | **893,136** | **893,184** |
| `src/data/npc` | 567,762 | 1,944 | 569,706 |
| `src/data/field` | 524,056 | 24,509 | 548,565 |
| `src/actor` | 135,231 | 7,535 | 142,766 |
| `src/game` | 99,822 | 19,687 | 119,509 |
| … | | | |
| **TOTAL** | **2,629,081** | **1,036,765** | **3,665,846** |

Largest single objects: `pc/src/pc_assets.c` **888,853** ·
`src/data/field/bg/acre/bg_data.c` 317,424 · `src/data/model/player_anim.c`
250,433.

**The 888,853 B `s_assets[]` name-string claim is confirmed to the byte.**
`.rodata` outside `pc_assets.c` is only 147,912 B, so
`kb/research-size-reduction.md`'s post-fix target of 165,000 B for `.rodata`
is **achievable and correctly sized** — that lever is real.

### 3.7 `.bss` composition, current ELF [M]

`.bss` = 12,415,508. `src/data/**` = **8,519,191 (62.6 %)** across **2,534
objects**, median 2,784 B, mean 3,361 B; only 15 objects are ≥32 KB and they
total 864,096 B. Non-`src/data` `.bss` = 3,896,317 B.

The flat, fine-grained distribution is the important part: there is no big
single win in `src/data`, which is exactly why demand residency (a mechanism)
is the only lever that reaches it.

### 3.8 Accounting note that has already caused confusion

`kb/mem-budget.md` §8.1's "`.text` 6,318,568" is **not** the `.text` section.
Measured today: `.text` alone = 5,257,344; `.rodata` = 1,053,740;
`.init/.fini/.ctors/.dtors/.eh_frame/.gcc_except_table/.got` = 7,556.
`5,257,344 + 1,053,740 + 7,556 = 6,318,640` ≈ the quoted figure. **Treat
"6,318,568" as text+rodata+misc-read-only and do not add `.rodata` to it
again.**

### 3.9 Premises confirmed unchanged

- `-DTARGET_PC` **is** defined for the Dreamcast build — `dc/Makefile:164`,
  alongside `-DTARGET_DC`. The Makefile comment (`:158-162`) states the intent:
  "*Here it means 'not GameCube', not 'PC'*", guarding the base port's
  little-endian correctness fixes. It is deliberate, and documented as
  non-negotiable. **The audit of what else it drags in is unfinished — §6.2.**
- The §8.3 `.bss` fixes are present in the tree: `src/game/m_play.c:54-64`
  (`prbuf` `u16` under `TARGET_DC`) and
  `include/libforest/emu64/texture_cache.h:76-86`.
- `dc/build/bsssym.tsv` is from the **first** link (it shows `prbuf` at
  1,228,800). The ELF and map next to it are **post-fix**. Do not mix them.
