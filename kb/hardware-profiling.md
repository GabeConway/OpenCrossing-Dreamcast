# Hardware profiling — the instrument that answers §6

**Why this exists.** The standing human verdict (2026-08-10) is that the port
runs **much worse on a retail Dreamcast than in Flycast**, and *nothing has ever
attributed that gap to a cause*. `kb/RESUME.md` §6 has carried it as the
headline open question, and Flycast is structurally unable to answer it: it
models **no instruction cache, no operand cache and no disc seek time**. The
only hardware instrument the project had was `dc/src/dc_pmcr.c` (P1), which
gives eight aggregate counters and has never been burned.

⭐ **AS OF 2026-08-12 THIS IS NO LONGER A DESIGN — IT RAN.** A `DC_GPROF=1`
town build linked, armed, sampled, dumped, decoded and symbolised end to end in
Flycast through the console sink. §7 carries the receipt. **The hardware half
has not been run**, and §2 explains why it is a *second image* rather than the
same one.

---

## 1. What KOS gives us, verified in `opencrossing-dc:sdk`

| thing | path in the image | state |
|---|---|---|
| gprof runtime | `/opt/toolchains/dc/kos/addons/libgprof/{gmon.c,arch/dreamcast/trap.c}` | built, `libgprof.a` present, **linked and run** |
| example + docs | `/opt/toolchains/dc/kos/examples/dreamcast/profiling/gprof/` | README is the reference |
| host reporter | `sh-elf-gprof` (via `$KOS_GPROF`) | ships with the toolchain, **used** |
| SD block driver | `kernel/arch/dreamcast/include/dc/sd.h`, `hardware/sd.c` | built into the kernel, **never mounted by this code** |
| FAT32 | `addons/libkosfat/`, `libkosfat.a` | built, **never mounted by this code** |
| SD speed test | `examples/dreamcast/filesystem/sd/speedtest/` | stock, run it first |

---

## 2. 🔴 IT IS TWO IMAGES, NOT ONE — AND THAT WAS NOT A CHOICE

Earlier revisions of this file promised **"the sink is chosen at RUNTIME, so one
CDI is correct with or without a card"**. **That is FALSIFIED.** Probing for an
SD card that is not there does not fail cleanly; it **wedges the machine**.

**The mechanism, out of KOS source.** `sci_spi_rw_byte()`
(`kernel/arch/dreamcast/hardware/sci.c`) waits for RDRF like this:

```c
do {
    status = SCSSR1;
    if(status & ORER) { ...; return SCI_ERR_OVERRUN; }
} while(!(status & RDRF));
```

— **unbounded**. Every TDRE wait in that same file is capped by
`SCI_MAX_WAIT_CYCLES`; **all four RDRF waits in the SPI helpers are not.** The
SH-4's SCSSR1 reset value is **0x84** (TDRE|TEND set, RDRF and ORER clear),
which passes every bounded TDRE gate, lets `sci_init()` return `SCI_OK`, and
then pins `sd_init_ex()`'s first `spi_rw_byte(0xFF)` **forever**. Nothing wired
to SCI ever sets RDRF or ORER, so there is no escape and no timeout.

SCIF is bounded but no use as a fallback either: `acmd41_loop()` runs up to
`MAX_RETRIES` **5000** iterations, each of them two `sd_send_cmd()`s with a
**500 ms** `thd_poll` ceiling.

**MEASURED 2026-08-12, Flycast:** a `DC_GPROF_SD=1` image printed the pre-mute
marker at frame 300 and never came back — **on both `DC_GPROF_SD_IF` settings**.

### ⭐ The consequence, and why it costs nothing

Build **two** images:

| half | knobs |
|---|---|
| Flycast | `DC_GPROF_SD=0` — console sink |
| hardware | `DC_GPROF_SD=1` — card sink |

**This is forced by the hardware anyway.** Flycast has no SD adapter, and on a
console the adapter *occupies the serial port*, so a hardware run with the card
has no console at all. There was never a world where one image served both.

⭐ **It does NOT weaken the Flycast-vs-hardware diff**, for three reasons:

1. `gprof` symbolises each run against **its own ELF**, so the two do not have
   to be the same binary — only the same *code*.
2. `-pg` is **link-line only**, so the profiled game code is byte-identical
   between the halves by construction (§3).
3. `dc_profdump.c` and `libkosfat` are **not on the hot path**. They run once,
   at the dump, after `_mcleanup()` has already stopped the sampler.

⚠️ What *does* differ is that the console half pays `scif_write` inside its own
profile (§7 measures it at 1.56 s / 0.50 %). Subtract it, or read the SD half
as the cleaner one.

---

## 3. ⭐ THE KEY TRICK — `-pg` ON THE LINK LINE ONLY, ON ZERO TUs

The KOS README warns that `-pg` puts a **trap + `mcount` on every function
entry**. Our hot path runs millions of calls a frame, and `-pg` also forces
`-fno-omit-frame-pointer` and (in the example) `-fno-inline`, which would
destroy the `-Os` + `-O3`-hot-list tuning that is worth 11.6 → 20.0 FPS.

**But the flat profile does not come from `mcount`.** `gmon.c`'s
`histogram_callback()` is a **scheduler-driven PC sampler** — it rides
`thd_poll` and reads `thd_current`'s saved PC — and it runs whether or not any
TU was compiled with `-pg`.

So: **compile nothing with `-pg`; put `-pg` on the link line only.** The
optimized code stays byte-identical; the call graph is lost and we do not need
it. The question is "where does the time go on silicon", and that is the flat
profile.

`/opt/toolchains/dc/kos/utils/build_wrappers/kos-cc` rewrites a bare `-pg` into
`-Wl,--whole-archive -lgprof -Wl,--no-whole-archive` and places it **after** our
objects and before `KOS_LIBS` — so `dc_profdump.c`'s strong `gprof_init()` is
the definition that wins under the `-Wl,--allow-multiple-definition` the link
already carries. `dc/Makefile` puts `-pg` (plus `-lkosfat` when `DC_GPROF_SD` is
on) on the `$(TARGET)` recipe as `GPROF_LDFLAGS`.

⭐ **VERIFIED ON THE REAL LINK, 2026-08-12.** All 3,936 objects link with `-pg`
on the line, exactly one `gprof_init` survives, and the resulting image arms and
samples (§7). The "still unproven, fallback is an explicit `-lgprof`" hedge
earlier revisions carried is retired.

⭐ `GPROF_LDFLAGS` is keyed into `$(LINKSTATE)`, **not** `$(FLAGSTATE)`. `-pg`
on the link line changes no object by construction — that is the entire point —
so keying it into the compile stamp would rebuild ~3,900 TUs to relink one ELF.
`make optreport` prints a `P2 gprof link` line so the state is visible.

---

## 4. The real constraints

### 4.1 Memory — it fits, with the keep list, and now that is measured

`monstartup()` allocates ~`0.615 × T` for a `.text` range of `T` bytes
(`HISTFRACTION 8`, `HASHFRACTION 16`, `ARCDENSITY 2`, `MAX_NODES 65535`).

**MEASURED on the 2026-08-12 keeplist-town link:**

```
range 8c010000..8c266062      predicted alloc 1,506,528 B
[GPROF] Total memory allocated: 1506528 bytes     <- gmon.c's own line
```

⭐ **The allocation SUCCEEDED.** `dc_prof_predict_alloc()`'s arithmetic and
`gmon.c`'s real figure agree to the byte, and the game went on to render and
dump. The "~1.55 M against ~499 KB of headroom, **it does not fit as shipped**"
arithmetic earlier revisions carried was correct *about the shipping keep list*
and is not the situation a profiling build is in.

The lever that pays for it, and it is legitimate because **a profiling build is
a MEASUREMENT build, not the shipping one**:

1. ⭐ **build with `keeplist-town.txt` instead of `keeplist-full.txt`.**
   `keeplist-full`'s extra `src/data/model/` families are content, not code, so
   fewer models render and it is not the shipping frame; nothing about the
   profile's *timing* is falsified by it. **This is the one that was used, and
   1,506,528 B is confirmed to fit alongside it.**
2. `DC_GPROF_SPAN=N` — profile only `N` bytes from the low address, scaling
   `monstartup()`'s allocation by `N/T`. **Not needed for a keeplist-town
   build.** Read the denominator warning in §8 before using it.

🔴 **`DC_ARAM_WINDOW=131072` IS THE WRONG LEVER HERE, AND EARLIER DRAFTS OF THIS
DOCUMENT RECOMMENDED IT.** It frees ~917 KB, which is why it looked attractive —
but `kb/RESUME.md` §5 measured matched 420 s runs at 131072 vs 1048576: disc
reads **106 → 4,183**, bytes off disc **4.3 MB → 137.9 MB**, evictions
**68 → 4,173**. Disc timing is *exactly one of the things hardware has and
Flycast does not*, so shrinking the window would put ~40× the disc traffic into
the very profile being measured, and `dc_dvd_read_yielding()` is already the
most over-represented symbol in it (§8). **Keep it at 1048576. The keep list is
the lever.**

### 4.2 🔴 F5 MUST BE OFF FOR A KEEPLIST-TOWN PROFILING BUILD

`dc/section-order.txt` was generated from a **keeplist-full** link. Applied to a
**keeplist-town** link — which is what a profiling build is (§4.1) — the image
**hangs in Flycast inside `maple_wait_scan()`**, i.e. inside `arch_auto_init()`,
before `main()` and before `gprof_init()` can print a thing.
`DC_SECTION_ORDER=0` on the identical tree boots clean. **Proven by a four-build
matrix, 2026-08-12.**

⚠️ **Hardware boots the F5-on keeplist-town image fine** — this is a
Flycast-only failure. But the lesson generalises: **F5 is not inert when the
link it describes changes.** An ordering file is a statement about a specific
set of input sections; feed it a different set and the result is not "the same
image, unordered".

`DC_SECTION_ORDER=0` is in both build lines in §5 for this reason. It also makes
the two halves match, which is what the A/B needs.

### 4.3 Getting `gmon.out` off the machine

Default target is `/pc/gmon.out`, which needs a dc-load host connection.
**Booting from a burned CD-R there is no `/pc`.** So `dc_profdump.c`'s strong
`gprof_init()` mounts a `/prof` VFS and points `GMON_OUT_PREFIX` at it before
`monstartup()` runs.

| sink | where it works | notes |
|---|---|---|
| **`/prof` console dump** | **Flycast**, and any console with a coder's cable | write-only VFS that stores nothing: zero-run-RLE, base64, straight out of `dbgio` as framed lines. `tools/dcprof/decode_gmon.py` recovers it from `console.log` |
| **`/sd`** ⭐ hardware only | retail console + SD adapter | real FAT32 file. No encoding, no console tax, no decoder — `sh-elf-gprof` eats it directly. **Never yet mounted by this code** |

---

## 5. The two build lines — validated

**Flycast half** (this is the exact line the §7 result came from):

```bash
DC_STUB_KEEP="$(grep -v '^#' tools/dcstub/keeplist-town.txt | paste -sd: -)" \
DC_DISC_ROOT=~/.cache/oc-dc-discroot DC_ASSET_STUB=1 \
DC_ARAM_WINDOW=1048576 DC_ARENA_BYTES=1200000 \
DC_AUDIO_SCENES=all DC_AUDIO_DISC_FRAMES=8 DC_AUDIO_VOICES=12 \
DC_GPROF=1 DC_GPROF_SD=0 DC_GPROF_HZ=0 DC_GPROF_DUMP_FRAME=300 \
DC_SECTION_ORDER=0 bash dc/build-dc.sh
```

**Hardware half** — the same line with three changes:

```bash
… DC_GPROF=1 DC_GPROF_SD=1 DC_GPROF_HZ=0 DC_GPROF_DUMP_FRAME=2400 \
DC_SECTION_ORDER=0 DC_CDI_PAD=1 bash dc/build-dc.sh
```

The frame cap goes up because a human driving to somewhere worth profiling takes
longer than an emulator does; the chord (§6.3) is the real trigger and the cap is
the backstop.

⚠️ **`DC_GPROF_HZ=0` — KOS's 100 Hz — is the recommended value, and it is the
one that is VALIDATED.** It produced 31,010 samples in 470 bins from a 300-frame
run (§7); that is not a shortage. `DC_GPROF_HZ=1000` was suspected of causing
the early stalls and **that theory was REFUTED** — the stall was the SD probe
wedge of §2, not the sampler. Do not repeat the claim that 1000 Hz is slow; just
do not reach for it unless a real run comes back sample-starved.

---

## 6. The SD sink (`DC_GPROF_SD=1`), and why it is bolted on at DUMP TIME

A serial-port SD reader (DreamShell V4.0 class). KOS drives exactly this:
`dc/sd.h` — *"The original SD card reader designed by jj1odm connects to the
SCIF port… bit-banging to emulate SPI"*, plus an SCI variant.

### ⭐ 6.1 The card comes up at the dump, and gmon.c never touches it

Three separate reasons, **each of which decides it on its own**:

1. **`monstartup()` runs before `main()` and allocates ~1.5 MB.** Deferring the
   *allocation* to mid-run would put it against a fragmented heap in a build
   whose entire difficulty is RAM. So the allocation stays at boot.
2. **The SCIF reader reconfigures SCIF for bit-banged SPI and collides with
   KOS's SCIF console.** SD-at-boot therefore means muting the console at boot —
   and muting at `main()` is the documented way to stop this port booting at all
   (`kb/traps.md`; it cost the 2026-08-08 `-f` burn). At dump time the
   measurement is over, so the collision is free.
3. 🔴 **AND THE ONE THAT DECIDED THE SHAPE: `gmon.c` MUST NEVER `fopen()` THE
   CARD.** `gmon.c` opens gmon.out with mode `"a"` for both the histogram and
   the arcs, and newlib turns `"a"` into `O_WRONLY|O_CREAT|O_APPEND`. `O_APPEND`
   is **`0x08`** and KOS's `O_MODE_MASK` is **`0x0f`**, so `O_APPEND` lands
   *inside the mode nibble* — and `fs_fat_write()` (`addons/libkosfat/fs_fat.c`)
   does

   ```c
   mode = fh[fd].mode & O_MODE_MASK;
   if(mode != O_WRONLY && mode != O_RDWR) { errno = EBADF; return -1; }
   ```

   `0x9` is neither, so **every append write to a FAT32 card fails with EBADF.**
   ⚠️ And it fails in the worst possible way: **`fs_fat_open()` does not check
   the mode, so the `fopen` SUCCEEDS.** Only the writes fail, into a console this
   code has already disabled, and you come home from the burn with a 20-byte
   gmon.out. (`"w"` is unaffected: `0x601 & 0x0f == 1 == O_WRONLY`, which is
   exactly why the trap is invisible until the burn.)

🔴 **THE SINK IS THEREFORE *NOT* SWITCHED VIA `GMON_OUT_PREFIX`.** Earlier
revisions of this file described that mechanism in detail — *"`gmon.c` re-reads
`getenv(GMON_OUT_PREFIX)` on every write, so the sink can be swapped between the
header write and the histogram write"*. **Delete that from your model.**
`gmon.c` writes to `/prof` for the whole run, exactly as it always did, and
`prof_write()` decides where the bytes land: console-encoded when there is no
card, or `fwrite()`n straight into an SD file **this code opened itself in
`"w"` and holds open**. One writer, one mode, no append.

**The cost:** `monstartup()`'s 20-byte gmon header has already gone down the
**console** by the time the card comes up, so `dc_prof_sd_open()` writes those 20
bytes to the card by hand — cookie `"gmon"`, `int32` version 1 little-endian,
twelve zero bytes, verified against `gmon.c`'s `gmon_hdr_t`. Without that the
file on the card is not a gmon.out.

### 6.2 ⚠️ `DC_GPROF_SD_IF` — the default is 0, and 1/2 can hang forever

```c
sd_init_ex(&p);                                   /* p.interface = SD_IF_SCIF or SD_IF_SCI */
sd_blockdev_for_partition(0, &s_sd_dev, &ptype);
fs_fat_init();
fs_fat_mount("/sd", &s_sd_dev, FS_FAT_MOUNT_READWRITE);
```

| value | meaning |
|---|---|
| **`0` — SCIF only. THE DEFAULT.** | jj1odm-style reader. Bit-banged SPI; **always fails cleanly** |
| `1` — SCI only | SWAT/Rostovtsev-style reader. ⚠️ **Wedges forever if nothing answers** (§2). Set it only when you *know* the adapter is SCI |
| `2` — SCIF then SCI | ⚠️ **DO NOT USE.** Inherits (1)'s hang as its fallback path, which is the opposite of what a fallback is for. Reopen only if KOS's RDRF loops get a timeout |

⚠️ **This was `2` for exactly one session**, on the reasoning that the tool
should probe rather than ask the human which reader they bought. It cost four
Flycast runs. The fall-back-to-console promise this design makes is only true if
**every probe can fail** — SCIF's can, SCI's cannot.

After the dump: `fs_fat_sync` → `fs_fat_unmount` → `fs_fat_shutdown` →
`sd_shutdown`, then `dbgio_enable()`.

- ⚠️ **The sync/unmount is not tidiness.** `fs_fat` postpones inode and block
  writes until eviction or unmount, so **without it the file on the card is
  empty or truncated** however well the writes went.
- 🔴 **The card must be MBR-partitioned FAT32 with 4 KB clusters.**
  `sd_blockdev_for_partition()` reads a partition table; a raw superfloppy
  (filesystem at sector 0) does not enumerate. And `fat_fs_init_ex()` allocates
  **eight cluster-sized cache buffers**, so a card formatted with 32 KB clusters
  asks for **~260 KB** in the middle of a run whose whole difficulty is RAM.
  **These two together are the most likely way a correctly-wired adapter fails.**
- ⚠️ **It occupies the serial port**, so it is mutually exclusive with a coder's
  cable — which is why the run writes a receipt (§6.4).
- ⚠️ **Bit-banged SPI is slow.** Fine for `gmon.out`; **do not** stream game
  assets from it. Run the stock `sd-speedtest` example to learn the real number
  — **unmeasured here.**
- 🔴 **It is not a BBA and gives no `/pc`** — no ELF upload, so it does not
  shorten the build→burn loop. Booting the CDI from SD via DreamShell might,
  but it is unverified AND it changes disc-read timing, which is one of the
  things we measure. Do not adopt it for anything judging `dc_assetwin.c`.

### ⭐ 6.3 The pad chord — L + R + START, held

`dc_prof_chord_down()` reads maple directly (both analog triggers > 200 plus
Start), not through `PADRead`, so it fires whether or not the game is polling.
`DC_GPROF_CHORD=0` disables it; it is **on by default**.

**This is what makes a human-driven hardware run possible.**
`DC_GPROF_DUMP_FRAME` is a guess at how long a person takes to reach the town;
the chord says *profile ends HERE* — walk to the middle of the town, stand
still, pull both triggers, press START. The frame cap stays as a backstop.

⚠️ The chord reaches the game as Z/L/R plus Start and opens the menu. Accepted:
the dump is the end of the useful part of the run.

### ⭐ 6.4 The pre-mute marker is MANDATORY

`dc_profdump_flush_now()` emits

```
[GPROF] dump: muting console, probing SD
```

**before** `dbgio_flush(); dbgio_disable();`.

**Here is why it is not optional.** Everything after that line is invisible, so a
wedge inside the probe window is **indistinguishable from the game hanging** —
same silent console, same last log line, same dead machine. That ambiguity is
exactly what cost four Flycast runs and a misdiagnosis (the sampler was blamed;
§5) before the SCI RDRF loop of §2 could be attributed. **Forty bytes of serial
buys the attribution.**

The mute itself is load-bearing in the other direction: the SCIF reader puts
SCIF into bit-banged SPI, and `scif_write()` then spins on a TDFE that can never
assert and latches `serial_enabled = 0`, which **nothing in KOS ever clears**.
One stray log line from any thread in that window kills the console permanently,
including the crash dump and including the fall-back-to-console path.

### 6.5 What lands on the card

| file | what it is |
|---|---|
| `/sd/gmon.<pid>` | the gmon.out. Feed it straight to `sh-elf-gprof` |
| `/sd/gprof.txt` | the receipt: path, **size**, short-write count, interface, range, predicted alloc, hz, frames presented, and the `sh-elf-gprof` command line |

⭐ **The receipt exists because with the adapter in the serial port there is NO
console.** It is the only channel that can say "it armed" — the same reason
`kb/traps.md` insists a screen-only image puts a liveness line on the screen. It
states outright that **a 20-byte file means the histogram never landed**, which
is the exact symptom the `O_APPEND` trap of §6.1 produces.

### 6.6 The hardware run, step by step

1. Format the card **MBR-partitioned FAT32, 4 KB clusters** (§6.2).
2. Build the hardware half (§5), burn the padded CDI.
3. Adapter in the serial port. **There is no console** — the receipt is the only
   output channel.
4. Boot, play into the town, walk to somewhere worth profiling, stand still.
5. **Hold L + R + START.** Expect a **multi-second hitch** — that is
   `_mcleanup()` writing ~600 KB through bit-banged SPI.
6. Power off. Read `/sd/gprof.txt` **first**, then `/sd/gmon.<pid>`.

`DC_GPROF_DUMP_FRAME` is the backstop if nobody presses the chord.

### 6.7 The side effect may be worth more than the profile

`kb/RESUME.md` §6 records that `DC_CONSOLE_MUTE=1` is *not optional* on a
measuring burn, because KOS busy-waits on the SCIF FIFO and a perf build puts
~10 lines into every window — **the log measures itself**. §7 now shows that tax
inside the profile itself: `scif_write` at **1.56 s, 0.50 %**, on a run that was
only lightly logging. With the dump going to SD, `DC_CONSOLE_MUTE` is not merely
safe but wanted, and a fully instrumented build becomes runnable on hardware for
the first time.

---

## 7. ⭐ THE RESULT — 2026-08-12, Flycast, console sink, town build

**It works, end to end.** The console carried:

```
[GPROF] arming: range 8c010000..8c266062, predicted alloc 1506528 B, hz=100, dump at frame 300
[GPROF] Total memory allocated: 1506528 bytes      <- gmon.c's own line; the alloc SUCCEEDED
[GPROF] BEGIN v=1 enc=z0b64 cols=76 low=8c010000 high=8c266062 alloc=1506528
[GPROF] END lines=28 raw=612433 enc=1585 crc32=2fd07100
```

`tools/dcprof/decode_gmon.py` recovered a **612,433-byte gmon.out**: 306,190
bins, **31,010 samples in 470 non-empty bins**, profrate 100 Hz. `sh-elf-gprof
-b -p` symbolised it against the run's own ELF. Top of the flat profile:

```
94.76%  293.86s  thd_idle_task
 0.66%    2.05s  vid_waitvbl
 0.51%    1.59s  dc_gx_backend_submit
 0.50%    1.56s  scif_write
 0.38%    1.17s  RspStart
 0.20%    0.63s  emu64::set_position(unsigned int)
 0.18%    0.55s  cull_batch(emu64*)
 0.17%    0.53s  GXPosition3f32
```

### ⭐ The z0 RLE is what makes this affordable

**612,433 raw → 1,585 encoded → 28 console lines.** The "~836 KB of base64 =
**145 seconds** of serial" estimate this document and `dc_profdump.c`'s header
comment both carried is **obsolete**: a histogram is almost all zeros, and RLE
takes it out before base64 ever sees it.

⚠️ The encoded size scales with **non-empty bins**, not with `.text`. A longer
run touches more code, so a 2,400-frame hardware run will encode larger than
this 300-frame one — but nowhere near the naive figure.

### 🔴 IDLE DOMINATES AT 94.76 % — COMPARE NON-IDLE SHARES, NEVER ABSOLUTE SECONDS

`thd_idle_task` takes 94.76 % of the samples. Every other number in that table
is a share of the ~5 % that is left, so **a "0.51 %" entry is really ~10 % of
the work the machine actually did**. Rules that follow:

- **Renormalise over non-idle samples before comparing anything.**
- **Never quote the seconds column.** It is samples ÷ profrate, and the profrate
  is nominal (see below).
- ⭐ **Hardware should idle LESS**, being slower, and therefore give **better**
  signal than this run did. That is an argument for the burn, not against it.
- Note `scif_write` at 1.56 s: **the console tax showing up in its own
  profile.** That is the argument for the SD sink (§6.7).

### The experiment this is all for

**Same code, same scene, flat profile in Flycast and on the console. Diff the
non-idle shares. The functions whose share GROWS on silicon ARE the
icache/memory victims.** That is the measurement `kb/RESUME.md` §6 says cannot
otherwise be made, and §7 is half of it.

Supporting evidence already on file: `tools/dcopt/icache_map.py` puts the town
frame's hot set at **16.40×** an 8 KB direct-mapped icache and the innermost
draw loop at **2.62×**; **`dc_gx_backend_submit` alone is 10,120 B — 1.24× the
whole icache** — and it is already **#3 in the Flycast flat profile**. If the
icache hypothesis is right, that symbol is where the hardware run's share grows.

Read the result against P1: run `DC_PMCR=1` separately (**burn only — Flycast
reports zero for every event**) and check that the functions that grew are the
ones `istall` says are stalling.

⚠️ **A flat profile is a ranking, not a mechanism.** It says which function
grew. With no call graph it cannot distinguish "this function got slower" from
"this function was called more times". Pair every claim with a counter.

---

## 8. Caveats to carry into any reading of the output

- ⚠️ **Samples accrue per RESCHEDULE, not per timer tick — while the gmon header
  claims `profrate = thd_get_hz()`.** `thd_poll` fires on every context switch,
  which includes **voluntary yields**, so yield-heavy phases are
  over-represented and the seconds column is a fiction built on a nominal rate.
  `dc_dvd_read_yielding()` (`dc/src/dc_dvd.c`, the one place a disc read may
  block) is the worst offender. **Treat the output as relative shares.**
- ⭐ **The sampler itself is NOT expensive — this was suspected and CLEARED.**
  `histogram_callback` runs once per reschedule and is **~20 instructions**: two
  range compares, a divide-by-8, a `uint16` increment. `thd_poll` parks the
  caller in `STATE_POLLING` via `thd_block_now` — **no busy-wait, no
  starvation.** `[PERF]` read **24–28 FPS** with the sampler armed. The early
  stalls were the SD probe wedge (§2), not this.
- **Sample counts are not the problem.** 100 Hz over 300 frames gave 31,010
  samples in 470 bins. Do not reach for `DC_GPROF_HZ=1000` reflexively.
- Measurement rule 11 still applies: **the town reseeds every boot**
  (`sys_math.c:7`), so two runs are two different towns. Compare profile
  SHAPES, not single-symbol percentages, until a save exists to pin the layout.
- ⚠️ **A narrowed range (`DC_GPROF_SPAN`) changes the DENOMINATOR.**
  `histogram_callback` **drops** a sample whose PC is out of range — it is not
  counted anywhere. Percentages become "percent of samples in the window". Say
  so when quoting one, and only compare narrowed-vs-narrowed at the same `N`.
- ⚠️ **A chord-terminated run has a duration nobody wrote down.** The frame cap
  gives every unattended run the same length; a human pressing L+R+START does
  not. The recoverable denominator is the `frames` line in `/sd/gprof.txt` —
  quote it, and do not diff a chord run against a frame-capped one without it.
- ⚠️ **Profiling is on from before `main()`.** Boot, asset loading and the title
  screen are all in the histogram.

---

## 9. Status — 2026-08-12

**⭐ VALIDATED, end to end:**

| piece | evidence |
|---|---|
| the full `DC_GPROF=1` link, 3,936 objects, one surviving `gprof_init` | it booted and armed |
| `monstartup()` fits — 1,506,528 B with keeplist-town | `[GPROF] Total memory allocated` |
| the sampler samples | 31,010 samples, 470 bins |
| the z0/base64 console sink | 612,433 raw → 1,585 enc → 28 lines |
| `decode_gmon.py` round-trip | CRC-32 clean, gmon cookie present |
| `sh-elf-gprof -b -p` symbolisation | §7's flat profile |
| the frame-cap trigger | dumped at frame 300 |

**Still unproven — and this is the whole remaining risk:**

- 🔴 **No hardware run.** The Flycast half is done; the console half of the diff
  is the deliverable and it does not exist yet.
- 🔴 **No card has ever been mounted by this code.** The MBR requirement, the
  4 KB cluster requirement, the 20-byte header hand-off, the
  `fs_fat_sync`/`fs_fat_unmount` requirement and the SCIF probe are all read out
  of KOS source, not observed. The only thing about the SD path that IS observed
  is that **the SCI probe wedges** (§2).
- 🔴 **The pad chord has never been pressed.** Whether `maple_dev_status()`
  returns a usable `cont_state_t` from inside the vblank path on hardware is
  unobserved.
- ⚠️ **The SD dump's cost is unmeasured** — bit-banged SPI writing ~600 KB
  unencoded. It does not corrupt anything: sampling has already stopped inside
  `_mcleanup()` before the write, so the stall lands after the profile, not in
  it. Budget "a multi-second hitch" and do not be alarmed.
- ⚠️ **Whether the sampler perturbs frame pacing on hardware**, where it competes
  with the audio thread for reschedules, is unmeasured. Flycast said 24–28 FPS.
- Bought: SD adapter, 2026-08-10. Card formatting per §6.2.
